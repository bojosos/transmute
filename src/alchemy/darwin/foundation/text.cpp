#include "alchemy/darwin/kit/completion/data/text.hpp"

#include "alchemy/host/locale.hpp"

#include <algorithm>
#include <optional>
#include <span>

namespace alchemy::gui::completion_kit::text {

std::u32string code_points(std::u16string_view value) {
    std::u32string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char32_t u = value[i];
        if (u >= 0xd800 && u <= 0xdbff && i + 1 < value.size() && value[i + 1] >= 0xdc00 && value[i + 1] <= 0xdfff) {
            out.push_back(0x10000 + ((u - 0xd800) << 10) + (static_cast<char32_t>(value[i + 1]) - 0xdc00));
            ++i;
        } else out.push_back(u);
    }
    return out;
}

void append_code_point(std::u16string& out, char32_t c) {
    if (c >= 0x10000 && c <= 0x10ffff) {
        c -= 0x10000;
        out.push_back(static_cast<char16_t>(0xd800 + (c >> 10)));
        out.push_back(static_cast<char16_t>(0xdc00 + (c & 0x3ff)));
    } else out.push_back(static_cast<char16_t>(c));
}

std::u16string from_code_points(std::u32string_view value) {
    std::u16string out;
    for (const auto c : value) append_code_point(out, c);
    return out;
}

namespace {

// Alternating upper/lower pairs: [first, last] with the upper case at even
// (or odd, when `odd_upper`) offsets from `first`.
struct PairRange { char32_t first, last; bool odd_upper; };
constexpr PairRange pair_ranges[] = {
    {0x0100, 0x012f, false}, {0x0132, 0x0137, false}, {0x0139, 0x0148, false}, {0x014a, 0x0177, false}, {0x0179, 0x017e, false},
    {0x01de, 0x01ef, false}, {0x01f8, 0x021f, false}, {0x0222, 0x0233, false}, {0x0246, 0x024f, false},
    {0x0460, 0x0481, false}, {0x048a, 0x04bf, false}, {0x04c1, 0x04ce, false}, {0x04d0, 0x052f, false},
    {0x1e00, 0x1e95, false}, {0x1ea0, 0x1eff, false},
};

} // namespace

char32_t to_lower(char32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if ((c >= 0xc0 && c <= 0xde && c != 0xd7)) return c + 32;
    if (c == 0x178) return 0xff;
    if (c == 0x130) return 'i';
    for (const auto& r : pair_ranges)
        if (c >= r.first && c <= r.last) {
            const bool upper = ((c - r.first) % 2 == 0) != r.odd_upper;
            return upper ? c + 1 : c;
        }
    if (c >= 0x391 && c <= 0x3ab && c != 0x3a2) return c + 32;
    if (c == 0x386) return 0x3ac;
    if (c >= 0x388 && c <= 0x38a) return c + 37;
    if (c == 0x38c) return 0x3cc;
    if (c == 0x38e || c == 0x38f) return c + 63;
    if (c >= 0x410 && c <= 0x42f) return c + 32;
    if (c >= 0x400 && c <= 0x40f) return c + 80;
    if (c == 0x4c0) return 0x4cf;
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (c >= 0xff21 && c <= 0xff3a) return c + 32;
    return c;
}

char32_t to_upper(char32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
    if (c == 0xff) return 0x178;
    if (c == 0xb5) return 0x39c;
    if (c == 0x131) return 'I';
    if (c == 0x17f) return 'S';
    for (const auto& r : pair_ranges)
        if (c >= r.first && c <= r.last) {
            const bool upper = ((c - r.first) % 2 == 0) != r.odd_upper;
            return upper ? c : c - 1;
        }
    if (c >= 0x3b1 && c <= 0x3cb && c != 0x3c2) return c - 32;
    if (c == 0x3c2) return 0x3a3;
    if (c == 0x3ac) return 0x386;
    if (c >= 0x3ad && c <= 0x3af) return c - 37;
    if (c == 0x3cc) return 0x38c;
    if (c == 0x3cd || c == 0x3ce) return c - 63;
    if (c >= 0x430 && c <= 0x44f) return c - 32;
    if (c >= 0x450 && c <= 0x45f) return c - 80;
    if (c == 0x4cf) return 0x4c0;
    if (c >= 0x561 && c <= 0x586) return c - 48;
    if (c >= 0xff41 && c <= 0xff5a) return c - 32;
    return c;
}

char32_t fold(char32_t c) {
    if (c == 0x3c2) return 0x3c3;
    if (c == 0x17f) return 's';
    if (c == 0xb5) return 0x3bc;
    if (c == 0x131) return 'i';
    return to_lower(c);
}

