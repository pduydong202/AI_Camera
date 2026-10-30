#include "gst_mpp_source.hpp"
#include "analysis_size.hpp"
#include "log_line.hpp"
#include "motion_timing.hpp"
#include "redact.hpp"
#include "rtsp_tail_guard.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <rga/RgaUtils.h>
#include <rga/im2d.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>

namespace {

using SourceClock = std::chrono::steady_clock;

double elapsedMs(SourceClock::time_point start) {
    return std::chrono::duration<double, std::milli>(SourceClock::now() - start).count();
}

std::int64_t monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               SourceClock::now().time_since_epoch())
        .count();
}

struct DecodeStamp {
    std::atomic<const GstMppSource*> owner{nullptr};
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::int64_t> arrivalNs{0};
    std::atomic<std::uint64_t> version{0};
};

GQuark decodeStampKey() {
    static const GQuark key = g_quark_from_static_string("motion-filter-decoded-timing");
    return key;
}

std::string quoteGst(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '\\' || ch == '\"')
            out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('\"');
    return out;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::int64_t toNs(GstClockTime t) {
    return GST_CLOCK_TIME_IS_VALID(t) ? static_cast<std::int64_t>(t) : -1;
}

int align16(int v) {
    return (v + 15) & ~15;
}

} // namespace

GstMppSource::GstMppSource(const std::string& input, const Config& cfg, const std::string& codec)
    : input_(input), cfg_(cfg), codec_(codec), isRtsp_(startsWith(input, "rtsp://")),
      liveBudget_(cfg.analysisTargetFps, cfg.analysisMaxBuffers,
                  isRtsp_ ? cfg.analysisRateBurstMs : 0) {
    if (codec_ != "h264" && codec_ != "h265")
        throw std::runtime_error("codec must be h264 or h265");
    pipelineText_ = buildPipeline();

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipelineText_.c_str(), &error);
    if (pipeline == nullptr || error != nullptr) {
        const std::string message = error ? error->message : "unknown GStreamer parse error";
        if (error)
            g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        throw std::runtime_error(redactUris("Cannot create GStreamer pipeline: " + message +
                                            "\nPipeline: " + pipelineText_));
    }

    GstElement* analysis = gst_bin_get_by_name(GST_BIN(pipeline), "analysis_sink");
    if (analysis == nullptr) {
        gst_object_unref(pipeline);
        throw std::runtime_error("analysis_sink not found in GStreamer pipeline");
    }

    GstElement* encoded = nullptr;
    if (cfg_.recordEnabled != 0) {
        encoded = gst_bin_get_by_name(GST_BIN(pipeline), "encoded_sink");
        if (encoded == nullptr) {
            gst_object_unref(analysis);
            gst_object_unref(pipeline);
            throw std::runtime_error("encoded_sink not found while record_enabled=1");
        }
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    pipeline_ = pipeline;
    analysisSink_ = analysis;
    encodedSink_ = encoded;
    bus_ = bus;
    videoInfo_ = gst_video_info_new();
    if (isRtsp_) {
        installInputTimeline();
        auto* depay = gst_bin_get_by_name(GST_BIN(pipeline_), "input_depay");
        if (!depay) throw std::runtime_error("RTSP depayloader unavailable");
        try { rtspTailGuard_ = std::make_unique<RtspTailGuard>(depay); }
        catch (...) { gst_object_unref(depay); throw; }
        gst_object_unref(depay);
    }
    if (cfg_.perfStatsEnabled != 0) {
        GstPad* pad = gst_element_get_static_pad(analysis, "sink");
        analysisProbePad_ = pad;
        if (pad != nullptr) {
            analysisProbeId_ = gst_pad_add_probe(
                pad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
                    auto* self = static_cast<GstMppSource*>(data);
                    const auto sequence =
                        self->decodedFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
                    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                    if (buffer != nullptr) {
                        auto* stamp = static_cast<DecodeStamp*>(
                            gst_mini_object_get_qdata(GST_MINI_OBJECT(buffer), decodeStampKey()));
                        if (stamp == nullptr) {
                            stamp = new (std::nothrow) DecodeStamp;
                            if (stamp != nullptr) {
                                gst_mini_object_set_qdata(
                                    GST_MINI_OBJECT(buffer), decodeStampKey(), stamp,
                                    [](gpointer p) { delete static_cast<DecodeStamp*>(p); });
                            }
                        }
                        if (stamp != nullptr) {
                            // No reader/writer data race if a pool recycles the
                            // object. Readers reject an inconsistent snapshot.
                            stamp->version.fetch_add(1);
                            stamp->owner.store(self);
                            stamp->sequence.store(sequence);
                            stamp->arrivalNs.store(monotonicNs());
                            stamp->version.fetch_add(1);
                        }
                    }
                    return GST_PAD_PROBE_OK;
                },
                this, nullptr);
        }
        if (analysisProbeId_ == 0) {
            LogLine(std::cerr) << "[PERF WARNING] decoder output counter unavailable\n";
        }
    }
}

