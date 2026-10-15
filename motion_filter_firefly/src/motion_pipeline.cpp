#include "motion_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
constexpr double NS_PER_SEC = 1e9;
}

MotionPipeline::MotionPipeline(const Config& cfg, double sourceFps, const cv::Size& analysisSize)
    : cfg_(cfg), size_(analysisSize) {
    analysisFps_ = std::min(sourceFps, cfg_.analysisTargetFps);
    if (!(analysisFps_ > 0.0)) {
        analysisFps_ = cfg_.analysisTargetFps;
    }

    // L1 remains a broad candidate generator. Defaults are explicit in YAML.
    mog_ = cv::createBackgroundSubtractorMOG2(cfg_.l1HistoryFrames, cfg_.l1VarThreshold, false);
    mog_->setDetectShadows(false);
    mogHistory_ = std::max(1, mog_->getHistory());

    // A local topology operator; the validated default is still 3x3.
    cleanupKernel_ = cv::getStructuringElement(
        cv::MORPH_RECT, cv::Size(cfg_.l1OpenKernelSize, cfg_.l1OpenKernelSize));

    // TAWL thresholds follow the configured analysis rate and frame width.
    taFrames_ = std::max(1, static_cast<int>(std::ceil(cfg_.tawlTemporalFpsRatio * analysisFps_)));
    widthThresholdPx_ =
        std::max(1, static_cast<int>(std::ceil(cfg_.tawlWidthFrameRatio * size_.width)));

    // L2 grid scale follows TAWL W so it scales with analysis resolution.
    cellSizePx_ = widthThresholdPx_;
    gridCols_ = std::max(1, (size_.width + cellSizePx_ - 1) / cellSizePx_);
    gridRows_ = std::max(1, (size_.height + cellSizePx_ - 1) / cellSizePx_);

    // L2 finite sliding window: old repetitive regions are forgotten when
    // their evidence leaves the window instead of surviving indefinitely via EMA.
    // Zero preserves the historical coupling to MOG2's history length.
    l2HistoryFrames_ = cfg_.l2HistoryFrames > 0 ? cfg_.l2HistoryFrames : mogHistory_;
    occupancyMemory_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    reentryMemory_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    occupancySum_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    reentrySum_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    previousDensity_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32F);
    reentryArmed_ = cv::Mat::zeros(gridRows_, gridCols_, CV_8U);
    reentryArmedAge_ = cv::Mat::zeros(gridRows_, gridCols_, CV_32S);
    gridNeighborhood_ =
        cv::Mat::ones(cfg_.l2RepeatNeighborhoodSize, cfg_.l2RepeatNeighborhoodSize, CV_8U);

    // L3 temporal support is component-level rather than exact-pixel age.
    temporalAge_ = cv::Mat::zeros(size_, CV_16U);
    pixelNeighborhood_ =
        cv::Mat::ones(cfg_.l3LocationNeighborhoodSize, cfg_.l3LocationNeighborhoodSize, CV_8U);
}

cv::Mat MotionPipeline::poolForegroundDensity(const cv::Mat& l1Mask) const {
    cv::Mat f32;
    l1Mask.convertTo(f32, CV_32F, 1.0 / 255.0);

    cv::Mat density;
    cv::resize(f32, density, cv::Size(gridCols_, gridRows_), 0.0, 0.0, cv::INTER_AREA);
    return density;
}

bool MotionPipeline::otsuHighMask(const cv::Mat& values, cv::Mat& highMask,
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

    // THRESH_BINARY uses the same src > threshold rule as the previous
    // threshold-then-compare implementation, but avoids an extra grid pass.
    const double t8 =
        cv::threshold(scaled8, highMask, 0.0, 255.0, cv::THRESH_BINARY | cv::THRESH_OTSU);

    threshold = static_cast<float>(t8 / 255.0);
    return cv::countNonZero(highMask) > 0;
}

