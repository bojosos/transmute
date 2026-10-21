// fx_neon_cost: host-code cost of SIMD&FP instruction forms (owner: neon/fp).
//
//   fx_neon_cost enumerate [--threads N] > supported.txt
//       Translates the SIMD&FP encoding space (data processing, bits 28:25 =
//       x111, with Rd in {0, 8, 16, 24} and Rn = 1; loads/stores, bits 27:25 =
//       110 with bit 26 = V, Rt = 0 and Rn = 1) as one-instruction regions and
//       prints every word the translator handles without an interpret exit.
//   fx_neon_cost cost <words.txt> > cost.txt
//       For every word (hex, first token of each line) prints two lines:
//         <word> unpinned <entry> <hex code>    one-instruction region
//         <word> pinned <body> <pins> <hex code>  loop `L: word; B L`
//       plus `00000000 baseline <entry> <hex>` for a NOP region (its exit
//       sequence is subtracted from unpinned counts). tools/fx_neon_cost.py
//       disassembles the code, follows the common path, runs llvm-mca and
//       joins the result with static/dynamic frequencies.
//
// Both modes honour TRANSMUTE_FX_NEON / TRANSMUTE_FX_FP knobs (read once), so
// running the tool with the "old" knobs gives the before-numbers.

#include "fx/translator/translator.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace transmute::fx;

namespace {

constexpr std::uint64_t kBase = 0x0000'0001'4000'0000ULL;

class WordsCode final : public CodeSource {
public:
    explicit WordsCode(std::vector<std::uint32_t> w) : words_(std::move(w)) {}
    bool read(std::uint64_t address, std::uint32_t& word) const override {
        if (address < kBase || ((address - kBase) & 3) != 0) return false;
        const auto i = (address - kBase) / 4;
        if (i >= words_.size()) return false;
        word = words_[i];
        return true;
    }
    bool function_bounds(std::uint64_t, std::uint64_t& begin, std::uint64_t& end) const override {
        begin = kBase;
        end = kBase + words_.size() * 4;
        return true;
    }

private:
    std::vector<std::uint32_t> words_;
};

bool supported(std::uint32_t word) {
    // The word is followed by a NOP so the region has a next instruction.
    WordsCode code({word, 0xd503201fu});
    TranslateOptions o;
    o.max_instructions = 1;
    o.interrupt_checks = false;
    try {
        const auto r = translate_region(kBase, code, o);
        return r.stats.interpreted_instructions == 0 && r.stats.guest_instructions == 1;
    } catch (...) {
        return false;
    }
}

void hex(std::string& out, const std::vector<std::uint8_t>& bytes) {
    static const char* d = "0123456789abcdef";
    for (const auto b : bytes) {
        out.push_back(d[b >> 4]);
        out.push_back(d[b & 15]);
    }
}

int enumerate(unsigned threads) {
    std::vector<std::uint32_t> space;
    // Data processing: bits 28:25 = x111; free bits 31:29 and 24:5 except Rn.
    for (std::uint32_t hi = 0; hi < 8; ++hi)
        for (std::uint32_t b28 = 0; b28 < 2; ++b28)
            for (std::uint32_t mid = 0; mid < (1u << 15); ++mid)
                for (std::uint32_t rd : {0u, 8u, 16u, 24u})
                    space.push_back((hi << 29) | (b28 << 28) | (0x7u << 25) | (mid << 10) | (1u << 5) | rd);
    // Loads/stores with V = 1: bits 27:25 = 1x0 with bit 26 = 1.
    for (std::uint32_t top = 0; top < 16; ++top)
        for (std::uint32_t mid = 0; mid < (1u << 15); ++mid)
            space.push_back((top << 28) | (0x6u << 25) | (mid << 10) | (1u << 5));
    std::vector<std::uint8_t> ok(space.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (;;) {
                const std::size_t i = next.fetch_add(256);
                if (i >= space.size()) return;
                for (std::size_t j = i; j < std::min(space.size(), i + 256); ++j) ok[j] = supported(space[j]);
            }
        });
    for (auto& t : pool) t.join();
    std::size_t n = 0;
    for (std::size_t i = 0; i < space.size(); ++i)
        if (ok[i]) {
            std::printf("%08x\n", space[i]);
            ++n;
        }
    std::fprintf(stderr, "enumerate: %zu of %zu encodings supported\n", n, space.size());
    return 0;
}