GstMppSource::~GstMppSource() {
    stop();
    rtspTailGuard_.reset();

    if (inputTimelinePad_) {
        if (inputTimelineProbeId_)
            gst_pad_remove_probe(static_cast<GstPad*>(inputTimelinePad_), inputTimelineProbeId_);
        gst_object_unref(static_cast<GstPad*>(inputTimelinePad_));
    }

    if (analysisProbePad_ != nullptr) {
        if (analysisProbeId_ != 0) {
            gst_pad_remove_probe(static_cast<GstPad*>(analysisProbePad_), analysisProbeId_);
        }
        gst_object_unref(static_cast<GstPad*>(analysisProbePad_));
        analysisProbePad_ = nullptr;
        analysisProbeId_ = 0;
    }

    if (videoInfo_ != nullptr) {
        gst_video_info_free(static_cast<GstVideoInfo*>(videoInfo_));
        videoInfo_ = nullptr;
    }
    if (bus_ != nullptr) {
        gst_object_unref(static_cast<GstBus*>(bus_));
        bus_ = nullptr;
    }
    if (encodedSink_ != nullptr) {
        gst_object_unref(static_cast<GstElement*>(encodedSink_));
        encodedSink_ = nullptr;
    }
    if (analysisSink_ != nullptr) {
        gst_object_unref(static_cast<GstElement*>(analysisSink_));
        analysisSink_ = nullptr;
    }
    if (pipeline_ != nullptr) {
        gst_object_unref(static_cast<GstElement*>(pipeline_));
        pipeline_ = nullptr;
    }
}

std::string GstMppSource::buildPipeline() const {
    const bool h265 = codec_ == "h265";
    const std::string parser = h265 ? "h265parse" : "h264parse";
    const std::string depay = h265 ? "rtph265depay" : "rtph264depay";
    const std::string mediaCaps = h265 ? "video/x-h265,stream-format=byte-stream,alignment=au"
                                       : "video/x-h264,stream-format=byte-stream,alignment=au";

    std::ostringstream ss;
    if (isRtsp_) {
        ss << "rtspsrc location=" << quoteGst(input_) << " latency=" << cfg_.rtspLatencyMs
           << " buffer-mode=" << cfg_.rtspBufferMode
           << " drop-on-latency=" << (cfg_.rtspDropOnLatency != 0 ? "true" : "false") << " ";
        if (cfg_.rtspUseTcp != 0)
            ss << "protocols=tcp ";
        ss << "! " << depay << " name=input_depay ! " << mediaCaps << " ";
    } else {
        ss << "filesrc location=" << quoteGst(input_) << " ! qtdemux ";
    }

    ss << "! " << parser << " name=input_parser config-interval=-1 " << "! " << mediaCaps << " ";

    const std::string queue =
        "! queue max-size-buffers=" + std::to_string(cfg_.gstQueueMaxBuffers) +
        " max-size-bytes=" + std::to_string(cfg_.gstQueueMaxBytes) + " max-size-time=" +
        std::to_string(static_cast<std::uint64_t>(cfg_.gstQueueMaxTimeMs) * GST_MSECOND) + " ";
    const std::string analysisSink =
        queue + "! " + cfg_.decoderElement +
        " "
        "! video/x-raw,format=NV12 "
        "! appsink name=analysis_sink emit-signals=false sync=false wait-on-eos=false "
        "max-buffers=" +
        std::to_string(cfg_.analysisMaxBuffers) +
        " drop=" + std::string(cfg_.analysisDropOldFrames != 0 ? "true" : "false");

    if (cfg_.recordEnabled != 0) {
        ss << "! tee name=t " << "t. " << analysisSink << " " << "t. " << queue
           << "! appsink name=encoded_sink emit-signals=false sync=false wait-on-eos=false "
              "max-buffers="
           << cfg_.encodedMaxBuffers << " drop=false";
    } else {
        ss << analysisSink;
    }

    return ss.str();
}

