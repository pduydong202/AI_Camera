#include "motion_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
constexpr double NS_PER_SEC = 1e9;
}

MotionPipeline::MotionPipeline(const Config& cfg,
                               double sourceFps,
                               const cv::Size& analysisSize)
    : cfg_(cfg), size_(analysisSize) {
    analysisFps_ = std::min(sourceFps, cfg_.analysisTargetFps);
    if (!(analysisFps_ > 0.0)) {
        analysisFps_ = cfg_.analysisTargetFps;
    }

    // L1: broad candidate generator. Keep OpenCV MOG2 defaults.
    mog_ = cv::createBackgroundSubtractorMOG2();
    mog_->setDetectShadows(false);
    mogHistory_ = std::max(1, mog_->getHistory());

    // Explicitly accepted local cleanup. 3x3 is a local topology operator,
    // not a camera-specific object-size threshold.
    cleanupKernel_ = cv::getStructuringElement(
        cv::MORPH_RECT,
        cv::Size(3, 3));

    // The only explicit algorithm ratios accepted by policy are TAWL TA/W.
    taFrames_ = std::max(
        1,
        static_cast<int>(std::ceil(cfg_.tawlTemporalFpsRatio * analysisFps_)));
    widthThresholdPx_ = std::max(
        1,
        static_cast<int>(std::ceil(cfg_.tawlWidthFrameRatio * size_.width)));

    // L2 grid scale follows TAWL W so it scales with analysis resolution.
    cellSizePx_ = widthThresholdPx_;
    gridCols_ = std::max(1, (size_.width + cellSizePx_ - 1) / cellSizePx_);
    gridRows_ = std::max(1, (size_.height + cellSizePx_ - 1) / cellSizePx_);

    // L2 finite sliding window: old repetitive regions are forgotten when
    // their evidence leaves the window instead of surviving indefinitely via EMA.
    // Reuse MOG2's default history length; this adds no new tuning parameter.
    l2HistoryFrames_ = mogHistory_;
    occupancyMemory_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    reentryMemory_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    occupancySum_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    reentrySum_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    previousDensity_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    reentryArmed_ = cv::Mat::zeros(gridRows_, gridCols_, CV_8U);
    reentryArmedAge_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32S);
    gridNeighborhood_ = cv::Mat::ones(3, 3, CV_8U);

    // L3 temporal support is component-level rather than exact-pixel age.
    temporalAge_ = cv::Mat::zeros(size_, CV_16U);
    pixelNeighborhood_ = cv::Mat::ones(3, 3, CV_8U);
}

cv::Mat MotionPipeline::poolForegroundDensity(const cv::Mat& l1Mask) const {
    cv::Mat f32;
    l1Mask.convertTo(f32, CV_32F, 1.0 / 255.0);

    cv::Mat density;
    cv::resize(
        f32,
        density,
        cv::Size(gridCols_, gridRows_),
        0.0,
        0.0,
        cv::INTER_AREA);
    return density;
}

bool MotionPipeline::otsuHighMask(const cv::Mat& values,
                                  cv::Mat& highMask,
                                  float& threshold) const {
    highMask = cv::Mat::zeros(values.size(), CV_8U);
    threshold = 0.0F;

    // repeatEvidence is already normalized by construction:
    // occupancyMemory and reentryMemory are both in [0,1], so their product
    // is also in [0,1]. Do not min-max stretch only positive cells, because
    // that can amplify tiny residual evidence into a strong repeat region.
    // Otsu sees the whole grid, including zero cells.
    cv::Mat finite = values.clone();
    cv::patchNaNs(finite, 0.0);
    cv::max(finite, 0.0, finite);
    cv::min(finite, 1.0, finite);

    double vmin = 0.0;
    double vmax = 0.0;
    cv::minMaxLoc(finite, &vmin, &vmax);
    if (!(vmax > vmin)) {
        return false;
    }

    cv::Mat scaled8;
    finite.convertTo(scaled8, CV_8U, 255.0);

    cv::Mat ignored;
    const double t8 = cv::threshold(
        scaled8,
        ignored,
        0.0,
        255.0,
        cv::THRESH_BINARY | cv::THRESH_OTSU);

    threshold = static_cast<float>(t8 / 255.0);
    cv::compare(scaled8, t8, highMask, cv::CMP_GT);
    return cv::countNonZero(highMask) > 0;
}

