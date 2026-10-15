#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>

// CPU time of the calling thread, not elapsed time and not all OpenCV workers.
inline double timingThreadCpuMs() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(value.tv_sec) * 1000.0 +
           static_cast<double>(value.tv_nsec) / 1000000.0;
}

enum class MotionStage : std::size_t {
    Setup,
    Mog,
    Cleanup,
    L2Prior,
    Ccl,
    L2Evidence,
    L2Update,
    L3,
    Decision,
    Debug,
    Count
};
inline constexpr std::size_t motionStageCount = static_cast<std::size_t>(MotionStage::Count);
inline constexpr std::array<const char*, motionStageCount> motionStageNames = {
    "setup",       "mog",       "cleanup", "l2_prior", "ccl",
    "l2_evidence", "l2_update", "l3",      "decision", "debug"};

struct StageDuration {
    double wallMs = 0.0;
    double cpuMs = 0.0;
};

struct MotionTiming {
    std::array<StageDuration, motionStageCount> stages{};
    std::uint64_t l3RegionPixels = 0;
    std::uint64_t l3ImagePixels = 0;
    std::uint64_t l3EmptyFastFrames = 0;
};

// Consecutive checkpoints account for the unchanged operation order.
// A null output skips clock calls and leaves detection state untouched.
class MotionStageProfiler {
  public:
    explicit MotionStageProfiler(MotionTiming* output) : output_(output) {
        if (output_ != nullptr) {
            *output_ = MotionTiming{};
            wallStart_ = Clock::now();
            cpuStartMs_ = timingThreadCpuMs();
        }
    }

    void mark(MotionStage stage) {
        if (output_ == nullptr)
            return;
        const auto wallNow = Clock::now();
        const double cpuNow = timingThreadCpuMs();
        auto& value = output_->stages[static_cast<std::size_t>(stage)];
        value.wallMs += std::chrono::duration<double, std::milli>(wallNow - wallStart_).count();
        value.cpuMs += cpuNow - cpuStartMs_;
        wallStart_ = wallNow;
        cpuStartMs_ = cpuNow;
    }

  private:
    using Clock = std::chrono::steady_clock;
    MotionTiming* output_ = nullptr;
    Clock::time_point wallStart_{};
    double cpuStartMs_ = 0.0;
};