cv::Mat MotionPipeline::buildL2RepeatRegion(FrameResult& result) const {
    // Historical prior only. It identifies regions that repeatedly receive
    // foreground; it is NOT the final L2 label for the current object.
    cv::Mat repeatEvidence = occupancyMemory_.mul(reentryMemory_);

    cv::Mat repeatGrid;
    float repeatThreshold = 0.0F;
    const bool hasRepeat = otsuHighMask(repeatEvidence, repeatGrid, repeatThreshold);
    result.l2RepeatOtsu = repeatThreshold;

    if (!hasRepeat) {
        result.l2RepeatCells = 0;
        return cv::Mat::zeros(size_, CV_8U);
    }

    // The configured neighborhood forms one local region, so motion crossing a cell
    // boundary is still treated as activity in the same neighborhood.
    cv::dilate(repeatGrid, repeatGrid, gridNeighborhood_);
    result.l2RepeatCells = cv::countNonZero(repeatGrid);

    cv::Mat full;
    cv::resize(repeatGrid, full, size_, 0.0, 0.0, cv::INTER_NEAREST);
    // repeatGrid is already binary 0/255; nearest-neighbor keeps it binary.
    return full;
}

void MotionPipeline::computeL2DeltaAndReentry(const cv::Mat& currentDensity,
                                              cv::Mat& currentReentryEvidence) {

    // delta and re-entry mask are only needed inside this step. Keeping them
    // local avoids carrying unused outputs through the per-frame hot path.
    cv::Mat delta;
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
    cv::Mat currentReentryMask;
    cv::bitwise_and(rising, reentryArmed_, currentReentryMask);

    currentReentryEvidence = cv::Mat::zeros(delta.size(), CV_32F);
    positiveChange.copyTo(currentReentryEvidence, currentReentryMask);

    // A completed re-entry consumes the armed state. A later fall arms it again.
    reentryArmed_.setTo(0, currentReentryMask);
    reentryArmedAge_.setTo(0, currentReentryMask);
}

void MotionPipeline::updateL2Memory(const cv::Mat& currentDensity,
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

    const float occCount = static_cast<float>(std::max<std::size_t>(1, occupancyWindow_.size()));
    const float reCount = static_cast<float>(std::max<std::size_t>(1, reentryWindow_.size()));

    occupancyMemory_ = occupancySum_ / occCount;
    reentryMemory_ = reentrySum_ / reCount;

    currentDensity.copyTo(previousDensity_);
}