cv::Mat MotionPipeline::buildL2RepeatRegion(FrameResult& result) const {
    // Historical prior only. It identifies regions that repeatedly receive
    // foreground; it is NOT the final L2 label for the current object.
    cv::Mat repeatEvidence = occupancyMemory_.mul(reentryMemory_);

    cv::Mat repeatGrid;
    float repeatThreshold = 0.0F;
    const bool hasRepeat =
        otsuHighMask(repeatEvidence, repeatGrid, repeatThreshold);
    result.l2RepeatOtsu = repeatThreshold;

    if (!hasRepeat || cv::countNonZero(repeatGrid) == 0) {
        result.l2RepeatCells = 0;
        return cv::Mat::zeros(size_, CV_8U);
    }

    // Cell + 8-neighbor forms one local region, so motion crossing a cell
    // boundary is still treated as activity in the same neighborhood.
    cv::dilate(repeatGrid, repeatGrid, gridNeighborhood_);
    result.l2RepeatCells = cv::countNonZero(repeatGrid);

    cv::Mat full;
    cv::resize(repeatGrid, full, size_, 0.0, 0.0, cv::INTER_NEAREST);
    cv::compare(full, 0, full, cv::CMP_GT);
    return full;
}

void MotionPipeline::computeL2DeltaAndReentry(
    const cv::Mat& currentDensity,
    cv::Mat& delta,
    cv::Mat& currentReentryMask,
    cv::Mat& currentReentryEvidence) {

    cv::subtract(currentDensity, previousDensity_, delta);

    // Re-entry is stateful, but the armed state must live on the same finite
    // horizon as the L2 history. Otherwise a fall from minutes ago could pair
    // with an unrelated rise now. No new tuning parameter is introduced: the
    // expiry is exactly l2HistoryFrames_.
    cv::Mat armedMask;
    cv::compare(reentryArmed_, 0, armedMask, cv::CMP_GT);
    cv::add(reentryArmedAge_, cv::Scalar(1), reentryArmedAge_, armedMask);

    cv::Mat expired;
    cv::compare(reentryArmedAge_, l2HistoryFrames_, expired, cv::CMP_GE);
    reentryArmed_.setTo(0, expired);
    reentryArmedAge_.setTo(0, expired);

    cv::Mat falling;
    cv::compare(delta, 0.0, falling, cv::CMP_LT);
    reentryArmed_.setTo(255, falling);
    reentryArmedAge_.setTo(0, falling);

    cv::Mat positiveChange;
    cv::max(delta, 0.0, positiveChange);

    cv::Mat rising;
    cv::compare(positiveChange, 0.0, rising, cv::CMP_GT);
    cv::bitwise_and(rising, reentryArmed_, currentReentryMask);

    currentReentryEvidence = cv::Mat::zeros(delta.size(), CV_32F);
    positiveChange.copyTo(currentReentryEvidence, currentReentryMask);

    // A completed re-entry consumes the armed state. A later fall arms it again.
    reentryArmed_.setTo(0, currentReentryMask);
    reentryArmedAge_.setTo(0, currentReentryMask);
}

void MotionPipeline::updateL2Memory(
    const cv::Mat& currentDensity,
    const cv::Mat& currentReentryEvidence) {

    // Finite-window memory: old evidence is actually removed when it leaves
    // the window, so Otsu cannot keep dead repeat regions alive forever.
    occupancyWindow_.push_back(currentDensity.clone());
    cv::add(occupancySum_, currentDensity, occupancySum_);

    reentryWindow_.push_back(currentReentryEvidence.clone());
    cv::add(reentrySum_, currentReentryEvidence, reentrySum_);

    while (static_cast<int>(occupancyWindow_.size()) > l2HistoryFrames_) {
        cv::subtract(occupancySum_, occupancyWindow_.front(), occupancySum_);
        occupancyWindow_.pop_front();
    }
    while (static_cast<int>(reentryWindow_.size()) > l2HistoryFrames_) {
        cv::subtract(reentrySum_, reentryWindow_.front(), reentrySum_);
        reentryWindow_.pop_front();
    }

    // Avoid tiny negative round-off after many add/subtract cycles.
    cv::max(occupancySum_, 0.0, occupancySum_);
    cv::max(reentrySum_, 0.0, reentrySum_);

    const float occCount =
        static_cast<float>(std::max<std::size_t>(1, occupancyWindow_.size()));
    const float reCount =
        static_cast<float>(std::max<std::size_t>(1, reentryWindow_.size()));

    occupancyMemory_ = occupancySum_ / occCount;
    reentryMemory_ = reentrySum_ / reCount;

    currentDensity.copyTo(previousDensity_);
}

