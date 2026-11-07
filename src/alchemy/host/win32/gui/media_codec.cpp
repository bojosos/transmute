// Win32 host backend, area "media_codec" (include/alchemy/host/media_codec.hpp). Decoders are the
// synchronous software decoder transforms Media Foundation lists (hardware and asynchronous decoders want a
// Direct3D device manager this layer does not give; Microsoft's decoders use the GPU themselves where they can):
// H.264 and HEVC from Annex B (the length-prefixed samples converted, the parameter sets ahead of the first
// sample and each keyframe) to NV12 or P010 read through IMF2DBuffer and cropped to the display aperture;
// AAC from raw frames and the AudioSpecificConfig (in MF_MT_USER_DATA after the HEAACWAVEINFO fields) to 16-bit
// PCM. Encoders are the encoder transforms
// Media Foundation lists (MFTEnumEx: Microsoft's own and the graphics drivers' hardware encoders; nothing
// is shipped with Alchemy). Each encoder owns a thread that does all its COM work (MTA). Synchronous
// transforms are driven by ProcessInput / ProcessOutput; asynchronous (hardware) ones are unlocked and
// driven by their METransformNeedInput / METransformHaveOutput events. Video output is H.264 or HEVC Annex
// B: it is split into NAL units, the parameter sets kept aside and access-unit delimiters dropped, the
// rest given 4-byte lengths. AAC output is raw frames; the AudioSpecificConfig is the tail of the output
// type's MF_MT_USER_DATA (HEAACWAVEINFO). What an AAC encoder takes is asked of the encoder: input types
// it accepts (test-only) and the output types it then offers for that input's rate and channels.
#include "alchemy/host/media_codec.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <initguid.h>
#include <codecapi.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace alchemy::host::media_codec {

namespace {

template <class T> struct Com {
    T* p{};
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : p(std::exchange(other.p, nullptr)) {}
    Com& operator=(Com&& other) noexcept {
        if (this != &other) { reset(); p = std::exchange(other.p, nullptr); }
        return *this;
    }
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// A thread for one encoder's COM work: run() executes a job there and waits for it.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    template <class F> auto run(F work) -> decltype(work()) {
        std::packaged_task<decltype(work())()> task(std::move(work));
        auto done = task.get_future();
        {
            std::lock_guard lock(mutex_);
            jobs_.push_back([&task] { task(); });
        }
        wake_.notify_all();
        return done.get();
    }

private:
    void loop() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
                if (jobs_.empty()) break;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool quit_{};
    std::thread thread_;
};

bool started() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    return ok;
}

std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string guid_text(const GUID& g) {
    wchar_t text[64]{};
    StringFromGUID2(g, text, 64);
    return utf8(text);
}

GUID subtype_of(Codec codec) {
    return codec == Codec::h264 ? MFVideoFormat_H264 : codec == Codec::hevc ? MFVideoFormat_HEVC : MFAudioFormat_AAC;
}

// The audio decoders' held-back frames (counted by the host GUI test: what comes out at the drain of a decoded stream)
// and the audio encoders' delays (the frames their output starts with), by CLSID, as measure_delay measured them on
// Windows 11: noise encoded, decoded by the system's AAC decoder, the shift that best matches the input plus the
// frames the decoder held back to hide its own delay (Microsoft's encoder: shift 0, one 1024-frame block held).
// The host GUI test measures them again and fails if they differ. An encoder not listed has an unknown delay (0)
// and is not used for files or AudioConverter.
struct KnownDelay { const char* clsid; std::uint32_t frames; };
constexpr KnownDelay known_delays[] = {
    {"{93AF0C51-2275-45D2-A35B-F2BA21CAED00}", 1024},   // Microsoft AAC Audio Encoder MFT (48 kHz stereo, Windows 11, 2026-11-06)
    {"{32D186A7-218F-4C75-8876-DD77273A8999}", 1024},   // Microsoft AAC Audio Decoder MFT: frames it holds back until the drain (2026-11-07)
};

