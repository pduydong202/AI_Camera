#pragma once

#include <mutex>
#include <ostream>
#include <sstream>

// Assemble each application log record before writing it. Main, packet and
// recorder threads share one lock, so PERF/STAGE fields cannot be split by a
// recorder message. Vendor-library stderr is outside this application's lock.
class LogLine : public std::ostringstream {
  public:
    explicit LogLine(std::ostream& output) : output_(output) {}

    ~LogLine() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex());
            output_ << str();
            output_.flush();
        } catch (...) {
            // Logging must not throw during worker cleanup/unwinding.
        }
    }

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

  private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }

    std::ostream& output_;
};
