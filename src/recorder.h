#pragma once
#include "capture.h"
#include "wave.h"
#include <deque>
namespace youcam {
class Movie {
    ComPtr<IMFSinkWriter> writer_;
    DWORD video_ = 0, audio_ = 0;
    uint32_t width_ = 0, height_ = 0;
    bool audioEnabled_ = false;
    bool audioStarted_ = false;
    int64_t videoFrames_ = 0;
    AudioFormat audioFormat_;
    WaveMaster master_;
    std::filesystem::path masterPath_;
    void open(const std::filesystem::path &, uint32_t, uint32_t, bool, bool, bool);
    void sample(DWORD stream, const void *data, size_t bytes, int64_t timestamp, int64_t duration);

  public:
    std::wstring codec;
    void start(const std::filesystem::path &, uint32_t, uint32_t, bool audio, AudioFormat = {});
    // Supply each source image's interval on the shared recording clock.
    // The writer resamples those intervals to a continuous 30 fps grid.
    void video(const Frame &, int64_t timestamp, int64_t duration);
    void audio(const std::vector<float> &, int64_t timestamp);
    void finish();
    void discardEmptyMaster();
};
enum class Phase { Idle, Starting, Recording, Paused, Saving };
struct RecordingState {
    uint64_t serial = 0;
    Phase phase = Phase::Idle;
    std::wstring source, codec;
    std::filesystem::path saved, audioMaster;
    std::string error, notice;
    uint32_t width = 0, height = 0;
    int64_t elapsed = 0;
};
class Recorder {
    struct AudioPacket {
        std::vector<float> samples;
        int64_t time;
    };
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    bool stop_ = false;
    Timeline timeline_;
    int64_t activeSince_ = 0;
    RecordingState state_;
    CameraOverlay cameraOverlay_;
    uint64_t serial_ = 0;
    std::deque<AudioPacket> audio_;
    size_t queuedSamples_ = 0;
    AudioFormat audioFormat_;
    void run(Source, Frames *, bool, std::filesystem::path);

  public:
    ~Recorder() {
        stop();
    }
    // For Stage, pass the rendered scene frames; nullptr retains WGC capture.
    // For Camera/Window, pass the webcam's native frames.
    void start(Source, Frames *camera, bool audio, const std::filesystem::path &, AudioFormat = {});
    void stop(const std::string &error = "");
    void pause(const std::string &reason = "");
    void resume();
    void audio(std::vector<float>, int64_t timestamp);
    void setCameraOverlay(CameraOverlay);
    RecordingState state() const;
};
std::filesystem::path nextMoviePath();
} // namespace youcam