// The encoder (or decoder) transforms of `codec`, preferred first (Media Foundation's sort: hardware before
// software). Decoders: the synchronous software ones.
struct Found {
    Com<IMFActivate> activate;
    Component component;
};
std::vector<Found> enumerate(Codec codec, Direction direction = Direction::encode) {
    std::vector<Found> out;
    if (!started()) return out;
    const bool audio = codec == Codec::aac, decode = direction == Direction::decode;
    const MFT_REGISTER_TYPE_INFO raw{audio ? MFMediaType_Audio : MFMediaType_Video, audio ? MFAudioFormat_PCM : MFVideoFormat_NV12};
    const MFT_REGISTER_TYPE_INFO coded{audio ? MFMediaType_Audio : MFMediaType_Video, subtype_of(codec)};
    MFT_REGISTER_TYPE_INFO input = decode ? coded : raw, output = decode ? raw : coded;
    IMFActivate** list{};
    UINT32 count{};
    const UINT32 flags = decode ? MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER
                                : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    const GUID category = decode ? (audio ? MFT_CATEGORY_AUDIO_DECODER : MFT_CATEGORY_VIDEO_DECODER) : (audio ? MFT_CATEGORY_AUDIO_ENCODER : MFT_CATEGORY_VIDEO_ENCODER);
    if (FAILED(MFTEnumEx(category, flags, &input, &output, &list, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) {
        Found f;
        f.activate.p = list[i];
        f.component.codec = codec;
        f.component.direction = direction;
        wchar_t* name{};
        UINT32 length{};
        if (SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
            f.component.name = utf8(name);
            CoTaskMemFree(name);
        }
        GUID clsid{};
        f.component.id = SUCCEEDED(list[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid)) ? guid_text(clsid) : "mft-" + std::to_string(i);
        UINT32 mft_flags{};
        f.component.hardware = SUCCEEDED(list[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &mft_flags)) && (mft_flags & MFT_ENUM_FLAG_HARDWARE);
        for (const auto& known : known_delays)
            if (_stricmp(known.clsid, f.component.id.c_str()) == 0) f.component.delay = known.frames;
        out.push_back(std::move(f));
    }
    CoTaskMemFree(list);
    return out;
}

// The transform's stream identifiers (0 and 0 when it does not say).
void stream_ids(IMFTransform* t, DWORD& in, DWORD& out) {
    in = out = 0;
    DWORD ins[1]{}, outs[1]{};
    if (SUCCEEDED(t->GetStreamIDs(1, ins, 1, outs))) { in = ins[0]; out = outs[0]; }
}

void set_codec_value(ICodecAPI* api, const GUID& key, UINT32 value) {
    if (!api) return;
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    static_cast<void>(api->SetValue(&key, &v));
}
void set_codec_bool(ICodecAPI* api, const GUID& key, bool value) {
    if (!api) return;
    VARIANT v{};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    static_cast<void>(api->SetValue(&key, &v));
}

// A transform being driven: synchronous, or asynchronous through its events.
class Driver {
public:
    Error start(IMFActivate* activate) {
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(transform.put())))) return Error::io;
        Com<IMFAttributes> attributes;
        UINT32 async = 0;
        if (SUCCEEDED(transform->GetAttributes(attributes.put())) && attributes &&
            SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
            if (FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)) ||
                FAILED(transform->QueryInterface(IID_PPV_ARGS(events.put()))))
                return Error::io;
        }
        static_cast<void>(transform->QueryInterface(IID_PPV_ARGS(codec_api.put())));
        stream_ids(transform.p, in_id, out_id);
        return Error::none;
    }
    Error begin() {
        if (FAILED(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
            return Error::io;
        return Error::none;
    }
    // Feeds a sample, collecting what comes out on the way.
    Error feed(IMFSample* sample, const std::function<void(IMFSample*)>& output) {
        if (events) {
            while (!need_input) {
                if (const auto error = next_event(true, output); error != Error::none) return error;
            }
            --need_input;
            if (FAILED(transform->ProcessInput(in_id, sample, 0))) return Error::io;
            return pump_events(output);
        }
        HRESULT hr = transform->ProcessInput(in_id, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (const auto error = pull(output); error != Error::none) return error;
            hr = transform->ProcessInput(in_id, sample, 0);
        }
        if (FAILED(hr)) return Error::io;
        return pull(output);
    }
    // Everything the transform holds; then a new stream starts, so it takes input again (an asynchronous one asks
    // for none until it is told the stream starts, and its earlier requests are void).
    Error drain(const std::function<void(IMFSample*)>& output) {
        static_cast<void>(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
        if (FAILED(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) return Error::io;
        if (events) {
            drained = false;
            while (!drained) {
                if (const auto error = next_event(true, output); error != Error::none) return error;
            }
            need_input = 0;
        } else if (const auto error = pull(output); error != Error::none) {
            return error;
        }
        if (FAILED(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return Error::io;
        return Error::none;
    }
    void stop() {
        if (transform) {
            static_cast<void>(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
            if (events) {
                Com<IMFShutdown> shutdown;
                if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(shutdown.put())))) static_cast<void>(shutdown->Shutdown());
            }
        }
    }

    Com<IMFTransform> transform;
    Com<IMFMediaEventGenerator> events;
    Com<ICodecAPI> codec_api;
    DWORD in_id{}, out_id{};
    // On a stream change: the output subtypes to take, in preference (empty: the first offered), and who to tell.
    std::vector<GUID> preferred_outputs;
    std::function<void()> output_type_changed;
    // Forgets what the transform holds (a seek).
    void flush() {
        if (transform) static_cast<void>(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
        need_input = 0;
    }
    // Sets the first output type offered of the preferred subtypes (or the first offered).
    bool choose_output() {
        for (const auto& wanted : preferred_outputs.empty() ? std::vector<GUID>{GUID_NULL} : preferred_outputs)
            for (DWORD i = 0;; ++i) {
                Com<IMFMediaType> type;
                if (FAILED(transform->GetOutputAvailableType(out_id, i, type.put()))) break;
                GUID subtype{};
                if (wanted != GUID_NULL && (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != wanted)) continue;
                if (SUCCEEDED(transform->SetOutputType(out_id, type.p, 0))) {
                    if (output_type_changed) output_type_changed();
                    return true;
                }
            }
        return false;
    }

private:
    // Every output the transform has now (synchronous transforms).
    Error pull(const std::function<void(IMFSample*)>& output) {
        for (;;) {
            const auto result = output_once(output);
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return Error::none;
            if (FAILED(result)) return Error::io;
        }
    }
    HRESULT output_once(const std::function<void(IMFSample*)>& output) {
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(transform->GetOutputStreamInfo(out_id, &info))) return E_FAIL;
        Com<IMFSample> sample;
        const bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        if (!provides) {
            Com<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(sample.put())) || FAILED(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1 << 20), buffer.put())) ||
                FAILED(sample->AddBuffer(buffer.p)))
                return E_OUTOFMEMORY;
        }
        MFT_OUTPUT_DATA_BUFFER data{};
        data.dwStreamID = out_id;
        data.pSample = sample.p;
        DWORD status{};
        HRESULT hr = transform->ProcessOutput(0, 1, &data, &status);
        if (data.pEvents) data.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // A new output type (the encoder or decoder settled its format).
            return choose_output() ? S_OK : E_FAIL;
        }
        if (SUCCEEDED(hr) && data.pSample) output(data.pSample);
        if (provides && data.pSample) data.pSample->Release();
        return hr;
    }
    // One event: NeedInput counts up, HaveOutput is taken, DrainComplete ends a drain.
    Error next_event(bool wait, const std::function<void(IMFSample*)>& output) {
        Com<IMFMediaEvent> event;
        const HRESULT hr = events->GetEvent(wait ? 0 : MF_EVENT_FLAG_NO_WAIT, event.put());
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return Error::would_block;
        if (FAILED(hr)) return Error::io;
        MediaEventType type{};
        event->GetType(&type);
        if (type == METransformNeedInput) ++need_input;
        else if (type == METransformHaveOutput) {
            const auto result = output_once(output);
            if (FAILED(result) && result != MF_E_TRANSFORM_NEED_MORE_INPUT) return Error::io;
        } else if (type == METransformDrainComplete) drained = true;
        return Error::none;
    }
    Error pump_events(const std::function<void(IMFSample*)>& output) {
        for (;;) {
            const auto error = next_event(false, output);
            if (error == Error::would_block) return Error::none;
            if (error != Error::none) return error;
        }
    }
    unsigned need_input{};
    bool drained{};
};