void GstMppSource::installInputTimeline() {
    auto* parser = gst_bin_get_by_name(GST_BIN(pipeline_), "input_parser");
    if (!parser) throw std::runtime_error("Missing input timeline parser");
    auto* pad = gst_element_get_static_pad(parser, "src");
    gst_object_unref(parser);
    if (!pad) throw std::runtime_error("Missing input timeline pad");
    inputTimelinePad_ = pad;
    inputTimelineProbeId_ = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad*, GstPadProbeInfo* info, gpointer user) -> GstPadProbeReturn {
            auto* self = static_cast<GstMppSource*>(user);
            if (self->hasError_.load()) return GST_PAD_PROBE_DROP;
            auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            if (!buffer) return GST_PAD_PROBE_OK;
            try {
                if (!self->startupGate_.opened()) {
                    GstMapInfo map{};
                    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
                        throw std::runtime_error("Cannot inspect RTSP startup access unit");
                    const bool accept = self->startupGate_.accept(map.data, map.size,
                                                                  self->codec_ == "h265");
                    gst_buffer_unmap(buffer, &map);
                    if (!accept) return GST_PAD_PROBE_DROP;
                    LogLine(std::cout) << "[SOURCE_START] dropped_before_keyframe="
                        << self->startupGate_.dropped()
                        << " first_pts_ns=" << toNs(GST_BUFFER_PTS(buffer))
                        << " shared_before_tee=1\n";
                }
                const auto raw = toNs(GST_BUFFER_PTS(buffer));
                const auto used = self->sourceTimeline_.normalize(raw, monotonicNs(),
                    static_cast<std::int64_t>(self->cfg_.rtspStallTimeoutSec * GST_SECOND));
                if (used != raw) {
                    const auto rawDts = toNs(GST_BUFFER_DTS(buffer));
                    buffer = gst_buffer_make_writable(buffer);
                    GST_PAD_PROBE_INFO_DATA(info) = buffer;
                    if (!buffer) throw std::runtime_error("Cannot repair input timestamp");
                    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(used);
                    if (rawDts >= 0 && raw >= 0)
                        GST_BUFFER_DTS(buffer) = static_cast<GstClockTime>(rawDts + (used - raw));
                    LogLine(std::cout) << "[SOURCE_TIME] raw_pts_ns=" << raw
                        << " used_pts_ns=" << used << " raw_dts_ns=" << rawDts
                        << " repair=" << (raw < 0 ? "missing" : "nonincreasing_or_subtick")
                        << " shared_before_tee=1\n";
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(self->errorMutex_);
                self->errorMessage_ = e.what();
                self->hasError_.store(true);
                return GST_PAD_PROBE_DROP;
            }
            return GST_PAD_PROBE_OK;
        }, this, nullptr);
    if (!inputTimelineProbeId_) throw std::runtime_error("Cannot install input timeline probe");
}