int cost(const char* path) {
    std::ifstream in(path);
    std::vector<std::uint32_t> words;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        words.push_back(static_cast<std::uint32_t>(std::strtoul(line.c_str(), nullptr, 16)));
    }
    TranslateOptions one;
    one.max_instructions = 1;
    one.interrupt_checks = false;
    TranslateOptions loop;
    loop.interrupt_checks = false;
    const auto emit_unpinned = [&](std::uint32_t word, const char* tag) {
        WordsCode code({word, 0xd503201fu});
        const auto r = translate_region(kBase, code, one);
        std::string out;
        hex(out, r.code);
        std::printf("%08x %s %u %u %s\n", word, tag, r.entries.empty() ? 0u : r.entries[0].offset,
                    r.stats.interpreted_instructions, out.c_str());
    };
    emit_unpinned(0xd503201fu, "baseline");
    for (const auto word : words) {
        try {
            emit_unpinned(word, "unpinned");
            // L: word; B L
            WordsCode code({word, 0x17ffffffu});
            const auto r = translate_region(kBase, code, loop);
            unsigned vpins = 0, gpins = 0;
            for (const auto& p : r.pins) (p.vector ? vpins : gpins) += 1;
            std::string out;
            hex(out, r.code);
            std::printf("%08x pinned %u %u %u %u %s\n", word, r.entries.empty() ? 0u : r.entries[0].offset,
                        r.stats.interpreted_instructions, vpins, gpins, out.c_str());
            // Known-zero sources: `L: LDR S<r>, [X28] (r = every register field); word; B L`
            // and the same loop without the word (subtracted by the script).
            if (((word >> 25) & 7u) == 7u) {
                std::vector<std::uint32_t> prefix;
                for (const unsigned f : {0u, 5u, 10u, 16u}) {
                    const std::uint32_t ldr = 0xbd400380u | ((word >> f) & 31u);
                    if (std::find(prefix.begin(), prefix.end(), ldr) == prefix.end()) prefix.push_back(ldr);
                }
                for (int with = 1; with >= 0; --with) {
                    std::vector<std::uint32_t> body = prefix;
                    if (with) body.push_back(word);
                    body.push_back(0x14000000u | ((0u - static_cast<std::uint32_t>(body.size())) & 0x03ffffffu));
                    WordsCode zc(body);
                    const auto z = translate_region(kBase, zc, loop);
                    std::string zout;
                    hex(zout, z.code);
                    std::printf("%08x %s %u %u 0 0 %s\n", word, with ? "pinz" : "pinzbase",
                                z.entries.empty() ? 0u : z.entries[0].offset, z.stats.interpreted_instructions,
                                zout.c_str());
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%08x: %s\n", word, e.what());
        }
    }
    return 0;
}

// ---- whole functions of a PE image (in-situ cost) -----------------------------------

struct Section {
    std::uint32_t rva, size, file_offset, file_size;
    bool executable;
};

class FileCode final : public CodeSource {
public:
    FileCode(std::vector<std::uint8_t> file, std::uint64_t base, std::vector<Section> s)
        : file_(std::move(file)), base_(base), sections_(std::move(s)) {}
    bool read(std::uint64_t address, std::uint32_t& word) const override {
        if (address < base_) return false;
        const auto rva = address - base_;
        for (const auto& s : sections_) {
            if (!s.executable || rva < s.rva || rva + 4 > s.rva + std::min(s.size, s.file_size)) continue;
            std::memcpy(&word, file_.data() + s.file_offset + (rva - s.rva), 4);
            return true;
        }
        return false;
    }
    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override {
        for (const auto& [b, e] : functions)
            if (address >= base_ + b && address < base_ + e) {
                begin = base_ + b;
                end = base_ + e;
                return true;
            }
        return false;
    }
    std::uint64_t base() const { return base_; }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> functions;

private:
    std::vector<std::uint8_t> file_;
    std::uint64_t base_;
    std::vector<Section> sections_;
};

template <typename T> T get(const std::vector<std::uint8_t>& f, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= f.size()) std::memcpy(&v, f.data() + at, sizeof(T));
    return v;
}

// fx_neon_cost regions <image> <rva> <size> [<rva> <size> ...]: translates the
// region at each function start (bounds = [rva, rva + size)) as the JIT would
// (tier 1, eager) and prints `<rva> <guest instructions> <hex code>`.
int regions(int argc, char** argv) {
    std::ifstream in(argv[2], std::ios::binary);
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    const auto pe = get<std::uint32_t>(file, 0x3c);
    if (get<std::uint32_t>(file, pe) != 0x4550) return 1;
    const auto count = get<std::uint16_t>(file, pe + 6);
    const auto optional_size = get<std::uint16_t>(file, pe + 20);
    const auto base = get<std::uint64_t>(file, pe + 24 + 24);
    std::vector<Section> sections;
    for (unsigned i = 0; i < count; ++i) {
        const auto h = pe + 24 + optional_size + i * 40;
        sections.push_back({get<std::uint32_t>(file, h + 12), get<std::uint32_t>(file, h + 8),
                            get<std::uint32_t>(file, h + 20), get<std::uint32_t>(file, h + 16),
                            (get<std::uint32_t>(file, h + 36) & 0x20000000u) != 0});
    }
    FileCode code(std::move(file), base, std::move(sections));
    for (int i = 3; i + 1 < argc; i += 2) {
        const auto rva = static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 16));
        const auto size = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 16));
        code.functions.emplace_back(rva, rva + size);
    }
    TranslateOptions o;
    for (const auto& [b, e] : code.functions) {
        try {
            const auto r = translate_region(code.base() + b, code, o);
            std::string out;
            hex(out, r.code);
            std::printf("%x %u %u %s\n", b, r.stats.guest_instructions, r.stats.interpreted_instructions, out.c_str());
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "%x: %s\n", b, ex.what());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fx_neon_cost enumerate [--threads N] | cost <words.txt>\n");
        return 2;
    }
    register_fp_emitters();
    register_neon_emitters();
    register_system_emitters();
    if (!std::strcmp(argv[1], "enumerate")) {
        unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        for (int i = 2; i + 1 < argc; ++i)
            if (!std::strcmp(argv[i], "--threads")) threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        return enumerate(threads);
    }
    if (!std::strcmp(argv[1], "cost") && argc >= 3) return cost(argv[2]);
    if (!std::strcmp(argv[1], "regions") && argc >= 5) return regions(argc, argv);
    std::fprintf(stderr, "unknown mode\n");
    return 2;
}
