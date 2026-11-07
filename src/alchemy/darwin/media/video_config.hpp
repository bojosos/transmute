#pragma once

// The decoder configuration records of H.264 and HEVC streams (host-only core), ISO/IEC 14496-15:
// the AVCDecoderConfigurationRecord (MP4's and the sample description's 'avcC', 5.3.3.1) and the
// HEVCDecoderConfigurationRecord ('hvcC', 8.3.3.1), made from the stream's parameter sets and read back
// into them. Parameter sets are NAL units without start codes or length prefixes, with their emulation
// prevention bytes (as they travel in the stream).

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace alchemy::gui::completion_kit::media::video_config {

using NalUnits = std::vector<std::vector<std::uint8_t>>;

// The 'avcC' of H.264 parameter sets (SPS, NAL type 7; PPS, 8; SPS extensions, 13), with `nal_length_size`
// (1, 2 or 4) bytes before each NAL unit in the samples. Empty without an SPS and a PPS.
[[nodiscard]] std::vector<std::uint8_t> avc_configuration(const NalUnits& sets, unsigned nal_length_size = 4);
// The 'hvcC' of HEVC parameter sets (VPS, NAL type 32; SPS, 33; PPS, 34; SEI prefixes, 39). Empty without
// an SPS and a PPS.
[[nodiscard]] std::vector<std::uint8_t> hevc_configuration(const NalUnits& sets, unsigned nal_length_size = 4);

struct Configuration {
    NalUnits sets;                  // in the record's order
    unsigned nal_length_size{};
};
// The parameter sets of an 'avcC' or 'hvcC'; nothing for a malformed record.
[[nodiscard]] std::optional<Configuration> read_avc_configuration(std::span<const std::uint8_t> record);
[[nodiscard]] std::optional<Configuration> read_hevc_configuration(std::span<const std::uint8_t> record);

} // namespace alchemy::gui::completion_kit::media::video_config
