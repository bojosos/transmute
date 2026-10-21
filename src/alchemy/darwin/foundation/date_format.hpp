#pragma once

// NSDateFormatter's core (host C++, testable without a guest): Gregorian
// civil dates from NSDate's reference time (seconds since 2001-01-01 UTC)
// plus a zone offset, Unicode TR35 patterns for formatting and parsing, and
// the en_US date and time style patterns.
// Pattern letters: G y yy yyyy u M MM MMM MMMM MMMMM L.. d dd D E EEE EEEE
// EEEEE c.. a h hh H HH k K m mm s ss S.. (fraction) Z ZZ ZZZ ZZZZ ZZZZZ x.. X..
// z.. (as "GMT+h[:mm]"), and 'quoted' literals. Other letters fail (format
// and parse throw std::runtime_error). Names are English.

#include <cstdint>
#include <optional>
#include <string>

namespace alchemy::gui::completion_kit::dates {

enum class Style : std::int64_t { none = 0, short_ = 1, medium = 2, long_ = 3, full = 4 };

struct Civil {
    std::int64_t year{}, month{}, day{}, hour{}, minute{}, second{};
    double fraction{};          // [0, 1)
    std::int64_t weekday{};     // 1 Sunday ... 7 Saturday
    std::int64_t day_of_year{};
};

[[nodiscard]] Civil civil(double reference_seconds, std::int64_t offset_seconds);
[[nodiscard]] double reference_seconds(const Civil& value, std::int64_t offset_seconds);

// The en_US pattern for the styles ("" when both are none).
[[nodiscard]] std::string style_pattern(Style date, Style time);

// The en_US pattern for a TR35 skeleton (+[NSDateFormatter dateFormatFromTemplate:options:locale:]): the fields of
// the skeleton in en_US order and punctuation, numeric fields at the skeleton's width ("yyyyMMdd" -> "MM/dd/yyyy",
// "yMMMEd" -> "EEE, MMM d, y", "jmm" -> "h:mm a", "Hms" -> "HH:mm:ss"). Skeleton letters: G y Y u M L d E c e
// j J C h K H k m s S a b B z Z O v V X x; others throw std::runtime_error.
[[nodiscard]] std::u16string pattern_from_template(const std::u16string& skeleton);

[[nodiscard]] std::u16string format(const std::u16string& pattern, double reference, std::int64_t offset_seconds);
// The reference time, or nullopt when the text does not match the pattern.
// Missing fields default to 2000-01-01 00:00:00 in the given offset (as
// NSDateFormatter's default date), unless the pattern names them.
// `default_reference` (-[NSDateFormatter defaultDate]) supplies the fields
// the pattern does not name instead of 2000-01-01 00:00:00.
[[nodiscard]] std::optional<double> parse(const std::u16string& pattern, const std::u16string& text, std::int64_t offset_seconds,
                                          std::optional<double> default_reference = std::nullopt);

} // namespace alchemy::gui::completion_kit::dates
