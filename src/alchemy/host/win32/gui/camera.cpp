// Win32 host backend, area "camera" (include/alchemy/host/camera.hpp): video capture through Media
// Foundation, the system's own components (mfplat, mf, mfreadwrite). Devices are the video capture
// sources MFEnumDeviceSources lists; their formats are the source reader's native media types. A
// stream is a source reader with video processing enabled, its current type set to the chosen native
// type and its output to NV12 (Media Foundation's decoders and converters do MJPG, YUY2 and the rest);
// one thread per stream does all COM and reads samples while running. The privacy setting is
// Windows' capability consent store for the webcam (audio_input.cpp reads the store).
#include "alchemy/host/camera.hpp"
#include "alchemy/host/audio.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace alchemy::host::audio {
Access consent_store_access(const wchar_t* capability);
}

namespace alchemy::host::camera {
namespace {

template <class T> struct Com {
    T* p{};
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Media Foundation started once for the process.
bool started() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    return ok;
}

std::string string_of(IMFActivate* activate, const GUID& key) {
    wchar_t* value{};
    UINT32 length{};
    if (FAILED(activate->GetAllocatedString(key, &value, &length)) || !value) return {};
    std::string out = win32::narrow(value);
    CoTaskMemFree(value);
    return out;
}

// The video capture sources (each +1), in Media Foundation's order.
std::vector<IMFActivate*> sources() {
    std::vector<IMFActivate*> out;
    Com<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(attributes.put(), 1)) ||
        FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return out;
    IMFActivate** list{};
    UINT32 count{};
    if (FAILED(MFEnumDeviceSources(attributes.p, &list, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) out.push_back(list[i]);
    CoTaskMemFree(list);
    return out;
}

std::uint32_t fourcc_of(const GUID& subtype) {
    // Video subtypes are FOURCC-based GUIDs: Data1 holds the code, little-endian ("NV12" = '21VN').
    const auto v = subtype.Data1;
    return ((v & 0xff) << 24) | (((v >> 8) & 0xff) << 16) | (((v >> 16) & 0xff) << 8) | (v >> 24);
}

struct Native { Format format; DWORD index{}; };

// The source reader's native types: one per size and subtype, at the fastest rate.
std::vector<Native> native_types(IMFSourceReader* reader) {
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Native> by_key;
    for (DWORD i = 0;; ++i) {
        Com<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, type.put()))) break;
        UINT32 width{}, height{}, numerator{}, denominator{}, min_n{}, min_d{}, max_n{}, max_d{};
        GUID subtype{};
        if (FAILED(MFGetAttributeSize(type.p, MF_MT_FRAME_SIZE, &width, &height)) || FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        MFGetAttributeRatio(type.p, MF_MT_FRAME_RATE, &numerator, &denominator);
        const double rate = denominator ? static_cast<double>(numerator) / denominator : 0;
        double low = rate, high = rate;
        if (SUCCEEDED(MFGetAttributeRatio(type.p, MF_MT_FRAME_RATE_RANGE_MIN, &min_n, &min_d)) && min_d) low = static_cast<double>(min_n) / min_d;
        if (SUCCEEDED(MFGetAttributeRatio(type.p, MF_MT_FRAME_RATE_RANGE_MAX, &max_n, &max_d)) && max_d) high = static_cast<double>(max_n) / max_d;
        const auto key = std::make_tuple(width, height, fourcc_of(subtype));
        auto& slot = by_key[key];
        if (slot.format.width == 0 || high > slot.format.max_rate) slot = {{width, height, std::min(low, high), std::max(low, high), fourcc_of(subtype)}, i};
    }
    std::vector<Native> out;
    for (auto& [key, native] : by_key) out.push_back(native.format.width ? native : Native{});
    std::erase_if(out, [](const Native& n) { return n.format.width == 0; });
    return out;
}

Error activation_error(HRESULT result) { return result == E_ACCESSDENIED ? Error::access_denied : Error::io; }

// One COM thread for listing (MTA).
template <class R> R on_com_thread(std::function<R()> work) {
    R result{Error::unsupported};
    std::thread([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        result = work();
        if (SUCCEEDED(com)) CoUninitialize();
    }).join();
    return result;
}

} // namespace

Access camera_access() {
    switch (audio::consent_store_access(L"webcam")) {
    case audio::Access::allowed: return Access::allowed;
    case audio::Access::denied: return Access::denied;
    default: return Access::unknown;
    }
}

