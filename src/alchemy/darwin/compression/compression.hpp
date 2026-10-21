#pragma once

// libcompression (host-only core): the buffer and stream APIs of
// <compression.h> for the algorithms Alchemy implements itself.
//   COMPRESSION_ZLIB     raw DEFLATE (RFC 1951, no zlib header), through
//                        zlib_codec.hpp (encoding at level 5)
//   COMPRESSION_LZ4_RAW  an LZ4 block (the public LZ4 block format)
//   COMPRESSION_LZ4      Apple's LZ4 framing of such blocks: "bv41" blocks
//                        (decoded size, encoded size, LZ4 block), "bv4-"
//                        uncompressed blocks, and the "bv4$" end marker
// COMPRESSION_LZFSE, COMPRESSION_LZMA, COMPRESSION_BROTLI and
// COMPRESSION_LZBITMAP are not provided: the buffer functions return 0 and
// compression_stream_init COMPRESSION_STATUS_ERROR, the documented failures.
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace alchemy::gui::completion_kit::compression {

enum Algorithm : std::uint32_t {
    lz4 = 0x100, lz4_raw = 0x101, zlib = 0x205, lzma = 0x306, lzbitmap = 0x702, lzfse = 0x801, brotli = 0xb02,
};
[[nodiscard]] bool supported(std::uint32_t algorithm);

// LZ4 block format.
[[nodiscard]] std::vector<std::uint8_t> lz4_block_encode(std::span<const std::uint8_t> input);
// nullopt for malformed data; output beyond `limit` bytes is an error.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> lz4_block_decode(std::span<const std::uint8_t> input, std::size_t limit);

// Whole buffers: nullopt for an unsupported algorithm or malformed input.
// Decoding stops after `limit` output bytes (the rest is dropped, as
// compression_decode_buffer truncates to its destination).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode(std::uint32_t algorithm, std::span<const std::uint8_t> input);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode(std::uint32_t algorithm, std::span<const std::uint8_t> input, std::size_t limit);

// compression_stream: consumes input, produces output into `pending`
// (the caller delivers and erases what fits its destination).
enum class Status { ok = 0, error = -1, end = 1 };   // compression_status
class Stream {
public:
    // nullptr for an unsupported algorithm / operation (0 encode, 1 decode).
    static std::unique_ptr<Stream> create(int operation, std::uint32_t algorithm);
    virtual ~Stream() = default;
    // Consumes some or all of `input` (`consumed` says how much); false for
    // malformed input. `finalize`: no more input follows
    // (COMPRESSION_STREAM_FINALIZE).
    virtual bool process(std::span<const std::uint8_t> input, std::size_t& consumed, bool finalize) = 0;
    // No more output will be produced (the end of the encoded stream was
    // decoded, or the finalized input was encoded completely).
    [[nodiscard]] virtual bool done() const = 0;
    std::vector<std::uint8_t> pending;   // produced, not yet delivered
};

} // namespace alchemy::gui::completion_kit::compression
