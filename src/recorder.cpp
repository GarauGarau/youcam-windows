#include "recorder.h"
#include "visuals.h"
#include <codecapi.h>
#include <cstring>
#include <iomanip>

namespace youcam {
namespace {
ComPtr<IMFMediaType> videoType(REFGUID subtype, uint32_t width, uint32_t height) {
    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "Create video format");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video type");
    check(type->SetGUID(MF_MT_SUBTYPE, subtype), "Set video codec");
    check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "Set full video resolution");
    check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30, 1), "Set video rate");
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return type;
}
} // namespace
void Movie::open(const std::filesystem::path &path, uint32_t width, uint32_t height, bool audio,
                 bool hardware, bool hevc) {
    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, 4), "Create encoder attributes");
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware);
    attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer_),
          "Create MP4 recording");
    auto output = videoType(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, width, height);
    UINT32 bitrate = UINT32(std::clamp<uint64_t>(uint64_t(width) * height * 30 / 4, 2000000, 400000000));
    output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (!hevc)
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    check(writer_->AddStream(output.Get(), &video_), "Configure native-resolution video encoder");
    auto input = videoType(MFVideoFormat_NV12, width, height);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    check(writer_->SetInputMediaType(video_, input.Get(), nullptr), "Connect full-resolution video input");
    if (audio) {
        ComPtr<IMFMediaType> compressed;
        check(MFCreateMediaType(&compressed), "Create AAC type");
        compressed->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        compressed->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        compressed->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, std::min<uint32_t>(2, audioFormat_.channels));
        compressed->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioFormat_.rate == 44100 ? 44100 : 48000);
        compressed->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        compressed->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        compressed->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        compressed->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        compressed->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        check(writer_->AddStream(compressed.Get(), &audio_), "Configure AAC encoder");
        ComPtr<IMFMediaType> pcm;
        check(MFCreateMediaType(&pcm), "Create audio format");
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audioFormat_.channels);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioFormat_.rate);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audioFormat_.channels * 4);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audioFormat_.rate * audioFormat_.channels * 4);
        pcm->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, audioFormat_.channelMask);
        check(writer_->SetInputMediaType(audio_, pcm.Get(), nullptr), "Connect microphone encoder");
    }
    check(writer_->BeginWriting(), "Start native video encoder");
    codec = hevc ? L"HEVC" : L"H.264";
    width_ = width;
    height_ = height;
    audioEnabled_ = audio;
    audioStarted_ = false;
    videoFrames_ = 0;
}
void Movie::start(const std::filesystem::path &path, uint32_t width, uint32_t height, bool audio, AudioFormat format) {
    audioFormat_ = format;
    masterPath_.clear();
    if (audio) {
        master_.open(wavePath(path), format);
        masterPath_ = wavePath(path);
    }
    std::string error;
    // Preserve dimensions for every attempt. Allow Windows to select a GPU MFT,
    // then try its software H.264 encoder; HEVC can support larger GPU modes.
    for (auto choice : {std::pair{true, false}, std::pair{false, false}, std::pair{true, true}}) {
        try {
            open(path, width, height, audio, choice.first, choice.second);
            return;
        } catch (const std::exception &failed) {
            error = failed.what();
            writer_.Reset();
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
    throw std::runtime_error("No installed Windows encoder accepted " + narrow(dimensions(width, height)) +
                             ". Resolution was not reduced. " + error);
}
void Movie::sample(DWORD stream, const void *data, size_t bytes, int64_t time, int64_t duration) {
    if (bytes > MAXDWORD)
        throw std::runtime_error("Frame exceeds Media Foundation's buffer size.");
    ComPtr<IMFMediaBuffer> buffer;
    check(MFCreateMemoryBuffer(DWORD(bytes), &buffer), "Allocate encoder buffer");
    BYTE *destination = nullptr;
    check(buffer->Lock(&destination, nullptr, nullptr), "Lock encoder buffer");
    std::memcpy(destination, data, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(bytes));
    ComPtr<IMFSample> sample;
    check(MFCreateSample(&sample), "Create encoder sample");
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);
    check(writer_->WriteSample(stream, sample.Get()), "Encode recording sample");
}
void Movie::video(const Frame &frame, int64_t timestamp, int64_t duration) {
    if (frame.width != width_ || frame.height != height_)
        throw std::runtime_error("Video resolution changed unexpectedly.");
    if (timestamp < 0 || duration <= 0)
        throw std::runtime_error("Invalid video interval.");
    // A nominal MF_MT_FRAME_RATE does not convert variable-duration samples.
    // Keep the capture clock, rounding each transition to the nearest frame;
    // duplicate images when capture is slow instead of stretching one sample.
    const auto endFrame = std::max<int64_t>(1, ((timestamp + duration) * 30 + 5000000) / 10000000);
    if (endFrame <= videoFrames_)
        return;
    // Convert once even when this image covers multiple output frames.
    toNV12(frame, videoBuffer_);
    while (videoFrames_ < endFrame) {
        // Use rational endpoints, not accumulated 333333-tick durations. Round
        // up to 100 ns so MP4 timescale conversion does not put frames early.
        const auto time = (videoFrames_ * 10000000 + 29) / 30;
        const auto next = ((videoFrames_ + 1) * 10000000 + 29) / 30;
        sample(video_, videoBuffer_.data(), videoBuffer_.size(), time, next - time);
        ++videoFrames_;
    }
}
void Movie::audio(const std::vector<float> &samples, int64_t timestamp) {
    if (!audioEnabled_ || samples.empty())
        return;
    master_.append(samples, timestamp);
    const auto rate = audioFormat_.rate;
    const auto channels = audioFormat_.channels;
    if (!audioStarted_) {
        // Microphone startup can lag video. Represent that real delay with
        // silence, so editors see both tracks beginning at zero, without
        // relying on an MP4 edit list or moving speech earlier.
        const int64_t silenceSamples = std::max<int64_t>(0, timestamp * rate / 10000000);
        std::vector<float> silence(size_t(rate / 10) * channels, 0);
        for (int64_t written = 0; written < silenceSamples;) {
            const auto count = std::min<int64_t>(int64_t(silence.size() / channels), silenceSamples - written);
            const auto time = written * 10000000 / rate;
            const auto end = (written + count) * 10000000 / rate;
            sample(audio_, silence.data(), size_t(count) * channels * 4, time, end - time);
            written += count;
        }
        audioStarted_ = true;
    }
    sample(audio_, samples.data(), samples.size() * 4, timestamp,
           int64_t(samples.size() / channels) * 10000000 / rate);
}
void Movie::finish() {
    std::exception_ptr waveError;
    try { master_.finish(videoFrames_ ? (videoFrames_ * 10000000 + 29) / 30 : -1); }
    catch (...) { waveError = std::current_exception(); }
    if (writer_) {
        HRESULT hr = writer_->Finalize();
        writer_.Reset();
        check(hr, "Finalize MP4 recording");
    }
    if (waveError) std::rethrow_exception(waveError);
}
void Movie::discardEmptyMaster() {
    if (!masterPath_.empty()) {
        master_.finish();
        std::error_code ignored;
        std::filesystem::remove(masterPath_, ignored);
        masterPath_.clear();
    }
}
std::filesystem::path nextMoviePath() {
    PWSTR desktop = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_CREATE, nullptr, &desktop), "Find Desktop folder");
    std::filesystem::path directory(desktop);
    CoTaskMemFree(desktop);
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t name[128];
    swprintf_s(name, L"YouCam-%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
               time.wMinute, time.wSecond);
    auto path = directory / (std::wstring(name) + L".mp4");
    for (int suffix = 2; std::filesystem::exists(path) || std::filesystem::exists(wavePath(path)); ++suffix)
        path = directory / (std::wstring(name) + L"-" + std::to_wstring(suffix) + L".mp4");
    return path;
}
RecordingState Recorder::state() const {
    std::lock_guard lock(mutex_);
    auto result = state_;
    if (state_.phase == Phase::Recording || state_.phase == Phase::Paused)
        result.elapsed = timeline_.time(clock100ns());
    return result;
}
void Recorder::start(Source source, Frames *camera, bool audio, const std::filesystem::path &path, AudioFormat format) {
    if (state().phase != Phase::Idle)
        return;
    if (thread_.joinable())
        thread_.join();
    {
        std::lock_guard lock(mutex_);
        audioFormat_ = format;
        state_ = {};
        state_.serial = ++serial_;
        state_.phase = Phase::Starting;
        state_.source = source.name;
        cameraOverlay_ = source.cameraOverlay;
        timeline_.start(clock100ns());
        stop_ = false;
        audio_.clear();
        queuedSamples_ = 0;
    }
    thread_ = std::thread([this, source, camera, audio, path] { run(source, camera, audio, path); });
}
void Recorder::setCameraOverlay(CameraOverlay overlay) {
    std::lock_guard lock(mutex_);
    cameraOverlay_ = overlay;
}
void Recorder::pause(const std::string &reason) {
    std::lock_guard lock(mutex_);
    if (state_.phase != Phase::Recording)
        return;
    timeline_.pause(clock100ns());
    state_.phase = Phase::Paused;
    state_.notice = reason;
    condition_.notify_all();
}
void Recorder::resume() {
    std::lock_guard lock(mutex_);
    if (state_.phase != Phase::Paused)
        return;
    activeSince_ = clock100ns();
    timeline_.resume(activeSince_);
    state_.phase = Phase::Recording;
    state_.notice.clear();
    condition_.notify_all();
}
void Recorder::stop(const std::string &error) {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        timeline_.pause(clock100ns());
        if (!error.empty())
            state_.error = error;
        condition_.notify_all();
    }
    if (thread_.joinable())
        thread_.join();
}
void Recorder::audio(std::vector<float> samples, int64_t timestamp) {
    std::lock_guard lock(mutex_);
    if (state_.phase != Phase::Recording || stop_)
        return;
    const auto rate = audioFormat_.rate;
    const auto channels = audioFormat_.channels;
    if (samples.size() % channels) return;
    // A packet straddling Start/Resume may contain samples from the pause.
    if (timestamp < activeSince_) {
        size_t trim = size_t(((activeSince_ - timestamp) * rate + 9999999) / 10000000) * channels;
        if (trim >= samples.size())
            return;
        samples.erase(samples.begin(), samples.begin() + trim);
        timestamp += int64_t(trim / channels) * 10000000 / rate;
    }
    queuedSamples_ += samples.size();
    if (queuedSamples_ > size_t(rate) * channels * 5) {
        state_.error = "The encoder cannot keep up with full-resolution recording.";
        stop_ = true;
        timeline_.pause(clock100ns());
        return;
    }
    audio_.push_back({std::move(samples), timeline_.time(timestamp)});
    condition_.notify_all();
}
void Recorder::run(Source source, Frames *camera, bool withAudio, std::filesystem::path path) {
    Apartment apartment;
    Movie movie;
    DesktopCapture capture;
    bool wroteVideo = false;
    std::shared_ptr<const Frame> pendingVideo;
    int64_t pendingTime = 0;
    try {
        // The compositor runs on this worker, independently of preview visibility.
        std::unique_ptr<Drawing> drawing;
        if (source.kind == SourceKind::Window)
            drawing = std::make_unique<Drawing>();
        Frames *frames = camera;
        const bool directStage = source.kind == SourceKind::Stage && camera;
        if (!cameraSource(source.kind) && !directStage) {
            capture.start(source);
            frames = capture.frames.get();
        }
        if (!frames)
            throw std::runtime_error("No camera source is available.");
        std::shared_ptr<const Frame> first;
        auto deadline = clock100ns() + 50000000;
        while (!(first = frames->get(cameraSource(source.kind) || directStage))) {
            {
                std::lock_guard lock(mutex_);
                if (stop_)
                    throw std::runtime_error("Recording cancelled before the first frame.");
            }
            if (clock100ns() > deadline)
                throw std::runtime_error("No full-resolution frame arrived from the selected source.");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const int turns = source.kind == SourceKind::Portrait
                            ? portraitTurns(first->width, first->height, source.portraitReverse) : 0;
        auto region = cropPixels(turns % 2 ? first->height : first->width,
                                 turns % 2 ? first->width : first->height,
                                 cameraSource(source.kind) ? source.crop : Crop{});
        auto width = evenExtent(region.width), height = evenExtent(region.height);
        movie.start(path, width, height, withAudio, audioFormat_);
        {
            std::lock_guard lock(mutex_);
            state_.width = width;
            state_.height = height;
            state_.codec = movie.codec;
            activeSince_ = clock100ns();
            timeline_.start(activeSince_);
            state_.phase = Phase::Recording;
        }
        int64_t nextFrame = 0, audioEnd = 0;
        while (true) {
            std::deque<AudioPacket> packets;
            bool stopping, paused;
            CameraOverlay overlay;
            int64_t elapsed;
            Timeline videoTimeline;
            int64_t activeSince;
            {
                std::unique_lock lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(5));
                stopping = stop_;
                paused = timeline_.paused();
                elapsed = timeline_.time(clock100ns());
                videoTimeline = timeline_;
                activeSince = activeSince_;
                overlay = cameraOverlay_;
                packets.swap(audio_);
                queuedSamples_ = 0;
            }
            for (auto &packet : packets) {
                auto time = std::max(packet.time, audioEnd);
                movie.audio(packet.samples, time);
                audioEnd = time + int64_t(packet.samples.size() / audioFormat_.channels) * 10000000 / audioFormat_.rate;
            }
            if (stopping)
                break;
            if (windowSource(source.kind)) {
                if (!IsWindow(source.window))
                    throw std::runtime_error(
                        "The selected window was closed. Received frames have been saved.");
                if (hiddenWindow(source.window)) {
                    pause("Window hidden or minimized. Restore it and press Resume.");
                    continue;
                }
            }
            if (paused)
                continue;
            const auto frameNumber = elapsed * 30 / 10000000;
            if (frameNumber < nextFrame)
                continue;
            auto frame = frames->get(cameraSource(source.kind) || directStage);
            if (!frame)
                continue;
            nextFrame = frameNumber + 1;
            if ((!windowSource(source.kind) || directStage) &&
                (frame->width != first->width || frame->height != first->height))
                throw std::runtime_error(
                    "The source resolution changed. Start a new recording to use the new native resolution.");
            Frame transformed;
            const Frame *pixels = frame.get();
            if (turns) {
                transformed = rotateFrame(*pixels, turns);
                pixels = &transformed;
            }
            if (source.kind == SourceKind::Window && overlay.enabled) {
                auto cameraFrame = camera ? camera->get(true) : nullptr;
                if (!cameraFrame)
                    throw std::runtime_error("No live webcam frame is available for window + camera recording.");
                transformed = composeWindow(*frame, *cameraFrame, overlay);
                pixels = &transformed;
            }
            if (cameraSource(source.kind) && !source.crop.full()) {
                transformed = cropFrame(*pixels, source.crop);
                pixels = &transformed;
            }
            if (pixels->width != width || pixels->height != height || source.mirror) {
                transformed = fitFrame(*pixels, width, height, source.mirror);
                pixels = &transformed;
            }
            // Camera and composed-camera images must share the microphone's
            // capture clock. Assigning the encoder's current time here shifts
            // speech by the entire camera/decode/render delay.
            bool timedCamera = cameraSource(source.kind) || directStage ||
                               (source.kind == SourceKind::Window && overlay.enabled);
            int64_t videoTime = frameNumber * 10000000 / 30;
            if (timedCamera && pixels->capturedAt) {
                // Frames still in flight at Start/Resume belong to the previous
                // segment. Never move them forward into the new segment.
                if (pixels->capturedAt < activeSince)
                    continue;
                videoTime = videoTimeline.time(pixels->capturedAt);
            }
            if (pendingVideo && videoTime <= pendingTime)
                continue;
            if (pendingVideo) {
                movie.video(*pendingVideo, pendingTime, videoTime - pendingTime);
                wroteVideo = true;
            }
            // Wait for the next capture timestamp before resampling this
            // interval. Using delivery time would bake camera latency into lip sync.
            if (pixels == frame.get()) pendingVideo = frame;
            else pendingVideo = std::make_shared<Frame>(std::move(transformed));
            pendingTime = videoTime;
        }
    } catch (const std::exception &error) {
        std::lock_guard lock(mutex_);
        state_.error = error.what();
    } catch (...) {
        std::lock_guard lock(mutex_);
        state_.error = "Windows could not capture the selected source.";
    }
    {
        std::lock_guard lock(mutex_);
        // Freeze before capture shutdown/finalization, which can take time.
        timeline_.pause(clock100ns());
        state_.elapsed = timeline_.time(clock100ns());
        state_.phase = Phase::Saving;
    }
    capture.stop();
    try {
        if (pendingVideo) {
            movie.video(*pendingVideo, pendingTime, std::max<int64_t>(1, state().elapsed - pendingTime));
            wroteVideo = true;
        }
    } catch (const std::exception &error) {
        std::lock_guard lock(mutex_);
        state_.error += std::string(" ") + error.what();
    }
    // Finalize even after device loss or a failed last frame, so useful frames
    // remain playable.
    try {
        movie.finish();
        if (wroteVideo) {
            std::lock_guard lock(mutex_);
            state_.saved = path;
            if (withAudio) state_.audioMaster = wavePath(path);
        }
    } catch (const std::exception &error) {
        std::lock_guard lock(mutex_);
        state_.error += std::string(" ") + error.what();
    }
    if (!wroteVideo) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        try { movie.discardEmptyMaster(); } catch (...) {}
    }
    {
        std::lock_guard lock(mutex_);
        state_.phase = Phase::Idle;
        audio_.clear();
        queuedSamples_ = 0;
    }
}
} // namespace youcam