Result<std::vector<Camera>> cameras() {
    return on_com_thread<Result<std::vector<Camera>>>([]() -> Result<std::vector<Camera>> {
        if (!started()) return Error::unsupported;
        std::vector<Camera> out;
        for (auto* activate : sources()) {
            Camera camera;
            camera.id = string_of(activate, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            camera.name = string_of(activate, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            Com<IMFMediaSource> source;
            if (camera_access() != Access::denied && SUCCEEDED(activate->ActivateObject(IID_PPV_ARGS(source.put())))) {
                Com<IMFSourceReader> reader;
                if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.p, nullptr, reader.put())))
                    for (const auto& n : native_types(reader.p)) camera.formats.push_back(n.format);
                source->Shutdown();
                activate->ShutdownObject();
            }
            activate->Release();
            out.push_back(std::move(camera));
        }
        return out;
    });
}

class Stream {
public:
    explicit Stream(Deliver deliver) : deliver_(std::move(deliver)) {}
    ~Stream() {
        request(close_command);
        if (thread_.joinable()) thread_.join();
    }

    Status open(std::string id, std::uint32_t width, std::uint32_t height, double rate) {
        std::unique_lock lock(mutex_);
        thread_ = std::thread([this, id = std::move(id), width, height, rate] { run(id, width, height, rate); });
        changed_.wait(lock, [this] { return opened_; });
        if (open_error_ != Error::none) {
            lock.unlock();
            thread_.join();
            return open_error_;
        }
        return {};
    }
    Status request(int command) {
        std::unique_lock lock(mutex_);
        if (!thread_.joinable() || exited_) return Error::bad_handle;
        pending_ = command;
        const auto ticket = ++requested_;
        changed_.notify_all();
        changed_.wait(lock, [&] { return served_ >= ticket || exited_; });
        return result_;
    }
    void dimensions(std::uint32_t& width, std::uint32_t& height) const noexcept { width = width_; height = height_; }

