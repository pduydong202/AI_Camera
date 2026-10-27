#include "config.hpp"
#include "event_manager.hpp"
#include "gst_mpp_source.hpp"
#include "l2_capture.hpp"
#include "log_line.hpp"
#include "main_affinity.hpp"
#include "motion_pipeline.hpp"
#include "recorder.hpp"
#include "redact.hpp"
#include "sample_stats.hpp"
#include "supervised_process.hpp"
#include "validation_recorder.hpp"

#include <gst/gst.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Sys = std::chrono::system_clock;

namespace {
std::atomic<bool> stopFlag{false};
static_assert(std::atomic<bool>::is_always_lock_free, "Signal stop flag must be lock-free");

void onSignal(int) {
    stopFlag = true;
}

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

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double processCpuSeconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

double mainCpuSeconds() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0)
        return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) / 1e9;
}

std::string allowedCpus() {
    cpu_set_t cpus{};
    if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0)
        return "unknown";
    std::ostringstream out;
    bool first = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &cpus)) {
            if (!first)
                out << ',';
            out << cpu;
            first = false;
        }
    }
    return out.str();
}

void printPtsSample(const char* branch, const char* kind, const PtsObservation& sample,
                    double wallSec) {
    std::ostringstream line;
    line << "[PTS_SAMPLE] wall_sec=" << f2(wallSec) << " branch=" << branch << " kind=" << kind
         << " index=" << sample.index << " reason=" << sample.reason() << " raw_ns=" << sample.rawNs
         << " previous_raw_ns=" << sample.previousRawNs
         << " previous_used_ns=" << sample.previousUsedNs << " used_ns=" << sample.usedNs
         << " raw_dts_ns=" << sample.rawDtsNs << " raw_duration_ns=" << sample.rawDurationNs
         << " used_duration_ns=" << sample.usedDurationNs
         << " offset_ms=" << f6(static_cast<double>(sample.offsetNs) / 1e6)
         << " repair_run=" << sample.repairRun << '\n';
    // Keep the whole row in one insertion to reduce interleaving with
    // recorder-thread output.
    LogLine(std::cout) << line.str();
}

void printPtsWindow(const char* branch, const PtsDiagnosticsWindow& window, double wallSec,
                    double dtSec) {
    if (window.observed == 0)
        return;
    std::ostringstream line;
    line << "[PTS] wall_sec=" << f2(wallSec) << " dt_sec=" << f2(dtSec) << " branch=" << branch
         << " observed=" << window.observed << " repaired=" << window.repaired
         << " raw_missing=" << window.missing << " raw_duplicate=" << window.duplicate
         << " raw_backward=" << window.backward << " catchup=" << window.catchup
         << " repair_run_max=" << window.repairRunMax
         << " offset_max_ms=" << f6(static_cast<double>(window.offsetMaxNs) / 1e6)
         << " raw_forward_gap_max_ms=" << f6(static_cast<double>(window.forwardGapMaxNs) / 1e6)
         << " raw_backward_gap_max_ms=" << f6(static_cast<double>(window.backwardGapMaxNs) / 1e6)
         << " last_raw_ns=" << window.last.rawNs << " last_used_ns=" << window.last.usedNs << '\n';
    LogLine(std::cout) << line.str();
    if (window.hasRepairSample)
        printPtsSample(branch, "first_repair", window.firstRepair, wallSec);
    if (window.hasRawIssueSample &&
        (!window.hasRepairSample || window.firstRawIssue.index != window.firstRepair.index))
        printPtsSample(branch, "first_raw_issue", window.firstRawIssue, wallSec);
}

struct PerformanceWindow {
    std::uint64_t frames = 0;
    FrameTiming source;
    double processMs = 0.0;
    double processCpuMs = 0.0;
    MotionTiming motion;
    double eventMs = 0.0;
    double loopMs = 0.0;
    double processMaxMs = 0.0;
    double rgaMaxMs = 0.0;
    double ptsGapMaxMs = 0.0;
    SampleStats queueAge;
    SampleStats localDelay;

