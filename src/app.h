#pragma once
#include "camera.h"
#include "audio.h"
#include "recorder.h"
#include "visuals.h"
#include "commands.h"
#include <commctrl.h>
#include <shellapi.h>
namespace youcam {
class App {
    friend struct AppTest;
    Drawing drawing_;
    HINSTANCE instance_;
    HFONT panelFont_ = nullptr;
    HWND overlay_ = nullptr, panel_ = nullptr, picker_ = nullptr, editor_ = nullptr, editorTools_ = nullptr;
    HWND status_ = nullptr, details_ = nullptr, notice_ = nullptr, record_ = nullptr, pause_ = nullptr,
         gain_ = nullptr, meter_ = nullptr;
    HWND sourceButton_ = nullptr;
    HWND cameraButton_ = nullptr, stage_ = nullptr, cropEditor_ = nullptr;
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
    Crop crop_, pendingCrop_;
    Crop portraitCrop_;
    bool portraitReverse_ = false;
    std::shared_ptr<const Frame> portraitInput_, portraitFrame_;
    int portraitTurns_ = -1;
    bool cropping_ = false, cropDragging_ = false;
    int cropHandle_ = -1, cameraWidth_ = 280;
    POINT cropStart_{};
    Crop cropDragStart_;
    RECT cropImage_{};
    double stageScale_ = .68;
    CameraOverlay windowCamera_{true};
    bool windowCameraAttached_ = false;
    RECT floatingCameraRect_{}, windowPreviewRect_{};
    Source beforeStage_;
    bool cameraBeforeStage_ = true;
    Frame terrain_;
    Frames stageFrames_;
    int64_t terrainTick_ = -1;
    std::wstring setting(const wchar_t *, const wchar_t *fallback = L"");
    void saveSettings();
    void initialize();
    void update();
    void error(const std::string &);
    void showPanel(bool show = true);
    void layoutPanel();
    void menu(POINT, bool sourceOnly = false);
    void cameraMenu(POINT);
    void command(int);
    bool allowSourceChange();
    void selectSource(Source);
    void selectCamera(const std::wstring &);
    void syncMicrophone();
    void toggleRecording();
    void togglePause();
    void beginSelection();
    void endSelection(bool accept);
    void showEditor();
    void resizeCamera(int width);
    void paintCamera(HDC);
    void renderCamera();
    void toggleStage();
    void leaveStage();
    void paintStage(HDC);
    void changeStageScale(double delta);
    bool windowCameraActive() const;
    CameraOverlay windowCameraSettings() const;
    void syncWindowCamera();
    void moveWindowCamera(POINT imageOrigin);
    void beginCrop();
    void finishCrop(bool apply, bool full = false);
    void paintCrop(HDC);
    void cropInput(UINT, LPARAM);
    std::shared_ptr<const Frame> cameraFrame();
    bool portraitMode() const { return source_.kind == SourceKind::Portrait; }
    Crop &cameraCrop() { return portraitMode() ? portraitCrop_ : crop_; }
    bool cameraMirror() const { return !portraitMode() && mirrored_; }
    double cameraAspect(const Frame *) const;
    void paintRegion(HWND, HDC);
    void notify(const std::wstring &);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT dispatch(HWND, UINT, WPARAM, LPARAM);

  public:
    explicit App(HINSTANCE, bool preview = false, const std::wstring &settingsPath = L"");
    ~App();
    int run();
};
} // namespace youcam
