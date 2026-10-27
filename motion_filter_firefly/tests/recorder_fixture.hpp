#pragma once
#include "recorder.hpp"
#include <chrono>
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '\\' || c == '\"')
            o += '\\';
        o += c;
    }
    return o + '\"';
}

std::vector<std::shared_ptr<EncodedPacket>> readPackets(const fs::path& path) {
    const std::string text = "filesrc location=" + quoted(path.string()) +
                             " ! qtdemux ! h264parse config-interval=-1 ! "
                             "video/x-h264,stream-format=byte-stream,alignment=au ! appsink name=s "
                             "sync=false wait-on-eos=false";
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(text.c_str(), &error);
    check(pipeline && error == nullptr, "Cannot parse test reader pipeline");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "s");
    check(sink, "No test sink");

    struct Owner {
        GstElement* pipeline;
        GstElement* sink;

        ~Owner() {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(sink);
            gst_object_unref(pipeline);
        }
    } owner{pipeline, sink};

    check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
          "Cannot start test reader");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::vector<std::shared_ptr<EncodedPacket>> packets;
    for (;;) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
        const std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sampleOwner(sample,
                                                                                  gst_sample_unref);
        if (!sample) {
            if (gst_app_sink_is_eos(GST_APP_SINK(sink)))
                break;
            check(std::chrono::steady_clock::now() < deadline, "Test reader timed out");
            continue;
        }
        auto* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        check(buffer && gst_buffer_map(buffer, &map, GST_MAP_READ), "Cannot map test packet");

        struct MapOwner {
            GstBuffer* b;
            GstMapInfo* m;

            ~MapOwner() {
                gst_buffer_unmap(b, m);
            }
        } mapped{buffer, &map};

        auto packet = std::make_shared<EncodedPacket>();
        packet->data.assign(map.data, map.data + map.size);
        packet->ptsNs = static_cast<std::int64_t>(GST_BUFFER_PTS(buffer));
        packet->dtsNs = static_cast<std::int64_t>(GST_BUFFER_DTS(buffer));
        packet->durationNs = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(buffer))
                                 ? static_cast<std::int64_t>(GST_BUFFER_DURATION(buffer))
                                 : 50000000;
        packet->keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        packets.push_back(std::move(packet));
    }
    check(!packets.empty(), "No test packets");
    return packets;
}

// Compare compressed picture NAL payloads; parser/muxer may rewrite SPS/PPS/AUD.
std::vector<std::vector<std::uint8_t>> pictures(const std::vector<std::uint8_t>& data) {
    std::vector<std::vector<std::uint8_t>> out;
    auto start = [&](std::size_t i) -> std::size_t {
        if (i + 3 <= data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            return 3;
        if (i + 4 <= data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
            data[i + 3] == 1)
            return 4;
        return 0;
    };
    for (std::size_t i = 0; i < data.size();) {
        const auto n = start(i);
        if (!n) {
            ++i;
            continue;
        }
        const auto begin = i + n;
        auto end = begin;
        while (end < data.size() && !start(end))
            ++end;
        if (begin < end && (data[begin] & 31) >= 1 && (data[begin] & 31) <= 5)
            out.emplace_back(data.begin() + begin, data.begin() + end);
        i = end;
    }
    return out;
}
