#include "event_manager.hpp"
#include <algorithm>
#include <cmath>

EventManager::EventManager(const Config& c)
    : postNs_(std::max<std::int64_t>(
          0, static_cast<std::int64_t>(std::llround(c.eventPostSec * 1e9)))) {}

std::optional<EventSignal> EventManager::process(bool motion, std::int64_t pts, std::int64_t dur) {
    const auto end = pts + std::max<std::int64_t>(0, dur);
    if (!active_) {
        if (!motion)
            return std::nullopt;
        active_ = true;
        activeId_ = nextId_++;
        lastMotionEnd_ = end;
        return EventSignal{EventSignal::Type::Start, activeId_, pts};
    }
    if (motion) {
        lastMotionEnd_ = std::max(lastMotionEnd_, end);
        return std::nullopt;
    }
    if (end - lastMotionEnd_ >= postNs_) {
        active_ = false;
        ++eventCount_;
        return EventSignal{EventSignal::Type::Stop, activeId_, end};
    }
    return std::nullopt;
}

std::optional<EventSignal> EventManager::finish(std::int64_t end) {
    if (!active_)
        return std::nullopt;
    active_ = false;
    ++eventCount_;
    return EventSignal{EventSignal::Type::Stop, activeId_, end};
}
