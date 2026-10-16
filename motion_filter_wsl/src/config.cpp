#include "config.hpp"
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>

namespace {
template <typename T>
void readRequired(const cv::FileStorage& fs, const char* key, T& out) {
    const cv::FileNode n = fs[key];
    if (n.empty()) throw std::runtime_error(std::string("Missing config key: ") + key);
    n >> out;
}
void flag(const char* n, int v) { if (v != 0 && v != 1) throw std::runtime_error(std::string(n)+" must be 0 or 1"); }
void positive(const char* n, double v) { if (!(v > 0.0)) throw std::runtime_error(std::string(n)+" must be > 0"); }
}

Config loadConfig(const std::string& path) {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) throw std::runtime_error("Cannot open policy: " + path);
    Config c{};
#define R(k,f) readRequired(fs,k,c.f)
    R("output_root", outputRoot);
    R("analysis_max_width", analysisMaxWidth);
    R("analysis_max_height", analysisMaxHeight);
    R("analysis_allow_upscale", analysisAllowUpscale);
    R("analysis_target_fps", analysisTargetFps);
    R("fallback_fps", fallbackFps);
    R("startup_warmup_sec", startupWarmupSec);
    R("event_pre_sec", eventPreSec);
    R("event_post_sec", eventPostSec);
    R("event_min_valid_ratio", eventMinValidRatio);
    R("tawl_temporal_fps_ratio", tawlTemporalFpsRatio);
    R("tawl_width_frame_ratio", tawlWidthFrameRatio);
    R("offline_record_enabled", offlineRecordEnabled);
    R("offline_fourcc", offlineFourcc);
    R("offline_writer_queue_max_messages", offlineWriterQueueMaxMessages);
    R("record_safety_sec", recordSafetySec);
    R("full_mask_enabled", fullMaskEnabled);
    R("full_mask_fourcc", fullMaskFourcc);
    R("validation_enabled", validationEnabled);
    R("validation_ring_sec", validationRingSec);
    R("validation_queue_max_messages", validationQueueMaxMessages);
    R("validation_fourcc", validationFourcc);
    R("frame_stats_enabled", frameStatsEnabled);
    R("stats_interval_sec", statsIntervalSec);
    R("decoder_element", decoderElement);
    R("rtsp_latency_ms", rtspLatencyMs);
    R("rtsp_use_tcp", rtspUseTcp);
    R("analysis_drop_old_frames", analysisDropOldFrames);
    R("analysis_max_buffers", analysisMaxBuffers);
    R("encoded_max_buffers", encodedMaxBuffers);
    R("record_enabled", recordEnabled);
    R("record_ring_sec", recordRingSec);
    R("writer_queue_max_messages", writerQueueMaxMessages);
#undef R
    if (c.outputRoot.empty()) throw std::runtime_error("output_root cannot be empty");
    if (c.analysisMaxWidth <= 0 || c.analysisMaxHeight <= 0) throw std::runtime_error("analysis dimensions must be > 0");
    positive("analysis_target_fps", c.analysisTargetFps);
    positive("fallback_fps", c.fallbackFps);
    if (c.startupWarmupSec < 0.0 || c.eventPreSec < 0.0 || c.eventPostSec < 0.0) throw std::runtime_error("time policies must be >= 0");
    if (!(c.eventMinValidRatio > 0.0 && c.eventMinValidRatio <= 1.0)) throw std::runtime_error("event_min_valid_ratio must be in (0, 1]");
    positive("tawl_temporal_fps_ratio", c.tawlTemporalFpsRatio);
    positive("tawl_width_frame_ratio", c.tawlWidthFrameRatio);
    if (c.offlineFourcc.size()!=4 || c.fullMaskFourcc.size()!=4 || c.validationFourcc.size()!=4) throw std::runtime_error("fourcc must have 4 characters");
    if (c.offlineWriterQueueMaxMessages<=0 || c.validationQueueMaxMessages<=0 || c.analysisMaxBuffers<=0 || c.encodedMaxBuffers<=0 || c.writerQueueMaxMessages<=0) throw std::runtime_error("queue/buffer sizes must be > 0");
    if (c.validationRingSec < c.eventPreSec || c.recordRingSec < c.eventPreSec) throw std::runtime_error("ring_sec must be >= event_pre_sec");
    flag("analysis_allow_upscale",c.analysisAllowUpscale); flag("offline_record_enabled",c.offlineRecordEnabled);
    flag("full_mask_enabled",c.fullMaskEnabled); flag("validation_enabled",c.validationEnabled); flag("frame_stats_enabled",c.frameStatsEnabled);
    flag("rtsp_use_tcp",c.rtspUseTcp); flag("analysis_drop_old_frames",c.analysisDropOldFrames); flag("record_enabled",c.recordEnabled);
    return c;
}
