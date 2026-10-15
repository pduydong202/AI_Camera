#include "motion_timing.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        MotionStageProfiler disabled(nullptr);
        disabled.mark(MotionStage::Setup);
        MotionTiming timing;
        timing.stages[0].wallMs = 999.0;
        MotionStageProfiler profiler(&timing);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        profiler.mark(MotionStage::Setup);
        const auto busyEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (std::chrono::steady_clock::now() < busyEnd) {
        }
        profiler.mark(MotionStage::Mog);
        const auto& waiting = timing.stages[static_cast<std::size_t>(MotionStage::Setup)];
        const auto& busy = timing.stages[static_cast<std::size_t>(MotionStage::Mog)];
        check(waiting.wallMs >= 15.0 && std::isfinite(waiting.cpuMs) && waiting.cpuMs >= 0.0,
              "waiting interval was not measured");
        check(waiting.wallMs - waiting.cpuMs >= 10.0, "wall time conflated with CPU time");
        check(busy.wallMs >= 15.0 && std::isfinite(busy.cpuMs) && busy.cpuMs > 0.0,
              "busy interval CPU time unavailable");
        check(busy.cpuMs <= busy.wallMs + 1.0, "thread CPU exceeded elapsed interval");
        check(timing.stages[static_cast<std::size_t>(MotionStage::L3)].wallMs == 0.0,
              "unmeasured stage contained stale samples");
        MotionStageProfiler nextFrame(&timing);
        check(timing.stages[0].wallMs == 0.0 && timing.stages[1].cpuMs == 0.0,
              "new frame retained old timing");
        nextFrame.mark(MotionStage::Debug);
        std::cout << "PASS: independent wall/thread CPU intervals and per-frame reset\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
