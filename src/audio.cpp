#include "audio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

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
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = 96000;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &format, nullptr),
              "Open 48 kHz microphone stream");
        available = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!available)
            throw std::runtime_error("Cannot create audio buffer event.");
        check(client->SetEventHandle(available), "Set microphone notification");
        ComPtr<IAudioCaptureClient> capture;
        check(client->GetService(IID_PPV_ARGS(&capture)), "Open microphone buffers");
        check(client->Start(), "Start microphone");
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
                std::vector<int16_t> output(count, 0);
                float maximum = 0;
                double gain = std::pow(10.0, gainDB.load() / 20.0);
                if (!(status & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const auto *input = reinterpret_cast<const int16_t *>(data);
                    for (UINT32 i = 0; i < count; ++i) {
                        double value = input[i] * gain;
                        maximum = std::max(maximum, float(std::abs(value) / 32768));
                        output[i] = int16_t(std::clamp(value, -32768.0, 32767.0));
                    }
                }
                capture->ReleaseBuffer(count);
                peak = maximum;
                if ((status & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !timestamp)
                    timestamp = clock100ns() - int64_t(count) * 10000000 / 48000;
                if (samples)
                    samples(std::move(output), int64_t(timestamp));
                check(capture->GetNextPacketSize(&packet), "Poll microphone samples");
            }
        }
        client->Stop();
    } catch (const std::exception &error) {
        std::lock_guard lock(mutex_);
        error_ = error.what();
    }
    if (available)
        CloseHandle(available);
    peak = 0;
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
