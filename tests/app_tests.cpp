#include "app.h"
#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
namespace youcam {
struct AppTest {
    static void require(bool value, const char *message) {
        if (!value) throw std::runtime_error(message);
    }
    static void pump(int milliseconds) {
        auto deadline = GetTickCount64() + milliseconds;
        do {
            MSG msg{};
            while (GetTickCount64() < deadline && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(5);
        } while (GetTickCount64() < deadline);
    }
    // Explicit opt-in only: never opened by ctest, no image/audio files saved.
    static void hardwareCameraSwitch() {
        auto devices = Camera::devices();
        require(devices.size() >= 2, "Two cameras are required for the switch regression.");
        const auto &first = devices[0];
        const auto &second = devices[1];
        auto path = std::filesystem::temp_directory_path() /
                    (L"YouCam-camera-switch-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
        } cleanup{path};
        require(WritePrivateProfileStringW(L"YouCam", L"Camera", second.id.c_str(), path.c_str()) != FALSE,
                "Cannot write isolated camera settings.");
        WritePrivateProfileStringW(L"YouCam", L"Microphone", L"none", path.c_str());
        auto live = [](App &app, const Device &device) {
            require(app.cameraID_ == device.id && app.cameraError_.empty(), "Camera selection failed.");
            std::shared_ptr<const Frame> previous;
            int count = 0;
            for (int i = 0; i < 300 && count < 10; ++i) {
                pump(10);
                auto frame = app.camera_.frames.get(true);
                if (frame && frame != previous) { ++count; previous = frame; }
            }
            require(count == 10, "Selected camera did not deliver live preview frames.");
            std::cout << "PASS: live app preview from " << narrow(device.name) << std::endl;
        };
        {
            App app(GetModuleHandleW(nullptr), false, path.wstring());
            live(app, second);
            for (auto device : {&first, &second, &first}) {
                auto selected = std::find_if(app.cameras_.begin(), app.cameras_.end(), [&](const Device &d) {
                    return d.id == device->id;
                });
                require(selected != app.cameras_.end(), "Camera disappeared from the app menu.");
                std::cout << "Selecting from app command: " << narrow(device->name) << std::endl;
                app.command(commands::CameraBase + int(selected - app.cameras_.begin()));
                live(app, *device);
            }
            require(app.setting(L"Camera") == first.id, "Camera selection was not saved.");
        }
        std::cout << "Reopening app with saved camera selection" << std::endl;
        {
            App app(GetModuleHandleW(nullptr), false, path.wstring());
            live(app, first);
        }
        std::cout << "PASS: app camera switching, remembered selection, restart and shutdown." << std::endl;
    }
    static void run() {
        App app(GetModuleHandleW(nullptr), true);
        app.crop_ = {};
        app.mirrored_ = true;
        app.stageScale_ = .68;
        pump(80);
        SendMessageW(app.overlay_, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(140, 140));
        pump(50);
        require(app.cropping_ && IsWindowVisible(app.cropEditor_), "Double click must open crop editor.");
        DWORD affinity = 0;
        require(GetWindowDisplayAffinity(app.cropEditor_, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
                "Crop controls must be excluded from recordings.");
        auto r = app.cropImage_;
        int w = rectWidth(r), h = rectHeight(r);
        require(w > 0 && h > 0, "Crop editor must show the complete sensor frame.");
        POINT a{r.left + w / 8, r.top + h / 4}, b{r.left + w / 2, r.top + h * 3 / 4};
        SendMessageW(app.cropEditor_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
        SendMessageW(app.cropEditor_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(b.x, b.y));
        SendMessageW(app.cropEditor_, WM_LBUTTONUP, 0, MAKELPARAM(b.x, b.y));
        SendMessageW(app.cropEditor_, WM_COMMAND, commands::CropApply, 0);
        require(!app.cropping_ && !IsWindowVisible(app.cropEditor_), "Apply must close crop editor.");
        require(std::abs(app.crop_.left - .5) < .01 && std::abs(app.crop_.right - .875) < .01 &&
                std::abs(app.crop_.top - .25) < .01 && std::abs(app.crop_.bottom - .75) < .01,
                "Mirrored drag must crop the selected sensor area.");
        auto applied = app.crop_;
        SendMessageW(app.overlay_, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(140, 140));
        pump(30);
        SendMessageW(app.cropEditor_, WM_KEYDOWN, VK_ESCAPE, 0);
        require(app.crop_.left == applied.left && app.crop_.right == applied.right,
                "Cancel must preserve the previously applied crop.");
        SendMessageW(app.overlay_, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(140, 140));
        pump(30);
        SendMessageW(app.cropEditor_, WM_COMMAND, commands::CropFull, 0);
        require(app.crop_.full(), "Full frame must remove the crop.");
        SendMessageW(app.overlay_, WM_KEYDOWN, VK_F11, 0);
        pump(120);
        require(app.source_.kind == SourceKind::Stage && IsWindowVisible(app.stage_) &&
                !IsWindowVisible(app.overlay_) && !IsWindowVisible(app.panel_),
                "F11 stage must contain only the scene, without floating controls.");
        RECT stageRect{};
        GetWindowRect(app.stage_, &stageRect);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(app.source_.monitor, &info);
        require(sameRect(stageRect, info.rcMonitor), "Stage must occupy the full physical monitor.");
        SendMessageW(app.stage_, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
        require(app.stageScale_ < .68, "Mouse wheel must resize the centered camera.");
        SendMessageW(app.stage_, WM_KEYDOWN, VK_ADD, 0);
        require(std::abs(app.stageScale_ - .68) < .001, "Plus must enlarge the stage camera.");
        SendMessageW(app.stage_, WM_COMMAND, commands::SizeBase + 3, 0);
        require(app.stageScale_ == .96, "Stage size presets must affect the stage card.");
        SendMessageW(app.stage_, WM_COMMAND, commands::StageLarger, 0);
        require(app.stageScale_ == StageMaximum, "Stage enlargement must stop at the new maximum.");
        SendMessageW(app.stage_, WM_KEYDOWN, VK_SUBTRACT, 0);
        require(app.stageScale_ < StageMaximum, "Minus must shrink the stage camera.");
        SendMessageW(app.stage_, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(400, 300));
        pump(30);
        require(app.cropping_, "Stage double click must also open the crop editor.");
        SendMessageW(app.cropEditor_, WM_COMMAND, commands::CropCancel, 0);
        if (winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
        auto path = std::filesystem::temp_directory_path() /
                    (L"YouCam-stage-test-" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
        app.recorder_.start(app.source_, &app.stageFrames_, false, path);
        for (int i = 0; i < 100 && app.recorder_.state().phase == Phase::Starting; ++i) pump(25);
        require(app.recorder_.state().phase == Phase::Recording, app.recorder_.state().error.c_str());
        pump(350);
        app.recorder_.stop();
        auto state = app.recorder_.state();
        require(state.error.empty(), state.error.c_str());
        require(state.width == evenExtent(rectWidth(stageRect)) && state.height == evenExtent(rectHeight(stageRect)) &&
                !state.saved.empty() && std::filesystem::file_size(path) > 1000,
                "The actual full-screen scene must produce a native-resolution MP4.");
        std::filesystem::remove(path);
        } else {
            std::cout << "SKIP: stage recording requires Windows Graphics Capture in an active desktop.\n";
        }
        SendMessageW(app.stage_, WM_KEYDOWN, VK_ESCAPE, 0);
        require(app.source_.kind != SourceKind::Stage && !IsWindowVisible(app.stage_),
                "Esc must restore the previous recording source.");
        RECT floating{};
        GetWindowRect(app.overlay_, &floating);
        HWND fixture = CreateWindowExW(0, L"STATIC", L"Window camera layout fixture",
                                        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 140, 560, 420,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(fixture != nullptr, "Cannot create window camera fixture.");
        app.windowCamera_ = {true, 0, true, {}, 1, 1, .24};
        app.selectSource({SourceKind::Window, nullptr, fixture, {}, L"Fixture"});
        pump(50);
        RECT anchored{};
        GetWindowRect(app.overlay_, &anchored);
        require(app.windowCameraAttached_ && IsWindowVisible(app.overlay_),
                "Selecting window + camera must attach a visible preview.");
        SetWindowPos(fixture, nullptr, 260, 220, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump(70);
        RECT moved{};
        GetWindowRect(app.overlay_, &moved);
        require(moved.left == anchored.left + 100 && moved.top == anchored.top + 80,
                "Camera preview must follow window movement without changing its relative position.");
        SetWindowPos(fixture, nullptr, 0, 0, 760, 560, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump(70);
        RECT resized{};
        GetWindowRect(app.overlay_, &resized);
        require(rectWidth(resized) > rectWidth(moved), "Camera preview must scale with its target window.");
        POINT origin{};
        ClientToScreen(fixture, &origin);
        app.moveWindowCamera(origin);
        RECT repositioned{};
        GetWindowRect(app.overlay_, &repositioned);
        require(repositioned.left < resized.left && repositioned.top < resized.top &&
                app.windowCamera_.x == 0 && app.windowCamera_.y == 0,
                "Dragging toward the upper-left must reposition the recorded camera.");
        auto oldSize = app.windowCamera_.size;
        SendMessageW(app.overlay_, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        require(app.windowCamera_.size > oldSize, "Wheel must enlarge the window camera.");
        app.command(commands::HideCamera);
        require(!IsWindowVisible(app.overlay_) && app.windowCameraActive() && app.camera_.frames.get(),
                "Hide preview must keep the camera available for window recording.");
        app.command(commands::HideCamera);
        ShowWindow(fixture, SW_MINIMIZE);
        pump(70);
        require(!IsWindowVisible(app.overlay_), "Camera preview must hide with a minimized target.");
        ShowWindow(fixture, SW_RESTORE);
        pump(70);
        require(IsWindowVisible(app.overlay_), "Camera preview must return with the restored window.");
        app.command(commands::WindowCamera);
        RECT restored{};
        GetWindowRect(app.overlay_, &restored);
        require(!app.windowCameraActive() && !app.windowCameraAttached_ && sameRect(restored, floating),
                "Disabling composition must restore the original floating camera geometry.");
        DestroyWindow(fixture);
        std::cout << "PASS: window camera anchoring, move/resize, preview hiding, minimize/restore and opt-out.\n";
        std::cout << "PASS: double click, crop drag, mirror, apply/cancel/reset, F11/Esc, wheel, "
                     "control exclusion and actual stage recording.\n";
    }
};
}
int main(int argc, char **argv) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&controls);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        youcam::check(MFStartup(MF_VERSION), "Initialize multimedia");
        if (argc > 1 && std::string(argv[1]) == "--camera-switch")
            youcam::AppTest::hardwareCameraSwitch();
        else
            youcam::AppTest::run();
        MFShutdown();
        winrt::uninit_apartment();
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