void MotionPipeline::buildL2CurrentRepeatEvidence(const cv::Mat& labels, int labelCount,
                                                  std::vector<int>& componentPixels,
                                                  std::vector<unsigned char>& repetitiveComponents,
                                                  const cv::Mat& repeatRegion, FrameResult& result,
                                                  cv::Mat* l2Debug,
                                                  cv::Rect* foregroundBounds) const {

    if (foregroundBounds)
        *foregroundBounds = cv::Rect{};

    componentPixels.assign(static_cast<std::size_t>(std::max(1, labelCount)), 0);
    repetitiveComponents.assign(static_cast<std::size_t>(std::max(1, labelCount)), 0);

    if (l2Debug) {
        *l2Debug = cv::Mat::zeros(size_, CV_8U);
    }

    if (labelCount <= 1) {
        return;
    }

    std::vector<int> insideRepeat(static_cast<std::size_t>(labelCount), 0);
    int left = labels.cols, right = -1, top = -1, bottom = -1;

    // One scan computes both component size and L2 containment.
    for (int y = 0; y < labels.rows; ++y) {
        const int* lp = labels.ptr<int>(y);
        const uchar* rp = repeatRegion.ptr<uchar>(y);
        int rowFirst = -1, rowLast = -1;
        for (int x = 0; x < labels.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }
            if (foregroundBounds) {
                if (rowFirst < 0)
                    rowFirst = x;
                rowLast = x;
            }
            ++componentPixels[static_cast<std::size_t>(id)];
            if (rp[x] != 0) {
                ++insideRepeat[static_cast<std::size_t>(id)];
            }
        }
        if (rowLast >= 0) {
            left = std::min(left, rowFirst);
            right = std::max(right, rowLast);
            if (top < 0)
                top = y;
            bottom = y;
        }
    }
    if (foregroundBounds && right >= 0) {
        *foregroundBounds = cv::Rect(left, top, right - left + 1, bottom - top + 1);
    }

    for (int id = 1; id < labelCount; ++id) {
        const int count = componentPixels[static_cast<std::size_t>(id)];
        if (count <= 0) {
            continue;
        }

        ++result.l2CandidateComponents;

        // Exact original containment rule: only a component with every pixel
        // inside the historical repeat region is repetitive.
        if (insideRepeat[static_cast<std::size_t>(id)] == count) {
            repetitiveComponents[static_cast<std::size_t>(id)] = 1;
            ++result.l2RepetitiveComponents;
            result.l2RepeatEvidencePixels += count;
        } else {
            ++result.l2NovelComponents;
        }
    }

    // Full L2 evidence image is needed only for optional validation output.
    // Production classification uses the exact whole-component flag above.
    if (l2Debug) {
        for (int y = 0; y < labels.rows; ++y) {
            const int* lp = labels.ptr<int>(y);
            uchar* ep = l2Debug->ptr<uchar>(y);
            for (int x = 0; x < labels.cols; ++x) {
                const int id = lp[x];
                if (id > 0 && repetitiveComponents[static_cast<std::size_t>(id)] != 0) {
                    ep[x] = 255;
                }
            }
        }
    }
}

