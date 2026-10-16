#pragma once

#include "config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

class FullMaskRecorder {
public:
    FullMaskRecorder(const Config& cfg, double fps, const cv::Size& size);
    ~FullMaskRecorder();

    void open();
    void write(const cv::Mat& l1,
               const cv::Mat& l2RepeatEvidence,
               const cv::Mat& l3RainNoiseEvidence,
               const cv::Mat& validMotion);
    void close();

    bool enabled() const { return cfg_.fullMaskEnabled != 0; }

private:
    Config cfg_;
    double fps_ = 0.0;
    cv::Size size_;
    cv::VideoWriter l1_;
    cv::VideoWriter l2_;
    cv::VideoWriter l3_;
    cv::VideoWriter valid_;
};
