#include "l2_capture.hpp"
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("Usage: l2_capture_inspect session/l2_capture.bin");
        l2capture::Reader reader(argv[1]);
        l2capture::Frame frame;
        std::uint64_t motion = 0, repeat = 0;
        std::int64_t first = 0, last = 0;
        while (reader.next(frame)) {
            if (frame.index == 0)
                first = frame.meta.ptsNs;
            else
                l2capture::require(frame.meta.ptsNs > last, "non-increasing PTS");
            last = frame.meta.ptsNs;
            motion += frame.meta.motion;
            repeat += frame.meta.repeatCells > 0;
            for (std::size_t i = 0; i < frame.masks[0].size(); ++i)
                l2capture::require((frame.masks[1][i] == 0 && frame.masks[2][i] == 0) ||
                                       frame.masks[0][i] != 0,
                                   "rain/VALID outside L1");
        }
        l2capture::require(reader.complete() && reader.frames() > 0, "empty/incomplete capture");
        std::cout << "CAPTURE_CHECK frames=" << reader.frames()
                  << " complete=YES crc=OK native_mask_counts=EQUAL indices=CONTIGUOUS"
                  << " motion_frames=" << motion << " repeat_prior_frames=" << repeat
                  << " pts_span_sec=" << static_cast<double>(last - first) / 1e9
                  << " size=" << reader.header.width << 'x' << reader.header.height
                  << " grid=" << reader.header.cols << 'x' << reader.header.rows
                  << " history=" << reader.header.history << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "CAPTURE_ERROR: " << e.what() << '\n';
        return 1;
    }
}