void MotionPipeline::buildL3RainNoiseEvidence(const cv::Mat& l1Mask, const cv::Mat& labels,
                                              int labelCount, std::vector<int>& rainPixels,
                                              FrameResult& result, cv::Mat* l3Debug,
                                              const cv::Rect& foregroundBounds,
                                              MotionTiming* timing) {

    // l1Mask is already binary 0/255 after local morphology.
    const cv::Mat& fg = l1Mask;

    rainPixels.assign(static_cast<std::size_t>(std::max(1, labelCount)), 0);
    if (l3Debug) {
        *l3Debug = cv::Mat::zeros(size_, CV_8U);
    }

    const bool sparse = cfg_.analysisSparseL3Enabled != 0;
    const cv::Rect region = sparse ? foregroundBounds : cv::Rect(0, 0, size_.width, size_.height);
    if (timing) {
        timing->l3ImagePixels = static_cast<std::uint64_t>(size_.width) * size_.height;
        timing->l3RegionPixels = static_cast<std::uint64_t>(region.width) * region.height;
    }
    if (sparse && region.empty()) {
        // No foreground: the original code clears all temporal ages and
        // returns zero rain/object evidence. Do the same without its scans.
        temporalAge_.setTo(0);
        result.l3TemporalObjectSeedPixels = 0;
        result.l3WideObjectSeedPixels = 0;
        result.l3LocationProtectedPixels = 0;
        result.l3RainNoisePixels = 0;
        if (timing)
            timing->l3EmptyFastFrames = 1;
        return;
    }
    const cv::Mat labelsView = labels(region);
    const cv::Mat fgView = fg(region);
    cv::Mat ageView = temporalAge_(region);
    cv::Mat debugView = l3Debug ? (*l3Debug)(region) : cv::Mat{};

    // TA object seed at COMPONENT level, not exact-pixel age.
    cv::Mat inheritedAge;
    if (sparse) {
        // Read the exact kernel neighborhood, including ages just outside
        // today's foreground bounds. Isolate the padded crop explicitly.
        const int rx = pixelNeighborhood_.cols / 2;
        const int ry = pixelNeighborhood_.rows / 2;
        const int x0 = std::max(0, region.x - rx);
        const int y0 = std::max(0, region.y - ry);
        const int x1 = std::min(size_.width, region.x + region.width + rx);
        const int y1 = std::min(size_.height, region.y + region.height + ry);
        const cv::Rect padded(x0, y0, x1 - x0, y1 - y0);
        cv::Mat inheritedPadded;
        cv::dilate(temporalAge_(padded), inheritedPadded, pixelNeighborhood_, cv::Point(-1, -1), 1,
                   cv::BORDER_CONSTANT | cv::BORDER_ISOLATED, cv::morphologyDefaultBorderValue());
        inheritedAge =
            inheritedPadded(cv::Rect(region.x - x0, region.y - y0, region.width, region.height));
    } else {
        cv::dilate(temporalAge_, inheritedAge, pixelNeighborhood_);
    }

    std::vector<unsigned short> nextAge(static_cast<std::size_t>(std::max(1, labelCount)), 1);

    for (int y = 0; y < labelsView.rows; ++y) {
        const int* lp = labelsView.ptr<int>(y);
        const unsigned short* ap = inheritedAge.ptr<unsigned short>(y);
        for (int x = 0; x < labelsView.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }

            const unsigned int candidate = static_cast<unsigned int>(ap[x]) + 1U;
            const unsigned short boundedCandidate = static_cast<unsigned short>(
                std::min<unsigned int>(candidate, std::numeric_limits<unsigned short>::max()));
            nextAge[static_cast<std::size_t>(id)] =
                std::max(nextAge[static_cast<std::size_t>(id)], boundedCandidate);
        }
    }

    temporalAge_.setTo(0);

    // objectSeed is exactly temporalObjectSeed OR wideObjectSeed from the old
    // implementation, built directly to avoid two full masks plus bitwise_or.
    cv::Mat objectSeed = cv::Mat::zeros(region.size(), CV_8U);
    int temporalObjectSeedPixels = 0;

    for (int y = 0; y < labelsView.rows; ++y) {
        const int* lp = labelsView.ptr<int>(y);
        unsigned short* tp = ageView.ptr<unsigned short>(y);
        uchar* op = objectSeed.ptr<uchar>(y);

        for (int x = 0; x < labelsView.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }

            const unsigned short age = nextAge[static_cast<std::size_t>(id)];
            tp[x] = age;

            if (age >= static_cast<unsigned short>(taFrames_)) {
                op[x] = 255;
                ++temporalObjectSeedPixels;
            }
        }
    }
    result.l3TemporalObjectSeedPixels = temporalObjectSeedPixels;

    // W object seed: wide horizontal runs do not look like narrow rain/noise.
    // Mark the same objectSeed mask directly; keep the original W pixel count.
    int wideObjectSeedPixels = 0;
    for (int y = 0; y < fgView.rows; ++y) {
        const uchar* row = fgView.ptr<uchar>(y);
        uchar* object = objectSeed.ptr<uchar>(y);
        int start = -1;

        for (int x = 0; x <= fgView.cols; ++x) {
            const bool on = (x < fgView.cols && row[x] != 0);
            if (on && start < 0) {
                start = x;
            }
            if (!on && start >= 0) {
                const int runLength = x - start;
                if (runLength > widthThresholdPx_) {
                    for (int k = start; k < x; ++k) {
                        object[k] = 255;
                    }
                    wideObjectSeedPixels += runLength;
                }
                start = -1;
            }
        }
    }
    result.l3WideObjectSeedPixels = wideObjectSeedPixels;

    // L: the centered, odd, all-ones kernel includes the seed pixel itself.
    // Therefore the old expressions simplify exactly to:
    //   rain = foreground AND NOT objectNeighborhood
    //   locationProtected = foreground AND objectNeighborhood AND NOT objectSeed
    cv::Mat objectNeighborhood;
    cv::dilate(objectSeed, objectNeighborhood, pixelNeighborhood_);

    int locationProtectedPixels = 0;
    int rainNoisePixels = 0;

    // A single scan produces the same L3 evidence counts per component.
    // Build the full rain mask only when validation output requests it.
    for (int y = 0; y < labelsView.rows; ++y) {
        const int* lp = labelsView.ptr<int>(y);
        const uchar* op = objectSeed.ptr<uchar>(y);
        const uchar* np = objectNeighborhood.ptr<uchar>(y);
        uchar* dp = l3Debug ? debugView.ptr<uchar>(y) : nullptr;

        for (int x = 0; x < labelsView.cols; ++x) {
            const int id = lp[x];
            if (id <= 0) {
                continue;
            }

            if (np[x] == 0) {
                ++rainPixels[static_cast<std::size_t>(id)];
                ++rainNoisePixels;
                if (dp) {
                    dp[x] = 255;
                }
            } else if (op[x] == 0) {
                ++locationProtectedPixels;
            }
        }
    }

    result.l3LocationProtectedPixels = locationProtectedPixels;
    result.l3RainNoisePixels = rainNoisePixels;
}