    enum { no_command, start_command, stop_command, close_command };

private:
    void run(const std::string& id, std::uint32_t width, std::uint32_t height, double rate) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto error = setup(id, width, height, rate);
        {
            std::lock_guard lock(mutex_);
            open_error_ = error;
            opened_ = true;
        }
        changed_.notify_all();
        if (error == Error::none) serve();
        if (reader_.p) {
            reader_.p->Release();
            reader_.p = nullptr;
        }
        if (source_) {
            source_->Shutdown();
            source_->Release();
            source_ = nullptr;
        }
        if (SUCCEEDED(com)) CoUninitialize();
        {
            std::lock_guard lock(mutex_);
            exited_ = true;
        }
        changed_.notify_all();
    }

    Error setup(const std::string& id, std::uint32_t width, std::uint32_t height, double rate) {
        if (!started()) return Error::unsupported;
        if (camera_access() == Access::denied) return Error::access_denied;
        IMFActivate* chosen{};
        for (auto* activate : sources()) {
            if (!chosen && (id.empty() || string_of(activate, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) == id)) chosen = activate;
            else activate->Release();
        }
        if (!chosen) return Error::not_found;
        const HRESULT activated = chosen->ActivateObject(IID_PPV_ARGS(&source_));
        chosen->Release();
        if (FAILED(activated)) return activation_error(activated);
        Com<IMFAttributes> attributes;
        if (FAILED(MFCreateAttributes(attributes.put(), 1)) || FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))) return Error::io;
        if (FAILED(MFCreateSourceReaderFromMediaSource(source_, attributes.p, reader_.put()))) return Error::io;
        // The native type closest to the size asked for (the first when none), the fastest within `rate`.
        const auto natives = native_types(reader_.p);
        if (natives.empty()) return Error::io;
        const Native* best = &natives.front();
        if (width && height) {
            const auto cost = [&](const Native& n) {
                const double area = std::fabs(static_cast<double>(n.format.width) * n.format.height - static_cast<double>(width) * height);
                const double over = rate > 0 && n.format.min_rate > rate ? 1e12 : 0;
                return area + over - (rate > 0 ? std::min(n.format.max_rate, rate) : n.format.max_rate);
            };
            best = &*std::min_element(natives.begin(), natives.end(), [&](const Native& a, const Native& b) { return cost(a) < cost(b); });
        }
        Com<IMFMediaType> native;
        if (FAILED(reader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), best->index, native.put()))) return Error::io;
        if (rate > 0 && rate < best->format.max_rate) {
            const auto frames = static_cast<UINT32>(std::lround(rate * 1000));
            MFSetAttributeRatio(native.p, MF_MT_FRAME_RATE, frames, 1000);
        }
        if (FAILED(reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native.p))) return Error::io;
        Com<IMFMediaType> output;
        if (FAILED(MFCreateMediaType(output.put()))) return Error::io;
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(output.p, MF_MT_FRAME_SIZE, best->format.width, best->format.height);
        if (FAILED(reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output.p))) return Error::io;
        width_ = best->format.width;
        height_ = best->format.height;
        return Error::none;
    }

    void serve() {
        bool running = false;
        std::vector<std::uint8_t> packed;
        for (;;) {
            int command = no_command;
            {
                std::unique_lock lock(mutex_);
                if (!running) changed_.wait(lock, [this] { return pending_ != no_command; });
                command = std::exchange(pending_, no_command);
            }
            if (command != no_command) {
                if (command == start_command) running = true;
                if (command == stop_command || command == close_command) running = false;
                {
                    std::lock_guard lock(mutex_);
                    result_ = {};
                    served_ = requested_;
                }
                changed_.notify_all();
                if (command == close_command) return;
                continue;
            }
            DWORD stream{}, flags{};
            LONGLONG timestamp{};
            Com<IMFSample> sample;
            const HRESULT read = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &timestamp, sample.put());
            if (FAILED(read) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                running = false;   // the device went away
                continue;
            }
            if (!sample) continue;
            Com<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) continue;
            BYTE* data{};
            DWORD length{};
            if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
            // NV12 rows and planes may be padded: the pitch comes from the 2-D buffer when it has one, and
            // the chroma plane is the buffer's last third (whatever height the luma plane was padded to).
            const std::size_t packed_size = static_cast<std::size_t>(width_) * height_ * 3 / 2;
            std::size_t pitch = width_;
            Com<IMF2DBuffer> planar;
            BYTE* scanline{};
            LONG signed_pitch{};
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(planar.put()))) && SUCCEEDED(planar->Lock2D(&scanline, &signed_pitch))) {
                if (signed_pitch > 0) pitch = static_cast<std::size_t>(signed_pitch);
                planar->Unlock2D();
            } else if (height_) {
                pitch = static_cast<std::size_t>(length) * 2 / (static_cast<std::size_t>(height_) * 3);
            }
            const std::size_t chroma = static_cast<std::size_t>(length) / 3 * 2;
            if (height_ >= 2 && pitch >= width_ && chroma + pitch * (height_ / 2 - 1) + width_ <= length && pitch * (height_ - 1) + width_ <= chroma) {
                const std::uint8_t* out = data;
                if (pitch != width_ || chroma != static_cast<std::size_t>(width_) * height_) {
                    packed.resize(packed_size);
                    auto* to = packed.data();
                    for (std::uint32_t row = 0; row < height_; ++row, to += width_) std::copy_n(data + row * pitch, width_, to);
                    for (std::uint32_t row = 0; row < height_ / 2; ++row, to += width_) std::copy_n(data + chroma + row * pitch, width_, to);
                    out = packed.data();
                }
                deliver_(Picture{width_, height_, time::monotonic_ns(), out, packed_size});
            }
            buffer->Unlock();
        }
    }

    Deliver deliver_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool opened_{}, exited_{};
    Error open_error_{Error::none};
    int pending_{no_command};
    std::uint64_t requested_{}, served_{};
    Status result_;
    IMFMediaSource* source_{};
    Com<IMFSourceReader> reader_;
    std::uint32_t width_{}, height_{};
};

Result<Stream*> open(std::string_view id, std::uint32_t width, std::uint32_t height, double rate, Deliver deliver) {
    if (!deliver) return Error::invalid_argument;
    auto stream = std::make_unique<Stream>(std::move(deliver));
    if (const auto opened = stream->open(std::string(id), width, height, rate); !opened) return opened.error();
    return stream.release();
}

void size(const Stream* stream, std::uint32_t& width, std::uint32_t& height) noexcept {
    if (stream) stream->dimensions(width, height);
    else width = height = 0;
}

Status start(Stream* stream) {
    if (!stream) return Error::bad_handle;
    return stream->request(Stream::start_command);
}

void stop(Stream* stream) noexcept {
    if (stream) static_cast<void>(stream->request(Stream::stop_command));
}

void close(Stream* stream) noexcept { delete stream; }

} // namespace alchemy::host::camera
