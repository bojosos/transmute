#include "alchemy/darwin/compression/compression.hpp"
#include "alchemy/darwin/compression/zlib_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace alchemy::gui::completion_kit::compression {
namespace {

std::uint32_t read32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
std::uint32_t le32(std::span<const std::uint8_t> s, std::size_t at) {
    return static_cast<std::uint32_t>(s[at]) | static_cast<std::uint32_t>(s[at + 1]) << 8 | static_cast<std::uint32_t>(s[at + 2]) << 16 | static_cast<std::uint32_t>(s[at + 3]) << 24;
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
void put_length(std::vector<std::uint8_t>& out, std::size_t extra) {
    while (extra >= 255) { out.push_back(255); extra -= 255; }
    out.push_back(static_cast<std::uint8_t>(extra));
}

constexpr std::size_t lz4_block = 1u << 16;   // input bytes per framed block
constexpr std::uint8_t magic_compressed[4] = {0x62, 0x76, 0x34, 0x31};   // "bv41"
constexpr std::uint8_t magic_raw[4] = {0x62, 0x76, 0x34, 0x2d};          // "bv4-"
constexpr std::uint8_t magic_end[4] = {0x62, 0x76, 0x34, 0x24};          // "bv4$"

std::vector<std::uint8_t> lz4_frame(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> out;
    for (std::size_t at = 0; at < input.size(); at += lz4_block) {
        const auto chunk = input.subspan(at, std::min(lz4_block, input.size() - at));
        const auto block = lz4_block_encode(chunk);
        if (block.size() < chunk.size()) {
            out.insert(out.end(), magic_compressed, magic_compressed + 4);
            put32(out, static_cast<std::uint32_t>(chunk.size()));
            put32(out, static_cast<std::uint32_t>(block.size()));
            out.insert(out.end(), block.begin(), block.end());
        } else {
            out.insert(out.end(), magic_raw, magic_raw + 4);
            put32(out, static_cast<std::uint32_t>(chunk.size()));
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
    }
    out.insert(out.end(), magic_end, magic_end + 4);
    return out;
}

// Decodes the complete framed blocks of `input` into `out`; returns the bytes
// used (`ended` once the end marker was read), nullopt for malformed data.
std::optional<std::size_t> lz4_unframe(std::span<const std::uint8_t> input, std::vector<std::uint8_t>& out, bool& ended, std::size_t limit) {
    std::size_t at = 0;
    ended = false;
    const auto is = [&](const std::uint8_t (&magic)[4]) { return std::equal(magic, magic + 4, input.begin() + static_cast<std::ptrdiff_t>(at)); };
    while (at + 4 <= input.size()) {
        if (is(magic_end)) { ended = true; return at + 4; }
        if (is(magic_compressed)) {
            if (at + 12 > input.size()) break;
            const auto size = le32(input, at + 4), encoded = le32(input, at + 8);
            if (at + 12 + encoded > input.size()) break;
            auto block = lz4_block_decode(input.subspan(at + 12, encoded), size);
            if (!block || block->size() != size) return std::nullopt;
            out.insert(out.end(), block->begin(), block->end());
            at += 12 + encoded;
        } else if (is(magic_raw)) {
            if (at + 8 > input.size()) break;
            const auto size = le32(input, at + 4);
            if (at + 8 + size > input.size()) break;
            out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(at + 8), input.begin() + static_cast<std::ptrdiff_t>(at + 8 + size));
            at += 8 + size;
        } else return std::nullopt;
        if (out.size() > limit) return at;
    }
    return at;
}

class ZlibDecode final : public Stream {
public:
    bool process(std::span<const std::uint8_t> input, std::size_t& consumed, bool finalize) override {
        consumed = 0;
        if (stream_.ended()) return true;
        const auto r = stream_.feed(input);
        consumed = r.consumed;
        auto& out = stream_.output();
        pending.insert(pending.end(), out.begin(), out.end());
        out.clear();
        if (r.status == export_fill::InflateStream::Status::data_error || r.status == export_fill::InflateStream::Status::need_dictionary) return false;
        // Finalized input that ends inside the DEFLATE stream is truncated data.
        return !(finalize && consumed == input.size() && !stream_.ended());
    }
    bool done() const override { return stream_.ended(); }
private:
    export_fill::InflateStream stream_{export_fill::Wrapper::raw};
};

class ZlibEncode final : public Stream {
public:
    bool process(std::span<const std::uint8_t> input, std::size_t& consumed, bool finalize) override {
        consumed = 0;
        if (stream_.finished()) return true;
        stream_.feed(input, finalize ? export_fill::DeflateStream::Flush::finish : export_fill::DeflateStream::Flush::none);
        consumed = input.size();
        auto& out = stream_.output();
        pending.insert(pending.end(), out.begin(), out.end());
        out.clear();
        return true;
    }
    bool done() const override { return stream_.finished(); }
private:
    export_fill::DeflateStream stream_{export_fill::Wrapper::raw, 5};
};

class Lz4Decode final : public Stream {
public:
    explicit Lz4Decode(bool framed) : framed_(framed) {}
    bool process(std::span<const std::uint8_t> input, std::size_t& consumed, bool finalize) override {
        consumed = 0;
        if (ended_) return true;
        buffer_.insert(buffer_.end(), input.begin(), input.end());
        consumed = input.size();
        if (framed_) {
            bool ended = false;
            const auto used = lz4_unframe(buffer_, pending, ended, SIZE_MAX);
            if (!used) return false;
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(*used));
            ended_ = ended;
            return !(finalize && !ended_);
        }
        if (!finalize) return true;   // a raw block has no end marker: decode at the end
        const auto block = lz4_block_decode(buffer_, std::size_t{1} << 31);
        if (!block) return false;
        pending.insert(pending.end(), block->begin(), block->end());
        buffer_.clear();
        ended_ = true;
        return true;
    }
    bool done() const override { return ended_; }
private:
    bool framed_, ended_{};
    std::vector<std::uint8_t> buffer_;
};

class Lz4Encode final : public Stream {
public:
    explicit Lz4Encode(bool framed) : framed_(framed) {}
    bool process(std::span<const std::uint8_t> input, std::size_t& consumed, bool finalize) override {
        consumed = 0;
        if (finished_) return true;
        buffer_.insert(buffer_.end(), input.begin(), input.end());
        consumed = input.size();
        if (!finalize) return true;
        const auto out = framed_ ? lz4_frame(buffer_) : lz4_block_encode(buffer_);
        pending.insert(pending.end(), out.begin(), out.end());
        buffer_.clear();
        finished_ = true;
        return true;
    }
    bool done() const override { return finished_; }
private:
    bool framed_, finished_{};
    std::vector<std::uint8_t> buffer_;
};

} // namespace

