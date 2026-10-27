#pragma once
#include "common.h"
namespace youcam {
enum class SourceKind { Screen, Region, Window, Camera };
struct Source {
    SourceKind kind = SourceKind::Screen;
    HMONITOR monitor = nullptr;
    HWND window = nullptr;
    RECT area{}; // Physical desktop pixels; region is confined to one monitor.
    std::wstring name;
    bool mirror = false;
};
struct Monitor { HMONITOR handle; RECT rect, work; std::wstring name; };
struct Window { HWND handle; std::wstring name; };
std::vector<Monitor> monitors();
std::vector<Window> windows();
class DesktopCapture {
    struct State;
    std::shared_ptr<State> state_;
public:
    std::shared_ptr<Frames> frames = std::make_shared<Frames>();
    ~DesktopCapture() { stop(); }
    void start(const Source& source);
    void stop();
};
} // namespace youcam
