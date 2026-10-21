#include "alchemy/darwin/compression/zlib_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

// Independent implementation of the published formats: RFC 1950 (zlib),
// RFC 1951 (DEFLATE) and the CRC-32 of ISO 3309 / ITU-T V.42.
namespace alchemy::export_fill {
namespace {

constexpr std::array<std::uint16_t, 29> length_base{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> distance_base{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> distance_extra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// Order of the code-length code lengths in a dynamic block header.
constexpr std::array<std::uint8_t, 19> code_length_order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2,
    14, 1, 15};

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320U ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}
constexpr auto crc_table = make_crc_table();

struct DataError {};
// Input ended inside the stream: a data error for the one-shot decoders, a
// request for more input for the streaming one.
struct Truncated : DataError {};
struct BufferError {};

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> input) : input_(input) {}
    std::uint32_t bits(unsigned count) {
        while (have_ < count) {
            if (position_ >= input_.size()) throw Truncated{};
            buffer_ |= static_cast<std::uint64_t>(input_[position_++]) << have_;
            have_ += 8;
        }
        const auto value = static_cast<std::uint32_t>(buffer_ & ((1ULL << count) - 1));
        buffer_ >>= count;
        have_ -= count;
        return value;
    }
    void align() { buffer_ >>= have_ % 8; have_ -= have_ % 8; }
    // Bytes consumed so far, not counting whole bytes still buffered.
    [[nodiscard]] std::size_t consumed() const noexcept { return position_ - have_ / 8; }
    [[nodiscard]] std::size_t bit_position() const noexcept { return position_ * 8 - have_; }
    std::uint8_t byte() { align(); return static_cast<std::uint8_t>(bits(8)); }

private:
    std::span<const std::uint8_t> input_;
    std::size_t position_{};
    std::uint64_t buffer_{};
    unsigned have_{};
};

