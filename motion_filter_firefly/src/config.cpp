#include "config.hpp"
#include "main_affinity.hpp"
#include "supervised_process.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <opencv2/core.hpp>
#include <ostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
template <class T> void read(const cv::FileNode& parent, const char* key, T& out) {
    const auto n = parent[key];
    if (n.empty())
        throw std::runtime_error(std::string("Missing config key: ") + key);
    bool valid = false;
    if constexpr (std::is_same_v<T, std::string>)
        valid = n.isString();
    else if constexpr (std::is_integral_v<T>)
        valid = n.isInt();
    else
        valid = n.isInt() || n.isReal();
    if (!valid)
        throw std::runtime_error(std::string("Invalid type for config key: ") + key);
    n >> out;
}

void positive(const char* key, double v) {
    if (!std::isfinite(v) || v <= 0)
        throw std::runtime_error(std::string(key) + " must be finite and >0");
}

void nonnegative(const char* key, double v) {
    if (!std::isfinite(v) || v < 0)
        throw std::runtime_error(std::string(key) + " must be finite and >=0");
}

void flag(const char* key, int v) {
    if (v != 0 && v != 1)
        throw std::runtime_error(std::string(key) + " must be 0 or 1");
}

void keys(const cv::FileNode& node, const std::set<std::string>& allowed) {
    std::set<std::string> seen;
    for (auto it = node.begin(); it != node.end(); ++it) {
        const auto key = (*it).name();
        if (!allowed.count(key))
            throw std::runtime_error("Unknown config key: " + key);
        if (!seen.insert(key).second)
            throw std::runtime_error("Duplicate config key: " + key);
    }
}
} // namespace

Config loadConfig(const std::string& path) {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened())
        throw std::runtime_error("Cannot open config: " + path);
    Config c{};
    std::set<std::string> allowed{"cameras"};
