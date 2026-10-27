#pragma once

#include "bounded_queue.hpp"
#include "config.hpp"
#include "http_upload.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct EncodedPacket {
    std::vector<std::uint8_t> data;
    std::int64_t ptsNs = 0;
    std::int64_t dtsNs = 0;
    std::int64_t durationNs = 0;
    bool keyframe = false;
};

class AsyncClipWriter {
  public:
    explicit AsyncClipWriter(const Config& cfg);
    ~AsyncClipWriter();

    void start();
    void close();

    bool beginClip(std::uint64_t eventId, const std::string& codec,
                   const std::vector<std::shared_ptr<EncodedPacket>>& preRoll);
    bool pushPacket(std::uint64_t eventId, const std::shared_ptr<EncodedPacket>& packet);
    bool endClip(std::uint64_t eventId);

    bool hasError() const {
        return failed_.load();
    }

    std::string errorMessage() const;
    std::uint64_t sentEvents() const { return sentEvents_.load(); }
    std::uint64_t lostEvents() const { return lostEvents_.load(); }

  private:
    enum class MessageType { Start, Packet, Stop };

    struct Message {
        MessageType type = MessageType::Packet;
        std::uint64_t eventId = 0;
        std::string codec;
        std::vector<std::shared_ptr<EncodedPacket>> preRoll;
        std::shared_ptr<EncodedPacket> packet;
        std::size_t bytes = 0;
    };

    bool enqueue(Message msg);
    void dropEvent(std::uint64_t eventId, const char* reason);

    void run();
    bool openSession(const Message& msg);
    bool writePacket(const std::shared_ptr<EncodedPacket>& packet);
    void closeSession();
    void fail(const std::string& message);

    Config cfg_;
    BoundedQueue<Message> queue_;
    std::thread thread_;
    bool started_ = false;
    std::atomic<bool> failed_{false};
    mutable std::mutex errorMutex_;
    std::string error_;
    std::string partialPath_, finalPath_;

    // GStreamer objects are stored as void* in the header to keep GST headers out.
    void* pipeline_ = nullptr;
    void* appsrc_ = nullptr;
    void* bus_ = nullptr;
    std::uint64_t activeEventId_ = 0;
    std::int64_t baseTimeNs_ = 0;
    std::int64_t lastPtsNs_ = 0;
    std::unique_ptr<HttpUpload> upload_;
    bool httpSessionFailed_ = false;
    std::atomic<std::size_t> queuedBytes_{0};
    std::atomic<std::uint64_t> abortedThrough_{0}, sentEvents_{0}, lostEvents_{0};
    std::atomic<std::int64_t> shutdownDeadlineNs_{0};
};

class EncodedRecorder {
  public:
    EncodedRecorder(const Config& cfg, AsyncClipWriter& writer, std::string codec,
                    double gopHintSec);

    void pushPacket(const std::shared_ptr<EncodedPacket>& packet);
    void startEvent(std::uint64_t eventId, std::int64_t eventStartPtsNs);
    void stopEvent(std::uint64_t eventId);
    void finish();

    bool enabled() const {
        return cfg_.recordEnabled != 0;
    }

  private:
    void purgeRingLocked(std::int64_t newestPtsNs);
    std::vector<std::shared_ptr<EncodedPacket>> selectPreRollLocked(std::int64_t eventPtsNs) const;

    Config cfg_;
    AsyncClipWriter& writer_;
    std::string codec_;
    double gopHintSec_ = 0.0;
    double observedGopSec_ = 0.0;
    std::int64_t lastKeyframePtsNs_ = -1;
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<EncodedPacket>> ring_;

    bool active_ = false;
    std::uint64_t activeEventId_ = 0;
    std::int64_t lastForwardedPtsNs_ = -1;
    std::size_t ringBytes_ = 0;
};
