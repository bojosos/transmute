// Win32 host backend, area "audio" (include/alchemy/host/audio.hpp): output
// streams over WASAPI. Each stream is an IAudioClient in shared mode with
// event-driven buffering, in the endpoint's mixer rate and channel count as
// 32-bit float. One thread per stream does everything COM: it runs in the
// multithreaded apartment under MMCSS ("Pro Audio"), opens the client, then
// serves start/stop/close requests and, while running, fills each buffer
// period through the stream's render function. A lost endpoint (unplugged,
// disabled) stops the stream: render is no longer called.
#include "alchemy/host/audio.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace alchemy::host::audio {
namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.
constexpr GUID ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

} // namespace

class OutputStream {
public:
    explicit OutputStream(Render render) : render_(std::move(render)) {}

    // Opens the client on the stream's thread; the result is that of the opening.
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
    ~OutputStream() {
        request(Command::close);
        if (thread_.joinable()) thread_.join();
        if (command_event_) CloseHandle(command_event_);
    }
    [[nodiscard]] OutputFormat format() const noexcept { return format_; }
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
        const HRESULT found = uid.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_)
                                          : enumerator->GetDevice(uid.c_str(), &device_);
        enumerator->Release();
        if (FAILED(found) || !device_) return Error::not_found;
        if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_)))) return Error::io;
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
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, &wanted.Format, nullptr))) return Error::io;
        audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!audio_event_ || FAILED(client_->SetEventHandle(audio_event_))) return Error::io;
        UINT32 buffer_frames{};
        REFERENCE_TIME period{}, minimum{};
        if (FAILED(client_->GetBufferSize(&buffer_frames)) || FAILED(client_->GetDevicePeriod(&period, &minimum))) return Error::io;
        if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client_)))) return Error::io;
        format_.sample_rate = wanted.Format.nSamplesPerSec;
        format_.channels = wanted.Format.nChannels;
        format_.channel_mask = wanted.dwChannelMask;
        format_.buffer_frames = buffer_frames;
        format_.period_frames = static_cast<std::uint32_t>(period * format_.sample_rate / 10000000);
        return Error::none;
    }

    void serve() {
        bool running = false;
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
                    BYTE* data{};
                    if (SUCCEEDED(render_client_->GetBuffer(format_.buffer_frames, &data)))
                        render_client_->ReleaseBuffer(format_.buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
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
            UINT32 padding{};
            HRESULT state = client_->GetCurrentPadding(&padding);
            const UINT32 available = SUCCEEDED(state) ? format_.buffer_frames - padding : 0;
            if (SUCCEEDED(state) && available) {
                BYTE* data{};
                state = render_client_->GetBuffer(available, &data);
                if (SUCCEEDED(state)) {
                    render_(reinterpret_cast<float*>(data), available, padding);
                    state = render_client_->ReleaseBuffer(available, 0);
                }
            }
            if (state == AUDCLNT_E_DEVICE_INVALIDATED) {
                std::cerr << "alchemy: audio output endpoint lost; the stream stops" << std::endl;
                running = false;
            }
        }
    }

    void teardown() {
        if (render_client_) render_client_->Release();
        if (client_) client_->Release();
        if (device_) device_->Release();
        if (audio_event_) CloseHandle(audio_event_);
        render_client_ = nullptr;
        client_ = nullptr;
        device_ = nullptr;
        audio_event_ = nullptr;
    }

    Render render_;
    std::uint32_t sample_rate_{}, channels_{};
    OutputFormat format_;
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
    IAudioRenderClient* render_client_{};
};

Result<OutputStream*> open_output(std::string_view uid, std::uint32_t sample_rate, std::uint32_t channels, Render render) {
    if (!render) return Error::invalid_argument;
    auto stream = std::make_unique<OutputStream>(std::move(render));
    if (const auto opened = stream->open(win32::widen(uid), sample_rate, channels); !opened) return opened.error();
    return stream.release();
}

OutputFormat output_format(const OutputStream* stream) noexcept { return stream ? stream->format() : OutputFormat{}; }

Status start_output(OutputStream* stream) {
    if (!stream) return Error::bad_handle;
    return stream->request(OutputStream::start);
}

void stop_output(OutputStream* stream) noexcept {
    if (stream) static_cast<void>(stream->request(OutputStream::stop));
}

void close_output(OutputStream* stream) noexcept { delete stream; }

} // namespace alchemy::host::audio
