#pragma once

// NSNumberFormatter's formatting and parsing core (host C++, no guest
// objects), so it is testable on its own: styles none / decimal / currency /
// percent, integer and fraction digit limits, grouping, prefixes and
// suffixes, the multiplier, the seven NSNumberFormatterRoundingMode values,
// padding to a format width, a zero symbol, infinity symbols and a
// rounding increment. Scientific, spell-out and
// ordinal styles are not provided (formatters.cpp fails explicitly).

#include <cstdint>
#include <optional>
#include <string>

namespace alchemy::gui::completion_kit::numbers {

enum class Style : std::int64_t { none = 0, decimal = 1, currency = 2, percent = 3, scientific = 4, spell_out = 5,
                                  ordinal = 6, currency_iso = 8, currency_plural = 9, currency_accounting = 10 };
enum class Rounding : std::int64_t { ceiling = 0, floor = 1, down = 2, up = 3, half_even = 4, half_down = 5, half_up = 6 };
enum class Padding : std::int64_t { before_prefix = 0, after_prefix = 1, before_suffix = 2, after_suffix = 3 };

struct Format {
    Style style{Style::none};
    std::int64_t min_integer{0}, max_integer{42}, min_fraction{0}, max_fraction{0};
    bool grouping{}, allows_floats{true};
    std::int64_t grouping_size{3};
    std::u16string decimal_separator{u"."}, grouping_separator{u","};
    std::u16string positive_prefix, positive_suffix, negative_prefix{u"-"}, negative_suffix;
    std::optional<std::u16string> zero_symbol;
    double multiplier{1};
    Rounding rounding{Rounding::half_even};
    std::int64_t format_width{0};
    std::u16string padding{u" "};
    Padding padding_position{Padding::before_prefix};
    // -positiveInfinitySymbol / -negativeInfinitySymbol when set (otherwise
    // the affixes around U+221E).
    std::optional<std::u16string> positive_infinity, negative_infinity;
    // -roundingIncrement: values round to a multiple of it (0: none).
    double rounding_increment{0};

    // NSNumberFormatter -setNumberStyle: (10.4 behaviour): resets the digit,
    // grouping, multiplier and affix attributes for the style.
    void apply_style(Style value, const std::u16string& currency_symbol);
};

[[nodiscard]] std::u16string format(const Format& format, double value);
// nullopt when the string is not a number in this format.
[[nodiscard]] std::optional<double> parse(const Format& format, const std::u16string& text);

} // namespace alchemy::gui::completion_kit::numbers
