#include "recorder.hpp"
#include "log_line.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace {

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

std::string eventFilename(const std::string& root, std::uint64_t id) {
    std::ostringstream ss;
    ss << "event_" << std::setw(6) << std::setfill('0') << id << ".mp4";
    return (fs::path(root) / "clips" / ss.str()).string();
}

GstClockTime rebaseTime(std::int64_t value, std::int64_t base) {
    if (value < 0 || base < 0)
        return GST_CLOCK_TIME_NONE;
    return static_cast<GstClockTime>(std::max<std::int64_t>(0, value - base));
}

} // namespace

AsyncClipWriter::AsyncClipWriter(const Config& cfg)
    : cfg_(cfg), queue_(static_cast<std::size_t>(std::max(1, cfg.writerQueueMaxMessages))) {}

AsyncClipWriter::~AsyncClipWriter() {
    close();
}

void AsyncClipWriter::start() {
    if (started_)
        return;
    fs::create_directories(fs::path(cfg_.outputRoot) / "clips");
    thread_ = std::thread(&AsyncClipWriter::run, this);
    started_ = true;
}

void AsyncClipWriter::close() {
    if (!started_)
        return;
    queue_.close();
    if (thread_.joinable())
        thread_.join();
    started_ = false;
}

bool AsyncClipWriter::beginClip(std::uint64_t eventId, const std::string& codec,
                                const std::vector<std::shared_ptr<EncodedPacket>>& preRoll) {
    Message m;
    m.type = MessageType::Start;
    m.eventId = eventId;
    m.codec = codec;
    m.preRoll = preRoll;
    return queue_.push(std::move(m));
}

bool AsyncClipWriter::pushPacket(std::uint64_t eventId,
                                 const std::shared_ptr<EncodedPacket>& packet) {
    Message m;
    m.type = MessageType::Packet;
    m.eventId = eventId;
    m.packet = packet;
    return queue_.push(std::move(m));
}

bool AsyncClipWriter::endClip(std::uint64_t eventId) {
    Message m;
    m.type = MessageType::Stop;
    m.eventId = eventId;
    return queue_.push(std::move(m));
}

std::string AsyncClipWriter::errorMessage() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
}

void AsyncClipWriter::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        if (!failed_.load())
            error_ = message;
        failed_.store(true);
    }
    queue_.close(); // Wake blocked producers instead of silently losing clips.
}

void AsyncClipWriter::run() {
    try {
        Message m;
        while (queue_.pop(m)) {
            switch (m.type) {
            case MessageType::Start:
                if (pipeline_ != nullptr)
                    closeSession();
                if (hasError())
                    break;
                if (openSession(m)) {
                    for (const auto& p : m.preRoll) {
                        if (!writePacket(p)) {
                            fail("Cannot write pre-roll packet");
                            break;
                        }
                    }
                } else if (!m.preRoll.empty())
                    fail("Cannot open encoded clip writer");
                break;

            case MessageType::Packet:
                if (pipeline_ != nullptr && m.eventId == activeEventId_) {
                    if (!writePacket(m.packet))
                        fail("Cannot write encoded packet");
                }
                break;

            case MessageType::Stop:
                if (pipeline_ != nullptr && m.eventId == activeEventId_) {
                    closeSession();
                }
                break;
            }
            if (hasError())
                break;
        }
    } catch (const std::exception& e) {
        fail(e.what());
    } catch (...) {
        fail("Unknown encoded writer exception");
    }
    try {
        if (pipeline_ != nullptr)
            closeSession();
    } catch (const std::exception& e) {
        fail(e.what());
    }
}

