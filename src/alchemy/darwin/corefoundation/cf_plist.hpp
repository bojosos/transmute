#pragma once

// Property lists for the export-fill providers: a host model
// with XML (DTD "-//Apple//DTD PLIST 1.0//EN") and binary ("bplist00")
// encodings, and conversion to and from guest CoreFoundation objects.

#include "alchemy/core_foundation.hpp"
#include "alchemy/process_context.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::export_fill {

struct Plist {
    // uid: NSKeyedArchiver's object references (binary marker 0x8n; XML as
    // a {CF$UID: n} dictionary); not a guest property-list object.
    enum class Kind { string, integer, real, boolean, date, data, array, dictionary, uid };
    Kind kind{Kind::string};
    std::string text;                                   // string (UTF-8)
    std::int64_t integer{};                             // integer; uid
    double real{};                                      // real; date (seconds since 2001-01-01)
    bool boolean{};
    std::vector<std::uint8_t> bytes;                    // data
    std::vector<Plist> items;                           // array
    std::vector<std::pair<std::string, Plist>> entries; // dictionary (keys are strings)

    [[nodiscard]] const Plist* find(std::string_view key) const;
    Plist* find(std::string_view key);
    void set(std::string key, Plist value);
    bool erase(std::string_view key);
};

// kCFPropertyListXMLFormat_v1_0 / kCFPropertyListBinaryFormat_v1_0.
inline constexpr std::uint64_t plist_xml_format = 100, plist_binary_format = 200;

[[nodiscard]] std::string plist_to_xml(const Plist& value);
[[nodiscard]] std::vector<std::uint8_t> plist_to_binary(const Plist& value);
// Parses either encoding; std::nullopt for malformed input. `format` receives
// plist_xml_format or plist_binary_format.
[[nodiscard]] std::optional<Plist> plist_parse(std::span<const std::uint8_t> bytes, std::uint64_t* format = nullptr);

// Guest objects. from_guest fails (runtime_error) for objects that are not
// property-list types; to_guest returns a +1 object.
[[nodiscard]] Plist plist_from_guest(ProcessContext& process, CoreFoundation& cf, std::uint64_t object);
// mutability: kCFPropertyListImmutable 0, MutableContainers 1, MutableContainersAndLeaves 2.
[[nodiscard]] std::uint64_t plist_to_guest(ProcessContext& process, CoreFoundation& cf, const Plist& value, std::uint64_t mutability = 0);

} // namespace alchemy::export_fill
