#include "event_manager.hpp"
#include "motion_pipeline.hpp"
#include "motion_pipeline_reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}

template <class A, class B> void compareResult(const A& a, const B& b, int frame) {
#define SAME(field)                                                                                \
    require(a.field == b.field, "Frame " + std::to_string(frame) + ": " #field " differs")
    SAME(warmupDone);
    SAME(hasValidMotion);
    SAME(fgRatio);
    SAME(l1RawPixels);
    SAME(l1Pixels);
    SAME(l2RepeatEvidencePixels);
    SAME(l2RepeatCells);
    SAME(l2RepeatOtsu);
    SAME(l2CandidateComponents);
    SAME(l2RepetitiveComponents);
    SAME(l2NovelComponents);
    SAME(l3RainNoisePixels);
    SAME(l3TemporalObjectSeedPixels);
    SAME(l3WideObjectSeedPixels);
    SAME(l3LocationProtectedPixels);
    SAME(candidateComponents);
    SAME(repeatExplainedComponents);
    SAME(rainExplainedComponents);
    SAME(fullyExplainedComponents);
    SAME(validComponents);
    SAME(validPixels);
    SAME(validRatio);
    SAME(tawlTemporalFrames);
    SAME(tawlWidthPx);
    SAME(l2CellSizePx);
    SAME(l2GridCols);
    SAME(l2GridRows);
#undef SAME
}

void compareMask(const cv::Mat& a, const cv::Mat& b, const std::string& name) {
    require(a.size() == b.size() && a.type() == b.type(), name + " shape/type differs");
    require(cv::norm(a, b, cv::NORM_INF) == 0.0, name + " pixels differ");
}

void compareSignal(const std::optional<EventSignal>& a, const std::optional<EventSignal>& b) {
    require(a.has_value() == b.has_value(), "event presence differs");
    if (a)
        require(a->type == b->type && a->eventId == b->eventId && a->ptsNs == b->ptsNs,
                "event type/id/time differs");
}

class Pair {
  public:
    Pair(const Config& cfg, const cv::Size& size)
        : reference_(cfg, cfg.analysisTargetFps, size), sparse_(cfg, cfg.analysisTargetFps, size),
          plain_(fullConfig(cfg), cfg.analysisTargetFps, size),
          productionReference_(cfg, cfg.analysisTargetFps, size),
          productionSparse_(cfg, cfg.analysisTargetFps, size), refEvents_(cfg), sparseEvents_(cfg) {
        duration_ = static_cast<std::int64_t>(std::llround(1e9 / cfg.analysisTargetFps));
    }

    void process(const cv::Mat& gray, int frame) {
        cv::Mat refMasks[4], newMasks[4], plainMasks[4];
        const std::int64_t pts = static_cast<std::int64_t>(frame) * duration_;
        const auto a =
            reference_.process(gray, pts, &refMasks[0], &refMasks[1], &refMasks[2], &refMasks[3]);
        MotionTiming timing;
        const auto b = sparse_.process(gray, pts, &newMasks[0], &newMasks[1], &newMasks[2],
                                       &newMasks[3], &timing);
        const auto c = plain_.process(gray, pts, &plainMasks[0], &plainMasks[1], &plainMasks[2],
                                      &plainMasks[3]);
        compareResult(a, b, frame);
        compareResult(a, c, frame);
        compareResult(a, productionReference_.process(gray, pts), frame);
        compareResult(a, productionSparse_.process(gray, pts), frame);
        for (int k = 0; k < 4; ++k) {
            compareMask(refMasks[k], newMasks[k], "sparse mask " + std::to_string(k));
            compareMask(refMasks[k], plainMasks[k], "plain mask " + std::to_string(k));
        }
        compareSignal(refEvents_.process(a.hasValidMotion, pts, duration_),
                      sparseEvents_.process(b.hasValidMotion, pts, duration_));
        totalRegion_ += timing.l3RegionPixels;
        totalImage_ += timing.l3ImagePixels;
        empty_ += timing.l3EmptyFastFrames;
        ++frames_;
        lastEnd_ = pts + duration_;
    }

    void finish(bool requireSparseCoverage) {
        compareSignal(refEvents_.finish(lastEnd_), sparseEvents_.finish(lastEnd_));
        require(refEvents_.eventCount() == sparseEvents_.eventCount(), "final event count differs");
        if (requireSparseCoverage) {
            require(empty_ > 0, "empty fast path not exercised");
            require(totalRegion_ < totalImage_, "ROI path not exercised");
        }
        std::cout << "PASS: " << frames_
                  << " frames, all FrameResult fields/masks/event signals equal; ROI="
                  << (totalImage_ ? 100.0 * totalRegion_ / totalImage_ : 0.0)
                  << "% empty_fast=" << empty_ << '\n';
    }

  private:
    static Config fullConfig(Config cfg) {
        cfg.analysisSparseL3Enabled = 0;
        return cfg;
    }

    ReferenceMotionPipeline reference_;
    MotionPipeline sparse_, plain_;
    ReferenceMotionPipeline productionReference_;
    MotionPipeline productionSparse_;
    EventManager refEvents_, sparseEvents_;
    std::int64_t duration_ = 0, lastEnd_ = 0;
    std::uint64_t totalRegion_ = 0, totalImage_ = 0, empty_ = 0, frames_ = 0;
};

void fixtures(Config cfg) {
    cfg.analysisSparseL3Enabled = 1;
    const cv::Size size(192, 128);
    Pair pair(cfg, size);
    std::mt19937 rng(12345);
    // Strided view, including corners, moving/splitting objects, rain/noise,
    // disappearance/reappearance, illumination change and L2 history expiry.
    cv::Mat backing(size.height + 4, size.width + 8, CV_8U);
    cv::Mat gray = backing(cv::Rect(3, 2, size.width, size.height));
    for (int i = 0; i < 1300; ++i) {
        gray.setTo(40);
        if (i >= 80 && i < 380) {
            const int x = (i - 80) % (size.width - 24);
            cv::rectangle(gray, cv::Rect(x, 45, 24, 18), cv::Scalar(200), cv::FILLED);
        }
        if (i >= 170 && i < 210) {
            cv::rectangle(gray, cv::Rect(0, 0, 14, 12), cv::Scalar(230), cv::FILLED);
            cv::rectangle(gray, cv::Rect(size.width - 14, size.height - 12, 14, 12),
                          cv::Scalar(210), cv::FILLED);
        }
        if (i >= 400 && i < 470) {
            for (int j = 0; j < 30; ++j) {
                const int x = static_cast<int>(rng() % static_cast<unsigned>(size.width));
                const int y = static_cast<int>(rng() % static_cast<unsigned>(size.height));
                cv::line(gray, cv::Point(x, y), cv::Point(x, std::min(y + 6, size.height - 1)),
                         cv::Scalar(180), 3);
            }
        }
        if (i >= 500 && i < 535)
            gray.setTo(120);
        if (i >= 600 && i < 750) {
            cv::rectangle(gray, cv::Rect((i % 2) * (size.width - 30), 0, 30, 10), cv::Scalar(220),
                          cv::FILLED);
            cv::rectangle(gray, cv::Rect(40, 70, (i % 3 == 0 ? 25 : 12), 10), cv::Scalar(180),
                          cv::FILLED);
        }
        if (i >= 1100 && i < 1160)
            cv::rectangle(gray, cv::Rect(80, (i - 1100) % (size.height - 8), 12, 8),
                          cv::Scalar(200), cv::FILLED);
        pair.process(gray, i);
    }
    pair.finish(true);
}

void video(Config cfg, const std::string& path) {
    cfg.analysisSparseL3Enabled = 1;
    cv::VideoCapture capture(path);
    require(capture.isOpened(), "cannot open video: " + path);
    const int width = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH));
    const int height = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT));
    require(width > 0 && height > 0, "video dimensions unavailable");
    const double scale =
        std::min(1.0, std::min(static_cast<double>(cfg.analysisMaxWidth) / width,
                               static_cast<double>(cfg.analysisMaxHeight) / height));
    const cv::Size size(std::max(1, static_cast<int>(std::lround(width * scale))),
                        std::max(1, static_cast<int>(std::lround(height * scale))));
    Pair pair(cfg, size);
    cv::Mat bgr, gray, resized;
    int frame = 0;
    while (capture.read(bgr)) {
        if (bgr.channels() == 1)
            gray = bgr;
        else
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::resize(gray, resized, size, 0.0, 0.0, cv::INTER_AREA);
        pair.process(resized, frame++);
    }
    require(frame > 0, "video has no readable frames");
    pair.finish(false);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "Usage: test_sparse_l3 policy.yaml [video]");
        cv::setNumThreads(1);
        cv::setUseOptimized(true);
        const Config cfg = loadConfig(argv[1]);
        fixtures(cfg);
        if (argc >= 3 && argv[2][0] != '\0')
            video(cfg, argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
