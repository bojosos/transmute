#include "alchemy/darwin/media/hal_host.hpp"

#include "alchemy/host/audio.hpp"

// The CoreAudio HAL's devices from the host's audio endpoints (host::audio:
// the MMDevice API under the win32 backend). An endpoint without a mixer
// format keeps the device defaults (stereo, 48 kHz); one without a name is
// "Audio Input" / "Audio Output". A host that cannot list endpoints gives no
// devices.
namespace alchemy::gui::completion_kit::media::hal {
namespace {

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
};

} // namespace

std::unique_ptr<Backend> make_host_backend() { return std::make_unique<HostEndpoints>(); }

} // namespace alchemy::gui::completion_kit::media::hal
