#pragma once

// UTF-16 text helpers shared by the G1 data providers (host-only): code
// point decoding, simple case mapping, Unicode character classes, and the
// NSString algorithms that need no guest (capitalization, numeric
// "localized standard" comparison, percent encoding, padding, trimming).
//
// Case mapping is the simple (1:1) mapping for ASCII, Latin-1, Latin
// Extended-A, Latin Extended Additional, Greek, Cyrillic, Armenian and the
// fullwidth Latin letters; other characters map to themselves. Character
// classes come from the host's Unicode tables (GetStringTypeW) for the Basic
// Multilingual Plane; supplementary-plane characters are letters unless they
// are in the symbol/emoji planes.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::text {

[[nodiscard]] std::u32string code_points(std::u16string_view value);
[[nodiscard]] std::u16string from_code_points(std::u32string_view value);
void append_code_point(std::u16string& out, char32_t c);

[[nodiscard]] char32_t to_lower(char32_t c);
[[nodiscard]] char32_t to_upper(char32_t c);
[[nodiscard]] char32_t fold(char32_t c);   // case folding for comparison

[[nodiscard]] bool is_digit(char32_t c);   // \p{Nd}
[[nodiscard]] bool is_alpha(char32_t c);
[[nodiscard]] bool is_mark(char32_t c);
[[nodiscard]] bool is_space(char32_t c);   // \s: [\t\n\f\r\x0b\p{Z}]
[[nodiscard]] bool is_word(char32_t c);    // \w
[[nodiscard]] bool is_punct(char32_t c);   // \p{P}

[[nodiscard]] std::u16string lower(std::u16string_view value);
[[nodiscard]] std::u16string upper(std::u16string_view value);
// -capitalizedString: the first character of each word upper case, the rest
// lower case; words are separated by white space.
[[nodiscard]] std::u16string capitalized(std::u16string_view value);

// NSStringCompareOptions bits used by the comparisons below.
enum CompareOption : std::uint64_t {
    case_insensitive_search = 1, literal_search = 2, backwards_search = 4, anchored_search = 8, numeric_search = 64,
    diacritic_insensitive_search = 128, width_insensitive_search = 256, forced_ordering_search = 512, regular_expression_search = 1024,
};
// -compare:options:range: result (NSOrderedAscending -1, same 0, descending 1).
[[nodiscard]] int compare(std::u16string_view left, std::u16string_view right, std::uint64_t options);
// Removes diacritics (Latin-1 / Latin Extended-A base letters) when asked.
[[nodiscard]] char32_t strip_diacritic(char32_t c);
// -rangeOfString:options:range: inside [from, from + length); location -1 when absent.
struct Found { std::int64_t location{-1}, length{0}; };
[[nodiscard]] Found find(std::u16string_view text, std::u16string_view target, std::uint64_t options, std::int64_t from, std::int64_t length);

// -stringByAddingPercentEncodingWithAllowedCharacters: over UTF-8; `allowed`
// tells whether a code point is kept.
template <typename Allowed>
std::u16string percent_encode(std::u16string_view value, Allowed allowed) {
    std::u16string out;
    static constexpr char16_t hex[] = u"0123456789ABCDEF";
    for (const auto c : code_points(value)) {
        if (allowed(c)) { append_code_point(out, c); continue; }
        char bytes[4];
        int n = 0;
        if (c < 0x80) bytes[n++] = static_cast<char>(c);
        else if (c < 0x800) { bytes[n++] = static_cast<char>(0xc0 | (c >> 6)); bytes[n++] = static_cast<char>(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { bytes[n++] = static_cast<char>(0xe0 | (c >> 12)); bytes[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3f)); bytes[n++] = static_cast<char>(0x80 | (c & 0x3f)); }
        else { bytes[n++] = static_cast<char>(0xf0 | (c >> 18)); bytes[n++] = static_cast<char>(0x80 | ((c >> 12) & 0x3f)); bytes[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3f)); bytes[n++] = static_cast<char>(0x80 | (c & 0x3f)); }
        for (int k = 0; k < n; ++k) {
            const auto b = static_cast<unsigned char>(bytes[k]);
            out.push_back(u'%'); out.push_back(hex[b >> 4]); out.push_back(hex[b & 15]);
        }
    }
    return out;
}

// -stringByPaddingToLength:withString:startingAtIndex:.
[[nodiscard]] std::u16string pad(std::u16string_view value, std::size_t length, std::u16string_view padding, std::size_t start);

// Thrown when a regular expression search exceeds its backtracking budget.
struct Exhausted : std::runtime_error { using std::runtime_error::runtime_error; };

} // namespace alchemy::gui::completion_kit::text

namespace alchemy::gui::completion_kit::regex { using Exhausted = text::Exhausted; }
