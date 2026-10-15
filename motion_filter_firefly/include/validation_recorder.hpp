#pragma once
#include "bounded_queue.hpp"
#include "config.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <thread>
#include <vector>

struct LayerFrame {
    cv::Mat l1, l2, l3, valid;
    std::int64_t ptsNs = 0;
};

class ValidationRecorder {
  public:
    ValidationRecorder(const Config&, double, const cv::Size&);
    ~ValidationRecorder();
    void start();
    void close();
    void pushFrame(const cv::Mat&, const cv::Mat&, const cv::Mat&, const cv::Mat&, std::int64_t);
    void startEvent(std::uint64_t, std::int64_t);
    void stopEvent(std::uint64_t);
    void finish();

    bool enabled() const {
        return cfg_.validationEnabled != 0;
    }

  private:
    enum class Type { Start, Frame, Stop };

    struct Msg {
        Type type = Type::Frame;
        std::uint64_t id = 0;
        std::vector<std::shared_ptr<LayerFrame>> pre;
        std::shared_ptr<LayerFrame> frame;
    };

    void run();
    void purge(std::int64_t);
    std::vector<std::shared_ptr<LayerFrame>> selectPre(std::int64_t) const;
    bool open(const Msg&);
    void write(const std::shared_ptr<LayerFrame>&);
    void closeWriters();
    Config cfg_;
    double fps_;
    cv::Size size_;
    BoundedQueue<Msg> q_;
    std::thread th_;
    bool started_ = false;
    mutable std::mutex mu_;
    std::deque<std::shared_ptr<LayerFrame>> ring_;
    bool active_ = false;
    std::uint64_t activeId_ = 0, writerId_ = 0;
    cv::VideoWriter w1_, w2_, w3_, wv_;
};
