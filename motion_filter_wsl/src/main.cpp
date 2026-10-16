#include "analysis_size.hpp"
#include "config.hpp"
#include "event_manager.hpp"
#include "full_mask_recorder.hpp"
#include "motion_pipeline.hpp"
#include "offline_recorder.hpp"
#include "validation_recorder.hpp"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Sys = std::chrono::system_clock;

namespace {

double sec(std::int64_t ns) {
    return static_cast<double>(ns) / 1e9;
}

std::string f2(double x) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << x;
    return s.str();
}

std::string f6(double x) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(6) << x;
    return s.str();
}

std::string safe(std::string s) {
    for (char& c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
            c = '_';
        }
    }
    if (s.empty()) {
        s = "camera";
    }
    return s;
}

std::string utc(Sys::time_point tp, const char* fmt) {
    const auto t = Sys::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream s;
    s << std::put_time(&tm, fmt);
    return s.str();
}

std::string sessionId() {
    const auto now = Sys::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    std::ostringstream s;
    s << utc(now, "%Y%m%dT%H%M%S")
      << '.' << std::setw(6) << std::setfill('0') << (us % 1000000)
      << "Z_pid" << getpid();
    return s.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cout
                << "Usage: ./build/motion_filter <video> [policy.yaml] [camera_id]\n";
            return 1;
        }

        const std::string input = argv[1];
        const std::string policy = argc >= 3 ? argv[2] : "policy.yaml";
        Config cfg = loadConfig(policy);
        const std::string camera = safe(
            argc >= 4 ? argv[3] : fs::path(input).stem().string());

        const std::string sid = sessionId();
        const fs::path root =
            fs::path(cfg.outputRoot) / camera / "sessions" / sid;
        cfg.outputRoot = root.string();
        fs::create_directories(root / "clips");
        fs::create_directories(root / "masks");
        fs::create_directories(root / "masks_full");

        cv::VideoCapture cap(input, cv::CAP_FFMPEG);
        if (!cap.isOpened()) {
            cap.open(input);
        }
        if (!cap.isOpened()) {
            throw std::runtime_error("Cannot open video: " + input);
        }

        double fps = cap.get(cv::CAP_PROP_FPS);
        if (!(fps > 0.1) || !std::isfinite(fps)) {
            fps = cfg.fallbackFps;
        }

        const int sourceWidth =
            static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        const int sourceHeight =
            static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            throw std::runtime_error("Invalid source size");
        }

        const cv::Size analysis =
            chooseAnalysisSize(sourceWidth, sourceHeight, cfg);
        const double analysisFps = std::min(fps, cfg.analysisTargetFps);
        const std::int64_t sourceDurationNs =
            static_cast<std::int64_t>(std::llround(1e9 / fps));
        const std::int64_t analysisStepNs =
            static_cast<std::int64_t>(std::llround(1e9 / analysisFps));

        MotionPipeline motion(cfg, fps, analysis);
        EventManager events(cfg);

        OfflineClipRecorder recorder(
            cfg,
            fps,
            cv::Size(sourceWidth, sourceHeight));
        recorder.start();

        // Event-aligned L1/L2/L3 evidence clips.
        ValidationRecorder eventMasks(cfg, analysisFps, analysis);
        eventMasks.start();

        // Full-video L1/L2/L3 evidence masks for WSL inspection.
        FullMaskRecorder fullMasks(cfg, analysisFps, analysis);
        fullMasks.open();

        std::ofstream eventCsv(root / "events.csv");
        eventCsv << "event,start_sec,stop_sec\n";

        std::ofstream stats;
        if (cfg.frameStatsEnabled) {
            stats.open(root / "frame_stats.csv");
            stats
                << "pts_sec,warmup,fg_ratio,l1_raw_px,l1_clean_px,"
                << "l2_repeat_evidence_px,l2_repeat_cells,l2_repeat_otsu,"
                << "l2_candidate_components,l2_repetitive_components,l2_novel_components,"
                << "l3_rain_noise_px,l3_ta_object_seed,l3_w_object_seed,"
                << "l3_location_protected,"
                << "candidate_components,repeat_explained_components,"
                << "rain_explained_components,fully_explained_components,"
                << "valid_components,valid_pixels,valid_ratio,motion\n";
        }

        std::cout
            << "================================================\n"
            << "MOTION FILTER - WSL FIXED\n"
            << "================================================\n"
            << "Input              : " << input << "\n"
            << "Source             : " << sourceWidth << "x" << sourceHeight
            << " @ " << f2(fps) << " FPS\n"
            << "Analysis box       : " << cfg.analysisMaxWidth << "x"
            << cfg.analysisMaxHeight << "\n"
            << "Analysis actual    : " << analysis.width << "x"
            << analysis.height << " (aspect preserved)\n"
            << "Analysis rate      : " << f2(analysisFps) << " FPS\n"
            << "Warmup             : " << f2(cfg.startupWarmupSec) << " s\n"
            << "MOG2               : OpenCV defaults, shadows OFF\n"
            << "L1                  : MOG2 foreground + 3x3 opening\n"
            << "L2                  : finite-window repeat prior + containment\n"
            << "L2 cell/grid        : " << motion.l2CellSizePx() << " px / "
            << motion.l2GridCols() << "x" << motion.l2GridRows() << "\n"
            << "L2 history          : " << motion.l2HistoryFrames() << " analysis frames (finite window)\n"
            << "TAWL TA             : " << cfg.tawlTemporalFpsRatio
            << " x analysis FPS = " << motion.tawlTemporalFrames() << " frames\n"
            << "TAWL W              : " << cfg.tawlWidthFrameRatio
            << " x analysis width = " << motion.tawlWidthPx() << " px\n"
            << "L3                  : TAWL evidence + component-level temporal support\n"
            << "Event               : valid_ratio >= " << cfg.eventMinValidRatio
            << " of analysis frame\n"
            << "Full masks          : masks_full/full_L1/L2/L3*.mp4\n"
            << "Event masks         : masks/motion_XXXXXX_L1/L2/L3.mp4\n"
            << "Output              : " << root << "\n"
            << "================================================\n";

        cv::Mat bgr;
        cv::Mat gray;
        cv::Mat resized;
        long long sourceFrames = 0;
        long long analysisFrames = 0;
        long long motionFrames = 0;
        std::int64_t firstPts = -1;
        std::int64_t previousPts = -1;
        std::int64_t nextAnalysisPts = -1;
        std::int64_t lastPts = 0;
        std::int64_t nextLogPts = 0;
        std::unordered_map<std::uint64_t, std::int64_t> starts;
        const auto wallStart = Clock::now();

        while (cap.read(bgr)) {
            const double posMs = cap.get(cv::CAP_PROP_POS_MSEC);
            std::int64_t pts =
                (std::isfinite(posMs) && posMs >= 0.0)
                    ? static_cast<std::int64_t>(std::llround(posMs * 1e6))
                    : sourceFrames * sourceDurationNs;

            if (previousPts >= 0 && pts <= previousPts) {
                pts = previousPts + sourceDurationNs;
            }
            previousPts = pts;
            lastPts = pts;
            if (firstPts < 0) {
                firstPts = pts;
            }

            recorder.pushFrame(bgr, pts);
            ++sourceFrames;

            if (nextAnalysisPts < 0) {
                nextAnalysisPts = pts;
            }
            if (pts < nextAnalysisPts) {
                continue;
            }
            do {
                nextAnalysisPts += analysisStepNs;
            } while (nextAnalysisPts <= pts);
            ++analysisFrames;

            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            if (gray.size() == analysis) {
                resized = gray;
            } else {
                cv::resize(gray, resized, analysis, 0.0, 0.0, cv::INTER_AREA);
            }

            cv::Mat l1;
            cv::Mat l2Repeat;
            cv::Mat l3RainNoise;
            cv::Mat validMotion;
            FrameResult r = motion.process(
                resized,
                pts,
                &l1,
                &l2Repeat,
                &l3RainNoise,
                &validMotion);

            // Full masks are always the evidence maps for the complete video.
            fullMasks.write(l1, l2Repeat, l3RainNoise, validMotion);

            // The event-mask recorder maintains a pre-roll ring even before an event.
            eventMasks.pushFrame(l1, l2Repeat, l3RainNoise, validMotion, pts);

            if (r.hasValidMotion) {
                ++motionFrames;
            }

            if (auto signal =
                    events.process(r.hasValidMotion, pts, analysisStepNs)) {
                if (signal->type == EventSignal::Type::Start) {
                    starts[signal->eventId] = signal->ptsNs;
                    recorder.startEvent(signal->eventId, signal->ptsNs);
                    eventMasks.startEvent(signal->eventId, signal->ptsNs);
                    std::cout
                        << "[EVENT START] " << signal->eventId
                        << " at=" << f2(sec(signal->ptsNs - firstPts)) << "s\n";
                } else {
                    const auto st = starts.count(signal->eventId)
                        ? starts[signal->eventId]
                        : signal->ptsNs;
                    eventCsv
                        << signal->eventId << ','
                        << sec(st - firstPts) << ','
                        << sec(signal->ptsNs - firstPts) << '\n';
                    eventCsv.flush();
                    recorder.stopEvent(signal->eventId);
                    eventMasks.stopEvent(signal->eventId);
                    starts.erase(signal->eventId);
                    std::cout
                        << "[EVENT STOP]  " << signal->eventId
                        << " at=" << f2(sec(signal->ptsNs - firstPts)) << "s\n";
                }
            }

            if (stats.is_open()) {
                stats
                    << sec(pts - firstPts) << ','
                    << (r.warmupDone ? 1 : 0) << ','
                    << r.fgRatio << ','
                    << r.l1RawPixels << ','
                    << r.l1Pixels << ','
                    << r.l2RepeatEvidencePixels << ','
                    << r.l2RepeatCells << ','
                    << r.l2RepeatOtsu << ','
                    << r.l2CandidateComponents << ','
                    << r.l2RepetitiveComponents << ','
                    << r.l2NovelComponents << ','
                    << r.l3RainNoisePixels << ','
                    << r.l3TemporalObjectSeedPixels << ','
                    << r.l3WideObjectSeedPixels << ','
                    << r.l3LocationProtectedPixels << ','
                    << r.candidateComponents << ','
                    << r.repeatExplainedComponents << ','
                    << r.rainExplainedComponents << ','
                    << r.fullyExplainedComponents << ','
                    << r.validComponents << ','
                    << r.validPixels << ','
                    << r.validRatio << ','
                    << (r.hasValidMotion ? 1 : 0) << '\n';
            }

            if (pts - firstPts >= nextLogPts) {
                std::cout
                    << "[STAT] t=" << f2(sec(pts - firstPts)) << "s"
                    << " warmup=" << (r.warmupDone ? "DONE" : "LEARNING")
                    << " fg=" << f2(r.fgRatio * 100.0) << "%"
                    << " L1raw=" << r.l1RawPixels
                    << " L1clean=" << r.l1Pixels
                    << " L2_repeat=" << r.l2RepeatEvidencePixels
                    << " repeat_cells=" << r.l2RepeatCells
                    << " repeatT=" << f6(r.l2RepeatOtsu)
                    << " L2comp=" << r.l2CandidateComponents
                    << " L2repComp=" << r.l2RepetitiveComponents
                    << " L2novelComp=" << r.l2NovelComponents
                    << " L3_rain=" << r.l3RainNoisePixels
                    << " TAobj=" << r.l3TemporalObjectSeedPixels
                    << " Wobj=" << r.l3WideObjectSeedPixels
                    << " Lprotect=" << r.l3LocationProtectedPixels
                    << " comps=" << r.candidateComponents
                    << " explained=" << r.fullyExplainedComponents
                    << " valid_comp=" << r.validComponents
                    << " valid_px=" << r.validPixels
                    << " valid_ratio=" << f6(r.validRatio)
                    << " motion=" << (r.hasValidMotion ? 1 : 0)
                    << " events=" << events.eventCount()
                    << "\n";
                nextLogPts +=
                    static_cast<std::int64_t>(cfg.statsIntervalSec * 1e9);
            }
        }

        const auto endPts = lastPts + sourceDurationNs;
        if (auto signal = events.finish(endPts)) {
            const auto st = starts.count(signal->eventId)
                ? starts[signal->eventId]
                : signal->ptsNs;
            eventCsv
                << signal->eventId << ','
                << sec(st - firstPts) << ','
                << sec(signal->ptsNs - firstPts) << '\n';
            recorder.stopEvent(signal->eventId);
            eventMasks.stopEvent(signal->eventId);
        }

        recorder.finish();
        recorder.close();
        eventMasks.finish();
        eventMasks.close();
        fullMasks.close();

        const double elapsed =
            std::chrono::duration<double>(Clock::now() - wallStart).count();

        std::cout
            << "================================================\n"
            << "DONE\n"
            << "Source frames  : " << sourceFrames << "\n"
            << "Analysis frames: " << analysisFrames << "\n"
            << "Motion frames  : " << motionFrames << "\n"
            << "Events         : " << events.eventCount() << "\n"
            << "Throughput     : "
            << f2(elapsed > 0.0 ? analysisFrames / elapsed : 0.0)
            << " analysis fps\n"
            << "Output         : " << root << "\n"
            << "================================================\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
}
