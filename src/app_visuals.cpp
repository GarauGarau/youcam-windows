#include "app.h"
#include <windowsx.h>
namespace youcam {
using namespace Gdiplus;
using namespace commands;
std::shared_ptr<const Frame> App::cameraFrame() {
    try { return camera_.frames.get(!preview_); }
    catch (const std::exception &e) { cameraError_ = e.what(); return {}; }
}
void App::renderCamera() {
    if (!overlay_) return;
    auto frame = cameraFrame();
    int width = cameraWidth_;
    int height = shape_ == 2 ? width * 9 / 16 : width;
    if (windowCameraActive() && rectWidth(windowPreviewRect_) > 0) {
        width = rectWidth(windowPreviewRect_);
        height = rectHeight(windowPreviewRect_);
    }
    auto image = cameraTile(frame.get(), width, height, shape_, crop_, mirrored_,
                            cameraError_.empty() ? L"Starting camera…" : wide(cameraError_));
    updateLayer(overlay_, image);
}
bool App::windowCameraActive() const {
    return source_.kind == SourceKind::Window && windowCamera_.enabled;
}
CameraOverlay App::windowCameraSettings() const {
    auto overlay = windowCamera_;
    overlay.shape = shape_;
    overlay.mirror = mirrored_;
    overlay.crop = crop_;
    return overlay;
}
void App::syncWindowCamera() {
    if (source_.kind == SourceKind::Stage)
        return;
    if (!windowCameraActive()) {
        if (windowCameraAttached_) {
            windowCameraAttached_ = false;
            SetWindowPos(overlay_, nullptr, floatingCameraRect_.left, floatingCameraRect_.top,
                         rectWidth(floatingCameraRect_), rectHeight(floatingCameraRect_),
                         SWP_NOACTIVATE | SWP_NOZORDER);
            resizeCamera(cameraWidth_);
            ShowWindow(overlay_, cameraVisible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        return;
    }
    if (!windowCameraAttached_) {
        GetWindowRect(overlay_, &floatingCameraRect_);
        windowCameraAttached_ = true;
    }
    if (!IsWindow(source_.window) || hiddenWindow(source_.window)) {
        ShowWindow(overlay_, SW_HIDE);
        return;
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(source_.window, &client) || !ClientToScreen(source_.window, &origin) ||
        client.right <= 0 || client.bottom <= 0) {
        ShowWindow(overlay_, SW_HIDE);
        return;
    }
    windowPreviewRect_ = windowCameraRect(client.right, client.bottom, windowCameraSettings());
    RECT desired{origin.x + windowPreviewRect_.left - ShadowMargin,
                 origin.y + windowPreviewRect_.top - ShadowMargin,
                 origin.x + windowPreviewRect_.right + ShadowMargin,
                 origin.y + windowPreviewRect_.bottom + ShadowMargin};
    RECT current{};
    GetWindowRect(overlay_, &current);
    if (!sameRect(current, desired))
        SetWindowPos(overlay_, HWND_TOPMOST, desired.left, desired.top, rectWidth(desired),
                     rectHeight(desired), SWP_NOACTIVATE);
    if (bool(IsWindowVisible(overlay_)) != cameraVisible_)
        ShowWindow(overlay_, cameraVisible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
}
void App::moveWindowCamera(POINT imageOrigin) {
    if (!windowCameraActive())
        return;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(source_.window, &client) || !ClientToScreen(source_.window, &origin) ||
        client.right <= 0 || client.bottom <= 0)
        return;
    auto settings = windowCameraSettings();
    settings.x = settings.y = 0;
    auto first = windowCameraRect(client.right, client.bottom, settings);
    settings.x = settings.y = 1;
    auto last = windowCameraRect(client.right, client.bottom, settings);
    if (last.left != first.left)
        windowCamera_.x = std::clamp(double(imageOrigin.x - origin.x - first.left) /
                                    (last.left - first.left), 0.0, 1.0);
    if (last.top != first.top)
        windowCamera_.y = std::clamp(double(imageOrigin.y - origin.y - first.top) /
                                    (last.top - first.top), 0.0, 1.0);
    syncWindowCamera();
    recorder_.setCameraOverlay(windowCameraSettings());
}
void App::changeStageScale(double delta) {
    stageScale_ = std::clamp(stageScale_ + delta, StageMinimum, StageMaximum);
    saveSettings();
    InvalidateRect(stage_, nullptr, FALSE);
}
void App::cameraMenu(POINT point) {
    if (recorder_.state().phase != Phase::Idle) return;
    cameras_ = Camera::devices();
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < cameras_.size() && i < 900; ++i)
        AppendMenuW(menu, MF_STRING | (cameras_[i].id == cameraID_ ? MF_CHECKED : 0),
                    CameraBase + i, cameras_[i].name.c_str());
    if (cameras_.empty()) AppendMenuW(menu, MF_GRAYED, 0, L"No camera connected");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Retry, L"Retry camera");
    int id = int(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
                               0, panel_, nullptr));
    DestroyMenu(menu);
    if (id) command(id);
}
void App::toggleStage() {
    if (cropping_ || !allowSourceChange()) return;
    if (source_.kind == SourceKind::Stage) {
        leaveStage();
    } else {
        beforeStage_ = source_;
        cameraBeforeStage_ = cameraVisible_;
        if (!camera_.mode.pixels() && !preview_) selectCamera(cameraID_);
        MONITORINFO info{sizeof(info)};
        HMONITOR monitor = source_.monitor ? source_.monitor : MonitorFromWindow(overlay_, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &info)) return;
        source_ = {SourceKind::Stage, monitor, stage_, info.rcMonitor, L"Camera stage"};
        terrainTick_ = -1;
        stageFrames_.clear();
        ShowWindow(overlay_, SW_HIDE);
        ShowWindow(editor_, SW_HIDE);
        ShowWindow(editorTools_, SW_HIDE);
        showPanel(false);
        SetWindowPos(stage_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     rectWidth(info.rcMonitor), rectHeight(info.rcMonitor), SWP_SHOWWINDOW);
        SetForegroundWindow(stage_);
        SetFocus(stage_);
        InvalidateRect(stage_, nullptr, FALSE);
    }
    saveSettings();
    update();
}
void App::leaveStage() {
    ShowWindow(stage_, SW_HIDE);
    terrain_ = {};
    stageFrames_.clear();
    terrainTick_ = -1;
    source_ = beforeStage_;
    cameraVisible_ = cameraBeforeStage_;
    ShowWindow(overlay_, cameraVisible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (!cameraVisible_ && source_.kind != SourceKind::Camera && !windowCameraActive() && !preview_)
        camera_.shutdown();
    showEditor();
}
void App::paintStage(HDC dc) {
    RECT bounds{};
    GetClientRect(stage_, &bounds);
    int width = rectWidth(bounds), height = rectHeight(bounds);
    if (width <= 0 || height <= 0) return;
    int64_t tick = clock100ns() / 1000000; // Terrain at 10 fps; live camera at 30 fps.
    if (tick != terrainTick_ || terrain_.width != uint32_t(width) || terrain_.height != uint32_t(height)) {
        terrain_ = topography(width, height, tick * .1);
        terrainTick_ = tick;
    }
    auto frame = cameraFrame();
    auto image = composeStage(terrain_, frame.get(), crop_, mirrored_, stageScale_,
                               cameraError_.empty() ? L"Starting camera…" : wide(cameraError_));
    blitFrame(dc, image);
    // Record the exact composition with the camera's capture timestamp, before
    // a second trip through DWM/Windows Graphics Capture can add latency.
    stageFrames_.put(std::move(image));
}
void App::beginCrop() {
    if (cropping_ || !allowSourceChange()) return;
    auto frame = cameraFrame();
    if (!frame) { error("Wait for the camera before cropping."); return; }
    pendingCrop_ = mirrored_ ? mirroredCrop(crop_) : crop_;
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(source_.kind == SourceKind::Stage ? stage_ : overlay_,
                                     MONITOR_DEFAULTTONEAREST), &info);
    int width = std::min(1000, rectWidth(info.rcWork) - 60),
        height = std::min(740, rectHeight(info.rcWork) - 60);
    cropping_ = true;
    for (HWND window : {overlay_, panel_, stage_}) EnableWindow(window, FALSE);
    SetWindowPos(cropEditor_, HWND_TOPMOST, info.rcWork.left + (rectWidth(info.rcWork) - width) / 2,
                 info.rcWork.top + (rectHeight(info.rcWork) - height) / 2, width, height, SWP_SHOWWINDOW);
    RECT client{};
    GetClientRect(cropEditor_, &client);
    MoveWindow(GetDlgItem(cropEditor_, CropFull), 20, client.bottom - 48, 130, 30, TRUE);
    MoveWindow(GetDlgItem(cropEditor_, CropApply), client.right - 284, client.bottom - 48, 130, 30, TRUE);
    MoveWindow(GetDlgItem(cropEditor_, CropCancel), client.right - 144, client.bottom - 48, 124, 30, TRUE);
    SetForegroundWindow(cropEditor_);
    SetFocus(cropEditor_);
    InvalidateRect(cropEditor_, nullptr, FALSE);
}
void App::finishCrop(bool apply, bool full) {
    if (!cropping_) return;
    if (apply) {
        crop_ = full ? Crop{} : validCrop(mirrored_ ? mirroredCrop(pendingCrop_) : pendingCrop_);
        saveSettings();
    }
    ReleaseCapture();
    cropping_ = cropDragging_ = false;
    ShowWindow(cropEditor_, SW_HIDE);
    for (HWND window : {overlay_, panel_, stage_}) EnableWindow(window, TRUE);
    if (source_.kind == SourceKind::Stage) SetForegroundWindow(stage_);
    else renderCamera();
    update();
}
void App::paintCrop(HDC dc) {
    RECT client{};
    GetClientRect(cropEditor_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    Frame image(client.right, client.bottom);
    auto frame = cameraFrame();
    {
        Bitmap surface(image.width, image.height, int(image.width * 4), PixelFormat32bppPARGB, image.bgra.data());
        Graphics g(&surface);
        g.Clear(Color(255, 24, 24, 24));
        Font font(L"Segoe UI", 14, FontStyleRegular, UnitPixel);
        SolidBrush text(Color(255, 220, 220, 220)), shade(Color(155, 0, 0, 0));
        g.DrawString(L"Drag to select. Drag inside to move; corners to resize. Enter applies, Esc cancels.",
                      -1, &font, PointF(20, 16), &text);
        if (frame) {
            float scale = std::min(float(client.right - 40) / frame->width,
                                   float(client.bottom - 120) / frame->height);
            int w = int(frame->width * scale), h = int(frame->height * scale);
            cropImage_ = {(client.right - w) / 2, 48 + (client.bottom - 120 - h) / 2,
                           (client.right + w) / 2, 48 + (client.bottom - 120 + h) / 2};
            RectF imageRect(float(cropImage_.left), float(cropImage_.top), float(w), float(h));
            drawImage(g, *frame, imageRect, {}, mirrored_, false);
            RectF selected(float(cropImage_.left + pendingCrop_.left * w),
                            float(cropImage_.top + pendingCrop_.top * h),
                            float((pendingCrop_.right - pendingCrop_.left) * w),
                            float((pendingCrop_.bottom - pendingCrop_.top) * h));
            auto saved = g.Save();
            g.SetClip(imageRect);
            g.ExcludeClip(selected);
            g.FillRectangle(&shade, imageRect);
            g.Restore(saved);
            Pen outline(Color(255, 255, 255, 255), 1.5f);
            g.DrawRectangle(&outline, selected);
            Pen grid(Color(85, 255, 255, 255), 1.f);
            for (int i = 1; i < 3; ++i) {
                float x = selected.X + selected.Width * i / 3, y = selected.Y + selected.Height * i / 3;
                g.DrawLine(&grid, x, selected.Y, x, selected.GetBottom());
                g.DrawLine(&grid, selected.X, y, selected.GetRight(), y);
            }
            for (auto p : {PointF(selected.X, selected.Y), PointF(selected.GetRight(), selected.Y),
                           PointF(selected.GetRight(), selected.GetBottom()), PointF(selected.X, selected.GetBottom())})
                g.FillRectangle(&text, p.X - 5, p.Y - 5, 10.f, 10.f);
        }
    }
    blitFrame(dc, image);
}
void App::cropInput(UINT message, LPARAM lparam) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    int w = rectWidth(cropImage_), h = rectHeight(cropImage_);
    if (w <= 0 || h <= 0) return;
    auto normalized = [&](POINT p) {
        return PointF(float(std::clamp(double(p.x - cropImage_.left) / w, 0.0, 1.0)),
                       float(std::clamp(double(p.y - cropImage_.top) / h, 0.0, 1.0)));
    };
    if (message == WM_LBUTTONDOWN) {
        if (!PtInRect(&cropImage_, point)) return;
        cropStart_ = point;
        cropDragStart_ = pendingCrop_;
        cropHandle_ = -1;
        PointF corners[] = {{float(pendingCrop_.left), float(pendingCrop_.top)},
                            {float(pendingCrop_.right), float(pendingCrop_.top)},
                            {float(pendingCrop_.right), float(pendingCrop_.bottom)},
                            {float(pendingCrop_.left), float(pendingCrop_.bottom)}};
        for (int i = 0; i < 4; ++i)
            if (std::abs(point.x - (cropImage_.left + corners[i].X * w)) < 14 &&
                std::abs(point.y - (cropImage_.top + corners[i].Y * h)) < 14) cropHandle_ = i;
        auto p = normalized(point);
        if (cropHandle_ < 0 && !pendingCrop_.full() && p.X > pendingCrop_.left && p.X < pendingCrop_.right &&
            p.Y > pendingCrop_.top && p.Y < pendingCrop_.bottom) cropHandle_ = 4;
        cropDragging_ = true;
        SetFocus(cropEditor_);
        SetCapture(cropEditor_);
    } else if (message == WM_MOUSEMOVE && cropDragging_) {
        auto p = normalized(point), start = normalized(cropStart_);
        Crop c = cropDragStart_;
        if (cropHandle_ == 4) {
            double dx = std::clamp(double(p.X - start.X), -c.left, 1 - c.right),
                   dy = std::clamp(double(p.Y - start.Y), -c.top, 1 - c.bottom);
            c.left += dx; c.right += dx; c.top += dy; c.bottom += dy;
        } else if (cropHandle_ < 0) {
            c = {std::min(p.X, start.X), std::min(p.Y, start.Y),
                  std::max(p.X, start.X), std::max(p.Y, start.Y)};
        } else {
            if (cropHandle_ == 0 || cropHandle_ == 3) c.left = std::min(double(p.X), c.right - .02);
            else c.right = std::max(double(p.X), c.left + .02);
            if (cropHandle_ == 0 || cropHandle_ == 1) c.top = std::min(double(p.Y), c.bottom - .02);
            else c.bottom = std::max(double(p.Y), c.top + .02);
        }
        pendingCrop_ = validCrop(c);
        InvalidateRect(cropEditor_, nullptr, FALSE);
    } else if (message == WM_LBUTTONUP && cropDragging_) {
        cropDragging_ = false;
        ReleaseCapture();
    }
}
} // namespace youcam
