#pragma once
#include "common.h"
namespace youcam {
class Microphone {
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    mutable std::mutex mutex_;
    std::string error_;
    void read(std::wstring id);
public:
    std::atomic<float> peak{0};
    std::atomic<float> gainDB{0};
    std::function<void(std::vector<int16_t>, int64_t)> samples;
    ~Microphone() { shutdown(); }
    static std::vector<Device> devices();
    void start(const std::wstring& id);
    void shutdown();
    std::string error() const { std::lock_guard lock(mutex_); return error_; }
};
} // namespace youcam
