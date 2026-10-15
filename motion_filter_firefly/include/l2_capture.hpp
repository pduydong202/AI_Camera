#pragma once

// Lossless diagnostic transport. No detection or tracking decisions live here.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace l2capture {
using Bytes = std::vector<std::uint8_t>;

struct Header {
    std::uint32_t width = 0, height = 0, cols = 0, rows = 0;
    std::uint32_t history = 0, taFrames = 0, widthThreshold = 0;
    double fps = 0.0, eventRatio = 0.0;
};

struct Meta {
    std::int64_t ptsNs = 0;
    std::uint32_t warmup = 0, motion = 0, l1Pixels = 0, rainPixels = 0;
    std::uint32_t validPixels = 0, repeatCells = 0, repeatPixels = 0, repeatComponents = 0;
    float otsu = 0.0F;
};

template <class T> struct Plane {
    const T* data;
    std::size_t stride;
};

struct Frame {
    std::uint64_t index = 0;
    Meta meta;
    std::vector<float> density;
    std::array<Bytes, 3> masks; // L1, native L3 rain evidence, native VALID.
};

inline void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(std::string("L2 capture: ") + message);
}

inline void validate(const Header& h) {
    require(h.width && h.height && h.cols && h.rows && h.history && h.taFrames && h.widthThreshold,
            "invalid dimensions/history");
    require(std::uint64_t(h.width) * h.height <= std::numeric_limits<std::uint32_t>::max(),
            "frame exceeds format limit");
    require(std::uint64_t(h.cols) * h.rows <= 1000000, "grid exceeds format limit");
    require(std::isfinite(h.fps) && h.fps > 0 && std::isfinite(h.eventRatio) && h.eventRatio > 0 &&
                h.eventRatio <= 1,
            "invalid fps/event ratio");
}

inline void integer(Bytes& b, std::uint64_t n, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        b.push_back(static_cast<std::uint8_t>(n >> (i * 8U)));
}

inline void f32(Bytes& b, float f) {
    std::uint32_t n;
    std::memcpy(&n, &f, 4);
    integer(b, n, 4);
}

inline void f64(Bytes& b, double f) {
    std::uint64_t n;
    std::memcpy(&n, &f, 8);
    integer(b, n, 8);
}

inline void varint(Bytes& b, std::uint32_t n) {
    do {
        b.push_back(static_cast<std::uint8_t>((n & 127U) | (n >= 128U ? 128U : 0U)));
        n >>= 7U;
    } while (n);
}

inline std::uint32_t crc32(const Bytes& b) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (unsigned i = 0; i < t.size(); ++i) {
            auto c = i;
            for (unsigned k = 0; k < 8; ++k)
                c = (c >> 1U) ^ ((c & 1U) ? 0xedb88320U : 0U);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xffffffffU;
    for (auto v : b)
        crc = table[(crc ^ v) & 255U] ^ (crc >> 8U);
    return crc ^ 0xffffffffU;
}

struct Cursor {
    const Bytes& b;
    std::size_t pos = 0;

    std::uint64_t number(unsigned size) {
        require(size <= 8 && pos + size <= b.size(), "truncated record");
        std::uint64_t n = 0;
        for (unsigned i = 0; i < size; ++i)
            n |= std::uint64_t(b[pos++]) << (i * 8U);
        return n;
    }

    std::uint32_t u32() {
        return static_cast<std::uint32_t>(number(4));
    }

    float float32() {
        const auto n = u32();
        float f;
        std::memcpy(&f, &n, 4);
        return f;
    }

    double float64() {
        const auto n = number(8);
        double f;
        std::memcpy(&f, &n, 8);
        return f;
    }

    std::uint32_t variable() {
        std::uint64_t n = 0;
        for (unsigned s = 0; s <= 28; s += 7) {
            const auto v = number(1);
            n |= (v & 127U) << s;
            if (!(v & 128U)) {
                require(n <= UINT32_MAX, "run length overflow");
                return static_cast<std::uint32_t>(n);
            }
        }
        throw std::runtime_error("L2 capture: invalid run length");
    }
};

