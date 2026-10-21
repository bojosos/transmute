#pragma once

// Host area "locale" (owner: r2). The rules are in include/alchemy/host/host.hpp. Locale data,
// collation, normalization, case mapping and time zones (ICU or the host NLS).
// Written by r3 for AppKit (cross-owner, agreed with r2): Mapping::linguistic_lower and
// linguistic_upper.
//
// Text is UTF-16 code units (std::u16string, as CFString and NSString keep
// it); names, locale data and formatted values are UTF-8. A locale is named
// by its BCP 47 tag ("en-US", "zh-Hans-CN"): the empty name is the user's
// default locale and "und" the root, locale-independent rules.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::locale {

// ---- Unicode text ----

// Unicode normalization forms: NFC, NFD, NFKC, NFKD.
enum class Form : std::uint8_t { c, d, kc, kd };
// `text` in `form`. invalid_argument when `text` is not valid UTF-16.
[[nodiscard]] Result<std::u16string> normalize(std::u16string_view text, Form form);

// Options of compare, sort_key and find (bits).
enum Option : std::uint32_t {
    ignore_case = 1u << 0,             // case folding, simple
    ignore_case_linguistic = 1u << 1,  // case folding by the locale's linguistic rules
    ignore_nonspace = 1u << 2,         // nonspacing marks do not count
    ignore_diacritics = 1u << 3,       // diacritics do not count, by the locale's linguistic rules
    ignore_width = 1u << 4,            // half- and full-width forms are equal
    ignore_kana_type = 1u << 5,        // hiragana and katakana are equal
    ignore_symbols = 1u << 6,          // symbols and punctuation do not count
    digits_as_numbers = 1u << 7,       // digit runs order by value
};

// Collation in `locale`: -1, 0 or 1 as `left` orders before, with or after `right`.
[[nodiscard]] Result<int> compare(std::string_view locale, std::u16string_view left, std::u16string_view right,
                                  std::uint32_t options);
// The sort key of `text` in `locale`: comparing keys byte by byte orders as compare does.
[[nodiscard]] Result<std::vector<std::uint8_t>> sort_key(std::string_view locale, std::u16string_view text,
                                                         std::uint32_t options);

// Case and width mappings. The linguistic case mappings follow the locale's
// own rules (Turkish dotted and dotless i, ...; NSString's localized case
// mapping).
enum class Mapping : std::uint8_t { lower, upper, half_width, full_width, linguistic_lower, linguistic_upper };
[[nodiscard]] Result<std::u16string> map(std::string_view locale, std::u16string_view text, Mapping mapping);

// Linguistic search of `pattern` in `text` (Option bits apply): the first or
// last match anywhere, or a match anchored at the start or the end. Offsets
// and lengths count UTF-16 units of `text`; a match may differ in length from
// `pattern`. not_found when there is none.
enum class Search : std::uint8_t { first, last, prefix, suffix };
struct Match {
    std::size_t offset{}, length{};
};
[[nodiscard]] Result<Match> find(std::string_view locale, std::u16string_view text, std::u16string_view pattern, Search where,
                                 std::uint32_t options);

// Character classes (bits) of UTF-16 units, from the host's Unicode tables.
enum Class : std::uint16_t {
    upper = 1u << 0,
    lower = 1u << 1,
    digit = 1u << 2,
    space = 1u << 3,
    punctuation = 1u << 4,  // punctuation and symbols
    control = 1u << 5,
    blank = 1u << 6,
    hex_digit = 1u << 7,
    alpha = 1u << 8,
    defined = 1u << 9,      // an assigned character
    nonspacing = 1u << 10,  // a nonspacing mark
    diacritic = 1u << 11,
    vowel_mark = 1u << 12,
    symbol = 1u << 13,
};
// One value per unit of `text` into `classes` (at least text.size() long).
[[nodiscard]] Status classify(std::u16string_view text, std::span<std::uint16_t> classes);

// Mac OS Roman bytes as UTF-8 (macOS's default C string encoding).
[[nodiscard]] Result<std::string> decode_mac_roman(std::string_view bytes);

// ---- Locales ----

// Whether the host knows `locale`.
[[nodiscard]] bool valid(std::string_view locale);
// The user's default locale ("en-US").
[[nodiscard]] Result<std::string> user_locale();
// Every locale the host knows, neutral ("de") and specific ("de-DE"), by BCP
// 47 name, sorted, without the root locale or alternate sort orders.
[[nodiscard]] Result<std::vector<std::string>> locales();

// Locale data. Patterns are ICU/CLDR date format patterns; months are indexed
// 0-11 and days 0-6 from Sunday. An empty string is a field the locale leaves
// unset.
enum class Text : std::uint8_t {
    short_date_pattern,
    long_date_pattern,
    short_time_pattern,
    long_time_pattern,
    month_name,          // index 0-11
    month_abbreviation,  // index 0-11
    day_name,            // index 0-6
    day_abbreviation,    // index 0-6
    day_shortest,        // index 0-6
    am,
    pm,
    decimal_separator,
    grouping_separator,
    minus_sign,
    percent_sign,
    currency_symbol,
    currency_code,       // ISO 4217
    english_currency_name,
    native_currency_name,
    english_display_name,    // the locale's name in English
    native_display_name,     // ... in its own language
    localized_display_name,  // ... in the user's UI language
    english_language_name,
    native_language_name,
    localized_language_name,
};
[[nodiscard]] Result<std::string> text(std::string_view locale, Text field, unsigned index = 0);

enum class Number : std::uint8_t {
    currency_digits,             // fraction digits of currency amounts
    first_weekday,               // 1 = Sunday ... 7 = Saturday
    minimum_days_in_first_week,  // 1-7
};
[[nodiscard]] Result<std::uint32_t> number(std::string_view locale, Number field);

// `plain` ("-1234.50": optional minus, digits, optional '.' and fraction) in
// `locale`'s number or currency format with the given separators. A currency
// value takes the locale's positive and negative currency layouts.
struct Format {
    unsigned digits{};                 // fraction digits
    std::string_view decimal, grouping;  // separators; grouping empty: no grouping
};
[[nodiscard]] Result<std::string> format_number(std::string_view locale, std::string_view plain, const Format& format);
[[nodiscard]] Result<std::string> format_currency(std::string_view locale, std::string_view plain, const Format& format,
                                                  std::string_view symbol);

// The name of a region ("DE") in the user's UI language.
[[nodiscard]] Result<std::string> region_name(std::string_view region);
// The name of an ISO 4217 currency ("EUR"), in English or in the language of
// a locale that uses it; empty when no host locale uses it.
[[nodiscard]] Result<std::string> currency_name(std::string_view code, bool english);

// The IANA identifier of the system time zone ("Europe/Sofia").
[[nodiscard]] Result<std::string> time_zone();

} // namespace alchemy::host::locale
