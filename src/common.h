#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "core.h"

namespace youcam {
using Microsoft::WRL::ComPtr;
inline std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n); return out;
}
inline std::string narrow(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr); return out;
}
inline void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) { std::ostringstream message; message << operation << " (0x" << std::hex << uint32_t(hr) << ")"; throw std::runtime_error(message.str()); }
}
inline int64_t clock100ns() {
    static const auto frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (t.QuadPart / frequency) * 10000000 + (t.QuadPart % frequency) * 10000000 / frequency;
}
struct Apartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Apartment() { check(hr, "Initialize COM"); }
    ~Apartment() { CoUninitialize(); }
};
struct Device { std::wstring id, name; };
class Frames {
    mutable std::mutex mutex_;
    std::shared_ptr<const Frame> frame_;
    std::string error_;
    int64_t updated_ = 0;
public:
    void put(Frame frame) { auto ptr = std::make_shared<Frame>(std::move(frame)); std::lock_guard lock(mutex_); frame_ = std::move(ptr); updated_ = clock100ns(); error_.clear(); }
    std::shared_ptr<const Frame> get(bool requireRecent = false) const {
        std::lock_guard lock(mutex_);
        if (!error_.empty()) throw std::runtime_error(error_);
        if (requireRecent && frame_ && clock100ns() - updated_ > 30000000) throw std::runtime_error("The camera stopped sending frames.");
        return frame_;
    }
    void fail(const std::string& message) { std::lock_guard lock(mutex_); error_ = message; }
    void clear() { std::lock_guard lock(mutex_); frame_.reset(); error_.clear(); updated_ = 0; }
};
inline bool hiddenWindow(HWND window) {
    DWORD cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return IsIconic(window) || !IsWindowVisible(window) || cloaked;
}
inline int rectWidth(const RECT& r) { return r.right - r.left; }
inline int rectHeight(const RECT& r) { return r.bottom - r.top; }
inline bool sameRect(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }
inline std::wstring dimensions(uint32_t w, uint32_t h) { return std::to_wstring(w) + L" × " + std::to_wstring(h); }
} // namespace youcam
