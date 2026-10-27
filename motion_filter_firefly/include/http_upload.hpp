#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One streaming PUT per event. No temporary files, redirects or automatic replay.
// Producers never wait for the network; exceeding the byte budget aborts this event.
class HttpUpload {
  public:
    HttpUpload(std::string url, std::string token, std::size_t maxBytes, double timeoutSec);
    ~HttpUpload();
    bool push(const std::uint8_t* data, std::size_t size) noexcept;
    bool finish(double waitSec = -1);
    void cancel() noexcept;
    bool failed() const { return failed_.load(); }
    std::string error() const;

  private:
    void run() noexcept;
    void fail(const std::string& message);
    static std::size_t read(char*, std::size_t, std::size_t, void*) noexcept;
    static std::size_t collectResponse(char*, std::size_t, std::size_t, void*) noexcept;
    std::string url_, token_, responseBody_;
    std::size_t maxBytes_, queuedBytes_ = 0, frontOffset_ = 0, responseBytes_ = 0;
    double timeoutSec_;
    std::deque<std::vector<std::uint8_t>> chunks_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool eof_ = false, consumedEof_ = false, done_ = false;
    std::atomic<bool> failed_{false};
    std::string error_;
    std::thread thread_;
};