void GstMppSource::start() {
    if (started_.exchange(true))
        return;
    stopRequested_.store(false);
    eos_.store(false);
    hasError_.store(false);
    liveBudget_.reset();
    lastPulledSequence_ = 0;

    const GstStateChangeReturn ret =
        gst_element_set_state(static_cast<GstElement*>(pipeline_), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        throw std::runtime_error("Failed to set source pipeline to PLAYING");
    }

    busThread_ = std::thread(&GstMppSource::busLoop, this);
}

void GstMppSource::stop() {
    if (!started_.exchange(false))
        return;
    requestStop();
    if (busThread_.joinable())
        busThread_.join();
    gst_element_set_state(static_cast<GstElement*>(pipeline_), GST_STATE_NULL);
    if (rtspTailGuard_ && rtspTailGuard_->eosCount())
        LogLine(std::cout) << "[SOURCE_EOS] bypass_depay_drain=1 complete_aus_only=1\n";
}

std::string GstMppSource::errorMessage() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return redactUris(errorMessage_);
}

void GstMppSource::busLoop() {
    while (!stopping()) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            static_cast<GstBus*>(bus_),
            static_cast<GstClockTime>(cfg_.sourcePollTimeoutMs) * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg == nullptr)
            continue;

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                errorMessage_ = err ? err->message : "unknown GStreamer error";
                if (debug != nullptr)
                    errorMessage_ += std::string(" | ") + debug;
            }
            if (err)
                g_error_free(err);
            if (debug)
                g_free(debug);
            hasError_.store(true);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            eos_.store(true);
        }

        gst_message_unref(msg);
        if (hasError_.load() || eos_.load())
            break;
    }
}

PtsDiagnosticsWindow GstMppSource::takeEncodedPtsWindow() {
    if (cfg_.perfStatsEnabled == 0)
        return {};
    std::lock_guard<std::mutex> lock(encodedPtsMutex_);
    auto window = encodedPtsWindow_;
    encodedPtsWindow_ = PtsDiagnosticsWindow{};
    return window;
}

std::int64_t GstMppSource::normalizeAnalysisPts(std::int64_t pts, std::int64_t duration,
                                                double fps) {
    const std::int64_t fallbackDuration =
        static_cast<std::int64_t>(GST_SECOND / std::max(1.0, fps));
    const std::int64_t step = duration > 0 ? duration : fallbackDuration;
    if (pts < 0 || (lastAnalysisPtsNs_ >= 0 && pts <= lastAnalysisPtsNs_))
        pts = lastAnalysisPtsNs_ >= 0 ? lastAnalysisPtsNs_ + step : 0;
    lastAnalysisPtsNs_ = pts;
    return pts;
}

std::int64_t GstMppSource::normalizeEncodedPts(std::int64_t pts, std::int64_t duration) {
    const std::int64_t fallbackDuration =
        static_cast<std::int64_t>(GST_SECOND / std::max(1.0, cfg_.fallbackFps));
    const std::int64_t step = duration > 0 ? duration : fallbackDuration;
    if (pts < 0 || (lastEncodedPtsNs_ >= 0 && pts <= lastEncodedPtsNs_))
        pts = lastEncodedPtsNs_ >= 0 ? lastEncodedPtsNs_ + step : 0;
    lastEncodedPtsNs_ = pts;
    return pts;
}