#define R(k, f)                                                                                    \
    do {                                                                                           \
        allowed.insert(k);                                                                         \
        read(fs.root(), k, c.f);                                                                   \
    } while (false)
    R("output_root", outputRoot);
    R("analysis_max_width", analysisMaxWidth);
    R("analysis_max_height", analysisMaxHeight);
    R("analysis_allow_upscale", analysisAllowUpscale);
    R("analysis_target_fps", analysisTargetFps);
    R("analysis_sampling_enabled", analysisSamplingEnabled);
    R("analysis_rate_burst_ms", analysisRateBurstMs);
    R("analysis_sparse_l3_enabled", analysisSparseL3Enabled);
    R("opencv_threads", opencvThreads);
    R("main_cpu_list", mainCpuList);
    R("fallback_fps", fallbackFps);
    R("startup_warmup_sec", startupWarmupSec);
    R("event_pre_sec", eventPreSec);
    R("event_post_sec", eventPostSec);
    R("event_min_valid_ratio", eventMinValidRatio);
    R("tawl_temporal_fps_ratio", tawlTemporalFpsRatio);
    R("tawl_width_frame_ratio", tawlWidthFrameRatio);
    R("l1_history_frames", l1HistoryFrames);
    R("l1_var_threshold", l1VarThreshold);
    R("l1_learning_rate", l1LearningRate);
    R("l1_open_kernel_size", l1OpenKernelSize);
    R("l2_history_frames", l2HistoryFrames);
    R("l2_repeat_neighborhood_size", l2RepeatNeighborhoodSize);
    R("l3_location_neighborhood_size", l3LocationNeighborhoodSize);
    R("component_connectivity", componentConnectivity);
    R("record_safety_sec", recordSafetySec);
    R("validation_enabled", validationEnabled);
    R("validation_ring_sec", validationRingSec);
    R("validation_queue_max_messages", validationQueueMaxMessages);
    R("validation_fourcc", validationFourcc);
    R("frame_stats_enabled", frameStatsEnabled);
    R("stats_interval_sec", statsIntervalSec);
    R("perf_stats_enabled", perfStatsEnabled);
    R("l2_capture_enabled", l2CaptureEnabled);
    R("decoder_element", decoderElement);
    R("rtsp_latency_ms", rtspLatencyMs);
    R("rtsp_buffer_mode", rtspBufferMode);
    R("rtsp_drop_on_latency", rtspDropOnLatency);
    R("rtsp_use_tcp", rtspUseTcp);
    R("rtsp_reconnect_enabled", rtspReconnectEnabled);
    R("rtsp_startup_timeout_sec", rtspStartupTimeoutSec);
    R("rtsp_stall_timeout_sec", rtspStallTimeoutSec);
    R("rtsp_shutdown_timeout_sec", rtspShutdownTimeoutSec);
    R("rtsp_retry_initial_sec", rtspRetryInitialSec);
    R("rtsp_retry_max_sec", rtspRetryMaxSec);
    R("rtsp_retry_reset_sec", rtspRetryResetSec);
    R("rtsp_watchdog_poll_ms", rtspWatchdogPollMs);
    R("rtsp_retry_multiplier", rtspRetryMultiplier);
    R("source_poll_timeout_ms", sourcePollTimeoutMs);
    R("offline_sampling_tolerance_ms", offlineSamplingToleranceMs);
    R("gst_queue_max_buffers", gstQueueMaxBuffers);
    R("gst_queue_max_bytes", gstQueueMaxBytes);
    R("gst_queue_max_time_ms", gstQueueMaxTimeMs);
    R("analysis_drop_old_frames", analysisDropOldFrames);
    R("analysis_max_buffers", analysisMaxBuffers);
    R("encoded_max_buffers", encodedMaxBuffers);
    R("record_enabled", recordEnabled);
    R("record_ring_sec", recordRingSec);
    R("writer_queue_max_messages", writerQueueMaxMessages);
    R("writer_appsrc_max_bytes", writerAppsrcMaxBytes);
    R("writer_eos_timeout_sec", writerEosTimeoutSec);
    R("status_interval_sec", statusIntervalSec);
    R("system_sample_interval_sec", systemSampleIntervalSec);
    R("affinity_snapshot_after_sec", affinitySnapshotAfterSec);
    R("launcher_poll_ms", launcherPollMs);
    R("evidence_sample_clip_count", evidenceSampleClipCount);
    R("evidence_sample_max_mib", evidenceSampleMaxMiB);
