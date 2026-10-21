#pragma once

// CGEvent records (owner: w2 events; CoreGraphics' CGEvent objects live in
// src/alchemy/darwin/coregraphics/graphics_gaps.cpp).
//
// A Record is what a CGEventRef holds: its type, location (global display
// coordinates, origin at the top-left of the primary display), flags,
// timestamp (mach absolute time, nanoseconds under Alchemy) and integer
// fields. encode/decode are the bytes of CGEventCreateData /
// CGEventCreateFromData: "ALCE", version 1, type, field count, x, y, flags,
// timestamp, then (field, value) pairs, little-endian. AppKit converts
// NSEvents to and from CGEvents through that pair (-[NSEvent CGEvent],
// +[NSEvent eventWithCGEvent:]) without reaching into CoreGraphics' objects.

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace alchemy::gui::cg_event {

// CGEventField values (CGEventTypes.h) that AppKit sets or reads.
namespace field {
inline constexpr std::uint32_t mouse_click_state = 1, mouse_button_number = 3, mouse_delta_x = 4, mouse_delta_y = 5,
    mouse_subtype = 7, keyboard_autorepeat = 8, keyboard_keycode = 9, keyboard_type = 10, scroll_delta_axis1 = 11,
    scroll_delta_axis2 = 12, scroll_is_continuous = 88, scroll_point_delta_axis1 = 96, scroll_point_delta_axis2 = 97,
    scroll_phase = 99, scroll_momentum_phase = 123;
} // namespace field

struct Record {
    std::uint32_t type{};                     // CGEventType (the NSEventType numbers)
    double x{}, y{};
    std::uint64_t flags{};                    // CGEventFlags (the NSEventModifierFlags bits)
    std::uint64_t timestamp{};
    std::map<std::uint32_t, std::int64_t> fields;
};

inline constexpr std::uint32_t magic = 0x414c4345;   // 'ALCE'
inline constexpr std::uint32_t version = 1;
inline constexpr std::size_t header_size = 48, field_size = 16;

inline std::vector<std::uint8_t> encode(const Record& record) {
    std::vector<std::uint8_t> bytes;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    };
    const auto count = static_cast<std::uint32_t>(record.fields.size());
    put(&magic, 4); put(&version, 4); put(&record.type, 4); put(&count, 4);
    put(&record.x, 8); put(&record.y, 8); put(&record.flags, 8); put(&record.timestamp, 8);
    for (const auto& [id, number] : record.fields) {
        const std::uint64_t wide = id;
        put(&wide, 8); put(&number, 8);
    }
    return bytes;
}

// nullopt: not a record of this format and version.
inline std::optional<Record> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_size) return std::nullopt;
    std::uint32_t tag{}, format{}, count{};
    std::memcpy(&tag, bytes.data(), 4);
    std::memcpy(&format, bytes.data() + 4, 4);
    std::memcpy(&count, bytes.data() + 12, 4);
    if (tag != magic || format != version || bytes.size() != header_size + count * std::uint64_t{field_size}) return std::nullopt;
    Record record;
    std::memcpy(&record.type, bytes.data() + 8, 4);
    std::memcpy(&record.x, bytes.data() + 16, 8);
    std::memcpy(&record.y, bytes.data() + 24, 8);
    std::memcpy(&record.flags, bytes.data() + 32, 8);
    std::memcpy(&record.timestamp, bytes.data() + 40, 8);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint64_t id{};
        std::int64_t number{};
        std::memcpy(&id, bytes.data() + header_size + index * field_size, 8);
        std::memcpy(&number, bytes.data() + header_size + index * field_size + 8, 8);
        record.fields[static_cast<std::uint32_t>(id)] = number;
    }
    return record;
}

} // namespace alchemy::gui::cg_event
