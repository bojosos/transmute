#pragma once

// JSON (RFC 8259) for NSJSONSerialization: a host value tree, a strict
// parser (fragments only when allowed; JSON5 is not provided) and a writer
// (compact or pretty-printed with two-space indentation like Foundation's,
// keys optionally sorted, "/" escaped unless asked not to). Parse errors
// carry the byte offset; the writer rejects NaN and infinities.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace alchemy::gui::completion_kit::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::u16string, Value>>;   // keys in document order

struct Null {};
struct Value {
    // An integer keeps its exact value; a real is a double.
    std::variant<Null, bool, std::int64_t, double, std::u16string, std::shared_ptr<Array>, std::shared_ptr<Object>> data;
};

struct ParseError { std::size_t offset{}; std::string message; };

[[nodiscard]] std::variant<Value, ParseError> parse(std::string_view utf8, bool allow_fragments);

struct WriteOptions { bool pretty{}, sorted_keys{}, escape_slashes{true}; };
// nullopt when a number is not finite.
[[nodiscard]] std::optional<std::string> write(const Value& value, const WriteOptions& options);

} // namespace alchemy::gui::completion_kit::json