bool AsyncClipWriter::openSession(const Message& msg) {
    if (msg.preRoll.empty()) {
        LogLine(std::cerr) << "[REC] event=" << msg.eventId
                           << " has no keyframe-backed pre-roll; clip skipped\n";
        return false;
    }

    const std::string filePath = eventFilename(cfg_.outputRoot, msg.eventId);
    const std::string parser = (msg.codec == "h265") ? "h265parse" : "h264parse";

    const std::string pipelineText =
        "appsrc name=src is-live=false format=time block=true max-bytes=" +
        std::to_string(cfg_.writerAppsrcMaxBytes) +
        " "
        "! " +
        parser +
        " "
        "! mp4mux faststart=true "
        "! filesink location=" +
        quoteGst(filePath + ".part") + " sync=false";

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipelineText.c_str(), &error);
    if (pipeline == nullptr || error != nullptr) {
        LogLine(std::cerr) << "[REC] cannot create writer pipeline: "
                           << (error ? error->message : "unknown") << "\n";
        if (error)
            g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        return false;
    }

    GstElement* srcElement = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (srcElement == nullptr) {
        LogLine(std::cerr) << "[REC] appsrc not found\n";
        gst_object_unref(pipeline);
        return false;
    }

    GstCaps* caps = nullptr;
    if (msg.codec == "h265") {
        caps = gst_caps_from_string("video/x-h265,stream-format=byte-stream,alignment=au");
    } else {
        caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    }
    gst_app_src_set_caps(GST_APP_SRC(srcElement), caps);
    gst_caps_unref(caps);

    GstBus* bus = gst_element_get_bus(pipeline);
    const GstStateChangeReturn stateRet = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateRet == GST_STATE_CHANGE_FAILURE) {
        LogLine(std::cerr) << "[REC] failed to start writer pipeline\n";
        gst_object_unref(bus);
        gst_object_unref(srcElement);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }

    const auto& first = msg.preRoll.front();
    baseTimeNs_ = first->ptsNs;
    if (first->dtsNs >= 0)
        baseTimeNs_ = std::min(baseTimeNs_, first->dtsNs);
    lastPtsNs_ = -1;
    activeEventId_ = msg.eventId;
    pipeline_ = pipeline;
    appsrc_ = srcElement;
    bus_ = bus;
    finalPath_ = filePath;
    partialPath_ = filePath + ".part";

    LogLine(std::cout) << "[REC] START event=" << activeEventId_ << " file=" << filePath
                       << " preroll_packets=" << msg.preRoll.size() << "\n";
    return true;
}

bool AsyncClipWriter::writePacket(const std::shared_ptr<EncodedPacket>& packet) {
    if (pipeline_ == nullptr || appsrc_ == nullptr || !packet || packet->data.empty())
        return false;
    if (lastPtsNs_ >= 0 && packet->ptsNs <= lastPtsNs_)
        return true;

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, packet->data.size(), nullptr);
    if (buffer == nullptr)
        return false;
    gst_buffer_fill(buffer, 0, packet->data.data(), packet->data.size());

    GST_BUFFER_PTS(buffer) = rebaseTime(packet->ptsNs, baseTimeNs_);
    GST_BUFFER_DTS(buffer) = rebaseTime(packet->dtsNs, baseTimeNs_);
    GST_BUFFER_DURATION(buffer) = packet->durationNs > 0
                                      ? static_cast<GstClockTime>(packet->durationNs)
                                      : GST_CLOCK_TIME_NONE;

    if (!packet->keyframe)
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    else
        GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if (ret != GST_FLOW_OK) {
        LogLine(std::cerr) << "[REC] appsrc push failed, flow=" << static_cast<int>(ret) << "\n";
        return false;
    }

    lastPtsNs_ = packet->ptsNs;
    return true;
}

void AsyncClipWriter::closeSession() {
    if (pipeline_ == nullptr)
        return;

    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));

    GstMessage* msg = gst_bus_timed_pop_filtered(
        static_cast<GstBus*>(bus_),
        static_cast<GstClockTime>(cfg_.writerEosTimeoutSec * GST_SECOND),
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));

    bool complete = false;
    if (msg != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            LogLine(std::cerr) << "[REC] writer error: " << (err ? err->message : "unknown")
                               << "\n";
            fail(std::string("Encoded writer: ") + (err ? err->message : "unknown"));
            if (err)
                g_error_free(err);
            if (debug)
                g_free(debug);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
            complete = true;
        gst_message_unref(msg);
    } else {
        LogLine(std::cerr) << "[REC] writer EOS timeout for event=" << activeEventId_ << "\n";
        fail("Encoded writer EOS timeout");
    }

    LogLine(std::cout) << "[REC] STOP event=" << activeEventId_ << "\n";

    gst_element_set_state(static_cast<GstElement*>(pipeline_), GST_STATE_NULL);
    gst_object_unref(static_cast<GstBus*>(bus_));
    gst_object_unref(static_cast<GstElement*>(appsrc_));
    gst_object_unref(static_cast<GstElement*>(pipeline_));

    pipeline_ = nullptr;
    appsrc_ = nullptr;
    bus_ = nullptr;
    activeEventId_ = 0;
    baseTimeNs_ = 0;
    lastPtsNs_ = -1;
    if (complete && !hasError()) {
        std::error_code error;
        fs::rename(partialPath_, finalPath_, error);
        if (error)
            fail("Cannot finalize encoded clip: " + error.message());
        else
            LogLine(std::cout) << "[REC] READY file=" << finalPath_ << '\n';
    }
    partialPath_.clear();
    finalPath_.clear();
}