bool supported(std::uint32_t algorithm) { return algorithm == zlib || algorithm == lz4 || algorithm == lz4_raw; }

std::vector<std::uint8_t> lz4_block_encode(std::span<const std::uint8_t> in) {
    std::vector<std::uint8_t> out;
    const auto n = in.size();
    std::vector<std::int64_t> table(std::size_t{1} << 12, -1);
    std::size_t anchor = 0, i = 0;
    const auto emit = [&](std::size_t literal_end, std::size_t offset, std::size_t match) {
        const auto literals = literal_end - anchor;
        const auto token_match = match ? std::min<std::size_t>(match - 4, 15) : 0;
        out.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(literals, 15) << 4 | token_match));
        if (literals >= 15) put_length(out, literals - 15);
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(anchor), in.begin() + static_cast<std::ptrdiff_t>(literal_end));
        if (!match) return;
        out.push_back(static_cast<std::uint8_t>(offset));
        out.push_back(static_cast<std::uint8_t>(offset >> 8));
        if (match - 4 >= 15) put_length(out, match - 4 - 15);
    };
    // The block format: the last match starts at least 12 bytes before the
    // end, and the last 5 bytes are literals.
    if (n >= 13) {
        const auto limit = n - 12;
        while (i < limit) {
            const auto sequence = read32(in.data() + i);
            const auto h = (sequence * 2654435761u) >> 20;
            const auto candidate = table[h];
            table[h] = static_cast<std::int64_t>(i);
            if (candidate >= 0 && i - static_cast<std::size_t>(candidate) <= 65535 && read32(in.data() + candidate) == sequence) {
                std::size_t length = 4;
                while (i + length < n - 5 && in[static_cast<std::size_t>(candidate) + length] == in[i + length]) ++length;
                emit(i, i - static_cast<std::size_t>(candidate), length);
                i += length;
                anchor = i;
                continue;
            }
            ++i;
        }
    }
    emit(n, 0, 0);
    return out;
}

