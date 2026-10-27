#include "camera.h"
#include <cstring>
namespace youcam {
namespace {
std::wstring attributeString(IMFAttributes* attributes, REFGUID key) {
    WCHAR* text = nullptr; UINT32 length = 0;
    check(attributes->GetAllocatedString(key, &text, &length), "Read camera name");
    std::wstring result(text, length); CoTaskMemFree(text); return result;
}
struct Activations {
    IMFActivate** items = nullptr; UINT32 count = 0;
    Activations() {
        ComPtr<IMFAttributes> config; check(MFCreateAttributes(&config, 1), "Create camera attributes");
        check(config->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), "Set camera source");
        check(MFEnumDeviceSources(config.Get(), &items, &count), "List cameras");
    }
    ~Activations() { for (UINT32 i = 0; i < count; ++i) items[i]->Release(); CoTaskMemFree(items); }
};
}
std::vector<Device> Camera::devices() {
    Activations list; std::vector<Device> result;
    for (UINT32 i = 0; i < list.count; ++i)
        result.push_back({attributeString(list.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK), attributeString(list.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)});
    return result;
}
void Camera::start(const std::wstring& id) {
    shutdown();
    try {
        Activations list;
        for (UINT32 i = 0; i < list.count; ++i) {
            auto key = attributeString(list.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            if (key == id || (id.empty() && i == 0)) {
                name = attributeString(list.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
                check(list.items[i]->ActivateObject(IID_PPV_ARGS(&source_)), "Open camera"); break;
            }
        }
        if (!source_) throw std::runtime_error("No camera available. Connect a camera and allow desktop apps in Windows privacy settings.");
        ComPtr<IMFAttributes> config; check(MFCreateAttributes(&config, 2), "Create camera reader attributes");
        check(config->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "Enable camera color conversion");
        check(MFCreateSourceReaderFromMediaSource(source_.Get(), config.Get(), &reader_), "Create camera reader");
        struct Candidate { VideoMode mode; ComPtr<IMFMediaType> type; };
        std::vector<Candidate> candidates;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> type;
            HRESULT hr = reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
            if (hr == MF_E_NO_MORE_TYPES) break;
            check(hr, "Enumerate camera formats");
            VideoMode item;
            if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &item.width, &item.height))) continue;
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &item.rateNumerator, &item.rateDenominator);
            if (item.width && item.height && item.rateDenominator) candidates.push_back({item, type});
        }
        if (candidates.empty()) throw std::runtime_error("The camera reports no video modes.");
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return betterMode(a.mode, b.mode); });
        const auto maximum = candidates.front().mode.pixels();
        bool configured = false;
        HRESULT last = E_FAIL;
        // Try pixel formats only at the maximum resolution. A failed decoder or
        // USB mode must never quietly turn a 4K camera into a 720p recording.
        for (const auto& item : candidates) {
            if (item.mode.pixels() != maximum) break;
            last = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, item.type.Get());
            if (FAILED(last)) continue;
            ComPtr<IMFMediaType> output; check(MFCreateMediaType(&output), "Create camera output type");
            output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, item.mode.width, item.mode.height);
            MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, item.mode.rateNumerator, item.mode.rateDenominator);
            last = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get());
            if (FAILED(last)) continue;
            ComPtr<IMFMediaType> actual; check(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual), "Verify camera format");
            UINT32 w = 0, h = 0; check(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h), "Read actual camera resolution");
            if (w != item.mode.width || h != item.mode.height) continue;
            mode = item.mode; configured = true; break;
        }
        if (!configured) { std::string detail = "Cannot open the camera at its maximum resolution " + narrow(dimensions(candidates.front().mode.width, candidates.front().mode.height)); check(FAILED(last) ? last : E_FAIL, detail.c_str()); }
        stop_ = false;
        thread_ = std::thread([this] { read(); });
    } catch (const std::exception& error) {
        auto message = error.what(); shutdown(); frames.fail(message); throw;
    }
}
void Camera::read() {
    try {
        Apartment apartment;
        ComPtr<IMFMediaType> type; check(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type), "Get camera pixel layout");
        LONG defaultStride = LONG(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, mode.width * 4));
        while (!stop_) {
            DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
            HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample);
            if (stop_) break;
            check(hr, "Read camera frame");
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) throw std::runtime_error("The camera was disconnected.");
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) throw std::runtime_error("Camera format changed; reopen the camera to preserve full resolution.");
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buffer; check(sample->ConvertToContiguousBuffer(&buffer), "Read camera pixels");
            Frame frame(mode.width, mode.height);
            ComPtr<IMF2DBuffer> twoDimensional;
            BYTE* pixels = nullptr; LONG stride = defaultStride;
            if (SUCCEEDED(buffer.As(&twoDimensional))) {
                check(twoDimensional->Lock2D(&pixels, &stride), "Lock camera image");
                for (UINT32 y = 0; y < mode.height; ++y) std::memcpy(&frame.bgra[size_t(y) * mode.width * 4], pixels + ptrdiff_t(y) * stride, size_t(mode.width) * 4);
                twoDimensional->Unlock2D();
            } else {
                DWORD length = 0; check(buffer->Lock(&pixels, nullptr, &length), "Lock camera buffer");
                if (uint64_t(std::abs(stride)) * mode.height > length) { buffer->Unlock(); throw std::runtime_error("Incomplete full-resolution camera frame."); }
                BYTE* top = stride < 0 ? pixels + size_t(mode.height - 1) * size_t(-stride) : pixels;
                for (UINT32 y = 0; y < mode.height; ++y) std::memcpy(&frame.bgra[size_t(y) * mode.width * 4], top + ptrdiff_t(y) * stride, size_t(mode.width) * 4);
                buffer->Unlock();
            }
            frames.put(std::move(frame));
        }
    } catch (const std::exception& error) { if (!stop_) frames.fail(error.what()); }
}
void Camera::shutdown() {
    stop_ = true;
    // Shutdown releases a pending synchronous ReadSample on device removal too.
    if (source_) source_->Shutdown();
    if (thread_.joinable()) thread_.join();
    reader_.Reset(); source_.Reset(); mode = {}; frames.clear();
}
} // namespace youcam