#undef R
    keys(fs.root(), allowed);
    if (c.outputRoot.empty() || c.decoderElement.empty())
        throw std::runtime_error("Output root and decoder element cannot be empty");
    positive("analysis_max_width", c.analysisMaxWidth);
    positive("analysis_max_height", c.analysisMaxHeight);
    positive("analysis_target_fps", c.analysisTargetFps);
    positive("opencv_threads", c.opencvThreads);
    positive("fallback_fps", c.fallbackFps);
    positive("stats_interval_sec", c.statsIntervalSec);
    nonnegative("analysis_rate_burst_ms", c.analysisRateBurstMs);
    nonnegative("rtsp_latency_ms", c.rtspLatencyMs);
    nonnegative("startup_warmup_sec", c.startupWarmupSec);
    nonnegative("event_pre_sec", c.eventPreSec);
    nonnegative("event_post_sec", c.eventPostSec);
    nonnegative("record_safety_sec", c.recordSafetySec);
    positive("event_min_valid_ratio", c.eventMinValidRatio);
    if (c.eventMinValidRatio > 1)
        throw std::runtime_error("event_min_valid_ratio must be <=1");
    positive("tawl_temporal_fps_ratio", c.tawlTemporalFpsRatio);
    positive("tawl_width_frame_ratio", c.tawlWidthFrameRatio);
    positive("l1_history_frames", c.l1HistoryFrames);
    positive("l1_var_threshold", c.l1VarThreshold);
    if (!std::isfinite(c.l1LearningRate) ||
        (c.l1LearningRate != -1.0 && (c.l1LearningRate < 0 || c.l1LearningRate > 1)))
        throw std::runtime_error("l1_learning_rate must be -1 (automatic) or in [0,1]");
    for (int size : {c.l1OpenKernelSize, c.l2RepeatNeighborhoodSize, c.l3LocationNeighborhoodSize})
        if (size <= 0 || size % 2 == 0)
            throw std::runtime_error("Morphology/neighborhood sizes must be positive odd integers");
    nonnegative("l2_history_frames", c.l2HistoryFrames);
    if (c.componentConnectivity != 4 && c.componentConnectivity != 8)
        throw std::runtime_error("component_connectivity must be 4 or 8");
    if (std::ceil(c.tawlTemporalFpsRatio * c.analysisTargetFps) >
        std::numeric_limits<unsigned short>::max())
        throw std::runtime_error("TAWL temporal threshold exceeds 16-bit age storage");
    if (c.tawlWidthFrameRatio > 1)
        throw std::runtime_error("tawl_width_frame_ratio must be <=1");
    if (c.analysisMaxWidth < 2 || c.analysisMaxHeight < 2 ||
        static_cast<double>(c.analysisMaxWidth) * c.analysisMaxHeight >
            std::numeric_limits<int>::max())
        throw std::runtime_error("Analysis dimensions must be >=2 and fit component pixel counts");
    positive("validation_queue_max_messages", c.validationQueueMaxMessages);
    positive("analysis_max_buffers", c.analysisMaxBuffers);
    positive("encoded_max_buffers", c.encodedMaxBuffers);
    positive("writer_queue_max_messages", c.writerQueueMaxMessages);
    nonnegative("record_ring_sec", c.recordRingSec);
    nonnegative("validation_ring_sec", c.validationRingSec);
    if (c.recordRingSec < c.eventPreSec || c.validationRingSec < c.eventPreSec)
        throw std::runtime_error("Ring duration must cover pre-roll");
    if (c.validationFourcc.size() != 4)
        throw std::runtime_error("validation_fourcc must contain four characters");
    if (c.rtspBufferMode < 0 || c.rtspBufferMode > 4)
        throw std::runtime_error("rtsp_buffer_mode must be in [0,4]");
    if (!c.mainCpuList.empty())
        parseCpuList(c.mainCpuList);
    positive("source_poll_timeout_ms", c.sourcePollTimeoutMs);
    nonnegative("offline_sampling_tolerance_ms", c.offlineSamplingToleranceMs);
    if (c.offlineSamplingToleranceMs >= 1000.0 / c.analysisTargetFps)
        throw std::runtime_error(
            "Offline sampling tolerance must be shorter than one target frame");
    nonnegative("gst_queue_max_buffers", c.gstQueueMaxBuffers);
    nonnegative("gst_queue_max_bytes", c.gstQueueMaxBytes);
    nonnegative("gst_queue_max_time_ms", c.gstQueueMaxTimeMs);
    if (c.gstQueueMaxBuffers == 0 && c.gstQueueMaxBytes == 0 && c.gstQueueMaxTimeMs == 0)
        throw std::runtime_error("At least one GStreamer queue limit must be nonzero");
    positive("writer_appsrc_max_bytes", c.writerAppsrcMaxBytes);
    positive("writer_eos_timeout_sec", c.writerEosTimeoutSec);
    if (c.rtspReconnectEnabled &&
        (c.writerEosTimeoutSec >= c.rtspShutdownTimeoutSec ||
         c.sourcePollTimeoutMs >= 1000 * std::min({c.rtspStartupTimeoutSec, c.rtspStallTimeoutSec,
                                                   c.rtspShutdownTimeoutSec})))
        throw std::runtime_error("Source poll and writer EOS waits must fit watchdog deadlines");
    positive("status_interval_sec", c.statusIntervalSec);
    positive("system_sample_interval_sec", c.systemSampleIntervalSec);
    nonnegative("affinity_snapshot_after_sec", c.affinitySnapshotAfterSec);
    positive("launcher_poll_ms", c.launcherPollMs);
    if (c.launcherPollMs > 1000.0 * std::min(c.statusIntervalSec, c.systemSampleIntervalSec))
        throw std::runtime_error("Launcher poll must fit status/system sample intervals");
    nonnegative("evidence_sample_clip_count", c.evidenceSampleClipCount);
    nonnegative("evidence_sample_max_mib", c.evidenceSampleMaxMiB);
