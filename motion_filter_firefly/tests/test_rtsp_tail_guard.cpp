#include "rtsp_tail_guard.hpp"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

struct Pipeline {
    GstElement* value = nullptr;
    explicit Pipeline(const std::string& text) {
        GError* error = nullptr;
        value = gst_parse_launch(text.c_str(), &error);
        if (!value || error) {
            const std::string message = error ? error->message : "Pipeline unavailable";
            if (error) g_error_free(error);
            if (value) gst_object_unref(value);
            throw std::runtime_error(message);
        }
    }
    ~Pipeline() { gst_element_set_state(value, GST_STATE_NULL); gst_object_unref(value); }
    void play() { check(gst_element_set_state(value,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"PLAYING failed"); }
};

struct Packet {
    std::vector<std::uint8_t> bytes;
    GstClockTime pts = GST_CLOCK_TIME_NONE;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
    guint flags = 0;
};

Packet copy(GstBuffer* buffer) {
    Packet packet;
    packet.bytes.resize(gst_buffer_get_size(buffer));
    gst_buffer_extract(buffer,0,packet.bytes.data(),packet.bytes.size());
    packet.pts=GST_BUFFER_PTS(buffer);packet.duration=GST_BUFFER_DURATION(buffer);
    packet.flags=GST_BUFFER_FLAGS(buffer);
    return packet;
}

void push(GstElement* source,const Packet& packet) {
    auto* buffer=gst_buffer_new_allocate(nullptr,packet.bytes.size(),nullptr);
    gst_buffer_fill(buffer,0,packet.bytes.data(),packet.bytes.size());
    GST_BUFFER_PTS(buffer)=packet.pts;GST_BUFFER_DURATION(buffer)=packet.duration;
    GST_BUFFER_FLAGS(buffer)=packet.flags;
    check(gst_app_src_push_buffer(GST_APP_SRC(source),buffer)==GST_FLOW_OK,"appsrc push failed");
}

GstSample* pull(GstElement* pipeline,GstElement* sink) {
    auto* sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),5*GST_SECOND);
    if (sample) return sample;
    auto* bus=gst_element_get_bus(pipeline);
    auto* message=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (message) {
        GError* error=nullptr;gchar* debug=nullptr;
        gst_message_parse_error(message,&error,&debug);
        const std::string text=error?error->message:"Pipeline error";
        if (error)g_error_free(error);
        g_free(debug);gst_message_unref(message);
        throw std::runtime_error(text);
    }
    check(gst_app_sink_is_eos(GST_APP_SINK(sink)),"Timed out waiting for EOS");
    return nullptr;
}

std::vector<Packet> accessUnits(const char* fixture) {
    Pipeline pipeline(std::string("filesrc location=\"")+fixture+"\" ! qtdemux ! h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! appsink name=sink sync=false wait-on-eos=false");
    auto* sink=gst_bin_get_by_name(GST_BIN(pipeline.value),"sink");
    pipeline.play();std::vector<Packet> result;
    while(result.size()<3) {
        auto* sample=pull(pipeline.value,sink);check(sample!=nullptr,"Fixture too short");
        result.push_back(copy(gst_sample_get_buffer(sample)));gst_sample_unref(sample);
    }
    gst_object_unref(sink);return result;
}

std::vector<Packet> packetize(const std::vector<Packet>& units,GstCaps*& caps) {
    Pipeline pipeline("appsrc name=source format=time ! rtph264pay pt=96 mtu=64 ! "
        "appsink name=sink sync=false wait-on-eos=false");
    auto* source=gst_bin_get_by_name(GST_BIN(pipeline.value),"source");
    auto* sink=gst_bin_get_by_name(GST_BIN(pipeline.value),"sink");
    auto* input=gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au,framerate=20/1");
    gst_app_src_set_caps(GST_APP_SRC(source),input);gst_caps_unref(input);
    pipeline.play();
    for(std::size_t i=0;i<units.size();++i) {
        auto packet=units[i];packet.pts=i*50*GST_MSECOND;packet.duration=50*GST_MSECOND;
        push(source,packet);
    }
    gst_app_src_end_of_stream(GST_APP_SRC(source));std::vector<Packet> result;
    while(auto* sample=pull(pipeline.value,sink)) {
        if(!caps)caps=gst_caps_ref(gst_sample_get_caps(sample));
        result.push_back(copy(gst_sample_get_buffer(sample)));gst_sample_unref(sample);
    }
    gst_object_unref(source);gst_object_unref(sink);return result;
}