// The bytes of a sample.
std::vector<std::uint8_t> bytes_of(IMFSample* sample) {
    std::vector<std::uint8_t> out;
    Com<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) return out;
    BYTE* data{};
    DWORD length{};
    if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
        out.assign(data, data + length);
        buffer->Unlock();
    }
    return out;
}

Sample sample_of(IMFSample* sample) {
    Sample s;
    LONGLONG time{}, duration{};
    if (SUCCEEDED(sample->GetSampleTime(&time))) s.pts = time;
    if (SUCCEEDED(sample->GetSampleDuration(&duration))) s.duration = duration;
    UINT64 dts{};
    s.dts = SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts)) ? static_cast<std::int64_t>(dts) : s.pts;
    UINT32 clean{};
    s.keyframe = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;
    return s;
}

// The NAL units of an Annex B stream (start codes removed, trailing zero bytes trimmed).
std::vector<std::pair<std::size_t, std::size_t>> nal_units(const std::vector<std::uint8_t>& b) {
    std::vector<std::pair<std::size_t, std::size_t>> units;
    std::size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= b.size()) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (start != SIZE_MAX) units.emplace_back(start, i);
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX) units.emplace_back(start, b.size());
    for (auto& [from, to] : units)
        while (to > from && b[to - 1] == 0) --to;
    return units;
}

} // namespace

// ---- video ------------------------------------------------------------------------------------------

class VideoEncoder {
public:
    Worker worker;
    Com<IMFActivate> activate;
    Driver driver;
    Component component;
    VideoSettings settings;
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    std::vector<Sample> ready;

    // Splits an output sample: parameter sets aside, delimiters dropped, the rest length-prefixed.
    void take(IMFSample* out) {
        auto s = sample_of(out);
        const auto bytes = bytes_of(out);
        const bool hevc = settings.codec == Codec::hevc;
        std::vector<std::vector<std::uint8_t>> sets;
        for (const auto& [from, to] : nal_units(bytes)) {
            if (to <= from) continue;
            const unsigned type = hevc ? (bytes[from] >> 1) & 0x3f : bytes[from] & 0x1f;
            const bool parameter = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
            const bool delimiter = hevc ? type == 35 : type == 9;
            if (parameter) {
                sets.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(to));
                continue;
            }
            if (delimiter) continue;
            const auto n = static_cast<std::uint32_t>(to - from);
            for (int k = 3; k >= 0; --k) s.data.push_back(static_cast<std::uint8_t>(n >> (8 * k)));
            s.data.insert(s.data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(to));
        }
        // The latest of each kind (a keyframe may repeat only some of them), in stream order: VPS, SPS,
        // PPS (HEVC) or SPS, PPS (H.264).
        const auto key = [hevc](const std::vector<std::uint8_t>& u) { return hevc ? (u[0] >> 1) & 0x3f : u[0] & 0x1f; };
        for (auto& set : sets) {
            const auto same = std::find_if(parameter_sets.begin(), parameter_sets.end(), [&](const auto& kept) { return key(kept) == key(set); });
            if (same != parameter_sets.end()) *same = std::move(set);
            else parameter_sets.push_back(std::move(set));
        }
        std::stable_sort(parameter_sets.begin(), parameter_sets.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
        if (!s.data.empty()) ready.push_back(std::move(s));
    }
};

Result<std::vector<Component>> encoders(Codec codec) {
    Worker worker;
    return worker.run([codec]() -> Result<std::vector<Component>> {
        if (!started()) return Error::unsupported;
        std::vector<Component> out;
        for (auto& f : enumerate(codec)) {
            if (codec == Codec::aac) {
                // What it takes: the input types it accepts, then the output bit rates it offers for stereo
                // 48 kHz (or the first input it took).
                Com<IMFTransform> t;
                if (FAILED(f.activate->ActivateObject(IID_PPV_ARGS(t.put())))) continue;
                DWORD in_id{}, out_id{};
                stream_ids(t.p, in_id, out_id);
                const auto pcm = [](UINT32 rate, UINT32 channels) {
                    Com<IMFMediaType> in;
                    if (FAILED(MFCreateMediaType(in.put()))) return in;
                    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
                    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
                    return in;
                };
                std::pair<UINT32, UINT32> probe{};
                for (const UINT32 rate : {8000u, 11025u, 12000u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u, 64000u, 88200u, 96000u})
                    for (UINT32 channels = 1; channels <= 8; ++channels) {
                        const auto in = pcm(rate, channels);
                        if (!in || FAILED(t->SetInputType(in_id, in.p, MFT_SET_TYPE_TEST_ONLY))) continue;
                        if (std::find(f.component.sample_rates.begin(), f.component.sample_rates.end(), rate) == f.component.sample_rates.end())
                            f.component.sample_rates.push_back(rate);
                        if (std::find(f.component.channel_counts.begin(), f.component.channel_counts.end(), channels) == f.component.channel_counts.end())
                            f.component.channel_counts.push_back(channels);
                        if (!probe.first || (rate == 48000 && channels == 2)) probe = {rate, channels};
                    }
                if (probe.first) {
                    const auto in = pcm(probe.first, probe.second);
                    if (in && SUCCEEDED(t->SetInputType(in_id, in.p, 0))) {
                        for (DWORD i = 0;; ++i) {
                            Com<IMFMediaType> offered;
                            if (FAILED(t->GetOutputAvailableType(out_id, i, offered.put()))) break;
                            UINT32 bytes{}, offered_rate{}, offered_channels{};
                            if (SUCCEEDED(offered->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes)) &&
                                SUCCEEDED(offered->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &offered_rate)) && offered_rate == probe.first &&
                                SUCCEEDED(offered->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &offered_channels)) && offered_channels == probe.second &&
                                std::find(f.component.bit_rates.begin(), f.component.bit_rates.end(), bytes * 8) == f.component.bit_rates.end())
                                f.component.bit_rates.push_back(bytes * 8);
                        }
                    }
                }
                std::sort(f.component.sample_rates.begin(), f.component.sample_rates.end());
                std::sort(f.component.channel_counts.begin(), f.component.channel_counts.end());
                std::sort(f.component.bit_rates.begin(), f.component.bit_rates.end());
                t.reset();
                static_cast<void>(f.activate->ShutdownObject());
                if (f.component.sample_rates.empty()) continue;
            }
            out.push_back(std::move(f.component));
        }
        return out;
    });
}

