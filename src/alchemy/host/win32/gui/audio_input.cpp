// Win32 host backend, area "audio" (include/alchemy/host/audio.hpp): input streams over WASAPI
// capture, and the microphone's privacy setting. Each stream is an IAudioClient in shared mode with
// event-driven buffering, converted by the audio engine to 32-bit float in the asked rate and
// channels (the mixer's when 0). One thread per stream does everything COM, as the output streams
// do: it opens the client, serves start/stop/close, and while running hands each captured packet
// to the stream's capture function. A lost endpoint stops the stream.
// The privacy setting is Windows' capability consent store for the microphone (the machine-wide
// switch, the user's, and the one for desktop applications), read from the registry.
#include "alchemy/host/audio.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "advapi32.lib")

namespace alchemy::host::audio {
namespace {

constexpr GUID ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// "Allow" / "Deny" under `path` (HKLM or HKCU), or "" when absent.
std::wstring consent(HKEY root, const wchar_t* path) {
    wchar_t value[32]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(root, path, L"Value", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}

} // namespace

// Shared with the camera area (camera.cpp): the consent store's verdict for one capability.
Access consent_store_access(const wchar_t* capability) {
    const std::wstring base = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + capability;
    const auto machine = consent(HKEY_LOCAL_MACHINE, base.c_str());
    const auto user = consent(HKEY_CURRENT_USER, base.c_str());
    const auto desktop = consent(HKEY_CURRENT_USER, (base + L"\\NonPackaged").c_str());
    if (machine == L"Deny" || user == L"Deny" || desktop == L"Deny") return Access::denied;
    if (user == L"Allow" || desktop == L"Allow" || machine == L"Allow") return Access::allowed;
    return Access::unknown;
}

Access microphone_access() { return consent_store_access(L"microphone"); }

class InputStream {
public:
    explicit InputStream(Capture capture) : capture_(std::move(capture)) {}

    Status open(std::wstring uid, std::uint32_t sample_rate, std::uint32_t channels) {
        sample_rate_ = sample_rate;
        channels_ = channels;
        command_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!command_event_) return win32::last_error();
        std::unique_lock lock(mutex_);
        thread_ = std::thread([this, uid = std::move(uid)] { run(uid); });
        done_.wait(lock, [this] { return opened_; });
        if (open_error_ != Error::none) {
            lock.unlock();
            thread_.join();
            return open_error_;
        }
        return {};
    }
    ~InputStream() {
        request(Command::close);
        if (thread_.joinable()) thread_.join();
        if (command_event_) CloseHandle(command_event_);
    }
    [[nodiscard]] InputFormat format() const noexcept { return format_; }
    Status request(int command) {
        std::unique_lock lock(mutex_);
        if (!thread_.joinable() || exited_) return Error::bad_handle;
        pending_ = command;
        const auto ticket = ++requested_;
        SetEvent(command_event_);
        done_.wait(lock, [&] { return served_ >= ticket || exited_; });
        return result_;
    }

    enum Command { none, start, stop, close };

private:
    void run(const std::wstring& uid) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task_index{};
        const HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        const auto error = setup(uid);
        {
            std::lock_guard lock(mutex_);
            open_error_ = error;
            opened_ = true;
        }
        done_.notify_all();
        if (error == Error::none) serve();
        teardown();
        if (task) AvRevertMmThreadCharacteristics(task);
        if (SUCCEEDED(com)) CoUninitialize();
        {
            std::lock_guard lock(mutex_);
            exited_ = true;
        }
        done_.notify_all();
    }

