#pragma once

// Linux process boundary: a stuck vendor decoder must not prevent recovery.
// The supervisor never opens RTSP or decodes video. One worker is reaped before
// another is started; every worker exec has fresh motion/PTS/recorder state.
#include "log_line.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace recovery {
constexpr int retryExitCode = 75;
using Clock = std::chrono::steady_clock;

struct Options {
    double startupSec = 20, stallSec = 15, shutdownSec = 20;
    double initialRetrySec = 1, maxRetrySec = 30, stableResetSec = 30;
    int pollMs = 100;
    double retryMultiplier = 2.0;

    void validate() const {
        for (double v :
             {startupSec, stallSec, shutdownSec, initialRetrySec, maxRetrySec, stableResetSec})
            if (!std::isfinite(v) || v <= 0 || v > 86400)
                throw std::runtime_error("Recovery times must be finite and in (0,86400] seconds");
        if (maxRetrySec < initialRetrySec)
            throw std::runtime_error("Recovery max retry < initial retry");
        if (pollMs <= 0 || pollMs > 1000 * std::min({startupSec, stallSec, shutdownSec}))
            throw std::runtime_error(
                "Watchdog poll interval must fit startup/stall/shutdown deadlines");
        if (!std::isfinite(retryMultiplier) || retryMultiplier < 1)
            throw std::runtime_error("Retry multiplier must be finite and >=1");
    }
};

class Fd {
  public:
    explicit Fd(int fd = -1) : fd_(fd) {}

    ~Fd() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const {
        return fd_;
    }

    void reset() {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }

  private:
    int fd_;
};

class Progress {
  public:
    explicit Progress(int fd = -1) : fd_(fd) {
        if (fd >= 0 && ::fcntl(fd, F_GETFD) < 0)
            throw std::runtime_error("Invalid worker progress FD");
    }

    void frame() const noexcept {
        send('F');
    }

    void finishing() const noexcept {
        send('S');
    }

    void fatal() const noexcept {
        send('E');
    }

  private:
    void send(char value) const noexcept {
        if (fd_.get() >= 0) {
            // Nonblocking. Dropping a pulse on a full pipe is safe: queued
            // pulses already prove progress. The parent drains each poll.
            const auto ignored = ::write(fd_.get(), &value, 1);
            (void)ignored;
        }
    }

    Fd fd_;
};

class Child {
  public:
    explicit Child(pid_t pid) : pid_(pid) {}

    ~Child() {
        if (pid_ <= 0)
            return;
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
    }

    void reaped() {
        pid_ = -1;
    }

  private:
    pid_t pid_;
};

inline double seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

inline void pause(int pollMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
}

inline int supervise(const std::string& executable, const std::vector<std::string>& arguments,
                     const Options& options, const std::atomic<bool>& stop, std::ostream& log) {
    options.validate();
    double delay = options.initialRetrySec;
    unsigned attempt = 0;
    while (!stop.load()) {
        int descriptors[2];
        if (::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) != 0)
            throw std::runtime_error("Cannot create recovery progress pipe");
        Fd reader(descriptors[0]), writer(descriptors[1]);
        std::vector<std::string> command{executable, "--worker", std::to_string(writer.get())};
        command.insert(command.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& s : command)
            argv.push_back(s.data());
        argv.push_back(nullptr);
        const pid_t parent = ::getpid();
        const pid_t pid = ::fork();
        if (pid < 0)
            throw std::runtime_error("Cannot fork RTSP worker");
        if (pid == 0) {
            // Only async-signal-safe/syscall operations between fork and exec.
            // Emergency parent death must not orphan a decoder. Normal stop
            // uses SIGINT and waits for MP4 EOS before escalating.
            if (::prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || ::getppid() != parent)
                ::_exit(127);
            ::close(reader.get());
            if (::fcntl(writer.get(), F_SETFD, 0) < 0)
                ::_exit(127);
            ::execv(executable.c_str(), argv.data());
            ::_exit(127);
        }
        Child child(pid);
        // Parent closes its write end now so EOF means the worker closed it.
        writer.reset();
        const auto started = Clock::now();
        auto firstFrame = started, lastFrame = started, deadline = started;
        bool seenFrame = false, stopping = false, forced = false, timedOut = false, fatal = false;
        int status = 0;
        LogLine(log) << "[WATCH] start attempt=" << ++attempt << " worker_pid=" << pid << std::endl;
        for (;;) {
            const auto result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                child.reaped();
                break;
            }
            if (result < 0 && errno != EINTR)
                throw std::runtime_error("Cannot reap RTSP worker");
            char data[512];
            ssize_t n;
            do {
                n = ::read(reader.get(), data, sizeof(data));
                for (ssize_t i = 0; i < n; ++i) {
                    if (data[i] == 'E')
                        fatal = true;
                    if (data[i] == 'F' && !stopping) {
                        if (!seenFrame) {
                            firstFrame = Clock::now();
                            LogLine(log) << "[WATCH] first_frame worker_pid=" << pid << std::endl;
                        }
                        seenFrame = true;
                        lastFrame = Clock::now();
                    } else if (data[i] == 'S' && !stopping) {
                        stopping = true;
                        deadline =
                            Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(options.shutdownSec));
                    }
                }
            } while (n > 0 || (n < 0 && errno == EINTR));
            const auto now = Clock::now();
            const bool expired = seenFrame ? seconds(now - lastFrame) >= options.stallSec
                                           : seconds(now - started) >= options.startupSec;
            if (!stopping && (stop.load() || expired)) {
                timedOut = !stop.load();
                stopping = true;
                LogLine(log) << "[WATCH] stop worker_pid=" << pid << " reason="
                             << (stop.load() ? "signal"
                                 : seenFrame ? "frame_stall"
                                             : "startup_timeout")
                             << std::endl;
                ::kill(pid, SIGINT);
                deadline = now + std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::duration<double>(options.shutdownSec));
            }
            if (stopping && now >= deadline && !forced) {
                forced = true;
                timedOut = true;
                LogLine(log) << "[WATCH] forced_kill worker_pid=" << pid
                             << " clip_may_be_incomplete=1" << std::endl;
                ::kill(pid, SIGKILL);
            }
            pause(options.pollMs);
        }
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        LogLine(log) << "[WATCH] reaped worker_pid=" << pid << " exit=" << code << std::endl;
        if (stop.load())
            return forced ? 1 : 0;
        if (fatal)
            return 1;
        if (WIFEXITED(status) && code != retryExitCode && (code != 0 || !timedOut))
            return code;
        if (seenFrame && seconds(lastFrame - firstFrame) >= options.stableResetSec)
            delay = options.initialRetrySec;
        LogLine(log) << "[WATCH] retry_in_sec=" << delay << std::endl;
        const auto retryAt = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                std::chrono::duration<double>(delay));
        while (!stop.load() && Clock::now() < retryAt)
            pause(options.pollMs);
        delay = std::min(options.maxRetrySec, delay * options.retryMultiplier);
    }
    return 0;
}
} // namespace recovery
