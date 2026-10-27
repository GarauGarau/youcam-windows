#pragma once
#include "common.h"
namespace youcam {
class Camera {
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    std::thread thread_;
    std::atomic<bool> stop_{true};
    void read();
public:
    Frames frames;
    VideoMode mode;
    std::wstring name;
    ~Camera() { shutdown(); }
    static std::vector<Device> devices();
    void start(const std::wstring& id);
    void shutdown();
};
} // namespace youcam
