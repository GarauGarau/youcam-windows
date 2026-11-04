#include "recorder.h"
#include <iostream>
using namespace youcam;
namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<int64_t> markers(const std::filesystem::path &path, bool audio, int64_t expectedFrames = 0) {
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Read sync fixture");
    DWORD stream = DWORD(audio ? MF_SOURCE_READER_FIRST_AUDIO_STREAM : MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    check(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Deselect streams");
    check(reader->SetStreamSelection(stream, TRUE), "Select sync stream");
    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "Create decoded sync format");
    type->SetGUID(MF_MT_MAJOR_TYPE, audio ? MFMediaType_Audio : MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, audio ? MFAudioFormat_PCM : MFVideoFormat_NV12);
    if (audio) {
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 96000);
    }
    check(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Decode sync stream");
    std::vector<int64_t> result;
    int64_t previous = -1;
    int64_t videoFrames = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        check(reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample), "Read sync sample");
        if (sample) {
            require(time > previous, "Encoded stream timestamps must increase.");
            if (audio && previous < 0)
                require(time == 0, "Audio must start at zero, including delayed microphone startup.");
            if (!audio) {
                // Inspect every decoded sample, not just the advertised frame rate.
                // The MP4 timescale can round a 100 ns timestamp by one tick.
                require(std::abs(time - videoFrames * 10000000 / 30) < 400,
                        "Saved video is not continuous 30 fps CFR starting at zero.");
                LONGLONG duration = 0;
                check(sample->GetSampleDuration(&duration), "Read encoded frame duration");
                require(std::abs(duration - 10000000 / 30) < 400,
                        "A saved frame has variable duration.");
                ++videoFrames;
            }
            previous = time;
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer), "Get sync pixels/PCM");
            BYTE *bytes = nullptr;
            DWORD length = 0;
            check(buffer->Lock(&bytes, nullptr, &length), "Lock sync sample");
            int64_t hit = -1;
            if (audio) {
                auto pcm = reinterpret_cast<const int16_t *>(bytes);
                for (DWORD i = 0; i < length / 2; ++i) {
                    if (std::abs(int(pcm[i])) > 3000) {
                        hit = time + int64_t(i) * 10000000 / 48000;
                        break;
                    }
                }
            } else if (length && bytes[0] > 180) hit = time;
            buffer->Unlock();
            // Each marker lasts 100 ms. Coalesce AAC packets / video frames.
            if (hit >= 0 && (result.empty() || hit - result.back() > 5000000)) result.push_back(hit);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!audio && expectedFrames)
        require(videoFrames == expectedFrames, "CFR conversion changed the recording duration/frame count.");
    return result;
}
void longRecording(const std::filesystem::path &path, bool stress = false) {
    const auto started = std::chrono::steady_clock::now();
    const uint32_t width = stress ? 1920 : 64, height = stress ? 1200 : 48;
    const int seconds = stress ? 600 : 60;
    Movie movie;
    movie.start(path, width, height, true);
    // Encode a synthetic capture timeline as fast as the machine can sustain.
    // Irregular intervals, delayed first image and 125 ms microphone startup.
    const int64_t end = int64_t(seconds) * 10000000;
    const int64_t cues[] = {5000000, end / 2 + 5000000, end - 5000000};
    constexpr int64_t steps[] = {670000, 1010000, 720000, 950000, 830000};
    int64_t audioSample = 6000;
    int cadence = 0;
    int nextProgress = 60;
    for (int64_t time = 700000; time < end;) {
        auto next = std::min(end, time + steps[cadence++ % 5]);
        bool lit = false;
        for (auto cue : cues) {
            lit |= time >= cue && time < cue + 1000000;
            for (auto transition : {cue, cue + 1000000})
                if (transition > time) next = std::min(next, transition);
        }
        Frame frame(width, height);
        if (lit) std::fill(frame.bgra.begin(), frame.bgra.end(), uint8_t(255));
        else if (stress) {
            // Moving textured content exercises full-resolution conversion
            // and encoding; luma stays below the synchronization marker threshold.
            for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                const auto p = (size_t(y) * width + x) * 4;
                const uint8_t texture = uint8_t((x * 31u + y * 17u) % 48u);
                const bool box = (x + uint32_t(cadence) * 9u) % width < 320 && y > 200 && y < 700;
                frame.bgra[p] = texture + (box ? 60 : 0);
                frame.bgra[p + 1] = texture + (box ? 30 : 0);
                frame.bgra[p + 2] = texture;
                frame.bgra[p + 3] = 255;
            }
        }
        movie.video(frame, time, next - time);
        const auto audioEnd = next * 48000 / 10000000;
        while (audioSample < audioEnd) {
            std::vector<float> sound(size_t(std::min<int64_t>(4800, audioEnd - audioSample)), 0);
            for (size_t i = 0; i < sound.size(); ++i) {
                const auto sampleTime = (audioSample + int64_t(i)) * 10000000 / 48000;
                for (auto cue : cues)
                    if (sampleTime >= cue && sampleTime < cue + 1000000)
                        sound[i] = float((12000.0 / 32768) * std::sin((audioSample + int64_t(i)) *
                                                          6.283185307179586 * 1000 / 48000));
            }
            movie.audio(sound, audioSample * 10000000 / 48000);
            audioSample += int64_t(sound.size());
        }
        time = next;
        if (stress && time / 10000000 >= nextProgress) {
            const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cout << "Encoded " << nextProgress << '/' << seconds << " seconds; " << wall
                      << " seconds wall time" << std::endl;
            nextProgress += 60;
        }
    }
    movie.finish();
    const auto encodeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "Finalized in " << encodeSeconds << " s (" << seconds / encodeSeconds
              << "x real time); decoding all frames and audio." << std::endl;
    auto video = markers(path, false, int64_t(seconds) * 30), audio = markers(path, true);
    auto master = markers(wavePath(path), true);
    require(master.size() == 3, "Lossless WAV lost a synchronization marker.");
    require(video.size() == 3 && audio.size() == 3, "Long recording lost a synchronization marker.");
    for (size_t i = 0; i < video.size(); ++i) {
        require(std::abs(video[i] - cues[i]) < 400, "Video cue moved on the constant-rate timeline.");
        require(std::abs(video[i] - audio[i]) < 600000, "Audio/video synchronization drifted over time.");
        require(std::abs(master[i] - cues[i]) < 1000, "Lossless WAV moved a cue on the timeline.");
        std::cout << "Cue " << i + 1 << " at " << cues[i] / 10000000.0 << " s: signed A/V skew "
                  << (video[i] - audio[i]) / 10000.0 << " ms\n";
    }
    const auto drift = (video.back() - audio.back()) - (video.front() - audio.front());
    require(std::abs(drift) < 200000, "A/V offset drifted by more than 20 ms during the recording.");
    std::cout << "PASS: " << seconds << " seconds at " << width << 'x' << height << ", exactly "
              << seconds * 30 << " frames, delayed microphone, three aligned cues; drift "
              << drift / 10000.0 << " ms.\n";
}
void segment(Recorder &recorder, Frames &frames, int64_t base, bool irregular) {
    constexpr int64_t delay = 1800000; // Simulated camera delivery delay: 180 ms.
    int audioPacket = 0, videoFrame = 0;
    int cadence = 0;
    constexpr int steps[] = {2, 3, 2, 3, 4, 1}; // About 12 fps, like the affected recordings.
    auto deadline = base + 12000000;
    while (clock100ns() < deadline) {
        auto now = clock100ns();
        while (base + int64_t(audioPacket + 1) * 100000 <= now) {
            auto time = base + int64_t(audioPacket) * 100000;
            std::vector<float> sound(480, 0);
            if (audioPacket >= 50 && audioPacket < 60)
                for (size_t i = 0; i < sound.size(); ++i)
                    sound[i] = float((12000.0 / 32768) * std::sin((i + audioPacket * 480) * 6.283185307179586 * 1000 / 48000));
            recorder.audio(std::move(sound), time);
            ++audioPacket;
        }
        while (base + int64_t(videoFrame) * 10000000 / 30 + delay <= now) {
            Frame frame(64, 48);
            frame.capturedAt = base + int64_t(videoFrame) * 10000000 / 30;
            if (videoFrame >= 15 && videoFrame < 18) std::fill(frame.bgra.begin(), frame.bgra.end(), uint8_t(255));
            frames.put(std::move(frame));
            videoFrame += irregular ? steps[cadence++ % 6] : 1;
        }
        Sleep(2);
    }
}
}
int main(int argc, char **argv) {
    std::filesystem::path path;
    const bool stress = argc == 2 && std::string(argv[1]) == "--stress";
    const bool keep = stress || (argc == 2 && std::string(argv[1]) == "--keep");
    try {
        Apartment apartment;
        check(MFStartup(MF_VERSION), "Start multimedia");
        const auto prefix = std::filesystem::temp_directory_path() /
                            (L"YouCam-sync-" + std::to_wstring(GetCurrentProcessId()));
        path = prefix.wstring() + L"-long.mp4";
        longRecording(path, stress);
        if (keep) std::cout << "Fixture: " << path << '\n';
        else { std::filesystem::remove(path); std::filesystem::remove(wavePath(path)); }
        if (stress) {
            MFShutdown();
            return 0;
        }
        for (bool irregular : {false, true}) for (auto kind : {SourceKind::Camera, SourceKind::Portrait, SourceKind::Stage}) {
        path = prefix.wstring() + (kind == SourceKind::Camera ? L"-camera" : kind == SourceKind::Portrait ? L"-portrait" : L"-stage") +
               (irregular ? L"-irregular.mp4" : L"-regular.mp4");
        Frames frames;
        Frame initial(64, 48);
        initial.capturedAt = clock100ns() - 1800000;
        frames.put(std::move(initial));
        Recorder recorder;
        recorder.start({kind, nullptr, kind == SourceKind::Stage ? GetDesktopWindow() : nullptr,
                        {}, L"Delayed synthetic camera/scene"}, &frames, true, path);
        for (int i = 0; i < 500 && recorder.state().phase == Phase::Starting; ++i) Sleep(10);
        require(recorder.state().phase == Phase::Recording, recorder.state().error.c_str());
        segment(recorder, frames, clock100ns(), irregular);
        recorder.pause();
        auto paused = recorder.state().elapsed;
        Sleep(120);
        require(recorder.state().elapsed == paused, "Pause must freeze the recording clock.");
        // A white frame captured during the pause arrives after Resume. It must
        // be rejected, otherwise decoding finds an extra unsynchronized marker.
        Frame inFlight(64, 48);
        std::fill(inFlight.bgra.begin(), inFlight.bgra.end(), uint8_t(255));
        inFlight.capturedAt = clock100ns() - 500000;
        recorder.resume();
        frames.put(std::move(inFlight));
        segment(recorder, frames, clock100ns(), irregular);
        recorder.stop();
        require(recorder.state().error.empty(), recorder.state().error.c_str());
        auto video = markers(path, false), audio = markers(path, true);
        auto master = markers(wavePath(path), true);
        require(master.size() == 2, "Lossless WAV lost a pause/resume marker.");
        require(video.size() == 2 && audio.size() == 2, "Expected two matching A/V markers, including after Resume.");
        for (size_t i = 0; i < video.size(); ++i) {
            auto skew = std::abs(video[i] - audio[i]);
            std::cout << "Marker " << i + 1 << ": A/V skew " << skew / 10000.0 << " ms\n";
            require(skew < 600000, "Camera delivery delay leaked into recorded lip sync.");
            require(std::abs(master[i] - video[i]) < 600000, "Lossless WAV lost pause/resume synchronization.");
        }
        require(video[1] - video[0] < 14000000, "Paused time leaked into the saved video.");
        if (keep) std::cout << "Fixture: " << path << '\n';
        else { std::filesystem::remove(path); std::filesystem::remove(wavePath(path)); }
        }
        MFShutdown();
        std::cout << "PASS: decoded 30 fps CFR and lip sync with delayed/irregular video and pause/resume.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); }
        return 1;
    }
}
