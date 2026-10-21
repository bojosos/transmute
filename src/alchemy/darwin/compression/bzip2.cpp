#include "alchemy/darwin/compression/bzip2.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <queue>

namespace alchemy::gui::completion_kit::bzip2 {
namespace {

constexpr std::uint64_t block_magic = 0x314159265359ull, end_magic = 0x177245385090ull;
constexpr int max_code_length = 20, encode_max_length = 17, group_size = 50;

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

struct Failure { Error error; std::string message; };

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> in) : in_(in) {}
    std::uint32_t bit() {
        if (at_ >= in_.size() * 8) throw Failure{Error::unexpected_end, "the stream ends early"};
        const auto b = (in_[at_ / 8] >> (7 - at_ % 8)) & 1u;
        ++at_;
        return b;
    }
    std::uint64_t bits(int n) { std::uint64_t v = 0; for (int i = 0; i < n; ++i) v = (v << 1) | bit(); return v; }
    std::size_t byte_position() const { return (at_ + 7) / 8; }
private:
    std::span<const std::uint8_t> in_;
    std::size_t at_{};
};

class BitWriter {
public:
    void bits(std::uint64_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            current_ = static_cast<std::uint8_t>((current_ << 1) | ((value >> i) & 1u));
            if (++count_ == 8) { out.push_back(current_); current_ = 0; count_ = 0; }
        }
    }
    void flush() { if (count_) { out.push_back(static_cast<std::uint8_t>(current_ << (8 - count_))); current_ = 0; count_ = 0; } }
    std::vector<std::uint8_t> out;
private:
    std::uint8_t current_{};
    int count_{};
};

// Canonical Huffman decoding table of one group.
struct Table {
    std::array<std::int32_t, max_code_length + 2> base{}, count{}, offset{};
    std::vector<int> symbols;   // by (length, symbol)
    int min_length{max_code_length}, max_length{0};
    void build(const std::vector<int>& lengths) {
        count.fill(0);
        for (const auto l : lengths) { ++count[l]; min_length = std::min(min_length, l); max_length = std::max(max_length, l); }
        symbols.clear();
        for (int l = 1; l <= max_code_length; ++l)
            for (int s = 0; s < static_cast<int>(lengths.size()); ++s) if (lengths[static_cast<std::size_t>(s)] == l) symbols.push_back(s);
        std::int32_t code = 0, index = 0;
        for (int l = 1; l <= max_code_length; ++l) {
            base[l] = code;
            offset[l] = index;
            code = (code + count[l]) << 1;
            index += count[l];
        }
    }
    int decode(BitReader& r) const {
        std::int32_t code = 0;
        for (int l = 1; l <= max_length; ++l) {
            code = (code << 1) | static_cast<std::int32_t>(r.bit());
            if (l >= min_length && code - base[l] < count[l] && code >= base[l]) return symbols[static_cast<std::size_t>(offset[l] + code - base[l])];
        }
        throw Failure{Error::data, "an invalid Huffman code"};
    }
};