// Canonical Huffman decoding (RFC 1951 section 3.2.2): codes of each length
// are consecutive integers, so a symbol is found by counting.
class Huffman {
public:
    // Returns false for an over-subscribed set or (unless `allow_incomplete`) an incomplete one.
    bool build(std::span<const std::uint8_t> lengths, bool allow_incomplete) {
        count_.fill(0);
        for (const auto length : lengths) ++count_[length];
        count_[0] = 0;
        int left = 1;
        for (int length = 1; length <= 15; ++length) {
            left <<= 1;
            left -= count_[length];
            if (left < 0) return false;
        }
        std::array<std::uint16_t, 16> offset{};
        for (int length = 1; length < 15; ++length) offset[length + 1] = static_cast<std::uint16_t>(offset[length] + count_[length]);
        symbol_.assign(lengths.size(), 0);
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
            if (lengths[symbol]) symbol_[offset[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
        return left == 0 || allow_incomplete;
    }
    int decode(BitReader& reader) const {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= 15; ++length) {
            code |= static_cast<int>(reader.bits(1));
            const int count = count_[length];
            if (code - first < count) return symbol_[static_cast<std::size_t>(index + code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw DataError{};
    }

private:
    std::array<std::uint16_t, 16> count_{};
    std::vector<std::uint16_t> symbol_;
};

void inflate_codes(BitReader& reader, std::vector<std::uint8_t>& output, std::size_t limit,
                   const Huffman& literals, const Huffman& distances) {
    for (;;) {
        const int symbol = literals.decode(reader);
        if (symbol < 256) {
            if (output.size() >= limit) throw BufferError{};
            output.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return;
        const auto index = static_cast<std::size_t>(symbol - 257);
        if (index >= length_base.size()) throw DataError{};
        const std::size_t length = length_base[index] + reader.bits(length_extra[index]);
        const auto distance_symbol = static_cast<std::size_t>(distances.decode(reader));
        if (distance_symbol >= distance_base.size()) throw DataError{};
        const std::size_t distance = distance_base[distance_symbol] + reader.bits(distance_extra[distance_symbol]);
        if (distance > output.size()) throw DataError{};
        if (output.size() + length > limit) throw BufferError{};
        const auto start = output.size() - distance;
        for (std::size_t i = 0; i < length; ++i) output.push_back(output[start + i]);
    }
}

struct FixedCodes {
    Huffman literals, distances;
    bool built{};
};

// Decodes one block onto `output`, whose tail is the back-reference window.
// Returns whether it was the final block.
bool decode_block(BitReader& reader, std::vector<std::uint8_t>& output, std::size_t limit, FixedCodes& fixed) {
    auto& fixed_literals = fixed.literals;
    auto& fixed_distances = fixed.distances;
    auto& fixed_built = fixed.built;
    const bool last = reader.bits(1) != 0;
    {
        switch (reader.bits(2)) {
        case 0: {
            reader.align();
            const auto length = reader.bits(16), complement = reader.bits(16);
            if ((length ^ 0xffffU) != complement) throw DataError{};
            if (output.size() + length > limit) throw BufferError{};
            for (std::uint32_t i = 0; i < length; ++i) output.push_back(static_cast<std::uint8_t>(reader.bits(8)));
            break;
        }
        case 1: {
            if (!fixed_built) {
                std::array<std::uint8_t, 288> lengths{};
                std::fill(lengths.begin(), lengths.begin() + 144, std::uint8_t{8});
                std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9});
                std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7});
                std::fill(lengths.begin() + 280, lengths.end(), std::uint8_t{8});
                std::array<std::uint8_t, 30> distance_lengths{};
                distance_lengths.fill(5);
                static_cast<void>(fixed_literals.build(lengths, false));
                static_cast<void>(fixed_distances.build(distance_lengths, true));
                fixed_built = true;
            }
            inflate_codes(reader, output, limit, fixed_literals, fixed_distances);
            break;
        }
        case 2: {
            const auto literal_count = reader.bits(5) + 257, distance_count = reader.bits(5) + 1,
                       code_count = reader.bits(4) + 4;
            if (literal_count > 286 || distance_count > 30) throw DataError{};
            std::array<std::uint8_t, 19> code_lengths{};
            for (std::uint32_t i = 0; i < code_count; ++i) code_lengths[code_length_order[i]] = static_cast<std::uint8_t>(reader.bits(3));
            Huffman lengths_code;
            if (!lengths_code.build(code_lengths, false)) throw DataError{};
            std::array<std::uint8_t, 316> lengths{};
            for (std::uint32_t index = 0; index < literal_count + distance_count;) {
                const int symbol = lengths_code.decode(reader);
                if (symbol < 16) { lengths[index++] = static_cast<std::uint8_t>(symbol); continue; }
                std::uint8_t value = 0;
                std::uint32_t repeat = 0;
                if (symbol == 16) {
                    if (index == 0) throw DataError{};
                    value = lengths[index - 1];
                    repeat = 3 + reader.bits(2);
                } else if (symbol == 17) repeat = 3 + reader.bits(3);
                else repeat = 11 + reader.bits(7);
                if (index + repeat > literal_count + distance_count) throw DataError{};
                while (repeat--) lengths[index++] = value;
            }
            if (lengths[256] == 0) throw DataError{};
            Huffman literals, distances;
            const std::span<const std::uint8_t> all(lengths.data(), literal_count + distance_count);
            // An incomplete code is valid only when it has a single code (RFC 1951 3.2.7).
            const auto count_used = [](std::span<const std::uint8_t> values) {
                return std::count_if(values.begin(), values.end(), [](auto v) { return v != 0; });
            };
            if (!literals.build(all.first(literal_count), count_used(all.first(literal_count)) == 1)) throw DataError{};
            if (!distances.build(all.subspan(literal_count), count_used(all.subspan(literal_count)) <= 1)) throw DataError{};
            inflate_codes(reader, output, limit, literals, distances);
            break;
        }
        default: throw DataError{};
        }
    }
    return last;
}

void inflate_stream(BitReader& reader, std::vector<std::uint8_t>& output, std::size_t limit) {
    FixedCodes fixed;
    while (!decode_block(reader, output, limit, fixed)) {}
}

class BitWriter {
public:
    void bits(std::uint32_t value, unsigned count) {
        buffer_ |= static_cast<std::uint64_t>(value) << have_;
        have_ += count;
        while (have_ >= 8) { output.push_back(static_cast<std::uint8_t>(buffer_)); buffer_ >>= 8; have_ -= 8; }
    }
    // Huffman codes are stored most-significant bit first.
    void code(std::uint32_t value, unsigned count) {
        std::uint32_t reversed = 0;
        for (unsigned i = 0; i < count; ++i) reversed |= ((value >> i) & 1U) << (count - 1 - i);
        bits(reversed, count);
    }
    void align() { if (have_) bits(0, 8 - have_); }
    std::vector<std::uint8_t> output;

private:
    std::uint64_t buffer_{};
    unsigned have_{};
};

struct Token { std::uint16_t length; std::uint16_t distance; std::uint8_t literal; };

void fixed_literal(BitWriter& writer, unsigned symbol) {
    if (symbol < 144) writer.code(0x30 + symbol, 8);
    else if (symbol < 256) writer.code(0x190 + symbol - 144, 9);
    else if (symbol < 280) writer.code(symbol - 256, 7);
    else writer.code(0xc0 + symbol - 280, 8);
}
unsigned fixed_literal_bits(unsigned symbol) { return symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8; }
std::size_t length_index(unsigned length) {
    return static_cast<std::size_t>(std::upper_bound(length_base.begin(), length_base.end(), length) - length_base.begin() - 1);
}
std::size_t distance_index(unsigned distance) {
    return static_cast<std::size_t>(std::upper_bound(distance_base.begin(), distance_base.end(), distance) - distance_base.begin() - 1);
}

} // namespace

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::uint8_t> bytes) noexcept {
    crc = ~crc;
    for (const auto byte : bytes) crc = crc_table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t adler32_update(std::uint32_t adler, std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t a = adler & 0xffff, b = adler >> 16;
    std::size_t index = 0;
    while (index < bytes.size()) {
        // 5552 is the largest run whose sums cannot overflow 32 bits.
        const auto end = std::min(bytes.size(), index + 5552);
        for (; index < end; ++index) { a += bytes[index]; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

std::uint64_t zlib_compress_bound(std::uint64_t length) noexcept {
    return length + (length >> 12) + (length >> 14) + (length >> 25) + 13;
}

namespace {

// Encodes `input` as DEFLATE blocks continuing `writer`'s bit stream; the last
// block carries BFINAL only when `final`. Matches never reach before `input`.
void deflate_into(BitWriter& writer, std::span<const std::uint8_t> input, int level, bool final) {
    constexpr std::size_t block = 65535, window = 32768, hash_size = 1U << 15;
    if (input.empty() && !final) return;
    if (level == 0 || input.empty()) {
        std::size_t position = 0;
        do {
            const auto size = std::min(block, input.size() - position);
            writer.bits(final && position + size == input.size() ? 1 : 0, 1);
            writer.bits(0, 2);
            writer.align();
            writer.bits(static_cast<std::uint32_t>(size), 16);
            writer.bits(static_cast<std::uint32_t>(size ^ 0xffff), 16);
            writer.output.insert(writer.output.end(), input.begin() + static_cast<std::ptrdiff_t>(position),
                                 input.begin() + static_cast<std::ptrdiff_t>(position + size));
            position += size;
        } while (position < input.size());
        return;
    }
    const unsigned chain_limit = static_cast<unsigned>(4 << std::min(level, 9));
    std::vector<std::int64_t> head(hash_size, -1), previous(input.size(), -1);
    const auto hash = [&](std::size_t at) {
        return ((static_cast<std::uint32_t>(input[at]) << 10) ^ (static_cast<std::uint32_t>(input[at + 1]) << 5) ^ input[at + 2]) & (hash_size - 1);
    };
    const auto insert = [&](std::size_t at) {
        if (at + 2 >= input.size()) return;
        const auto h = hash(at);
        previous[at] = head[h];
        head[h] = static_cast<std::int64_t>(at);
    };
    std::size_t position = 0;
    while (position < input.size()) {
        // Tokenize up to one block of input, then pick the cheaper encoding.
        const std::size_t begin = position, end = std::min(input.size(), position + block);
        std::vector<Token> tokens;
        std::size_t fixed_cost = 3 + 7;   // header + end-of-block
        while (position < end) {
            unsigned best = 0, best_distance = 0;
            if (position + 2 < input.size()) {
                const auto maximum = static_cast<unsigned>(std::min<std::size_t>(258, end - position));
                auto candidate = head[hash(position)];
                for (unsigned chain = 0; candidate >= 0 && chain < chain_limit; ++chain) {
                    const auto from = static_cast<std::size_t>(candidate);
                    if (position - from > window) break;
                    unsigned length = 0;
                    while (length < maximum && input[from + length] == input[position + length]) ++length;
                    if (length > best) { best = length; best_distance = static_cast<unsigned>(position - from); if (length == maximum) break; }
                    candidate = previous[from];
                }
            }
            if (best >= 3) {
                tokens.push_back({static_cast<std::uint16_t>(best), static_cast<std::uint16_t>(best_distance), 0});
                const auto li = length_index(best), di = distance_index(best_distance);
                fixed_cost += fixed_literal_bits(257 + static_cast<unsigned>(li)) + length_extra[li] + 5 + distance_extra[di];
                for (unsigned i = 0; i < best; ++i) insert(position + i);
                position += best;
            } else {
                tokens.push_back({0, 0, input[position]});
                fixed_cost += fixed_literal_bits(input[position]);
                insert(position);
                ++position;
            }
        }
        const bool last = final && position == input.size();
        const std::size_t stored_cost = 3 + 7 + 32 + (end - begin) * 8;
        if (stored_cost <= fixed_cost) {
            writer.bits(last ? 1 : 0, 1);
            writer.bits(0, 2);
            writer.align();
            writer.bits(static_cast<std::uint32_t>(end - begin), 16);
            writer.bits(static_cast<std::uint32_t>((end - begin) ^ 0xffff), 16);
            writer.output.insert(writer.output.end(), input.begin() + static_cast<std::ptrdiff_t>(begin),
                                 input.begin() + static_cast<std::ptrdiff_t>(end));
            continue;
        }
        writer.bits(last ? 1 : 0, 1);
        writer.bits(1, 2);
        for (const auto& token : tokens) {
            if (!token.length) { fixed_literal(writer, token.literal); continue; }
            const auto li = length_index(token.length), di = distance_index(token.distance);
            fixed_literal(writer, 257 + static_cast<unsigned>(li));
            writer.bits(token.length - length_base[li], length_extra[li]);
            writer.code(static_cast<std::uint32_t>(di), 5);
            writer.bits(token.distance - distance_base[di], distance_extra[di]);
        }
        fixed_literal(writer, 256);
    }
}

} // namespace

std::vector<std::uint8_t> deflate_raw(std::span<const std::uint8_t> input, int level) {
    BitWriter writer;
    deflate_into(writer, input, level, true);
    writer.align();
    return std::move(writer.output);
}

std::vector<std::uint8_t> zlib_compress(std::span<const std::uint8_t> input, int level) {
    // CMF: deflate, 32K window. FLG: FLEVEL from the level, FCHECK makes the pair a multiple of 31.
    const std::uint8_t cmf = 0x78;
    const std::uint8_t flevel = level == 0 || level == 1 ? 0 : level < 6 ? 1 : level == 6 ? 2 : 3;
    std::uint8_t flg = static_cast<std::uint8_t>(flevel << 6);
    flg = static_cast<std::uint8_t>(flg + (31 - (cmf * 256 + flg) % 31) % 31);
    std::vector<std::uint8_t> output{cmf, flg};
    const auto data = deflate_raw(input, level);
    output.insert(output.end(), data.begin(), data.end());
    const auto adler = adler32_update(1, input);
    for (int shift = 24; shift >= 0; shift -= 8) output.push_back(static_cast<std::uint8_t>(adler >> shift));
    return output;
}

InflateResult inflate_raw(std::span<const std::uint8_t> input, std::size_t limit) {
    InflateResult result;
    BitReader reader(input);
    try {
        inflate_stream(reader, result.output, limit);
        reader.align();
        result.consumed = reader.consumed();
        result.status = InflateStatus::ok;
    } catch (const DataError&) {
        result.status = InflateStatus::data_error;
    } catch (const BufferError&) {
        result.status = InflateStatus::buffer_error;
    }
    return result;
}

InflateResult zlib_decompress(std::span<const std::uint8_t> input, std::size_t limit) {
    InflateResult failed;
    if (input.size() < 2) return failed;   // truncated input is a data error, as in uncompress()
    const auto cmf = input[0], flg = input[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 != 0 || (flg & 0x20)) return failed;
    auto result = inflate_raw(input.subspan(2), limit);
    if (result.status != InflateStatus::ok) return result;
    const auto trailer = 2 + result.consumed;
    if (input.size() < trailer + 4) { result.status = InflateStatus::data_error; return result; }
    std::uint32_t expected = 0;
    for (int i = 0; i < 4; ++i) expected = (expected << 8) | input[trailer + static_cast<std::size_t>(i)];
    if (expected != adler32_update(1, result.output)) { result.status = InflateStatus::data_error; return result; }
    result.consumed = trailer + 4;
    return result;
}

// ---- Streaming decoder ----------------------------------------------------

struct InflateStream::State {
    enum class Stage { header, blocks, trailer, done, failed };
    Wrapper requested{Wrapper::zlib}, wrapper{Wrapper::zlib};
    Stage stage{Stage::header};
    std::vector<std::uint8_t> buffer;      // input not yet consumed, from the current block start
    unsigned bit_offset{};                  // bits of buffer[0] already consumed
    std::vector<std::uint8_t> history;      // the last 32 KiB of output (back-reference window)
    std::size_t emitted_in_block{};         // output of the incomplete block already delivered
    std::vector<std::uint8_t> output;
    FixedCodes fixed;
    std::uint32_t check{}, dictionary{};
    std::uint64_t total_out{};
    bool verify{true};
    std::string_view message;

    void emit(std::span<const std::uint8_t> bytes) {
        output.insert(output.end(), bytes.begin(), bytes.end());
        total_out += bytes.size();
        if (wrapper == Wrapper::zlib) check = adler32_update(check, bytes);
        else if (wrapper == Wrapper::gzip) check = crc32_update(check, bytes);
    }
    void fail(std::string_view text) { stage = Stage::failed; message = text; }
    void start(Wrapper value) {
        requested = value;
        wrapper = value == Wrapper::automatic ? Wrapper::zlib : value;
        stage = value == Wrapper::raw ? Stage::blocks : Stage::header;
        buffer.clear(); bit_offset = 0; history.clear(); emitted_in_block = 0; output.clear();
        check = wrapper == Wrapper::zlib ? 1U : 0U; dictionary = 0; total_out = 0; message = {};
    }
    // Parses the zlib or gzip header; false when more input is needed.
    bool header() {
        if (buffer.size() < 2) return false;
        if (requested == Wrapper::automatic) wrapper = buffer[0] == 0x1f && buffer[1] == 0x8b ? Wrapper::gzip : Wrapper::zlib;
        check = wrapper == Wrapper::zlib ? 1U : 0U;
        if (wrapper == Wrapper::zlib) {
            const auto cmf = buffer[0], flg = buffer[1];
            if ((cmf * 256 + flg) % 31 != 0) { fail("incorrect header check"); return true; }
            if ((cmf & 0x0f) != 8) { fail("unknown compression method"); return true; }
            if ((cmf >> 4) > 7) { fail("invalid window size"); return true; }
            if (flg & 0x20) {
                // Preset dictionaries (inflateSetDictionary) are not provided.
                if (buffer.size() < 6) return false;
                dictionary = (std::uint32_t{buffer[2]} << 24) | (std::uint32_t{buffer[3]} << 16) |
                             (std::uint32_t{buffer[4]} << 8) | buffer[5];
                fail("preset dictionaries are unsupported");
                return true;
            }
            buffer.erase(buffer.begin(), buffer.begin() + 2);
            stage = Stage::blocks;
            return true;
        }
        // RFC 1952 member header.
        if (buffer.size() < 10) return false;
        if (buffer[0] != 0x1f || buffer[1] != 0x8b) { fail("incorrect header check"); return true; }
        if (buffer[2] != 8) { fail("unknown compression method"); return true; }
        const auto flags = buffer[3];
        if (flags & 0xe0) { fail("unknown header flags set"); return true; }
        std::size_t at = 10;
        if (flags & 4) {
            if (buffer.size() < at + 2) return false;
            at += 2 + (std::size_t{buffer[at]} | (std::size_t{buffer[at + 1]} << 8));
            if (buffer.size() < at) return false;
        }
        for (const std::uint8_t field : {std::uint8_t{8}, std::uint8_t{16}}) {
            if (!(flags & field)) continue;
            while (at < buffer.size() && buffer[at] != 0) ++at;
            if (at >= buffer.size()) return false;
            ++at;
        }
        if (flags & 2) {
            if (buffer.size() < at + 2) return false;
            const auto expected = std::uint32_t{buffer[at]} | (std::uint32_t{buffer[at + 1]} << 8);
            if (verify && expected != (crc32_update(0, std::span(buffer.data(), at)) & 0xffffU)) {
                fail("header crc mismatch");
                return true;
            }
            at += 2;
        }
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(at));
        stage = Stage::blocks;
        return true;
    }
    // Decodes complete blocks; false when more input is needed.
    bool blocks() {
        for (;;) {
            BitReader reader(buffer);
            const auto before = history.size();
            bool last{};
            try {
                if (bit_offset) static_cast<void>(reader.bits(bit_offset));
                last = decode_block(reader, history, SIZE_MAX, fixed);
            } catch (const Truncated&) {
                if (history.size() > before + emitted_in_block) {
                    emit(std::span(history).subspan(before + emitted_in_block));
                    emitted_in_block = history.size() - before;
                }
                history.resize(before);
                return false;
            } catch (const DataError&) {
                history.resize(before);
                fail("invalid deflate data");
                return true;
            }
            if (history.size() > before + emitted_in_block) emit(std::span(history).subspan(before + emitted_in_block));
            emitted_in_block = 0;
            const auto bits = reader.bit_position();
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bits / 8));
            bit_offset = static_cast<unsigned>(bits % 8);
            if (history.size() > 32768) history.erase(history.begin(), history.end() - 32768);
            if (last) {
                if (bit_offset) { buffer.erase(buffer.begin()); bit_offset = 0; }
                stage = wrapper == Wrapper::raw ? Stage::done : Stage::trailer;
                return true;
            }
        }
    }
    bool trailer() {
        const std::size_t size = wrapper == Wrapper::gzip ? 8 : 4;
        if (buffer.size() < size) return false;
        if (wrapper == Wrapper::zlib) {
            const auto expected = (std::uint32_t{buffer[0]} << 24) | (std::uint32_t{buffer[1]} << 16) |
                                  (std::uint32_t{buffer[2]} << 8) | buffer[3];
            if (verify && expected != check) { fail("incorrect data check"); return true; }
        } else {
            const auto crc = std::uint32_t{buffer[0]} | (std::uint32_t{buffer[1]} << 8) |
                             (std::uint32_t{buffer[2]} << 16) | (std::uint32_t{buffer[3]} << 24);
            const auto length = std::uint32_t{buffer[4]} | (std::uint32_t{buffer[5]} << 8) |
                                (std::uint32_t{buffer[6]} << 16) | (std::uint32_t{buffer[7]} << 24);
            if (verify && crc != check) { fail("incorrect data check"); return true; }
            if (verify && length != static_cast<std::uint32_t>(total_out)) { fail("incorrect length check"); return true; }
        }
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
        stage = Stage::done;
        return true;
    }
};

InflateStream::InflateStream(Wrapper wrapper) : state_(std::make_unique<State>()) { state_->start(wrapper); }
InflateStream::~InflateStream() = default;
std::vector<std::uint8_t>& InflateStream::output() noexcept { return state_->output; }
bool InflateStream::ended() const noexcept { return state_->stage == State::Stage::done; }
std::uint32_t InflateStream::check() const noexcept { return state_->dictionary ? state_->dictionary : state_->check; }
std::string_view InflateStream::message() const noexcept { return state_->message; }
void InflateStream::verify(bool enabled) noexcept { state_->verify = enabled; }
void InflateStream::reset() { reset(state_->requested); }
void InflateStream::reset(Wrapper wrapper) {
    const auto keep = state_->verify;
    state_->start(wrapper);
    state_->verify = keep;
}

InflateStream::Result InflateStream::feed(std::span<const std::uint8_t> input) {
    auto& s = *state_;
    using Stage = State::Stage;
    if (s.stage == Stage::failed) return {s.dictionary ? Status::need_dictionary : Status::data_error, 0};
    if (s.stage == Stage::done) return {Status::end, 0};
    s.buffer.insert(s.buffer.end(), input.begin(), input.end());
    for (;;) {
        bool progressed = false;
        switch (s.stage) {
        case Stage::header: progressed = s.header(); break;
        case Stage::blocks: progressed = s.blocks(); break;
        case Stage::trailer: progressed = s.trailer(); break;
        case Stage::done: {
            // Bytes after the stream belong to the caller. Earlier input was
            // all needed (the stream had not ended), so they came with this call.
            const auto leftover = std::min(s.buffer.size(), input.size());
            s.buffer.clear();
            return {Status::end, input.size() - leftover};
        }
        case Stage::failed:
            s.buffer.clear();
            return {s.dictionary ? Status::need_dictionary : Status::data_error, input.size()};
        }
        if (!progressed) return {Status::more, input.size()};
    }
}

// ---- Streaming encoder ----------------------------------------------------

struct DeflateStream::State {
    Wrapper wrapper{Wrapper::zlib};
    int level{6};
    BitWriter writer;
    std::vector<std::uint8_t> pending;   // input waiting for a flush
    std::vector<std::uint8_t> output;
    std::uint32_t check{};
    std::uint64_t total_in{};
    bool header{}, finished{};

    void drain() {
        output.insert(output.end(), writer.output.begin(), writer.output.end());
        writer.output.clear();
    }
    void start() {
        writer = BitWriter{};
        pending.clear();
        output.clear();
        check = wrapper == Wrapper::zlib ? 1U : 0U;
        total_in = 0;
        header = false;
        finished = false;
    }
};

DeflateStream::DeflateStream(Wrapper wrapper, int level) : state_(std::make_unique<State>()) {
    state_->wrapper = wrapper == Wrapper::automatic ? Wrapper::zlib : wrapper;
    state_->level = level < 0 ? 6 : std::min(level, 9);
    state_->start();
}
DeflateStream::~DeflateStream() = default;
std::vector<std::uint8_t>& DeflateStream::output() noexcept { return state_->output; }
bool DeflateStream::finished() const noexcept { return state_->finished; }
std::uint32_t DeflateStream::check() const noexcept { return state_->check; }
void DeflateStream::reset() { state_->start(); }

void DeflateStream::feed(std::span<const std::uint8_t> input, Flush flush) {
    auto& s = *state_;
    if (s.finished) return;
    if (!s.header) {
        if (s.wrapper == Wrapper::zlib) {
            const std::uint8_t cmf = 0x78;
            const std::uint8_t flevel = s.level <= 1 ? 0 : s.level < 6 ? 1 : s.level == 6 ? 2 : 3;
            auto flg = static_cast<std::uint8_t>(flevel << 6);
            flg = static_cast<std::uint8_t>(flg + (31 - (cmf * 256 + flg) % 31) % 31);
            s.output.insert(s.output.end(), {cmf, flg});
        } else if (s.wrapper == Wrapper::gzip) {
            // No name or modification time; XFL by level; OS 3 (Unix), as on macOS.
            const std::uint8_t xfl = s.level == 9 ? 2 : s.level == 1 ? 4 : 0;
            s.output.insert(s.output.end(), {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, xfl, 3});
        }
        s.header = true;
    }
    s.pending.insert(s.pending.end(), input.begin(), input.end());
    s.total_in += input.size();
    if (s.wrapper == Wrapper::zlib) s.check = adler32_update(s.check, input);
    else if (s.wrapper == Wrapper::gzip) s.check = crc32_update(s.check, input);
    // Without a flush, input is batched up to 1 MiB before it is encoded.
    if (flush == Flush::none && s.pending.size() < (1U << 20)) return;
    const bool final = flush == Flush::finish;
    deflate_into(s.writer, s.pending, s.level, final);
    s.pending.clear();
    if (final) {
        s.writer.align();
        s.drain();
        if (s.wrapper == Wrapper::zlib) {
            for (int shift = 24; shift >= 0; shift -= 8) s.output.push_back(static_cast<std::uint8_t>(s.check >> shift));
        } else if (s.wrapper == Wrapper::gzip) {
            for (int shift = 0; shift < 32; shift += 8) s.output.push_back(static_cast<std::uint8_t>(s.check >> shift));
            for (int shift = 0; shift < 32; shift += 8) s.output.push_back(static_cast<std::uint8_t>(s.total_in >> shift));
        }
        s.finished = true;
        return;
    }
    if (flush == Flush::sync || flush == Flush::full || flush == Flush::partial) {
        // An empty stored block: everything so far decodes completely.
        s.writer.bits(0, 1);
        s.writer.bits(0, 2);
        s.writer.align();
        s.writer.bits(0, 16);
        s.writer.bits(0xffff, 16);
    }
    s.drain();
}

} // namespace alchemy::export_fill
