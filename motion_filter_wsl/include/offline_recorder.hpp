#pragma once
#include "bounded_queue.hpp"
#include "config.hpp"
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct SourceFrame { cv::Mat bgr; std::int64_t ptsNs=0; };

class OfflineClipRecorder {
public:
    OfflineClipRecorder(const Config& cfg,double fps,const cv::Size& frameSize);
    ~OfflineClipRecorder();
    void start(); void close();
    void pushFrame(const cv::Mat& bgr,std::int64_t ptsNs);
    void startEvent(std::uint64_t eventId,std::int64_t motionStartPtsNs);
    void stopEvent(std::uint64_t eventId);
    void finish();
    bool enabled() const { return cfg_.offlineRecordEnabled!=0; }
private:
    enum class Type{Start,Frame,Stop};
    struct Message{ Type type=Type::Frame; std::uint64_t eventId=0; std::vector<std::shared_ptr<SourceFrame>> pre; std::shared_ptr<SourceFrame> frame; };
    void run(); bool openWriter(const Message&); void writeFrame(const std::shared_ptr<SourceFrame>&); void closeWriter();
    void purge(std::int64_t newest); std::vector<std::shared_ptr<SourceFrame>> selectPre(std::int64_t start) const;
    Config cfg_; double fps_; cv::Size frameSize_; BoundedQueue<Message> queue_; std::thread thread_; bool started_=false;
    mutable std::mutex mu_; std::deque<std::shared_ptr<SourceFrame>> ring_; bool active_=false; std::uint64_t activeId_=0;
    cv::VideoWriter writer_; std::uint64_t writerId_=0;
};
