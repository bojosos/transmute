#pragma once

// Property lists on the host side (no guest code): the session launchd reads
// launchd.plist files and job dictionaries with it, and hands dictionaries
// back as XML. Formats: XML (version 1.0) and binary (bplist00), from their
// public descriptions. OpenStep text plists are not read.

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::launchd {

struct PlistError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Value {
    enum class Kind : std::uint8_t { string, integer, real, boolean, date, data, array, dictionary };
    Kind kind{Kind::string};
    std::string text;                                       // string; date as ISO 8601 (UTC, "Z")
    std::int64_t integer{};
    double real{};
    bool boolean{};
    std::vector<std::uint8_t> data;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> dictionary;  // in file order; keys unique

    static Value of_string(std::string value);
    static Value of_integer(std::int64_t value);
    static Value of_boolean(bool value);
    static Value of_array(std::vector<Value> values = {});
    static Value of_dictionary();

    [[nodiscard]] bool is(Kind which) const noexcept { return kind == which; }
    // Dictionary member; nullptr when absent or when this is no dictionary.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    // Sets (replacing) a dictionary member.
    void set(std::string key, Value value);
    void erase(std::string_view key);
};

// Parses XML or binary plist bytes; throws PlistError.
[[nodiscard]] Value parse_plist(std::span<const std::uint8_t> bytes);
[[nodiscard]] Value read_plist_file(const std::string& host_path);
// XML plist text (with the XML declaration and DOCTYPE).
[[nodiscard]] std::string to_xml(const Value& value);
// The value as launchctl prints a job ("print" and "list <label>" style):
// nested braces, one member per line.
[[nodiscard]] std::string to_text(const Value& value, int indent = 0);

} // namespace alchemy::launchd
