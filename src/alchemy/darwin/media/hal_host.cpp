#include "alchemy/darwin/media/hal_host.hpp"

#include "alchemy/host/audio.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
//
// Input streams are host::audio input streams in the device's rate and
// channels; the host's privacy settings may refuse them. Silent input: with
// ALCHEMY_AUDIO_INPUT=null, or in a cloaked run unless
// ALCHEMY_AUDIO_INPUT=device, a stream captures silence at the device's rate
// and no microphone is opened. Test tone: with ALCHEMY_AUDIO_INPUT=tone the
// host's input endpoints are replaced by one input device, "Alchemy Test
// Tone" (mono, 48 kHz, the default input), which captures a 440 Hz sine at
// a quarter of full scale; no microphone is opened.
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

enum class InputMode { device, silent, tone };

InputMode input_mode() {
    static const InputMode mode = [] {
        const auto input = host::process::environment("ALCHEMY_AUDIO_INPUT").value_or("");
        if (input == "null") return InputMode::silent;
        if (input == "tone") return InputMode::tone;
        if (input == "device") return InputMode::device;
        const auto cloak = host::process::environment("ALCHEMY_GUI_CLOAK").value_or("");
        return !cloak.empty() && cloak != "0" ? InputMode::silent : InputMode::device;
    }();
    return mode;
}

constexpr const char* tone_uid = "alchemy-test-tone";

class HostInput final : public InputStream {
public:
    explicit HostInput(host::audio::InputStream* stream) : stream_(stream) {}
    ~HostInput() override { host::audio::close_input(stream_); }
    bool start() override { return static_cast<bool>(host::audio::start_input(stream_)); }
    void stop() override { host::audio::stop_input(stream_); }

private:
    host::audio::InputStream* stream_;
};

// Hands over 10 ms of silence (or of a 440 Hz sine at 0.25) every 10 ms.
class SilentInput final : public InputStream {
public:
    SilentInput(const Device& device, Capture capture, bool tone = false) : capture_(std::move(capture)), device_(device), tone_(tone) {}
    ~SilentInput() override { stop(); }
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
        const auto channels = std::max(device_.channels, 1U);
        std::vector<float> samples(static_cast<std::size_t>(period) * channels);
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
            if (tone_)
                for (std::uint32_t f = 0; f < period; ++f, ++frame_) {
                    const auto value = static_cast<float>(0.25 * std::sin(2 * 3.14159265358979323846 * 440.0 * static_cast<double>(frame_) / rate));
                    for (std::uint32_t c = 0; c < channels; ++c) samples[static_cast<std::size_t>(f) * channels + c] = value;
                }
            if (running_) capture_(samples.data(), period, host::time::monotonic_ns());
        }
    }

    Capture capture_;
    Device device_;
    bool tone_{};
    std::uint64_t frame_{};
    std::mutex mutex_;
    std::atomic<bool> running_{};
    std::thread thread_;
};

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
        if (listed) {
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
        }
        if (input_mode() == InputMode::tone) {
            std::erase_if(snapshot.devices, [](const Device& d) { return d.input; });
            Device tone;
            tone.uid = tone_uid;
            tone.name = "Alchemy Test Tone";
            tone.manufacturer = "Alchemy";
            tone.input = true;
            tone.channels = 1;
            tone.sample_rate = 48000;
            tone.channel_mask = 4;   // SPEAKER_FRONT_CENTER
            tone.form_factor = 4;    // Microphone
            snapshot.devices.push_back(std::move(tone));
            snapshot.default_input = tone_uid;
        }
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

    std::unique_ptr<InputStream> open_input(const Device& device, Capture capture) override {
        if (!device.input) return nullptr;
        if (input_mode() == InputMode::tone) return std::make_unique<SilentInput>(device, std::move(capture), true);
        if (input_mode() == InputMode::silent) return std::make_unique<SilentInput>(device, std::move(capture));
        auto opened = host::audio::open_input(device.uid, static_cast<std::uint32_t>(device.sample_rate), device.channels, std::move(capture));
        if (!opened) {
            std::cerr << "alchemy: audio input on \"" << device.name << "\" did not open (" << host::name(opened.error()) << ")" << std::endl;
            return nullptr;
        }
        return std::make_unique<HostInput>(*opened);
    }
};

} // namespace

std::unique_ptr<Backend> make_host_backend() { return std::make_unique<HostEndpoints>(); }

} // namespace alchemy::gui::completion_kit::media::hal
