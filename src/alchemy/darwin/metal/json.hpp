#pragma once

// A small JSON document model for the AIR translator's C ABI (air_translator.hpp),
// whose plain data travels as JSON text: parse, read, build and write.
// Numbers keep their integer value exactly when they are integers (u64 or i64)
// beside the double; objects keep their members in order.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::metal::json {

class Value {
public:
    enum class Kind : std::uint8_t { null, boolean, number, string, array, object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool value) : kind_(Kind::boolean), boolean_(value) {}
    Value(double value) : kind_(Kind::number), number_(value) {}
    Value(std::int64_t value);
    Value(std::uint64_t value);
    Value(int value) : Value(static_cast<std::int64_t>(value)) {}
    Value(unsigned value) : Value(static_cast<std::uint64_t>(value)) {}
    Value(std::string value) : kind_(Kind::string), string_(std::move(value)) {}
    Value(std::string_view value) : Value(std::string(value)) {}
    Value(const char* value) : Value(std::string(value)) {}

    [[nodiscard]] static Value array() { Value v; v.kind_ = Kind::array; return v; }
    [[nodiscard]] static Value object() { Value v; v.kind_ = Kind::object; return v; }

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::null; }
    [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::object; }
    [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::array; }
    [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::string; }
    [[nodiscard]] bool is_number() const noexcept { return kind_ == Kind::number; }

    // Typed reads: nullopt when the value is not of that kind (or out of range).
    [[nodiscard]] std::optional<bool> as_bool() const;
    [[nodiscard]] std::optional<std::uint64_t> as_u64() const;
    [[nodiscard]] std::optional<std::int64_t> as_i64() const;
    [[nodiscard]] std::optional<double> as_double() const;
    [[nodiscard]] const std::string* as_string() const { return kind_ == Kind::string ? &string_ : nullptr; }

    // Arrays and objects. Elements and member values are `items()`; an
    // object's names are `names()`, in the same order.
    [[nodiscard]] const std::vector<Value>& items() const noexcept { return items_; }
    [[nodiscard]] std::vector<Value>& items() noexcept { return items_; }
    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    // The member `name` of an object (the last one when repeated); null
    // pointer when absent or not an object.
    [[nodiscard]] const Value* find(std::string_view name) const;
    [[nodiscard]] Value* find(std::string_view name) {
        return const_cast<Value*>(static_cast<const Value&>(*this).find(name));
    }
    // As find(), but a shared null value when absent: chains read safely.
    [[nodiscard]] const Value& operator[](std::string_view name) const;
    [[nodiscard]] const Value& operator[](std::size_t index) const;
    // Sets member `name` (replacing it) and returns it.
    Value& set(std::string_view name, Value value);
    // Appends to an array and returns the element.
    Value& push(Value value);

    // Compact JSON text.
    [[nodiscard]] std::string dump() const;
    void dump(std::string& out) const;

    // Parses one JSON document; throws std::runtime_error on malformed text.
    [[nodiscard]] static Value parse(std::string_view text);

    friend bool operator==(const Value& a, const Value& b);

private:
    friend class Parser;
    Kind kind_{Kind::null};
    bool boolean_{};
    bool integer_{}, negative_{};   // number: an exact integer (magnitude in bits_)
    std::uint64_t bits_{};
    double number_{};
    std::string string_;
    std::vector<std::string> names_;
    std::vector<Value> items_;
};

} // namespace alchemy::metal::json
