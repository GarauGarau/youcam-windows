#pragma once
#include "common.h"

namespace youcam {
struct AudioFormat {
    uint32_t rate = 48000;
    uint16_t channels = 1;
    uint32_t channelMask = 4;
};
// Preserve every captured float, including gain headroom and separate channels.
// Reserve ds64 space so long recordings can become RF64 without moving audio.
class WaveMaster {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    AudioFormat format_;
    uint64_t frames_ = 0;
    void write(const void *, size_t);
    void seek(uint64_t);
    void silence(uint64_t);
    void header();
  public:
    WaveMaster() = default;
    WaveMaster(const WaveMaster &) = delete;
    WaveMaster &operator=(const WaveMaster &) = delete;
    ~WaveMaster();
    void open(const std::filesystem::path &, AudioFormat);
    void append(const std::vector<float> &, int64_t timestamp);
    void finish(int64_t duration = -1);
};
inline std::filesystem::path wavePath(std::filesystem::path path) {
    path.replace_extension(L".wav");
    return path;
}
} // namespace youcam
