#pragma once

// Compiled nib files (owner: AppKit). Interface Builder compiles .xib files
// into "NIBArchive" files (the format QLab's Contents/Resources/*.nib use):
// a flat table of objects, each a class name plus a run of key/value pairs
// whose values are scalars, byte strings or references to other objects.
// Object 0 is the archive root; its "IB.objectdata" value is the
// NSIBObjectData describing the nib's object graph (the File's Owner
// "NSRoot", "NSVisibleWindows", "NSConnections", the parent links
// "NSObjectsKeys"/"NSObjectsValues").
//
// Format (all integers little-endian):
//   "NIBArchive" header, then ten uint32: format version (1), coder version
//   (9 or 10), object count, object table offset, key count, key table
//   offset, value count, value table offset, class-name count, class-name
//   table offset.
//   Variable-length integers ("varint"): 7 bits per byte, least significant
//   group first, the high bit set on the LAST byte.
//   Object: varint class index, varint first value index, varint value count.
//   Key: varint length, UTF-8 bytes.
//   Value: varint key index, one type byte, then by type: 0 int8, 1 int16,
//     2 int32, 3 int64, 4 false, 5 true (no data), 6 float32, 7 float64,
//     8 data (varint length + bytes), 9 nil (no data), 10 object reference
//     (uint32 object index).
//   Class name: varint length (including the NUL), varint count of extra
//     int32 values (fallback class indexes), those int32 values, the name.
// Collections (NSArray, NSSet, NSDictionary...) list their elements as
// values keyed "UINibEncoderEmptyKey" (dictionaries: key, value, key, ...).
//
// This parser is project-authored from the format description above; it
// reads untrusted bytes and fails with std::runtime_error on any
// inconsistency (no partial archives).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::nib {

enum class ValueType : std::uint8_t {
    int8 = 0, int16 = 1, int32 = 2, int64 = 3, false_ = 4, true_ = 5, float32 = 6, float64 = 7, data = 8, nil = 9, object = 10,
};

struct Value {
    std::uint32_t key{};                  // index into Archive::keys
    ValueType type{ValueType::nil};
    std::int64_t integer{};               // int8..int64, booleans (1/0), object index
    double real{};                        // float32 / float64
    std::vector<std::uint8_t> data;       // data
    [[nodiscard]] bool is_object() const { return type == ValueType::object; }
    [[nodiscard]] bool is_integer() const { return type <= ValueType::int64; }
    [[nodiscard]] bool is_bool() const { return type == ValueType::true_ || type == ValueType::false_; }
    [[nodiscard]] bool is_real() const { return type == ValueType::float32 || type == ValueType::float64; }
};

struct Object {
    std::uint32_t class_index{};
    std::uint32_t first_value{}, value_count{};
};

struct Archive {
    std::uint32_t format_version{}, coder_version{};
    std::vector<std::string> keys;
    std::vector<std::string> class_names;
    std::vector<Value> values;
    std::vector<Object> objects;

    [[nodiscard]] static bool is_nib_archive(std::span<const std::uint8_t> bytes);
    [[nodiscard]] static Archive parse(std::span<const std::uint8_t> bytes);
    // Keyed-archive nibs (nib_keyed.cpp): an NSKeyedArchiver binary property
    // list ("bplist00"; older Interface Builder output, e.g. Sparkle's
    // keyedobjects.nib) converted to the same model, so loading is shared.
    // Each $objects entry becomes an object (plain strings NSString with
    // NS.bytes, numbers NSNumber, data NSData); inline strings (encoded rects
    // and points) become NSString objects; NSArray / NSSet / NSDictionary
    // elements (NS.objects / NS.keys) become UINibEncoderEmptyKey values.
    [[nodiscard]] static bool is_keyed_archive(std::span<const std::uint8_t> bytes);
    [[nodiscard]] static Archive from_keyed_archive(std::span<const std::uint8_t> bytes);
    // Either format.
    [[nodiscard]] static Archive parse_any(std::span<const std::uint8_t> bytes) {
        return is_keyed_archive(bytes) ? from_keyed_archive(bytes) : parse(bytes);
    }
    [[nodiscard]] std::vector<std::uint8_t> encode() const;   // the same format (tests, fixtures)

    [[nodiscard]] const std::string& class_name(std::uint32_t object) const;
    [[nodiscard]] std::span<const Value> values_of(std::uint32_t object) const;
    // The first value of `object` keyed `key`.
    [[nodiscard]] const Value* find(std::uint32_t object, std::string_view key) const;
    // Every value keyed `key` (collection elements).
    [[nodiscard]] std::vector<const Value*> all(std::uint32_t object, std::string_view key) const;
    [[nodiscard]] const std::string& key_name(const Value& value) const { return keys.at(value.key); }
    // Object references of a collection object (UINibEncoderEmptyKey values).
    [[nodiscard]] std::vector<std::uint32_t> elements(std::uint32_t object) const;
    // NSString / NSMutableString bytes of `object` (NS.bytes), nullopt for others.
    [[nodiscard]] std::optional<std::string> string_value(std::uint32_t object) const;
    // The string of the object `key` refers to.
    [[nodiscard]] std::optional<std::string> string_at(std::uint32_t object, std::string_view key) const;
    [[nodiscard]] std::optional<std::uint32_t> object_at(std::uint32_t object, std::string_view key) const;
};

// The interface-builder object graph of an archive.
struct Connection {
    enum class Kind { outlet, action, other } kind{Kind::other};
    std::uint32_t object{};               // the connector object
    std::string connector_class;
    std::optional<std::uint32_t> source, destination;
    std::string label;                    // outlet name / action selector
};
struct Graph {
    std::uint32_t object_data{};          // the NSIBObjectData object
    std::uint32_t root{};                 // NSRoot (the File's Owner placeholder)
    std::vector<std::uint32_t> visible_windows;
    std::vector<Connection> connections;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> parents;   // NSObjectsKeys[i] -> NSObjectsValues[i]
    std::vector<std::uint32_t> top_level; // objects whose parent is the root
    // NSAccessibilityConnectors: NSNibAXAttributeConnector objects, kept
    // apart from NSConnections by Interface Builder. (NSOidsKeys/Values and
    // NSAccessibilityOidsKeys/Values are Interface Builder's object ids and
    // have no runtime meaning.)
    std::vector<Connection> accessibility_connections;
};
[[nodiscard]] Graph read_graph(const Archive& archive);

// "{x, y}" / "{{x, y}, {w, h}}" (NSStringFromPoint / NSStringFromRect).
[[nodiscard]] std::optional<std::vector<double>> parse_numbers(std::string_view text);

} // namespace alchemy::gui::nib
