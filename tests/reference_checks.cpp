#include "visuals.h"
#define youcam reference_youcam
#include YOUCAM_REFERENCE_HEADER
#undef youcam
#include <iostream>
#include <iomanip>
#include <random>
#include <string>

namespace {
uint64_t compared = 0;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void equal(const youcam::Frame &now, const reference_youcam::Frame &before) {
    require(now.width == before.width && now.height == before.height &&
            now.capturedAt == before.capturedAt && now.bgra == before.bgra,
            "Frame pixels, dimensions or capture clock changed.");
    compared += now.bgra.size();
}
std::pair<youcam::Frame, reference_youcam::Frame> frames(uint32_t w, uint32_t h) {
    youcam::Frame now(w, h);
    reference_youcam::Frame before(w, h);
    std::mt19937 random(92);
    for (auto &pixel : now.bgra) pixel = uint8_t(random());
    before.bgra = now.bgra;
    now.capturedAt = before.capturedAt = 123456789;
    return {std::move(now), std::move(before)};
}
template<class Old, class New> void benchmark(const char *name, int count, Old old, New current) {
    std::vector<double> before, after;
    for (int trial = 0; trial < 5; ++trial) {
        auto measure = [&](auto action, std::vector<double> &results) {
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < count; ++i) action();
            results.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count() / count);
        };
        if (trial % 2 == 0) { measure(old, before); measure(current, after); }
        else { measure(current, after); measure(old, before); }
    }
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    std::cout << name << ": " << before[2] << " -> " << after[2] << " ms/op ("
              << (1 - after[2] / before[2]) * 100 << "% less time)\n";
}
}
int main(int argc, char **argv) {
    try {
        youcam::Drawing drawing;
        reference_youcam::Drawing referenceDrawing;
        for (auto [w, h] : {std::pair{1u, 1u}, {3u, 3u}, {17u, 9u}, {640u, 360u},
                           {1919u, 1079u}, {3840u, 2160u}}) {
            auto [now, before] = frames(w, h);
            for (auto [dw, dh] : {std::pair{w, h}, {w + 1, h + 1},
                                  {std::max(1u, w / 2), h + 2}, {w + 7, std::max(1u, h / 2)}})
                for (bool mirror : {false, true})
                    equal(youcam::fitFrame(now, dw, dh, mirror),
                          reference_youcam::fitFrame(before, dw, dh, mirror));
        }
        std::vector<uint8_t> scratch;
        for (auto [w, h] : {std::pair{2u, 2u}, {10u, 6u}, {1920u, 1080u}, {3840u, 2160u}, {640u, 480u}}) {
            auto [now, before] = frames(w, h);
            for (int revision = 0; revision < 2; ++revision) {
                youcam::toNV12(now, scratch);
                auto reference = reference_youcam::toNV12(before);
                require(scratch == reference && youcam::toNV12(now) == reference,
                        "NV12 samples changed or reusable buffer contains stale pixels.");
                compared += reference.size();
                auto memory = scratch.data();
                youcam::toNV12(now, scratch);
                require(scratch.data() == memory, "Same-size conversion reallocates its buffer.");
                for (auto &pixel : now.bgra) pixel ^= 0xff;
                before.bgra = now.bgra;
            }
        }
        for (auto [w, h] : {std::pair{160, 90}, {1280, 800}, {1920, 1080}, {3840, 2160}})
            for (double seconds : {0., .1, 8., 1234.5})
                equal(youcam::topography(w, h, seconds), reference_youcam::topography(w, h, seconds));
        std::cout << "PASS: " << compared << " identical pixel/conversion bytes; dimensions, timestamps, "
                     "odd edges, mirror, resize, reusable buffers and terrain.\n";
        if (argc > 1 && std::string(argv[1]) == "--benchmark") {
            std::cout << std::fixed << std::setprecision(3);
            auto [now, before] = frames(3840, 2160);
            // Results remain observable; no conversion is optimized away.
            std::vector<uint8_t> oldOutput, newOutput;
            benchmark("4K NV12", 8, [&] { oldOutput = reference_youcam::toNV12(before); },
                                     [&] { youcam::toNV12(now, newOutput); });
            require(oldOutput == newOutput, "Benchmark NV12 differs.");
            youcam::Frame newFrame;
            reference_youcam::Frame oldFrame;
            benchmark("4K row copy/padding", 8,
                      [&] { oldFrame = reference_youcam::fitFrame(before, 3841, 2161); },
                      [&] { newFrame = youcam::fitFrame(now, 3841, 2161); });
            equal(newFrame, oldFrame);
            benchmark("4K terrain", 3,
                      [&] { oldFrame = reference_youcam::topography(3840, 2160, 8); },
                      [&] { newFrame = youcam::topography(3840, 2160, 8); });
            equal(newFrame, oldFrame);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
