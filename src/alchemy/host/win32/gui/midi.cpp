// Win32 host backend, area "midi" (include/alchemy/host/midi.hpp): the MIDI
// input and output devices Windows MIDI (winmm) lists. Outputs open on the
// first send. Inputs deliver through a host thread that owns the winmm
// callback window messages (CALLBACK_THREAD), so SysEx buffers are re-queued
// outside the driver callback.
#include "alchemy/host/midi.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <mmeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#pragma comment(lib, "winmm.lib")

namespace alchemy::host::midi {
namespace {

std::string manufacturer(WORD mid) { return mid == 1 ? "Microsoft" : std::string(); }   // 1 = MM_MICROSOFT (mmreg.h)

// The bytes of a short message with this status (MM_MIM_DATA packs up to three).
std::size_t short_length(std::uint8_t status) {
    if (status < 0x80) return 1;
    if (status < 0xc0) return 3;            // note off/on, poly pressure, control change
    if (status < 0xe0) return 2;            // program change, channel pressure
    if (status < 0xf0) return 3;            // pitch bend
    switch (status) {
    case 0xf1: case 0xf3: return 2;         // time code quarter frame, song select
    case 0xf2: return 3;                    // song position pointer
    default: return 1;                      // tune request, EOX, undefined, realtime
    }
}

Error mm_error(MMRESULT result) {
    switch (result) {
    case MMSYSERR_BADDEVICEID: return Error::not_found;
    case MMSYSERR_ALLOCATED: return Error::busy;
    case MMSYSERR_NOMEM: return Error::no_memory;
    case MMSYSERR_INVALPARAM: case MMSYSERR_INVALFLAG: return Error::invalid_argument;
    case MMSYSERR_NODRIVER: return Error::unsupported;
    default: return Error::io;
    }
}

constexpr std::size_t sysex_buffers = 4, sysex_buffer_size = 4096;
constexpr UINT stop_message = WM_APP + 1;

} // namespace

struct Devices::State {
    struct Input {
        HMIDIIN handle{};
        Receiver receive;
        std::vector<std::unique_ptr<MIDIHDR>> headers;
        std::vector<std::vector<char>> storage;
    };

    bool start_thread() {
        std::unique_lock lock(thread_mutex);
        if (thread.joinable()) return thread_id != 0;
        std::atomic<bool> ready{false};
        thread = std::thread([this, &ready] {
            MSG message{};
            PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // creates the queue
            thread_id = GetCurrentThreadId();
            ready = true;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (message.message == stop_message) break;
                handle(message);
            }
        });
        while (!ready) std::this_thread::yield();
        return true;
    }

    void handle(const MSG& message) {
        if (message.message != MM_MIM_DATA && message.message != MM_MIM_LONGDATA) return;
        const auto handle = reinterpret_cast<HMIDIIN>(message.wParam);
        Receiver receive;
        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard lock(mutex);
            Input* input = nullptr;
            for (auto& [index, state] : inputs) if (state.handle == handle) input = &state;
            if (!input) return;   // closed meanwhile (the header may be gone: never touched)
            receive = input->receive;
            if (message.message == MM_MIM_DATA) {
                const auto packed = static_cast<DWORD>(message.lParam);
                const auto length = short_length(static_cast<std::uint8_t>(packed & 0xff));
                for (std::size_t i = 0; i < length; ++i) bytes.push_back(static_cast<std::uint8_t>((packed >> (8 * i)) & 0xff));
            } else {
                auto* header = reinterpret_cast<MIDIHDR*>(message.lParam);
                bytes.assign(header->lpData, header->lpData + header->dwBytesRecorded);
                if (header->dwBytesRecorded) midiInAddBuffer(handle, header, sizeof(MIDIHDR));   // 0 bytes: returned by reset
            }
        }
        if (!bytes.empty() && receive) receive(std::move(bytes), time::monotonic_ns());
    }

    std::mutex mutex;
    std::map<std::size_t, HMIDIOUT> outputs;
    std::map<std::size_t, Input> inputs;
    std::mutex thread_mutex;
    std::thread thread;
    std::atomic<DWORD> thread_id{0};
};

Devices::Devices() : state_(std::make_unique<State>()) {}

Devices::~Devices() {
    std::vector<std::size_t> open;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& [index, input] : state_->inputs) open.push_back(index);
    }
    for (const auto index : open) close_input(index);
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& [index, handle] : state_->outputs) { midiOutReset(handle); midiOutClose(handle); }
        state_->outputs.clear();
    }
    if (state_->thread.joinable()) {
        PostThreadMessageW(state_->thread_id, stop_message, 0, 0);
        state_->thread.join();
    }
}

