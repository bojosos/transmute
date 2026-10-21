#pragma once

// Project-authored DEFLATE (RFC 1951) and zlib container (RFC 1950) codec used
// by the /usr/lib/libz.1.dylib provider. Owner: v-payload (src/alchemy/darwin/kit/export_fill).
//
// The decoder handles stored, fixed-Huffman and dynamic-Huffman blocks and
// verifies the Adler-32 trailer. The encoder emits stored blocks for level 0
// and fixed-Huffman blocks with greedy hash-chain LZ77 matching for levels
// 1-9; every output is a valid zlib stream any inflater accepts. Neither
// derives from zlib's sources.

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <span>
#include <vector>

namespace alchemy::export_fill {

[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::uint32_t adler32_update(std::uint32_t adler, std::span<const std::uint8_t> bytes) noexcept;

// zlib's compressBound: the largest zlib stream compress2 produces for
// `length` input bytes (stored-block worst case plus header and trailer).
[[nodiscard]] std::uint64_t zlib_compress_bound(std::uint64_t length) noexcept;

// Raw DEFLATE data for `input`; level 0 stores, 1-9 use fixed Huffman codes.
[[nodiscard]] std::vector<std::uint8_t> deflate_raw(std::span<const std::uint8_t> input, int level);
// zlib container: CMF/FLG header, raw DEFLATE data, big-endian Adler-32.
[[nodiscard]] std::vector<std::uint8_t> zlib_compress(std::span<const std::uint8_t> input, int level);

enum class InflateStatus { ok, data_error, buffer_error };
struct InflateResult {
    InflateStatus status{InflateStatus::data_error};
    std::vector<std::uint8_t> output;
    std::size_t consumed{};   // input bytes used, including the zlib trailer
};
// Decodes a raw DEFLATE stream. Output beyond `limit` bytes is a buffer error.
[[nodiscard]] InflateResult inflate_raw(std::span<const std::uint8_t> input, std::size_t limit);
// Decodes a zlib stream (header, data, Adler-32). Preset dictionaries are a
// data error (uncompress() reports Z_DATA_ERROR for them too: Z_NEED_DICT).
[[nodiscard]] InflateResult zlib_decompress(std::span<const std::uint8_t> input, std::size_t limit);

// Container around the DEFLATE data of a stream (zlib.h windowBits):
// RFC 1950 zlib, RFC 1952 gzip, none, or (inflate only) zlib-or-gzip.
enum class Wrapper { zlib, gzip, raw, automatic };

// Incremental decoder behind inflate(). Input is taken in any split; output
// becomes available block by block (and inside an incomplete block, as far as
// it has been decoded). A block is re-decoded from its start when it is
// continued, so no decoder state is held mid-block.
class InflateStream {
public:
    enum class Status { more, end, data_error, need_dictionary };
    struct Result { Status status{}; std::size_t consumed{}; };
    explicit InflateStream(Wrapper wrapper);
    ~InflateStream();
    InflateStream(const InflateStream&) = delete;
    InflateStream& operator=(const InflateStream&) = delete;
    // Consumes input up to the end of the stream (bytes after it are not consumed).
    Result feed(std::span<const std::uint8_t> input);
    // Decoded bytes not yet delivered; the caller erases what it delivers.
    [[nodiscard]] std::vector<std::uint8_t>& output() noexcept;
    // True once the stream (and its trailer) has been decoded completely.
    [[nodiscard]] bool ended() const noexcept;
    // Adler-32 (zlib) or CRC-32 (gzip) of the output so far; for
    // need_dictionary, the dictionary id.
    [[nodiscard]] std::uint32_t check() const noexcept;
    [[nodiscard]] std::string_view message() const noexcept;
    // Checksum verification of the trailer (inflateValidate).
    void verify(bool enabled) noexcept;
    // Restarts for a new stream: with the given wrapper, or the current one.
    void reset(Wrapper wrapper);
    void reset();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Incremental encoder behind deflate(). Data waiting for a flush is held;
// each flush encodes it as independent blocks, a sync/full/partial flush ends
// with an empty stored block (byte aligned), and finish writes the final
// block and the trailer.
class DeflateStream {
public:
    enum class Flush { none, partial, sync, full, finish, block };
    DeflateStream(Wrapper wrapper, int level);
    ~DeflateStream();
    DeflateStream(const DeflateStream&) = delete;
    DeflateStream& operator=(const DeflateStream&) = delete;
    void feed(std::span<const std::uint8_t> input, Flush flush);
    [[nodiscard]] std::vector<std::uint8_t>& output() noexcept;
    [[nodiscard]] bool finished() const noexcept;
    // Adler-32 (zlib) or CRC-32 (gzip) of the input so far.
    [[nodiscard]] std::uint32_t check() const noexcept;
    void reset();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace alchemy::export_fill
