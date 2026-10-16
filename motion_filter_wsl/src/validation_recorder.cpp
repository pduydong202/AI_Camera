#include "validation_recorder.hpp"
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
namespace fs = std::filesystem;
namespace {
std::string stem(std::uint64_t id) { std::ostringstream s; s << "motion_" << std::setw(6) << std::setfill('0') << id; return s.str(); }
int cc(const std::string& s) { return cv::VideoWriter::fourcc(s[0], s[1], s[2], s[3]); }
}
ValidationRecorder::ValidationRecorder(const Config& c, double fps, const cv::Size& s)
    : cfg_(c), fps_(fps), size_(s), q_(static_cast<std::size_t>(std::max(1, c.validationQueueMaxMessages))) {}
ValidationRecorder::~ValidationRecorder() { close(); }
void ValidationRecorder::start() { if (!enabled() || started_) return; fs::create_directories(fs::path(cfg_.outputRoot) / "masks"); started_ = true; th_ = std::thread(&ValidationRecorder::run, this); }
void ValidationRecorder::close() { if (!started_) return; q_.close(); if (th_.joinable()) th_.join(); started_ = false; }
void ValidationRecorder::purge(std::int64_t newest) { const auto cut = newest - static_cast<std::int64_t>(cfg_.validationRingSec * 1e9); while (!ring_.empty() && ring_.front()->ptsNs < cut) ring_.pop_front(); }
void ValidationRecorder::pushFrame(const cv::Mat& a, const cv::Mat& b, const cv::Mat& c, const cv::Mat& v, std::int64_t pts) {
    if (!enabled()) return;
    auto f = std::make_shared<LayerFrame>();
    a.copyTo(f->l1); b.copyTo(f->l2); c.copyTo(f->l3); v.copyTo(f->valid); f->ptsNs = pts;
    bool fw = false; std::uint64_t id = 0;
    { std::lock_guard<std::mutex> l(mu_); ring_.push_back(f); purge(pts); fw = active_; id = activeId_; }
    if (fw) { Msg m; m.type = Type::Frame; m.id = id; m.frame = std::move(f); q_.push(std::move(m)); }
}
std::vector<std::shared_ptr<LayerFrame>> ValidationRecorder::selectPre(std::int64_t start) const {
    std::vector<std::shared_ptr<LayerFrame>> o; if (ring_.empty()) return o;
    const auto target = start - static_cast<std::int64_t>(cfg_.eventPreSec * 1e9);
    std::size_t i = 0; while (i < ring_.size() && ring_[i]->ptsNs < target) ++i; if (i >= ring_.size()) i = ring_.size() - 1;
    for (; i < ring_.size(); ++i) o.push_back(ring_[i]); return o;
}
void ValidationRecorder::startEvent(std::uint64_t id, std::int64_t pts) { if (!enabled()) return; Msg m; { std::lock_guard<std::mutex> l(mu_); if (active_) return; m.type = Type::Start; m.id = id; m.pre = selectPre(pts); active_ = true; activeId_ = id; } q_.push(std::move(m)); }
void ValidationRecorder::stopEvent(std::uint64_t id) { if (!enabled()) return; { std::lock_guard<std::mutex> l(mu_); if (!active_ || activeId_ != id) return; active_ = false; activeId_ = 0; } Msg m; m.type = Type::Stop; m.id = id; q_.push(std::move(m)); }
void ValidationRecorder::finish() { if (!enabled()) return; std::uint64_t id = 0; { std::lock_guard<std::mutex> l(mu_); if (!active_) return; id = activeId_; active_ = false; activeId_ = 0; } Msg m; m.type = Type::Stop; m.id = id; q_.push(std::move(m)); }
bool ValidationRecorder::open(const Msg& m) {
    closeWriters(); if (m.pre.empty()) return false;
    const auto dir = fs::path(cfg_.outputRoot) / "masks"; const auto s = stem(m.id); const int f = cc(cfg_.validationFourcc);
    w1_.open((dir / (s + "_L1.mp4")).string(), f, fps_, size_, false);
    w2_.open((dir / (s + "_L2.mp4")).string(), f, fps_, size_, false);
    w3_.open((dir / (s + "_L3.mp4")).string(), f, fps_, size_, false);
    wv_.open((dir / (s + "_VALID.mp4")).string(), f, fps_, size_, false);
    if (!w1_.isOpened() || !w2_.isOpened() || !w3_.isOpened() || !wv_.isOpened()) { std::cerr << "[MASK] cannot open one or more writers\n"; closeWriters(); return false; }
    writerId_ = m.id; std::cout << "[MASK] START event=" << m.id << " preroll_frames=" << m.pre.size() << "\n"; return true;
}
void ValidationRecorder::write(const std::shared_ptr<LayerFrame>& f) { if (!f) return; if (w1_.isOpened()) w1_.write(f->l1); if (w2_.isOpened()) w2_.write(f->l2); if (w3_.isOpened()) w3_.write(f->l3); if (wv_.isOpened()) wv_.write(f->valid); }
void ValidationRecorder::closeWriters() { if (w1_.isOpened()) w1_.release(); if (w2_.isOpened()) w2_.release(); if (w3_.isOpened()) w3_.release(); if (wv_.isOpened()) wv_.release(); if (writerId_) { std::cout << "[MASK] STOP event=" << writerId_ << "\n"; writerId_ = 0; } }
void ValidationRecorder::run() { Msg m; while (q_.pop(m)) { if (m.type == Type::Start) { if (open(m)) for (const auto& f : m.pre) write(f); } else if (m.type == Type::Frame) { if (writerId_ == m.id) write(m.frame); } else if (m.type == Type::Stop) { if (writerId_ == m.id) closeWriters(); } } closeWriters(); }
