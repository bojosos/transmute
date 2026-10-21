#pragma once

// AudioFile / ExtAudioFile core (host-only, QLab gap group G5): the linear
// PCM audio files Alchemy reads, and conversion between linear PCM formats.
//
// Containers: WAVE (PCM, IEEE float and WAVE_FORMAT_EXTENSIBLE with either
// subformat; RF64 is not read), AIFF and AIFC ('NONE', 'sowt', 'twos',
// 'fl32', 'fl64', 'in24', 'in32', and their little-endian forms) and CAF
// ('lpcm' only). Compressed data (MP3, AAC, ALAC, IMA, ...) has no decoder
// under Alchemy and is reported as an unsupported data format.
//
// Conversion: any linear PCM description (integer of 8-32 bits in any
// container, signed or unsigned, either endianness, high- or low-aligned;
// 32- or 64-bit float; interleaved or not) to any other with the same sample
// rate. Channel counts must match, except mono to N channels (copied) and N
// to mono (averaged), as AudioConverter's channel mapping does by default.
// Sample-rate conversion is not provided. Integer output rounds and clips.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::audio_file {

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

namespace error {
inline constexpr std::int32_t unspecified = static_cast<std::int32_t>(four("wht?")), unsupported_file_type = static_cast<std::int32_t>(four("typ?")),
    unsupported_data_format = static_cast<std::int32_t>(four("fmt?")), unsupported_property = static_cast<std::int32_t>(four("pty?")),
    bad_property_size = static_cast<std::int32_t>(four("!siz")), invalid_file = static_cast<std::int32_t>(four("dta!")),
    operation_not_supported = static_cast<std::int32_t>(0x6F703F3F),   // 'op??'
    invalid_packet_offset = static_cast<std::int32_t>(four("pck?")), end_of_file = -39, file_not_found = -43, param = -50;
}

// kLinearPCMFormatFlag*
namespace flag {
inline constexpr std::uint32_t is_float = 1, big_endian = 2, signed_integer = 4, packed = 8, aligned_high = 16, non_interleaved = 32;
}

struct Pcm {
    double rate{44100};
    std::uint32_t channels{2};
    std::uint32_t bits{16};            // valid bits per sample
    std::uint32_t container{2};        // bytes per sample
    bool is_float{}, big_endian{}, is_signed{true}, non_interleaved{}, aligned_high{};

    [[nodiscard]] std::uint32_t frame_bytes() const { return container * (non_interleaved ? 1 : channels); }   // per buffer
    [[nodiscard]] std::uint32_t flags() const;
};

// AudioStreamBasicDescription (40 bytes) <-> Pcm; nullopt for non-PCM or unsupported layouts.
std::optional<Pcm> pcm_from_asbd(std::span<const std::uint8_t> asbd, std::string& why);
std::vector<std::uint8_t> asbd_from_pcm(const Pcm& format);

struct Container {
    std::uint32_t file_type{};         // 'WAVE', 'AIFF', 'AIFC', 'caff'
    Pcm format;
    std::uint64_t data_offset{}, data_bytes{};
    std::uint32_t channel_mask{};      // WAVE_FORMAT_EXTENSIBLE dwChannelMask (0 = none)
};

// Reads up to `out.size()` bytes at `offset`; returns the count read.
using ReadAt = std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)>;

// Parses a file's header; an OSStatus (error::unsupported_file_type for an
// unknown container, error::unsupported_data_format for compressed data).
std::int32_t parse(const ReadAt& read, std::uint64_t file_size, Container& out, std::string& why);

// Interleaved samples in [-1, 1] from `frames` frames of `bytes` (one buffer per
// channel when non-interleaved: `buffers` holds them in order).
std::vector<double> decode(const Pcm& format, const std::vector<std::span<const std::uint8_t>>& buffers, std::size_t frames);
// Encodes interleaved samples (in `source_channels`) into the buffers of `format`.
void encode(const Pcm& format, std::span<const double> interleaved, std::uint32_t source_channels, std::size_t frames,
            const std::vector<std::span<std::uint8_t>>& buffers);
// Whether `from` can be converted to `to` (see the header comment).
bool convertible(const Pcm& from, const Pcm& to);

// The AudioChannelLayout bytes for a WAVE speaker mask (descriptions), empty for none.
std::vector<std::uint8_t> layout_for_mask(std::uint32_t mask, std::uint32_t channels);

} // namespace alchemy::gui::completion_kit::media::audio_file