Result<VideoEncoder*> open_video(const VideoSettings& settings) {
    if (settings.codec == Codec::aac) return Error::invalid_argument;
    if (!settings.width || !settings.height || (settings.width & 1) || (settings.height & 1) || !(settings.frame_rate > 0)) return Error::invalid_argument;
    auto encoder = std::make_unique<VideoEncoder>();
    encoder->settings = settings;
    auto* e = encoder.get();
    const auto opened = e->worker.run([e]() -> Error {
        if (!started()) return Error::unsupported;
        auto found = enumerate(e->settings.codec);
        bool any = false;
        for (auto& f : found) {
            if (!e->settings.component.empty() && f.component.id != e->settings.component) continue;
            if (e->settings.require_hardware && !f.component.hardware) continue;
            any = true;
            Driver d;
            if (d.start(f.activate.p) != Error::none) { static_cast<void>(f.activate->ShutdownObject()); continue; }
            const auto& s = e->settings;
            const auto rate_num = static_cast<UINT32>(std::lround(s.frame_rate * 1000)), rate_den = 1000u;
            const UINT32 bits = s.bit_rate ? s.bit_rate
                                           : static_cast<UINT32>(std::min(100e6, std::max(250e3, double(s.width) * s.height * s.frame_rate * 0.1)));
            // Codec settings before the types (some encoders fix their stream at SetOutputType).
            set_codec_value(d.codec_api.p, CODECAPI_AVEncMPVDefaultBPictureCount, s.frame_reordering ? 2 : 0);
            if (s.keyframe_interval) set_codec_value(d.codec_api.p, CODECAPI_AVEncMPVGOPSize, s.keyframe_interval);
            set_codec_bool(d.codec_api.p, CODECAPI_AVLowLatencyMode, s.real_time);
            set_codec_value(d.codec_api.p, CODECAPI_AVEncCommonMeanBitRate, bits);
            Com<IMFMediaType> out_type, in_type;
            if (FAILED(MFCreateMediaType(out_type.put())) || FAILED(MFCreateMediaType(in_type.put()))) return Error::no_memory;
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out_type->SetGUID(MF_MT_SUBTYPE, subtype_of(s.codec));
            out_type->SetUINT32(MF_MT_AVG_BITRATE, bits);
            MFSetAttributeSize(out_type.p, MF_MT_FRAME_SIZE, s.width, s.height);
            MFSetAttributeRatio(out_type.p, MF_MT_FRAME_RATE, rate_num, rate_den);
            MFSetAttributeRatio(out_type.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            switch (s.profile) {
            case Profile::h264_baseline: out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base); break;
            case Profile::h264_main: out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main); break;
            case Profile::h264_high: out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High); break;
            case Profile::hevc_main: out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8); break;
            case Profile::hevc_main10: out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_10); break;
            default: break;
            }
            // eAVEncH264VLevel and eAVEncH265VLevel are the level_idc values themselves.
            if (s.level) out_type->SetUINT32(MF_MT_MPEG2_LEVEL, s.level);
            bool typed = SUCCEEDED(d.transform->SetOutputType(d.out_id, out_type.p, 0));
            if (typed) {
                in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                MFSetAttributeSize(in_type.p, MF_MT_FRAME_SIZE, s.width, s.height);
                MFSetAttributeRatio(in_type.p, MF_MT_FRAME_RATE, rate_num, rate_den);
                MFSetAttributeRatio(in_type.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                typed = SUCCEEDED(d.transform->SetInputType(d.in_id, in_type.p, 0));
                // Some encoders want one of their own NV12 types, completed with the size and rate.
                for (DWORD i = 0; !typed; ++i) {
                    Com<IMFMediaType> offered;
                    if (FAILED(d.transform->GetInputAvailableType(d.in_id, i, offered.put()))) break;
                    GUID subtype{};
                    if (FAILED(offered->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
                    MFSetAttributeSize(offered.p, MF_MT_FRAME_SIZE, s.width, s.height);
                    MFSetAttributeRatio(offered.p, MF_MT_FRAME_RATE, rate_num, rate_den);
                    typed = SUCCEEDED(d.transform->SetInputType(d.in_id, offered.p, 0));
                }
            }
            if (!typed || d.begin() != Error::none) {
                d.stop();
                d.transform.reset();
                static_cast<void>(f.activate->ShutdownObject());
                continue;
            }
            e->driver = std::move(d);
            e->activate = std::move(f.activate);
            e->component = std::move(f.component);
            return Error::none;
        }
        return any ? Error::invalid_argument : Error::not_found;
    });
    if (opened != Error::none) return opened;
    return encoder.release();
}

const Component& component(const VideoEncoder* encoder) noexcept { return encoder->component; }

