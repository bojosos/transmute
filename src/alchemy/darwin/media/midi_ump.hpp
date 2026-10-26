#pragma once

// Universal MIDI Packets (MIDI 2.0) <-> MIDI 1.0 byte streams, for CoreMIDI's event lists
// (host-only core; coremidi.cpp). Group 0 throughout.
// - From bytes: channel voice messages become MIDI 1.0 channel voice packets (type 2) for
//   protocol 1.0, or MIDI 2.0 channel voice packets (type 4, values scaled up by the
//   min-center-max rule) for protocol 2.0; system common and real-time messages become type 1;
//   system exclusive becomes 7-bit data packets (type 3, six bytes each). A system exclusive
//   message still open at the end of the bytes ends there.
// - To bytes: types 1, 2 and 3, and MIDI 2.0 channel voice (type 4) scaled down; a program change
//   with its bank becomes bank select (controllers 0 and 32) and the program. Utility messages,
//   128-bit data, per-note and registered/assignable controllers of MIDI 2.0, and the other types
//   have no MIDI 1.0 form and are dropped.

#include <cstdint>
#include <span>
#include <vector>

namespace alchemy::gui::completion_kit::media::midi::ump {

inline constexpr std::uint32_t protocol_1_0 = 1, protocol_2_0 = 2;   // kMIDIProtocol_1_0 / _2_0

// The words of the packet that starts with `first` (by its message type).
[[nodiscard]] std::size_t word_count(std::uint32_t first);
// `value` of `from_bits` scaled to `to_bits` (min-center-max upscaling, or a right shift down).
[[nodiscard]] std::uint32_t scale(std::uint32_t value, unsigned from_bits, unsigned to_bits);

[[nodiscard]] std::vector<std::uint32_t> from_bytes(std::span<const std::uint8_t> bytes, std::uint32_t protocol);
[[nodiscard]] std::vector<std::uint8_t> to_bytes(std::span<const std::uint32_t> words);

} // namespace alchemy::gui::completion_kit::media::midi::ump
