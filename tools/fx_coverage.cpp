// fx_coverage: static translator coverage audit for ARM64 PE images.
//
//   fx_coverage <image.dll|exe> [--top N] [--words out.txt]
//
// Translates every 4-byte word of the image's executable sections as a
// one-instruction region (core translator plus FP, NEON and system families)
// and reports encodings that still become interpret exits, ranked by how
// often they occur. Data embedded in code sections is decoded too, so treat
// rare entries with care. tools/fx_coverage_names.py adds mnemonics.

#include "fx/translator/translator.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

using namespace transmute::fx;

namespace {

struct Section {
    std::uint32_t rva, size, file_offset, file_size;
    bool executable;
};

class FileCode final : public CodeSource {
public:
    FileCode(const std::vector<std::uint8_t>& file, std::uint64_t base, std::vector<Section> sections)
        : file_(file), base_(base), sections_(std::move(sections)) {}
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

private:
    const std::vector<std::uint8_t>& file_;
    std::uint64_t base_;
    std::vector<Section> sections_;
};

template <typename T> T get(const std::vector<std::uint8_t>& f, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= f.size()) std::memcpy(&v, f.data() + at, sizeof(T));
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fx_coverage <image> [--top N] [--words out.txt]\n");
        return 2;
    }
    std::size_t top = 40;
    const char* words_out = nullptr;
    for (int i = 2; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--top")) top = std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--words")) words_out = argv[++i];
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    const auto pe = get<std::uint32_t>(file, 0x3c);
    if (get<std::uint32_t>(file, pe) != 0x4550 || get<std::uint16_t>(file, pe + 4) != 0xaa64) {
        std::fprintf(stderr, "not an ARM64 PE image\n");
        return 1;
    }
    const auto sections_count = get<std::uint16_t>(file, pe + 6);
    const auto optional_size = get<std::uint16_t>(file, pe + 20);
    const auto optional = pe + 24;
    const auto base = get<std::uint64_t>(file, optional + 24);
    std::vector<Section> sections;
    for (unsigned i = 0; i < sections_count; ++i) {
        const auto h = optional + optional_size + i * 40;
        sections.push_back({get<std::uint32_t>(file, h + 12), get<std::uint32_t>(file, h + 8),
                            get<std::uint32_t>(file, h + 20), get<std::uint32_t>(file, h + 16),
                            (get<std::uint32_t>(file, h + 36) & 0x20000000u) != 0});
    }
    register_fp_emitters();
    register_neon_emitters();
    register_system_emitters();
    FileCode code(file, base, sections);
    TranslateOptions options;
    options.max_instructions = 1;
    options.interrupt_checks = false;

    // Supportedness depends on the encoding; translate each distinct word once
    // (at its first address, for PC-relative forms).
    std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> words;  // word -> {count, first address}
    std::uint64_t total = 0;
    for (const auto& s : sections) {
        if (!s.executable) continue;
        const auto limit = std::min(s.size, s.file_size) & ~3u;
        for (std::uint32_t off = 0; off < limit; off += 4) {
            std::uint32_t w;
            std::memcpy(&w, file.data() + s.file_offset + off, 4);
            auto& entry = words[w];
            if (entry.first++ == 0) entry.second = base + s.rva + off;
            ++total;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::pair<std::uint32_t, std::uint64_t>> unsupported;
    std::uint64_t unsupported_total = 0, errors = 0;
    for (const auto& [word, info] : words) {
        bool interpreted = false;
        try {
            const auto region = translate_region(info.second, code, options);
            interpreted = region.stats.interpreted_instructions != 0;
        } catch (const std::exception&) {
            ++errors;
            interpreted = true;
        }
        if (interpreted) {
            unsupported.emplace_back(word, info.first);
            unsupported_total += info.first;
        }
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::sort(unsupported.begin(), unsupported.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::printf("%s: %llu code words, %zu distinct, %zu distinct unsupported covering %llu words (%.3f%%), "
                "%llu translator errors, %.2f s\n",
                argv[1], static_cast<unsigned long long>(total), words.size(), unsupported.size(),
                static_cast<unsigned long long>(unsupported_total),
                total ? 100.0 * static_cast<double>(unsupported_total) / static_cast<double>(total) : 0.0,
                static_cast<unsigned long long>(errors), seconds);
    for (std::size_t i = 0; i < unsupported.size() && i < top; ++i)
        std::printf("  %08x  %llu\n", unsupported[i].first, static_cast<unsigned long long>(unsupported[i].second));
    if (words_out) {
        std::ofstream out(words_out);
        for (const auto& [w, n] : unsupported) out << std::hex << w << ' ' << std::dec << n << '\n';
    }
    return 0;
}
