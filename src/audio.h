#pragma once
#include "wave.h"
namespace youcam {
class Microphone {
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    mutable std::mutex mutex_;
    std::string error_;
    std::condition_variable ready_;
    bool started_ = false;
    AudioFormat format_;
    bool raw_ = false;
    void read(std::wstring id);

  public:
    std::atomic<float> peak{0};
    std::atomic<float> gainDB{0};
    std::function<void(std::vector<float>, int64_t)> samples;
    ~Microphone() {
        shutdown();
    }
    static std::vector<Device> devices();
    void start(const std::wstring &id);
    void shutdown();
    AudioFormat format();
    bool raw() const { std::lock_guard lock(mutex_); return raw_; }
    std::string error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }
};
} // namespace youcam