inline std::uint32_t encodeMask(Bytes& out, const Header& h, Plane<std::uint8_t> p) {
    require(p.data && p.stride >= h.width, "invalid mask stride");
    const auto pixels = std::size_t(h.width) * h.height;
    Bytes packed((pixels + 7) / 8, 0), runs;
    bool previous = false;
    std::uint32_t length = 0, white = 0;
    std::size_t index = 0;
    for (unsigned y = 0; y < h.height; ++y)
        for (unsigned x = 0; x < h.width; ++x, ++index) {
            const auto value = p.data[y * p.stride + x];
            require(value == 0 || value == 255, "non-binary mask");
            const bool on = value != 0;
            if (on) {
                packed[index / 8] |= std::uint8_t(1U << (index % 8));
                ++white;
            }
            if (on == previous)
                ++length;
            else {
                varint(runs, length);
                length = 1;
                previous = on;
            }
        }
    varint(runs, length);
    const bool useRuns = runs.size() < packed.size();
    const auto& encoded = useRuns ? runs : packed;
    integer(out, useRuns ? 1 : 0, 4);
    integer(out, encoded.size(), 4);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return white;
}

inline Bytes decodeMask(Cursor& c, const Header& h) {
    const auto kind = c.u32(), size = c.u32();
    const auto pixels = std::size_t(h.width) * h.height;
    require(size <= (pixels + 7) / 8 && c.pos + size <= c.b.size(), "invalid mask length");
    Bytes data(c.b.begin() + static_cast<std::ptrdiff_t>(c.pos),
               c.b.begin() + static_cast<std::ptrdiff_t>(c.pos + size));
    c.pos += size;
    Bytes mask(pixels, 0);
    if (kind == 0) {
        require(size == (pixels + 7) / 8, "invalid packed mask");
        for (std::size_t i = 0; i < pixels; ++i)
            mask[i] = ((data[i / 8] >> (i % 8)) & 1U) ? 255 : 0;
        if (pixels % 8)
            require((data.back() >> (pixels % 8)) == 0, "nonzero mask padding");
    } else {
        require(kind == 1, "unknown mask encoding");
        Cursor r{data};
        std::size_t position = 0;
        bool on = false, first = true;
        while (r.pos < data.size()) {
            const auto count = r.variable();
            require((first || count > 0) && count <= pixels - position, "invalid mask run");
            if (on)
                std::fill(mask.begin() + static_cast<std::ptrdiff_t>(position),
                          mask.begin() + static_cast<std::ptrdiff_t>(position + count), 255);
            position += count;
            on = !on;
            first = false;
        }
        require(position == pixels, "incomplete mask runs");
    }
    return mask;
}

inline std::uint32_t white(const Bytes& b) {
    std::uint32_t count = 0;
    for (auto v : b)
        count += v != 0;
    return count;
}

class Writer {
  public:
    explicit Writer(const std::string& path) {
        output_.exceptions(std::ios::badbit | std::ios::failbit);
        output_.open(path, std::ios::binary | std::ios::trunc);
    }

    void begin(const Header& h) {
        require(!started_, "duplicate header");
        validate(h);
        header_ = h;
        static_assert(sizeof(float) == 4 && sizeof(double) == 8 &&
                          std::numeric_limits<float>::is_iec559 &&
                          std::numeric_limits<double>::is_iec559,
                      "IEEE floats required");
        output_.write("FFL2T001", 8);
        Bytes b;
        for (auto v : {h.width, h.height, h.cols, h.rows, h.history, h.taFrames, h.widthThreshold})
            integer(b, v, 4);
        f64(b, h.fps);
        f64(b, h.eventRatio);
        write(b);
        started_ = true;
    }

    void append(const Meta& m, Plane<float> density,
                const std::array<Plane<std::uint8_t>, 3>& masks) {
        require(started_ && !finished_, "writer not active");
        require(density.data && density.stride >= header_.cols, "invalid density stride");
        Bytes b;
        integer(b, frames_, 8);
        integer(b, static_cast<std::uint64_t>(m.ptsNs), 8);
        for (auto v : {m.warmup, m.motion, m.l1Pixels, m.rainPixels, m.validPixels, m.repeatCells,
                       m.repeatPixels, m.repeatComponents})
            integer(b, v, 4);
        f32(b, m.otsu);
        for (unsigned y = 0; y < header_.rows; ++y)
            for (unsigned x = 0; x < header_.cols; ++x) {
                const float d = density.data[y * density.stride + x];
                require(std::isfinite(d) && d >= 0 && d <= 1.00001F, "invalid density");
                f32(b, d);
            }
        const std::array<std::uint32_t, 3> counts{m.l1Pixels, m.rainPixels, m.validPixels};
        for (unsigned i = 0; i < 3; ++i)
            require(encodeMask(b, header_, masks[i]) == counts[i], "native mask/count mismatch");
        Bytes prefix;
        integer(prefix, b.size(), 4);
        integer(prefix, crc32(b), 4);
        write(prefix);
        write(b);
        ++frames_;
    }

