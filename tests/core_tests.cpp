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
        for (size_t i = 0; i < camera.bgra.size(); ++i)
            camera.bgra[i] = uint8_t(i);
        auto padded = fitFrame(camera, 4, 4);
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 3; ++x)
                require(padded.bgra[(y * 4 + x) * 4] == camera.bgra[(y * 3 + x) * 4],
                        "All native camera pixels must survive padding.");
        require(padded.bgra[(3 * 4 + 3) * 4] == camera.bgra[(2 * 3 + 2) * 4],
                "Padding repeats the last edge pixel.");
        auto mirrored = fitFrame(camera, 4, 4, true);
        require(mirrored.bgra[0] == camera.bgra[8], "Mirroring must preserve full source width.");
        Frame black(4, 4);
        auto nv12 = toNV12(black);
        require(nv12.size() == 24 && nv12[0] == 16 && nv12[16] == 128, "NV12 limited-range black is wrong.");
        Frame white(4, 4);
        std::fill(white.bgra.begin(), white.bgra.end(), uint8_t(255));
        auto encoded = toNV12(white);
        require(encoded[0] >= 234 && encoded[0] <= 235, "NV12 white level is wrong.");
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
