#include "recorder.h"
#include "visuals.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <iostream>
#include <fstream>
using namespace youcam;
bool stageFixture = false;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
std::shared_ptr<const Frame> awaitFrame(Frames &frames) {
    for (int i = 0; i < 250; ++i) {
        if (auto frame = frames.get())
            return frame;
        Sleep(20);
    }
    throw std::runtime_error("No native capture frame within five seconds.");
}
template <typename Predicate>
std::shared_ptr<const Frame> awaitPixels(Frames &frames, Predicate predicate, const char *message) {
    // Composition and capture are asynchronous. Observe the requested pixels,
    // rather than assuming that the first queued frame reflects a UI change.
    for (int i = 0; i < 250; ++i) {
        MSG event{};
        while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&event);
            DispatchMessageW(&event);
        }
        auto frame = frames.get();
        if (frame && predicate(*frame))
            return frame;
        Sleep(20);
    }
    throw std::runtime_error(message);
}
void inspectMovie(const std::filesystem::path &path, UINT32 width, UINT32 height, bool audio) {
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Open saved MP4");
    ComPtr<IMFMediaType> type;
    check(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type),
          "Read saved video metadata");
    UINT32 actualW = 0, actualH = 0;
    check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &actualW, &actualH),
          "Read saved video dimensions");
    require(actualW == width && actualH == height, "Encoded video lost source resolution.");
    if (audio) {
        ComPtr<IMFMediaType> sound;
        check(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &sound),
              "Read saved microphone stream");
    }
    ComPtr<IMFMediaType> decoded;
    MFCreateMediaType(&decoded);
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    check(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, decoded.Get()),
          "Configure native video decoder");
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    int count = 0;
    for (;;) {
        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG t = 0;
        check(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &t, &sample),
              "Decode saved video");
        if (sample) {
            require(std::abs(t - int64_t(count) * 10000000 / 30) < 400,
                    "Full-resolution output must be continuous 30 fps CFR from zero.");
            LONGLONG duration = 0;
            check(sample->GetSampleDuration(&duration), "Read saved frame duration");
            require(std::abs(duration - 10000000 / 30) < 400,
                    "Full-resolution output contains a variable-duration frame.");
            ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    require(count > 0, "Saved video contains no decoded frames.");
    std::cout << "PASS: encoded and decoded " << width << 'x' << height << (audio ? " with AAC" : "")
              << ".\n";
}
void inspectWindowCameraMovie(const std::filesystem::path &path) {
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Open composited movie");
    ComPtr<IMFMediaType> decoded;
    MFCreateMediaType(&decoded);
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    check(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, decoded.Get()),
          "Decode composited pixels");
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    ComPtr<IMFMediaType> actual;
    check(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &actual), "Read decoder layout");
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height);
    require(width == 320 && height == 240, "Composite must retain original window dimensions.");
    bool bottomRight = false, topLeft = false, resized = false, disabled = false;
    for (;;) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        check(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                                 &flags, &timestamp, &sample), "Read decoded composition");
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer), "Read decoded pixels");
            ComPtr<IMF2DBuffer> plane;
            BYTE *pixels = nullptr;
            LONG stride = LONG(MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, width));
            bool twoDimensional = SUCCEEDED(buffer.As(&plane));
            if (twoDimensional) {
                check(plane->Lock2D(&pixels, &stride), "Lock decoded plane");
            } else {
                DWORD length = 0;
                check(buffer->Lock(&pixels, nullptr, &length), "Lock decoded image");
                if (stride <= 0 || uint64_t(stride) * height > length) {
                    buffer->Unlock();
                    throw std::runtime_error("Unexpected NV12 decoder layout.");
                }
            }
            int oldCamera = pixels[ptrdiff_t(136) * stride + 216];
            int newCamera = pixels[ptrdiff_t(104) * stride + 104];
            int lowerCameraEdge = pixels[ptrdiff_t(162) * stride + 104];
            int backgroundRed = pixels[ptrdiff_t(10) * stride + 10];
            int backgroundBlue = pixels[ptrdiff_t(10) * stride + 310];
            if (twoDimensional) plane->Unlock2D();
            else buffer->Unlock();
            // Known BT.709 luma: green camera ~173, red window ~63, blue ~32.
            // A white occluding desktop window would be ~235 and must never appear.
            require(backgroundRed > 45 && backgroundRed < 90 && backgroundBlue < 65,
                    "An unrelated window appeared in the saved composition.");
            bool oldGreen = oldCamera > 140 && oldCamera < 200;
            bool newGreen = newCamera > 140 && newCamera < 200;
            bottomRight |= oldGreen;
            topLeft |= newGreen;
            resized |= topLeft && newGreen && lowerCameraEdge < 100;
            disabled |= topLeft && !oldGreen && !newGreen;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    require(bottomRight && topLeft && resized && disabled,
            "Saved video must contain the webcam, live move, window resize and window-only phase.");
    std::cout << "PASS: decoded window + camera pixels, crop, live placement, opt-out and occlusion isolation.\n";
}
LRESULT CALLBACK fixtureProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        if (stageFixture) {
            static Drawing drawing;
            static Frame camera(320, 180);
            for (size_t i = 0; i < camera.bgra.size(); i += 4) {
                camera.bgra[i] = 35; camera.bgra[i + 1] = 100; camera.bgra[i + 2] = 180;
            }
            auto scene = composeStage(topography(r.right, r.bottom, clock100ns() / 10000000.0),
                                       &camera, {.15, 0, .85, 1}, true, .68);
            blitFrame(dc, scene);
            EndPaint(h, &ps);
            return 0;
        }
        RECT half = r;
        half.right = r.right / 2;
        HBRUSH red = CreateSolidBrush(RGB(255, 0, 0)), blue = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(dc, &r, blue);
        FillRect(dc, &half, red);
        DeleteObject(red);
        DeleteObject(blue);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) { InvalidateRect(h, nullptr, FALSE); return 0; }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int fixture(bool stage = false) {
    stageFixture = stage;
    WNDCLASSW c{};
    c.hInstance = GetModuleHandleW(nullptr);
    c.lpfnWndProc = fixtureProc;
    c.lpszClassName = L"YouCamTestFixture";
    c.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&c);
    RECT r{0, 0, 320, 240};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window =
        CreateWindowW(c.lpszClassName, L"YouCam capture fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                      rectWidth(r), rectHeight(r), nullptr, nullptr, c.hInstance, nullptr);
    if (!window)
        return 1;
    if (stage) SetTimer(window, 1, 60, nullptr);
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
struct Child {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    Child(bool stage = false) {
        wchar_t executable[MAX_PATH];
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(executable) + (stage ? L"\" --stage-fixture" : L"\" --fixture");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &startup, &process) != FALSE,
                "Cannot start window fixture.");
        for (int i = 0; i < 100 && !window; ++i) {
            EnumWindows(
                [](HWND h, LPARAM data) -> BOOL {
                    auto child = reinterpret_cast<Child *>(data);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(h, &pid);
                    if (pid == child->process.dwProcessId && IsWindowVisible(h)) {
                        child->window = h;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(this));
            Sleep(50);
        }
        require(window != nullptr, "Fixture did not create a visible window.");
    }
    ~Child() {
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
};
int wmain(int argc, wchar_t **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc > 1 && std::wstring(argv[1]) == L"--fixture")
        return fixture();
    if (argc > 1 && std::wstring(argv[1]) == L"--stage-fixture")
        return fixture(true);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        check(MFStartup(MF_VERSION), "Initialize Media Foundation");
        auto directory = std::filesystem::temp_directory_path() /
                         (L"YouCam native è '" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        for (auto size : {std::pair{640u, 480u}, std::pair{3840u, 2160u}}) {
            Frame frame(size.first, size.second);
            for (size_t i = 0; i < frame.bgra.size(); i += 4) {
                frame.bgra[i] = 40;
                frame.bgra[i + 1] = 100;
                frame.bgra[i + 2] = 220;
                frame.bgra[i + 3] = 255;
            }
            bool audio = size.first == 640;
            auto path = directory / (std::to_wstring(size.first) + L".mp4");
            Movie movie;
            movie.start(path, size.first, size.second, audio);
            for (int i = 0; i < 3; ++i) {
                movie.video(frame, int64_t(i) * 10000000 / 30, 10000000 / 30);
                if (audio)
                    movie.audio(std::vector<int16_t>(1600, 1200), int64_t(i) * 10000000 / 30);
            }
            movie.finish();
            inspectMovie(path, size.first, size.second, audio);
        }
        require(RegisterHotKey(nullptr, 11, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'R') != FALSE,
                "Cannot register native shortcut.");
        UnregisterHotKey(nullptr, 11);
        {
            Frames camera;
            Frame source(161, 121);
            for (size_t i = 0; i < source.bgra.size(); i += 4) {
                source.bgra[i] = 200;
                source.bgra[i + 1] = 90;
                source.bgra[i + 2] = 30;
            }
            camera.put(std::move(source));
            Recorder recorder;
            auto path = directory / L"odd-camera.mp4";
            recorder.start(
                {SourceKind::Camera, nullptr, nullptr, {}, L"Synthetic full-resolution camera", true},
                &camera, true, path);
            for (int i = 0; i < 250 && recorder.state().phase == Phase::Starting; ++i)
                Sleep(20);
            require(recorder.state().phase == Phase::Recording, recorder.state().error.c_str());
            for (int i = 0; i < 6; ++i) {
                recorder.audio(std::vector<int16_t>(1600, 100), clock100ns());
                Sleep(34);
            }
            recorder.pause();
            auto paused = recorder.state().elapsed;
            Sleep(100);
            require(recorder.state().elapsed == paused, "Camera pause must freeze time.");
            recorder.resume();
            for (int i = 0; i < 6; ++i) {
                recorder.audio(std::vector<int16_t>(1600, 100), clock100ns());
                Sleep(34);
            }
            recorder.stop();
            require(recorder.state().error.empty(), recorder.state().error.c_str());
            inspectMovie(path, 162, 122, true);
            Source croppedSource{SourceKind::Camera, nullptr, nullptr, {}, L"Cropped camera", true};
            croppedSource.crop = {.2, .1, .8, .9};
            auto cropPath = directory / L"cropped-camera.mp4";
            recorder.start(croppedSource, &camera, false, cropPath);
            for (int i = 0; i < 250 && recorder.state().phase == Phase::Starting; ++i) Sleep(20);
            require(recorder.state().phase == Phase::Recording, recorder.state().error.c_str());
            Sleep(180);
            recorder.stop();
            require(recorder.state().error.empty(), recorder.state().error.c_str());
            inspectMovie(cropPath, 98, 98, false);
        }
        if (winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            auto displays = monitors();
            require(!displays.empty(), "No native monitor.");
            auto m = displays.front();
            DesktopCapture screen;
            screen.start({SourceKind::Screen, m.handle, nullptr, m.rect, m.name});
            auto full = awaitFrame(*screen.frames);
            require(full->width == uint32_t(rectWidth(m.rect)) &&
                        full->height == uint32_t(rectHeight(m.rect)),
                    "Screen capture is not in physical pixels.");
            screen.stop();
            RECT crop{m.rect.left + 20, m.rect.top + 30, m.rect.left + 181, m.rect.top + 151};
            DesktopCapture region;
            region.start({SourceKind::Region, m.handle, nullptr, crop, L"Test area"});
            auto cropped = awaitFrame(*region.frames);
            require(cropped->width == 161 && cropped->height == 121,
                    "Region capture changed native dimensions.");
            region.stop();
            Child child;
            // Keep the fixture above other desktop apps, on the captured
            // monitor. Otherwise the test samples an unrelated foreground app.
            SetWindowPos(child.window, HWND_TOPMOST, m.rect.left + 100, m.rect.top + 100, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
            HWND cover = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"YouCam capture occlusion test",
                                         WS_POPUP | WS_VISIBLE | SS_WHITERECT,
                                         m.rect.left + 80, m.rect.top + 80, 440, 340, nullptr,
                                         nullptr, GetModuleHandleW(nullptr), nullptr);
            require(cover != nullptr, "Cannot create occluding window.");
            DesktopCapture window;
            window.start({SourceKind::Window, nullptr, child.window, {}, L"Fixture"});
            auto image = awaitFrame(*window.frames);
            require(image->width == 320 && image->height == 240,
                    "Window client area capture has wrong dimensions.");
            require(image->bgra[(120 * 320 + 80) * 4 + 2] > 240 && image->bgra[(120 * 320 + 240) * 4] > 240,
                    "Window capture has wrong colors.");
            RECT original{};
            GetWindowRect(child.window, &original);
            require(SetWindowPos(child.window, nullptr, 180, 160, 450, 350, SWP_NOZORDER | SWP_NOACTIVATE) !=
                        FALSE,
                    "Cannot resize capture fixture.");
            ShowWindow(cover, SW_HIDE);
            RedrawWindow(child.window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
            RECT resizedClient{};
            GetClientRect(child.window, &resizedClient);
            bool resized = false;
            for (int i = 0; i < 100 && !resized; ++i) {
                Sleep(20);
                auto current = window.frames->get();
                resized = current && current->width == uint32_t(resizedClient.right) &&
                          current->height == uint32_t(resizedClient.bottom);
            }
            if (!resized) {
                auto current = window.frames->get();
                std::ostringstream message;
                message << "Window capture did not retain native pixels after resize: expected "
                        << resizedClient.right << 'x' << resizedClient.bottom << ", got "
                        << (current ? current->width : 0) << 'x' << (current ? current->height : 0);
                throw std::runtime_error(message.str());
            }
            SetWindowPos(child.window, nullptr, original.left, original.top, rectWidth(original),
                         rectHeight(original), SWP_NOZORDER | SWP_NOACTIVATE);
            window.stop();
            SetWindowPos(child.window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(cover, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(cover);
            DesktopCapture excluded;
            excluded.start({SourceKind::Screen, m.handle, nullptr, m.rect, m.name});
            POINT sample{80, 120};
            ClientToScreen(child.window, &sample);
            auto colorAt = [&](const Frame &frame, bool red) {
                int x = sample.x - m.rect.left, y = sample.y - m.rect.top;
                if (x < 0 || y < 0 || x >= int(frame.width) || y >= int(frame.height))
                    return false;
                auto pixel = &frame.bgra[(size_t(y) * frame.width + x) * 4];
                return red ? pixel[2] > 240 && pixel[0] < 10 && pixel[1] < 10
                           : pixel[0] > 240 && pixel[1] > 240 && pixel[2] > 240;
            };
            awaitPixels(*excluded.frames, [&](const Frame &f) { return colorAt(f, false); },
                        "The test control was not visible before exclusion.");
            require(SetWindowDisplayAffinity(cover, WDA_EXCLUDEFROMCAPTURE) != FALSE,
                    "Cannot exclude recording controls.");
            DwmFlush();
            awaitPixels(*excluded.frames, [&](const Frame &f) { return colorAt(f, true); },
                        "Excluded control did not reveal the live background.");
            excluded.stop();
            DestroyWindow(cover);
            auto path = directory / L"pause.mp4";
            Recorder recorder;
            recorder.start({SourceKind::Window, nullptr, child.window, {}, L"Fixture"}, nullptr, false, path);
            for (int i = 0; i < 250 && recorder.state().phase == Phase::Starting; ++i)
                Sleep(20);
            require(recorder.state().phase == Phase::Recording,
                    narrow(L"Recording startup: " + wide(recorder.state().error)).c_str());
            Sleep(300);
            ShowWindow(child.window, SW_MINIMIZE);
            for (int i = 0; i < 100 && recorder.state().phase != Phase::Paused; ++i)
                Sleep(20);
            require(recorder.state().phase == Phase::Paused, "Minimized window did not pause recording.");
            auto paused = recorder.state().elapsed;
            Sleep(100);
            require(recorder.state().elapsed == paused, "Paused time advanced.");
            ShowWindow(child.window, SW_RESTORE);
            Sleep(200);
            recorder.resume();
            Sleep(300);
            recorder.stop();
            require(recorder.state().error.empty(), recorder.state().error.c_str());
            inspectMovie(path, 320, 240, false);
            {
                RECT bounds{};
                GetWindowRect(child.window, &bounds);
                HWND blocker = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"Composite occlusion test",
                                               WS_POPUP | WS_VISIBLE | SS_WHITERECT,
                                               bounds.left - 10, bounds.top - 10,
                                               rectWidth(bounds) + 100, rectHeight(bounds) + 100,
                                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                require(blocker != nullptr, "Cannot create compositor occlusion fixture.");
                UpdateWindow(blocker);
                Frame cameraFrame(160, 90);
                for (uint32_t y = 0; y < cameraFrame.height; ++y)
                    for (uint32_t x = 0; x < cameraFrame.width; ++x) {
                        auto pixel = &cameraFrame.bgra[(size_t(y) * cameraFrame.width + x) * 4];
                        pixel[1] = x >= 80 ? 255 : 0;
                        pixel[2] = x < 80 ? 255 : 0;
                        pixel[3] = 255;
                    }
                Frames webcam;
                webcam.put(std::move(cameraFrame));
                Source compositeSource{SourceKind::Window, nullptr, child.window, {}, L"Window + camera"};
                compositeSource.cameraOverlay = {true, 0, true, {.5, 0, 1, 1}, 1, 1, .4};
                auto compositePath = directory / L"window-camera.mp4";
                Recorder composite;
                composite.start(compositeSource, &webcam, false, compositePath);
                for (int i = 0; i < 250 && composite.state().phase == Phase::Starting; ++i) Sleep(20);
                require(composite.state().phase == Phase::Recording, composite.state().error.c_str());
                Sleep(250);
                composite.pause();
                auto frozen = composite.state().elapsed;
                SetWindowPos(child.window, nullptr, bounds.left + 20, bounds.top + 20, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                Sleep(100);
                require(composite.state().elapsed == frozen, "Composite pause must freeze movie time.");
                auto placement = compositeSource.cameraOverlay;
                placement.x = placement.y = 0;
                composite.setCameraOverlay(placement);
                composite.resume();
                Sleep(250);
                RECT outer{}, inner{};
                GetWindowRect(child.window, &outer);
                GetClientRect(child.window, &inner);
                SetWindowPos(child.window, nullptr, 0, 0,
                             640 + rectWidth(outer) - rectWidth(inner),
                             480 + rectHeight(outer) - rectHeight(inner),
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                RedrawWindow(child.window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
                Sleep(500);
                placement.enabled = false;
                composite.setCameraOverlay(placement);
                Sleep(250);
                placement.enabled = true;
                webcam.fail("Synthetic webcam disconnected.");
                composite.setCameraOverlay(placement);
                for (int i = 0; i < 250 && composite.state().phase != Phase::Idle; ++i) Sleep(20);
                composite.stop();
                require(composite.state().error.find("Synthetic webcam disconnected") != std::string::npos &&
                        !composite.state().saved.empty(), "Camera loss must stop and save useful video.");
                DestroyWindow(blocker);
                inspectWindowCameraMovie(compositePath);
            }
            {
                Child stage(true);
                auto stagePath = directory / L"stage.mp4";
                Recorder stageRecording;
                stageRecording.start({SourceKind::Stage, m.handle, stage.window, {}, L"Camera stage"},
                                      nullptr, false, stagePath);
                for (int i = 0; i < 250 && stageRecording.state().phase == Phase::Starting; ++i) Sleep(20);
                require(stageRecording.state().phase == Phase::Recording, stageRecording.state().error.c_str());
                Sleep(450);
                stageRecording.stop();
                require(stageRecording.state().error.empty(), stageRecording.state().error.c_str());
                inspectMovie(stagePath, 320, 240, false);
                std::cout << "PASS: camera stage captured and recorded with animated background.\n";
            }
            std::cout << "PASS: physical monitor pixels, exact region pixels, occluded window capture, "
                         "control exclusion, minimize/pause/resume.\n";
        } else
            std::cout << "SKIP: Windows Graphics Capture is unavailable on this runner; desktop checks "
                         "require an active Windows 11 session.\n";
        std::filesystem::remove_all(directory);
        MFShutdown();
        winrt::uninit_apartment();
        return 0;
    } catch (const winrt::hresult_error &error) {
        std::cerr << narrow(error.message().c_str()) << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
