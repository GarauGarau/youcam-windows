#include "wave.h"
#include <cstring>
#include <ks.h>
#include <ksmedia.h>

namespace youcam {
namespace {
constexpr uint64_t HeaderSize = 116; // RIFF, reserved ds64, extensible fmt, fact, data.
template<class T> void put(std::vector<uint8_t> &header, size_t offset, T value) {
    std::memcpy(header.data() + offset, &value, sizeof(value));
}
}
WaveMaster::~WaveMaster() {
    try { finish(); } catch (...) { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
}
void WaveMaster::write(const void *data, size_t bytes) {
    DWORD written = 0;
    if (bytes > MAXDWORD || !WriteFile(file_, data, DWORD(bytes), &written, nullptr) || written != bytes)
        throw std::runtime_error("Cannot write lossless WAV. Check free disk space.");
}
void WaveMaster::seek(uint64_t offset) {
    LARGE_INTEGER location;
    location.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(file_, location, nullptr, FILE_BEGIN))
        throw std::runtime_error("Cannot seek in lossless WAV.");
}
void WaveMaster::header() {
    const uint64_t bytes = frames_ * format_.channels * sizeof(float);
    const uint64_t riffSize = bytes + HeaderSize - 8;
    const bool rf64 = riffSize > MAXDWORD;
    std::vector<uint8_t> h(HeaderSize, 0);
    std::memcpy(h.data(), rf64 ? "RF64" : "RIFF", 4);
    put(h, 4, rf64 ? MAXDWORD : DWORD(riffSize));
    std::memcpy(h.data() + 8, "WAVE", 4);
    std::memcpy(h.data() + 12, rf64 ? "ds64" : "JUNK", 4);
    put(h, 16, uint32_t(28));
    if (rf64) { put(h, 20, riffSize); put(h, 28, bytes); put(h, 36, frames_); }
    std::memcpy(h.data() + 48, "fmt ", 4);
    put(h, 52, uint32_t(40));
    put(h, 56, uint16_t(WAVE_FORMAT_EXTENSIBLE));
    put(h, 58, format_.channels);
    put(h, 60, format_.rate);
    put(h, 64, uint32_t(format_.rate * format_.channels * 4));
    put(h, 68, uint16_t(format_.channels * 4));
    put(h, 70, uint16_t(32));
    put(h, 72, uint16_t(22));
    put(h, 74, uint16_t(32));
    put(h, 76, format_.channelMask);
    put(h, 80, GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    std::memcpy(h.data() + 96, "fact", 4);
    put(h, 100, uint32_t(4));
    put(h, 104, DWORD(std::min<uint64_t>(frames_, MAXDWORD)));
    std::memcpy(h.data() + 108, "data", 4);
    put(h, 112, rf64 ? MAXDWORD : DWORD(bytes));
    seek(0);
    write(h.data(), h.size());
}
void WaveMaster::open(const std::filesystem::path &path, AudioFormat format) {
    finish();
    if (!format.channels || format.channels > 32 || !format.rate || format.rate > 384000)
        throw std::runtime_error("Invalid microphone format for WAV.");
    format_ = format;
    frames_ = 0;
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create lossless WAV (file exists or folder is not writable).");
    header();
}
void WaveMaster::silence(uint64_t frames) {
    std::vector<float> zeros(size_t(format_.rate / 10) * format_.channels, 0);
    while (frames) {
        auto count = std::min<uint64_t>(frames, zeros.size() / format_.channels);
        write(zeros.data(), size_t(count) * format_.channels * 4);
        frames_ += count;
        frames -= count;
    }
}
void WaveMaster::append(const std::vector<float> &samples, int64_t timestamp) {
    if (file_ == INVALID_HANDLE_VALUE) return;
    if (timestamp < 0 || samples.size() % format_.channels)
        throw std::runtime_error("Invalid lossless audio packet.");
    // Round to the nearest sample; flooring 100 ns timestamps would lose samples.
    const auto start = uint64_t((timestamp * format_.rate + 5000000) / 10000000);
    if (start > frames_) silence(start - frames_);
    const auto skip = std::min<uint64_t>(frames_ - start, samples.size() / format_.channels);
    const auto count = samples.size() - size_t(skip) * format_.channels;
    if (count) write(samples.data() + size_t(skip) * format_.channels, count * 4);
    frames_ += count / format_.channels;
}
void WaveMaster::finish(int64_t duration) {
    if (file_ == INVALID_HANDLE_VALUE) return;
    if (duration >= 0) {
        const auto end = uint64_t((duration * format_.rate + 5000000) / 10000000);
        if (end > frames_) silence(end - frames_);
        frames_ = end;
        seek(HeaderSize + frames_ * format_.channels * 4);
        if (!SetEndOfFile(file_)) throw std::runtime_error("Cannot finish lossless WAV data.");
    }
    header();
    const bool flushed = FlushFileBuffers(file_) != FALSE;
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    if (!flushed) throw std::runtime_error("Cannot flush lossless WAV to disk.");
}
} // namespace youcam