// One block's bytes (after the initial run-length stage is undone).
std::vector<std::uint8_t> decode_block(BitReader& r, std::size_t block_limit, std::uint32_t& stored_crc) {
    stored_crc = static_cast<std::uint32_t>(r.bits(32));
    if (r.bit()) throw Failure{Error::unsupported, "a randomised block (bzip2 0.9.0 and older) is not provided"};
    const auto origin = static_cast<std::size_t>(r.bits(24));
    // Symbol map.
    std::vector<std::uint8_t> used;
    const auto groups16 = r.bits(16);
    for (int i = 0; i < 16; ++i)
        if (groups16 & (0x8000u >> i)) {
            const auto mask = r.bits(16);
            for (int j = 0; j < 16; ++j) if (mask & (0x8000u >> j)) used.push_back(static_cast<std::uint8_t>(i * 16 + j));
        }
    if (used.empty()) throw Failure{Error::data, "no symbols in use"};
    const int alpha = static_cast<int>(used.size()) + 2;
    const auto groups = static_cast<int>(r.bits(3));
    if (groups < 2 || groups > 6) throw Failure{Error::data, "bad table count"};
    const auto selector_count = static_cast<std::size_t>(r.bits(15));
    if (selector_count == 0) throw Failure{Error::data, "no selectors"};
    std::vector<std::uint8_t> selectors(selector_count);
    {
        std::vector<std::uint8_t> mtf(static_cast<std::size_t>(groups));
        std::iota(mtf.begin(), mtf.end(), std::uint8_t{0});
        for (auto& sel : selectors) {
            int j = 0;
            while (r.bit()) if (++j >= groups) throw Failure{Error::data, "bad selector"};
            const auto v = mtf[static_cast<std::size_t>(j)];
            std::rotate(mtf.begin(), mtf.begin() + j, mtf.begin() + j + 1);
            sel = v;
        }
    }
    std::vector<Table> tables(static_cast<std::size_t>(groups));
    for (auto& t : tables) {
        std::vector<int> lengths(static_cast<std::size_t>(alpha));
        int current = static_cast<int>(r.bits(5));
        for (auto& l : lengths) {
            for (;;) {
                if (current < 1 || current > max_code_length) throw Failure{Error::data, "bad code length"};
                if (!r.bit()) break;
                current += r.bit() ? -1 : 1;
            }
            l = current;
        }
        t.build(lengths);
    }
    // Symbols: MTF indexes with RUNA / RUNB zero runs, up to EOB.
    std::vector<std::uint8_t> mtf = used;
    std::vector<std::uint8_t> tt;
    const int end_of_block = alpha - 1;
    std::size_t group = 0;
    int in_group = 0;
    std::int64_t run = 0, weight = 1;   // RUNA / RUNB: the run length in bijective base 2
    bool in_run = false;
    const auto next_symbol = [&] {
        if (in_group == 0) {
            if (group >= selectors.size()) throw Failure{Error::data, "more symbols than selectors"};
            in_group = group_size;
            ++group;
        }
        --in_group;
        return tables[selectors[group - 1]].decode(r);
    };
    for (;;) {
        const auto s = next_symbol();
        if (s == 0 || s == 1) {   // RUNA adds 1 x weight, RUNB 2 x weight
            run += (s + 1) * weight;
            weight <<= 1;
            in_run = true;
            if (run > static_cast<std::int64_t>(block_limit)) throw Failure{Error::data, "a block larger than its level allows"};
            continue;
        }
        if (in_run) {
            if (tt.size() + static_cast<std::size_t>(run) > block_limit) throw Failure{Error::data, "a block larger than its level allows"};
            tt.insert(tt.end(), static_cast<std::size_t>(run), mtf[0]);
            run = 0;
            weight = 1;
            in_run = false;
        }
        if (s == end_of_block) break;
        const auto index = static_cast<std::size_t>(s - 1);
        if (index >= mtf.size()) throw Failure{Error::data, "an MTF index past the symbols in use"};
        const auto value = mtf[index];
        std::rotate(mtf.begin(), mtf.begin() + static_cast<std::ptrdiff_t>(index), mtf.begin() + static_cast<std::ptrdiff_t>(index) + 1);
        if (tt.size() >= block_limit) throw Failure{Error::data, "a block larger than its level allows"};
        tt.push_back(value);
    }
    if (tt.empty()) return {};
    if (origin >= tt.size()) throw Failure{Error::data, "the BWT origin is past the block"};
    // Inverse BWT.
    std::array<std::size_t, 256> counts{};
    for (const auto c : tt) ++counts[c];
    std::array<std::size_t, 256> start{};
    for (std::size_t c = 1; c < 256; ++c) start[c] = start[c - 1] + counts[c - 1];
    std::vector<std::uint32_t> next(tt.size());
    for (std::size_t i = 0; i < tt.size(); ++i) next[start[tt[i]]++] = static_cast<std::uint32_t>(i);
    std::vector<std::uint8_t> rle;
    rle.reserve(tt.size());
    auto p = next[origin];
    for (std::size_t i = 0; i < tt.size(); ++i) { rle.push_back(tt[p]); p = next[p]; }
    // Undo the initial run-length stage.
    std::vector<std::uint8_t> out;
    out.reserve(rle.size());
    int same = 0;
    int last = -1;
    for (std::size_t i = 0; i < rle.size(); ++i) {
        const auto c = rle[i];
        if (same == 4) {
            out.insert(out.end(), c, static_cast<std::uint8_t>(last));
            same = 0;
            last = -1;
            continue;
        }
        if (c == last) ++same; else { same = 1; last = c; }
        out.push_back(c);
    }
    return out;
}

