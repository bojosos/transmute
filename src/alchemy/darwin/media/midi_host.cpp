#include "alchemy/darwin/media/midi_host.hpp"

#include "alchemy/host/midi.hpp"

// The CoreMIDI core's devices from the host (host::midi: Windows MIDI under
// the win32 backend). A host that lists no ports, or cannot, gives no
// devices.
namespace alchemy::gui::completion_kit::media::midi {
namespace {

std::vector<PortInfo> ports(const host::Result<std::vector<host::midi::Port>>& listed) {
    std::vector<PortInfo> result;
    if (!listed) return result;
    for (const auto& port : *listed) result.push_back({port.name, port.manufacturer, port.driver, port.synthesizer});
    return result;
}

class HostDevices final : public Backend {
public:
    std::vector<PortInfo> inputs() override { return ports(devices_.inputs()); }
    std::vector<PortInfo> outputs() override { return ports(devices_.outputs()); }
    bool open_input(std::size_t index, Input deliver) override { return static_cast<bool>(devices_.open_input(index, std::move(deliver))); }
    void close_input(std::size_t index) override { devices_.close_input(index); }
    bool send(std::size_t index, std::span<const std::uint8_t> message) override { return static_cast<bool>(devices_.send(index, message)); }

private:
    host::midi::Devices devices_;
};

} // namespace

std::unique_ptr<Backend> make_host_backend() { return std::make_unique<HostDevices>(); }

} // namespace alchemy::gui::completion_kit::media::midi
