#include "audio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <ksmedia.h>
#include <cstring>

namespace youcam {
std::vector<Device> Microphone::devices() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
          "Create microphone enumerator");
    ComPtr<IMMDeviceCollection> collection;
    check(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection), "List microphones");
    UINT count = 0;
    collection->GetCount(&count);
    std::vector<Device> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        check(collection->Item(i, &device), "Read microphone");
        LPWSTR id = nullptr;
        check(device->GetId(&id), "Read microphone ID");
        Device info{id, L"Microphone"};
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                info.name = value.pwszVal;
            PropVariantClear(&value);
        }
        result.push_back(std::move(info));
    }
    return result;
}
void Microphone::start(const std::wstring &id) {
    shutdown();
    {
        std::lock_guard lock(mutex_);
        error_.clear();
        started_ = false;
        raw_ = false;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_)
        throw std::runtime_error("Cannot create microphone event.");
    thread_ = std::thread([this, id] { read(id); });
}
void Microphone::read(std::wstring id) {
    HANDLE available = nullptr;
    try {
        Apartment apartment;
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
              "Create audio enumerator");
        ComPtr<IMMDevice> device;
        if (id.empty())
            check(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device),
                  "Open default microphone");
        else
            check(enumerator->GetDevice(id.c_str(), &device), "Open selected microphone");
        ComPtr<IAudioClient> client;
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Activate microphone");
        // RAW bypasses optional Windows speech processing (AEC, AGC, denoising).
        // Unsupported endpoints retain their normal shared mode; never take
        // exclusive ownership or change the user's global device settings.
        ComPtr<IAudioClient2> client2;
        bool raw = false;
        if (SUCCEEDED(client.As(&client2))) {
            AudioClientProperties properties{sizeof(AudioClientProperties), FALSE,
                                              AudioCategory_Media, AUDCLNT_STREAMOPTIONS_RAW};
            raw = SUCCEEDED(client2->SetClientProperties(&properties));
            if (!raw) {
                properties.Options = AUDCLNT_STREAMOPTIONS_NONE;
                check(client2->SetClientProperties(&properties), "Configure microphone stream");
            }
        }
        WAVEFORMATEX *allocated = nullptr;
        check(client->GetMixFormat(&allocated), "Read native microphone format");
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> owner(allocated, CoTaskMemFree);
        const auto &format = *allocated;
        GUID subtype = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                                                 : KSDATAFORMAT_SUBTYPE_PCM;
        uint32_t mask = format.nChannels == 1 ? 4 : format.nChannels == 2 ? 3 : 0;
        if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
            auto ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(allocated);
            subtype = ext->SubFormat;
            mask = ext->dwChannelMask;
        } else if (format.wFormatTag != WAVE_FORMAT_PCM && format.wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
            throw std::runtime_error("Unsupported microphone sample format.");
        const bool floating = subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if ((!floating && subtype != KSDATAFORMAT_SUBTYPE_PCM) ||
            (floating && format.wBitsPerSample != 32) ||
            (!floating && format.wBitsPerSample != 16 && format.wBitsPerSample != 24 && format.wBitsPerSample != 32) ||
            !format.nChannels || !format.nSamplesPerSec ||
            format.nBlockAlign != format.nChannels * format.wBitsPerSample / 8)
            throw std::runtime_error("Unsupported native microphone format.");
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  1000000, 0, allocated, nullptr), "Open native microphone stream");
        available = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!available)
            throw std::runtime_error("Cannot create audio buffer event.");
        check(client->SetEventHandle(available), "Set microphone notification");
        ComPtr<IAudioCaptureClient> capture;
        check(client->GetService(IID_PPV_ARGS(&capture)), "Open microphone buffers");
        check(client->Start(), "Start microphone");
        {
            std::lock_guard lock(mutex_);
            format_ = {format.nSamplesPerSec, format.nChannels, mask};
            raw_ = raw;
            started_ = true;
        }
        ready_.notify_all();
        HANDLE events[] = {stopEvent_, available};
        while (WaitForMultipleObjects(2, events, FALSE, 1000) != WAIT_OBJECT_0) {
            UINT32 packet = 0;
            check(capture->GetNextPacketSize(&packet), "Poll microphone");
            while (packet) {
                BYTE *data = nullptr;
                UINT32 count = 0;
                DWORD status = 0;
                UINT64 position = 0, timestamp = 0;
                check(capture->GetBuffer(&data, &count, &status, &position, &timestamp),
                      "Read microphone samples");
                std::vector<float> output(size_t(count) * format.nChannels, 0);
                float maximum = 0;
                double gain = std::pow(10.0, gainDB.load() / 20.0);
                if (!(status & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    for (size_t i = 0; i < output.size(); ++i) {
                        const BYTE *p = data + i * (format.wBitsPerSample / 8);
                        double value = 0;
                        if (floating) {
                            float v;
                            std::memcpy(&v, p, 4);
                            value = std::isfinite(v) ? v : 0;
                        } else if (format.wBitsPerSample == 16) {
                            int16_t v;
                            std::memcpy(&v, p, 2);
                            value = v / 32768.0;
                        } else if (format.wBitsPerSample == 24) {
                            int32_t v = int32_t(p[0]) | int32_t(p[1]) << 8 | int32_t(p[2]) << 16;
                            if (v & 0x800000) v -= 0x1000000;
                            value = v / 8388608.0;
                        } else {
                            int32_t v;
                            std::memcpy(&v, p, 4);
                            value = v / 2147483648.0;
                        }
                        value *= gain;
                        maximum = std::max(maximum, float(std::abs(value)));
                        output[i] = float(value);
                    }
                }
                capture->ReleaseBuffer(count);
                peak = maximum;
                if ((status & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !timestamp)
                    timestamp = clock100ns() - int64_t(count) * 10000000 / format.nSamplesPerSec;
                if (samples)
                    samples(std::move(output), int64_t(timestamp));
                check(capture->GetNextPacketSize(&packet), "Poll microphone samples");
            }
        }
        client->Stop();
    } catch (const std::exception &error) {
        std::lock_guard lock(mutex_);
        error_ = error.what();
        ready_.notify_all();
    }
    if (available)
        CloseHandle(available);
    peak = 0;
}
AudioFormat Microphone::format() {
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::seconds(3), [this] { return started_ || !error_.empty(); }))
        throw std::runtime_error("The microphone did not start. Check the audio input.");
    if (!error_.empty()) throw std::runtime_error(error_);
    return format_;
}
void Microphone::shutdown() {
    if (stopEvent_)
        SetEvent(stopEvent_);
    if (thread_.joinable())
        thread_.join();
    if (stopEvent_)
        CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    peak = 0;
}
} // namespace youcam
