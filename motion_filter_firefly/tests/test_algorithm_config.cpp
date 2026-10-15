#include "motion_pipeline.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) {
    if (!valid)
        throw std::runtime_error(message);
}

void compare(const cv::Mat& a, const cv::Mat& b) {
    require(a.size() == b.size() && a.type() == b.type(), "Mask shape/type changed");
    require(cv::norm(a, b, cv::NORM_INF) == 0, "Configured sparse/full mask mismatch");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected config.yaml");
        auto cfg = loadConfig(argv[1]);
        cv::setNumThreads(1);
        const cv::Size size(160, 96);
        cfg.l1LearningRate = 0;
        cfg.l1OpenKernelSize = 1;
        MotionPipeline noOpening(cfg, 20, size);
        auto openedCfg = cfg;
        openedCfg.l1OpenKernelSize = 3;
        MotionPipeline opening(openedCfg, 20, size);
        const cv::Mat black = cv::Mat::zeros(size, CV_8U);
        noOpening.process(black, 0);
        opening.process(black, 0);
        cv::Mat dot = black.clone();
        dot.at<unsigned char>(48, 80) = 255;
        cv::Mat rawMask, openedMask;
        noOpening.process(dot, 50000000, &rawMask);
        opening.process(dot, 50000000, &openedMask);
        require(cv::countNonZero(rawMask) == 1, "YAML kernel=1 must keep isolated foreground");
        require(cv::countNonZero(openedMask) == 0,
                "Default kernel=3 must remove isolated foreground");

        cfg.l1HistoryFrames = 37;
        cfg.l2HistoryFrames = 0;
        MotionPipeline coupled(cfg, 20, size);
        require(coupled.mogHistory() == 37 && coupled.l2HistoryFrames() == 37,
                "History coupling lost");
        cfg.l2HistoryFrames = 19;
        cfg.l2RepeatNeighborhoodSize = 5;
        cfg.l3LocationNeighborhoodSize = 5;
        cfg.componentConnectivity = 4;
        cfg.analysisSparseL3Enabled = 1;
        MotionPipeline sparse(cfg, 20, size);
        cfg.analysisSparseL3Enabled = 0;
        MotionPipeline full(cfg, 20, size);
        require(sparse.l2HistoryFrames() == 19, "Independent L2 history ignored");
        // Changes of neighborhood size must also hold at image/ROI boundaries.
        for (int index = 0; index < 180; ++index) {
            cv::Mat frame = black.clone();
            if (index > 4 && index % 30 < 24) {
                const int x = (index * 3) % (size.width - 12);
                cv::rectangle(frame, cv::Rect(x, index % (size.height - 10), 12, 10),
                              cv::Scalar(240), cv::FILLED);
            }
            cv::Mat a[4], b[4];
            const auto pts = static_cast<std::int64_t>(index) * 50000000;
            const auto ra = sparse.process(frame, pts, &a[0], &a[1], &a[2], &a[3]);
            const auto rb = full.process(frame, pts, &b[0], &b[1], &b[2], &b[3]);
            for (int mask = 0; mask < 4; ++mask)
                compare(a[mask], b[mask]);
            require(ra.hasValidMotion == rb.hasValidMotion && ra.validPixels == rb.validPixels &&
                        ra.l3RainNoisePixels == rb.l3RainNoisePixels,
                    "Configured sparse/full result mismatch");
        }
        std::cout << "PASS: configured morphology/history and 180-frame nondefault neighborhood "
                     "ROI parity\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
