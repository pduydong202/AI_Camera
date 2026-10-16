#include "full_mask_recorder.hpp"

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
int fourcc(const std::string& s) {
    return cv::VideoWriter::fourcc(s[0], s[1], s[2], s[3]);
}
}

FullMaskRecorder::FullMaskRecorder(const Config& cfg,
                                   double fps,
                                   const cv::Size& size)
    : cfg_(cfg), fps_(fps), size_(size) {}

FullMaskRecorder::~FullMaskRecorder() {
    close();
}

void FullMaskRecorder::open() {
    if (!enabled()) {
        return;
    }

    const fs::path dir = fs::path(cfg_.outputRoot) / "masks_full";
    fs::create_directories(dir);
    const int f = fourcc(cfg_.fullMaskFourcc);

    l1_.open((dir / "full_L1_foreground.mp4").string(), f, fps_, size_, false);
    l2_.open((dir / "full_L2_repeat_evidence.mp4").string(), f, fps_, size_, false);
    l3_.open((dir / "full_L3_rain_noise_evidence.mp4").string(), f, fps_, size_, false);
    valid_.open((dir / "full_VALID_motion.mp4").string(), f, fps_, size_, false);

    if (!l1_.isOpened() || !l2_.isOpened() || !l3_.isOpened() || !valid_.isOpened()) {
        close();
        throw std::runtime_error("Cannot open one or more full-mask VideoWriter outputs");
    }
}

void FullMaskRecorder::write(const cv::Mat& l1,
                             const cv::Mat& l2RepeatEvidence,
                             const cv::Mat& l3RainNoiseEvidence,
                             const cv::Mat& validMotion) {
    if (!enabled()) {
        return;
    }
    if (l1_.isOpened()) {
        l1_.write(l1);
    }
    if (l2_.isOpened()) {
        l2_.write(l2RepeatEvidence);
    }
    if (l3_.isOpened()) {
        l3_.write(l3RainNoiseEvidence);
    }
    if (valid_.isOpened()) {
        valid_.write(validMotion);
    }
}

void FullMaskRecorder::close() {
    if (l1_.isOpened()) {
        l1_.release();
    }
    if (l2_.isOpened()) {
        l2_.release();
    }
    if (l3_.isOpened()) {
        l3_.release();
    }
    if (valid_.isOpened()) {
        valid_.release();
    }
}
