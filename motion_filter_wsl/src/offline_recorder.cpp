#include "offline_recorder.hpp"
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
namespace fs=std::filesystem;
namespace {
std::string stem(std::uint64_t id){ std::ostringstream s; s<<"event_"<<std::setw(6)<<std::setfill('0')<<id; return s.str(); }
int fourcc(const std::string& s){ return cv::VideoWriter::fourcc(s[0],s[1],s[2],s[3]); }
}
OfflineClipRecorder::OfflineClipRecorder(const Config& c,double fps,const cv::Size& size):cfg_(c),fps_(fps),frameSize_(size),queue_(static_cast<std::size_t>(std::max(1,c.offlineWriterQueueMaxMessages))){}
OfflineClipRecorder::~OfflineClipRecorder(){ close(); }
void OfflineClipRecorder::start(){ if(!enabled()||started_)return; fs::create_directories(fs::path(cfg_.outputRoot)/"clips"); started_=true; thread_=std::thread(&OfflineClipRecorder::run,this); }
void OfflineClipRecorder::close(){ if(!started_)return; queue_.close(); if(thread_.joinable())thread_.join(); started_=false; }
void OfflineClipRecorder::purge(std::int64_t newest){ const auto keep=static_cast<std::int64_t>((cfg_.eventPreSec+cfg_.recordSafetySec)*1e9); const auto cut=newest-keep; while(!ring_.empty()&&ring_.front()->ptsNs<cut)ring_.pop_front(); }
void OfflineClipRecorder::pushFrame(const cv::Mat& bgr,std::int64_t pts){ if(!enabled())return; auto f=std::make_shared<SourceFrame>(); bgr.copyTo(f->bgr); f->ptsNs=pts; bool forward=false; std::uint64_t id=0; {std::lock_guard<std::mutex>l(mu_); ring_.push_back(f); purge(pts); forward=active_; id=activeId_;} if(forward){Message m;m.type=Type::Frame;m.eventId=id;m.frame=std::move(f);queue_.push(std::move(m));}}
std::vector<std::shared_ptr<SourceFrame>> OfflineClipRecorder::selectPre(std::int64_t start) const{ std::vector<std::shared_ptr<SourceFrame>> out; if(ring_.empty())return out; const auto target=start-static_cast<std::int64_t>(cfg_.eventPreSec*1e9); std::size_t i=0; while(i<ring_.size()&&ring_[i]->ptsNs<target)++i; if(i>=ring_.size())i=ring_.size()-1; out.reserve(ring_.size()-i); for(;i<ring_.size();++i)out.push_back(ring_[i]); return out; }
void OfflineClipRecorder::startEvent(std::uint64_t id,std::int64_t startPts){ if(!enabled())return; Message m; {std::lock_guard<std::mutex>l(mu_); if(active_)return; m.type=Type::Start;m.eventId=id;m.pre=selectPre(startPts);active_=true;activeId_=id;} queue_.push(std::move(m)); }
void OfflineClipRecorder::stopEvent(std::uint64_t id){ if(!enabled())return; {std::lock_guard<std::mutex>l(mu_);if(!active_||activeId_!=id)return;active_=false;activeId_=0;} Message m;m.type=Type::Stop;m.eventId=id;queue_.push(std::move(m)); }
void OfflineClipRecorder::finish(){ if(!enabled())return; std::uint64_t id=0; {std::lock_guard<std::mutex>l(mu_);if(!active_)return;id=activeId_;active_=false;activeId_=0;} Message m;m.type=Type::Stop;m.eventId=id;queue_.push(std::move(m)); }
bool OfflineClipRecorder::openWriter(const Message& m){ closeWriter(); if(m.pre.empty())return false; const auto path=fs::path(cfg_.outputRoot)/"clips"/(stem(m.eventId)+".mp4"); writer_.open(path.string(),fourcc(cfg_.offlineFourcc),fps_,frameSize_,true); if(!writer_.isOpened()){std::cerr<<"[REC] cannot open "<<path<<"\n";return false;} writerId_=m.eventId; std::cout<<"[REC] START event="<<m.eventId<<" preroll_frames="<<m.pre.size()<<" file="<<path<<"\n"; return true; }
void OfflineClipRecorder::writeFrame(const std::shared_ptr<SourceFrame>& f){ if(writer_.isOpened()&&f&&!f->bgr.empty())writer_.write(f->bgr); }
void OfflineClipRecorder::closeWriter(){ if(writer_.isOpened())writer_.release(); if(writerId_){std::cout<<"[REC] STOP event="<<writerId_<<"\n";writerId_=0;} }
void OfflineClipRecorder::run(){ Message m; while(queue_.pop(m)){ if(m.type==Type::Start){ if(openWriter(m))for(const auto&f:m.pre)writeFrame(f);} else if(m.type==Type::Frame){if(writerId_==m.eventId)writeFrame(m.frame);} else if(m.type==Type::Stop){if(writerId_==m.eventId)closeWriter();} } closeWriter(); }