EncodedRecorder::EncodedRecorder(const Config& cfg, AsyncClipWriter& writer, std::string codec,
                                 double gopHintSec)
    : cfg_(cfg), writer_(writer), codec_(std::move(codec)), gopHintSec_(std::max(0.0, gopHintSec)) {
    if (codec_ != "h264" && codec_ != "h265")
        throw std::runtime_error("codec must be h264 or h265");
}

void EncodedRecorder::purgeRingLocked(std::int64_t newestPtsNs) {
    // V5: ring length follows camera GOP when known/observed, instead of
    // assuming one fixed GOP for the whole fleet. recordRingSec remains a floor.
    const double gopSec = std::max(gopHintSec_, observedGopSec_);
    const double keepSec =
        std::max(cfg_.recordRingSec, cfg_.eventPreSec + gopSec) + cfg_.recordSafetySec;
    const std::int64_t keepNs =
        static_cast<std::int64_t>(keepSec * static_cast<double>(GST_SECOND));
    const std::int64_t cutoff = newestPtsNs - keepNs;
    while (!ring_.empty() && ring_.front()->ptsNs < cutoff)
        ring_.pop_front();
}

void EncodedRecorder::pushPacket(const std::shared_ptr<EncodedPacket>& packet) {
    if (!enabled() || !packet)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (packet->keyframe) {
        if (lastKeyframePtsNs_ >= 0 && packet->ptsNs > lastKeyframePtsNs_) {
            const double interval = static_cast<double>(packet->ptsNs - lastKeyframePtsNs_) /
                                    static_cast<double>(GST_SECOND);
            if (interval > 0.0)
                observedGopSec_ = std::max(observedGopSec_, interval);
        }
        lastKeyframePtsNs_ = packet->ptsNs;
    }
    ring_.push_back(packet);
    purgeRingLocked(packet->ptsNs);

    if (active_ && packet->ptsNs > lastForwardedPtsNs_) {
        writer_.pushPacket(activeEventId_, packet);
        lastForwardedPtsNs_ = packet->ptsNs;
    }
}

std::vector<std::shared_ptr<EncodedPacket>>
EncodedRecorder::selectPreRollLocked(std::int64_t eventPtsNs) const {
    std::vector<std::shared_ptr<EncodedPacket>> out;
    if (ring_.empty())
        return out;

    const std::int64_t target =
        eventPtsNs - static_cast<std::int64_t>(cfg_.eventPreSec * GST_SECOND);
    std::size_t selected = ring_.size();
    std::size_t firstKeyframe = ring_.size();

    for (std::size_t i = 0; i < ring_.size(); ++i) {
        if (!ring_[i]->keyframe)
            continue;
        if (firstKeyframe == ring_.size())
            firstKeyframe = i;
        if (ring_[i]->ptsNs <= target)
            selected = i;
    }

    if (selected == ring_.size())
        selected = firstKeyframe;
    if (selected == ring_.size())
        return out;

    out.reserve(ring_.size() - selected);
    for (std::size_t i = selected; i < ring_.size(); ++i)
        out.push_back(ring_[i]);
    return out;
}

void EncodedRecorder::startEvent(std::uint64_t eventId, std::int64_t eventStartPtsNs) {
    if (!enabled())
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (active_)
        return;

    const auto preRoll = selectPreRollLocked(eventStartPtsNs);
    if (preRoll.empty()) {
        LogLine(std::cerr) << "[REC] no keyframe in ring for event=" << eventId
                           << "; recording skipped\n";
        return;
    }

    if (!writer_.beginClip(eventId, codec_, preRoll))
        return;

    active_ = true;
    activeEventId_ = eventId;
    lastForwardedPtsNs_ = preRoll.back()->ptsNs;
}

void EncodedRecorder::stopEvent(std::uint64_t eventId) {
    if (!enabled())
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || eventId != activeEventId_)
        return;

    writer_.endClip(eventId);
    active_ = false;
    activeEventId_ = 0;
    lastForwardedPtsNs_ = -1;
}

void EncodedRecorder::finish() {
    if (!enabled())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
        writer_.endClip(activeEventId_);
        active_ = false;
        activeEventId_ = 0;
        lastForwardedPtsNs_ = -1;
    }
}
