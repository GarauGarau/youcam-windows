#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

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
    // Capture time on the system QPC clock, in 100 ns units (not delivery time).
    int64_t capturedAt = 0;
    std::vector<uint8_t> bgra;
    Frame() = default;
    Frame(uint32_t w, uint32_t h) : width(w), height(h) {
        if (!w || !h || w > 32768 || h > 32768 || uint64_t(w) * h > 268435456)
            throw std::runtime_error("Unsupported frame size.");
        bgra.resize(size_t(w) * h * 4);
    }
};
struct Crop {
    double left = 0, top = 0, right = 1, bottom = 1;
    bool full() const { return left == 0 && top == 0 && right == 1 && bottom == 1; }
};
struct CameraOverlay {
    bool enabled = false;
    int shape = 0;
    bool mirror = true;
    Crop crop;
    // Position in the available travel area, independent of desktop coordinates.
    double x = 1, y = 1, size = .24;
};
inline Crop validCrop(Crop c) {
    if (!std::isfinite(c.left) || !std::isfinite(c.top) || !std::isfinite(c.right) ||
        !std::isfinite(c.bottom) || c.right <= c.left || c.bottom <= c.top)
        return {};
    c.left = std::clamp(c.left, 0.0, 0.98);
    c.top = std::clamp(c.top, 0.0, 0.98);
    c.right = std::clamp(c.right, c.left + 0.02, 1.0);
    c.bottom = std::clamp(c.bottom, c.top + 0.02, 1.0);
    return c;
}
inline Crop mirroredCrop(Crop c) { return {1 - c.right, c.top, 1 - c.left, c.bottom}; }
struct PixelCrop { uint32_t x, y, width, height; };
inline PixelCrop cropPixels(uint32_t width, uint32_t height, Crop crop) {
    crop = validCrop(crop);
    uint32_t x = std::min(width - 1, uint32_t(std::lround(crop.left * width))),
             y = std::min(height - 1, uint32_t(std::lround(crop.top * height)));
    uint32_t right = std::clamp(uint32_t(std::lround(crop.right * width)), x + 1, width),
             bottom = std::clamp(uint32_t(std::lround(crop.bottom * height)), y + 1, height);
    return {x, y, right - x, bottom - y};
}
inline Frame cropFrame(const Frame &input, Crop crop) {
    auto r = cropPixels(input.width, input.height, crop);
    Frame out(r.width, r.height);
    out.capturedAt = input.capturedAt;
    for (uint32_t y = 0; y < r.height; ++y)
        std::copy_n(&input.bgra[(size_t(y + r.y) * input.width + r.x) * 4], size_t(r.width) * 4,
                    &out.bgra[size_t(y) * r.width * 4]);
    return out;
}
// Fit is used only for a resized window. Screen and camera recordings retain
// every source pixel; an odd edge is duplicated to satisfy 4:2:0 encoders.
inline Frame fitFrame(const Frame &input, uint32_t width, uint32_t height, bool mirror = false) {
    Frame out(width, height);
    out.capturedAt = input.capturedAt;
    if (width >= input.width && height >= input.height && width - input.width <= 1 &&
        height - input.height <= 1) {
        for (uint32_t y = 0; y < height; ++y) {
            auto sy = std::min(y, input.height - 1);
            if (!mirror) {
                const auto row = input.bgra.data() + size_t(sy) * input.width * 4;
                auto destination = out.bgra.data() + size_t(y) * width * 4;
                std::copy_n(row, size_t(input.width) * 4, destination);
                if (width > input.width)
                    std::copy_n(row + size_t(input.width - 1) * 4, 4,
                                destination + size_t(input.width) * 4);
                continue;
            }
            for (uint32_t x = 0; x < width; ++x) {
                auto sx = std::min(x, input.width - 1);
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
    for (uint32_t y = 0; y < dh; ++y) {
        auto sy = std::min(input.height - 1, uint32_t(uint64_t(y) * input.height / dh));
        for (uint32_t x = 0; x < dw; ++x) {
            auto sx = std::min(input.width - 1, uint32_t(uint64_t(x) * input.width / dw));
            if (mirror)
                sx = input.width - 1 - sx;
            std::copy_n(&input.bgra[(size_t(sy) * input.width + sx) * 4], 4,
                        &out.bgra[(size_t(y + oy) * width + x + ox) * 4]);
        }
    }
    return out;
}
// Convert decoded camera NV12 directly. The Source Reader's basic RGB converter
// is too slow for live 4K on some machines. SSE2 is part of the x64 baseline.
inline void fromNV12(Frame &out, const uint8_t *pixels, size_t stride,
                     bool bt709 = false, bool fullRange = false) {
    if ((out.width | out.height) & 1 || stride < out.width)
        throw std::runtime_error("Invalid camera NV12 layout.");
    const int cy = fullRange ? 256 : 298, offset = fullRange ? 0 : 16;
    const int cr = bt709 ? (fullRange ? 403 : 459) : (fullRange ? 359 : 409);
    const int cb = bt709 ? (fullRange ? 475 : 541) : (fullRange ? 454 : 516);
    const int gu = bt709 ? (fullRange ? -48 : -55) : (fullRange ? -88 : -100);
    const int gv = bt709 ? (fullRange ? -120 : -136) : (fullRange ? -183 : -208);
    for (uint32_t y = 0; y < out.height; ++y) {
        const auto luma = pixels + size_t(y) * stride;
        const auto chroma = pixels + size_t(out.height + y / 2) * stride;
        auto destination = out.bgra.data() + size_t(y) * out.width * 4;
        uint32_t x = 0;
#if defined(_M_X64) || defined(__SSE2__)
        const auto zero = _mm_setzero_si128();
        auto pair = [](int a, int b) { return _mm_set1_epi32(int(uint32_t(uint16_t(a)) | (uint32_t(uint16_t(b)) << 16))); };
        for (; x + 8 <= out.width; x += 8) {
            auto yy = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(
                reinterpret_cast<const __m128i *>(luma + x)), zero), _mm_set1_epi16(short(offset)));
            auto uv = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(
                reinterpret_cast<const __m128i *>(chroma + x)), zero), _mm_set1_epi16(128));
            auto u = _mm_shufflehi_epi16(_mm_shufflelo_epi16(uv, _MM_SHUFFLE(2, 2, 0, 0)), _MM_SHUFFLE(2, 2, 0, 0));
            auto v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(uv, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
            auto channel = [&](int uc, int vc) {
                auto low = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(yy, u), pair(cy, uc)),
                                         _mm_madd_epi16(_mm_unpacklo_epi16(v, zero), pair(vc, 0)));
                auto high = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(yy, u), pair(cy, uc)),
                                          _mm_madd_epi16(_mm_unpackhi_epi16(v, zero), pair(vc, 0)));
                low = _mm_srai_epi32(_mm_add_epi32(low, _mm_set1_epi32(128)), 8);
                high = _mm_srai_epi32(_mm_add_epi32(high, _mm_set1_epi32(128)), 8);
                return _mm_packus_epi16(_mm_packs_epi32(low, high), zero);
            };
            auto bg = _mm_unpacklo_epi8(channel(cb, 0), channel(gu, gv));
            auto ra = _mm_unpacklo_epi8(channel(0, cr), _mm_set1_epi8(-1));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + size_t(x) * 4), _mm_unpacklo_epi16(bg, ra));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + size_t(x) * 4 + 16), _mm_unpackhi_epi16(bg, ra));
        }
#endif
        for (; x < out.width; ++x) {
            int yy = int(luma[x]) - offset, u = int(chroma[x & ~1u]) - 128,
                v = int(chroma[(x & ~1u) + 1]) - 128;
            auto p = destination + size_t(x) * 4;
            p[0] = uint8_t(std::clamp((cy * yy + cb * u + 128) >> 8, 0, 255));
            p[1] = uint8_t(std::clamp((cy * yy + gu * u + gv * v + 128) >> 8, 0, 255));
            p[2] = uint8_t(std::clamp((cy * yy + cr * v + 128) >> 8, 0, 255));
            p[3] = 255;
        }
    }
}
inline void toNV12(const Frame &frame, std::vector<uint8_t> &output) {
    const auto w = frame.width, h = frame.height;
    if ((w | h) & 1)
        throw std::runtime_error("NV12 requires even dimensions.");
    // Every byte is written below; retain storage across recording frames.
    output.resize(size_t(w) * h * 3 / 2);
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
}
inline std::vector<uint8_t> toNV12(const Frame &frame) {
    std::vector<uint8_t> output;
    toNV12(frame, output);
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
