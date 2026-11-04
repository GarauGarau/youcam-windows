#include "recorder.h"
#include <fstream>
#include <iostream>
#include <cstring>
using namespace youcam;
namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class T> T field(const std::vector<char> &bytes, size_t offset) {
    T v;
    require(offset + sizeof(v) <= bytes.size(), "WAV is truncated.");
    std::memcpy(&v, bytes.data() + offset, sizeof(v));
    return v;
}
std::vector<char> read(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void masterTest(const std::filesystem::path &path) {
    const AudioFormat format{48000, 4, 0x33};
    WaveMaster master;
    master.open(path, format);
    // Separate channels, sub-16-bit detail, and values above digital full scale.
    const std::vector<float> first{1.25f, -1.125f, 1.0f / 8388608, .1234567f,
                                   -.25f, .5f, -.3333333f, .7654321f};
    master.append(first, 1000000); // 100 ms real microphone startup delay.
    master.append({.75f, .625f, .125f, -.125f}, 1000625); // Frame 4803; one-frame gap.
    master.finish(2000000);
    auto bytes = read(path);
    require(bytes.size() == 116 + 9600 * 4 * 4, "WAV duration does not match video.");
    require(field<uint16_t>(bytes, 58) == 4 && field<uint32_t>(bytes, 60) == 48000,
            "Native rate/channels were lost.");
    require(field<uint16_t>(bytes, 70) == 32, "WAV did not retain float32 precision.");
    for (size_t i = 0; i < 4800 * 4; ++i)
        require(field<float>(bytes, 116 + i * 4) == 0, "Startup silence missing.");
    for (size_t i = 0; i < first.size(); ++i)
        require(field<float>(bytes, 116 + (4800 * 4 + i) * 4) == first[i],
                "Float precision, gain headroom or channel isolation was lost.");
    require(field<float>(bytes, 116 + 4802 * 16) == 0, "Packet gap was not preserved.");
    require(field<float>(bytes, 116 + 4803 * 16) == .75f, "Gap moved audio on the timeline.");
    require(field<float>(bytes, bytes.size() - 4) == 0, "End padding missing.");
    bool refused = false;
    try { WaveMaster duplicate; duplicate.open(path, format); } catch (...) { refused = true; }
    require(refused && read(path) == bytes, "Existing master was overwritten.");
    std::filesystem::remove(path);
    // Overlap trim, fractional 100ns timestamp rounding, and stop truncation.
    master.open(path, {44100, 2, 3});
    master.append({.1f, .2f, .3f, .4f, .5f, .6f}, 0);
    master.append({.5f, .6f, .7f, .8f}, 2 * 10000000 / 44100);
    master.finish(3 * 10000000 / 44100);
    bytes = read(path);
    require(bytes.size() == 116 + 3 * 8, "Stop failed to trim excess audio.");
    require(field<float>(bytes, 116 + 2 * 8) == .5f, "Overlap changed existing audio.");
}
void movieTest(const std::filesystem::path &path, AudioFormat format) {
    Movie movie;
    movie.start(path, 64, 48, true, format);
    Frame frame(64, 48);
    for (int i = 0; i < 3; ++i) {
        movie.video(frame, int64_t(i) * 10000000 / 30, 10000000 / 30);
        std::vector<float> sound(size_t(format.rate / 30) * format.channels);
        for (size_t j = 0; j < sound.size(); ++j)
            sound[j] = float(.1 * std::sin(double(j / format.channels) * .13));
        movie.audio(sound, int64_t(i) * 10000000 / 30);
    }
    movie.finish();
    auto bytes = read(wavePath(path));
    require(bytes.size() == 116 + size_t(format.rate / 10) * format.channels * 4,
            "Movie did not finalize a synchronized lossless master.");
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Read MP4 audio");
    ComPtr<IMFMediaType> type;
    check(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &type), "Inspect AAC");
    UINT32 channels = 0, bitrate = 0;
    type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bitrate);
    require(channels == std::min<uint32_t>(2, format.channels), "MP4 compatibility channel count incorrect.");
    require(bitrate >= 20000, "MP4 AAC bitrate was not raised.");
}
}
int main() {
    auto directory = std::filesystem::temp_directory_path() /
                      (L"YouCam-audio-" + std::to_wstring(GetCurrentProcessId()));
    try {
        Apartment apartment;
        check(MFStartup(MF_VERSION), "Start multimedia");
        std::filesystem::create_directories(directory);
        masterTest(directory / L"master.wav");
        int index = 0;
        for (auto format : {AudioFormat{48000, 1, 4}, AudioFormat{48000, 2, 3}, AudioFormat{48000, 4, 0x33},
                             AudioFormat{44100, 2, 3}, AudioFormat{96000, 2, 3}})
            movieTest(directory / (std::to_wstring(index++) + L".mp4"), format);
        MFShutdown();
        std::filesystem::remove_all(directory);
        std::cout << "PASS: lossless precision, headroom, channels, timing, no overwrite, and MP4 compatibility.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
}
