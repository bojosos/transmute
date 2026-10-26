#pragma once

// Host area "audio" (owner: r3; output streams: fw-media). The rules are in
// include/alchemy/host/host.hpp. Audio devices: enumeration and output streams. The CoreAudio HAL (src/alchemy/darwin/media/hal.cpp, over
// hal_core.hpp) runs on this. GUI-only: the backends are src/alchemy/host/<backend>/gui/audio.cpp,
// compiled into alchemy-fx.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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

// ---- output streams ----
// A shared-mode stream on a render endpoint. A thread of the backend asks `render` for frames
// of interleaved 32-bit float samples in the stream's format, about once per period, while
// the stream runs. `queued` is how many frames the host already holds ahead of the ones asked for
// (they play first). `render` must fill all `frames` frames and must not call back into the stream.
using Render = std::function<void(float* samples, std::uint32_t frames, std::uint32_t queued)>;

struct OutputFormat {
    std::uint32_t sample_rate{}, channels{};
    std::uint32_t channel_mask{};      // speaker positions; 0 unknown
    std::uint32_t period_frames{};     // how many frames a render call asks for, about
    std::uint32_t buffer_frames{};     // the host's buffer, the most it holds ahead
};

class OutputStream;

// Opens a stream on the endpoint `uid` ("" for the default output) in `sample_rate` and
// `channels` (0: the mixer's; the host converts others to its mixer format). Error::not_found:
// no such endpoint; Error::unsupported: no audio output on this host. Never touches the calling
// thread's COM (or other per-thread) state.
[[nodiscard]] Result<OutputStream*> open_output(std::string_view uid, std::uint32_t sample_rate, std::uint32_t channels, Render render);
[[nodiscard]] OutputFormat output_format(const OutputStream* stream) noexcept;
// Starts asking `render` for frames (the host buffer starts silent).
[[nodiscard]] Status start_output(OutputStream* stream);
// Stops; when it returns, `render` is not running and is not called again until a start.
void stop_output(OutputStream* stream) noexcept;
// Stops and frees the stream.
void close_output(OutputStream* stream) noexcept;

} // namespace alchemy::host::audio