cv::Mat MotionPipeline::buildL2CurrentRepeatEvidence(
    const cv::Mat& l1Mask,
    const cv::Mat& repeatRegion,
    FrameResult& result) const {

    cv::Mat labels;
    const int labelCount = cv::connectedComponents(
        l1Mask,
        labels,
        8,
        CV_32S);

    if (labelCount <= 1) {
        return cv::Mat::zeros(size_, CV_8U);
    }

    std::vector<int> total(static_cast<std::size_t>(labelCount), 0);
    std::vector<int> insideRepeat(static_cast<std::size_t>(labelCount), 0);

    for (int y = 0; y < labels.rows; ++y) {
        const int* lp = labels.ptr<int>(y);
        const uchar* rp = repeatRegion.ptr<uchar>(y);
        for (int x = 0; x < labels.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }
            ++total[static_cast<std::size_t>(id)];
            if (rp[x] != 0) {
                ++insideRepeat[static_cast<std::size_t>(id)];
            }
        }
    }

    std::vector<unsigned char> repetitive(
        static_cast<std::size_t>(labelCount),
        0);

    for (int id = 1; id < labelCount; ++id) {
        const int count = total[static_cast<std::size_t>(id)];
        if (count <= 0) {
            continue;
        }

        ++result.l2CandidateComponents;

        // Containment, not overlap percentage:
        // - 100% inside learned repeat region -> repetitive.
        // - any pixel outside -> preserve the whole current component as novel.
        if (insideRepeat[static_cast<std::size_t>(id)] == count) {
            repetitive[static_cast<std::size_t>(id)] = 1;
            ++result.l2RepetitiveComponents;
        } else {
            ++result.l2NovelComponents;
        }
    }

    cv::Mat evidence = cv::Mat::zeros(size_, CV_8U);
    for (int y = 0; y < labels.rows; ++y) {
        const int* lp = labels.ptr<int>(y);
        uchar* ep = evidence.ptr<uchar>(y);
        for (int x = 0; x < labels.cols; ++x) {
            const int id = lp[x];
            if (id > 0 && repetitive[static_cast<std::size_t>(id)] != 0) {
                ep[x] = 255;
            }
        }
    }

    result.l2RepeatEvidencePixels = cv::countNonZero(evidence);
    return evidence;
}

