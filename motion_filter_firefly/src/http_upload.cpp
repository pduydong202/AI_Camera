#include "http_upload.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

HttpUpload::HttpUpload(std::string url, std::string token, std::size_t maxBytes, double timeoutSec)
    : url_(std::move(url)), token_(std::move(token)), maxBytes_(maxBytes), timeoutSec_(timeoutSec) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK)
        throw std::runtime_error("Cannot initialize HTTP client");
    thread_ = std::thread(&HttpUpload::run, this);
}

HttpUpload::~HttpUpload() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void HttpUpload::fail(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failed_.exchange(true)) error_ = message;
    changed_.notify_all();
}

void HttpUpload::cancel() noexcept {
    fail("upload cancelled");
}

std::string HttpUpload::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool HttpUpload::push(const std::uint8_t* data, std::size_t size) noexcept {
    if (size == 0) return !failed_;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_ || done_ || eof_) return false;
        if (size > maxBytes_ - queuedBytes_) {
            error_ = "HTTP byte buffer full";
            failed_ = true;
            changed_.notify_all();
            return false;
        }
        chunks_.emplace_back(data, data + size);
        queuedBytes_ += size;
        changed_.notify_all();
        return true;
    } catch (...) {
        fail("HTTP buffer allocation failed");
        return false;
    }
}

std::size_t HttpUpload::read(char* out, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& self = *static_cast<HttpUpload*>(opaque);
    try {
        std::unique_lock<std::mutex> lock(self.mutex_);
        if (!self.changed_.wait_for(lock, std::chrono::duration<double>(self.timeoutSec_), [&] {
                return self.failed_ || self.eof_ || !self.chunks_.empty();
            })) {
            self.error_ = "muxer produced no data before timeout";
            self.failed_ = true;
        }
        if (self.failed_) return CURL_READFUNC_ABORT;
        if (self.chunks_.empty()) {
            self.consumedEof_ = true;
            return 0;
        }
        auto& front = self.chunks_.front();
        const auto bytes = std::min(size * count, front.size() - self.frontOffset_);
        std::memcpy(out, front.data() + self.frontOffset_, bytes);
        self.frontOffset_ += bytes;
        self.queuedBytes_ -= bytes;
        if (self.frontOffset_ == front.size()) {
            self.chunks_.pop_front();
            self.frontOffset_ = 0;
        }
        return bytes;
    } catch (...) {
        return CURL_READFUNC_ABORT;
    }
}

std::size_t HttpUpload::collectResponse(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& self = *static_cast<HttpUpload*>(opaque);
    try {
        const auto bytes = size * count;
        if (bytes > 16384 - self.responseBytes_) return 0;
        self.responseBytes_ += bytes;
        self.responseBody_.append(data, bytes);
        return bytes;
    } catch (...) {
        return 0;
    }
}

void HttpUpload::run() noexcept {
    try {
        const auto cleanup = [](CURL* p) { if (p) curl_easy_cleanup(p); };
        std::unique_ptr<CURL, decltype(cleanup)> handle(curl_easy_init(), cleanup);
        if (!handle) throw std::runtime_error("Cannot create HTTP client");
        CURL* h = handle.get();
        curl_slist* raw = nullptr;
        raw = curl_slist_append(raw, "Content-Type: video/mp4");
        raw = curl_slist_append(raw, "Transfer-Encoding: chunked");
        raw = curl_slist_append(raw, "Expect:");
        raw = curl_slist_append(raw, ("Authorization: Bearer " + token_).c_str());
        const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
        curl_easy_setopt(h, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, raw);
        curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutSec_ * 1000));
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, static_cast<long>(std::ceil(timeoutSec_)));
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
        // Read the receiver's bounded error JSON as well as its status code.
        curl_easy_setopt(h, CURLOPT_FAILONERROR, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(h, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(h, CURLOPT_PROXY, ""); // Camera traffic never follows ambient proxy settings.
        curl_easy_setopt(h, CURLOPT_READFUNCTION, &HttpUpload::read);
        curl_easy_setopt(h, CURLOPT_READDATA, this);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &HttpUpload::collectResponse);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        // curl_off_t is the ABI-defined type for this callback on each target.
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION,
            +[](void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<HttpUpload*>(p)->failed() ? 1 : 0;
            });
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, this);
        const auto result = curl_easy_perform(h);
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        auto detail = responseBody_;
        if (!token_.empty()) {
            std::size_t pos = 0;
            while ((pos = detail.find(token_, pos)) != std::string::npos) {
                detail.replace(pos, token_.size(), "[redacted]");
                pos += 10;
            }
        }
        for (auto& ch : detail)
            if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) == 127) ch = ' ';
        detail.resize(std::min<std::size_t>(detail.size(), 2048));
        const std::string response = "HTTP " + std::to_string(status) +
            (detail.empty() ? "" : " receiver=" + detail);
        if (status >= 300)
            fail(response);
        else if (result != CURLE_OK)
            fail(std::string("HTTP transfer failed: ") + curl_easy_strerror(result) + " (" + response + ")");
        else if (status != 201)
            fail("receiver did not confirm creation (" + response + ")");
        else {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!consumedEof_) {
                error_ = "receiver replied before complete upload";
                failed_ = true;
            }
        }
    } catch (const std::exception& e) {
        fail(e.what());
    } catch (...) {
        fail("unknown HTTP transfer error");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    changed_.notify_all();
}

bool HttpUpload::finish(double waitSec) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        eof_ = true;
        changed_.notify_all();
        const auto wait = waitSec < 0 ? timeoutSec_ : std::min(timeoutSec_, waitSec);
        if (!changed_.wait_for(lock, std::chrono::duration<double>(wait), [&] { return done_; })) {
            error_ = "receiver completion timeout";
            failed_ = true;
            changed_.notify_all();
        }
    }
    if (thread_.joinable()) thread_.join();
    return !failed_;
}
