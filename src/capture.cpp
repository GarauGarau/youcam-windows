#include "capture.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <cstring>

namespace youcam {
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
std::vector<Monitor> monitors() {
    std::vector<Monitor> result;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        if (GetMonitorInfoW(handle, &info)) reinterpret_cast<std::vector<Monitor>*>(data)->push_back({handle, info.rcMonitor, info.rcWork, info.szDevice});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
std::vector<Window> windows() {
    std::vector<Window> result;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() || hiddenWindow(window) || !GetWindowTextLengthW(window)) return TRUE;
        wchar_t title[512]; GetWindowTextW(window, title, 512);
        reinterpret_cast<std::vector<Window>*>(data)->push_back({window, title}); return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}
struct DesktopCapture::State {
    Source source;
    std::shared_ptr<Frames> output;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    IDirect3DDevice winDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::event_token arrived{}, closed{};
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    winrt::Windows::Graphics::SizeInt32 size{};
    void frame(const Direct3D11CaptureFramePool& sender) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        try {
            auto captured = sender.TryGetNextFrame();
            if (!captured) return;
            auto content = captured.ContentSize();
            if (content.Width <= 0 || content.Height <= 0) return;
            auto access = captured.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            check(access->GetInterface(IID_PPV_ARGS(&texture)), "Read capture texture");
            D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
            if (!staging) {
                auto cpu = description; cpu.Usage = D3D11_USAGE_STAGING; cpu.BindFlags = 0;
                cpu.CPUAccessFlags = D3D11_CPU_ACCESS_READ; cpu.MiscFlags = 0;
                check(device->CreateTexture2D(&cpu, nullptr, &staging), "Allocate full-resolution capture buffer");
            }
            D3D11_TEXTURE2D_DESC allocated{}; staging->GetDesc(&allocated);
            if (allocated.Width != description.Width || allocated.Height != description.Height) {
                staging.Reset(); captured.Close(); return;
            }
            context->CopyResource(staging.Get(), texture.Get());
            RECT crop{0, 0, std::min<LONG>(content.Width, description.Width), std::min<LONG>(content.Height, description.Height)};
            if (source.kind == SourceKind::Region) {
                MONITORINFO info{sizeof(info)};
                if (!GetMonitorInfoW(source.monitor, &info)) throw std::runtime_error("The selected monitor was disconnected.");
                crop = {source.area.left - info.rcMonitor.left, source.area.top - info.rcMonitor.top,
                        source.area.right - info.rcMonitor.left, source.area.bottom - info.rcMonitor.top};
                if (crop.left < 0 || crop.top < 0 || crop.right > content.Width || crop.bottom > content.Height)
                    throw std::runtime_error("The selected area no longer fits the display. Select it again.");
            } else if (source.kind == SourceKind::Window) {
                RECT bounds{}; GetWindowRect(source.window, &bounds);
                RECT visible{};
                if (SUCCEEDED(DwmGetWindowAttribute(source.window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))) &&
                    rectWidth(visible) == content.Width && rectHeight(visible) == content.Height) bounds = visible;
                RECT client{}; GetClientRect(source.window, &client); POINT origin{}; ClientToScreen(source.window, &origin);
                RECT desired{origin.x - bounds.left, origin.y - bounds.top, origin.x - bounds.left + client.right, origin.y - bounds.top + client.bottom};
                if (desired.left >= 0 && desired.top >= 0 && desired.right <= crop.right && desired.bottom <= crop.bottom && client.right > 0 && client.bottom > 0) crop = desired;
            }
            Frame image(uint32_t(rectWidth(crop)), uint32_t(rectHeight(crop)));
            D3D11_MAPPED_SUBRESOURCE mapped{}; check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map capture pixels");
            for (uint32_t y = 0; y < image.height; ++y)
                std::memcpy(&image.bgra[size_t(y) * image.width * 4], static_cast<BYTE*>(mapped.pData) + size_t(y + crop.top) * mapped.RowPitch + size_t(crop.left) * 4, size_t(image.width) * 4);
            context->Unmap(staging.Get(), 0);
            output->put(std::move(image));
            captured.Close();
            if (content.Width != size.Width || content.Height != size.Height) {
                size = content; staging.Reset();
                pool.Recreate(winDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            }
        } catch (const winrt::hresult_error& error) { if (!stopping) output->fail(narrow(error.message().c_str())); }
        catch (const std::exception& error) { if (!stopping) output->fail(error.what()); }
    }
};
void DesktopCapture::start(const Source& source) {
    stop(); frames->clear();
    try {
        if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Windows Graphics Capture is not available on this desktop.");
        auto state = std::make_shared<State>(); state->source = source; state->output = frames;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &state->device, nullptr, &state->context);
        if (FAILED(hr)) check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &state->device, nullptr, &state->context), "Create capture device");
        ComPtr<IDXGIDevice> dxgi; check(state->device.As(&dxgi), "Get graphics device");
        winrt::com_ptr<IInspectable> inspectable;
        check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()), "Create Windows graphics device");
        state->winDevice = inspectable.as<IDirect3DDevice>();
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (source.kind == SourceKind::Window)
            check(factory->CreateForWindow(source.window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(state->item)), "Select window capture");
        else check(factory->CreateForMonitor(source.monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(state->item)), "Select monitor capture");
        state->size = state->item.Size();
        if (source.kind == SourceKind::Screen || source.kind == SourceKind::Region) {
            MONITORINFO monitor{sizeof(monitor)};
            if (!GetMonitorInfoW(source.monitor, &monitor) || state->size.Width != rectWidth(monitor.rcMonitor) || state->size.Height != rectHeight(monitor.rcMonitor))
                throw std::runtime_error("Windows did not provide the monitor's full physical resolution. Recording was not started.");
        }
        state->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(state->winDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, state->size);
        state->session = state->pool.CreateCaptureSession(state->item);
        std::weak_ptr<State> weak = state;
        state->arrived = state->pool.FrameArrived([weak](auto const& sender, auto const&) { if (auto s = weak.lock()) s->frame(sender); });
        state->closed = state->item.Closed([weak](auto const&, auto const&) { if (auto s = weak.lock()) s->output->fail("The captured window or display was closed."); });
        state_ = state; state->session.StartCapture();
    } catch (...) { stop(); throw; }
}
void DesktopCapture::stop() {
    auto state = std::move(state_);
    if (!state) return;
    state->stopping = true;
    // Never hold the frame mutex while closing: Close may wait for a callback.
    try {
        state->pool.FrameArrived(state->arrived); state->item.Closed(state->closed);
        state->session.Close(); state->pool.Close();
    } catch (...) {}
}
} // namespace youcam
