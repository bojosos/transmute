#include "alchemy/darwin/media/hal_host.hpp"

#include "alchemy/host/audio.hpp"
#include "alchemy/host/process.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// The CoreAudio HAL's devices from the host's audio endpoints (host::audio:
// the MMDevice API under the win32 backend). An endpoint without a mixer
// format keeps the device defaults (stereo, 48 kHz); one without a name is
// "Audio Input" / "Audio Output". A host that cannot list endpoints gives no
// devices.
//
// Output streams are host::audio streams in the device's rate and channels.
// Silent runs: with ALCHEMY_AUDIO_OUTPUT=null, or in a cloaked run
// (ALCHEMY_GUI_CLOAK) unless ALCHEMY_AUDIO_OUTPUT=device, a stream plays
// nowhere: a thread of its own takes the frames at the device's rate, so
// programs run as they would with a device, and the user hears nothing.
namespace alchemy::gui::completion_kit::media::hal {
namespace {

bool silent_output() {
    static const bool silent = [] {
        const auto output = host::process::environment("ALCHEMY_AUDIO_OUTPUT").value_or("");
        if (output == "null") return true;
        if (output == "device") return false;
        const auto cloak = host::process::environment("ALCHEMY_GUI_CLOAK").value_or("");
        return !cloak.empty() && cloak != "0";
    }();
    return silent;
}

class HostStream final : public OutputStream {
public:
    explicit HostStream(host::audio::OutputStream* stream) : stream_(stream) {}
    ~HostStream() override { host::audio::close_output(stream_); }
    bool start() override { return static_cast<bool>(host::audio::start_output(stream_)); }
    void stop() override { host::audio::stop_output(stream_); }

private:
    host::audio::OutputStream* stream_;
};

// Takes 10 ms of frames every 10 ms, holding one period ahead (as a host stream does).
class SilentStream final : public OutputStream {
public:
    SilentStream(const Device& device, Render render) : render_(std::move(render)), device_(device) {}
    ~SilentStream() override { stop(); }
    bool start() override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return true;
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        const auto rate = device_.sample_rate > 0 ? device_.sample_rate : 48000.0;
        const auto period = static_cast<std::uint32_t>(rate / 100);
        std::vector<float> scratch(static_cast<std::size_t>(period) * std::max(device_.channels, 1U));
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            render_(scratch.data(), period, period);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    }

    Render render_;
    Device device_;
    std::mutex mutex_;
    std::atomic<bool> running_{};
    std::thread thread_;
};

class HostEndpoints final : public Backend {
public:
    Snapshot snapshot() override {
        Snapshot snapshot;
        const auto listed = host::audio::endpoints();
        if (!listed) return snapshot;
        for (const auto& endpoint : listed->endpoints) {
            Device device;
            device.uid = endpoint.uid;
            device.name = endpoint.name.empty() ? (endpoint.input ? "Audio Input" : "Audio Output") : endpoint.name;
            device.input = endpoint.input;
            device.form_factor = endpoint.form_factor;
            if (endpoint.has_format) {
                device.channels = endpoint.channels;
                device.sample_rate = endpoint.sample_rate;
                device.channel_mask = endpoint.channel_mask;
            }
            snapshot.devices.push_back(std::move(device));
        }
        snapshot.default_output = listed->default_output;
        snapshot.default_input = listed->default_input;
        return snapshot;
    }

    std::unique_ptr<OutputStream> open_output(const Device& device, Render render) override {
        if (device.input) return nullptr;
        if (silent_output()) return std::make_unique<SilentStream>(device, std::move(render));
        auto opened = host::audio::open_output(device.uid, static_cast<std::uint32_t>(device.sample_rate), device.channels, std::move(render));
        if (!opened) {
            std::cerr << "alchemy: audio output on \"" << device.name << "\" did not open (" << host::name(opened.error()) << ")" << std::endl;
            return nullptr;
        }
        return std::make_unique<HostStream>(*opened);
    }
};

} // namespace

std::unique_ptr<Backend> make_host_backend() { return std::make_unique<HostEndpoints>(); }

} // namespace alchemy::gui::completion_kit::media::hal