    void add(const FrameTiming& t, double process, double event, double loop, double ptsGap,
             double localDelayMs, double processCpu, const MotionTiming& stages) {
        ++frames;
        source.waitMs += t.waitMs;
        source.mapMs += t.mapMs;
        source.rgaMs += t.rgaMs;
        source.rgaCpuMs += t.rgaCpuMs;
        source.pullMs += t.pullMs;
        source.pulled += t.pulled;
        source.skipped += t.skipped;
        source.inputGaps += t.inputGaps;
        source.metadataMissing += t.metadataMissing;
        source.ptsRepaired += t.ptsRepaired;
        source.pts.merge(t.pts);
        processMs += process;
        processCpuMs += processCpu;
        for (std::size_t i = 0; i < motionStageCount; ++i) {
            motion.stages[i].wallMs += stages.stages[i].wallMs;
            motion.stages[i].cpuMs += stages.stages[i].cpuMs;
        }
        motion.l3RegionPixels += stages.l3RegionPixels;
        motion.l3ImagePixels += stages.l3ImagePixels;
        motion.l3EmptyFastFrames += stages.l3EmptyFastFrames;
        eventMs += event;
        loopMs += loop;
        processMaxMs = std::max(processMaxMs, process);
        rgaMaxMs = std::max(rgaMaxMs, t.rgaMs);
        ptsGapMaxMs = std::max(ptsGapMaxMs, ptsGap);
        if (t.decodedArrivalNs >= 0 && t.queueAgeMs >= 0.0 && localDelayMs >= 0.0) {
            queueAge.add(t.queueAgeMs);
            localDelay.add(localDelayMs);
        }
    }

    void clear() {
        frames = 0;
        source = FrameTiming{};
        processMs = eventMs = loopMs = 0.0;
        processCpuMs = 0.0;
        motion = MotionTiming{};
        processMaxMs = rgaMaxMs = ptsGapMaxMs = 0.0;
        queueAge.clear();
        localDelay.clear();
    }
};

std::string safe(std::string s) {
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
            c = '_';
    if (s.empty())
        throw std::runtime_error("camera_id cannot be empty");
    return s;
}

std::string codecNorm(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s != "h264" && s != "h265")
        throw std::runtime_error("codec must be h264 or h265");
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
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    std::ostringstream s;
    s << utc(now, "%Y%m%dT%H%M%S") << '.' << std::setw(6) << std::setfill('0') << (us % 1000000)
      << "Z_pid" << getpid();
    return s.str();
}

std::string redact(const std::string& u) {
    return redactUris(u);
}

// Own the packet thread so every exception path requests cancellation and
// joins it while source/recorder/error state still exist.
class PacketWorker {
  public:
    PacketWorker(GstMppSource& source, const recovery::Progress& progress)
        : source_(source), progress_(progress) {}

    ~PacketWorker() {
        quiesce();
    }

    template <class F> void start(F&& f) {
        thread_ = std::thread(std::forward<F>(f));
    }

    void quiesce() {
        if (done_)
            return;
        done_ = true;
        if (std::uncaught_exceptions() != 0)
            progress_.fatal();
        progress_.finishing();
        source_.requestStop();
        if (thread_.joinable())
            thread_.join();
    }

