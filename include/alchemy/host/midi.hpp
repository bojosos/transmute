#pragma once

// Host area "midi" (owner: r3). The rules are in include/alchemy/host/host.hpp. MIDI devices:
// enumeration, input and output. CoreMIDI (src/alchemy/darwin/media/coremidi.cpp, over
// midi_core.hpp) runs on this. GUI-only: the backends are src/alchemy/host/<backend>/gui/midi.cpp,
// compiled into alchemy-fx.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace alchemy::host::midi {

// One input or output port the host lists, in the host's order (its index is the port's
// number in the calls below).
struct Port {
    std::string name, manufacturer;
    std::string driver;                // the host's MIDI API ("com.microsoft.winmm")
    bool synthesizer{};                // a software or hardware synth, not a MIDI port
};

// Gets one complete message from an open input (a channel or system message, or a whole
// F0..F7 SysEx) with its host time (time::monotonic_ns), on a host thread.
using Receiver = std::function<void(std::vector<std::uint8_t> message, std::uint64_t time_ns)>;

// The host's MIDI devices for one user (CoreMIDI's server of the process). Outputs open on
// their first send, so a user that never sends opens nothing. Destroying it closes every
// port; no Receiver runs after that.
class Devices {
public:
    Devices();
    ~Devices();
    Devices(const Devices&) = delete;
    Devices& operator=(const Devices&) = delete;

    [[nodiscard]] Result<std::vector<Port>> inputs();
    [[nodiscard]] Result<std::vector<Port>> outputs();
    // Starts input port `index`; its messages go to `receive` until close_input. Opening an
    // open port succeeds and keeps its first Receiver.
    [[nodiscard]] Status open_input(std::size_t index, Receiver receive);
    void close_input(std::size_t index) noexcept;
    // Sends one complete message to output port `index`. A SysEx returns once the device has
    // taken it (at the port's speed; Error::timed_out after 10 seconds).
    [[nodiscard]] Status send(std::size_t index, std::span<const std::uint8_t> message);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace alchemy::host::midi
