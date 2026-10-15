#pragma once
#include "config.hpp"
#include <cstdint>
#include <optional>

struct EventSignal {
    enum class Type { Start, Stop };
    Type type;
    std::uint64_t eventId;
    std::int64_t ptsNs;
};

class EventManager {
  public:
    explicit EventManager(const Config& cfg);
    std::optional<EventSignal> process(bool motion, std::int64_t ptsNs, std::int64_t durationNs);
    std::optional<EventSignal> finish(std::int64_t lastEndPtsNs);

    int eventCount() const {
        return eventCount_;
    }

  private:
    std::int64_t postNs_ = 0;
    bool active_ = false;
    std::uint64_t activeId_ = 0;
    std::int64_t lastMotionEnd_ = 0;
    std::uint64_t nextId_ = 1;
    int eventCount_ = 0;
};
