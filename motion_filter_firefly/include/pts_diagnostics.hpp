#pragma once

#include <algorithm>
#include <cstdint>

// Observes timestamp decisions made by the source. No timestamp is modified.
// One tracker is owned by each source-consumer thread. Window snapshots are
// merged by the main thread; the encoded window is transferred under a mutex.
struct PtsObservation {
    std::uint64_t index = 0;
    std::int64_t rawNs = -1;
    std::int64_t previousRawNs = -1; // previous VALID raw PTS
    std::int64_t previousUsedNs = -1;
    std::int64_t usedNs = -1;
    std::int64_t rawDtsNs = -1;
    std::int64_t rawDurationNs = -1;
    std::int64_t usedDurationNs = 0;
    std::int64_t forwardGapNs = 0;
    std::int64_t backwardGapNs = 0;
    std::int64_t offsetNs = 0; // only defined for a valid raw PTS
    std::uint64_t repairRun = 0;
    bool repaired = false;
    bool missing = false;
    bool duplicate = false;
    bool backward = false;
    bool catchup = false;

    const char* reason() const {
        if (!repaired)
            return "none";
        if (missing)
            return "missing";
        if (duplicate)
            return "duplicate";
        if (backward)
            return "backward";
        return "catchup";
    }
};

class PtsDiagnosticsTracker {
  public:
    PtsObservation observe(std::int64_t raw, std::int64_t used, std::int64_t previousUsed,
                           std::int64_t rawDuration, std::int64_t usedDuration,
                           std::int64_t rawDts) {
        PtsObservation o;
        o.index = ++index_;
        o.rawNs = raw;
        o.previousRawNs = previousValidRawNs_;
        o.previousUsedNs = previousUsed;
        o.usedNs = used;
        o.rawDtsNs = rawDts;
        o.rawDurationNs = rawDuration;
        o.usedDurationNs = usedDuration;
        o.missing = raw < 0;
        if (raw >= 0) {
            if (previousValidRawNs_ >= 0) {
                o.duplicate = raw == previousValidRawNs_;
                o.backward = raw < previousValidRawNs_;
                if (raw > previousValidRawNs_)
                    o.forwardGapNs = raw - previousValidRawNs_;
                else if (o.backward)
                    o.backwardGapNs = previousValidRawNs_ - raw;
            }
            previousValidRawNs_ = raw;
            if (used >= raw)
                o.offsetNs = used - raw;
        }
        o.repaired = used != raw;
        o.catchup = o.repaired && !o.missing && !o.duplicate && !o.backward;
        repairRun_ = o.repaired ? repairRun_ + 1 : 0;
        o.repairRun = repairRun_;
        return o;
    }

  private:
    std::uint64_t index_ = 0;
    std::uint64_t repairRun_ = 0;
    std::int64_t previousValidRawNs_ = -1;
};

struct PtsDiagnosticsWindow {
    std::uint64_t observed = 0;
    std::uint64_t repaired = 0;
    std::uint64_t missing = 0;
    std::uint64_t duplicate = 0;
    std::uint64_t backward = 0;
    std::uint64_t catchup = 0;
    std::uint64_t repairRunMax = 0;
    std::int64_t offsetMaxNs = 0;
    std::int64_t forwardGapMaxNs = 0;
    std::int64_t backwardGapMaxNs = 0;
    bool hasRepairSample = false;
    bool hasRawIssueSample = false;
    PtsObservation firstRepair;
    PtsObservation firstRawIssue;
    PtsObservation last;

    void add(const PtsObservation& o) {
        ++observed;
        repaired += o.repaired;
        missing += o.missing;
        duplicate += o.duplicate;
        backward += o.backward;
        catchup += o.catchup;
        repairRunMax = std::max(repairRunMax, o.repairRun);
        offsetMaxNs = std::max(offsetMaxNs, o.offsetNs);
        forwardGapMaxNs = std::max(forwardGapMaxNs, o.forwardGapNs);
        backwardGapMaxNs = std::max(backwardGapMaxNs, o.backwardGapNs);
        if (o.repaired && !hasRepairSample) {
            firstRepair = o;
            hasRepairSample = true;
        }
        if ((o.missing || o.duplicate || o.backward) && !hasRawIssueSample) {
            firstRawIssue = o;
            hasRawIssueSample = true;
        }
        last = o;
    }

    void merge(const PtsDiagnosticsWindow& w) {
        if (w.observed == 0)
            return;
        observed += w.observed;
        repaired += w.repaired;
        missing += w.missing;
        duplicate += w.duplicate;
        backward += w.backward;
        catchup += w.catchup;
        repairRunMax = std::max(repairRunMax, w.repairRunMax);
        offsetMaxNs = std::max(offsetMaxNs, w.offsetMaxNs);
        forwardGapMaxNs = std::max(forwardGapMaxNs, w.forwardGapMaxNs);
        backwardGapMaxNs = std::max(backwardGapMaxNs, w.backwardGapMaxNs);
        if (!hasRepairSample && w.hasRepairSample) {
            firstRepair = w.firstRepair;
            hasRepairSample = true;
        }
        if (!hasRawIssueSample && w.hasRawIssueSample) {
            firstRawIssue = w.firstRawIssue;
            hasRawIssueSample = true;
        }
        last = w.last;
    }
};
