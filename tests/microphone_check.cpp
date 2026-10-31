#include "audio.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <iostream>

using namespace youcam;
int main(int argc, char **argv) {
    try {
        Apartment apartment;
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(&enumerator)), "Enumerate audio");
        for (const auto &info : Microphone::devices()) {
            if (info.name.find(L"DJI") == std::wstring::npos) continue;
            std::cout << narrow(info.name) << '\n';
            ComPtr<IMMDevice> device;
            check(enumerator->GetDevice(info.id.c_str(), &device), "Open microphone");
            ComPtr<IAudioClient> client;
            check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Activate audio");
            WAVEFORMATEX *mix = nullptr;
            check(client->GetMixFormat(&mix), "Get native mix format");
            std::cout << "Windows mix: " << mix->nSamplesPerSec << " Hz, " << mix->nChannels
                      << " channels, " << mix->wBitsPerSample << " bits, tag " << mix->wFormatTag;
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                auto ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix);
                std::cout << ", valid bits " << ext->Samples.wValidBitsPerSample
                          << ", float " << (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            }
            std::cout << '\n';
            CoTaskMemFree(mix);
            for (WORD channels : {WORD(1), WORD(2), WORD(4)}) for (WORD bits : {WORD(16), WORD(24), WORD(32)}) {
                WAVEFORMATEXTENSIBLE format{};
                format.Format = {WAVE_FORMAT_EXTENSIBLE, channels, 48000,
                                 DWORD(48000 * channels * bits / 8), WORD(channels * bits / 8), bits, 22};
                format.Samples.wValidBitsPerSample = bits;
                format.dwChannelMask = channels == 1 ? 4 : channels == 2 ? 3 : 0x33;
                format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                auto hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr);
                std::cout << "Hardware 48000 / " << channels << " ch / " << bits << " bit: "
                          << (hr == S_OK ? "supported" : "unsupported") << '\n';
            }
            // Explicit opt-in: record three seconds locally; never part of CTest.
            if (argc == 3 && std::string(argv[1]) == "--capture") {
                youcam::Microphone mic;
                std::mutex mutex;
                std::vector<float> captured;
                mic.samples = [&](auto packet, int64_t) {
                    std::lock_guard lock(mutex);
                    captured.insert(captured.end(), packet.begin(), packet.end());
                };
                mic.start(info.id);
                auto format = mic.format();
                Sleep(3000);
                mic.shutdown();
                if (!mic.error().empty()) throw std::runtime_error(mic.error());
                if (captured.empty()) throw std::runtime_error("No audio samples arrived.");
                std::cout << "Capture: " << format.rate << " Hz, " << format.channels
                          << " channels, float32, raw=" << mic.raw() << '\n';
                for (unsigned c = 0; c < format.channels; ++c) {
                    double square = 0, peak = 0;
                    size_t detail = 0;
                    for (size_t i = c; i < captured.size(); i += format.channels) {
                        square += double(captured[i]) * captured[i];
                        peak = std::max(peak, std::abs(double(captured[i])));
                        detail += std::abs(captured[i] * 32768 - std::round(captured[i] * 32768)) > .001;
                    }
                    auto frames = captured.size() / format.channels;
                    std::cout << "Channel " << c + 1 << ": RMS=" << std::sqrt(square / frames)
                              << ", peak=" << peak << ", samples beyond 16-bit grid=" << detail << '\n';
                }
                WaveMaster master;
                master.open(std::filesystem::path(argv[2]), format);
                master.append(captured, 0);
                master.finish();
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