cv::Mat MotionPipeline::buildL3RainNoiseEvidence(const cv::Mat& l1Mask,
                                                 FrameResult& result) {
    cv::Mat fg;
    cv::compare(l1Mask, 0, fg, cv::CMP_GT);

    // TA object seed at COMPONENT level, not exact-pixel age.
    // A current connected component inherits the maximum age from any
    // 8-neighbor pixel of the previous frame. This protects small moving
    // objects whose pixels shift position without tracker IDs, optical flow,
    // distance thresholds, or any new tunable.
    cv::Mat inheritedAge;
    cv::dilate(temporalAge_, inheritedAge, pixelNeighborhood_);

    cv::Mat labels;
    const int labelCount = cv::connectedComponents(
        fg,
        labels,
        8,
        CV_32S);

    std::vector<unsigned short> nextAge(
        static_cast<std::size_t>(std::max(1, labelCount)),
        1);

    for (int y = 0; y < labels.rows; ++y) {
        const int* lp = labels.ptr<int>(y);
        const unsigned short* ap = inheritedAge.ptr<unsigned short>(y);
        for (int x = 0; x < labels.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }

            const unsigned int candidate =
                static_cast<unsigned int>(ap[x]) + 1U;
            const unsigned short boundedCandidate =
                static_cast<unsigned short>(
                    std::min<unsigned int>(
                        candidate,
                        std::numeric_limits<unsigned short>::max()));
            nextAge[static_cast<std::size_t>(id)] =
                std::max(
                    nextAge[static_cast<std::size_t>(id)],
                    boundedCandidate);
        }
    }

    temporalAge_.setTo(0);
    cv::Mat temporalObjectSeed = cv::Mat::zeros(size_, CV_8U);

    for (int y = 0; y < labels.rows; ++y) {
        const int* lp = labels.ptr<int>(y);
        unsigned short* tp = temporalAge_.ptr<unsigned short>(y);
        uchar* sp = temporalObjectSeed.ptr<uchar>(y);

        for (int x = 0; x < labels.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }

            const unsigned short age =
                nextAge[static_cast<std::size_t>(id)];
            tp[x] = age;

            if (age >= static_cast<unsigned short>(taFrames_)) {
                sp[x] = 255;
            }
        }
    }

    result.l3TemporalObjectSeedPixels = cv::countNonZero(temporalObjectSeed);

    // W object seed: wide horizontal runs do not look like narrow rain/noise.
    cv::Mat wideObjectSeed = cv::Mat::zeros(size_, CV_8U);
    for (int y = 0; y < fg.rows; ++y) {
        const uchar* row = fg.ptr<uchar>(y);
        uchar* wide = wideObjectSeed.ptr<uchar>(y);
        int start = -1;

        for (int x = 0; x <= fg.cols; ++x) {
            const bool on = (x < fg.cols && row[x] != 0);
            if (on && start < 0) {
                start = x;
            }
            if (!on && start >= 0) {
                const int runLength = x - start;
                if (runLength > widthThresholdPx_) {
                    for (int k = start; k < x; ++k) {
                        wide[k] = 255;
                    }
                }
                start = -1;
            }
        }
    }
    result.l3WideObjectSeedPixels = cv::countNonZero(wideObjectSeed);

    cv::Mat objectSeed;
    cv::bitwise_or(temporalObjectSeed, wideObjectSeed, objectSeed);

    // Short + narrow foreground is a rain/noise candidate.
    cv::Mat notObjectSeed;
    cv::bitwise_not(objectSeed, notObjectSeed);
    cv::Mat rainNoiseCandidate;
    cv::bitwise_and(fg, notObjectSeed, rainNoiseCandidate);

    // L: candidates directly touching an object seed are protected, because
    // they may be fragments of the same moving object rather than isolated rain.
    cv::Mat objectNeighborhood;
    cv::dilate(objectSeed, objectNeighborhood, pixelNeighborhood_);

    cv::Mat locationProtected;
    cv::bitwise_and(rainNoiseCandidate, objectNeighborhood, locationProtected);
    result.l3LocationProtectedPixels = cv::countNonZero(locationProtected);

    cv::Mat notObjectNeighborhood;
    cv::bitwise_not(objectNeighborhood, notObjectNeighborhood);

    cv::Mat rainNoiseEvidence;
    cv::bitwise_and(rainNoiseCandidate, notObjectNeighborhood, rainNoiseEvidence);
    result.l3RainNoisePixels = cv::countNonZero(rainNoiseEvidence);
    return rainNoiseEvidence;
}

void MotionPipeline::classifyCurrentComponents(
    const cv::Mat& l1Mask,
    const cv::Mat& l2RepeatEvidence,
    const cv::Mat& l3RainNoiseEvidence,
    FrameResult& result,
    cv::Mat* validMask) const {

    if (validMask) {
        *validMask = cv::Mat::zeros(size_, CV_8U);
    }

    cv::Mat labels;
    const int labelCount = cv::connectedComponents(
        l1Mask,
        labels,
        8,
        CV_32S);

    if (labelCount <= 1) {
        return;
    }

    std::vector<int> total(labelCount, 0);
    std::vector<int> repeat(labelCount, 0);
    std::vector<int> rain(labelCount, 0);
    std::vector<int> explained(labelCount, 0);
    std::vector<unsigned char> validComponent(
        static_cast<std::size_t>(labelCount),
        0);

    for (int y = 0; y < labels.rows; ++y) {
        const int* lp = labels.ptr<int>(y);
        const uchar* rp = l2RepeatEvidence.ptr<uchar>(y);
        const uchar* np = l3RainNoiseEvidence.ptr<uchar>(y);

        for (int x = 0; x < labels.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }

            ++total[id];
            const bool isRepeat = rp[x] != 0;
            const bool isRainNoise = np[x] != 0;
            if (isRepeat) {
                ++repeat[id];
            }
            if (isRainNoise) {
                ++rain[id];
            }
            if (isRepeat || isRainNoise) {
                ++explained[id];
            }
        }
    }

    for (int id = 1; id < labelCount; ++id) {
        if (total[id] <= 0) {
            continue;
        }

        ++result.candidateComponents;
        if (repeat[id] == total[id]) {
            ++result.repeatExplainedComponents;
        }
        if (rain[id] == total[id]) {
            ++result.rainExplainedComponents;
        }

        if (explained[id] == total[id]) {
            ++result.fullyExplainedComponents;
            continue;
        }

        ++result.validComponents;
        validComponent[static_cast<std::size_t>(id)] = 1;

        // If L2/L3 cannot explain the WHOLE connected component, keep the
        // whole component for event-area accounting. Do not shrink a real
        // object merely because part of it looks rain-like.
        result.validPixels += total[id];
    }

    if (validMask) {
        for (int y = 0; y < labels.rows; ++y) {
            const int* lp = labels.ptr<int>(y);
            uchar* vp = validMask->ptr<uchar>(y);
            for (int x = 0; x < labels.cols; ++x) {
                const int id = lp[x];
                if (id > 0 && validComponent[static_cast<std::size_t>(id)] != 0) {
                    vp[x] = 255;
                }
            }
        }
    }
}