Status encode(VideoEncoder* encoder, const Picture& picture, bool keyframe, std::vector<Sample>& out) {
    if (!encoder || picture.width != encoder->settings.width || picture.height != encoder->settings.height || !picture.luma || !picture.chroma)
        return Error::invalid_argument;
    const auto error = encoder->worker.run([&]() -> Error {
        const DWORD luma = picture.width * picture.height, size = luma + luma / 2;
        Com<IMFMediaBuffer> buffer;
        Com<IMFSample> sample;
        if (FAILED(MFCreateMemoryBuffer(size, buffer.put())) || FAILED(MFCreateSample(sample.put())) || FAILED(sample->AddBuffer(buffer.p)))
            return Error::no_memory;
        BYTE* data{};
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return Error::io;
        for (std::uint32_t y = 0; y < picture.height; ++y)
            std::memcpy(data + std::size_t(y) * picture.width, picture.luma + y * picture.luma_stride, picture.width);
        for (std::uint32_t y = 0; y < picture.height / 2; ++y)
            std::memcpy(data + luma + std::size_t(y) * picture.width, picture.chroma + y * picture.chroma_stride, picture.width);
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        sample->SetSampleTime(picture.pts);
        sample->SetSampleDuration(picture.duration);
        if (keyframe) {
            set_codec_value(encoder->driver.codec_api.p, CODECAPI_AVEncVideoForceKeyFrame, 1);
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        }
        return encoder->driver.feed(sample.p, [encoder](IMFSample* s) { encoder->take(s); });
    });
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(VideoEncoder* encoder, std::vector<Sample>& out) {
    if (!encoder) return Error::invalid_argument;
    const auto error = encoder->worker.run([encoder]() -> Error { return encoder->driver.drain([encoder](IMFSample* s) { encoder->take(s); }); });
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

std::vector<std::vector<std::uint8_t>> parameter_sets(const VideoEncoder* encoder) { return encoder ? encoder->parameter_sets : std::vector<std::vector<std::uint8_t>>{}; }

void close(VideoEncoder* encoder) noexcept {
    if (!encoder) return;
    encoder->worker.run([encoder] {
        encoder->driver.stop();
        encoder->driver.transform.reset();
        encoder->driver.events.reset();
        encoder->driver.codec_api.reset();
        if (encoder->activate) static_cast<void>(encoder->activate->ShutdownObject());
        encoder->activate.reset();
        return 0;
    });
    delete encoder;
}

// ---- audio ------------------------------------------------------------------------------------------

class AudioEncoder {
public:
    Worker worker;
    Com<IMFActivate> activate;
    Driver driver;
    Component component;
    AudioSettings settings;
    std::uint32_t bit_rate{};
    std::vector<std::uint8_t> configuration;
    std::uint64_t frames_in{};
    std::vector<Sample> ready;
    void take(IMFSample* out) {
        auto s = sample_of(out);
        s.data = bytes_of(out);
        s.keyframe = true;
        if (!s.data.empty()) ready.push_back(std::move(s));
    }
};

Result<AudioEncoder*> open_audio(const AudioSettings& settings) {
    if (settings.codec != Codec::aac || !settings.sample_rate || !settings.channels) return Error::invalid_argument;
    auto encoder = std::make_unique<AudioEncoder>();
    encoder->settings = settings;
    auto* e = encoder.get();
    const auto opened = e->worker.run([e]() -> Error {
        if (!started()) return Error::unsupported;
        auto found = enumerate(Codec::aac);
        bool any = false;
        for (auto& f : found) {
            if (!e->settings.component.empty() && f.component.id != e->settings.component) continue;
            any = true;
            Driver d;
            if (d.start(f.activate.p) != Error::none) { static_cast<void>(f.activate->ShutdownObject()); continue; }
            const auto& s = e->settings;
            Com<IMFMediaType> in;
            if (FAILED(MFCreateMediaType(in.put()))) return Error::no_memory;
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, s.sample_rate);
            in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, s.channels);
            in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, s.channels * 2);
            in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, s.sample_rate * s.channels * 2);
            bool typed = SUCCEEDED(d.transform->SetInputType(d.in_id, in.p, 0));
            // The output type it offers nearest the asked bit rate (raw AAC: payload type 0).
            Com<IMFMediaType> chosen;
            UINT32 chosen_bits = 0;
            for (DWORD i = 0; typed; ++i) {
                Com<IMFMediaType> offered;
                if (FAILED(d.transform->GetOutputAvailableType(d.out_id, i, offered.put()))) break;
                UINT32 bytes{}, payload{}, offered_rate{}, offered_channels{};
                if (FAILED(offered->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes))) continue;
                // It offers types for other rates and channel counts too: only those of the input.
                if (FAILED(offered->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &offered_rate)) || offered_rate != s.sample_rate ||
                    FAILED(offered->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &offered_channels)) || offered_channels != s.channels)
                    continue;
                if (SUCCEEDED(offered->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &payload)) && payload != 0) continue;
                const auto want = s.bit_rate ? s.bit_rate : 64000u * std::min(s.channels, 2u);
                if (!chosen || std::abs(double(bytes) * 8 - want) < std::abs(double(chosen_bits) - want)) {
                    chosen = std::move(offered);
                    chosen_bits = bytes * 8;
                }
            }
            typed = typed && chosen && SUCCEEDED(d.transform->SetOutputType(d.out_id, chosen.p, 0));
            if (typed) {
                // MF_MT_USER_DATA of the type set: the HEAACWAVEINFO fields after WAVEFORMATEX (12 bytes), then
                // the AudioSpecificConfig.
                Com<IMFMediaType> current;
                IMFMediaType* holder = SUCCEEDED(d.transform->GetOutputCurrentType(d.out_id, current.put())) ? current.p : chosen.p;
                UINT32 size{};
                if (SUCCEEDED(holder->GetBlobSize(MF_MT_USER_DATA, &size)) && size > 12) {
                    std::vector<std::uint8_t> blob(size);
                    if (SUCCEEDED(holder->GetBlob(MF_MT_USER_DATA, blob.data(), size, nullptr))) e->configuration.assign(blob.begin() + 12, blob.end());
                }
                if (e->configuration.empty()) {
                    // AAC LC (object type 2), the sampling frequency index and the channel configuration (ISO/IEC
                    // 14496-3, 1.6.2.1), as the encoder was set up.
                    static constexpr std::uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
                    const auto found = std::find(std::begin(rates), std::end(rates), s.sample_rate);
                    // Channel configurations 1-6 are those channel counts; 7 is eight channels (7.1).
                    if (found != std::end(rates) && (s.channels <= 6 || s.channels == 8)) {
                        const auto index = static_cast<std::uint32_t>(found - std::begin(rates));
                        const auto channel_config = s.channels == 8 ? 7u : s.channels;
                        e->configuration = {static_cast<std::uint8_t>((2 << 3) | (index >> 1)),
                                            static_cast<std::uint8_t>(((index & 1) << 7) | (channel_config << 3))};
                    }
                }
            }
            if (!typed || e->configuration.empty() || d.begin() != Error::none) {
                d.stop();
                d.transform.reset();
                static_cast<void>(f.activate->ShutdownObject());
                e->configuration.clear();
                continue;
            }
            e->bit_rate = chosen_bits;
            e->driver = std::move(d);
            e->activate = std::move(f.activate);
            e->component = std::move(f.component);
            return Error::none;
        }
        return any ? Error::invalid_argument : Error::not_found;
    });
    if (opened != Error::none) return opened;
    return encoder.release();
}