    void finish() {
        require(started_ && !finished_, "capture has no header or is already finished");
        Bytes b;
        integer(b, 0, 4);
        integer(b, frames_, 8);
        write(b);
        output_.write("L2C_END1", 8);
        output_.flush();
        output_.close();
        finished_ = true;
    }

    std::uint64_t frames() const {
        return frames_;
    }

  private:
    void write(const Bytes& b) {
        output_.write(reinterpret_cast<const char*>(b.data()),
                      static_cast<std::streamsize>(b.size()));
    }

    std::ofstream output_;
    Header header_;
    bool started_ = false, finished_ = false;
    std::uint64_t frames_ = 0;
};

class Reader {
  public:
    explicit Reader(const std::string& path) : input_(path, std::ios::binary) {
        require(input_.is_open(), "cannot open capture");
        const auto magic = read(8);
        require(std::string(magic.begin(), magic.end()) == "FFL2T001", "unknown file format");
        const auto h = read(44);
        Cursor c{h};
        header.width = c.u32();
        header.height = c.u32();
        header.cols = c.u32();
        header.rows = c.u32();
        header.history = c.u32();
        header.taFrames = c.u32();
        header.widthThreshold = c.u32();
        header.fps = c.float64();
        header.eventRatio = c.float64();
        validate(header);
    }

    bool next(Frame& frame) {
        if (complete_)
            return false;
        const auto prefix = read(4);
        Cursor p{prefix};
        const auto size = p.u32();
        if (size == 0) {
            const auto footer = read(16);
            Cursor f{footer};
            require(f.number(8) == frames_, "footer frame count mismatch");
            require(std::string(footer.begin() + 8, footer.end()) == "L2C_END1", "invalid footer");
            require(input_.peek() == std::char_traits<char>::eof(),
                    "unexpected bytes after footer");
            complete_ = true;
            return false;
        }
        const auto maximum = 3 * ((std::uint64_t(header.width) * header.height + 7) / 8) +
                             std::uint64_t(header.cols) * header.rows * 4 + 256;
        require(size <= maximum, "record exceeds geometry bound");
        const auto crc = read(4);
        Cursor checksum{crc};
        const auto data = read(size);
        require(checksum.u32() == crc32(data), "record CRC mismatch");
        Cursor c{data};
        frame.index = c.number(8);
        require(frame.index == frames_, "missing/out-of-order analysis frame");
        const auto pts = c.number(8);
        std::memcpy(&frame.meta.ptsNs, &pts, 8);
        auto& m = frame.meta;
        m.warmup = c.u32();
        m.motion = c.u32();
        m.l1Pixels = c.u32();
        m.rainPixels = c.u32();
        m.validPixels = c.u32();
        m.repeatCells = c.u32();
        m.repeatPixels = c.u32();
        m.repeatComponents = c.u32();
        m.otsu = c.float32();
        require(m.warmup <= 1 && m.motion <= 1, "invalid frame flags");
        frame.density.resize(std::size_t(header.cols) * header.rows);
        for (auto& d : frame.density) {
            d = c.float32();
            require(std::isfinite(d) && d >= 0 && d <= 1.00001F, "invalid density");
        }
        const std::array<std::uint32_t, 3> counts{m.l1Pixels, m.rainPixels, m.validPixels};
        for (unsigned i = 0; i < 3; ++i) {
            frame.masks[i] = decodeMask(c, header);
            require(white(frame.masks[i]) == counts[i], "native mask/count mismatch");
        }
        require(c.pos == data.size(), "unexpected bytes in record");
        ++frames_;
        return true;
    }

    bool complete() const {
        return complete_;
    }

    std::uint64_t frames() const {
        return frames_;
    }

    Header header;

  private:
    Bytes read(std::size_t size) {
        Bytes b(size);
        require(bool(input_.read(reinterpret_cast<char*>(b.data()),
                                 static_cast<std::streamsize>(size))),
                "truncated capture (missing complete footer/record)");
        return b;
    }

    std::ifstream input_;
    bool complete_ = false;
    std::uint64_t frames_ = 0;
};
} // namespace l2capture
