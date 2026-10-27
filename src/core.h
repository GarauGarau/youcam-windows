#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace youcam {
struct VideoMode {
    uint32_t width = 0, height = 0, rateNumerator = 30, rateDenominator = 1;
    uint64_t pixels() const {
        return uint64_t(width) * height;
    }
};
inline bool betterMode(const VideoMode &a, const VideoMode &b) {
    if (a.pixels() != b.pixels())
        return a.pixels() > b.pixels();
    return uint64_t(a.rateNumerator) * b.rateDenominator > uint64_t(b.rateNumerator) * a.rateDenominator;
}
inline VideoMode maximumMode(const std::vector<VideoMode> &modes) {
    VideoMode best;
    for (const auto &mode : modes)
        if (mode.width && mode.height && mode.rateDenominator && betterMode(mode, best))
            best = mode;
    if (!best.pixels())
        throw std::runtime_error("No camera video formats are available.");
    return best;
}
inline uint32_t evenExtent(uint32_t value) {
    if (!value || value > 32768)
        throw std::runtime_error("Unsupported frame size.");
    // Pad odd dimensions. Cropping would discard a row or column of source pixels.
    return (value + 1u) & ~1u;
}
struct Frame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
    Frame() = default;
    Frame(uint32_t w, uint32_t h) : width(w), height(h) {
        if (!w || !h || w > 32768 || h > 32768 || uint64_t(w) * h > 268435456)
            throw std::runtime_error("Unsupported frame size.");
        bgra.resize(size_t(w) * h * 4);
    }
};
// Fit is used only for a resized window. Screen and camera recordings retain
// every source pixel; an odd edge is duplicated to satisfy 4:2:0 encoders.
inline Frame fitFrame(const Frame &input, uint32_t width, uint32_t height, bool mirror = false) {
    Frame out(width, height);
    if (width >= input.width && height >= input.height && width - input.width <= 1 &&
        height - input.height <= 1) {
        for (uint32_t y = 0; y < height; ++y) {
            auto sy = std::min(y, input.height - 1);
            for (uint32_t x = 0; x < width; ++x) {
                auto sx = std::min(x, input.width - 1);
                if (mirror)
                    sx = input.width - 1 - sx;
                std::copy_n(&input.bgra[(size_t(sy) * input.width + sx) * 4], 4,
                            &out.bgra[(size_t(y) * width + x) * 4]);
            }
        }
        return out;
    }
    double scale = std::min(double(width) / input.width, double(height) / input.height);
    auto dw = std::max(1u, uint32_t(input.width * scale));
    auto dh = std::max(1u, uint32_t(input.height * scale));
    uint32_t ox = (width - dw) / 2, oy = (height - dh) / 2;
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x) {
            auto sx = std::min(input.width - 1, uint32_t(uint64_t(x) * input.width / dw));
            auto sy = std::min(input.height - 1, uint32_t(uint64_t(y) * input.height / dh));
            if (mirror)
                sx = input.width - 1 - sx;
            std::copy_n(&input.bgra[(size_t(sy) * input.width + sx) * 4], 4,
                        &out.bgra[(size_t(y + oy) * width + x + ox) * 4]);
        }
    return out;
}
inline std::vector<uint8_t> toNV12(const Frame &frame) {
    const auto w = frame.width, h = frame.height;
    if ((w | h) & 1)
        throw std::runtime_error("NV12 requires even dimensions.");
    std::vector<uint8_t> output(size_t(w) * h * 3 / 2);
    auto limit = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
    // Limited-range BT.709, matching the media type passed to Media Foundation.
    for (uint32_t y = 0; y < h; y += 2)
        for (uint32_t x = 0; x < w; x += 2) {
            int rs = 0, gs = 0, bs = 0;
            for (uint32_t dy = 0; dy < 2; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const auto p = &frame.bgra[(size_t(y + dy) * w + x + dx) * 4];
                    int b = p[0], g = p[1], r = p[2];
                    output[size_t(y + dy) * w + x + dx] =
                        limit(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
                    rs += r;
                    gs += g;
                    bs += b;
                }
            int r = (rs + 2) / 4, g = (gs + 2) / 4, b = (bs + 2) / 4;
            size_t uv = size_t(w) * h + size_t(y / 2) * w + x;
            output[uv] = limit(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            output[uv + 1] = limit(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    return output;
}
class Timeline {
    int64_t start_ = 0, pausedAt_ = 0, excluded_ = 0;
    bool paused_ = false;

  public:
    void start(int64_t now) {
        start_ = now;
        pausedAt_ = excluded_ = 0;
        paused_ = false;
    }
    void pause(int64_t now) {
        if (!paused_) {
            paused_ = true;
            pausedAt_ = now;
        }
    }
    void resume(int64_t now) {
        if (paused_) {
            excluded_ += now - pausedAt_;
            paused_ = false;
        }
    }
    int64_t time(int64_t now) const {
        return std::max<int64_t>(0, (paused_ ? pausedAt_ : now) - start_ - excluded_);
    }
    bool paused() const {
        return paused_;
    }
};
} // namespace youcam