const Component& component(const AudioEncoder* encoder) noexcept { return encoder->component; }
std::uint32_t bit_rate(const AudioEncoder* encoder) noexcept { return encoder ? encoder->bit_rate : 0; }
std::uint32_t frames_per_packet(const AudioEncoder* encoder) noexcept { return encoder ? 1024 : 0; }
std::vector<std::uint8_t> configuration(const AudioEncoder* encoder) { return encoder ? encoder->configuration : std::vector<std::uint8_t>{}; }

Status encode(AudioEncoder* encoder, std::span<const std::int16_t> samples, std::vector<Sample>& out) {
    if (!encoder || samples.size() % encoder->settings.channels) return Error::invalid_argument;
    if (samples.empty()) return {};
    const auto error = encoder->worker.run([&]() -> Error {
        const auto bytes = static_cast<DWORD>(samples.size() * 2);
        Com<IMFMediaBuffer> buffer;
        Com<IMFSample> sample;
        if (FAILED(MFCreateMemoryBuffer(bytes, buffer.put())) || FAILED(MFCreateSample(sample.put())) || FAILED(sample->AddBuffer(buffer.p)))
            return Error::no_memory;
        BYTE* data{};
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return Error::io;
        std::memcpy(data, samples.data(), bytes);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);
        const auto frames = samples.size() / encoder->settings.channels;
        const auto rate = encoder->settings.sample_rate;
        sample->SetSampleTime(static_cast<LONGLONG>(encoder->frames_in * 10000000ULL / rate));
        sample->SetSampleDuration(static_cast<LONGLONG>(frames * 10000000ULL / rate));
        encoder->frames_in += frames;
        return encoder->driver.feed(sample.p, [encoder](IMFSample* s) { encoder->take(s); });
    });
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(AudioEncoder* encoder, std::vector<Sample>& out) {
    if (!encoder) return Error::invalid_argument;
    const auto error = encoder->worker.run([encoder]() -> Error { return encoder->driver.drain([encoder](IMFSample* s) { encoder->take(s); }); });
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Result<std::uint32_t> measure_delay(const AudioSettings& settings) {
    const auto opened = open_audio(settings);
    if (!opened) return opened.error();
    auto* encoder = *opened;
    const auto rate = settings.sample_rate, channels = settings.channels;
    // One second of deterministic noise, encoded.
    std::vector<std::int16_t> input(std::size_t(rate) * channels);
    std::uint32_t seed = 1;
    for (auto& v : input) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<std::int16_t>(static_cast<std::int32_t>(seed >> 16) % 8000 - 4000);
    }
    std::vector<Sample> packets;
    for (std::size_t at = 0; at < input.size(); at += 1024ULL * channels)
        if (!encode(encoder, std::span(input).subspan(at, std::min<std::size_t>(1024ULL * channels, input.size() - at)), packets)) { close(encoder); return Error::io; }
    if (!drain(encoder, packets)) { close(encoder); return Error::io; }
    const auto config = configuration(encoder);
    close(encoder);
    // Decoded by the host's AAC decoder; what it gives before the drain, and what it held back.
    const auto opened_decoder = open_audio_decoder({Codec::aac, rate, channels, config, {}});
    if (!opened_decoder) return opened_decoder.error();
    auto* decoder = *opened_decoder;
    std::vector<std::int16_t> out;
    for (const auto& p : packets)
        if (!decode(decoder, p.data, out)) { close(decoder); return Error::io; }
    const std::size_t held = out.size();
    const bool drained = static_cast<bool>(drain(decoder, out));
    close(decoder);
    if (!drained) return Error::io;
    const std::size_t span = rate / 2;
    std::uint32_t best = 0;
    double best_score = -1;
    for (std::uint32_t shift = 0; shift <= 4096; ++shift) {
        if ((std::size_t(shift) + span) * channels > out.size()) break;
        double score = 0;
        for (std::size_t i = 0; i < span; ++i) score += double(out[(shift + i) * channels]) * input[i * channels];
        if (score > best_score) { best_score = score; best = shift; }
    }
    if (best_score <= 0) return Error::io;
    // A decoder that hides its own delay holds that many frames back until the drain (Microsoft's holds one
    // 1024-frame block); a player trimming by the stream's priming outputs every frame as it comes, so for it
    // those frames are delay too.
    return best + static_cast<std::uint32_t>((out.size() - held) / channels);
}

void close(AudioEncoder* encoder) noexcept {
    if (!encoder) return;
    encoder->worker.run([encoder] {
        encoder->driver.stop();
        encoder->driver.transform.reset();
        encoder->driver.events.reset();
        encoder->driver.codec_api.reset();
        if (encoder->activate) static_cast<void>(encoder->activate->ShutdownObject());
        encoder->activate.reset();
        return 0;
    });
    delete encoder;
}

// ---- decoding ---------------------------------------------------------------------------------------

Result<std::vector<Component>> decoders(Codec codec) {
    Worker worker;
    return worker.run([codec]() -> Result<std::vector<Component>> {
        if (!started()) return Error::unsupported;
        std::vector<Component> out;
        for (auto& f : enumerate(codec, Direction::decode)) out.push_back(std::move(f.component));
        return out;
    });
}

namespace {
// Annex B: a start code before each NAL unit of a length-prefixed sample.
void append_annex_b(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> sample) {
    for (std::size_t at = 0; at + 4 <= sample.size();) {
        const std::size_t n = (std::size_t(sample[at]) << 24) | (std::size_t(sample[at + 1]) << 16) | (std::size_t(sample[at + 2]) << 8) | sample[at + 3];
        at += 4;
        if (n > sample.size() - at) break;
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), sample.begin() + static_cast<std::ptrdiff_t>(at), sample.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
}
} // namespace

