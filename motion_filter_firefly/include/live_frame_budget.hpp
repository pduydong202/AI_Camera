#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

// Live input rate limit: tokens are replenished by receiver monotonic time.
// Burst allowance is independent of appsink's storage capacity: accepting
// a bounded catch-up burst does not require retaining more decoded buffers.
// This class never reads or changes media PTS/DTS.
class LiveFrameBudget {
  public:
    LiveFrameBudget(double targetFps, int queueFrames, int extraBurstMs = 0)
        : rate_(targetFps), capacity_(capacityFor(targetFps, queueFrames, extraBurstMs)),
          tokens_(capacity_) {}

    int burstFrames() const {
        return static_cast<int>(capacity_);
    }

    bool allow(std::int64_t nowNs) {
        if (!initialized_) {
            lastNs_ = nowNs;
            initialized_ = true;
        } else if (nowNs > lastNs_) {
            const long double elapsedSec =
                (static_cast<long double>(nowNs) - static_cast<long double>(lastNs_)) /
                1000000000.0L;
            tokens_ = std::min(capacity_, tokens_ + elapsedSec * rate_);
            lastNs_ = nowNs;
        }
        if (tokens_ < 1.0L)
            return false;
        tokens_ -= 1.0L;
        return true;
    }

    void reset() {
        tokens_ = capacity_;
        initialized_ = false;
        lastNs_ = 0;
    }

  private:
    static long double capacityFor(double targetFps, int queueFrames, int extraBurstMs) {
        if (!std::isfinite(targetFps) || targetFps <= 0.0 || queueFrames <= 0 || extraBurstMs < 0) {
            throw std::invalid_argument("LiveFrameBudget requires positive finite FPS/queue and "
                                        "nonnegative burst milliseconds");
        }
        const long double frames =
            queueFrames + std::ceil(static_cast<long double>(targetFps) * extraBurstMs / 1000.0L);
        if (!std::isfinite(frames) || frames > std::numeric_limits<int>::max()) {
            throw std::invalid_argument("LiveFrameBudget burst capacity is too large");
        }
        return frames;
    }

    long double rate_;
    long double capacity_;
    long double tokens_;
    std::int64_t lastNs_ = 0;
    bool initialized_ = false;
};