bool is_digit(char32_t c) {
    static constexpr char32_t zeros[] = {0x30, 0x660, 0x6f0, 0x7c0, 0x966, 0x9e6, 0xa66, 0xae6, 0xb66, 0xbe6, 0xc66, 0xce6, 0xd66, 0xde6,
                                         0xe50, 0xed0, 0xf20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946, 0x19d0, 0x1a80, 0x1a90, 0x1b50,
                                         0x1bb0, 0x1c40, 0x1c50, 0xa620, 0xa8d0, 0xa900, 0xa9d0, 0xaa50, 0xabf0, 0xff10};
    for (const auto z : zeros) if (c >= z && c <= z + 9) return true;
    return false;
}

namespace {
// The host's character classes of `c` (none above the BMP); nullopt without host Unicode tables.
std::optional<std::uint16_t> classes(char32_t c) {
    if (c > 0xffff) return std::uint16_t{0};
    const char16_t unit = static_cast<char16_t>(c);
    std::uint16_t value{};
    if (!alchemy::host::locale::classify(std::u16string_view(&unit, 1), std::span(&value, 1))) return std::nullopt;
    return value;
}
} // namespace

bool is_space(char32_t c) {
    return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

bool is_alpha(char32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c < 0x100) return c == 0xaa || c == 0xb5 || c == 0xba || (c >= 0xc0 && c != 0xd7 && c != 0xf7);
    if (c > 0xffff) {
        // Supplementary planes: letters except the symbol, emoji and private-use ranges.
        return !(c >= 0x1d000 && c <= 0x1d3ff) && !(c >= 0x1f000 && c <= 0x1faff) && c < 0xe0000;
    }
    if (const auto type = classes(c)) return (*type & alchemy::host::locale::alpha) != 0 && !is_digit(c);
    return !is_space(c) && !is_punct(c) && !(c >= 0x2000 && c <= 0x2bff) && !(c >= 0x3000 && c <= 0x303f);
}

bool is_mark(char32_t c) {
    if (c >= 0x300 && c <= 0x36f) return true;
    if (c > 0xffff || c < 0x300) return false;
    namespace locale = alchemy::host::locale;
    if (const auto type = classes(c)) return (*type & (locale::nonspacing | locale::diacritic | locale::vowel_mark)) != 0 && !(*type & locale::alpha);
    return (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe20 && c <= 0xfe2f);
}

bool is_punct(char32_t c) {
    if (c < 0x80) {
        static constexpr std::u32string_view p = U"!\"#%&'()*,-./:;?@[\\]_{}";
        return p.find(c) != std::u32string_view::npos;
    }
    if (c < 0x100) return c == 0xa1 || c == 0xa7 || c == 0xab || c == 0xb6 || c == 0xb7 || c == 0xbb || c == 0xbf;
    if (c >= 0x2010 && c <= 0x2027) return true;
    if (c >= 0x2030 && c <= 0x205e) return c != 0x2044 && c != 0x2052;
    if (c >= 0x3001 && c <= 0x3003) return true;
    if (c >= 0x3008 && c <= 0x3011) return true;
    if (c > 0xffff) return false;
    // The host's punctuation class also covers symbols; keep the general punctuation ranges.
    if (const auto type = classes(c)) return (*type & alchemy::host::locale::punctuation) != 0 && ((c >= 0x2e00 && c <= 0x2e7f) || (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff01 && c <= 0xff65 && c != 0xff04 && c != 0xff0b && c != 0xff1c && c != 0xff1d && c != 0xff1e && c != 0xff3e && c != 0xff40 && c != 0xff5c && c != 0xff5e));
    return false;
}

bool is_word(char32_t c) {
    return is_alpha(c) || is_digit(c) || is_mark(c) || c == '_' || c == 0x203f || c == 0x2040 || c == 0x2054 || c == 0xfe33 || c == 0xfe34 ||
           (c >= 0xfe4d && c <= 0xfe4f) || c == 0xff3f || c == 0x200c || c == 0x200d;
}

std::u16string lower(std::u16string_view value) {
    std::u16string out;
    for (const auto c : code_points(value)) append_code_point(out, to_lower(c));
    return out;
}

std::u16string upper(std::u16string_view value) {
    std::u16string out;
    for (const auto c : code_points(value)) {
        if (c == 0xdf) { out += u"SS"; continue; }   // full mapping of the sharp s
        append_code_point(out, to_upper(c));
    }
    return out;
}

std::u16string capitalized(std::u16string_view value) {
    std::u16string out;
    bool word_start = true;
    for (const auto c : code_points(value)) {
        const bool letter = is_alpha(c) || is_digit(c) || is_mark(c);
        if (letter) {
            append_code_point(out, word_start ? to_upper(c) : to_lower(c));
            word_start = false;
        } else {
            append_code_point(out, c);
            // Apostrophes stay inside words (O'neil); everything else ends one.
            if (c != '\'' && c != 0x2019) word_start = true;
        }
    }
    return out;
}

