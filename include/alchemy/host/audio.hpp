#pragma once

// Host area "audio" (owner: r3). The rules are in include/alchemy/host/host.hpp. Audio devices:
// enumeration and streams. The CoreAudio HAL (src/alchemy/darwin/media/hal.cpp, over
// hal_core.hpp) runs on this. GUI-only: the backends are src/alchemy/host/<backend>/gui/audio.cpp,
// compiled into alchemy-fx.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alchemy::host::audio {

// One active audio endpoint with its shared-mode (mixer) format.
struct Endpoint {
    std::string uid;                   // the host's stable endpoint ID
    std::string name;                  // its friendly name ("" when the host has none)
    bool input{};                      // capture (else render)
    std::uint32_t form_factor{};       // EndpointFormFactor (1 speakers, 3 headphones, 4 microphone, ...; 0 when not given)
    // The mixer format, when the host gives one (else the three are 0).
    bool has_format{};
    std::uint32_t channels{}, sample_rate{};
    std::uint32_t channel_mask{};      // speaker positions (WAVEFORMATEXTENSIBLE dwChannelMask); 0 unknown
};

struct Endpoints {
    std::vector<Endpoint> endpoints;   // in the host's order
    std::string default_output, default_input;   // the console defaults' uids ("" for none)
};

// The active endpoints. Only their properties are read: no stream is opened.
// Never touches the calling thread's COM (or other per-thread) state.
[[nodiscard]] Result<Endpoints> endpoints();

} // namespace alchemy::host::audio