#define F(k, f) flag(k, c.f)
    F("analysis_allow_upscale", analysisAllowUpscale);
    F("analysis_sampling_enabled", analysisSamplingEnabled);
    F("analysis_sparse_l3_enabled", analysisSparseL3Enabled);
    F("validation_enabled", validationEnabled);
    F("frame_stats_enabled", frameStatsEnabled);
    F("perf_stats_enabled", perfStatsEnabled);
    F("l2_capture_enabled", l2CaptureEnabled);
    F("rtsp_drop_on_latency", rtspDropOnLatency);
    F("rtsp_use_tcp", rtspUseTcp);
    F("rtsp_reconnect_enabled", rtspReconnectEnabled);
    F("analysis_drop_old_frames", analysisDropOldFrames);
    F("record_enabled", recordEnabled);
#undef F
    recovery::Options{c.rtspStartupTimeoutSec, c.rtspStallTimeoutSec, c.rtspShutdownTimeoutSec,
                      c.rtspRetryInitialSec,   c.rtspRetryMaxSec,     c.rtspRetryResetSec,
                      c.rtspWatchdogPollMs,    c.rtspRetryMultiplier}
        .validate();
    return c;
}

void writeRuntimeSettings(const Config& c, std::ostream& output) {
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
#define S(k, f) output << k << '\t' << c.f << '\n'
    S("startup_warmup_sec", startupWarmupSec);
    S("status_interval_sec", statusIntervalSec);
    S("system_sample_interval_sec", systemSampleIntervalSec);
    S("affinity_snapshot_after_sec", affinitySnapshotAfterSec);
    S("launcher_poll_ms", launcherPollMs);
    S("evidence_sample_clip_count", evidenceSampleClipCount);
    S("evidence_sample_max_mib", evidenceSampleMaxMiB);
#undef S
}

std::vector<CameraConfig> loadCameras(const std::string& path) {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened())
        throw std::runtime_error("Cannot open camera config");
    const auto list = fs["cameras"];
    if (!list.isSeq() || list.empty())
        throw std::runtime_error("cameras must be a nonempty sequence");
    const std::regex id("[A-Za-z0-9_-]+"), env("[A-Za-z_][A-Za-z0-9_]*");
    std::set<std::string> used;
    std::vector<CameraConfig> out;
    for (const auto& node : list) {
        if (!node.isMap())
            throw std::runtime_error("Each camera must be a mapping");
        keys(node, {"id", "url_env", "codec", "gop_hint_sec"});
        CameraConfig camera;
        read(node, "id", camera.id);
        read(node, "url_env", camera.urlEnv);
        read(node, "codec", camera.codec);
        read(node, "gop_hint_sec", camera.gopHintSec);
        if (!std::regex_match(camera.id, id) || !used.insert(camera.id).second)
            throw std::runtime_error("Invalid or duplicate camera id");
        if (!std::regex_match(camera.urlEnv, env))
            throw std::runtime_error("Invalid URL environment key");
        if (camera.codec != "h264" && camera.codec != "h265")
            throw std::runtime_error("Camera codec must be h264 or h265");
        nonnegative("gop_hint_sec", camera.gopHintSec);
        out.push_back(std::move(camera));
    }
    return out;
}