class VideoDecoder {
public:
    Worker worker;
    Com<IMFActivate> activate;
    Driver driver;
    Component component;
    std::vector<std::uint8_t> sets;        // the parameter sets, Annex B
    bool started{};
    std::vector<DecodedPicture> ready;
    // The output type: coded size, pitch (from the buffer), the displayed area, the sample size.
    bool typed{};
    UINT32 coded_width{}, coded_height{};
    MFVideoArea area{};
    std::uint32_t bit_depth{8};

    void refresh() {
        Com<IMFMediaType> type;
        typed = SUCCEEDED(driver.transform->GetOutputCurrentType(driver.out_id, type.put()));
        if (!typed) return;
        MFGetAttributeSize(type.p, MF_MT_FRAME_SIZE, &coded_width, &coded_height);
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        bit_depth = subtype == MFVideoFormat_P010 ? 10 : 8;
        UINT32 size{};
        if (FAILED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, &size)) || size != sizeof area) {
            area = {};
            area.Area = {static_cast<LONG>(coded_width), static_cast<LONG>(coded_height)};
        }
    }
    void take(IMFSample* out) {
        if (!typed) refresh();
        DecodedPicture p;
        LONGLONG time{}, duration{};
        if (SUCCEEDED(out->GetSampleTime(&time))) p.pts = time;
        if (SUCCEEDED(out->GetSampleDuration(&duration))) p.duration = duration;
        p.bit_depth = bit_depth;
        const std::size_t bytes = bit_depth > 8 ? 2 : 1;
        const auto x = static_cast<std::size_t>(area.OffsetX.value), y = static_cast<std::size_t>(area.OffsetY.value);
        p.width = static_cast<std::uint32_t>(area.Area.cx) & ~1u;
        p.height = static_cast<std::uint32_t>(area.Area.cy) & ~1u;
        Com<IMFMediaBuffer> buffer;
        if (FAILED(out->GetBufferByIndex(0, buffer.put()))) return;
        Com<IMF2DBuffer> planar;
        BYTE* base{};
        LONG pitch{};
        DWORD length{};
        const bool two_d = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(planar.put()))) && SUCCEEDED(planar->Lock2D(&base, &pitch));
        if (!two_d) {
            if (FAILED(buffer->Lock(&base, nullptr, &length))) return;
            pitch = static_cast<LONG>(coded_width * bytes);
        }
        if (pitch > 0 && base) {
            const std::size_t row = std::size_t(p.width) * bytes;
            p.luma_stride = p.chroma_stride = row;
            p.luma.resize(row * p.height);
            p.chroma.resize(row * (p.height / 2));
            const BYTE* chroma = base + std::size_t(pitch) * coded_height;
            for (std::uint32_t r = 0; r < p.height; ++r) std::memcpy(p.luma.data() + r * row, base + (y + r) * pitch + x * bytes, row);
            for (std::uint32_t r = 0; r < p.height / 2; ++r) std::memcpy(p.chroma.data() + r * row, chroma + (y / 2 + r) * pitch + (x & ~std::size_t{1}) * bytes, row);
            ready.push_back(std::move(p));
        }
        if (two_d) planar->Unlock2D();
        else buffer->Unlock();
    }
};

Result<VideoDecoder*> open_video_decoder(const VideoDecoderSettings& settings) {
    if (settings.codec == Codec::aac) return Error::invalid_argument;
    auto decoder = std::make_unique<VideoDecoder>();
    for (const auto& set : settings.parameter_sets) append_annex_b(decoder->sets, [&] {
        std::vector<std::uint8_t> prefixed{static_cast<std::uint8_t>(set.size() >> 24), static_cast<std::uint8_t>(set.size() >> 16),
                                           static_cast<std::uint8_t>(set.size() >> 8), static_cast<std::uint8_t>(set.size())};
        prefixed.insert(prefixed.end(), set.begin(), set.end());
        return prefixed;
    }());
    auto* d = decoder.get();
    const auto opened = d->worker.run([d, &settings]() -> Error {
        if (!started()) return Error::unsupported;
        bool any = false;
        for (auto& f : enumerate(settings.codec, Direction::decode)) {
            if (!settings.component.empty() && f.component.id != settings.component) continue;
            any = true;
            Driver driver;
            if (driver.start(f.activate.p) != Error::none) { static_cast<void>(f.activate->ShutdownObject()); continue; }
            Com<IMFMediaType> in;
            if (FAILED(MFCreateMediaType(in.put()))) return Error::no_memory;
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            in->SetGUID(MF_MT_SUBTYPE, subtype_of(settings.codec));
            in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (settings.width && settings.height) MFSetAttributeSize(in.p, MF_MT_FRAME_SIZE, settings.width, settings.height);
            driver.preferred_outputs = {MFVideoFormat_NV12, MFVideoFormat_P010};
            // A decoder that knows its output only from the stream offers none yet: the first stream change sets it.
            const HRESULT typed = driver.transform->SetInputType(driver.in_id, in.p, 0);
            if (SUCCEEDED(typed)) static_cast<void>(driver.choose_output());
            if (FAILED(typed) || driver.begin() != Error::none) {
                driver.stop();
                driver.transform.reset();
                static_cast<void>(f.activate->ShutdownObject());
                continue;
            }
            d->driver = std::move(driver);
            d->driver.output_type_changed = [d] { d->typed = false; };
            d->activate = std::move(f.activate);
            d->component = std::move(f.component);
            return Error::none;
        }
        return any ? Error::invalid_argument : Error::not_found;
    });
    if (opened != Error::none) return opened;
    return decoder.release();
}

const Component& component(const VideoDecoder* decoder) noexcept { return decoder->component; }