void MotionPipeline::classifyCurrentComponents(
    const cv::Mat& labels, int labelCount, const std::vector<int>& componentPixels,
    const std::vector<unsigned char>& repetitiveComponents, const std::vector<int>& rainPixels,
    FrameResult& result, cv::Mat* validMask) const {

    if (validMask) {
        *validMask = cv::Mat::zeros(size_, CV_8U);
    }

    if (labelCount <= 1) {
        return;
    }

    std::vector<unsigned char> validComponent;
    if (validMask) {
        validComponent.assign(static_cast<std::size_t>(labelCount), 0);
    }

    for (int id = 1; id < labelCount; ++id) {
        const std::size_t idx = static_cast<std::size_t>(id);
        const int total = componentPixels[idx];
        if (total <= 0) {
            continue;
        }

        ++result.candidateComponents;

        const bool repeatExplained = repetitiveComponents[idx] != 0;
        const bool rainExplained = rainPixels[idx] == total;

        if (repeatExplained) {
            ++result.repeatExplainedComponents;
        }
        if (rainExplained) {
            ++result.rainExplainedComponents;
        }

        // Exact equivalent of the previous per-pixel union:
        // L2 evidence is all-or-nothing for each component. If L2 explains it,
        // the union explains every pixel. Otherwise the union is complete only
        // when L3 rain evidence covers every pixel.
        if (repeatExplained || rainExplained) {
            ++result.fullyExplainedComponents;
            continue;
        }

        ++result.validComponents;
        if (validMask) {
            validComponent[idx] = 1;
        }
        result.validPixels += total;
    }

    // Full VALID mask is an optional observability output only.
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

FrameResult MotionPipeline::process(const cv::Mat& gray, std::int64_t ptsNs, cv::Mat* l1Debug,
                                    cv::Mat* l2RepeatDebug, cv::Mat* l3RainNoiseDebug,
                                    cv::Mat* validDebug, MotionTiming* timing) {
    MotionStageProfiler perf(timing);
    if (gray.empty() || gray.type() != CV_8UC1 || gray.size() != size_) {
        throw std::runtime_error("MotionPipeline expects CV_8UC1 at the configured analysis size");
    }

    if (firstPtsNs_ < 0) {
        firstPtsNs_ = ptsNs;
    }

    FrameResult result;
    result.warmupDone =
        static_cast<double>(std::max<std::int64_t>(0, ptsNs - firstPtsNs_)) / NS_PER_SEC >=
        cfg_.startupWarmupSec;
    result.tawlTemporalFrames = taFrames_;
    result.tawlWidthPx = widthThresholdPx_;
    result.l2CellSizePx = cellSizePx_;
    result.l2GridCols = gridCols_;
    result.l2GridRows = gridRows_;
    perf.mark(MotionStage::Setup);

    // ------------------------------
    // L1: DETECT ALL FOREGROUND
    // ------------------------------
    cv::Mat raw;
    mog_->apply(gray, raw, cfg_.l1LearningRate);
    perf.mark(MotionStage::Mog);

    // With detectShadows(false), OpenCV MOG2 already emits a binary 0/255
    // foreground mask. Reuse it directly instead of a full-frame compare pass.
    cv::Mat rawBinary = raw;
    result.l1RawPixels = cv::countNonZero(rawBinary);
    if (result.l1RawPixels == static_cast<int>(rawBinary.total())) {
        rawBinary.setTo(0);
        result.l1RawPixels = 0;
    }

    // Accepted lightweight cleanup: a single 3x3 opening removes isolated
    // salt/pepper MOG2 specks before connected-component logic.
    cv::Mat l1;
    cv::morphologyEx(rawBinary, l1, cv::MORPH_OPEN, cleanupKernel_);
    perf.mark(MotionStage::Cleanup);

    // ------------------------------
    // L2: EXPLAIN REPETITIVE MOTION
    // ------------------------------
    const cv::Mat currentDensity = poolForegroundDensity(l1);

    // Historical repeat region is a PRIOR only, built before current update.
    const cv::Mat repeatRegion = buildL2RepeatRegion(result);

    cv::Mat currentReentryEvidence;
    computeL2DeltaAndReentry(currentDensity, currentReentryEvidence);
    perf.mark(MotionStage::L2Prior);

    // Connected components are a major CPU cost. Compute them exactly once
    // from the cleaned L1 mask and reuse the identical labels in L2, L3 and
    // final classification. This changes no containment/TAWL/event logic.
    cv::Mat labels;
    const int labelCount = cv::connectedComponents(l1, labels, cfg_.componentConnectivity, CV_32S);
    perf.mark(MotionStage::Ccl);
    std::vector<int> componentPixels;
    std::vector<unsigned char> repetitiveComponents;
    cv::Rect foregroundBounds;

    buildL2CurrentRepeatEvidence(labels, labelCount, componentPixels, repetitiveComponents,
                                 repeatRegion, result, l2RepeatDebug,
                                 cfg_.analysisSparseL3Enabled != 0 ? &foregroundBounds : nullptr);

    // countNonZero(l1) is exactly the sum of all non-background component sizes.
    // Reuse those already-computed sizes instead of another full-frame pass.
    for (int id = 1; id < labelCount; ++id) {
        result.l1Pixels += componentPixels[static_cast<std::size_t>(id)];
    }
    result.fgRatio = l1.total() > 0
                         ? static_cast<double>(result.l1Pixels) / static_cast<double>(l1.total())
                         : 0.0;

    perf.mark(MotionStage::L2Evidence);
    updateL2Memory(currentDensity, currentReentryEvidence);
    perf.mark(MotionStage::L2Update);

    // ------------------------------
    // L3: EXPLAIN RAIN / NOISE
    // ------------------------------
    std::vector<int> rainPixels;
    buildL3RainNoiseEvidence(l1, labels, labelCount, rainPixels, result, l3RainNoiseDebug,
                             foregroundBounds, timing);
    perf.mark(MotionStage::L3);

    // ------------------------------
    // EVENT EVIDENCE DECISION
    // ------------------------------
    classifyCurrentComponents(labels, labelCount, componentPixels, repetitiveComponents, rainPixels,
                              result, validDebug);

    result.validRatio =
        l1.total() > 0 ? static_cast<double>(result.validPixels) / static_cast<double>(l1.total())
                       : 0.0;
    result.hasValidMotion = result.warmupDone && result.validRatio >= cfg_.eventMinValidRatio;
    perf.mark(MotionStage::Decision);

    if (l1Debug) {
        l1.copyTo(*l1Debug);
    }
    perf.mark(MotionStage::Debug);

    return result;
}
