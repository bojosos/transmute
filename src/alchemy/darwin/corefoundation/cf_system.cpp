#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/darwin_version.hpp"
#include "alchemy/object_store.hpp"
#include "alchemy/placeholder_exports.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// CoreFoundation exports Squirrel, Mantle and ReactiveObjC import (Electron's
// update frameworks):
//   - _CFCopySystemVersionDictionary and its _kCFSystemVersion*Key constants:
//     the SystemVersion.plist dictionary of Alchemy's one OS profile
//     (alchemy/darwin_version.hpp; no ProductVersionExtra, as on a release);
//   - NSGetSizeAndAlignment (exported by CoreFoundation on arm64): size and
//     alignment of one Objective-C type encoding, by the arm64 (LP64) ABI;
//   - CFBinaryHeap: explicit stops. Only ReactiveObjC's RACTestScheduler
//     uses a binary heap, and nothing implements the CF type yet.
namespace alchemy::export_fill {

// NSGetSizeAndAlignment's parser: the type at `encoding[at]`, with its leading
// qualifiers, a quoted class or field name and a trailing frame offset (method
// signatures) skipped. Returns {size, alignment}; `at` ends past the type.
std::pair<std::uint64_t, std::uint64_t> objc_type_layout(const std::string& encoding, std::size_t& at) {
    const auto fail = [&encoding](const char* why) -> std::pair<std::uint64_t, std::uint64_t> {
        throw std::runtime_error("Alchemy NSGetSizeAndAlignment: " + std::string(why) + " in type encoding \"" + encoding + "\"");
    };
    const auto skip_name = [&] {
        if (at < encoding.size() && encoding[at] == '"') {
            const auto end = encoding.find('"', at + 1);
            if (end == std::string::npos) fail("an unterminated name");
            at = end + 1;
        }
    };
    const auto skip_offset = [&] { while (at < encoding.size() && encoding[at] >= '0' && encoding[at] <= '9') ++at; };
    const auto align = [](std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; };
    while (at < encoding.size() && std::string_view("rnNoORVA").find(encoding[at]) != std::string_view::npos) ++at;   // qualifiers
    if (at >= encoding.size()) return fail("a missing type");
    const auto code = encoding[at++];
    std::pair<std::uint64_t, std::uint64_t> result;
    switch (code) {
    case 'c': case 'C': case 'B': result = {1, 1}; break;
    case 's': case 'S': result = {2, 2}; break;
    case 'i': case 'I': case 'l': case 'L': case 'f': result = {4, 4}; break;   // 'l' is a 32-bit long; LP64 long encodes as 'q'
    case 'q': case 'Q': case 'd': case 'D': result = {8, 8}; break;          // long double is a double on arm64
    case 'v': result = {0, 1}; break;
    case '*': case ':': case '#': result = {8, 8}; break;
    case '@':
        result = {8, 8};
        if (at < encoding.size() && encoding[at] == '?') ++at;          // a block
        else skip_name();                                                // @"NSString"
        break;
    case '^':
        result = {8, 8};
        if (at < encoding.size() && encoding[at] == '?') ++at;          // a function pointer
        else { auto inner = at; static_cast<void>(objc_type_layout(encoding, inner)); at = inner; }
        break;
    case '[': {
        std::uint64_t count = 0;
        while (at < encoding.size() && encoding[at] >= '0' && encoding[at] <= '9') count = count * 10 + static_cast<std::uint64_t>(encoding[at++] - '0');
        const auto element = objc_type_layout(encoding, at);
        if (at >= encoding.size() || encoding[at] != ']') return fail("an unterminated array");
        ++at;
        result = {element.first * count, element.second};
        break;
    }
    case '{': case '(': {
        const auto close = code == '{' ? '}' : ')';
        while (at < encoding.size() && encoding[at] != '=' && encoding[at] != close) ++at;   // the tag
        std::uint64_t size = 0, alignment = 1;
        if (at < encoding.size() && encoding[at] == '=') {
            ++at;
            while (at < encoding.size() && encoding[at] != close) {
                skip_name();
                const auto field = objc_type_layout(encoding, at);
                alignment = std::max(alignment, field.second);
                size = code == '{' ? align(size, field.second) + field.first : std::max(size, field.first);
            }
        }
        if (at >= encoding.size()) return fail(code == '{' ? "an unterminated structure" : "an unterminated union");
        ++at;
        result = {align(size, alignment), alignment};
        break;
    }
    case 'b': return fail("a bit field");
    default: return fail("an unknown type code");
    }
    skip_offset();
    return result;
}

void install_cf_system(Provider& p, CoreFoundation& cf) {
    auto& process = p.process();
    const std::string cf_library(core_foundation_library);

    // SystemVersion.plist's keys and the dictionary of Alchemy's OS profile. The keys
    // are SPI whose C names begin with an underscore (_kCFSystemVersionProductNameKey),
    // so their Mach-O symbols have two.
    const std::array<std::pair<const char*, const char*>, 6> keys{{
        {"__kCFSystemVersionProductNameKey", "ProductName"}, {"__kCFSystemVersionProductVersionKey", "ProductVersion"},
        {"__kCFSystemVersionProductVersionExtraKey", "ProductVersionExtra"},
        {"__kCFSystemVersionBuildVersionKey", "ProductBuildVersion"}, {"__kCFSystemVersionProductCopyrightKey", "ProductCopyright"},
        {"__kCFSystemVersionProductUserVisibleVersionKey", "ProductUserVisibleVersion"}}};
    for (const auto& [name, value] : keys) {
        if (process.resolve(cf_library, name)) continue;
        const auto string = cf.string(value);
        if (const auto record = process.objects().find(string)) record->immortal = true;
        const auto cell = process.allocate_guest(8);
        process.main_engine().write64(cell, string);
        process.install_export(cf_library, name, {cell, ExportKind::data});
    }
    // CFDictionaryRef _CFCopySystemVersionDictionary(void) (+1)
    p.function(cf_library, "__CFCopySystemVersionDictionary", [&cf, p](CpuState&, GuestMemory&) -> std::uint64_t {
        const std::string version(macos_product_version), build(darwin_build);
        const std::array<std::pair<std::string_view, std::string_view>, 5> values{{
            {"ProductName", "macOS"}, {"ProductVersion", version}, {"ProductUserVisibleVersion", version},
            {"ProductBuildVersion", build}, {"ProductCopyright", "Alchemy compatibility profile"}}};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
        for (const auto& [key, value] : values) entries.emplace_back(cf.string(key), cf.string(value));
        const auto dictionary = cf.dictionary(entries);
        for (const auto& [key, value] : entries) for (const auto object : {key, value}) static_cast<void>(p.call(core_foundation_library, "_CFRelease", {object}));
        return dictionary;
    });

    // const char* NSGetSizeAndAlignment(const char* typePtr, NSUInteger* sizep, NSUInteger* alignp)
    p.function(cf_library, "_NSGetSizeAndAlignment", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto encoding = guest_string(m, s.x[0], 1 << 16);
        std::size_t at = 0;
        const auto [size, alignment] = objc_type_layout(encoding, at);
        if (s.x[1]) m.write64(s.x[1], size);
        if (s.x[2]) m.write64(s.x[2], alignment);
        return s.x[0] + at;
    });

    // CFBinaryHeap: explicit stops.
    for (const auto* name : {"_CFBinaryHeapCreate", "_CFBinaryHeapAddValue", "_CFBinaryHeapGetMinimumIfPresent",
                             "_CFBinaryHeapRemoveMinimumValue", "_CFBinaryHeapGetCount", "_CFBinaryHeapGetMinimum",
                             "_CFBinaryHeapRemoveAllValues"}) {
        if (process.resolve(cf_library, name)) continue;
        p.function(cf_library, name, [name](CpuState&, GuestMemory&) -> std::uint64_t {
            throw std::runtime_error(std::string("Alchemy: ") + (name + 1) + " is unsupported (no CFBinaryHeap type)");
        });
        mark_placeholder_export(process, cf_library, name);
    }
}

} // namespace alchemy::export_fill
