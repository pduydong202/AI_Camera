#include "supervised_process.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/file.h>

namespace fs = std::filesystem;
std::atomic<bool> stopFlag{false};

void stopNow(int) {
    stopFlag.store(true);
}

void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

int worker(int argc, char** argv) {
    if (argc != 6)
        return 1;
    recovery::Progress progress(std::stoi(argv[2]));
    const std::string mode = argv[3];
    const fs::path root = argv[4];
    recovery::Fd lock(::open((root / "lock").c_str(), O_CREAT | O_RDWR, 0600));
    if (lock.get() < 0 || ::flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
        return 90;
    unsigned count = 0;
    {
        std::ifstream f(root / "attempts");
        f >> count;
    }
    ++count;
    {
        std::ofstream f(root / "attempts");
        f << count;
    }
    {
        std::ofstream f(root / ("pid" + std::to_string(count)));
        f << ::getpid();
    }
    if (mode == "fatal")
        return 1;
    if (mode == "transient" && count == 1)
        return recovery::retryExitCode;
    if (mode == "crash" && count == 1) {
        ::raise(SIGKILL);
        return 99;
    }
    if ((mode == "startup_hang" || mode == "frame_hang" || mode == "shutdown_hang" ||
         mode == "cancel_hang" || mode == "fatal_cleanup_hang") &&
        count == 1) {
        std::signal(SIGINT, SIG_IGN);
        if (mode == "frame_hang" || mode == "cancel_hang")
            progress.frame();
        if (mode == "shutdown_hang") {
            progress.frame();
            progress.finishing();
        }
        if (mode == "fatal_cleanup_hang") {
            progress.fatal();
            progress.finishing();
        }
        for (;;)
            ::pause();
    }
    if (mode == "cancel_backoff")
        return recovery::retryExitCode;
    progress.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    progress.finishing();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--worker")
        return worker(argc, argv);
    try {
        std::signal(SIGINT, stopNow);
        std::signal(SIGTERM, stopNow);
        std::signal(SIGPIPE, SIG_IGN);
        const fs::path base =
            fs::temp_directory_path() / ("firefly_recovery_" + std::to_string(::getpid()));
        fs::create_directories(base);
        const recovery::Options options{.25, .25, .25, .1, .2, .5};
        for (const std::string mode :
             {"healthy", "transient", "startup_hang", "frame_hang", "shutdown_hang", "crash",
              "fatal", "cancel_hang", "cancel_backoff", "fatal_cleanup_hang"}) {
            const auto dir = base / mode;
            fs::create_directories(dir);
            stopFlag = false;
            std::ostringstream log;
            std::thread cancel;
            if (mode == "cancel_hang" || mode == "cancel_backoff")
                cancel = std::thread([] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    stopFlag = true;
                });
            const auto begin = recovery::Clock::now();
            const int code = recovery::supervise("/proc/self/exe", {mode, dir.string(), "unused"},
                                                 options, stopFlag, log);
            if (cancel.joinable())
                cancel.join();
            const auto elapsed = recovery::seconds(recovery::Clock::now() - begin);
            unsigned attempts = 0;
            {
                std::ifstream f(dir / "attempts");
                f >> attempts;
            }
            const bool twice = mode == "transient" || mode == "startup_hang" ||
                               mode == "frame_hang" || mode == "shutdown_hang" || mode == "crash";
            check(attempts == (twice ? 2U : 1U), "Wrong retry count or overlapping worker");
            check(code ==
                      ((mode == "fatal" || mode == "cancel_hang" || mode == "fatal_cleanup_hang")
                           ? 1
                           : 0),
                  "Wrong supervisor exit");
            check(elapsed < 4, "Recovery or shutdown exceeded test deadline");
            for (unsigned i = 1; i <= attempts; ++i) {
                pid_t pid = 0;
                {
                    std::ifstream f(dir / ("pid" + std::to_string(i)));
                    f >> pid;
                }
                errno = 0;
                check(::kill(pid, 0) < 0 && errno == ESRCH,
                      "Unreaped/live worker after supervisor exit");
            }
            if (mode == "startup_hang")
                check(log.str().find("startup_timeout") != std::string::npos,
                      "Startup watchdog did not fire");
            if (mode == "frame_hang")
                check(log.str().find("frame_stall") != std::string::npos,
                      "Frame watchdog did not fire");
            if (mode == "shutdown_hang")
                check(log.str().find("forced_kill") != std::string::npos,
                      "Shutdown watchdog did not fire");
            std::cout << "PASS " << mode << " attempts=" << attempts << " exit=" << code << '\n';
        }
        fs::remove_all(base);
        std::cout << "PASS: recovery, watchdog deadlines, fatal errors, cancellation, no "
                     "overlapping/orphan workers\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