std::optional<std::vector<std::uint8_t>> lz4_block_decode(std::span<const std::uint8_t> in, std::size_t limit) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    const auto length = [&](std::size_t base) -> std::optional<std::size_t> {
        if (base != 15) return base;
        std::size_t total = base;
        for (;;) {
            if (at >= in.size()) return std::nullopt;
            const auto b = in[at++];
            total += b;
            if (b != 255) return total;
        }
    };
    while (at < in.size()) {
        const auto token = in[at++];
        const auto literals = length(token >> 4);
        if (!literals || at + *literals > in.size() || out.size() + *literals > limit) return std::nullopt;
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + *literals));
        at += *literals;
        if (at == in.size()) break;   // the last sequence has only literals
        if (at + 2 > in.size()) return std::nullopt;
        const std::size_t offset = in[at] | static_cast<std::size_t>(in[at + 1]) << 8;
        at += 2;
        if (offset == 0 || offset > out.size()) return std::nullopt;
        const auto match = length(token & 15);
        if (!match) return std::nullopt;
        const auto count = *match + 4;
        if (out.size() + count > limit) return std::nullopt;
        const auto from = out.size() - offset;
        for (std::size_t k = 0; k < count; ++k) out.push_back(out[from + k]);   // overlapping copies repeat
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> encode(std::uint32_t algorithm, std::span<const std::uint8_t> input) {
    switch (algorithm) {
    case zlib: return export_fill::deflate_raw(input, 5);
    case lz4_raw: return lz4_block_encode(input);
    case lz4: return lz4_frame(input);
    default: return std::nullopt;
    }
}

std::optional<std::vector<std::uint8_t>> decode(std::uint32_t algorithm, std::span<const std::uint8_t> input, std::size_t limit) {
    switch (algorithm) {
    case zlib: {
        export_fill::InflateStream stream(export_fill::Wrapper::raw);
        std::vector<std::uint8_t> out;
        std::size_t at = 0;
        while (out.size() < limit && !stream.ended()) {
            const auto r = stream.feed(input.subspan(at));
            at += r.consumed;
            auto& produced = stream.output();
            out.insert(out.end(), produced.begin(), produced.end());
            produced.clear();
            if (r.status == export_fill::InflateStream::Status::data_error || r.status == export_fill::InflateStream::Status::need_dictionary) return std::nullopt;
            if (r.consumed == 0) break;   // input exhausted (a truncated stream yields what was decoded)
        }
        if (out.size() > limit) out.resize(limit);
        return out;
    }
    case lz4_raw: {
        auto out = lz4_block_decode(input, std::size_t{1} << 31);
        if (out && out->size() > limit) out->resize(limit);
        return out;
    }
    case lz4: {
        std::vector<std::uint8_t> out;
        bool ended = false;
        if (!lz4_unframe(input, out, ended, limit)) return std::nullopt;
        if (out.size() > limit) out.resize(limit);
        return out;
    }
    default: return std::nullopt;
    }
}

std::unique_ptr<Stream> Stream::create(int operation, std::uint32_t algorithm) {
    if (operation != 0 && operation != 1) return nullptr;
    const bool encoding = operation == 0;
    switch (algorithm) {
    case zlib: if (encoding) return std::make_unique<ZlibEncode>(); return std::make_unique<ZlibDecode>();
    case lz4: if (encoding) return std::make_unique<Lz4Encode>(true); return std::make_unique<Lz4Decode>(true);
    case lz4_raw: if (encoding) return std::make_unique<Lz4Encode>(false); return std::make_unique<Lz4Decode>(false);
    default: return nullptr;
    }
}

} // namespace alchemy::gui::completion_kit::compression
