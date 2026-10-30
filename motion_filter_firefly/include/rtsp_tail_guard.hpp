#pragma once

#include <gst/gst.h>
#include <atomic>
#include <stdexcept>

// The depayloader must negotiate alignment=au. On RTSP disconnect, draining
// GStreamer 1.20 can drain early slices while the final slice is unfinished,
// manufacturing a truncated final picture.
// Forward EOS directly to the parser; only already completed AUs may drain.
// File input does not use this guard.
class RtspTailGuard {
  public:
    explicit RtspTailGuard(GstElement* depay) {
        sink_ = gst_element_get_static_pad(depay, "sink");
        source_ = gst_element_get_static_pad(depay, "src");
        if (!sink_ || !source_) {
            clear();
            throw std::runtime_error("RTSP depayloader pads unavailable");
        }
        probe_ = gst_pad_add_probe(sink_, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
            [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
                auto* self = static_cast<RtspTailGuard*>(data);
                auto* event = GST_PAD_PROBE_INFO_EVENT(info);
                if (!event || GST_EVENT_TYPE(event) != GST_EVENT_EOS)
                    return GST_PAD_PROBE_OK;
                ++self->eosCount_;
                const bool delivered = gst_pad_push_event(self->source_, gst_event_ref(event));
                if (!delivered && !GST_PAD_IS_FLUSHING(self->source_)) {
                    auto* parent = gst_pad_get_parent_element(self->sink_);
                    if (parent) {
                        auto* error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
                            "Cannot finalize complete RTSP access units");
                        gst_element_post_message(parent, gst_message_new_error(
                            GST_OBJECT(parent), error, nullptr));
                        g_error_free(error);
                        gst_object_unref(parent);
                    }
                }
                return GST_PAD_PROBE_DROP;
            }, this, nullptr);
        if (!probe_) {
            clear();
            throw std::runtime_error("Cannot install RTSP tail guard");
        }
    }

    ~RtspTailGuard() { clear(); }
    RtspTailGuard(const RtspTailGuard&) = delete;
    RtspTailGuard& operator=(const RtspTailGuard&) = delete;
    unsigned eosCount() const { return eosCount_.load(); }

  private:
    void clear() {
        if (sink_) {
            if (probe_) gst_pad_remove_probe(sink_, probe_);
            gst_object_unref(sink_);
            sink_ = nullptr;
            probe_ = 0;
        }
        if (source_) { gst_object_unref(source_); source_ = nullptr; }
    }
    GstPad* sink_ = nullptr;
    GstPad* source_ = nullptr;
    gulong probe_ = 0;
    std::atomic<unsigned> eosCount_{0};
};
