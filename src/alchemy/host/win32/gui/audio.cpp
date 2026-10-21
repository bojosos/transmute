// Win32 host backend, area "audio" (include/alchemy/host/audio.hpp): the
// active render and capture endpoints of the MMDevice API with their friendly
// names, endpoint IDs, shared-mode formats (PKEY_AudioEngine_DeviceFormat:
// rate, channels, speaker mask) and form factors, plus the console default
// render and capture endpoints. Only property stores are read: no audio
// client is activated. The enumeration runs on its own short-lived thread in
// the multithreaded apartment, so the caller's COM state is never touched.
#include "alchemy/host/audio.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

namespace alchemy::host::audio {
namespace {

template <class T>
struct Com {
    T* pointer{};
    ~Com() { if (pointer) pointer->Release(); }
    T** put() { return &pointer; }
    T* operator->() const { return pointer; }
    explicit operator bool() const { return pointer != nullptr; }
};

std::string endpoint_id(IMMDevice* device) {
    LPWSTR id{};
    if (FAILED(device->GetId(&id)) || !id) return {};
    auto text = win32::narrow(id);
    CoTaskMemFree(id);
    return text;
}

Endpoints enumerate() {
    Endpoints result;
    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put()))))
        return result;
    Com<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, collection.put()))) return result;
    UINT count{};
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;
        Endpoint info;
        info.uid = endpoint_id(device.pointer);
        Com<IMMEndpoint> endpoint;
        EDataFlow flow = eRender;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(endpoint.put())))) endpoint->GetDataFlow(&flow);
        info.input = flow == eCapture;
        Com<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.put()))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
                info.name = win32::narrow(value.pwszVal);
            PropVariantClear(&value);
            if (SUCCEEDED(store->GetValue(PKEY_AudioEndpoint_FormFactor, &value)) && value.vt == VT_UI4) info.form_factor = value.ulVal;
            PropVariantClear(&value);
            if (SUCCEEDED(store->GetValue(PKEY_AudioEngine_DeviceFormat, &value)) && value.vt == VT_BLOB && value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                const auto* format = reinterpret_cast<const WAVEFORMATEX*>(value.blob.pBlobData);
                info.has_format = true;
                info.channels = format->nChannels;
                info.sample_rate = format->nSamplesPerSec;
                if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && value.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE))
                    info.channel_mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask;
            }
            PropVariantClear(&value);
        }
        result.endpoints.push_back(std::move(info));
    }
    for (const auto flow : {eRender, eCapture}) {
        Com<IMMDevice> device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.put())) && device)
            (flow == eRender ? result.default_output : result.default_input) = endpoint_id(device.pointer);
    }
    return result;
}

} // namespace

Result<Endpoints> endpoints() {
    Endpoints result;
    std::thread worker([&result] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        result = enumerate();
        if (SUCCEEDED(initialized)) CoUninitialize();
    });
    worker.join();
    return result;
}

} // namespace alchemy::host::audio
