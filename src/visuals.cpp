#include "visuals.h"
#include <cstring>
namespace youcam {
using namespace Gdiplus;
Drawing::Drawing() {
    GdiplusStartupInput input;
    if (GdiplusStartup(&token_, &input, nullptr) != Ok)
        throw std::runtime_error("Cannot initialize Windows drawing.");
}
Drawing::~Drawing() { if (token_) GdiplusShutdown(token_); }
void drawImage(Graphics &g, const Frame &frame, RectF area, Crop crop, bool mirror, bool cover) {
    auto r = cropPixels(frame.width, frame.height, crop);
    float sx = float(r.x), sy = float(r.y), sw = float(r.width), sh = float(r.height);
    float scale = cover ? std::max(area.Width / sw, area.Height / sh)
                        : std::min(area.Width / sw, area.Height / sh);
    if (cover) {
        float w = area.Width / scale, h = area.Height / scale;
        sx += (sw - w) * .5f; sy += (sh - h) * .5f;
        sw = w; sh = h;
    } else {
        area.X += (area.Width - sw * scale) * .5f;
        area.Y += (area.Height - sh * scale) * .5f;
        area.Width = sw * scale; area.Height = sh * scale;
    }
    Bitmap image(frame.width, frame.height, int(frame.width * 4), PixelFormat32bppRGB,
                 const_cast<BYTE *>(frame.bgra.data()));
    PointF points[] = {{area.X + (mirror ? area.Width : 0), area.Y},
                       {area.X + (mirror ? 0 : area.Width), area.Y},
                       {area.X + (mirror ? area.Width : 0), area.Y + area.Height}};
    ImageAttributes attributes;
    attributes.SetWrapMode(WrapModeTileFlipXY);
    g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(&image, points, 3, sx, sy, sw, sh, UnitPixel, &attributes);
}
namespace {
float distanceToShape(float x, float y, float w, float h, int shape) {
    if (shape == 0)
        return std::hypot(x - w * .5f, y - h * .5f) - w * .5f;
    float radius = std::min(w, h) * (shape == 3 ? .055f : .07f);
    float qx = std::abs(x - w * .5f) - (w * .5f - radius);
    float qy = std::abs(y - h * .5f) - (h * .5f - radius);
    // Only corner pixels require a square root; most of a large camera card
    // lies on the flat interior/edges and uses the cheaper exact distance.
    return (qx > 0 && qy > 0 ? std::hypot(qx, qy) : std::max(qx, qy)) - radius;
}
}
Frame cameraTile(const Frame *frame, int width, int height, int shape, Crop crop, bool mirror,
                 const std::wstring &message) {
    Frame out(width + ShadowMargin * 2, height + ShadowMargin * 2);
    {
        Bitmap surface(out.width, out.height, int(out.width * 4), PixelFormat32bppPARGB, out.bgra.data());
        Graphics g(&surface);
        RectF area{float(ShadowMargin), float(ShadowMargin), float(width), float(height)};
        SolidBrush black(Color(255, 24, 24, 24));
        g.FillRectangle(&black, area);
        if (frame)
            drawImage(g, *frame, area, crop, mirror, true);
        else {
            Font font(L"Segoe UI", 13, FontStyleRegular, UnitPixel);
            SolidBrush ink(Color(255, 225, 225, 225));
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            RectF textArea(area.X + 20, area.Y + 20, area.Width - 40, area.Height - 40);
            g.DrawString(message.c_str(), -1, &font, textArea, &format, &ink);
        }
    }
    // Premultiplied alpha gives every shape a real translucent soft shadow.
    // A signed-distance edge also avoids jagged circle/rounded-rectangle masks.
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            float px = float(x) + .5f - ShadowMargin, py = float(y) + .5f - ShadowMargin;
            float d = distanceToShape(px, py, float(width), float(height), shape);
            auto p = &out.bgra[(size_t(y) * out.width + x) * 4];
            if (d <= -.5f) { p[3] = 255; continue; }
            float coverage = std::clamp(.5f - d, 0.f, 1.f);
            float shadowDistance = std::max(0.f, distanceToShape(px, py - 7, float(width), float(height), shape));
            float shadow = 105.f * std::exp(-shadowDistance * shadowDistance / 170.f);
            for (int c = 0; c < 3; ++c)
                p[c] = uint8_t(std::lround(p[c] * coverage));
            p[3] = uint8_t(std::lround(255 * coverage + shadow * (1 - coverage)));
        }
    return out;
}
Frame topography(int width, int height, double seconds) {
    Frame out(width, height);
    Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB, out.bgra.data());
    Graphics g(&surface);
    g.Clear(Color(255, 13, 13, 13));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    const int step = std::max(10, width / 150), nx = (width + step - 1) / step + 1,
              ny = (height + step - 1) / step + 1;
    std::vector<double> field(size_t(nx) * ny);
    double t = seconds * .035;
    const double horizontal = .7 * std::sin(t), vertical = .6 * std::cos(t * .7);
    for (int y = 0; y < ny; ++y) {
        const double v = double(y * step) / height * 5.0;
        const double bend = .4 * std::sin(v + t), ridge = .7 * std::cos(v * 1.1 - t);
        const double b = v - 2.0 - vertical;
        for (int x = 0; x < nx; ++x) {
            double u = double(x * step) / height * 5.0;
            double a = u - 2.5 - horizontal;
            field[size_t(y) * nx + x] = std::sin(u * .85 + bend) +
                ridge + .35 * std::sin(u * 1.6 + v * .9 + t * .5) +
                1.6 * std::exp(-(a * a + b * b) / 2.4);
        }
    }
    for (int level = -20; level <= 30; ++level) {
        double value = level * .14;
        GraphicsPath path;
        for (int y = 0; y < ny - 1; ++y)
            for (int x = 0; x < nx - 1; ++x) {
                double f[] = {field[size_t(y) * nx + x], field[size_t(y) * nx + x + 1],
                              field[size_t(y + 1) * nx + x + 1], field[size_t(y + 1) * nx + x]};
                PointF corners[] = {{float(x * step), float(y * step)},
                                    {float((x + 1) * step), float(y * step)},
                                    {float((x + 1) * step), float((y + 1) * step)},
                                    {float(x * step), float((y + 1) * step)}};
                PointF points[4];
                int count = 0;
                for (int edge = 0; edge < 4; ++edge) {
                    int next = (edge + 1) % 4;
                    if ((f[edge] < value) == (f[next] < value)) continue;
                    float ratio = float((value - f[edge]) / (f[next] - f[edge]));
                    points[count++] = {corners[edge].X + ratio * (corners[next].X - corners[edge].X),
                                       corners[edge].Y + ratio * (corners[next].Y - corners[edge].Y)};
                }
                if (count == 4 && (f[0] + f[1] + f[2] + f[3]) * .25 > value)
                    std::swap(points[1], points[3]);
                for (int i = 0; i + 1 < count; i += 2) {
                    path.StartFigure();
                    path.AddLine(points[i], points[i + 1]);
                }
            }
        Pen ink(level % 5 == 0 ? Color(255, 74, 74, 74) : Color(255, 43, 43, 43),
                level % 5 == 0 ? 1.25f : .85f);
        g.DrawPath(&ink, &path);
    }
    return out;
}
RECT stageCard(int width, int height, double scale, const Frame *frame, Crop crop) {
    double aspect = 16.0 / 9.0;
    if (frame) {
        auto r = cropPixels(frame->width, frame->height, crop);
        aspect = double(r.width) / r.height;
    }
    scale = std::clamp(scale, StageMinimum, StageMaximum);
    double w = width * scale, h = w / aspect;
    if (h > height * scale) { h = height * scale; w = h * aspect; }
    int iw = std::max(1, int(w)), ih = std::max(1, int(h));
    return {(width - iw) / 2, (height - ih) / 2, (width + iw) / 2, (height + ih) / 2};
}
Frame composeStage(const Frame &terrain, const Frame *frame, Crop crop, bool mirror, double scale,
                   const std::wstring &message) {
    Frame image = terrain;
    image.capturedAt = frame ? frame->capturedAt : 0;
    RECT rect = stageCard(image.width, image.height, scale, frame, crop);
    auto tile = cameraTile(frame, rectWidth(rect), rectHeight(rect), 3, crop, mirror, message);
    Bitmap surface(image.width, image.height, int(image.width * 4), PixelFormat32bppPARGB, image.bgra.data());
    Bitmap camera(tile.width, tile.height, int(tile.width * 4), PixelFormat32bppPARGB, tile.bgra.data());
    Graphics g(&surface);
    g.DrawImage(&camera, int(rect.left) - ShadowMargin, int(rect.top) - ShadowMargin);
    return image;
}
RECT windowCameraRect(int width, int height, const CameraOverlay &overlay) {
    double aspect = overlay.shape == 2 ? 16.0 / 9.0 : 1.0;
    int inset = std::min(ShadowMargin + 8, std::max(0, std::min(width, height) / 5));
    int availableWidth = std::max(1, width - 2 * inset);
    int availableHeight = std::max(1, height - 2 * inset);
    int w = std::clamp(int(std::lround(width * std::clamp(overlay.size, .08, .75))),
                       1, availableWidth);
    int h = std::max(1, int(std::lround(w / aspect)));
    if (h > availableHeight) {
        h = availableHeight;
        w = std::max(1, int(std::lround(h * aspect)));
    }
    int x = inset + int(std::lround((availableWidth - w) * std::clamp(overlay.x, 0.0, 1.0)));
    int y = inset + int(std::lround((availableHeight - h) * std::clamp(overlay.y, 0.0, 1.0)));
    return {x, y, x + w, y + h};
}
Frame composeWindow(const Frame &window, const Frame &camera, const CameraOverlay &overlay) {
    Frame image = window;
    if (!overlay.enabled)
        return image;
    image.capturedAt = camera.capturedAt;
    RECT rect = windowCameraRect(image.width, image.height, overlay);
    auto tile = cameraTile(&camera, rectWidth(rect), rectHeight(rect), overlay.shape,
                           overlay.crop, overlay.mirror);
    // Window-capture alpha is driver-dependent. RGB32 treats the captured image
    // as opaque, while the webcam tile retains its premultiplied shadow alpha.
    Bitmap surface(image.width, image.height, int(image.width * 4), PixelFormat32bppRGB, image.bgra.data());
    Bitmap cameraImage(tile.width, tile.height, int(tile.width * 4), PixelFormat32bppPARGB, tile.bgra.data());
    Graphics g(&surface);
    g.DrawImage(&cameraImage, int(rect.left) - ShadowMargin, int(rect.top) - ShadowMargin);
    return image;
}
void blitFrame(HDC dc, const Frame &frame) {
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(frame.width), -LONG(frame.height), 1, 32, BI_RGB};
    SetDIBitsToDevice(dc, 0, 0, frame.width, frame.height, 0, 0, 0, frame.height, frame.bgra.data(),
                      &info, DIB_RGB_COLORS);
}
void updateLayer(HWND window, const Frame &frame) {
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(frame.width), -LONG(frame.height), 1, 32, BI_RGB};
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !memory) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        throw std::runtime_error("Cannot allocate camera display.");
    }
    auto previous = SelectObject(memory, bitmap);
    std::memcpy(bits, frame.bgra.data(), frame.bgra.size());
    SIZE size{LONG(frame.width), LONG(frame.height)};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL ok = UpdateLayeredWindow(window, screen, nullptr, &size, memory, &origin, 0, &blend, ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
    if (!ok) throw std::runtime_error("Cannot update translucent camera display.");
}
} // namespace youcam
