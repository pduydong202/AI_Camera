#include "live_frame_budget.hpp"
#include "sample_stats.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr std::int64_t NS = 1000000000LL;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testConstantAndJitteredInput() {
    LiveFrameBudget constant(20.0, 4);
    LiveFrameBudget jittered(20.0, 4);
    for (std::int64_t i = 0; i < 1800; ++i) {
        check(constant.allow(i * NS / 20), "20 FPS input lost a frame");
        const auto offset = (i % 2 == 0 ? 10000000LL : -10000000LL);
        check(jittered.allow(i * NS / 20 + offset), "bounded arrival jitter caused needless loss");
    }
}

void testBatchedInput() {
    LiveFrameBudget budget(20.0, 4);
    for (std::int64_t i = 0; i < 1800; ++i) {
        check(budget.allow((i / 4) * NS / 5), "four-frame 20 FPS batches were not absorbed");
    }
}

void testLargerBursts() {
    LiveFrameBudget legacy(20.0, 4, 0);
    LiveFrameBudget extended(20.0, 4, 200);
    check(legacy.burstFrames() == 4 && extended.burstFrames() == 8,
          "extra allowance changed the queue base or capacity calculation");
    std::int64_t legacyAccepted = 0;
    for (std::int64_t i = 0; i < 3600; ++i) {
        const auto time = (i / 8) * NS * 2 / 5;
        if (legacy.allow(time))
            ++legacyAccepted;
        check(extended.allow(time), "eight-frame 20 FPS batches were needlessly dropped");
    }
    check(legacyAccepted == 1800, "fixture did not reproduce tight-budget burst loss");
    check(!extended.allow((3599 / 8) * NS * 2 / 5), "extra burst exceeded finite capacity");
    extended.reset();
    for (int i = 0; i < 8; ++i)
        check(extended.allow(0), "extended reset lost capacity");
    check(!extended.allow(0), "extended reset created unlimited credit");
    check(LiveFrameBudget(12.5, 4, 200).burstFrames() == 7,
          "fractional burst allowance rounded down");
}

void testHigherInputRate(int inputFps, double targetFps, int burstMs = 0) {
    LiveFrameBudget budget(targetFps, 4, burstMs);
    const std::int64_t count = static_cast<std::int64_t>(inputFps) * 90;
    std::int64_t accepted = 0;
    for (std::int64_t i = 0; i < count; ++i) {
        const auto t = i * NS / inputFps;
        if (budget.allow(t))
            ++accepted;
        const auto expectedMaximum =
            static_cast<std::int64_t>(std::floor(static_cast<long double>(targetFps) * t / NS)) +
            budget.burstFrames();
        check(accepted <= expectedMaximum, "live rate exceeded FPS plus bounded burst capacity");
    }
    const long double elapsed = static_cast<long double>((count - 1) * NS / inputFps) / NS;
    check(accepted >= static_cast<std::int64_t>(std::floor(targetFps * elapsed)),
          "higher-rate input was undersampled");
}

void testIdleAndClockRollback() {
    LiveFrameBudget budget(20.0, 4);
    for (int i = 0; i < 4; ++i)
        check(budget.allow(NS), "initial capacity missing");
    check(!budget.allow(NS), "unbounded initial burst");
    check(!budget.allow(0), "clock rollback replenished tokens");
    for (int i = 0; i < 4; ++i)
        check(budget.allow(11 * NS), "idle did not restore bounded capacity");
    check(!budget.allow(11 * NS), "idle allowed an unlimited catch-up burst");
    budget.reset();
    check(budget.allow(0), "reset did not restore initial state");
}

void testInvalidConfiguration() {
    for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        bool threw = false;
        try {
            LiveFrameBudget budget(rate, 4);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "invalid FPS accepted");
    }
    bool threw = false;
    try {
        LiveFrameBudget budget(20.0, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "invalid burst capacity accepted");
    threw = false;
    try {
        LiveFrameBudget budget(20.0, 4, -1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "negative extra burst accepted");
    threw = false;
    try {
        LiveFrameBudget budget(std::numeric_limits<double>::max(), 4, 200);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "overflowing burst capacity accepted");
}

void testReportingStatistics() {
    SampleStats stats;
    check(std::isnan(stats.mean()) && std::isnan(stats.p95()), "missing latency reported as zero");
    for (int i = 0; i < 100; ++i)
        stats.add(i);
    check(stats.count() == 100 && stats.mean() == 49.5 && stats.maximum() == 99.0,
          "latency sample accounting incorrect");
    check(stats.p95() == 94.0, "nearest-rank P95 incorrect");
    stats.clear();
    check(stats.count() == 0 && std::isnan(stats.maximum()), "report window retained old samples");
    stats.add(-1.0);
    stats.add(std::numeric_limits<double>::quiet_NaN());
    check(stats.count() == 0, "invalid latency accepted");
}
} // namespace

int main() {
    try {
        testConstantAndJitteredInput();
        testBatchedInput();
        testLargerBursts();
        testHigherInputRate(21, 20.0);
        testHigherInputRate(30, 20.0);
        testHigherInputRate(60, 20.0);
        testHigherInputRate(30, 12.5);
        testHigherInputRate(21, 20.0, 200);
        testHigherInputRate(30, 20.0, 200);
        testHigherInputRate(60, 20.0, 200);
        testHigherInputRate(30, 12.5, 200);
        testIdleAndClockRollback();
        testInvalidConfiguration();
        testReportingStatistics();
        std::cout << "PASS: live rate budget and latency statistics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