// ---- encoding ----
std::vector<std::uint32_t> suffix_order(const std::vector<std::uint8_t>& s) {
    const auto n = s.size();
    std::vector<std::uint32_t> sa(n), rank(n), next_rank(n);
    std::vector<std::uint64_t> key(n);
    for (std::size_t i = 0; i < n; ++i) { sa[i] = static_cast<std::uint32_t>(i); rank[i] = s[i]; }
    for (std::size_t k = 1;; k <<= 1) {
        for (std::size_t i = 0; i < n; ++i) key[i] = (std::uint64_t{rank[i]} << 32) | rank[(i + k) % n];
        std::sort(sa.begin(), sa.end(), [&](std::uint32_t a, std::uint32_t b) { return key[a] < key[b] || (key[a] == key[b] && a < b); });
        next_rank[sa[0]] = 0;
        for (std::size_t i = 1; i < n; ++i) next_rank[sa[i]] = next_rank[sa[i - 1]] + (key[sa[i]] != key[sa[i - 1]] ? 1 : 0);
        rank.swap(next_rank);
        if (rank[sa[n - 1]] == n - 1 || k >= n) break;
    }
    return sa;
}

std::vector<int> code_lengths(std::vector<std::uint64_t> freq) {
    const auto n = freq.size();
    for (;;) {
        // Huffman tree depths.
        using Node = std::pair<std::uint64_t, int>;
        std::priority_queue<Node, std::vector<Node>, std::greater<>> heap;
        std::vector<int> parent(2 * n, -1);
        for (std::size_t i = 0; i < n; ++i) heap.emplace(freq[i] ? freq[i] : 1, static_cast<int>(i));
        int next = static_cast<int>(n);
        while (heap.size() > 1) {
            const auto a = heap.top(); heap.pop();
            const auto b = heap.top(); heap.pop();
            parent[static_cast<std::size_t>(a.second)] = next;
            parent[static_cast<std::size_t>(b.second)] = next;
            heap.emplace(a.first + b.first, next++);
        }
        std::vector<int> lengths(n);
        int longest = 0;
        for (std::size_t i = 0; i < n; ++i) {
            int depth = 0;
            for (auto p = parent[i]; p >= 0; p = parent[static_cast<std::size_t>(p)]) ++depth;
            lengths[i] = std::max(depth, 1);
            longest = std::max(longest, lengths[i]);
        }
        if (longest <= encode_max_length) return lengths;
        for (auto& f : freq) f = 1 + f / 2;
    }
}

void encode_block(BitWriter& w, const std::vector<std::uint8_t>& block, std::uint32_t block_crc) {
    w.bits(block_magic, 48);
    w.bits(block_crc, 32);
    w.bits(0, 1);   // not randomised
    const auto sa = suffix_order(block);
    const auto n = block.size();
    std::size_t origin = 0;
    std::vector<std::uint8_t> last(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (sa[i] == 0) origin = i;
        last[i] = block[(sa[i] + n - 1) % n];
    }
    w.bits(origin, 24);
    // Symbol map.
    std::array<bool, 256> in_use{};
    for (const auto c : block) in_use[c] = true;
    std::vector<std::uint8_t> used;
    for (int c = 0; c < 256; ++c) if (in_use[static_cast<std::size_t>(c)]) used.push_back(static_cast<std::uint8_t>(c));
    std::uint32_t groups16 = 0;
    for (int i = 0; i < 16; ++i) for (int j = 0; j < 16; ++j) if (in_use[static_cast<std::size_t>(i * 16 + j)]) groups16 |= 0x8000u >> i;
    w.bits(groups16, 16);
    for (int i = 0; i < 16; ++i) {
        if (!(groups16 & (0x8000u >> i))) continue;
        std::uint32_t mask = 0;
        for (int j = 0; j < 16; ++j) if (in_use[static_cast<std::size_t>(i * 16 + j)]) mask |= 0x8000u >> j;
        w.bits(mask, 16);
    }
    // MTF and zero runs.
    const int alpha = static_cast<int>(used.size()) + 2;
    const int end_of_block = alpha - 1;
    std::vector<std::uint8_t> mtf = used;
    std::vector<int> symbols;
    std::uint64_t zeros = 0;
    const auto flush_zeros = [&] {
        if (!zeros) return;
        auto r = zeros - 1;
        for (;;) {
            symbols.push_back((r & 1) ? 1 : 0);
            if (r < 2) break;
            r = (r - 2) / 2;
        }
        zeros = 0;
    };
    for (const auto c : last) {
        const auto at = static_cast<std::size_t>(std::find(mtf.begin(), mtf.end(), c) - mtf.begin());
        if (at == 0) { ++zeros; continue; }
        flush_zeros();
        std::rotate(mtf.begin(), mtf.begin() + static_cast<std::ptrdiff_t>(at), mtf.begin() + static_cast<std::ptrdiff_t>(at) + 1);
        symbols.push_back(static_cast<int>(at) + 1);
    }
    flush_zeros();
    symbols.push_back(end_of_block);
    // Two tables (the minimum the format allows), both from the block's frequencies.
    std::vector<std::uint64_t> freq(static_cast<std::size_t>(alpha));
    for (const auto s : symbols) ++freq[static_cast<std::size_t>(s)];
    const auto lengths = code_lengths(freq);
    std::vector<std::uint32_t> codes(lengths.size());
    {
        std::uint32_t code = 0;
        for (int l = 1; l <= max_code_length; ++l) {
            for (std::size_t s = 0; s < lengths.size(); ++s) if (lengths[s] == l) codes[s] = code++;
            code <<= 1;
        }
    }
    const auto selector_count = (symbols.size() + group_size - 1) / group_size;
    w.bits(2, 3);
    w.bits(selector_count, 15);
    for (std::size_t i = 0; i < selector_count; ++i) w.bits(0, 1);   // table 0 each time (MTF value 0)
    for (int t = 0; t < 2; ++t) {
        int current = lengths[0];
        w.bits(static_cast<std::uint64_t>(current), 5);
        for (const auto l : lengths) {
            while (current < l) { w.bits(2, 2); ++current; }   // "10": increment
            while (current > l) { w.bits(3, 2); --current; }   // "11": decrement
            w.bits(0, 1);
        }
    }
    for (const auto s : symbols) w.bits(codes[static_cast<std::size_t>(s)], lengths[static_cast<std::size_t>(s)]);
}

} // namespace

