#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

// One decision per encoded access unit, before the decoder/recorder tee.
// Use the same microsecond tick as the MP4 track and avoid sub-microsecond
// collisions at the decoder boundary.
// Repair only unusable timestamps by the smallest representable step; using
// a whole nominal frame here creates a long catch-up cascade after a burst.
class SourceTimeline {
  public:
    static constexpr std::int64_t tickNs = 1000;
    std::int64_t normalize(std::int64_t raw, std::int64_t nowNs = 0,
                           std::int64_t maxInvalidNs = 0) {
        const bool invalid = raw < 0 || (last_ >= 0 &&
            (raw <= last_ || raw - last_ < tickNs));
        if (invalid) {
            if (invalidSince_ < 0) invalidSince_ = nowNs;
            if (maxInvalidNs > 0 && nowNs - invalidSince_ >= maxInvalidNs)
                throw std::runtime_error("RTSP source timestamps did not recover before stall timeout");
        } else {
            invalidSince_ = -1;
        }
        if (last_ < 0) {
            last_ = raw < 0 ? 0 : raw;
            return last_;
        }
        if (last_ > std::numeric_limits<std::int64_t>::max() - tickNs)
            throw std::overflow_error("Source timeline overflow");
        last_ = raw < last_ + tickNs ? last_ + tickNs : raw;
        return last_;
    }
  private:
    std::int64_t last_ = -1;
    std::int64_t invalidSince_ = -1;
};
