#include "recorder.h"
#include <codecapi.h>
#include <cstring>
#include <iomanip>

namespace youcam {
namespace {
ComPtr<IMFMediaType> videoType(REFGUID subtype, uint32_t width, uint32_t height) {
    ComPtr<IMFMediaType> type; check(MFCreateMediaType(&type), "Create video format");
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
}
void Movie::open(const std::filesystem::path& path, uint32_t width, uint32_t height, bool audio, bool hardware, bool hevc) {
    ComPtr<IMFAttributes> attributes; check(MFCreateAttributes(&attributes, 3), "Create encoder attributes");
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware);
    attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer_), "Create MP4 recording");
    auto output = videoType(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, width, height);
    UINT32 bitrate = UINT32(std::clamp<uint64_t>(uint64_t(width) * height * 30 / 4, 2000000, 400000000));
    output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (!hevc) output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    check(writer_->AddStream(output.Get(), &video_), "Configure native-resolution video encoder");
    auto input = videoType(MFVideoFormat_NV12, width, height);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    check(writer_->SetInputMediaType(video_, input.Get(), nullptr), "Connect full-resolution video input");
    if (audio) {
        ComPtr<IMFMediaType> compressed; check(MFCreateMediaType(&compressed), "Create AAC type");
        compressed->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        compressed->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        compressed->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        compressed->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        compressed->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        compressed->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
        compressed->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        compressed->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        compressed->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        check(writer_->AddStream(compressed.Get(), &audio_), "Configure AAC encoder");
        ComPtr<IMFMediaType> pcm; check(MFCreateMediaType(&pcm), "Create audio format");
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1); pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 96000);
        check(writer_->SetInputMediaType(audio_, pcm.Get(), nullptr), "Connect microphone encoder");
    }
    check(writer_->BeginWriting(), "Start native video encoder");
    codec = hevc ? L"HEVC" : L"H.264";
    width_ = width; height_ = height; audioEnabled_ = audio;
}
void Movie::start(const std::filesystem::path& path, uint32_t width, uint32_t height, bool audio) {
    std::string error;
    // Preserve dimensions for every attempt. Allow Windows to select a GPU MFT,
    // then try its software H.264 encoder; HEVC can support larger GPU modes.
    for (auto choice : {std::pair{true, false}, std::pair{false, false}, std::pair{true, true}}) {
        try { open(path, width, height, audio, choice.first, choice.second); return; }
        catch (const std::exception& failed) { error = failed.what(); writer_.Reset(); std::error_code ignored; std::filesystem::remove(path, ignored); }
    }
    throw std::runtime_error("No installed Windows encoder accepted " + narrow(dimensions(width, height)) + ". Resolution was not reduced. " + error);
}
void Movie::sample(DWORD stream, const void* data, size_t bytes, int64_t time, int64_t duration) {
    if (bytes > MAXDWORD) throw std::runtime_error("Frame exceeds Media Foundation's buffer size.");
    ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(DWORD(bytes), &buffer), "Allocate encoder buffer");
    BYTE* destination = nullptr; check(buffer->Lock(&destination, nullptr, nullptr), "Lock encoder buffer");
    std::memcpy(destination, data, bytes); buffer->Unlock(); buffer->SetCurrentLength(DWORD(bytes));
    ComPtr<IMFSample> sample; check(MFCreateSample(&sample), "Create encoder sample");
    sample->AddBuffer(buffer.Get()); sample->SetSampleTime(time); sample->SetSampleDuration(duration);
    check(writer_->WriteSample(stream, sample.Get()), "Encode recording sample");
}
void Movie::video(const Frame& frame, int64_t timestamp, int64_t duration) {
    if (frame.width != width_ || frame.height != height_) throw std::runtime_error("Video resolution changed unexpectedly.");
    auto nv12 = toNV12(frame); sample(video_, nv12.data(), nv12.size(), timestamp, duration);
}
void Movie::audio(const std::vector<int16_t>& samples, int64_t timestamp) {
    if (audioEnabled_ && !samples.empty()) sample(audio_, samples.data(), samples.size() * 2, timestamp, int64_t(samples.size()) * 10000000 / 48000);
}
void Movie::finish() {
    if (!writer_) return;
    HRESULT hr = writer_->Finalize(); writer_.Reset(); check(hr, "Finalize MP4 recording");
}
std::filesystem::path nextMoviePath() {
    PWSTR desktop = nullptr; check(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_CREATE, nullptr, &desktop), "Find Desktop folder");
    std::filesystem::path directory(desktop); CoTaskMemFree(desktop);
    SYSTEMTIME time; GetLocalTime(&time); wchar_t name[128];
    swprintf_s(name, L"YouCam-%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    auto path = directory / (std::wstring(name) + L".mp4");
    for (int suffix = 2; std::filesystem::exists(path); ++suffix) path = directory / (std::wstring(name) + L"-" + std::to_wstring(suffix) + L".mp4");
    return path;
}
RecordingState Recorder::state() const {
    std::lock_guard lock(mutex_); auto result = state_;
    if (state_.phase == Phase::Recording || state_.phase == Phase::Paused) result.elapsed = timeline_.time(clock100ns());
    return result;
}
void Recorder::start(Source source, Frames* camera, bool audio, const std::filesystem::path& path) {
    if (state().phase != Phase::Idle) return;
    if (thread_.joinable()) thread_.join();
    { std::lock_guard lock(mutex_); state_ = {}; state_.serial = ++serial_; state_.phase = Phase::Starting; state_.source = source.name; timeline_.start(clock100ns()); stop_ = false; audio_.clear(); queuedSamples_ = 0; }
    thread_ = std::thread([this, source, camera, audio, path] { run(source, camera, audio, path); });
}
void Recorder::pause(const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (state_.phase != Phase::Recording) return;
    timeline_.pause(clock100ns()); state_.phase = Phase::Paused; state_.notice = reason; condition_.notify_all();
}
void Recorder::resume() {
    std::lock_guard lock(mutex_);
    if (state_.phase != Phase::Paused) return;
    activeSince_ = clock100ns(); timeline_.resume(activeSince_); state_.phase = Phase::Recording; state_.notice.clear(); condition_.notify_all();
}
void Recorder::stop(const std::string& error) {
    { std::lock_guard lock(mutex_); stop_ = true; if (!error.empty()) state_.error = error; condition_.notify_all(); }
    if (thread_.joinable()) thread_.join();
}
void Recorder::audio(std::vector<int16_t> samples, int64_t timestamp) {
    std::lock_guard lock(mutex_);
    if (state_.phase != Phase::Recording || stop_) return;
    // A packet straddling Start/Resume may contain samples from the pause.
    if (timestamp < activeSince_) {
        size_t trim = size_t((activeSince_ - timestamp) * 48000 / 10000000);
        if (trim >= samples.size()) return;
        samples.erase(samples.begin(), samples.begin() + trim); timestamp += int64_t(trim) * 10000000 / 48000;
    }
    queuedSamples_ += samples.size();
    if (queuedSamples_ > 48000 * 5) { state_.error = "The encoder cannot keep up with full-resolution recording."; stop_ = true; return; }
    audio_.push_back({std::move(samples), timeline_.time(timestamp)}); condition_.notify_all();
}
void Recorder::run(Source source, Frames* camera, bool withAudio, std::filesystem::path path) {
    Apartment apartment;
    Movie movie; DesktopCapture capture; bool wroteVideo = false;
    try {
        Frames* frames = camera;
        if (source.kind != SourceKind::Camera) { capture.start(source); frames = capture.frames.get(); }
        if (!frames) throw std::runtime_error("No camera source is available.");
        std::shared_ptr<const Frame> first;
        auto deadline = clock100ns() + 50000000;
        while (!(first = frames->get(source.kind == SourceKind::Camera))) {
            { std::lock_guard lock(mutex_); if (stop_) throw std::runtime_error("Recording cancelled before the first frame."); }
            if (clock100ns() > deadline) throw std::runtime_error("No full-resolution frame arrived from the selected source.");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        auto width = evenExtent(first->width), height = evenExtent(first->height);
        movie.start(path, width, height, withAudio);
        {
            std::lock_guard lock(mutex_); state_.width = width; state_.height = height; state_.codec = movie.codec;
            activeSince_ = clock100ns(); timeline_.start(activeSince_); state_.phase = Phase::Recording;
        }
        int64_t nextFrame = 0, audioEnd = 0;
        while (true) {
            std::deque<AudioPacket> packets; bool stopping, paused; int64_t elapsed;
            {
                std::unique_lock lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(5));
                stopping = stop_; paused = timeline_.paused(); elapsed = timeline_.time(clock100ns());
                packets.swap(audio_); queuedSamples_ = 0;
            }
            for (auto& packet : packets) {
                auto time = std::max(packet.time, audioEnd); movie.audio(packet.samples, time);
                audioEnd = time + int64_t(packet.samples.size()) * 10000000 / 48000;
            }
            if (stopping) break;
            if (source.kind == SourceKind::Window) {
                if (!IsWindow(source.window)) throw std::runtime_error("The selected window was closed. Received frames have been saved.");
                if (hiddenWindow(source.window)) { pause("Window hidden or minimized. Restore it and press Resume."); continue; }
            }
            if (paused) continue;
            const auto frameNumber = elapsed * 30 / 10000000;
            if (frameNumber < nextFrame) continue;
            auto frame = frames->get(source.kind == SourceKind::Camera);
            if (!frame) continue;
            if (source.kind != SourceKind::Window && (frame->width != first->width || frame->height != first->height))
                throw std::runtime_error("The source resolution changed. Start a new recording to use the new native resolution.");
            Frame fitted;
            const Frame* pixels = frame.get();
            if (frame->width != width || frame->height != height || source.mirror) {
                fitted = fitFrame(*frame, width, height, source.mirror); pixels = &fitted;
            }
            movie.video(*pixels, frameNumber * 10000000 / 30, 10000000 / 30);
            wroteVideo = true; nextFrame = frameNumber + 1;
        }
    } catch (const std::exception& error) { std::lock_guard lock(mutex_); state_.error = error.what(); }
    catch (...) { std::lock_guard lock(mutex_); state_.error = "Windows could not capture the selected source."; }
    capture.stop();
    { std::lock_guard lock(mutex_); state_.elapsed = timeline_.time(clock100ns()); state_.phase = Phase::Saving; }
    // Finalize even after device loss, so useful frames remain playable.
    try { movie.finish(); if (wroteVideo) { std::lock_guard lock(mutex_); state_.saved = path; } }
    catch (const std::exception& error) { std::lock_guard lock(mutex_); state_.error += std::string(" ") + error.what(); }
    if (!wroteVideo) { std::error_code ignored; std::filesystem::remove(path, ignored); }
    { std::lock_guard lock(mutex_); state_.phase = Phase::Idle; audio_.clear(); queuedSamples_ = 0; }
}
} // namespace youcam