bool GstMppSource::pullGrayFrame(GrayFrame& out) {
    GstAppSink* sink = GST_APP_SINK(analysisSink_);
    const bool measure = cfg_.perfStatsEnabled != 0;
    const auto pullStart = measure ? SourceClock::now() : SourceClock::time_point{};
    out.timing = FrameTiming{};

    // imconfig() applies to the current thread. The analysis pull runs on the
    // main analysis thread, so configure it here rather than in the encoded
    // packet thread. Restrict jobs to the two RK3588 RGA3 cores and exclude
    // RGA2, whose MMU path can reject >4 GiB virtual addresses.
    if (!rgaSchedulerConfigured_) {
        const std::uint64_t rga3Mask = static_cast<std::uint64_t>(IM_SCHEDULER_RGA3_CORE0) |
                                       static_cast<std::uint64_t>(IM_SCHEDULER_RGA3_CORE1);
        const IM_STATUS st = imconfig(IM_CONFIG_SCHEDULER_CORE, rga3Mask);
        if (st != IM_STATUS_SUCCESS) {
            throw std::runtime_error(std::string("Cannot configure RGA3 scheduler: ") +
                                     imStrError(st));
        }
        rgaSchedulerConfigured_ = true;
    }

    while (!stopping()) {
        const auto waitStart = measure ? SourceClock::now() : SourceClock::time_point{};
        GstSample* sample = gst_app_sink_try_pull_sample(
            sink, static_cast<GstClockTime>(cfg_.sourcePollTimeoutMs) * GST_MSECOND);
        if (measure)
            out.timing.waitMs += elapsedMs(waitStart);
        const std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sampleOwner(sample,
                                                                                  gst_sample_unref);
        if (sample == nullptr) {
            if (hasError_.load())
                return false;
            if (gst_app_sink_is_eos(sink))
                eos_.store(true);
            if (eos_.load())
                return false;
            continue;
        }

        GstCaps* caps = gst_sample_get_caps(sample);
        if (measure)
            ++out.timing.pulled;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (measure) {
            out.timing.decodedArrivalNs = -1;
            out.timing.queueAgeMs = -1.0;
            const auto* stamp = buffer != nullptr
                                    ? static_cast<DecodeStamp*>(gst_mini_object_get_qdata(
                                          GST_MINI_OBJECT(buffer), decodeStampKey()))
                                    : nullptr;
            bool validStamp = false;
            if (stamp != nullptr) {
                const auto before = stamp->version.load();
                const auto owner = stamp->owner.load();
                const auto sequence = stamp->sequence.load();
                const auto arrival = stamp->arrivalNs.load();
                const auto after = stamp->version.load();
                const auto pulledAt = monotonicNs();
                validStamp = before == after && (before & 1U) == 0 && owner == this &&
                             sequence > 0 &&
                             (lastPulledSequence_ == 0 || sequence > lastPulledSequence_) &&
                             arrival <= pulledAt;
                if (validStamp) {
                    if (lastPulledSequence_ > 0) {
                        out.timing.inputGaps += sequence - lastPulledSequence_ - 1;
                    }
                    lastPulledSequence_ = sequence;
                    out.timing.decodedArrivalNs = arrival;
                    out.timing.queueAgeMs = static_cast<double>(pulledAt - arrival) / 1e6;
                }
            }
            if (!validStamp) {
                ++out.timing.metadataMissing;
                lastPulledSequence_ = 0;
            }
        }
        if (caps == nullptr || buffer == nullptr) {
            continue;
        }

        GstVideoInfo* info = static_cast<GstVideoInfo*>(videoInfo_);
        if (cachedWidth_ == 0) {
            if (!gst_video_info_from_caps(info, caps)) {
                throw std::runtime_error("Cannot parse NV12 video caps");
            }
            cachedWidth_ = static_cast<int>(GST_VIDEO_INFO_WIDTH(info));
            cachedHeight_ = static_cast<int>(GST_VIDEO_INFO_HEIGHT(info));
            const int fpsN = GST_VIDEO_INFO_FPS_N(info);
            const int fpsD = GST_VIDEO_INFO_FPS_D(info);
            cachedFps_ = (fpsN > 0 && fpsD > 0)
                             ? static_cast<double>(fpsN) / static_cast<double>(fpsD)
                             : cfg_.fallbackFps;
            if (cfg_.perfStatsEnabled != 0) {
                LogLine(std::cout)
                    << "[SOURCE] caps_fps=" << ((fpsN > 0 && fpsD > 0) ? cachedFps_ : 0.0)
                    << " fps_used=" << cachedFps_
                    << " fps_origin=" << ((fpsN > 0 && fpsD > 0) ? "caps" : "fallback") << '\n';
            }

            cachedAnalysisSize_ = chooseAnalysisSize(cachedWidth_, cachedHeight_, cfg_);
            rgaOutputStride_ = align16(cachedAnalysisSize_.width);
            rgaOutputHeightStride_ = cachedAnalysisSize_.height;
            const std::size_t bytes = static_cast<std::size_t>(rgaOutputStride_) *
                                      static_cast<std::size_t>(rgaOutputHeightStride_) * 3U / 2U;
            rgaOutputBuffer_.assign(bytes, 0x80);
        }

        // Preserve every encoded packet for recording, but only map/RGA-resize
        // frames selected by the shared analysis_target_fps policy. This keeps
        // analysis_target_fps in YAML and avoids spending map/RGA work on frames
        // that the motion core will not use.
        const std::int64_t rawDurationNs = toNs(GST_BUFFER_DURATION(buffer));
        std::int64_t durationNs = rawDurationNs;
        if (durationNs <= 0)
            durationNs = static_cast<std::int64_t>(GST_SECOND / std::max(1.0, cachedFps_));
        const std::int64_t rawPtsNs = toNs(GST_BUFFER_PTS(buffer));
        const std::int64_t previousUsedPtsNs = lastAnalysisPtsNs_;
        const std::int64_t ptsNs = normalizeAnalysisPts(rawPtsNs, durationNs, cachedFps_);
        if (measure && rawPtsNs != ptsNs)
            ++out.timing.ptsRepaired;
        if (measure) {
            out.timing.pts.add(analysisPtsTracker_.observe(rawPtsNs, ptsNs, previousUsedPtsNs,
                                                           rawDurationNs, durationNs,
                                                           toNs(GST_BUFFER_DTS(buffer))));
        }
        if (cfg_.analysisSamplingEnabled != 0) {
            if (isRtsp_) {
                // Bound live CPU work by wall time with configurable burst
                // credit independent of the stored queue. This limiter does not
                // rewrite timestamps; existing normalization stays in place.
                if (!liveBudget_.allow(monotonicNs())) {
                    if (measure)
                        ++out.timing.skipped;
                    continue;
                }
            } else {
                // Offline input is sampled in media time, as in the previous build.
                const std::int64_t targetIntervalNs =
                    static_cast<std::int64_t>(GST_SECOND / std::max(0.1, cfg_.analysisTargetFps));
                const auto scheduleToleranceNs =
                    static_cast<std::int64_t>(cfg_.offlineSamplingToleranceMs * GST_MSECOND);
                if (nextAnalysisDuePtsNs_ < 0)
                    nextAnalysisDuePtsNs_ = ptsNs;
                if (ptsNs + scheduleToleranceNs < nextAnalysisDuePtsNs_) {
                    if (measure)
                        ++out.timing.skipped;
                    continue;
                }
                do {
                    nextAnalysisDuePtsNs_ += targetIntervalNs;
                } while (nextAnalysisDuePtsNs_ <= ptsNs + scheduleToleranceNs);
            }
        }

        GstVideoFrame videoFrame{};
        GstMapInfo rawMap{};
        bool mappedVideoFrame = false;
        bool mappedRawBuffer = false;
        std::uint8_t* yData = nullptr;
        std::uint8_t* uvData = nullptr;
        int yStride = 0;
        int uvStride = 0;

        const auto mapStart = measure ? SourceClock::now() : SourceClock::time_point{};
        if (gst_video_frame_map(&videoFrame, info, buffer, GST_MAP_READ)) {
            mappedVideoFrame = true;
            yData = static_cast<std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&videoFrame, 0));
            uvData = static_cast<std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&videoFrame, 1));
            yStride = GST_VIDEO_FRAME_PLANE_STRIDE(&videoFrame, 0);
            uvStride = GST_VIDEO_FRAME_PLANE_STRIDE(&videoFrame, 1);
        } else if (gst_buffer_map(buffer, &rawMap, GST_MAP_READ)) {
            mappedRawBuffer = true;
            const gsize yOffset = GST_VIDEO_INFO_PLANE_OFFSET(info, 0);
            const gsize uvOffset = GST_VIDEO_INFO_PLANE_OFFSET(info, 1);
            yStride = GST_VIDEO_INFO_PLANE_STRIDE(info, 0);
            uvStride = GST_VIDEO_INFO_PLANE_STRIDE(info, 1);
            const gsize required = uvOffset + static_cast<gsize>(uvStride) *
                                                  static_cast<gsize>((cachedHeight_ + 1) / 2);
            if (rawMap.size < required) {
                gst_buffer_unmap(buffer, &rawMap);
                throw std::runtime_error(
                    "Mapped NV12 buffer is smaller than the planes described by caps");
            }
            yData = rawMap.data + yOffset;
            uvData = rawMap.data + uvOffset;
        } else {
            throw std::runtime_error("Cannot map NV12 frame from mppvideodec. MPP output is not "
                                     "CPU-mappable on this image.");
        }
        if (measure)
            out.timing.mapMs += elapsedMs(mapStart);

        auto releaseMapped = [&]() {
            const auto unmapStart = measure ? SourceClock::now() : SourceClock::time_point{};
            if (mappedVideoFrame)
                gst_video_frame_unmap(&videoFrame);
            if (mappedRawBuffer)
                gst_buffer_unmap(buffer, &rawMap);
            if (measure)
                out.timing.mapMs += elapsedMs(unmapStart);
        };

        // wrapbuffer_virtualaddr describes one contiguous NV12 allocation. MPP
        // may use a vertically padded Y plane, so derive hstride from the real
        // UV offset instead of assuming hstride == visible height.
        if (yData == nullptr || uvData == nullptr || yStride < cachedWidth_ ||
            uvStride < cachedWidth_ || yStride != uvStride || uvData <= yData) {
            releaseMapped();
            throw std::runtime_error("Unsupported MPP NV12 plane layout for RGA");
        }

        const std::ptrdiff_t uvOffset = uvData - yData;
        if (uvOffset % yStride != 0) {
            releaseMapped();
            throw std::runtime_error("MPP NV12 UV offset is not aligned to the Y stride");
        }
        const int sourceHeightStride = static_cast<int>(uvOffset / yStride);
        if (sourceHeightStride < cachedHeight_) {
            releaseMapped();
            throw std::runtime_error(
                "MPP NV12 source height stride is smaller than visible height");
        }

        rga_buffer_t src =
            wrapbuffer_virtualaddr(yData, cachedWidth_, cachedHeight_, RK_FORMAT_YCbCr_420_SP,
                                   yStride, sourceHeightStride);
        rga_buffer_t dst = wrapbuffer_virtualaddr(
            rgaOutputBuffer_.data(), cachedAnalysisSize_.width, cachedAnalysisSize_.height,
            RK_FORMAT_YCbCr_420_SP, rgaOutputStride_, rgaOutputHeightStride_);

        if (!rgaGeometryChecked_) {
            im_rect srcRect{};
            im_rect dstRect{};
            // Pass usage explicitly: older librga variadic macros otherwise
            // form an empty __args array, rejected in strict C++17 mode.
            const IM_STATUS check = imcheck(src, dst, srcRect, dstRect, 0);
            if (check != IM_STATUS_NOERROR) {
                releaseMapped();
                throw std::runtime_error(std::string("RGA geometry check failed: ") +
                                         imStrError(check));
            }
            rgaGeometryChecked_ = true;
        }

        const double rgaCpuStart = measure ? timingThreadCpuMs() : 0.0;
        const auto rgaStart = measure ? SourceClock::now() : SourceClock::time_point{};
        const IM_STATUS resizeStatus = imresize(src, dst);
        if (measure) {
            out.timing.rgaMs += elapsedMs(rgaStart);
            out.timing.rgaCpuMs += timingThreadCpuMs() - rgaCpuStart;
        }
        if (resizeStatus != IM_STATUS_SUCCESS) {
            releaseMapped();
            throw std::runtime_error(std::string("RGA resize failed: ") + imStrError(resizeStatus));
        }

        releaseMapped();

        // Expose only the resized NV12 Y plane. The Mat is a lightweight view
        // over rgaOutputBuffer_; it remains valid until the next pullGrayFrame()
        // call on this source, which is after the motion core finishes this frame.
        out.image = cv::Mat(cachedAnalysisSize_.height, cachedAnalysisSize_.width, CV_8UC1,
                            rgaOutputBuffer_.data(), static_cast<std::size_t>(rgaOutputStride_));
        out.durationNs = durationNs;
        out.ptsNs = ptsNs;
        out.sourceWidth = cachedWidth_;
        out.sourceHeight = cachedHeight_;
        out.fps = cachedFps_;

        if (measure)
            out.timing.pullMs = elapsedMs(pullStart);
        return true;
    }

    return false;
}