FrameResult MotionPipeline::process(const cv::Mat& gray,
                                    std::int64_t ptsNs,
                                    cv::Mat* l1Debug,
                                    cv::Mat* l2RepeatDebug,
                                    cv::Mat* l3RainNoiseDebug,
                                    cv::Mat* validDebug) {
    if (gray.empty() || gray.type() != CV_8UC1 || gray.size() != size_) {
        throw std::runtime_error(
            "MotionPipeline expects CV_8UC1 at the configured analysis size");
    }

    if (firstPtsNs_ < 0) {
        firstPtsNs_ = ptsNs;
    }

    FrameResult result;
    result.warmupDone =
        static_cast<double>(std::max<std::int64_t>(0, ptsNs - firstPtsNs_)) /
            NS_PER_SEC >=
        cfg_.startupWarmupSec;
    result.tawlTemporalFrames = taFrames_;
    result.tawlWidthPx = widthThresholdPx_;
    result.l2CellSizePx = cellSizePx_;
    result.l2GridCols = gridCols_;
    result.l2GridRows = gridRows_;

    // ------------------------------
    // L1: DETECT ALL FOREGROUND
    // ------------------------------
    cv::Mat raw;
    mog_->apply(gray, raw);

    cv::Mat rawBinary;
    cv::compare(raw, 0, rawBinary, cv::CMP_GT);
    if (cv::countNonZero(rawBinary) == static_cast<int>(rawBinary.total())) {
        rawBinary.setTo(0);
    }
    result.l1RawPixels = cv::countNonZero(rawBinary);

    // Accepted lightweight cleanup: a single 3x3 opening removes isolated
    // salt/pepper MOG2 specks before connected-component logic.
    cv::Mat l1;
    cv::morphologyEx(
        rawBinary,
        l1,
        cv::MORPH_OPEN,
        cleanupKernel_);

    result.l1Pixels = cv::countNonZero(l1);
    result.fgRatio = l1.total() > 0
        ? static_cast<double>(result.l1Pixels) /
              static_cast<double>(l1.total())
        : 0.0;

    // ------------------------------
    // L2: EXPLAIN REPETITIVE MOTION
    // ------------------------------
    const cv::Mat currentDensity = poolForegroundDensity(l1);

    // Historical repeat region is a PRIOR only, built before current update.
    const cv::Mat repeatRegion = buildL2RepeatRegion(result);

    cv::Mat delta;
    cv::Mat currentReentryGrid;
    cv::Mat currentReentryEvidence;
    computeL2DeltaAndReentry(
        currentDensity,
        delta,
        currentReentryGrid,
        currentReentryEvidence);

    cv::Mat l2RepeatEvidence = buildL2CurrentRepeatEvidence(
        l1,
        repeatRegion,
        result);

    updateL2Memory(
        currentDensity,
        currentReentryEvidence);

    // ------------------------------
    // L3: EXPLAIN RAIN / NOISE
    // ------------------------------
    cv::Mat l3RainNoiseEvidence = buildL3RainNoiseEvidence(l1, result);

    // ------------------------------
    // EVENT EVIDENCE DECISION
    // ------------------------------
    classifyCurrentComponents(
        l1,
        l2RepeatEvidence,
        l3RainNoiseEvidence,
        result,
        validDebug);

    result.validRatio = l1.total() > 0
        ? static_cast<double>(result.validPixels) /
              static_cast<double>(l1.total())
        : 0.0;
    result.hasValidMotion =
        result.warmupDone &&
        result.validRatio >= cfg_.eventMinValidRatio;

    if (l1Debug) {
        l1.copyTo(*l1Debug);
    }
    if (l2RepeatDebug) {
        l2RepeatEvidence.copyTo(*l2RepeatDebug);
    }
    if (l3RainNoiseDebug) {
        l3RainNoiseEvidence.copyTo(*l3RainNoiseDebug);
    }

    return result;
}
