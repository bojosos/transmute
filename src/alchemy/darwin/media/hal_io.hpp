#pragma once

// CoreAudio HAL device I/O (media group-internal). One DeviceIo per output device plays its
// running clients through the device's output stream (hal_core.hpp Backend::open_output):
// - the clients render on the device's I/O thread, a guest thread of its own
//   ("com.apple.audio.IOThread.client"), one buffer of the device's I/O buffer frame size at a
//   time, each into silence, and their sum goes into a FIFO;
// - the host stream takes frames from the FIFO (silence when it runs dry) and the I/O thread
//   keeps two buffers ahead of it;
// - sample times count the device's frames from the first start; host times are
//   mach_absolute_time's (host_time_now), the output time being when the host plays the buffer.
// The first running client opens and starts the stream, the last one stops it. Once stop(key)
// returns, the client no longer runs (stop waits for a render in progress), except when it is
// called from inside a render: a client may stop itself (or others) there, and the I/O thread
// then ends after the render.

#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/media_support.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace alchemy::gui {
class CompletionThread;
}

namespace alchemy::gui::completion_kit::media::hal_io {

struct Timing {
    double now_sample{};               // the device's sample time now (frames the host has taken)
    std::uint64_t now_host{};
    double output_sample{};            // the sample time of the buffer's first frame
    std::uint64_t output_host{};       // when that frame plays
    double rate{};
};

// Adds `frames` interleaved frames in the device's channels into `mix` (the client's own output
// is mixed by the caller: `mix` is the client's buffer, zeroed). Runs on the I/O thread.
using Client = std::function<void(float* mix, std::uint32_t frames, const Timing& timing)>;

class DeviceIo : public std::enable_shared_from_this<DeviceIo> {
public:
    DeviceIo(Support& support, hal::Backend& backend, std::uint32_t id, hal::Device device);
    ~DeviceIo();

    [[nodiscard]] std::uint32_t channels() const { return channels_; }
    [[nodiscard]] double rate() const { return rate_; }
    // Starts `client` under `key` (already running: done). On a guest thread inside a call.
    // kAudioHardwareUnspecifiedError when the device's stream does not open or start.
    std::int32_t start(std::uint64_t key, Client client, std::uint32_t buffer_frames);
    // Stops the client `key`; the last one stops the device.
    void stop(std::uint64_t key);
    [[nodiscard]] bool running(std::uint64_t key) const;
    [[nodiscard]] bool running() const;
    // The device's time now: false when it is not running.
    bool now(Timing& out) const;

    // Tests: the host side of the stream (what Render does).
    void take(float* samples, std::uint32_t frames, std::uint32_t queued);

private:
    void loop(std::uint64_t generation);
    // Runs or stops the device as its clients want (running while it has any). One thread
    // changes the device at a time; the others wait, except the I/O thread, whose changes the
    // changing thread picks up before it finishes.
    void settle(std::unique_lock<std::mutex>& lock);
    bool activate(std::unique_lock<std::mutex>& lock);   // false: the stream did not start
    void deactivate(std::unique_lock<std::mutex>& lock);
    [[nodiscard]] bool on_io_thread() const { return std::this_thread::get_id() == io_thread_; }

    Support& support_;
    hal::Backend& backend_;
    std::uint32_t id_;
    hal::Device device_;
    std::uint32_t channels_{};
    double rate_{};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::uint64_t, Client> clients_;
    std::unique_ptr<hal::OutputStream> stream_;
    std::shared_ptr<CompletionThread> thread_;
    std::thread::id io_thread_;       // the completion thread the loops run on (once known)
    bool active_{};
    bool changing_{};                  // a thread is starting or stopping the device
    bool rendering_{};                 // the I/O thread is running clients (unlocked)
    bool start_failed_{};
    std::uint64_t generation_{};       // a stop or a restart ends the loops of earlier generations
    unsigned loops_{};
    std::uint32_t buffer_frames_{512};
    std::deque<float> fifo_;
    std::uint64_t produced_{}, consumed_{};
    std::uint32_t host_queued_{};
};

} // namespace alchemy::gui::completion_kit::media::hal_io
