#include "app.h"
#include <winrt/base.h>
#include <cstdio>
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    bool smoke = wcsstr(arguments, L"--smoke-test") != nullptr;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),
                                  ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        youcam::check(MFStartup(MF_VERSION), "Initialize Windows multimedia");
        {
            youcam::App app(instance, smoke || wcsstr(arguments, L"--preview") != nullptr);
            if (smoke)
                SetTimer(nullptr, 0, 1200, [](HWND, UINT, UINT_PTR id, DWORD) {
                    KillTimer(nullptr, id);
                    PostQuitMessage(0);
                });
            app.run();
        }
        MFShutdown();
        winrt::uninit_apartment();
        return 0;
    } catch (const winrt::hresult_error &e) {
        if (smoke)
            std::fprintf(stderr, "%s\n", youcam::narrow(e.message().c_str()).c_str());
        else
            MessageBoxW(nullptr, e.message().c_str(), L"YouCam", MB_OK | MB_ICONERROR);
    } catch (const std::exception &e) {
        if (smoke)
            std::fprintf(stderr, "%s\n", e.what());
        else
            MessageBoxW(nullptr, youcam::wide(e.what()).c_str(), L"YouCam", MB_OK | MB_ICONERROR);
    }
    return 1;
}