char32_t strip_diacritic(char32_t c) {
    static constexpr std::u16string_view latin1 = u"AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0xc0 && c <= 0xff && c != 0xc6 && c != 0xe6 && c != 0xd7 && c != 0xf7 && c != 0xdf && c != 0xde && c != 0xfe && c != 0xd0 && c != 0xf0)
        return latin1[c - 0xc0];
    static constexpr std::u16string_view extended_a =
        u"AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    if (c >= 0x100 && c <= 0x17f) {
        const auto r = extended_a[c - 0x100];
        // Ligatures and letters without a base keep themselves.
        if (c == 0x132 || c == 0x133 || c == 0x152 || c == 0x153 || c == 0x138 || c == 0x149 || c == 0x14a || c == 0x14b) return c;
        return r;
    }
    if (c >= 0x300 && c <= 0x36f) return 0;   // a combining mark disappears
    return c;
}

namespace {

char32_t width_fold(char32_t c) {
    if (c >= 0xff01 && c <= 0xff5e) return c - 0xff01 + 0x21;
    if (c == 0x3000) return 0x20;
    return c;
}

std::u32string normalized(std::u16string_view value, std::uint64_t options) {
    std::u32string out;
    for (auto c : code_points(value)) {
        if (options & width_insensitive_search) c = width_fold(c);
        if (options & diacritic_insensitive_search) { c = strip_diacritic(c); if (!c) continue; }
        if (options & case_insensitive_search) c = fold(c);
        out.push_back(c);
    }
    return out;
}

int sign(std::int64_t v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }

int compare_points(const std::u32string& a, const std::u32string& b, bool numeric) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (numeric && a[i] >= '0' && a[i] <= '9' && b[j] >= '0' && b[j] <= '9') {
            auto si = i, sj = j;
            while (si < a.size() && a[si] == '0') ++si;
            while (sj < b.size() && b[sj] == '0') ++sj;
            auto ei = si, ej = sj;
            while (ei < a.size() && a[ei] >= '0' && a[ei] <= '9') ++ei;
            while (ej < b.size() && b[ej] >= '0' && b[ej] <= '9') ++ej;
            if (ei - si != ej - sj) return ei - si < ej - sj ? -1 : 1;
            for (std::size_t k = 0; k < ei - si; ++k) if (a[si + k] != b[sj + k]) return a[si + k] < b[sj + k] ? -1 : 1;
            i = ei;
            j = ej;
            continue;
        }
        if (a[i] != b[j]) {
            // UTF-16 order (surrogates sort above the BMP below U+E000), as CFString compares units.
            const auto key = [](char32_t c) -> std::uint64_t { return c >= 0x10000 ? 0xd800 + (c >> 10) : c; };
            const auto ka = key(a[i]), kb = key(b[j]);
            if (ka != kb) return ka < kb ? -1 : 1;
            return a[i] < b[j] ? -1 : 1;
        }
        ++i;
        ++j;
    }
    return sign(static_cast<std::int64_t>(a.size() - i) - static_cast<std::int64_t>(b.size() - j));
}

} // namespace

int compare(std::u16string_view left, std::u16string_view right, std::uint64_t options) {
    const bool numeric = options & numeric_search;
    const auto r = compare_points(normalized(left, options), normalized(right, options), numeric);
    if (r || !(options & forced_ordering_search)) return r;
    return compare_points(normalized(left, 0), normalized(right, 0), numeric);
}

Found find(std::u16string_view text, std::u16string_view target, std::uint64_t options, std::int64_t from, std::int64_t length) {
    Found none;
    if (target.empty() || length <= 0) return none;
    // Unit-wise normalization (1:1 in the BMP) keeps indices aligned.
    const auto unit = [options](char16_t c) -> char32_t {
        char32_t x = c;
        if (options & width_insensitive_search) x = width_fold(x);
        if (options & diacritic_insensitive_search) { const auto s = strip_diacritic(x); if (s) x = s; }
        if (options & case_insensitive_search) x = fold(x);
        return x;
    };
    const auto n = static_cast<std::int64_t>(target.size());
    if (n > length) return none;
    const auto matches = [&](std::int64_t at) {
        for (std::int64_t k = 0; k < n; ++k)
            if (unit(text[static_cast<std::size_t>(at + k)]) != unit(target[static_cast<std::size_t>(k)])) return false;
        return true;
    };
    const auto first = from, last = from + length - n;
    if (options & backwards_search) {
        if (options & anchored_search) return matches(last) ? Found{last, n} : none;
        for (auto at = last; at >= first; --at) if (matches(at)) return {at, n};
        return none;
    }
    if (options & anchored_search) return matches(first) ? Found{first, n} : none;
    for (auto at = first; at <= last; ++at) if (matches(at)) return {at, n};
    return none;
}

std::u16string pad(std::u16string_view value, std::size_t length, std::u16string_view padding, std::size_t start) {
    if (length <= value.size()) return std::u16string(value.substr(0, length));
    std::u16string out(value);
    if (padding.empty()) throw std::invalid_argument("the padding string is empty");
    if (start >= padding.size()) throw std::out_of_range("the padding index is beyond the padding string");
    auto at = start;
    while (out.size() < length) {
        out.push_back(padding[at]);
        at = (at + 1) % padding.size();
    }
    return out;
}

} // namespace alchemy::gui::completion_kit::text