std::vector<Packet> receive(const std::vector<Packet>& packets,GstCaps* caps,bool guard) {
    Pipeline pipeline("appsrc name=source format=time ! rtph264depay name=depay ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! appsink name=sink sync=false wait-on-eos=false");
    auto* source=gst_bin_get_by_name(GST_BIN(pipeline.value),"source");
    auto* sink=gst_bin_get_by_name(GST_BIN(pipeline.value),"sink");
    std::unique_ptr<RtspTailGuard> protection;
    if(guard) {
        auto* depay=gst_bin_get_by_name(GST_BIN(pipeline.value),"depay");
        protection=std::make_unique<RtspTailGuard>(depay);gst_object_unref(depay);
    }
    gst_app_src_set_caps(GST_APP_SRC(source),caps);pipeline.play();
    for(const auto& packet:packets)push(source,packet);
    gst_app_src_end_of_stream(GST_APP_SRC(source));std::vector<Packet> result;
    while(auto* sample=pull(pipeline.value,sink)) {
        result.push_back(copy(gst_sample_get_buffer(sample)));gst_sample_unref(sample);
    }
    if(protection)check(protection->eosCount()==1,"EOS did not pass through guard");
    gst_element_set_state(pipeline.value,GST_STATE_NULL);protection.reset();
    gst_object_unref(source);gst_object_unref(sink);return result;
}

int main(int argc,char** argv) {
    try {
        check(argc==2,"Usage: test_rtsp_tail_guard fixture.mp4");
        gst_init(nullptr,nullptr);
        GstCaps* caps=nullptr;
        const auto packets=packetize(accessUnits(argv[1]),caps);
        check(packets.size()>5 && packets.back().bytes.size()>14,"Fixture did not fragment");
        check((packets.back().bytes[1]&0x80)!=0,"Last RTP marker missing");
        check((packets.back().bytes[12]&31U)==28U,"Last packet is not FU-A");
        auto truncated=packets;truncated.pop_back();
        const auto complete=receive(packets,caps,false);
        const auto protectedComplete=receive(packets,caps,true);
        const auto unsafe=receive(truncated,caps,false);
        const auto protectedTruncated=receive(truncated,caps,true);
        gst_caps_unref(caps);
        std::cout<<"AU counts complete="<<complete.size()<<" protected="<<protectedComplete.size()
                 <<" truncated="<<unsafe.size()<<" protected_truncated="<<protectedTruncated.size()<<'\n';
        check(complete.size()==3 && protectedComplete.size()==complete.size(),"Complete AU lost");
        for(std::size_t i=0;i<complete.size();++i) {
            check(complete[i].bytes==protectedComplete[i].bytes,"Complete payload changed");
            check(complete[i].pts==protectedComplete[i].pts &&
                  complete[i].duration==protectedComplete[i].duration,"Complete timestamps changed");
        }
        check(unsafe.size()==3,"Truncated FU regression was not reproduced");
        check(unsafe.back().bytes!=complete.back().bytes,"Unsafe tail is unexpectedly complete");
        check(protectedTruncated.size()==2,"Incomplete final AU was forwarded");
        for(std::size_t i=0;i<protectedTruncated.size();++i) {
            check(protectedTruncated[i].bytes==complete[i].bytes,"Earlier complete AU changed");
            check(protectedTruncated[i].pts==complete[i].pts,"Earlier complete PTS changed");
        }
        std::cout<<"PASS: complete RTP AUs preserved byte-for-byte; truncated FU-A at EOS discarded before parser\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
