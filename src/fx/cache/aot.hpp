#pragma once

// AOT driver internals (owner: cache). aot_translate_image() is this with
// translate_region; tests inject a stub translator.

#include "fx/cache/cache.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace transmute::fx::aot_detail {

using TranslateFn = TranslatedRegion (*)(std::uint64_t guest_pc, const CodeSource& code,
                                         const TranslateOptions& options);

struct Detail {
    std::uint64_t image_base{};
    std::uint32_t size_of_image{};
    std::vector<std::uint32_t> pdata_begins;   // .pdata BeginAddress values in executable sections
    std::vector<std::uint32_t> static_roots;   // sorted, unique
    std::vector<std::uint64_t> covered;        // bit (rva / 4): an entry of some stored region
    std::vector<std::uint32_t> unencodable;    // roots whose region could not be stored
    [[nodiscard]] bool is_covered(std::uint32_t rva) const {
        const std::uint64_t slot = rva / 4;
        return (rva & 3) == 0 && slot / 64 < covered.size() && ((covered[slot / 64] >> (slot % 64)) & 1);
    }
};

std::size_t translate_image(const std::wstring& path, const AotOptions& options, TranslateFn translate,
                            AotStats* stats, Detail* detail, std::wstring* error);

} // namespace transmute::fx::aot_detail
