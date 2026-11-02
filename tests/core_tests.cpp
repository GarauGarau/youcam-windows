#include "core.h"
#include <iostream>
using namespace youcam;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        std::vector<VideoMode> formats{
            {1920, 1080, 120, 1}, {3840, 2160, 15, 1}, {3840, 2160, 30, 1}, {1280, 720, 240, 1}};
        auto mode = maximumMode(formats);
        require(mode.width == 3840 && mode.height == 2160 && mode.rateNumerator == 30,
                "Maximum resolution must outrank frame rate.");
        require(evenExtent(7680) == 7680 && evenExtent(4320) == 4320,
                "8K must not be reduced to 4096 pixels.");
        require(evenExtent(5121) == 5122 && evenExtent(1441) == 1442,
                "Odd dimensions must be padded, not cropped.");
        Frame camera(3, 3);
        camera.capturedAt = 1234567;
        for (size_t i = 0; i < camera.bgra.size(); ++i)
            camera.bgra[i] = uint8_t(i);
        auto padded = fitFrame(camera, 4, 4);
        require(padded.capturedAt == camera.capturedAt && cropFrame(camera, {}).capturedAt == camera.capturedAt,
                "Crop and fit must preserve capture timestamps for lip sync.");
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 3; ++x)
                require(padded.bgra[(y * 4 + x) * 4] == camera.bgra[(y * 3 + x) * 4],
                        "All native camera pixels must survive padding.");
        require(padded.bgra[(3 * 4 + 3) * 4] == camera.bgra[(2 * 3 + 2) * 4],
                "Padding repeats the last edge pixel.");
        auto mirrored = fitFrame(camera, 4, 4, true);
        require(mirrored.bgra[0] == camera.bgra[8], "Mirroring must preserve full source width.");
        Frame grid(8, 6);
        for (uint32_t y = 0; y < grid.height; ++y)
            for (uint32_t x = 0; x < grid.width; ++x)
                grid.bgra[(y * grid.width + x) * 4] = uint8_t(y * 10 + x);
        Crop selection{.25, 1.0 / 6, .75, 5.0 / 6};
        auto cropped = cropFrame(grid, selection);
        require(cropped.width == 4 && cropped.height == 4 && cropped.bgra[0] == 12 &&
                cropped.bgra[(3 * 4 + 3) * 4] == 45, "Crop must preserve the selected native pixels.");
        auto cropMirror = fitFrame(cropped, 4, 4, true);
        require(cropMirror.bgra[0] == 15, "Mirror must operate within the selected crop.");
        auto full = cropFrame(grid, {});
        require(full.bgra == grid.bgra, "Full frame must restore every original pixel.");
        Frame landscape(65, 37); // Cross the rotation tile boundary in both axes.
        landscape.capturedAt = 7654321;
        for (size_t i = 0; i < landscape.bgra.size(); ++i) landscape.bgra[i] = uint8_t(i * 71);
        for (int turn : {1, 2, 3}) {
            auto rotated = rotateFrame(landscape, turn);
            require(rotated.width == (turn == 2 ? 65u : 37u) &&
                    rotated.height == (turn == 2 ? 37u : 65u) && rotated.capturedAt == landscape.capturedAt,
                    "Rotation must preserve full resolution and the audio synchronization timestamp.");
            require(rotateFrame(rotated, 4 - turn).bgra == landscape.bgra,
                    "Rotation must preserve every channel of every pixel, including odd edges.");
        }
        auto clockwise = rotateFrame(grid, 1), counterclockwise = rotateFrame(grid, 3);
        require(clockwise.bgra[0] == 50 && clockwise.bgra[5 * 4] == 0 &&
                clockwise.bgra[7 * 6 * 4] == 57 && counterclockwise.bgra[0] == 7 &&
                counterclockwise.bgra[5 * 4] == 57, "Quarter turns must have the correct direction.");
        auto portraitCrop = cropFrame(clockwise, {1.0 / 6, .25, 5.0 / 6, .75});
        require(portraitCrop.width == 4 && portraitCrop.height == 4 && portraitCrop.bgra[0] == 42 &&
                portraitCrop.bgra[15 * 4] == 15, "Portrait crops must use coordinates after rotation.");
        require(portraitTurns(3840, 2160) == 1 && portraitTurns(2160, 3840) == 0 &&
                portraitTurns(3840, 2160, true) == 3 && portraitTurns(2160, 3840, true) == 2 &&
                portraitTurns(720, 720) == 0, "Native portrait frames must not be rotated a second time.");
        auto flipped = mirroredCrop({0, .2, .25, .8});
        require(flipped.left == .75 && flipped.right == 1 && flipped.top == .2,
                "A mirrored selection must map back to the original sensor coordinates.");
        auto edge = cropPixels(3840, 2160, {.999, .999, 1, 1});
        require(edge.x + edge.width <= 3840 && edge.y + edge.height <= 2160 && edge.width && edge.height,
                "Edge crops must stay inside the image.");
        Frame black(4, 4);
        auto nv12 = toNV12(black);
        require(nv12.size() == 24 && nv12[0] == 16 && nv12[16] == 128, "NV12 limited-range black is wrong.");
        Frame white(4, 4);
        std::fill(white.bgra.begin(), white.bgra.end(), uint8_t(255));
        auto encoded = toNV12(white);
        require(encoded[0] >= 234 && encoded[0] <= 235, "NV12 white level is wrong.");
        // Exercise SIMD lanes, scalar tail, row padding and unaligned input.
        for (bool bt709 : {false, true}) for (bool fullRange : {false, true}) {
            constexpr size_t stride = 18;
            Frame decoded(10, 6);
            std::vector<uint8_t> input(stride * 9 + 1);
            for (size_t i = 0; i < input.size(); ++i) input[i] = uint8_t(i * 97 + 11);
            fromNV12(decoded, input.data() + 1, stride, bt709, fullRange);
            int cy = fullRange ? 256 : 298, offset = fullRange ? 0 : 16;
            int cr = bt709 ? (fullRange ? 403 : 459) : (fullRange ? 359 : 409);
            int cb = bt709 ? (fullRange ? 475 : 541) : (fullRange ? 454 : 516);
            int gu = bt709 ? (fullRange ? -48 : -55) : (fullRange ? -88 : -100);
            int gv = bt709 ? (fullRange ? -120 : -136) : (fullRange ? -183 : -208);
            for (uint32_t y = 0; y < decoded.height; ++y) for (uint32_t x = 0; x < decoded.width; ++x) {
                int yy = int(input[1 + y * stride + x]) - offset;
                int u = int(input[1 + (6 + y / 2) * stride + (x & ~1u)]) - 128;
                int v = int(input[1 + (6 + y / 2) * stride + (x & ~1u) + 1]) - 128;
                const auto p = &decoded.bgra[(y * decoded.width + x) * 4];
                require(p[0] == std::clamp((cy * yy + cb * u + 128) >> 8, 0, 255) &&
                        p[1] == std::clamp((cy * yy + gu * u + gv * v + 128) >> 8, 0, 255) &&
                        p[2] == std::clamp((cy * yy + cr * v + 128) >> 8, 0, 255) && p[3] == 255,
                        "SIMD camera conversion differs from scalar reference.");
            }
        }
        Frame roundTrip(4, 4);
        std::vector<uint8_t> neutralWhite(24, 128);
        std::fill_n(neutralWhite.begin(), 16, uint8_t(235));
        fromNV12(roundTrip, neutralWhite.data(), 4, true);
        require(roundTrip.bgra[0] >= 254 && roundTrip.bgra[1] >= 254 && roundTrip.bgra[2] >= 254,
                "Camera conversion must preserve white without a color cast.");
        Timeline time;
        time.start(100);
        time.pause(400);
        require(time.time(800) == 300, "Pause must freeze movie time.");
        time.resume(900);
        require(time.time(1100) == 500, "Paused time must not enter the movie.");
        time.pause(1200);
        time.resume(1600);
        require(time.time(1800) == 800, "Multiple pauses must preserve a continuous timeline.");
        std::cout << "PASS: maximum camera mode, 8K extents, odd-edge pixel preservation, mirror, NV12, "
                     "pause timeline.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