bool GstMppSource::pullEncodedPacket(std::shared_ptr<EncodedPacket>& out) {
    if (cfg_.recordEnabled == 0 || encodedSink_ == nullptr)
        return false;

    GstAppSink* sink = GST_APP_SINK(encodedSink_);
    while (!stopping()) {
        GstSample* sample = gst_app_sink_try_pull_sample(
            sink, static_cast<GstClockTime>(cfg_.sourcePollTimeoutMs) * GST_MSECOND);
        const std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sampleOwner(sample,
                                                                                  gst_sample_unref);
        if (sample == nullptr) {
            if (hasError_.load())
                return false;
            if (gst_app_sink_is_eos(sink))
                eos_.store(true);
            if (eos_.load())
                return false;
            continue;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer == nullptr) {
            continue;
        }

        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            throw std::runtime_error("Cannot map encoded H264/H265 packet");
        }

        struct MappedBuffer {
            GstBuffer* buffer;
            GstMapInfo* info;

            ~MappedBuffer() {
                gst_buffer_unmap(buffer, info);
            }
        } mapped{buffer, &map};

        auto packet = std::make_shared<EncodedPacket>();
        packet->data.assign(map.data, map.data + map.size);
        const std::int64_t rawDurationNs = toNs(GST_BUFFER_DURATION(buffer));
        std::int64_t durationNs = rawDurationNs;
        if (durationNs <= 0)
            durationNs = static_cast<std::int64_t>(GST_SECOND / std::max(1.0, cfg_.fallbackFps));
        packet->durationNs = durationNs;
        const std::int64_t rawPtsNs = toNs(GST_BUFFER_PTS(buffer));
        const std::int64_t rawDtsNs = toNs(GST_BUFFER_DTS(buffer));
        const std::int64_t previousUsedPtsNs = lastEncodedPtsNs_;
        packet->ptsNs = normalizeEncodedPts(rawPtsNs, durationNs);
        packet->dtsNs = rawDtsNs;
        if (packet->dtsNs < 0)
            packet->dtsNs = packet->ptsNs;
        if (cfg_.perfStatsEnabled != 0) {
            const auto observation = encodedPtsTracker_.observe(
                rawPtsNs, packet->ptsNs, previousUsedPtsNs, rawDurationNs, durationNs, rawDtsNs);
            std::lock_guard<std::mutex> lock(encodedPtsMutex_);
            encodedPtsWindow_.add(observation);
        }
        packet->keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        out = std::move(packet);
        return true;
    }

    return false;
}