std::uint32_t crc(std::span<const std::uint8_t> bytes, std::uint32_t value) {
    const auto& t = crc_table();
    for (const auto b : bytes) value = (value << 8) ^ t[((value >> 24) ^ b) & 0xff];
    return value;
}

Decoded decode(std::span<const std::uint8_t> input) {
    Decoded d;
    try {
        if (input.size() < 4 || input[0] != 'B' || input[1] != 'Z' || input[2] != 'h' || input[3] < '1' || input[3] > '9')
            throw Failure{Error::magic, "not a bzip2 stream"};
        const std::size_t block_limit = static_cast<std::size_t>(input[3] - '0') * 100000;
        BitReader r(input.subspan(4));
        std::uint32_t combined = 0;
        for (;;) {
            const auto magic = r.bits(48);
            if (magic == end_magic) {
                const auto stored = static_cast<std::uint32_t>(r.bits(32));
                if (stored != combined) throw Failure{Error::data, "the stream CRC does not match"};
                break;
            }
            if (magic != block_magic) throw Failure{Error::data, "a bad block header"};
            std::uint32_t stored = 0;
            const auto block = decode_block(r, block_limit, stored);
            const auto actual = ~crc(block);
            if (actual != stored) throw Failure{Error::data, "a block CRC does not match"};
            combined = ((combined << 1) | (combined >> 31)) ^ actual;
            d.output.insert(d.output.end(), block.begin(), block.end());
        }
        d.consumed = 4 + r.byte_position();
    } catch (const Failure& f) {
        d.error = f.error;
        d.message = f.message;
    }
    return d;
}

std::vector<std::uint8_t> encode(std::span<const std::uint8_t> input, int level) {
    level = std::clamp(level, 1, 9);
    BitWriter w;
    w.bits('B', 8); w.bits('Z', 8); w.bits('h', 8); w.bits(static_cast<std::uint64_t>('0' + level), 8);
    const std::size_t limit = static_cast<std::size_t>(level) * 100000 - 19;
    std::uint32_t combined = 0;
    std::size_t at = 0;
    while (at < input.size()) {
        // The initial run-length stage, up to the block size (runs never split).
        std::vector<std::uint8_t> block;
        const auto start = at;
        while (at < input.size() && block.size() + 5 <= limit) {
            const auto c = input[at];
            std::size_t run = 1;
            while (at + run < input.size() && input[at + run] == c && run < 255) ++run;
            if (run >= 4) { block.insert(block.end(), 4, c); block.push_back(static_cast<std::uint8_t>(run - 4)); }
            else block.insert(block.end(), run, c);
            at += run;
        }
        const auto block_crc = ~crc(input.subspan(start, at - start));
        combined = ((combined << 1) | (combined >> 31)) ^ block_crc;
        encode_block(w, block, block_crc);
    }
    w.bits(end_magic, 48);
    w.bits(combined, 32);
    w.flush();
    return std::move(w.out);
}

} // namespace alchemy::gui::completion_kit::bzip2
