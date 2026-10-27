#pragma once
#include "camera.h"
#include "audio.h"
#include "recorder.h"
#include <commctrl.h>
#include <shellapi.h>
namespace youcam {
class App {
    HINSTANCE instance_;
    HWND overlay_ = nullptr, panel_ = nullptr, picker_ = nullptr, editor_ = nullptr, editorTools_ = nullptr;
    HWND status_ = nullptr, details_ = nullptr, notice_ = nullptr, record_ = nullptr, pause_ = nullptr, gain_ = nullptr, meter_ = nullptr;
    HWND sourceButton_ = nullptr;
    NOTIFYICONDATAW tray_{};
    Camera camera_;
    Microphone microphone_;
    Recorder recorder_;
    std::vector<Device> cameras_, microphones_;
    std::vector<Monitor> monitors_;
    std::vector<Window> windows_;
    Source source_;
    std::wstring cameraID_, microphoneID_, settings_;
    std::string cameraError_;
    std::wstring noticeText_;
    int shape_ = 0;
    bool mirrored_ = true, cameraVisible_ = true, micRunning_ = false, closing_ = false;
    bool picking_ = false, dragging_ = false;
    POINT dragStart_{};
    RECT dragRect_{}, selection_{}, selectionBounds_{};
    int dragCorner_ = -1;
    Phase previousPhase_ = Phase::Idle;
    uint64_t shownCompletion_ = 0;
    bool shortcuts_ = false;
    bool preview_ = false;
    std::wstring setting(const wchar_t*, const wchar_t* fallback = L"");
    void saveSettings();
    void initialize();
    void update();
    void error(const std::string&);
    void showPanel(bool show = true);
    void layoutPanel();
    void menu(POINT, bool sourceOnly = false);
    void command(int);
    bool allowSourceChange();
    void selectSource(Source);
    void selectCamera(const std::wstring&);
    void syncMicrophone();
    void toggleRecording();
    void togglePause();
    void beginSelection();
    void endSelection(bool accept);
    void showEditor();
    void resizeCamera(int width);
    void paintCamera(HDC);
    void paintRegion(HWND, HDC);
    void notify(const std::wstring&);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT dispatch(HWND, UINT, WPARAM, LPARAM);
public:
    explicit App(HINSTANCE, bool preview = false);
    ~App();
    int run();
};
} // namespace youcam
