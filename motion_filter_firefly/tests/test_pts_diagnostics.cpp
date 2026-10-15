#include "pts_diagnostics.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr std::int64_t STEP = 50000000;

void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

// Frozen rule from GstMppSource, with the already selected duration supplied.
std::int64_t legacyNormalize(std::int64_t raw, std::int64_t& last) {
    std::int64_t used = raw;
    if (used < 0 || (last >= 0 && used <= last))
        used = last >= 0 ? last + STEP : 0;
    last = used;
    return used;
}

struct Harness {
    PtsDiagnosticsTracker tracker;
    std::int64_t last = -1;

    PtsObservation feed(std::int64_t raw) {
        const auto previous = last;
        const auto used = legacyNormalize(raw, last);
        return tracker.observe(raw, used, previous, STEP, STEP, raw);
    }
};

void testNormal() {
    Harness h;
    PtsDiagnosticsWindow w;
    for (int i = 0; i < 200; ++i)
        w.add(h.feed(i * STEP));
    require(w.observed == 200 && w.repaired == 0, "normal stream must need no repair");
    require(w.duplicate == 0 && w.backward == 0 && w.missing == 0, "normal raw counters");
    require(w.offsetMaxNs == 0 && w.repairRunMax == 0, "normal offset/run");
    require(w.forwardGapMaxNs == STEP && !w.hasRepairSample, "normal forward duration");
}

void testOneDuplicateLongCascade() {
    Harness h;
    PtsDiagnosticsWindow w;
    for (int i = 0; i < 3; ++i)
        w.add(h.feed(i * STEP));
    w.add(h.feed(2 * STEP)); // one duplicate; old rule moves a whole frame ahead
    for (int i = 3; i <= 1001; ++i)
        w.add(h.feed(i * STEP));
    require(w.repaired == 1000 && w.duplicate == 1 && w.catchup == 999,
            "one raw duplicate must be distinguished from 999 cascade repairs");
    require(w.repairRunMax == 1000 && w.offsetMaxNs == STEP, "cascade run/offset");
    require(std::string(w.firstRepair.reason()) == "duplicate", "cascade trigger detail");
    w.add(h.feed(1003 * STEP)); // source moves beyond synthetic timeline
    require(h.last == 1003 * STEP && w.last.repairRun == 0 && !w.last.repaired,
            "raw timestamp recovery must terminate the repair run");
}

void testBackwardAndMissing() {
    Harness h;
    PtsDiagnosticsWindow w;
    w.add(h.feed(2 * STEP));
    w.add(h.feed(STEP));
    require(w.backward == 1 && w.backwardGapMaxNs == STEP, "backward raw PTS");
    require(w.offsetMaxNs == 2 * STEP, "backward used/raw distance");
    w.add(h.feed(-1));
    require(w.missing == 1 && w.repaired == 2, "missing PTS must be counted separately");
    require(w.offsetMaxNs == 2 * STEP, "missing raw PTS has no measurable offset");
    w.add(h.feed(4 * STEP));
    require(w.catchup == 1 && w.last.previousRawNs == STEP,
            "raw comparison spans missing entries using previous valid raw PTS");
    require(w.last.rawDurationNs == STEP && w.last.rawDtsNs == 4 * STEP,
            "raw sample metadata must survive");
}

void testWindowMergeAndCrossWindowRun() {
    Harness h;
    PtsDiagnosticsWindow first, second, merged;
    first.add(h.feed(0));
    first.add(h.feed(0));
    second.add(h.feed(STEP));
    second.add(h.feed(2 * STEP));
    second.add(h.feed(4 * STEP));
    merged.merge(first);
    merged.merge(second);
    merged.merge(PtsDiagnosticsWindow{});
    require(merged.observed == 5 && merged.repaired == 3 && merged.catchup == 2,
            "window counts must sum exactly");
    require(merged.repairRunMax == 3 && second.repairRunMax == 3,
            "a repair run continues across diagnostic window boundaries");
    require(merged.firstRepair.index == first.firstRepair.index &&
                merged.last.index == second.last.index,
            "first/last samples retain chronological order");
}

void testReadOnlyAndSeparateBranches() {
    Harness analysis, encoded;
    std::int64_t controlLast = -1;
    PtsDiagnosticsWindow w;
    for (int i = 0; i < 5000; ++i) {
        std::int64_t raw = i * STEP;
        if (i % 101 == 7)
            raw = -1;
        else if (i % 137 == 9)
            raw -= STEP;
        else if (i % 173 == 11)
            raw -= 2 * STEP;
        const auto expected = legacyNormalize(raw, controlLast);
        const auto observed = analysis.feed(raw);
        require(observed.usedNs == expected && analysis.last == controlLast,
                "diagnostics must not alter the selected timestamp");
        w.add(observed);
        const auto clean = encoded.feed(i * STEP);
        require(!clean.repaired && clean.usedNs == i * STEP, "independent branch state");
    }
    require(w.missing > 0 && w.duplicate > 0 && w.backward > 0 && w.catchup > 0,
            "mixed stream must exercise every reason category");
    require(w.repaired == w.missing + w.duplicate + w.backward + w.catchup,
            "legacy repair reasons must partition repairs on this mixed stream");
}
} // namespace

int main() {
    try {
        testNormal();
        testOneDuplicateLongCascade();
        testBackwardAndMissing();
        testWindowMergeAndCrossWindowRun();
        testReadOnlyAndSeparateBranches();
        std::cout << "PASS: PTS diagnostics; raw reasons, 1000-frame cascade, window merge, "
                     "branch isolation and unchanged legacy timestamps\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
