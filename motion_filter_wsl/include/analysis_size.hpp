#pragma once
#include "config.hpp"
#include <opencv2/core.hpp>
#include <algorithm>
#include <cmath>

inline cv::Size chooseAnalysisSize(int width, int height, const Config& cfg) {
    if (width <= 0 || height <= 0) return {};
    double scale = std::min(
        static_cast<double>(cfg.analysisMaxWidth) / width,
        static_cast<double>(cfg.analysisMaxHeight) / height);
    if (!cfg.analysisAllowUpscale) scale = std::min(1.0, scale);
    int w = std::max(2, static_cast<int>(std::lround(width * scale)));
    int h = std::max(2, static_cast<int>(std::lround(height * scale)));
    auto makeEven = [](int v, int limit) {
        if ((v & 1) == 0) return v;
        if (v + 1 <= limit) return v + 1;
        return std::max(2, v - 1);
    };
    w = makeEven(w, cfg.analysisMaxWidth);
    h = makeEven(h, cfg.analysisMaxHeight);
    return {std::max(2,w), std::max(2,h)};
}
