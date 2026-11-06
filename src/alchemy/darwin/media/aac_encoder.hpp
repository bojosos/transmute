#pragma once

// AAC encoding for the media group (host-only core): the host system's AAC encoders
// (alchemy/host/media_codec.hpp) behind a backend seam the tests replace with a fake. A stream takes
// interleaved 16-bit PCM and gives raw AAC-LC frames of 1024 frames each, in order; the encoder's delay
// (priming) leads the stream. The magic cookie AudioConverter and the files carry is the MPEG-4 ES
// descriptor (ISO/IEC 14496-1) around the encoder's AudioSpecificConfig (ISO/IEC 14496-3).

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::aac {

inline constexpr std::uint32_t frames_per_packet = 1024;

// What the encoders take: sample rates, channel counts and bit rates (bits per second).
struct Capabilities {
    std::vector<std::uint32_t> sample_rates, channel_counts, bit_rates;
    [[nodiscard]] bool empty() const { return sample_rates.empty(); }
};

class Encoder {
public:
    virtual ~Encoder() = default;
    // Appends the frames ready so far (raw AAC, one 1024-frame packet each). False on an encoder failure.
    virtual bool encode(std::span<const std::int16_t> samples, std::vector<std::vector<std::uint8_t>>& out) = 0;
    // No more samples: the rest.
    virtual bool drain(std::vector<std::vector<std::uint8_t>>& out) = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> audio_specific_config() const = 0;
    [[nodiscard]] virtual std::uint32_t bit_rate() const = 0;
    // Frames of the encoder's delay at the start of the stream.
    [[nodiscard]] virtual std::uint32_t priming() const = 0;
};

struct Backend {
    std::function<Capabilities()> capabilities;
    // Null with `why` when no encoder takes the rate and channels.
    std::function<std::unique_ptr<Encoder>(std::uint32_t rate, std::uint32_t channels, std::uint32_t bit_rate, std::string& why)> open;
};
// Tests: replaces the host's encoders (an empty Backend restores them).
void set_backend(Backend backend);

[[nodiscard]] Capabilities capabilities();
[[nodiscard]] bool available();
[[nodiscard]] std::unique_ptr<Encoder> open(std::uint32_t rate, std::uint32_t channels, std::uint32_t bit_rate, std::string& why);

// The ES_Descriptor (from its tag 0x03) of an MPEG-4 audio stream with this AudioSpecificConfig: AudioConverter's
// and CAF's magic cookie, and the payload of MP4's 'esds' box after its version and flags.
[[nodiscard]] std::vector<std::uint8_t> es_descriptor(std::span<const std::uint8_t> audio_specific_config, std::uint32_t average_bit_rate,
                                                      std::uint32_t maximum_bit_rate, std::uint32_t buffer_bytes);

} // namespace alchemy::gui::completion_kit::media::aac
