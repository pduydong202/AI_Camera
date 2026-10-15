#pragma once

#include "config.hpp"
#include "live_frame_budget.hpp"
#include "pts_diagnostics.hpp"
#include "recorder.hpp"

#include <opencv2/core.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FrameTiming {
    double waitMs = 0.0;
    double mapMs = 0.0;
    double rgaMs = 0.0;
    double rgaCpuMs = 0.0;
    double pullMs = 0.0;
    std::uint64_t pulled = 0;
    std::uint64_t skipped = 0;
    std::uint64_t inputGaps = 0;
    std::uint64_t metadataMissing = 0;
    std::uint64_t ptsRepaired = 0;
    PtsDiagnosticsWindow pts;
    std::int64_t decodedArrivalNs = -1;
    double queueAgeMs = -1.0;
};

struct GrayFrame {
    cv::Mat image;
    std::int64_t ptsNs = 0;
    std::int64_t durationNs = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    double fps = 0.0;
    FrameTiming timing;
};

class GstMppSource {
  public:
    GstMppSource(const std::string& input, const Config& cfg, const std::string& codec);
    ~GstMppSource();

    void start();
    void stop();

    // Nonblocking cancellation; join consumers before setting pipeline NULL.
    void requestStop() noexcept {
        stopRequested_.store(true);
    }

    void setStopFlag(const std::atomic<bool>& flag) {
        externalStop_ = &flag;
    }

    bool pullGrayFrame(GrayFrame& out);
    bool pullEncodedPacket(std::shared_ptr<EncodedPacket>& out);
    PtsDiagnosticsWindow takeEncodedPtsWindow();

    bool recordBranchEnabled() const {
        return cfg_.recordEnabled != 0;
    }

    bool isRtsp() const {
        return isRtsp_;
    }

    int samplingBurstFrames() const {
        return liveBudget_.burstFrames();
    }

    bool hasError() const {
        return hasError_.load();
    }

    bool eos() const {
        return eos_.load();
    }

    std::string errorMessage() const;

    const std::string& pipelineDescription() const {
        return pipelineText_;
    }

    // Counts decoded buffers arriving at appsink, including buffers it may drop.
    // Read concurrently with the decoder's streaming thread.
    std::int64_t decodedFrames() const {
        return analysisProbeId_ != 0
                   ? static_cast<std::int64_t>(decodedFrames_.load(std::memory_order_relaxed))
                   : -1;
    }

  private:
    std::string buildPipeline() const;

    bool stopping() const {
        return stopRequested_.load() || (externalStop_ && externalStop_->load());
    }

    void busLoop();
    std::int64_t normalizeAnalysisPts(std::int64_t pts, std::int64_t duration, double fps);
    std::int64_t normalizeEncodedPts(std::int64_t pts, std::int64_t duration);

    std::string input_;
    Config cfg_;
    std::string codec_;
    bool isRtsp_ = false;
    LiveFrameBudget liveBudget_;
    std::string pipelineText_;

    void* pipeline_ = nullptr;
    void* analysisSink_ = nullptr;
    void* encodedSink_ = nullptr;
    void* bus_ = nullptr;
    void* videoInfo_ = nullptr;
    void* analysisProbePad_ = nullptr;
    unsigned long analysisProbeId_ = 0;
    std::atomic<std::uint64_t> decodedFrames_{0};
    std::uint64_t lastPulledSequence_ = 0;

    std::thread busThread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};
    const std::atomic<bool>* externalStop_ = nullptr;
    std::atomic<bool> eos_{false};
    std::atomic<bool> hasError_{false};

    mutable std::mutex errorMutex_;
    std::string errorMessage_;

    std::int64_t lastAnalysisPtsNs_ = -1;
    std::int64_t nextAnalysisDuePtsNs_ = -1;
    std::int64_t lastEncodedPtsNs_ = -1;
    PtsDiagnosticsTracker analysisPtsTracker_;
    PtsDiagnosticsTracker encodedPtsTracker_;
    std::mutex encodedPtsMutex_;
    PtsDiagnosticsWindow encodedPtsWindow_;
    double cachedFps_ = 0.0;
    int cachedWidth_ = 0;
    int cachedHeight_ = 0;
    cv::Size cachedAnalysisSize_;

    // Firefly RGA output. RGA writes NV12 into this owned buffer; GrayFrame
    // exposes only its Y plane to the motion core. The allocation stays stable
    // for the lifetime of the source, so no CPU resize/copy is required.
    std::vector<std::uint8_t> rgaOutputBuffer_;
    int rgaOutputStride_ = 0;
    int rgaOutputHeightStride_ = 0;
    bool rgaSchedulerConfigured_ = false;
    bool rgaGeometryChecked_ = false;
};
