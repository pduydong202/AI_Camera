#pragma once

#include "config.hpp"

#include <opencv2/opencv.hpp>
#include <cstdint>
#include <deque>
#include <vector>

struct FrameResult {
    bool warmupDone = false;
    bool hasValidMotion = false;

    double fgRatio = 0.0;
    int l1RawPixels = 0;
    int l1Pixels = 0;

    // L2 is evidence only. It does not destructively modify L1.
    int l2RepeatEvidencePixels = 0;
    int l2RepeatCells = 0;
    float l2RepeatOtsu = 0.0F;
    int l2CandidateComponents = 0;
    int l2RepetitiveComponents = 0;
    int l2NovelComponents = 0;

    // L3 is rain/noise evidence only. It does not destructively modify L1.
    int l3RainNoisePixels = 0;
    int l3TemporalObjectSeedPixels = 0;
    int l3WideObjectSeedPixels = 0;
    int l3LocationProtectedPixels = 0;

    // Event decision is made per current L1 connected component.
    int candidateComponents = 0;
    int repeatExplainedComponents = 0;
    int rainExplainedComponents = 0;
    int fullyExplainedComponents = 0;
    int validComponents = 0;
    int validPixels = 0;
    double validRatio = 0.0;

    int tawlTemporalFrames = 0;
    int tawlWidthPx = 0;
    int l2CellSizePx = 0;
    int l2GridCols = 0;
    int l2GridRows = 0;
};

class MotionPipeline {
public:
    MotionPipeline(const Config& cfg, double sourceFps, const cv::Size& analysisSize);

    FrameResult process(const cv::Mat& gray,
                        std::int64_t ptsNs,
                        cv::Mat* l1Debug = nullptr,
                        cv::Mat* l2RepeatDebug = nullptr,
                        cv::Mat* l3RainNoiseDebug = nullptr,
                        cv::Mat* validDebug = nullptr);

    int mogHistory() const { return mogHistory_; }
    int tawlTemporalFrames() const { return taFrames_; }
    int tawlWidthPx() const { return widthThresholdPx_; }
    int l2CellSizePx() const { return cellSizePx_; }
    int l2GridCols() const { return gridCols_; }
    int l2GridRows() const { return gridRows_; }
    int l2HistoryFrames() const { return l2HistoryFrames_; }

private:
    cv::Mat poolForegroundDensity(const cv::Mat& l1Mask) const;
    bool otsuHighMask(const cv::Mat& values, cv::Mat& highMask, float& threshold) const;

    // L2 historical prior: where repetitive activity has accumulated.
    cv::Mat buildL2RepeatRegion(FrameResult& result) const;

    // Current re-entry is computed from the current frame against history,
    // before history is updated with the current frame.
    void computeL2DeltaAndReentry(const cv::Mat& currentDensity,
                                  cv::Mat& delta,
                                  cv::Mat& currentReentryMask,
                                  cv::Mat& currentReentryEvidence);
    void updateL2Memory(const cv::Mat& currentDensity,
                        const cv::Mat& currentReentryEvidence);

    // L2 is intentionally simple: the historical repeat region is a prior.
    // A current connected component is marked repetitive only when every pixel
    // of that component is contained inside the learned repeat region. If even
    // one part leaves the repeat region, the whole current component is kept as
    // novel motion. No tracker, path history, IoU, area, or distance threshold.
    cv::Mat buildL2CurrentRepeatEvidence(const cv::Mat& l1Mask,
                                         const cv::Mat& repeatRegion,
                                         FrameResult& result) const;

    // L3: pixels explained as short + narrow + isolated rain/noise.
    cv::Mat buildL3RainNoiseEvidence(const cv::Mat& l1Mask, FrameResult& result);

    // A current L1 component is ignored only when every one of its pixels is
    // explained by the union of L2 repetitive evidence and L3 rain/noise evidence.
    void classifyCurrentComponents(const cv::Mat& l1Mask,
                                   const cv::Mat& l2RepeatEvidence,
                                   const cv::Mat& l3RainNoiseEvidence,
                                   FrameResult& result,
                                   cv::Mat* validMask) const;

    Config cfg_;
    cv::Size size_;
    double analysisFps_ = 0.0;

    // L1
    cv::Ptr<cv::BackgroundSubtractorMOG2> mog_;
    int mogHistory_ = 1;
    cv::Mat cleanupKernel_;

    // L2: local-grid recurrence prior + current component containment.
    int cellSizePx_ = 1;
    int gridCols_ = 1;
    int gridRows_ = 1;
    int l2HistoryFrames_ = 1;
    cv::Mat occupancyMemory_;
    cv::Mat reentryMemory_;
    cv::Mat occupancySum_;
    cv::Mat reentrySum_;
    cv::Mat previousDensity_;
    cv::Mat reentryArmed_;
    cv::Mat reentryArmedAge_;
    std::deque<cv::Mat> occupancyWindow_;
    std::deque<cv::Mat> reentryWindow_;
    cv::Mat gridNeighborhood_;

    // L3: TAWL temporal / width / location.
    cv::Mat temporalAge_;
    cv::Mat pixelNeighborhood_;
    int taFrames_ = 1;
    int widthThresholdPx_ = 1;

    std::int64_t firstPtsNs_ = -1;
};
