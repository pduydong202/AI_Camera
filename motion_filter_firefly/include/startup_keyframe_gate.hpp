#pragma once

#include <cstddef>
#include <cstdint>

// RTSP may join in the middle of a GOP. Do not submit undecodable leading
// pictures to either consumer: some MPP plugins associate the first decoded
// picture with the oldest queued input PTS. Input is parsed Annex-B / AU.
// This gate is one-shot; it never filters or retimes frames after startup.
class StartupKeyframeGate {
  public:
    bool accept(const std::uint8_t* data, std::size_t size, bool h265) {
        if (opened_) return true;
        if (hasRandomAccess(data, size, h265)) {
            opened_ = true;
            return true;
        }
        ++dropped_;
        return false;
    }

    bool opened() const { return opened_; }
    std::uint64_t dropped() const { return dropped_; }

  private:
    static bool hasRandomAccess(const std::uint8_t* data, std::size_t size, bool h265) {
        if (!data) return false;
        for (std::size_t i = 0; i + 3 < size; ++i) {
            if (data[i] != 0 || data[i + 1] != 0) continue;
            std::size_t prefix = 0;
            if (data[i + 2] == 1) prefix = 3;
            else if (data[i + 2] == 0 && data[i + 3] == 1) prefix = 4;
            if (!prefix || i + prefix >= size) continue;
            const auto nal = i + prefix;
            if ((data[nal] & 0x80U) != 0) continue;
            if (h265) {
                if (nal + 2 >= size || (data[nal + 1] & 7U) == 0) continue;
                const unsigned type = (data[nal] >> 1U) & 63U;
                if (type >= 16 && type <= 21) return true;
            } else if (nal + 1 < size && (data[nal] & 31U) == 5) {
                return true;
            }
            i = nal;
        }
        return false;
    }

    bool opened_ = false;
    std::uint64_t dropped_ = 0;
};
