#include "l2_capture.hpp"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

void check(bool good) {
    if (!good)
        throw std::runtime_error("Capture transport assertion failed");
}

void mustReject(const fs::path& path) {
    bool rejected = false;
    try {
        l2capture::Reader r(path.string());
        l2capture::Frame f;
        while (r.next(f)) {
        }
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected);
}

int main(int argc, char** argv) {
    try {
        check(argc == 2);
        const fs::path dir(argv[1]), path = dir / "roundtrip.bin";
        l2capture::Header h;
        h.width = 9;
        h.height = 3;
        h.cols = 2;
        h.rows = 2;
        h.history = 500;
        h.taFrames = 4;
        h.widthThreshold = 1;
        h.fps = 20;
        h.eventRatio = 0.005;
        // A padded source checks that row padding never enters the stream.
        std::array<l2capture::Bytes, 3> padded;
        for (auto& p : padded)
            p.assign(17 * h.height, 99);
        const std::array<float, 6> density{{0.125F, 0.25F, -99.0F, 0.375F, 0.5F, -99.0F}};
        std::array<std::array<l2capture::Bytes, 3>, 3> expected;
        const std::array<std::int64_t, 3> pts{{0, 50000000, 9876543210123LL}};
        {
            l2capture::Writer writer(path.string());
            writer.begin(h);
            for (unsigned frame = 0; frame < 3; ++frame) {
                for (unsigned plane = 0; plane < 3; ++plane) {
                    auto& out = expected[frame][plane];
                    for (unsigned y = 0; y < h.height; ++y)
                        for (unsigned x = 0; x < h.width; ++x) {
                            const auto i = y * h.width + x;
                            const bool on =
                                frame == 2 || (frame == 0 && i % 2 == 0 && i % (plane + 2) == 0);
                            const auto value = static_cast<std::uint8_t>(on ? 255 : 0);
                            padded[plane][y * 17 + x] = value;
                            out.push_back(value);
                        }
                }
                l2capture::Meta meta;
                meta.ptsNs = pts[frame];
                meta.motion = frame == 2;
                meta.warmup = frame > 0;
                meta.l1Pixels = l2capture::white(expected[frame][0]);
                meta.rainPixels = l2capture::white(expected[frame][1]);
                meta.validPixels = l2capture::white(expected[frame][2]);
                const std::array<l2capture::Plane<std::uint8_t>, 3> views{
                    {{padded[0].data(), 17}, {padded[1].data(), 17}, {padded[2].data(), 17}}};
                writer.append(meta, {density.data(), 3}, views);
            }
            writer.finish();
            check(writer.frames() == 3);
        }
        l2capture::Reader reader(path.string());
        l2capture::Frame frame;
        check(reader.header.width == 9 && reader.header.eventRatio == h.eventRatio);
        unsigned count = 0;
        while (reader.next(frame)) {
            check(frame.index == count && frame.meta.ptsNs == pts[count]);
            check(frame.masks == expected[count]);
            check(frame.density == std::vector<float>({0.125F, 0.25F, 0.375F, 0.5F}));
            ++count;
        }
        check(count == 3 && reader.complete() && !reader.next(frame));
        std::ifstream in(path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto corrupt = raw;
        corrupt[65] ^= 1;
        const auto damaged = dir / "corrupt.bin", truncated = dir / "truncated.bin";
        {
            std::ofstream out(damaged, std::ios::binary);
            out.write(corrupt.data(), static_cast<std::streamsize>(corrupt.size()));
        }
        {
            std::ofstream out(truncated, std::ios::binary);
            out.write(raw.data(), static_cast<std::streamsize>(raw.size() - 5));
        }
        mustReject(damaged);
        mustReject(truncated);
        std::cout << "PASS: lossless masks, padded/odd strides, bit-pack and RLE, float density, "
                     "64-bit PTS, CRC damage and missing-footer rejection\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
