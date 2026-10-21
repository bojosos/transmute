#pragma once

// NSDatePicker's text: the date picker elements (NSDatePickerElementFlags)
// formatted the en_US way (M/d/yyyy, h:mm[:ss] a), the fields a click
// selects, and the per-field editing of the text-field style: arrow keys
// and the stepper wrap a field inside its range without carrying into the
// next one, and typed digits fill a field. Host C++, testable without a guest.

#include <cstdint>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::controls::date_elements {

// NSDatePickerElementFlags.
inline constexpr std::uint64_t hour_minute = 0x000c, hour_minute_second = 0x000e, time_zone = 0x0010, year_month = 0x00c0,
    year_month_day = 0x00e0, era = 0x0100;

// A civil date and time (Gregorian, local zone).
struct Civil { std::int64_t year{2001}, month{1}, day{1}, hour{}, minute{}, second{}; };

struct Field {
    enum Kind : std::int64_t { month, day, year, hour, minute, second, am_pm };
    Kind kind{};
    std::size_t start{}, length{};   // UTF-16 units of the formatted text
};
struct Formatted {
    std::u16string text;
    std::vector<Field> fields;
};

[[nodiscard]] std::int64_t days_in_month(std::int64_t year, std::int64_t month);
// The elements' text; time-zone and era elements are not shown (the
// control refuses them).
[[nodiscard]] Formatted format(std::uint64_t elements, const Civil& value);
// The field at a text index (the one containing it, else the last one
// before it, else the first); -1 when there are none.
[[nodiscard]] std::int64_t field_at(const Formatted& formatted, std::size_t index);
// One step of a field (up +1, down -1), wrapping within the field.
[[nodiscard]] Civil step(Civil value, Field::Kind field, std::int64_t delta);
// A typed digit: `pending` is the field's digits so far (0: none). The
// field takes pending * 10 + digit when that is valid, else starts over
// with the digit; `complete` when no further digit could follow.
struct Typed { Civil value; std::int64_t pending{}; bool complete{}; };
[[nodiscard]] Typed type_digit(Civil value, Field::Kind field, std::int64_t pending, int digit);

} // namespace alchemy::gui::completion_kit::controls::date_elements