    Error setup(const std::wstring& uid) {
        IMMDeviceEnumerator* enumerator{};
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator))))
            return Error::unsupported;
        const HRESULT found = uid.empty() ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device_)
                                          : enumerator->GetDevice(uid.c_str(), &device_);
        enumerator->Release();
        if (FAILED(found) || !device_) return Error::not_found;
        const HRESULT activated = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
        if (activated == E_ACCESSDENIED) return Error::access_denied;
        if (FAILED(activated)) return Error::io;
        WAVEFORMATEX* mix{};
        if (FAILED(client_->GetMixFormat(&mix)) || !mix) return Error::io;
        WAVEFORMATEXTENSIBLE wanted{};
        wanted.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wanted.Format.nChannels = static_cast<WORD>(channels_ ? channels_ : mix->nChannels);
        wanted.Format.nSamplesPerSec = sample_rate_ ? sample_rate_ : mix->nSamplesPerSec;
        wanted.Format.wBitsPerSample = 32;
        wanted.Format.nBlockAlign = static_cast<WORD>(wanted.Format.nChannels * 4);
        wanted.Format.nAvgBytesPerSec = wanted.Format.nSamplesPerSec * wanted.Format.nBlockAlign;
        wanted.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wanted.Samples.wValidBitsPerSample = 32;
        wanted.SubFormat = ieee_float;
        const bool mixer_channels = wanted.Format.nChannels == mix->nChannels;
        wanted.dwChannelMask = mixer_channels && mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask
            : wanted.Format.nChannels == 1 ? SPEAKER_FRONT_CENTER
            : wanted.Format.nChannels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : 0;
        CoTaskMemFree(mix);
        constexpr REFERENCE_TIME buffer_duration = 200000;   // 20 ms
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const HRESULT initialized = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, &wanted.Format, nullptr);
        if (initialized == E_ACCESSDENIED) return Error::access_denied;
        if (FAILED(initialized)) return Error::io;
        audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!audio_event_ || FAILED(client_->SetEventHandle(audio_event_))) return Error::io;
        if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_client_)))) return Error::io;
        format_.sample_rate = wanted.Format.nSamplesPerSec;
        format_.channels = wanted.Format.nChannels;
        format_.channel_mask = wanted.dwChannelMask;
        return Error::none;
    }

    void serve() {
        bool running = false;
        std::vector<float> silence;
        for (;;) {
            const HANDLE handles[] = {command_event_, audio_event_};
            const DWORD woken = WaitForMultipleObjects(running ? 2 : 1, handles, FALSE, running ? 200 : INFINITE);
            if (woken == WAIT_OBJECT_0) {
                int command{};
                {
                    std::lock_guard lock(mutex_);
                    command = std::exchange(pending_, none);
                }
                Status result;
                if (command == start && !running) {
                    if (FAILED(client_->Start())) result = Error::io;
                    else running = true;
                } else if ((command == stop || command == close) && running) {
                    client_->Stop();
                    client_->Reset();
                    running = false;
                }
                {
                    std::lock_guard lock(mutex_);
                    result_ = result;
                    served_ = requested_;
                }
                done_.notify_all();
                if (command == close) return;
                continue;
            }
            if (!running || woken != WAIT_OBJECT_0 + 1) continue;
            HRESULT state = S_OK;
            for (;;) {
                UINT32 packet{};
                state = capture_client_->GetNextPacketSize(&packet);
                if (FAILED(state) || packet == 0) break;
                BYTE* data{};
                UINT32 frames{};
                DWORD flags{};
                state = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(state)) break;
                const auto now = time::monotonic_ns();
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(static_cast<std::size_t>(frames) * format_.channels, 0.0f);
                    capture_(silence.data(), frames, now);
                } else {
                    capture_(reinterpret_cast<const float*>(data), frames, now);
                }
                state = capture_client_->ReleaseBuffer(frames);
                if (FAILED(state)) break;
            }
            if (state == AUDCLNT_E_DEVICE_INVALIDATED) {
                std::cerr << "alchemy: audio input endpoint lost; the stream stops" << std::endl;
                running = false;
            }
        }
    }

    void teardown() {
        if (capture_client_) capture_client_->Release();
        if (client_) client_->Release();
        if (device_) device_->Release();
        if (audio_event_) CloseHandle(audio_event_);
        capture_client_ = nullptr;
        client_ = nullptr;
        device_ = nullptr;
        audio_event_ = nullptr;
    }

    Capture capture_;
    std::uint32_t sample_rate_{}, channels_{};
    InputFormat format_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable done_;
    bool opened_{}, exited_{};
    Error open_error_{Error::none};
    int pending_{none};
    std::uint64_t requested_{}, served_{};
    Status result_;
    HANDLE command_event_{}, audio_event_{};
    IMMDevice* device_{};
    IAudioClient* client_{};
    IAudioCaptureClient* capture_client_{};
};

Result<InputStream*> open_input(std::string_view uid, std::uint32_t sample_rate, std::uint32_t channels, Capture capture) {
    if (!capture) return Error::invalid_argument;
    if (microphone_access() == Access::denied) return Error::access_denied;
    auto stream = std::make_unique<InputStream>(std::move(capture));
    if (const auto opened = stream->open(win32::widen(uid), sample_rate, channels); !opened) return opened.error();
    return stream.release();
}

InputFormat input_format(const InputStream* stream) noexcept { return stream ? stream->format() : InputFormat{}; }

Status start_input(InputStream* stream) {
    if (!stream) return Error::bad_handle;
    return stream->request(InputStream::start);
}

void stop_input(InputStream* stream) noexcept {
    if (stream) static_cast<void>(stream->request(InputStream::stop));
}

void close_input(InputStream* stream) noexcept { delete stream; }

} // namespace alchemy::host::audio
