#include "visuals.h"
#include <iostream>
using namespace youcam;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void saveBitmap(const Frame &frame, const std::filesystem::path &path) {
    Gdiplus::Bitmap bitmap(frame.width, frame.height, int(frame.width * 4), PixelFormat32bppPARGB,
                           const_cast<BYTE *>(frame.bgra.data()));
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> memory(bytes);
    auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo *>(memory.data());
    Gdiplus::GetImageEncoders(count, bytes, encoders);
    for (UINT i = 0; i < count; ++i)
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            require(bitmap.Save(path.c_str(), &encoders[i].Clsid) == Gdiplus::Ok, "Cannot save render.");
            return;
        }
    throw std::runtime_error("PNG encoder missing.");
}
int wmain(int argc, wchar_t **argv) {
    try {
        Drawing drawing;
        Frame camera(640, 360);
        for (uint32_t y = 0; y < camera.height; ++y)
            for (uint32_t x = 0; x < camera.width; ++x) {
                auto p = &camera.bgra[(size_t(y) * camera.width + x) * 4];
                p[0] = 65; p[1] = uint8_t(60 + x / 4); p[2] = uint8_t(95 + y / 3); p[3] = 255;
                if (x % 80 < 2 || y % 60 < 2) p[0] = p[1] = p[2] = 225;
            }
        for (int shape = 0; shape < 4; ++shape) {
            auto tile = cameraTile(&camera, 240, shape == 0 || shape == 1 ? 240 : 135, shape, {}, false);
            size_t translucent = 0;
            for (size_t i = 0; i < tile.bgra.size(); i += 4) {
                auto alpha = tile.bgra[i + 3];
                require(tile.bgra[i] <= alpha && tile.bgra[i + 1] <= alpha && tile.bgra[i + 2] <= alpha,
                        "Layered camera pixels must be premultiplied.");
                if (alpha > 0 && alpha < 200) ++translucent;
            }
            require(translucent > 1000 && tile.bgra[3] == 0, "Every shape needs a soft transparent shadow.");
            if (argc > 1) saveBitmap(tile, std::filesystem::path(argv[1]) / (L"shape-" + std::to_wstring(shape) + L".png"));
        }
        auto terrain = topography(1280, 800, 0);
        auto moved = topography(1280, 800, 8);
        require(terrain.bgra != moved.bgra, "Topographic background must animate.");
        for (size_t i = 0; i < terrain.bgra.size(); i += 4)
            require(terrain.bgra[i] == terrain.bgra[i + 1] && terrain.bgra[i + 1] == terrain.bgra[i + 2] &&
                    terrain.bgra[i + 3] == 255, "Stage background must be monochrome and opaque.");
        RECT card = stageCard(1280, 800, .68, &camera, {});
        require(card.left > 0 && card.top > 0 && std::abs(card.left + card.right - 1280) <= 1 &&
                std::abs(card.top + card.bottom - 800) <= 1,
                "Stage card must be centered with visible margins.");
        auto cropped = stageCard(1280, 800, .68, &camera, {.25, 0, .75, 1});
        require(rectWidth(cropped) < rectWidth(card), "Tall crops must fit within stage margins.");
        auto enlarged = stageCard(1280, 800, StageMaximum, &camera, {});
        require(rectWidth(enlarged) > 1200 && enlarged.left > 0 && enlarged.right < 1280,
                "Stage must allow a nearly full-width camera with a visible margin.");
        Frame background(1280, 800);
        for (size_t i = 0; i < background.bgra.size(); i += 4) {
            background.bgra[i] = 240;
            background.bgra[i + 1] = 240;
            background.bgra[i + 2] = 240;
            background.bgra[i + 3] = 255;
        }
        CameraOverlay overlay{true, 2, true, {.25, .2, .75, .8}, 1, 1, .3};
        auto combined = composeWindow(background, camera, overlay);
        camera.capturedAt = 123456789;
        require(composeWindow(background, camera, overlay).capturedAt == camera.capturedAt &&
                composeStage(terrain, &camera, {}, false, .68).capturedAt == camera.capturedAt,
                "Compositions must retain the camera timestamp for audio synchronization.");
        auto cameraRect = windowCameraRect(1280, 800, overlay);
        size_t outside = (size_t(20) * 1280 + 20) * 4;
        require(combined.width == 1280 && combined.height == 800 &&
                combined.bgra[outside] == background.bgra[outside],
                "Window composition must keep native dimensions and unrelated pixels.");
        size_t center = (size_t((cameraRect.top + cameraRect.bottom) / 2 + 13) * 1280 +
                        (cameraRect.left + cameraRect.right) / 2 + 11) * 4;
        require(combined.bgra[center] < 100, "Camera pixels must appear inside the recorded window.");
        size_t shadow = (size_t(cameraRect.bottom + 5) * 1280 +
                        (cameraRect.left + cameraRect.right) / 2) * 4;
        require(combined.bgra[shadow] > 50 && combined.bgra[shadow] < 230,
                "The recorded camera must cast a translucent shadow on window content.");
        auto unmirrored = overlay;
        unmirrored.mirror = false;
        auto flipped = composeWindow(background, camera, unmirrored);
        size_t left = (size_t((cameraRect.top + cameraRect.bottom) / 2 + 13) * 1280 + cameraRect.left + 45) * 4;
        require(combined.bgra[left + 1] > flipped.bgra[left + 1] + 20,
                "Window camera mirroring must apply inside the selected crop.");
        auto disabled = overlay;
        disabled.enabled = false;
        require(composeWindow(background, camera, disabled).bgra == background.bgra,
                "Disabling the webcam must preserve the window frame exactly.");
        auto corner = overlay;
        corner.x = corner.y = 0;
        auto movedRect = windowCameraRect(1280, 800, corner);
        require(movedRect.left < cameraRect.left && movedRect.top < cameraRect.top,
                "Webcam placement must support moving to a different corner.");
        Frame largeWindow(3840, 2160);
        auto largeComposition = composeWindow(largeWindow, camera, overlay);
        require(largeComposition.width == 3840 && largeComposition.height == 2160,
                "Window + webcam composition must preserve 4K resolution.");
        if (argc > 1) {
            saveBitmap(combined, std::filesystem::path(argv[1]) / L"window-camera.png");
            saveBitmap(composeStage(terrain, &camera, {}, false, StageMaximum),
                       std::filesystem::path(argv[1]) / L"stage-large.png");
        }
        auto start = std::chrono::steady_clock::now();
        auto scene = composeStage(terrain, &camera, {}, false, .68);
        auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        require(scene.width == 1280 && scene.height == 800, "Stage must retain monitor dimensions.");
        if (argc > 1) saveBitmap(scene, std::filesystem::path(argv[1]) / L"stage.png");
        std::cout << "PASS: shadows, premultiplied alpha, centered crop, monochrome animation. Compose: "
                  << milliseconds << " ms.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
