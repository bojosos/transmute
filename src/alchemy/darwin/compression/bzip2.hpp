#pragma once

// bzip2 streams (host-only), written from the public description of the
// format: "BZh" + level, blocks (48-bit magic 0x314159265359, block CRC,
// randomised flag, BWT origin pointer, symbol map, 2..6 Huffman tables,
// selectors, RUNA/RUNB zero runs over the move-to-front of the BWT), and the
// end-of-stream marker 0x177245385090 with the combined CRC. The initial
// run-length stage (4..255 equal bytes become 4 bytes and a count) is
// applied per block; CRCs are CRC-32/BZIP2 (polynomial 0x04c11db7, MSB first).
// Blocks marked "randomised" (written only by bzip2 0.9.0 and older) are not
// provided: decoding reports them as a data error.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::bzip2 {

[[nodiscard]] std::uint32_t crc(std::span<const std::uint8_t> bytes, std::uint32_t value = 0xffffffffu);   // running CRC (finish with ~)

enum class Error { none, magic, data, unexpected_end, unsupported };
struct Decoded {
    Error error{Error::none};
    std::vector<std::uint8_t> output;
    std::size_t consumed{};   // bytes of the first stream (the rest is "unused")
    std::string message;
};
// Decodes the first bzip2 stream of `input`.
[[nodiscard]] Decoded decode(std::span<const std::uint8_t> input);
// Encodes `input` as one stream; `level` 1..9 sets the block size (level x 100k).
[[nodiscard]] std::vector<std::uint8_t> encode(std::span<const std::uint8_t> input, int level = 9);

} // namespace alchemy::gui::completion_kit::bzip2
