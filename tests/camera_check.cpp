#include "camera.h"
#include <iostream>
using namespace youcam;
// Opt-in hardware check. Never opened by ctest; no images or audio are saved.
int wmain(int argc, wchar_t **argv) {
    try {
        Apartment apartment;
        check(MFStartup(MF_VERSION), "Initialize multimedia");
        auto devices = Camera::devices();
        for (const auto &device : devices)
            std::cout << "Camera: " << narrow(device.name) << std::endl;
        if (argc < 2)
            return 0;
        if (std::wstring(argv[1]) == L"--switch") {
            Camera camera;
            for (int round = 0; round < 2; ++round) {
                for (const auto &device : devices) {
                    std::cout << "Opening: " << narrow(device.name) << std::endl;
                    camera.start(device.id);
                    int count = 0;
                    std::shared_ptr<const Frame> previous;
                    for (int i = 0; i < 500 && count < 15; ++i) {
                        auto frame = camera.frames.get(true);
                        if (frame && frame != previous) { ++count; previous = frame; }
                        Sleep(20);
                    }
                    if (count < 15) throw std::runtime_error("Switch did not produce live frames.");
                    std::cout << "Received frames; switching away from " << narrow(camera.name) << std::endl;
                }
            }
            std::cout << "Closing last camera" << std::endl;
            camera.shutdown();
            MFShutdown();
            std::cout << "PASS: repeated device switching and shutdown." << std::endl;
            return 0;
        }
        for (const auto &device : devices) {
            if (device.name.find(argv[1]) == std::wstring::npos)
                continue;
            Camera camera;
            camera.start(device.id);
            std::shared_ptr<const Frame> previous;
            int received = 0;
            int64_t totalAge = 0, maxAge = 0, firstTime = 0, lastTime = 0;
            for (int i = 0; i < 1500 && received < 120; ++i) {
                auto frame = camera.frames.get(true);
                if (frame && frame != previous) {
                    ++received;
                    previous = frame;
                    auto now = clock100ns();
                    if (!firstTime) firstTime = now;
                    lastTime = now;
                    auto age = now - frame->capturedAt;
                    totalAge += age;
                    maxAge = std::max(maxAge, age);
                }
                Sleep(20);
            }
            if (received < 120)
                throw std::runtime_error("Camera did not supply 120 live frames within thirty seconds.");
            std::cout << "PASS: " << narrow(camera.name) << " " << camera.mode.width << 'x'
                      << camera.mode.height << " @ " << camera.mode.rateNumerator << '/'
                      << camera.mode.rateDenominator << " fps; " << received << " live frames.\n";
            std::cout << "DeviceId: " << narrow(device.id) << '\n';
            std::cout << "Timestamp: " << (camera.deviceTimestamps ? "driver QPC" : "delivery fallback")
                      << "; mean/max frame age: " << totalAge / received / 10000.0 << "/"
                      << maxAge / 10000.0 << " ms; observed fps: "
                      << (received - 1) * 10000000.0 / (lastTime - firstTime) << '\n';
            camera.shutdown();
            MFShutdown();
            return 0;
        }
        throw std::runtime_error("Requested camera is not connected.");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