Status decode(VideoDecoder* decoder, const Sample& sample, std::vector<DecodedPicture>& out) {
    if (!decoder) return Error::invalid_argument;
    const auto error = decoder->worker.run([&]() -> Error {
        std::vector<std::uint8_t> bytes;
        if (!decoder->started || sample.keyframe) bytes = decoder->sets;
        append_annex_b(bytes, sample.data);
        decoder->started = true;
        Com<IMFMediaBuffer> buffer;
        Com<IMFSample> s;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), buffer.put())) || FAILED(MFCreateSample(s.put())) || FAILED(s->AddBuffer(buffer.p)))
            return Error::no_memory;
        BYTE* data{};
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return Error::io;
        std::memcpy(data, bytes.data(), bytes.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
        s->SetSampleTime(sample.pts);
        s->SetSampleDuration(sample.duration);
        if (sample.keyframe) s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        return decoder->driver.feed(s.p, [decoder](IMFSample* o) { decoder->take(o); });
    });
    for (auto& p : decoder->ready) out.push_back(std::move(p));
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(VideoDecoder* decoder, std::vector<DecodedPicture>& out) {
    if (!decoder) return Error::invalid_argument;
    const auto error = decoder->worker.run([decoder]() -> Error { return decoder->driver.drain([decoder](IMFSample* o) { decoder->take(o); }); });
    for (auto& p : decoder->ready) out.push_back(std::move(p));
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

void flush(VideoDecoder* decoder) noexcept {
    if (!decoder) return;
    decoder->worker.run([decoder] {
        decoder->driver.flush();
        decoder->started = false;
        return 0;
    });
}

void close(VideoDecoder* decoder) noexcept {
    if (!decoder) return;
    decoder->worker.run([decoder] {
        decoder->driver.stop();
        decoder->driver.transform.reset();
        decoder->driver.codec_api.reset();
        if (decoder->activate) static_cast<void>(decoder->activate->ShutdownObject());
        decoder->activate.reset();
        return 0;
    });
    delete decoder;
}

class AudioDecoder {
public:
    Worker worker;
    Com<IMFActivate> activate;
    Driver driver;
    Component component;
    std::uint32_t rate{};
    std::vector<std::int16_t> ready;
    void take(IMFSample* out) {
        const auto bytes = bytes_of(out);
        const auto at = ready.size();
        ready.resize(at + bytes.size() / 2);
        std::memcpy(ready.data() + at, bytes.data(), bytes.size() / 2 * 2);
    }
};

Result<AudioDecoder*> open_audio_decoder(const AudioDecoderSettings& settings) {
    if (settings.codec != Codec::aac || !settings.sample_rate || !settings.channels || settings.configuration.size() < 2) return Error::invalid_argument;
    auto decoder = std::make_unique<AudioDecoder>();
    decoder->rate = settings.sample_rate;
    auto* d = decoder.get();
    const auto opened = d->worker.run([d, &settings]() -> Error {
        if (!started()) return Error::unsupported;
        bool any = false;
        for (auto& f : enumerate(Codec::aac, Direction::decode)) {
            if (!settings.component.empty() && f.component.id != settings.component) continue;
            any = true;
            Driver driver;
            if (driver.start(f.activate.p) != Error::none) { static_cast<void>(f.activate->ShutdownObject()); continue; }
            Com<IMFMediaType> in;
            if (FAILED(MFCreateMediaType(in.put()))) return Error::no_memory;
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            driver.preferred_outputs = {MFAudioFormat_PCM};
            in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, settings.sample_rate);
            in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, settings.channels);
            in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            in->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            // MF_MT_USER_DATA: HEAACWAVEINFO after WAVEFORMATEX (payload type 0: raw; profile 0x29), then the AudioSpecificConfig.
            std::vector<std::uint8_t> user(12, 0);
            user[2] = 0x29;
            user.insert(user.end(), settings.configuration.begin(), settings.configuration.end());
            in->SetBlob(MF_MT_USER_DATA, user.data(), static_cast<UINT32>(user.size()));
            bool typed = SUCCEEDED(driver.transform->SetInputType(driver.in_id, in.p, 0));
            for (DWORD i = 0; typed; ++i) {
                Com<IMFMediaType> offered;
                if (FAILED(driver.transform->GetOutputAvailableType(driver.out_id, i, offered.put()))) { typed = false; break; }
                GUID subtype{};
                UINT32 bits{};
                if (SUCCEEDED(offered->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFAudioFormat_PCM &&
                    SUCCEEDED(offered->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) && bits == 16 && SUCCEEDED(driver.transform->SetOutputType(driver.out_id, offered.p, 0)))
                    break;
            }
            if (!typed || driver.begin() != Error::none) {
                driver.stop();
                driver.transform.reset();
                static_cast<void>(f.activate->ShutdownObject());
                continue;
            }
            d->driver = std::move(driver);
            d->activate = std::move(f.activate);
            d->component = std::move(f.component);
            return Error::none;
        }
        return any ? Error::invalid_argument : Error::not_found;
    });
    if (opened != Error::none) return opened;
    return decoder.release();
}

const Component& component(const AudioDecoder* decoder) noexcept { return decoder->component; }

Status decode(AudioDecoder* decoder, std::span<const std::uint8_t> packet, std::vector<std::int16_t>& out) {
    if (!decoder) return Error::invalid_argument;
    const auto error = decoder->worker.run([&]() -> Error {
        Com<IMFMediaBuffer> buffer;
        Com<IMFSample> s;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(packet.size()), buffer.put())) || FAILED(MFCreateSample(s.put())) || FAILED(s->AddBuffer(buffer.p)))
            return Error::no_memory;
        BYTE* data{};
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return Error::io;
        std::memcpy(data, packet.data(), packet.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(packet.size()));
        return decoder->driver.feed(s.p, [decoder](IMFSample* o) { decoder->take(o); });
    });
    out.insert(out.end(), decoder->ready.begin(), decoder->ready.end());
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(AudioDecoder* decoder, std::vector<std::int16_t>& out) {
    if (!decoder) return Error::invalid_argument;
    const auto error = decoder->worker.run([decoder]() -> Error { return decoder->driver.drain([decoder](IMFSample* o) { decoder->take(o); }); });
    out.insert(out.end(), decoder->ready.begin(), decoder->ready.end());
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

void flush(AudioDecoder* decoder) noexcept {
    if (!decoder) return;
    decoder->worker.run([decoder] {
        decoder->driver.flush();
        return 0;
    });
}

void close(AudioDecoder* decoder) noexcept {
    if (!decoder) return;
    decoder->worker.run([decoder] {
        decoder->driver.stop();
        decoder->driver.transform.reset();
        if (decoder->activate) static_cast<void>(decoder->activate->ShutdownObject());
        decoder->activate.reset();
        return 0;
    });
    delete decoder;
}

} // namespace alchemy::host::media_codec
