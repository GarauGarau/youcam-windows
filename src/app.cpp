#include "app.h"
#include <windowsx.h>
#include <iomanip>

namespace youcam {
namespace {
constexpr UINT TrayMessage = WM_APP + 1;
constexpr int Start = 100, Pause = 101, Panel = 102, HideCamera = 103, Mirror = 104, SelectArea = 105,
              WholeScreen = 106, CameraOnly = 107, Quit = 108, Retry = 109, Reveal = 110, Help = 111,
              SourceMenu = 112, AudioNone = 113, ShapeBase = 200, SizeBase = 210, CameraBase = 1000,
              AudioBase = 2000, MonitorBase = 3000, WindowBase = 4000;
constexpr COLORREF Transparent = RGB(1, 2, 3);
HWND makeControl(HWND parent, const wchar_t *type, const wchar_t *label, int id, DWORD style = 0) {
    HWND window = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 20, 20, parent,
                                  reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return window;
}
std::wstring escaped(std::wstring value) {
    for (size_t at = 0; (at = value.find(L'&', at)) != std::wstring::npos; at += 2)
        value.insert(at, 1, L'&');
    return value;
}
RECT monitorBounds(HWND window) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    return info.rcWork;
}
RECT constrained(RECT rect, RECT bounds) {
    int w = std::min(rectWidth(rect), rectWidth(bounds)), h = std::min(rectHeight(rect), rectHeight(bounds));
    int x = std::clamp<int>(rect.left, bounds.left, bounds.right - w),
        y = std::clamp<int>(rect.top, bounds.top, bounds.bottom - h);
    return {x, y, x + w, y + h};
}
void exclude(HWND window) {
    SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
}
void CALLBACK controlShown(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD) {
    if (!window || object != OBJID_WINDOW)
        return;
    wchar_t name[64];
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"#32768") == 0 || wcscmp(name, L"#32770") == 0 ||
        wcscmp(name, L"tooltips_class32") == 0)
        exclude(window);
}
} // namespace
App::App(HINSTANCE instance, bool preview) : instance_(instance), preview_(preview) {
    initialize();
}
std::wstring App::setting(const wchar_t *key, const wchar_t *fallback) {
    wchar_t value[4096];
    GetPrivateProfileStringW(L"YouCam", key, fallback, value, 4096, settings_.c_str());
    return value;
}
void App::initialize() {
    PWSTR local = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local),
          "Find settings folder");
    auto directory = std::filesystem::path(local) / L"YouCamNative";
    CoTaskMemFree(local);
    std::filesystem::create_directories(directory);
    settings_ = (directory / L"settings.ini").wstring();
    cameraID_ = setting(L"Camera");
    microphoneID_ = setting(L"Microphone");
    shape_ = std::clamp(_wtoi(setting(L"Shape", L"0").c_str()), 0, 2);
    mirrored_ = setting(L"Mirror", L"1") != L"0";
    microphone_.gainDB = float(std::clamp(_wtoi(setting(L"Gain", L"0").c_str()), -36, 12));
    WNDCLASSEXW klass{sizeof(klass)};
    klass.hInstance = instance_;
    klass.lpfnWndProc = procedure;
    klass.lpszClassName = L"YouCamNative";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassExW(&klass))
        throw std::runtime_error("Cannot register the application window.");
    monitors_ = monitors();
    if (monitors_.empty())
        throw std::runtime_error("No active display.");
    auto selected = monitors_.front();
    for (const auto &monitor : monitors_)
        if (monitor.name == setting(L"Monitor"))
            selected = monitor;
    source_ = {SourceKind::Screen, selected.handle, nullptr, selected.rect, selected.name};
    auto mode = setting(L"Source", L"screen");
    if (mode == L"camera")
        source_.kind = SourceKind::Camera;
    if (mode == L"region") {
        RECT area{_wtoi(setting(L"Left").c_str()), _wtoi(setting(L"Top").c_str()),
                  _wtoi(setting(L"Right").c_str()), _wtoi(setting(L"Bottom").c_str())};
        RECT intersection{};
        if (IntersectRect(&intersection, &area, &selected.rect) && sameRect(intersection, area) &&
            rectWidth(area) >= 40 && rectHeight(area) >= 40) {
            source_.kind = SourceKind::Region;
            source_.area = area;
            source_.name = L"Selected area";
        }
    }
    overlay_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, klass.lpszClassName, L"YouCam", WS_POPUP,
                               selected.work.right - 312, selected.work.bottom - 312, 280, 280, nullptr,
                               nullptr, instance_, this);
    panel_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, klass.lpszClassName, L"YouCam — recording",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, selected.work.left + 60,
                             selected.work.top + 60, 580, 310, nullptr, nullptr, instance_, this);
    picker_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, klass.lpszClassName,
                              L"Select recording area", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, instance_,
                              this);
    editor_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, klass.lpszClassName,
                              L"Recording area", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, instance_, this);
    editorTools_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, klass.lpszClassName, L"Area controls",
                                   WS_POPUP, 0, 0, 280, 36, nullptr, nullptr, instance_, this);
    if (!overlay_ || !panel_ || !picker_ || !editor_ || !editorTools_)
        throw std::runtime_error("Cannot create YouCam controls.");
    SetLayeredWindowAttributes(picker_, 0, 120, LWA_ALPHA);
    SetLayeredWindowAttributes(editor_, Transparent, 255, LWA_COLORKEY);
    exclude(panel_);
    exclude(picker_);
    exclude(editor_);
    exclude(editorTools_);
    status_ = makeControl(panel_, L"STATIC", L"Ready", 0);
    details_ = makeControl(panel_, L"STATIC", L"", 0);
    notice_ = makeControl(panel_, L"STATIC", L"", 0);
    record_ = makeControl(panel_, L"BUTTON", L"Record", Start);
    pause_ = makeControl(panel_, L"BUTTON", L"Pause", Pause);
    sourceButton_ = makeControl(panel_, L"BUTTON", L"Source…", SourceMenu);
    makeControl(panel_, L"BUTTON", L"Hide", Panel);
    gain_ = makeControl(panel_, TRACKBAR_CLASSW, L"", 120, TBS_AUTOTICKS);
    SendMessageW(gain_, TBM_SETRANGE, TRUE, MAKELPARAM(-36, 12));
    SendMessageW(gain_, TBM_SETPOS, TRUE, LPARAM(microphone_.gainDB.load()));
    meter_ = makeControl(panel_, PROGRESS_CLASSW, L"", 121);
    SendMessageW(meter_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    makeControl(panel_, L"BUTTON", L"Show saved file", Reveal);
    auto select = makeControl(editorTools_, L"BUTTON", L"Select area…", SelectArea);
    MoveWindow(select, 4, 3, 130, 30, TRUE);
    auto whole = makeControl(editorTools_, L"BUTTON", L"Whole screen", WholeScreen);
    MoveWindow(whole, 140, 3, 135, 30, TRUE);
    layoutPanel();
    resizeCamera(std::clamp(_wtoi(setting(L"Width", L"280").c_str()), 120, 960));
    RECT cameraRect{};
    GetWindowRect(overlay_, &cameraRect);
    int x = _wtoi(setting(L"X", std::to_wstring(cameraRect.left).c_str()).c_str()),
        y = _wtoi(setting(L"Y", std::to_wstring(cameraRect.top).c_str()).c_str());
    RECT candidate{x, y, x + rectWidth(cameraRect), y + rectHeight(cameraRect)};
    HMONITOR home = MonitorFromRect(&candidate, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(home, &info);
    candidate = constrained(candidate, info.rcWork);
    SetWindowPos(overlay_, nullptr, candidate.left, candidate.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = overlay_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = TrayMessage;
    tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray_.szTip, L"YouCam");
    Shell_NotifyIconW(NIM_ADD, &tray_);
    const UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    bool first = RegisterHotKey(overlay_, 1, modifiers, 'R') != FALSE;
    bool second = RegisterHotKey(overlay_, 2, modifiers, 'P') != FALSE;
    shortcuts_ = first && second;
    microphone_.samples = [this](auto samples, int64_t time) { recorder_.audio(std::move(samples), time); };
    if (!preview_) {
        cameras_ = Camera::devices();
        microphones_ = Microphone::devices();
        if (microphoneID_.empty())
            microphoneID_ = microphones_.empty() ? L"none" : microphones_.front().id;
        selectCamera(cameraID_);
    } else {
        microphoneID_ = L"none";
        Frame test(640, 480);
        for (size_t i = 0; i < test.bgra.size(); i += 4) {
            test.bgra[i] = 180;
            test.bgra[i + 1] = 100;
            test.bgra[i + 2] = 35;
            test.bgra[i + 3] = 255;
        }
        camera_.frames.put(std::move(test));
    }
    ShowWindow(overlay_, SW_SHOWNOACTIVATE);
    showEditor();
    SetTimer(overlay_, 1, 33, nullptr);
}
void App::saveSettings() {
    if (!overlay_ || preview_)
        return;
    auto put = [&](const wchar_t *key, const std::wstring &value) {
        WritePrivateProfileStringW(L"YouCam", key, value.c_str(), settings_.c_str());
    };
    RECT r{};
    GetWindowRect(overlay_, &r);
    put(L"Camera", cameraID_);
    put(L"Microphone", microphoneID_);
    put(L"Shape", std::to_wstring(shape_));
    put(L"Mirror", mirrored_ ? L"1" : L"0");
    put(L"Gain", std::to_wstring(int(microphone_.gainDB.load())));
    put(L"X", std::to_wstring(r.left));
    put(L"Y", std::to_wstring(r.top));
    put(L"Width", std::to_wstring(rectWidth(r)));
    put(L"Source", source_.kind == SourceKind::Camera   ? L"camera"
                   : source_.kind == SourceKind::Region ? L"region"
                                                        : L"screen");
    for (const auto &m : monitors_)
        if (m.handle == source_.monitor)
            put(L"Monitor", m.name);
    put(L"Left", std::to_wstring(source_.area.left));
    put(L"Top", std::to_wstring(source_.area.top));
    put(L"Right", std::to_wstring(source_.area.right));
    put(L"Bottom", std::to_wstring(source_.area.bottom));
}
App::~App() {
    closing_ = true;
    saveSettings();
    if (overlay_) {
        KillTimer(overlay_, 1);
        UnregisterHotKey(overlay_, 1);
        UnregisterHotKey(overlay_, 2);
        Shell_NotifyIconW(NIM_DELETE, &tray_);
    }
    recorder_.stop();
    microphone_.shutdown();
    camera_.shutdown();
    for (HWND h : {editorTools_, editor_, picker_, panel_, overlay_})
        if (IsWindow(h))
            DestroyWindow(h);
    if (panelFont_)
        DeleteObject(panelFont_);
    UnregisterClassW(L"YouCamNative", instance_);
}
int App::run() {
    auto hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, controlShown,
                                GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (hook)
        UnhookWinEvent(hook);
    return int(message.wParam);
}
void App::error(const std::string &message) {
    MessageBoxW(panel_, wide(message).c_str(), L"YouCam", MB_OK | MB_ICONERROR);
}
void App::notify(const std::wstring &message) {
    noticeText_ = message;
    SetWindowTextW(notice_, message.c_str());
    showPanel();
    tray_.uFlags = NIF_INFO;
    wcsncpy_s(tray_.szInfoTitle, L"YouCam", _TRUNCATE);
    wcsncpy_s(tray_.szInfo, message.c_str(), _TRUNCATE);
    tray_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}
void App::layoutPanel() {
    if (!status_)
        return;
    int dpi = int(GetDpiForWindow(panel_));
    auto p = [dpi](int value) { return MulDiv(value, dpi, 96); };
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
        if (font) {
            EnumChildWindows(
                panel_,
                [](HWND child, LPARAM value) -> BOOL {
                    SendMessageW(child, WM_SETFONT, WPARAM(value), TRUE);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(font));
            if (panelFont_)
                DeleteObject(panelFont_);
            panelFont_ = font;
        }
    }
    auto place = [&](HWND h, int x, int y, int w, int height) {
        MoveWindow(h, p(x), p(y), p(w), p(height), TRUE);
    };
    place(status_, 14, 12, 530, 24);
    place(details_, 14, 40, 530, 44);
    place(notice_, 14, 90, 530, 62);
    place(record_, 14, 160, 105, 30);
    place(pause_, 128, 160, 105, 30);
    place(sourceButton_, 242, 160, 130, 30);
    place(GetDlgItem(panel_, Panel), 386, 160, 105, 30);
    place(gain_, 14, 202, 260, 30);
    place(meter_, 290, 208, 100, 18);
    place(GetDlgItem(panel_, Reveal), 400, 202, 142, 30);
    RECT frame{0, 0, p(560), p(246)};
    AdjustWindowRectExForDpi(&frame, GetWindowLongW(panel_, GWL_STYLE), FALSE,
                             GetWindowLongW(panel_, GWL_EXSTYLE), dpi);
    SetWindowPos(panel_, nullptr, 0, 0, rectWidth(frame), rectHeight(frame), SWP_NOMOVE | SWP_NOZORDER);
}
void App::showPanel(bool show) {
    ShowWindow(panel_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    syncMicrophone();
}
void App::syncMicrophone() {
    bool needed = !preview_ && microphoneID_ != L"none" &&
                  (IsWindowVisible(panel_) || recorder_.state().phase != Phase::Idle);
    if (needed == micRunning_)
        return;
    micRunning_ = needed;
    if (needed) {
        try {
            microphone_.start(microphoneID_);
        } catch (const std::exception &e) {
            micRunning_ = false;
            error(e.what());
        }
    } else
        microphone_.shutdown();
}
void App::selectCamera(const std::wstring &id) {
    if (preview_)
        return;
    cameraID_ = id;
    cameraError_.clear();
    try {
        camera_.start(id);
        if (cameraID_.empty() && !cameras_.empty())
            cameraID_ = cameras_.front().id;
    } catch (const std::exception &e) {
        cameraError_ = e.what();
    }
    saveSettings();
}
void App::update() {
    if (closing_)
        return;
    if (cameraVisible_)
        InvalidateRect(overlay_, nullptr, FALSE);
    auto state = recorder_.state();
    bool busy = state.phase != Phase::Idle;
    std::wstring status = L"Ready";
    if (state.phase == Phase::Starting)
        status = L"Starting at native resolution…";
    else if (state.phase == Phase::Saving)
        status = L"Saving recording…";
    else if (state.phase == Phase::Recording || state.phase == Phase::Paused) {
        wchar_t time[64];
        auto seconds = state.elapsed / 10000000;
        swprintf_s(time, L"%02lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
        status = std::wstring(state.phase == Phase::Paused ? L"Paused · " : L"Recording · ") + time;
        status += L" · " + dimensions(state.width, state.height) + L" · " + state.codec;
    } else if (!state.saved.empty())
        status = L"Saved to Desktop";
    SetWindowTextW(status_, status.c_str());
    std::wstring detail = L"Source: " + (source_.kind == SourceKind::Camera ? L"Camera only" : source_.name);
    if (camera_.mode.pixels())
        detail += L"\nCamera: " + dimensions(camera_.mode.width, camera_.mode.height) + L" (maximum)";
    detail += L" · Mic gain " + std::to_wstring(int(microphone_.gainDB.load())) + L" dB";
    SetWindowTextW(details_, detail.c_str());
    SetWindowTextW(record_, busy ? L"Stop" : L"Record");
    SetWindowTextW(pause_, state.phase == Phase::Paused ? L"Resume" : L"Pause");
    EnableWindow(record_, !preview_ && state.phase != Phase::Saving && !picking_);
    EnableWindow(pause_, state.phase == Phase::Recording || state.phase == Phase::Paused);
    EnableWindow(sourceButton_, state.phase != Phase::Starting && state.phase != Phase::Saving);
    SendMessageW(meter_, PBM_SETPOS, WPARAM(std::min(1.0f, microphone_.peak.load()) * 100), 0);
    if (state.phase != previousPhase_) {
        if (state.phase == Phase::Paused && !state.notice.empty())
            notify(wide(state.notice));
        previousPhase_ = state.phase;
        syncMicrophone();
    }
    if (state.phase == Phase::Idle && state.serial != shownCompletion_) {
        shownCompletion_ = state.serial;
        auto message = wide(state.error);
        if (!state.saved.empty())
            message += (message.empty() ? L"" : L"\n") + std::wstring(L"Saved: ") + state.saved.wstring();
        if (!message.empty())
            notify(message);
        showEditor();
    }
    if (busy && micRunning_) {
        auto failure = microphone_.error();
        if (!failure.empty())
            recorder_.stop("Microphone stopped: " + failure);
    }
}
void App::menu(POINT position, bool sourceOnly) {
    HMENU root = CreatePopupMenu(), sources = sourceOnly ? root : CreatePopupMenu();
    auto append = [](HMENU menu, int id, const std::wstring &title, bool selected = false,
                     bool enabled = true) {
        AppendMenuW(menu, MF_STRING | (selected ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id,
                    title.c_str());
    };
    auto state = recorder_.state();
    bool busy = state.phase != Phase::Idle;
    append(sources, WholeScreen, L"Whole screen", source_.kind == SourceKind::Screen);
    append(sources, SelectArea, L"Select area…", source_.kind == SourceKind::Region);
    monitors_ = monitors();
    if (monitors_.size() > 1)
        for (size_t i = 0; i < monitors_.size() && i < 900; ++i)
            append(sources, MonitorBase + int(i),
                   monitors_[i].name + L" — " +
                       dimensions(rectWidth(monitors_[i].rect), rectHeight(monitors_[i].rect)),
                   source_.monitor == monitors_[i].handle && source_.kind == SourceKind::Screen);
    HMENU windowMenu = CreatePopupMenu();
    windows_ = windows();
    if (windows_.empty())
        append(windowMenu, 0, L"No available windows", false, false);
    for (size_t i = 0; i < windows_.size() && i < 900; ++i)
        append(windowMenu, WindowBase + int(i), escaped(windows_[i].name),
               source_.kind == SourceKind::Window && source_.window == windows_[i].handle);
    AppendMenuW(sources, MF_POPUP, reinterpret_cast<UINT_PTR>(windowMenu), L"Window");
    append(sources, CameraOnly, L"Camera only (full frame)", source_.kind == SourceKind::Camera);
    if (!sourceOnly) {
        append(root, 0, L"YouCam", false, false);
        append(root, Start, busy ? L"Stop recording" : L"Start recording", false,
               !preview_ && state.phase != Phase::Saving);
        append(root, Pause, state.phase == Phase::Paused ? L"Resume recording" : L"Pause recording", false,
               state.phase == Phase::Recording || state.phase == Phase::Paused);
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(sources), L"Recording source");
        append(root, Panel, IsWindowVisible(panel_) ? L"Hide recording panel" : L"Show recording panel");
        if (!state.saved.empty())
            append(root, Reveal, L"Show last recording");
        AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
        append(root, HideCamera,
               cameraVisible_ ? (source_.kind == SourceKind::Camera ? L"Hide camera preview" : L"Hide camera")
                              : L"Show camera");
        HMENU cameraMenu = CreatePopupMenu();
        try {
            cameras_ = Camera::devices();
        } catch (...) {
            cameras_.clear();
        }
        if (cameras_.empty())
            append(cameraMenu, 0, L"No camera available", false, false);
        for (size_t i = 0; i < cameras_.size() && i < 900; ++i)
            append(cameraMenu, CameraBase + int(i), escaped(cameras_[i].name), cameraID_ == cameras_[i].id,
                   !(busy && source_.kind == SourceKind::Camera));
        append(cameraMenu, Retry, L"Retry camera", false, !(busy && source_.kind == SourceKind::Camera));
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(cameraMenu), L"Camera (maximum resolution)");
        HMENU audioMenu = CreatePopupMenu();
        append(audioMenu, AudioNone, L"No audio", microphoneID_ == L"none", !busy);
        try {
            microphones_ = Microphone::devices();
        } catch (...) {
            microphones_.clear();
        }
        for (size_t i = 0; i < microphones_.size() && i < 900; ++i)
            append(audioMenu, AudioBase + int(i), escaped(microphones_[i].name),
                   microphoneID_ == microphones_[i].id, !busy);
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(audioMenu), L"Audio input");
        HMENU shapes = CreatePopupMenu();
        append(shapes, ShapeBase, L"Circle", shape_ == 0);
        append(shapes, ShapeBase + 1, L"Square", shape_ == 1);
        append(shapes, ShapeBase + 2, L"Rectangle (16:9)", shape_ == 2);
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(shapes), L"Shape");
        HMENU sizes = CreatePopupMenu();
        const wchar_t *names[] = {L"Small", L"Medium", L"Large", L"Extra large"};
        for (int i = 0; i < 4; ++i)
            append(sizes, SizeBase + i, names[i]);
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), L"Size");
        append(root, Mirror, L"Mirror camera", mirrored_, !(busy && source_.kind == SourceKind::Camera));
        append(root, Help, L"How to use YouCam");
        if (!shortcuts_)
            append(root, 0, L"Some global shortcuts are unavailable", false, false);
        AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
        append(root, Quit, L"Quit YouCam");
    }
    SetForegroundWindow(overlay_);
    int selected = int(
        TrackPopupMenu(root, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y, 0, overlay_, nullptr));
    DestroyMenu(root);
    PostMessageW(overlay_, WM_NULL, 0, 0);
    if (selected)
        command(selected);
}
bool App::allowSourceChange() {
    auto state = recorder_.state();
    if (state.phase == Phase::Idle)
        return true;
    if (state.phase == Phase::Starting || state.phase == Phase::Saving)
        return false;
    bool running = state.phase == Phase::Recording;
    if (running)
        recorder_.pause();
    int choice = MessageBoxW(panel_,
                             L"Save the current recording and change source?\n\nRecording is paused while "
                             L"you choose. A new source starts a separate video.",
                             L"YouCam", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (choice != IDOK) {
        if (running)
            recorder_.resume();
        return false;
    }
    recorder_.stop();
    auto saved = recorder_.state();
    if (saved.saved.empty()) {
        error(saved.error.empty() ? "The current recording could not be saved." : saved.error);
        return false;
    }
    notify(L"Saved: " + saved.saved.wstring());
    return true;
}
void App::selectSource(Source source) {
    if (!allowSourceChange())
        return;
    source_ = source;
    if (source_.kind == SourceKind::Camera && !preview_)
        selectCamera(cameraID_);
    if (source_.kind != SourceKind::Camera && !cameraVisible_)
        camera_.shutdown();
    saveSettings();
    showEditor();
    update();
}
void App::command(int id) {
    try {
        if (id == Start)
            toggleRecording();
        else if (id == Pause)
            togglePause();
        else if (id == Panel)
            showPanel(!IsWindowVisible(panel_));
        else if (id == SourceMenu) {
            POINT point{};
            GetCursorPos(&point);
            menu(point, true);
        } else if (id == Quit)
            PostQuitMessage(0);
        else if (id == SelectArea)
            beginSelection();
        else if (id == WholeScreen) {
            HMONITOR monitor =
                source_.monitor ? source_.monitor : MonitorFromWindow(overlay_, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info))
                return;
            selectSource({SourceKind::Screen, monitor, nullptr, info.rcMonitor, info.szDevice});
        } else if (id == CameraOnly)
            selectSource({SourceKind::Camera, source_.monitor, nullptr, {}, L"Camera only", mirrored_});
        else if (id == HideCamera) {
            cameraVisible_ = !cameraVisible_;
            ShowWindow(overlay_, cameraVisible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
            if (cameraVisible_ && !camera_.mode.pixels())
                selectCamera(cameraID_);
            else if (!cameraVisible_ && source_.kind != SourceKind::Camera)
                camera_.shutdown();
        } else if (id == Mirror) {
            mirrored_ = !mirrored_;
            saveSettings();
            InvalidateRect(overlay_, nullptr, FALSE);
        } else if (id == Retry) {
            cameras_ = Camera::devices();
            selectCamera(cameraID_);
        } else if (id == Reveal) {
            auto path = recorder_.state().saved;
            if (!path.empty()) {
                std::wstring args = L"/select,\"" + path.wstring() + L"\"";
                ShellExecuteW(panel_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
        } else if (id == Help)
            MessageBoxW(
                panel_,
                L"Drag the camera to move it; scroll to resize. Right-click for controls.\n\n"
                L"Recording source: whole monitor, selected area, a window, or the full camera frame. Drag "
                L"the area corners to resize; drag its top edge to move. Esc cancels selection.\n\n"
                L"Ctrl+Alt+Shift+R starts/stops. Ctrl+Alt+Shift+P pauses/resumes. Movies save to your "
                L"Desktop.\n\n"
                L"Camera capture uses the highest resolution reported by its driver. Screen capture uses "
                L"physical monitor pixels. Preview size never reduces the camera recording. Unsupported "
                L"encoder resolutions produce an error instead of downscaling.\n\n"
                L"Choose a microphone in Audio input; use the panel slider for gain. System audio is not "
                L"recorded.",
                L"YouCam", MB_OK);
        else if (id >= ShapeBase && id < ShapeBase + 3) {
            shape_ = id - ShapeBase;
            RECT r{};
            GetWindowRect(overlay_, &r);
            resizeCamera(rectWidth(r));
            saveSettings();
        } else if (id >= SizeBase && id < SizeBase + 4) {
            int widths[] = {180, 280, 400, 560};
            resizeCamera(widths[id - SizeBase]);
            saveSettings();
        } else if (id >= CameraBase && id < CameraBase + int(cameras_.size()))
            selectCamera(cameras_[id - CameraBase].id);
        else if (id == AudioNone || (id >= AudioBase && id < AudioBase + int(microphones_.size()))) {
            if (recorder_.state().phase != Phase::Idle)
                return;
            microphone_.shutdown();
            micRunning_ = false;
            microphoneID_ = id == AudioNone ? L"none" : microphones_[id - AudioBase].id;
            syncMicrophone();
            saveSettings();
        } else if (id >= MonitorBase && id < MonitorBase + int(monitors_.size())) {
            auto m = monitors_[id - MonitorBase];
            selectSource({SourceKind::Screen, m.handle, nullptr, m.rect, m.name});
        } else if (id >= WindowBase && id < WindowBase + int(windows_.size())) {
            auto w = windows_[id - WindowBase];
            selectSource({SourceKind::Window, nullptr, w.handle, {}, w.name});
        }
    } catch (const std::exception &failure) {
        error(failure.what());
    }
}
void App::toggleRecording() {
    if (preview_ || picking_)
        return;
    if (recorder_.state().phase != Phase::Idle) {
        recorder_.stop();
        update();
        return;
    }
    if (source_.kind == SourceKind::Camera && !camera_.frames.get(true))
        throw std::runtime_error("Wait for the full-resolution camera image before recording.");
    if (source_.kind == SourceKind::Window && (!IsWindow(source_.window) || hiddenWindow(source_.window)))
        throw std::runtime_error("Restore the selected window before recording.");
    source_.mirror = source_.kind == SourceKind::Camera && mirrored_;
    ShowWindow(editor_, SW_HIDE);
    ShowWindow(editorTools_, SW_HIDE);
    noticeText_.clear();
    SetWindowTextW(notice_, L"");
    recorder_.start(source_, &camera_.frames, microphoneID_ != L"none", nextMoviePath());
    syncMicrophone();
    update();
}
void App::togglePause() {
    auto state = recorder_.state();
    if (state.phase == Phase::Paused) {
        if (source_.kind == SourceKind::Window && (!IsWindow(source_.window) || hiddenWindow(source_.window)))
            throw std::runtime_error("Restore the selected window before resuming.");
        recorder_.resume();
    } else
        recorder_.pause();
    update();
}
void App::resizeCamera(int width) {
    RECT bounds = monitorBounds(overlay_);
    double aspect = shape_ == 2 ? 16.0 / 9.0 : 1.0;
    width =
        std::clamp(width, 120, std::min(960, std::min(rectWidth(bounds), int(rectHeight(bounds) * aspect))));
    int height = int(width / aspect);
    RECT current{};
    GetWindowRect(overlay_, &current);
    RECT rect{current.left, current.top, current.left + width, current.top + height};
    rect = constrained(rect, bounds);
    SetWindowPos(overlay_, HWND_TOPMOST, rect.left, rect.top, width, height, SWP_NOACTIVATE);
    HRGN mask = shape_ == 0 ? CreateEllipticRgn(0, 0, width, height) : CreateRectRgn(0, 0, width, height);
    if (!SetWindowRgn(overlay_, mask, TRUE))
        DeleteObject(mask);
    InvalidateRect(overlay_, nullptr, TRUE);
}
void App::paintCamera(HDC dc) {
    RECT area{};
    GetClientRect(overlay_, &area);
    FillRect(dc, &area, GetSysColorBrush(COLOR_WINDOWTEXT));
    std::shared_ptr<const Frame> frame;
    std::wstring message = preview_ ? L"Preview" : L"Starting camera…";
    try {
        frame = camera_.frames.get();
    } catch (const std::exception &e) {
        message = wide(e.what());
    }
    if (!cameraError_.empty())
        message = wide(cameraError_);
    if (frame) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = frame->width;
        info.bmiHeader.biHeight = -LONG(frame->height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        double scale =
            std::max(double(rectWidth(area)) / frame->width, double(rectHeight(area)) / frame->height);
        int sw = std::min(int(frame->width), int(rectWidth(area) / scale)),
            sh = std::min(int(frame->height), int(rectHeight(area) / scale));
        int sx = (int(frame->width) - sw) / 2, sy = (int(frame->height) - sh) / 2;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, mirrored_ ? rectWidth(area) : 0, 0, mirrored_ ? -rectWidth(area) : rectWidth(area),
                      rectHeight(area), sx, sy, sw, sh, frame->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(245, 245, 245));
        area.left += 24;
        area.right -= 24;
        area.top += 40;
        area.bottom -= 20;
        DrawTextW(dc, message.c_str(), -1, &area, DT_CENTER | DT_WORDBREAK);
    }
}
void App::beginSelection() {
    if (!allowSourceChange())
        return;
    ShowWindow(editor_, SW_HIDE);
    ShowWindow(editorTools_, SW_HIDE);
    selection_ = {};
    picking_ = true;
    dragging_ = false;
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    SetWindowPos(picker_, HWND_TOPMOST, x, y, GetSystemMetrics(SM_CXVIRTUALSCREEN),
                 GetSystemMetrics(SM_CYVIRTUALSCREEN), SWP_SHOWWINDOW);
    SetForegroundWindow(picker_);
    SetFocus(picker_);
    InvalidateRect(picker_, nullptr, TRUE);
}
void App::endSelection(bool accept) {
    ReleaseCapture();
    ShowWindow(picker_, SW_HIDE);
    picking_ = dragging_ = false;
    if (accept && rectWidth(selection_) >= 40 && rectHeight(selection_) >= 40) {
        POINT middle{(selection_.left + selection_.right) / 2, (selection_.top + selection_.bottom) / 2};
        source_ = {SourceKind::Region, MonitorFromPoint(middle, MONITOR_DEFAULTTONEAREST), nullptr,
                   selection_, L"Selected area"};
        saveSettings();
    }
    showEditor();
}
void App::showEditor() {
    if (source_.kind != SourceKind::Region || recorder_.state().phase != Phase::Idle || picking_ ||
        closing_) {
        ShowWindow(editor_, SW_HIDE);
        ShowWindow(editorTools_, SW_HIDE);
        return;
    }
    auto area = source_.area;
    SetWindowPos(editor_, HWND_TOPMOST, area.left - 8, area.top - 8, rectWidth(area) + 16,
                 rectHeight(area) + 16, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(source_.monitor, &info);
    int y = area.top - 50;
    if (y < info.rcWork.top)
        y = std::min<int>(area.bottom + 14, info.rcWork.bottom - 36);
    int x = std::clamp<int>((area.left + area.right) / 2 - 140, info.rcWork.left,
                            std::max(info.rcWork.left, info.rcWork.right - 280));
    SetWindowPos(editorTools_, HWND_TOPMOST, x, y, 280, 36, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(editor_, nullptr, TRUE);
}
void App::paintRegion(HWND window, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH background = CreateSolidBrush(window == editor_ ? Transparent : RGB(0, 0, 0));
    FillRect(dc, &client, background);
    DeleteObject(background);
    RECT rect{};
    if (window == picker_) {
        rect = selection_;
        POINT origin{0, 0};
        ClientToScreen(window, &origin);
        OffsetRect(&rect, -origin.x, -origin.y);
    } else
        rect = {8, 8, client.right - 8, client.bottom - 8};
    if (rectWidth(rect) > 0 && rectHeight(rect) > 0) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        auto oldPen = SelectObject(dc, pen);
        auto oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        if (window == editor_) {
            for (POINT point : {POINT{rect.left, rect.top}, POINT{rect.right, rect.top},
                                POINT{rect.right, rect.bottom}, POINT{rect.left, rect.bottom}}) {
                RECT dot{point.x - 6, point.y - 6, point.x + 6, point.y + 6};
                FillRect(dc, &dot, GetSysColorBrush(COLOR_HIGHLIGHT));
            }
            RECT move{client.right / 2 - 38, 0, client.right / 2 + 38, 20};
            FillRect(dc, &move, GetSysColorBrush(COLOR_HIGHLIGHT));
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"Move", -1, &move, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    if (window == picker_) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        client.top = client.bottom - 80;
        DrawTextW(dc, L"Drag to select an area on one monitor. Esc cancels.", -1, &client,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
LRESULT CALLBACK App::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    App *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) {
        try {
            return app->dispatch(window, message, wparam, lparam);
        } catch (const std::exception &error) {
            app->error(error.what());
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT App::dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND && (window == overlay_ || window == picker_ || window == editor_))
        return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (window == overlay_)
            paintCamera(dc);
        else if (window == picker_ || window == editor_)
            paintRegion(window, dc);
        else
            FillRect(dc, &paint.rcPaint, GetSysColorBrush(COLOR_WINDOW));
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_COMMAND) {
        command(LOWORD(wparam));
        return 0;
    }
    if (message == WM_HSCROLL && reinterpret_cast<HWND>(lparam) == gain_) {
        microphone_.gainDB = float(SendMessageW(gain_, TBM_GETPOS, 0, 0));
        saveSettings();
        return 0;
    }
    if (message == WM_TIMER && window == overlay_) {
        update();
        return 0;
    }
    if (message == WM_HOTKEY) {
        if (wparam == 1)
            toggleRecording();
        else if (wparam == 2)
            togglePause();
        return 0;
    }
    if (message == TrayMessage) {
        if (lparam == WM_LBUTTONUP)
            showPanel(!IsWindowVisible(panel_));
        else if (lparam == WM_RBUTTONUP) {
            POINT point{};
            GetCursorPos(&point);
            menu(point);
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        if (window == panel_)
            showPanel(false);
        else if (window == picker_)
            endSelection(false);
        else if (window == overlay_)
            PostQuitMessage(0);
        return 0;
    }
    if (message == WM_KEYDOWN && window == picker_ && wparam == VK_ESCAPE) {
        endSelection(false);
        return 0;
    }
    if (message == WM_RBUTTONUP || message == WM_CONTEXTMENU) {
        if (window == picker_) {
            endSelection(false);
            return 0;
        }
        POINT point{};
        GetCursorPos(&point);
        menu(point, window == editor_ || window == editorTools_);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && window == overlay_) {
        RECT r{};
        GetWindowRect(overlay_, &r);
        resizeCamera(rectWidth(r) + GET_WHEEL_DELTA_WPARAM(wparam) / 4);
        saveSettings();
        return 0;
    }
    if (message == WM_NCHITTEST && window == editor_) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        RECT rect{};
        GetClientRect(window, &rect);
        if (point.x > 16 && point.x < rect.right - 16 && point.y > 20 && point.y < rect.bottom - 16)
            return HTTRANSPARENT;
        return HTCLIENT;
    }
    if (message == WM_SETCURSOR && window == picker_) {
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    }
    if (message == WM_LBUTTONDOWN && (window == overlay_ || window == picker_ || window == editor_)) {
        GetCursorPos(&dragStart_);
        GetWindowRect(window, &dragRect_);
        dragging_ = true;
        SetCapture(window);
        if (window == picker_) {
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromPoint(dragStart_, MONITOR_DEFAULTTONEAREST), &info);
            selectionBounds_ = info.rcMonitor;
            selection_ = {};
        }
        if (window == editor_) {
            dragRect_ = source_.area;
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(source_.monitor, &info);
            selectionBounds_ = info.rcMonitor;
            bool left = std::abs(dragStart_.x - source_.area.left) < 16,
                 right = std::abs(dragStart_.x - source_.area.right) < 16;
            bool top = std::abs(dragStart_.y - source_.area.top) < 16,
                 bottom = std::abs(dragStart_.y - source_.area.bottom) < 16;
            dragCorner_ = left && top ? 0 : right && top ? 1 : right && bottom ? 2 : left && bottom ? 3 : 4;
        }
        return 0;
    }
    if (message == WM_MOUSEMOVE && dragging_ && GetCapture() == window) {
        POINT point{};
        GetCursorPos(&point);
        int dx = point.x - dragStart_.x, dy = point.y - dragStart_.y;
        if (window == overlay_) {
            RECT r = dragRect_;
            OffsetRect(&r, dx, dy);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info);
            r = constrained(r, info.rcWork);
            SetWindowPos(window, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (window == picker_) {
            RECT r{std::min(dragStart_.x, point.x), std::min(dragStart_.y, point.y),
                   std::max(dragStart_.x, point.x), std::max(dragStart_.y, point.y)};
            IntersectRect(&selection_, &r, &selectionBounds_);
            InvalidateRect(window, nullptr, TRUE);
        } else if (window == editor_) {
            RECT r = dragRect_;
            if (dragCorner_ == 4) {
                OffsetRect(&r, dx, dy);
                r = constrained(r, selectionBounds_);
            } else {
                if (dragCorner_ == 0 || dragCorner_ == 3)
                    r.left = std::clamp<LONG>(r.left + dx, selectionBounds_.left, r.right - 40);
                else
                    r.right = std::clamp<LONG>(r.right + dx, r.left + 40, selectionBounds_.right);
                if (dragCorner_ == 0 || dragCorner_ == 1)
                    r.top = std::clamp<LONG>(r.top + dy, selectionBounds_.top, r.bottom - 40);
                else
                    r.bottom = std::clamp<LONG>(r.bottom + dy, r.top + 40, selectionBounds_.bottom);
            }
            source_.area = r;
            showEditor();
        }
        return 0;
    }
    if (message == WM_LBUTTONUP && dragging_) {
        dragging_ = false;
        ReleaseCapture();
        if (window == picker_)
            endSelection(true);
        else
            saveSettings();
        return 0;
    }
    if (message == WM_DPICHANGED) {
        // Region geometry is already in physical pixels. Scaling the editor
        // with the suggested UI rectangle would misrepresent the saved area.
        if (window == editor_ || window == editorTools_) {
            showEditor();
            return 0;
        }
        if (window == picker_)
            return 0;
        auto rect = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rectWidth(*rect), rectHeight(*rect),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (window == panel_)
            layoutPanel();
        else if (window == overlay_)
            resizeCamera(rectWidth(*rect));
        return 0;
    }
    if (message == WM_DISPLAYCHANGE && window == overlay_) {
        if (source_.kind == SourceKind::Screen || source_.kind == SourceKind::Region)
            recorder_.stop();
        monitors_ = monitors();
        if (!monitors_.empty() && source_.kind != SourceKind::Camera && source_.kind != SourceKind::Window) {
            auto m = monitors_.front();
            source_ = {SourceKind::Screen, m.handle, nullptr, m.rect, m.name};
            showEditor();
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace youcam
