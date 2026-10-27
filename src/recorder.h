#pragma once
#include "capture.h"
#include <deque>
namespace youcam {
class Movie {
    ComPtr<IMFSinkWriter> writer_;
    DWORD video_ = 0, audio_ = 0;
    uint32_t width_ = 0, height_ = 0;
    bool audioEnabled_ = false;
    void open(const std::filesystem::path&, uint32_t, uint32_t, bool, bool, bool);
    void sample(DWORD stream, const void* data, size_t bytes, int64_t timestamp, int64_t duration);
public:
    std::wstring codec;
    void start(const std::filesystem::path&, uint32_t, uint32_t, bool audio);
    void video(const Frame&, int64_t timestamp, int64_t duration);
    void audio(const std::vector<int16_t>&, int64_t timestamp);
    void finish();
};
enum class Phase { Idle, Starting, Recording, Paused, Saving };
struct RecordingState {
    Phase phase = Phase::Idle;
    std::wstring source, codec;
    std::filesystem::path saved;
    std::string error, notice;
    uint32_t width = 0, height = 0;
    int64_t elapsed = 0;
};
class Recorder {
    struct AudioPacket { std::vector<int16_t> samples; int64_t time; };
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    bool stop_ = false;
    Timeline timeline_;
    int64_t activeSince_ = 0;
    RecordingState state_;
    std::deque<AudioPacket> audio_;
    size_t queuedSamples_ = 0;
    void run(Source, Frames*, bool, std::filesystem::path);
public:
    ~Recorder() { stop(); }
    void start(Source, Frames* camera, bool audio, const std::filesystem::path&);
    void stop();
    void pause(const std::string& reason = "");
    void resume();
    void audio(std::vector<int16_t>, int64_t timestamp);
    RecordingState state() const;
};
std::filesystem::path nextMoviePath();
} // namespace youcam
