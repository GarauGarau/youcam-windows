#pragma once
#include "common.h"
#include <gdiplus.h>
namespace youcam {
class Drawing {
    ULONG_PTR token_ = 0;
  public:
    Drawing();
    ~Drawing();
};
constexpr int ShadowMargin = 32;
constexpr double StageMinimum = .25, StageMaximum = .98;
void drawImage(Gdiplus::Graphics &, const Frame &, Gdiplus::RectF, Crop, bool mirror, bool cover);
Frame cameraTile(const Frame *, int width, int height, int shape, Crop, bool mirror,
                 const std::wstring &message = L"");
Frame topography(int width, int height, double seconds);
Frame composeStage(const Frame &terrain, const Frame *camera, Crop, bool mirror, double scale,
                   const std::wstring &message = L"");
RECT stageCard(int width, int height, double scale, const Frame *, Crop);
RECT windowCameraRect(int width, int height, const CameraOverlay &);
Frame composeWindow(const Frame &window, const Frame &camera, const CameraOverlay &);
void blitFrame(HDC, const Frame &);
void updateLayer(HWND, const Frame &);
} // namespace youcam