Result<std::vector<Port>> Devices::inputs() {
    std::vector<Port> ports;
    const auto count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) caps.szPname[0] = 0;
        ports.push_back({win32::narrow(caps.szPname), manufacturer(caps.wMid), "com.microsoft.winmm", false});
    }
    return ports;
}

Result<std::vector<Port>> Devices::outputs() {
    std::vector<Port> ports;
    const auto count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) caps.szPname[0] = 0;
        const bool synth = caps.wTechnology != MOD_MIDIPORT;
        ports.push_back({win32::narrow(caps.szPname), manufacturer(caps.wMid), "com.microsoft.winmm", synth});
    }
    return ports;
}

Status Devices::open_input(std::size_t index, Receiver receive) {
    if (!state_->start_thread()) return Error::other;
    std::lock_guard lock(state_->mutex);
    if (state_->inputs.contains(index)) return {};
    HMIDIIN handle{};
    if (const auto opened = midiInOpen(&handle, static_cast<UINT>(index), state_->thread_id, 0, CALLBACK_THREAD); opened != MMSYSERR_NOERROR)
        return mm_error(opened);
    auto& input = state_->inputs[index];
    input.handle = handle;
    input.receive = std::move(receive);
    for (std::size_t i = 0; i < sysex_buffers; ++i) {
        auto header = std::make_unique<MIDIHDR>();
        input.storage.emplace_back(sysex_buffer_size);
        header->lpData = input.storage.back().data();
        header->dwBufferLength = static_cast<DWORD>(sysex_buffer_size);
        if (midiInPrepareHeader(handle, header.get(), sizeof(MIDIHDR)) == MMSYSERR_NOERROR) {
            midiInAddBuffer(handle, header.get(), sizeof(MIDIHDR));
            input.headers.push_back(std::move(header));
        }
    }
    if (const auto started = midiInStart(handle); started != MMSYSERR_NOERROR) {
        for (auto& header : input.headers) midiInUnprepareHeader(handle, header.get(), sizeof(MIDIHDR));
        midiInClose(handle);
        state_->inputs.erase(index);
        return mm_error(started);
    }
    return {};
}

void Devices::close_input(std::size_t index) noexcept {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->inputs.find(index);
    if (found == state_->inputs.end()) return;
    auto& input = found->second;
    midiInStop(input.handle);
    midiInReset(input.handle);   // returns the SysEx buffers (marked done)
    for (auto& header : input.headers) midiInUnprepareHeader(input.handle, header.get(), sizeof(MIDIHDR));
    midiInClose(input.handle);
    state_->inputs.erase(found);
}

Status Devices::send(std::size_t index, std::span<const std::uint8_t> message) {
    if (message.empty()) return {};
    std::lock_guard lock(state_->mutex);
    auto found = state_->outputs.find(index);
    if (found == state_->outputs.end()) {
        HMIDIOUT handle{};
        if (const auto opened = midiOutOpen(&handle, static_cast<UINT>(index), 0, 0, CALLBACK_NULL); opened != MMSYSERR_NOERROR)
            return mm_error(opened);
        found = state_->outputs.emplace(index, handle).first;
    }
    const auto handle = found->second;
    if (message[0] != 0xf0) {
        DWORD packed = 0;
        for (std::size_t i = 0; i < message.size() && i < 3; ++i) packed |= static_cast<DWORD>(message[i]) << (8 * i);
        const auto sent = midiOutShortMsg(handle, packed);
        return sent == MMSYSERR_NOERROR ? Status{} : Status{mm_error(sent)};
    }
    std::vector<char> bytes(message.begin(), message.end());
    MIDIHDR header{};
    header.lpData = bytes.data();
    header.dwBufferLength = static_cast<DWORD>(bytes.size());
    header.dwBytesRecorded = static_cast<DWORD>(bytes.size());
    if (const auto prepared = midiOutPrepareHeader(handle, &header, sizeof header); prepared != MMSYSERR_NOERROR) return mm_error(prepared);
    Status result;
    if (const auto sent = midiOutLongMsg(handle, &header, sizeof header); sent != MMSYSERR_NOERROR) result = mm_error(sent);
    // The driver owns the buffer until it marks it done (at the port's SysEx speed).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (result && !(header.dwFlags & MHDR_DONE)) {
        if (std::chrono::steady_clock::now() > deadline) { midiOutReset(handle); result = Error::timed_out; break; }
        Sleep(1);
    }
    while (midiOutUnprepareHeader(handle, &header, sizeof header) == MIDIERR_STILLPLAYING) Sleep(1);
    return result;
}

} // namespace alchemy::host::midi