  private:
    GstMppSource& source_;
    const recovery::Progress& progress_;
    std::thread thread_;
    bool done_ = false;
};
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 &&
            (std::string(argv[1]) == "--camera-list" || std::string(argv[1]) == "--output-root" ||
             std::string(argv[1]) == "--runtime-settings")) {
            const Config config = loadConfig(argv[2]);
            const auto cameras = loadCameras(argv[2]);
            if (std::string(argv[1]) == "--output-root")
                LogLine(std::cout) << config.outputRoot << '\n';
            else if (std::string(argv[1]) == "--runtime-settings")
                writeRuntimeSettings(config, std::cout);
            else
                for (const auto& camera : cameras)
                    LogLine(std::cout) << camera.id << '\t' << camera.urlEnv << '\t' << camera.codec
                                       << '\t' << camera.gopHintSec << '\n';
            return 0;
        }
        bool workerMode = false;
        int progressFd = -1;
        if (argc >= 3 && std::string(argv[1]) == "--worker") {
            std::size_t used = 0;
            progressFd = std::stoi(argv[2], &used);
            if (used != std::string(argv[2]).size() || progressFd < 0)
                throw std::runtime_error("Invalid worker progress descriptor");
            workerMode = true;
            argc -= 2;
            argv += 2;
        }
        if (argc < 5) {
            LogLine(std::cout) << "Usage: ./build/motion_filter <rtsp-or-mp4> <config.yaml> "
                                  "<camera_id> <h264|h265> [gop_hint_sec]\n";
            return 1;
        }
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        std::signal(SIGPIPE, SIG_IGN);
        if (!workerMode) {
            if (const char* expected = std::getenv("MOTION_LAUNCHER_PID")) {
                std::size_t used = 0;
                const auto parent = std::stol(expected, &used);
                if (used != std::string(expected).size() || parent <= 1 ||
                    prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent)
                    throw std::runtime_error("Launcher disappeared before camera startup");
            }
        }

        const std::string input = argv[1];
        const std::string policy = argv[2];
        const std::string camera = safe(argv[3]);
        const std::string codec = codecNorm(argv[4]);
        const double gopHintSec = argc >= 6 ? std::max(0.0, std::stod(argv[5])) : 0.0;
        Config cfg = loadConfig(policy);
        if (!workerMode && cfg.rtspReconnectEnabled != 0 && input.rfind("rtsp://", 0) == 0) {
            const recovery::Options options{cfg.rtspStartupTimeoutSec,  cfg.rtspStallTimeoutSec,
                                            cfg.rtspShutdownTimeoutSec, cfg.rtspRetryInitialSec,
                                            cfg.rtspRetryMaxSec,        cfg.rtspRetryResetSec,
                                            cfg.rtspWatchdogPollMs,     cfg.rtspRetryMultiplier};
            return recovery::supervise("/proc/self/exe",
                                       {input, policy, camera, codec, std::to_string(gopHintSec)},
                                       options, stopFlag, std::cout);
        }
        const recovery::Progress progress(progressFd);
        if (cfg.recordEnabled && cfg.recordDestination == "http") {
            const auto* url = std::getenv(cfg.recordServerUrlEnv.c_str());
            const auto* token = std::getenv(cfg.recordTokenEnv.c_str());
            if (!url || !token || !*url || !*token)
                throw std::runtime_error("Set video server URL/token environment variables before running");
            cfg.recordServerUrl = url;
            cfg.recordToken = token;
            if ((cfg.recordServerUrl.rfind("http://", 0) != 0 &&
                 cfg.recordServerUrl.rfind("https://", 0) != 0) ||
                cfg.recordServerUrl.find_first_of("\r\n\t ?#@") != std::string::npos ||
                cfg.recordToken.find_first_of("\r\n") != std::string::npos)
                throw std::runtime_error("Invalid video server URL/token (use a base HTTP(S) URL without credentials)");
            while (!cfg.recordServerUrl.empty() && cfg.recordServerUrl.back() == '/')
                cfg.recordServerUrl.pop_back();
        }
        gst_init(&argc, &argv);
        cv::setUseOptimized(true);
        cv::setNumThreads(cfg.opencvThreads);
        if (const char* run = std::getenv("MOTION_RUN_ID")) {
            if (safe(run) != run)
                throw std::runtime_error("Invalid run id");
            cfg.outputRoot = (fs::path(cfg.outputRoot) / run).string();
        }
        cfg.recordCameraId = camera;
        cfg.recordSessionId = sessionId();
        const fs::path root = fs::path(cfg.outputRoot) / camera / "sessions" / cfg.recordSessionId;
        cfg.outputRoot = root.string();
        fs::create_directories(root);
        if (cfg.recordDestination == "file") fs::create_directories(root / "clips");
        if (cfg.validationEnabled) fs::create_directories(root / "masks");

        std::ofstream eventCsv(root / "events.csv");
        eventCsv.exceptions(std::ios::badbit | std::ios::failbit);
        eventCsv << "event,start_sec,stop_sec\n";
        std::ofstream stats;
        if (cfg.frameStatsEnabled) {
            stats.open(root / "frame_stats.csv");
            stats.exceptions(std::ios::badbit | std::ios::failbit);
            stats << "pts_sec,warmup,fg_ratio,l1_raw,l1_clean,l2_repeat,repeat_cells,repeat_otsu,"
                     "l2_components,l2_repeat_components,l2_novel_components,l3_rain,ta_object_"
                     "seed,w_object_seed,l_protected,components,repeat_explained,rain_explained,"
                     "fully_explained,valid_components,valid_pixels,valid_ratio,motion\n";
        }

        // Open before starting worker threads so an open failure is handled by
        // the existing top-level error path without a joinable packet thread.
        std::unique_ptr<l2capture::Writer> l2Capture;
        std::string l2CaptureError;
        if (cfg.l2CaptureEnabled != 0)
            l2Capture = std::make_unique<l2capture::Writer>((root / "l2_capture.bin").string());

        AsyncClipWriter writer(cfg);
        if (cfg.recordEnabled)
            writer.start();
        EncodedRecorder recorder(cfg, writer, codec, gopHintSec);
        GstMppSource source(input, cfg, codec);
        source.setStopFlag(stopFlag);

        std::atomic<bool> packetFailed{false};
        std::mutex errorMutex;
        std::string packetError;
        PacketWorker packetThread(source, progress);
        try {
            source.start();
        } catch (const std::exception& e) {
            if (!source.isRtsp())
                throw;
            LogLine(std::cerr) << "[SOURCE_END] retryable=1 message=" << e.what() << '\n';
            return recovery::retryExitCode;
        }
        if (cfg.recordEnabled) {
            packetThread.start([&] {
                try {
                    std::shared_ptr<EncodedPacket> packet;
                    while (!stopFlag && source.pullEncodedPacket(packet))
                        recorder.pushPacket(packet);
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    packetError = e.what();
                    packetFailed = true;
                    stopFlag = true;
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    packetError = "unknown exception";
                    packetFailed = true;
                    stopFlag = true;
                }
            });
        }

        std::unique_ptr<MotionPipeline> motion;
        std::unique_ptr<EventManager> events;
        std::unique_ptr<ValidationRecorder> eventMasks;
        std::unordered_map<std::uint64_t, std::int64_t> starts;

        long long analysisFrames = 0;
        long long motionFrames = 0;
        std::int64_t firstPts = -1, lastPts = 0, lastDur = 0, nextLogPts = 0;
        const auto wallStart = Clock::now();
        GrayFrame frame;
        const bool measure = cfg.perfStatsEnabled != 0;
        PerformanceWindow perf;
        auto perfStart = wallStart;
        double previousProcessCpu = measure ? processCpuSeconds() : 0.0;
        double previousMainCpu = measure ? mainCpuSeconds() : 0.0;
        auto previousDecoded = source.decodedFrames();
        std::int64_t previousPerfPts = -1;

        LogLine(std::cout)
            << "================================================\n"
            << "MOTION FILTER\n"
            << "================================================\n"
            << "Camera             : " << camera << "\n"
            << "Input              : " << redact(input) << "\n"
            << "Codec              : " << codec << "\n"
            << "Decoder            : " << cfg.decoderElement << "\n"
            << "Resize backend     : RGA3\n"
            << "OpenCV threads     : " << cfg.opencvThreads << "\n"
            << "Performance log    : " << (measure ? "on (wall/CPU timing)" : "off") << "\n"
            << "CPU allowed (main) : " << allowedCpus() << "\n"
            << "RTSP drop latency  : " << (cfg.rtspDropOnLatency != 0 ? "on" : "off") << "\n"
            << "RTSP buffer mode   : " << cfg.rtspBufferMode
            << (cfg.rtspBufferMode == 0   ? " (RTP timestamps)"
                : cfg.rtspBufferMode == 3 ? " (auto)"
                                          : "")
            << "\n"
            << "Analysis box       : " << cfg.analysisMaxWidth << "x" << cfg.analysisMaxHeight
            << " (aspect preserved)\n"
            << "Analysis target    : " << cfg.analysisTargetFps << " FPS\n"
            << "Analysis sampler   : " << (cfg.analysisSamplingEnabled != 0 ? "on" : "off") << "\n"
            << "Sampling clock     : "
            << (source.isRtsp() ? "receiver monotonic time" : "media PTS") << "\n"
            << "Analysis buffers   : " << cfg.analysisMaxBuffers << "\n"
            << "Sparse L3          : " << (cfg.analysisSparseL3Enabled != 0 ? "on" : "off") << "\n"
            << "L2 capture         : "
            << (l2Capture ? "continuous, lossless (diagnostic only)" : "off") << "\n"
            << "Live burst budget  : "
            << (source.isRtsp() && cfg.analysisSamplingEnabled != 0
                    ? std::to_string(source.samplingBurstFrames()) +
                          " frames; extra=" + std::to_string(cfg.analysisRateBurstMs) + " ms"
                    : "inactive")
            << "\n"
            << "Warmup             : " << cfg.startupWarmupSec << " s\n"
            << "Event min ratio    : " << cfg.eventMinValidRatio << "\n"
            << "Recorder           : " << cfg.recordDestination << " (compressed PTS/keyframe RAM ring)\n"
            << "Output             : " << root << "\n"
            << "================================================\n";

        while (!stopFlag) {
            const auto loopStart = measure ? Clock::now() : Clock::time_point{};
            if (!source.pullGrayFrame(frame))
                break;
            if (writer.hasError())
                throw std::runtime_error(writer.errorMessage());
            if (!motion) {
                motion = std::make_unique<MotionPipeline>(cfg, frame.fps, frame.image.size());
                if (l2Capture) {
                    try {
                        l2capture::Header h;
                        h.width = static_cast<std::uint32_t>(frame.image.cols);
                        h.height = static_cast<std::uint32_t>(frame.image.rows);
                        h.cols = static_cast<std::uint32_t>(motion->l2GridCols());
                        h.rows = static_cast<std::uint32_t>(motion->l2GridRows());
                        h.history = static_cast<std::uint32_t>(motion->l2HistoryFrames());
                        h.taFrames = static_cast<std::uint32_t>(motion->tawlTemporalFrames());
                        h.widthThreshold = static_cast<std::uint32_t>(motion->tawlWidthPx());
                        h.fps = std::min(frame.fps, cfg.analysisTargetFps);
                        if (!(h.fps > 0.0))
                            h.fps = cfg.analysisTargetFps;
                        h.eventRatio = cfg.eventMinValidRatio;
                        l2Capture->begin(h);
                    } catch (const std::exception& e) {
                        l2CaptureError = e.what();
                        stopFlag = true;
                        break;
                    }
                }
                events = std::make_unique<EventManager>(cfg);
                if (cfg.validationEnabled != 0) {
                    eventMasks = std::make_unique<ValidationRecorder>(
                        cfg, std::min(frame.fps, cfg.analysisTargetFps), frame.image.size());
                    eventMasks->start();
                }
                LogLine(std::cout)
                    << "Analysis actual    : " << frame.image.cols << "x" << frame.image.rows
                    << "\n"
                    << "MOG history        : " << motion->mogHistory() << " (OpenCV default)\n"
                    << "L2 grid            : " << motion->l2CellSizePx() << " px / "
                    << motion->l2GridCols() << "x" << motion->l2GridRows() << "\n"
                    << "L2 history         : " << motion->l2HistoryFrames() << " analysis frames\n"
                    << "TAWL TA            : " << cfg.tawlTemporalFpsRatio
                    << " x analysis FPS = " << motion->tawlTemporalFrames() << " frames\n"
                    << "TAWL W             : " << cfg.tawlWidthFrameRatio
                    << " x analysis width = " << motion->tawlWidthPx() << " px\n";
                // Decoder/bus/packet/writer threads already exist. Pin only
                // this thread; each reconnect starts again from parent affinity.
                pinMainThread(cfg.mainCpuList);
                LogLine(std::cout)
                    << "[AFFINITY] main_pid=" << getpid() << " cpus=" << allowedCpus() << '\n';
            }

            if (firstPts < 0)
                firstPts = frame.ptsNs;
            lastPts = frame.ptsNs;
            lastDur = frame.durationNs;
            ++analysisFrames;

            MotionTiming frameStages;
            const double processCpuStart = measure ? timingThreadCpuMs() : 0.0;
            const auto processStart = measure ? Clock::now() : Clock::time_point{};
            FrameResult r;
            if (eventMasks || l2Capture) {
                cv::Mat l1, l2, l3, valid;
                r = motion->process(frame.image, frame.ptsNs, &l1, eventMasks ? &l2 : nullptr, &l3,
                                    &valid, measure ? &frameStages : nullptr);
                if (eventMasks)
                    eventMasks->pushFrame(l1, l2, l3, valid, frame.ptsNs);
                if (l2Capture) {
                    try {
                        l2capture::Meta m;
                        m.ptsNs = frame.ptsNs;
                        m.warmup = r.warmupDone ? 1U : 0U;
                        m.motion = r.hasValidMotion ? 1U : 0U;
                        m.l1Pixels = static_cast<std::uint32_t>(r.l1Pixels);
                        m.rainPixels = static_cast<std::uint32_t>(r.l3RainNoisePixels);
                        m.validPixels = static_cast<std::uint32_t>(r.validPixels);
                        m.repeatCells = static_cast<std::uint32_t>(r.l2RepeatCells);
                        m.repeatPixels = static_cast<std::uint32_t>(r.l2RepeatEvidencePixels);
                        m.repeatComponents = static_cast<std::uint32_t>(r.l2RepetitiveComponents);
                        m.otsu = r.l2RepeatOtsu;
                        const auto& density = motion->lastL2Density();
                        const std::array<l2capture::Plane<std::uint8_t>, 3> masks{
                            {{l1.ptr<std::uint8_t>(), l1.step1()},
                             {l3.ptr<std::uint8_t>(), l3.step1()},
                             {valid.ptr<std::uint8_t>(), valid.step1()}}};
                        l2Capture->append(m, {density.ptr<float>(), density.step1()}, masks);
                    } catch (const std::exception& e) {
                        l2CaptureError = e.what();
                        stopFlag = true;
                        break;
                    }
                }
            } else {
                // Production path: do not build/copy L1/L2/L3/VALID debug masks.
                // Detection/event logic is identical; only observability work is skipped.
                r = motion->process(frame.image, frame.ptsNs, nullptr, nullptr, nullptr, nullptr,
                                    measure ? &frameStages : nullptr);
            }
            const double processMs = measure ? elapsedMs(processStart) : 0.0;
            const double processCpuMs = measure ? timingThreadCpuMs() - processCpuStart : 0.0;
            if (r.hasValidMotion)
                ++motionFrames;

            const auto eventStart = measure ? Clock::now() : Clock::time_point{};
            if (auto signal = events->process(r.hasValidMotion, frame.ptsNs, frame.durationNs)) {
                if (signal->type == EventSignal::Type::Start) {
                    starts[signal->eventId] = signal->ptsNs;
                    recorder.startEvent(signal->eventId, signal->ptsNs);
                    if (eventMasks)
                        eventMasks->startEvent(signal->eventId, signal->ptsNs);
                    LogLine(std::cout) << "[EVENT START] " << signal->eventId
                                       << " at=" << f2(sec(signal->ptsNs - firstPts)) << "s\n";
                } else {
                    const auto st =
                        starts.count(signal->eventId) ? starts[signal->eventId] : signal->ptsNs;
                    eventCsv << signal->eventId << ',' << sec(st - firstPts) << ','
                             << sec(signal->ptsNs - firstPts) << '\n';
                    eventCsv.flush();
                    recorder.stopEvent(signal->eventId);
                    if (eventMasks)
                        eventMasks->stopEvent(signal->eventId);
                    starts.erase(signal->eventId);
                    LogLine(std::cout) << "[EVENT STOP]  " << signal->eventId
                                       << " at=" << f2(sec(signal->ptsNs - firstPts)) << "s\n";
                }
            }

            const double eventMs = measure ? elapsedMs(eventStart) : 0.0;
            double localDelayMs = -1.0;
            if (measure && frame.timing.decodedArrivalNs >= 0) {
                const auto doneNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        Clock::now().time_since_epoch())
                                        .count();
                localDelayMs = static_cast<double>(std::max<std::int64_t>(
                                   0, doneNs - frame.timing.decodedArrivalNs)) /
                               1e6;
            }

            if (stats.is_open()) {
                stats << sec(frame.ptsNs - firstPts) << ',' << (r.warmupDone ? 1 : 0) << ','
                      << r.fgRatio << ',' << r.l1RawPixels << ',' << r.l1Pixels << ','
                      << r.l2RepeatEvidencePixels << ',' << r.l2RepeatCells << ',' << r.l2RepeatOtsu
                      << ',' << r.l2CandidateComponents << ',' << r.l2RepetitiveComponents << ','
                      << r.l2NovelComponents << ',' << r.l3RainNoisePixels << ','
                      << r.l3TemporalObjectSeedPixels << ',' << r.l3WideObjectSeedPixels << ','
                      << r.l3LocationProtectedPixels << ',' << r.candidateComponents << ','
                      << r.repeatExplainedComponents << ',' << r.rainExplainedComponents << ','
                      << r.fullyExplainedComponents << ',' << r.validComponents << ','
                      << r.validPixels << ',' << r.validRatio << ',' << (r.hasValidMotion ? 1 : 0)
                      << '\n';
            }

            if (frame.ptsNs - firstPts >= nextLogPts) {
                LogLine(std::cout)
                    << "[STAT] t=" << f2(sec(frame.ptsNs - firstPts)) << "s"
                    << " warmup=" << (r.warmupDone ? "DONE" : "LEARNING")
                    << " fg=" << f2(r.fgRatio * 100.0) << "%" << " L1raw=" << r.l1RawPixels
                    << " L1clean=" << r.l1Pixels << " L2_repeat=" << r.l2RepeatEvidencePixels
                    << " repeat_cells=" << r.l2RepeatCells << " repeatT=" << f6(r.l2RepeatOtsu)
                    << " L3_rain=" << r.l3RainNoisePixels << " valid_px=" << r.validPixels
                    << " valid_ratio=" << f6(r.validRatio)
                    << " motion=" << (r.hasValidMotion ? 1 : 0)
                    << " events=" << events->eventCount() << "\n";
                nextLogPts += static_cast<std::int64_t>(cfg.statsIntervalSec * 1e9);
            }
            if (measure) {
                const double ptsGapMs = previousPerfPts >= 0
                                            ? static_cast<double>(std::max<std::int64_t>(
                                                  0, frame.ptsNs - previousPerfPts)) /
                                                  1e6
                                            : 0.0;
                previousPerfPts = frame.ptsNs;
                perf.add(frame.timing, processMs, eventMs, elapsedMs(loopStart), ptsGapMs,
                         localDelayMs, processCpuMs, frameStages);
                const auto now = Clock::now();
                const double dt = std::chrono::duration<double>(now - perfStart).count();
                if (dt >= cfg.statsIntervalSec) {
                    const double frames = static_cast<double>(perf.frames);
                    const auto decoded = source.decodedFrames();
                    const double decodeFps =
                        decoded >= 0 && previousDecoded >= 0
                            ? static_cast<double>(decoded - previousDecoded) / dt
                            : std::numeric_limits<double>::quiet_NaN();
                    const double processCpu = processCpuSeconds();
                    const double mainCpu = mainCpuSeconds();
                    const double sourceOtherMs =
                        std::max(0.0, perf.source.pullMs - perf.source.waitMs - perf.source.mapMs -
                                          perf.source.rgaMs);
                    LogLine(std::cout)
                        << "[PERF] wall_sec="
                        << f2(std::chrono::duration<double>(now - wallStart).count())
                        << " dt_sec=" << f2(dt) << " frames=" << perf.frames
                        << " analysis_fps=" << f2(frames / dt) << " decode_fps=" << f2(decodeFps)
                        << " pull_fps=" << f2(static_cast<double>(perf.source.pulled) / dt)
                        << " sample_skips=" << perf.source.skipped
                        << " input_gaps=" << perf.source.inputGaps
                        << " metadata_misses=" << perf.source.metadataMissing
                        << " pts_repaired=" << perf.source.ptsRepaired
                        << " wait_ms=" << f2(perf.source.waitMs / frames)
                        << " map_ms=" << f2(perf.source.mapMs / frames)
                        << " rga_ms=" << f2(perf.source.rgaMs / frames)
                        << " rga_cpu_ms=" << f2(perf.source.rgaCpuMs / frames)
                        << " source_other_ms=" << f2(sourceOtherMs / frames)
                        << " process_ms=" << f2(perf.processMs / frames)
                        << " process_cpu_ms=" << f2(perf.processCpuMs / frames)
                        << " event_ms=" << f2(perf.eventMs / frames)
                        << " loop_ms=" << f2(perf.loopMs / frames)
                        << " rga_max_ms=" << f2(perf.rgaMaxMs)
                        << " process_max_ms=" << f2(perf.processMaxMs)
                        << " pts_gap_max_ms=" << f2(perf.ptsGapMaxMs)
                        << " cpu_pct=" << f2(100.0 * (processCpu - previousProcessCpu) / dt)
                        << " main_cpu_ms=" << f2(1000.0 * (mainCpu - previousMainCpu) / frames)
                        << " latency_frames=" << perf.queueAge.count()
                        << " queue_age_ms=" << f2(perf.queueAge.mean())
                        << " queue_p95_ms=" << f2(perf.queueAge.p95())
                        << " queue_max_ms=" << f2(perf.queueAge.maximum())
                        << " local_delay_ms=" << f2(perf.localDelay.mean())
                        << " local_p95_ms=" << f2(perf.localDelay.p95())
                        << " local_max_ms=" << f2(perf.localDelay.maximum()) << std::endl;
                    const double ptsWallSec =
                        std::chrono::duration<double>(now - wallStart).count();
                    printPtsWindow("analysis", perf.source.pts, ptsWallSec, dt);
                    if (source.recordBranchEnabled())
                        printPtsWindow("encoded", source.takeEncodedPtsWindow(), ptsWallSec, dt);
                    std::ostringstream stageLine;
                    stageLine << "[STAGE] wall_sec="
                              << f2(std::chrono::duration<double>(now - wallStart).count())
                              << " dt_sec=" << f2(dt) << " frames=" << perf.frames;
                    for (std::size_t i = 0; i < motionStageCount; ++i) {
                        stageLine << ' ' << motionStageNames[i]
                                  << "_ms=" << f2(perf.motion.stages[i].wallMs / frames) << ' '
                                  << motionStageNames[i]
                                  << "_cpu_ms=" << f2(perf.motion.stages[i].cpuMs / frames);
                    }
                    stageLine << " l3_region_pct="
                              << f2(perf.motion.l3ImagePixels > 0
                                        ? 100.0 * static_cast<double>(perf.motion.l3RegionPixels) /
                                              static_cast<double>(perf.motion.l3ImagePixels)
                                        : 0.0)
                              << " l3_empty_fast=" << perf.motion.l3EmptyFastFrames;
                    LogLine(std::cout) << stageLine.str() << '\n';
                    perf.clear();
                    perfStart = now;
                    previousDecoded = decoded;
                    previousProcessCpu = processCpu;
                    previousMainCpu = mainCpu;
                }
            }
            progress.frame();
        }

        // Stop and join consumers before EOS/finalizing clips or releasing MPP.
        if (!l2CaptureError.empty() || packetFailed.load())
            progress.fatal();
        packetThread.quiesce();
        if (events) {
            const auto endPts = lastPts + std::max<std::int64_t>(0, lastDur);
            if (auto signal = events->finish(endPts)) {
                const auto st =
                    starts.count(signal->eventId) ? starts[signal->eventId] : signal->ptsNs;
                eventCsv << signal->eventId << ',' << sec(st - firstPts) << ','
                         << sec(signal->ptsNs - firstPts) << '\n';
                recorder.stopEvent(signal->eventId);
                if (eventMasks)
                    eventMasks->stopEvent(signal->eventId);
            }
        }

        recorder.finish();
        if (eventMasks) {
            eventMasks->finish();
            eventMasks->close();
        }
        writer.close();
        if (writer.hasError()) {
            progress.fatal();
            throw std::runtime_error(writer.errorMessage());
        }
        source.stop();

        if (!l2CaptureError.empty())
            throw std::runtime_error(l2CaptureError);
        if (l2Capture) {
            l2Capture->finish();
            LogLine(std::cout) << "[L2CAPTURE] complete=1 frames=" << l2Capture->frames()
                               << " file=" << (root / "l2_capture.bin") << '\n';
        }

        if (packetFailed) {
            std::lock_guard<std::mutex> lock(errorMutex);
            throw std::runtime_error("encoded packet branch failed: " + packetError);
        }
        if (!stopFlag && source.hasError()) {
            LogLine(std::cerr) << "[SOURCE_END] retryable=" << source.isRtsp()
                               << " message=" << source.errorMessage() << '\n';
            return source.isRtsp() ? recovery::retryExitCode : 1;
        }
        if (!stopFlag && source.isRtsp() && source.eos()) {
            LogLine(std::cout) << "[SOURCE_END] retryable=1 reason=rtsp_eos\n";
            return recovery::retryExitCode;
        }

        const double elapsed = std::chrono::duration<double>(Clock::now() - wallStart).count();
        LogLine(std::cout) << "================================================\n"
                           << "DONE\n"
                           << "Analysis frames: " << analysisFrames << "\n"
                           << "Motion frames  : " << motionFrames << "\n"
                           << "Events         : " << (events ? events->eventCount() : 0) << "\n"
                           << "Throughput     : "
                           << f2(elapsed > 0.0 ? analysisFrames / elapsed : 0.0)
                           << " analysis fps\n"
                           << "Output         : " << root << "\n"
                           << "================================================\n";
        return 0;
    } catch (const std::exception& e) {
        LogLine(std::cerr) << "[ERROR] " << e.what() << "\n";
        return 1;
    }
}
