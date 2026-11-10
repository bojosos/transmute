// Linux host backend, area "locale" (include/alchemy/host/locale.hpp), over the distribution's
// ICU4C loaded at run time (icu_library.hpp): normalization (unorm2), collation, sort keys and
// linguistic search (ucol, usearch), case mapping (u_strTo*), width mapping (transliterators),
// character classes (uchar), Mac OS Roman (a converter), locales and their data (uloc, udat,
// unum, ucurr, ucal). The user's locale is ICU's default (LC_ALL, LC_MESSAGES/LANG). Number and
// currency formatting follow the Windows backend's layout rules (the caller's separators and
// digits, the locale's currency layout). A host without ICU answers unsupported.
#include "alchemy/host/locale.hpp"

#include "icu_library.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

namespace alchemy::host::locale {
namespace {

using UChar = char16_t;
using UChar32 = std::int32_t;
using UErrorCode = std::int32_t;
using UBool = std::int8_t;
[[nodiscard]] bool failed(UErrorCode code) noexcept { return code > 0; }

// ICU's C API, the calls this area makes (signatures from ICU's public headers, ICU license).
#define ALCHEMY_LOCALE_ICU(X)                                                                                         \
    X(u_strToUTF8, char*, (char*, std::int32_t, std::int32_t*, const UChar*, std::int32_t, UErrorCode*))              \
    X(u_strFromUTF8, UChar*, (UChar*, std::int32_t, std::int32_t*, const char*, std::int32_t, UErrorCode*))           \
    X(unorm2_getNFCInstance, const void*, (UErrorCode*))                                                              \
    X(unorm2_getNFDInstance, const void*, (UErrorCode*))                                                              \
    X(unorm2_getNFKCInstance, const void*, (UErrorCode*))                                                             \
    X(unorm2_getNFKDInstance, const void*, (UErrorCode*))                                                             \
    X(unorm2_normalize, std::int32_t, (const void*, const UChar*, std::int32_t, UChar*, std::int32_t, UErrorCode*))  \
    X(ucol_open, void*, (const char*, UErrorCode*))                                                                   \
    X(ucol_close, void, (void*))                                                                                      \
    X(ucol_setAttribute, void, (void*, std::int32_t, std::int32_t, UErrorCode*))                                      \
    X(ucol_strcoll, std::int32_t, (const void*, const UChar*, std::int32_t, const UChar*, std::int32_t))             \
    X(ucol_getSortKey, std::int32_t, (const void*, const UChar*, std::int32_t, std::uint8_t*, std::int32_t))          \
    X(usearch_openFromCollator, void*, (const UChar*, std::int32_t, const UChar*, std::int32_t, const void*, void*,   \
                                        UErrorCode*))                                                                 \
    X(usearch_first, std::int32_t, (void*, UErrorCode*))                                                              \
    X(usearch_last, std::int32_t, (void*, UErrorCode*))                                                               \
    X(usearch_getMatchedLength, std::int32_t, (const void*))                                                          \
    X(usearch_close, void, (void*))                                                                                   \
    X(u_strToLower, std::int32_t, (UChar*, std::int32_t, const UChar*, std::int32_t, const char*, UErrorCode*))       \
    X(u_strToUpper, std::int32_t, (UChar*, std::int32_t, const UChar*, std::int32_t, const char*, UErrorCode*))       \
    X(utrans_openU, void*, (const UChar*, std::int32_t, std::int32_t, const UChar*, std::int32_t, void*, UErrorCode*)) \
    X(utrans_close, void, (void*))                                                                                    \
    X(utrans_transUChars, void, (const void*, UChar*, std::int32_t*, std::int32_t, std::int32_t, std::int32_t*,       \
                                 UErrorCode*))                                                                        \
    X(u_charType, std::int8_t, (UChar32))                                                                             \
    X(u_isupper, UBool, (UChar32))                                                                                    \
    X(u_islower, UBool, (UChar32))                                                                                    \
    X(u_isdigit, UBool, (UChar32))                                                                                    \
    X(u_isspace, UBool, (UChar32))                                                                                    \
    X(u_ispunct, UBool, (UChar32))                                                                                    \
    X(u_iscntrl, UBool, (UChar32))                                                                                    \
    X(u_isblank, UBool, (UChar32))                                                                                    \
    X(u_isxdigit, UBool, (UChar32))                                                                                   \
    X(u_isalpha, UBool, (UChar32))                                                                                    \
    X(u_isdefined, UBool, (UChar32))                                                                                  \
    X(u_hasBinaryProperty, UBool, (UChar32, std::int32_t))                                                            \
    X(ucnv_open, void*, (const char*, UErrorCode*))                                                                   \
    X(ucnv_close, void, (void*))                                                                                      \
    X(ucnv_toUChars, std::int32_t, (void*, UChar*, std::int32_t, const char*, std::int32_t, UErrorCode*))             \
    X(uloc_getDefault, const char*, ())                                                                               \
    X(uloc_countAvailable, std::int32_t, ())                                                                          \
    X(uloc_getAvailable, const char*, (std::int32_t))                                                                 \
    X(uloc_forLanguageTag, std::int32_t, (const char*, char*, std::int32_t, std::int32_t*, UErrorCode*))              \
    X(uloc_toLanguageTag, std::int32_t, (const char*, char*, std::int32_t, UBool, UErrorCode*))                       \
    X(uloc_getLanguage, std::int32_t, (const char*, char*, std::int32_t, UErrorCode*))                                \
    X(uloc_getCountry, std::int32_t, (const char*, char*, std::int32_t, UErrorCode*))                                 \
    X(uloc_getScript, std::int32_t, (const char*, char*, std::int32_t, UErrorCode*))                                  \
    X(uloc_addLikelySubtags, std::int32_t, (const char*, char*, std::int32_t, UErrorCode*))                           \
    X(uloc_getDisplayName, std::int32_t, (const char*, const char*, UChar*, std::int32_t, UErrorCode*))               \
    X(uloc_getDisplayLanguage, std::int32_t, (const char*, const char*, UChar*, std::int32_t, UErrorCode*))           \
    X(uloc_getDisplayCountry, std::int32_t, (const char*, const char*, UChar*, std::int32_t, UErrorCode*))            \
    X(udat_open, void*, (std::int32_t, std::int32_t, const char*, const UChar*, std::int32_t, const UChar*,           \
                         std::int32_t, UErrorCode*))                                                                  \
    X(udat_close, void, (void*))                                                                                      \
    X(udat_toPattern, std::int32_t, (const void*, UBool, UChar*, std::int32_t, UErrorCode*))                          \
    X(udat_getSymbols, std::int32_t, (const void*, std::int32_t, std::int32_t, UChar*, std::int32_t, UErrorCode*))    \
    X(unum_open, void*, (std::int32_t, const UChar*, std::int32_t, const char*, void*, UErrorCode*))                  \
    X(unum_close, void, (void*))                                                                                      \
    X(unum_getSymbol, std::int32_t, (const void*, std::int32_t, UChar*, std::int32_t, UErrorCode*))                   \
    X(unum_toPattern, std::int32_t, (const void*, UBool, UChar*, std::int32_t, UErrorCode*))                          \
    X(ucurr_forLocale, std::int32_t, (const char*, UChar*, std::int32_t, UErrorCode*))                                \
    X(ucurr_getName, const UChar*, (const UChar*, const char*, std::int32_t, UBool*, std::int32_t*, UErrorCode*))     \
    X(ucurr_getDefaultFractionDigits, std::int32_t, (const UChar*, UErrorCode*))                                      \
    X(ucal_open, void*, (const UChar*, std::int32_t, const char*, std::int32_t, UErrorCode*))                         \
    X(ucal_close, void, (void*))                                                                                      \
    X(ucal_getAttribute, std::int32_t, (const void*, std::int32_t))                                                   \
    X(ucal_getDefaultTimeZone, std::int32_t, (UChar*, std::int32_t, UErrorCode*))

struct Icu {
#define ALCHEMY_LOCALE_ENTRY(name, result, parameters) result(*name) parameters = nullptr;
    ALCHEMY_LOCALE_ICU(ALCHEMY_LOCALE_ENTRY)
#undef ALCHEMY_LOCALE_ENTRY
};

// The table, all or nothing: nullptr without ICU or when one call is missing.
const Icu* icu() noexcept {
    static const auto table = []() -> std::optional<Icu> {
        Icu value;
        bool complete = true;
#define ALCHEMY_LOCALE_RESOLVE(name, result, parameters) \
        value.name = reinterpret_cast<result(*) parameters>(linux_icu::symbol(#name)); \
        complete = complete && value.name != nullptr;
        ALCHEMY_LOCALE_ICU(ALCHEMY_LOCALE_RESOLVE)
#undef ALCHEMY_LOCALE_RESOLVE
        if (!complete) return std::nullopt;
        return value;
    }();
    return table ? &*table : nullptr;
}

// ICU constants (utypes.h, ucol.h, udat.h, unum.h, ucal.h, ucurr.h, uchar.h, usearch.h).
constexpr UErrorCode buffer_overflow = 15;
constexpr std::int32_t col_alternate_handling = 1, col_case_level = 3, col_strength = 5, col_numeric = 7;
constexpr std::int32_t col_primary = 0, col_secondary = 1, col_on = 17, col_shifted = 20;
constexpr std::int32_t dat_full = 0, dat_medium = 2, dat_short = 3, dat_none = -1;
constexpr std::int32_t dat_months = 1, dat_short_months = 2, dat_weekdays = 3, dat_short_weekdays = 4, dat_am_pms = 5,
                       dat_shorter_weekdays = 20;
constexpr std::int32_t num_decimal = 1, num_currency = 2;
constexpr std::int32_t num_decimal_separator = 0, num_grouping_separator = 1, num_percent = 3, num_minus = 6,
                       num_currency_symbol = 8;
constexpr std::int32_t cal_gregorian = 1, cal_first_day = 1, cal_minimal_days = 2;
constexpr std::int32_t curr_long_name = 1;
constexpr std::int32_t prop_diacritic = 7;
constexpr std::int8_t nonspacing_mark = 6, spacing_mark = 8, math_symbol = 24, currency_symbol_type = 25,
                      modifier_symbol = 26, other_symbol = 27;

// An ICU call that writes into a UTF-16 buffer: grown once when it overflows.
template <class Call>
Result<std::u16string> uchars(Call&& call, std::size_t first = 64) {
    std::u16string out(first, u'\0');
    UErrorCode error = 0;
    auto length = call(out.data(), static_cast<std::int32_t>(out.size()), &error);
    if (error == buffer_overflow || length > static_cast<std::int32_t>(out.size())) {
        out.assign(static_cast<std::size_t>(length) + 1, u'\0');
        error = 0;
        length = call(out.data(), static_cast<std::int32_t>(out.size()), &error);
    }
    if (failed(error) || length < 0) return Error::invalid_argument;
    out.resize(static_cast<std::size_t>(length));
    return out;
}

std::string utf8(const Icu& api, std::u16string_view text) {
    if (text.empty()) return {};
    std::string out(text.size() * 3 + 1, '\0');
    std::int32_t length = 0;
    UErrorCode error = 0;
    api.u_strToUTF8(out.data(), static_cast<std::int32_t>(out.size()), &length, text.data(), static_cast<std::int32_t>(text.size()), &error);
    if (failed(error)) return {};
    out.resize(static_cast<std::size_t>(length));
    return out;
}
std::u16string utf16(const Icu& api, std::string_view text) {
    if (text.empty()) return {};
    std::u16string out(text.size() + 1, u'\0');
    std::int32_t length = 0;
    UErrorCode error = 0;
    api.u_strFromUTF8(out.data(), static_cast<std::int32_t>(out.size()), &length, text.data(), static_cast<std::int32_t>(text.size()), &error);
    if (failed(error)) return {};
    out.resize(static_cast<std::size_t>(length));
    return out;
}

// A BCP 47 name as an ICU locale ID: "" is ICU's default (the user's), "und" the root.
std::string locale_id(const Icu& api, std::string_view name) {
    if (name.empty()) return api.uloc_getDefault();
    if (name == "und") return "root";
    std::array<char, 157> id{};
    std::int32_t parsed = 0;
    UErrorCode error = 0;
    const std::string tag(name);
    const auto length = api.uloc_forLanguageTag(tag.c_str(), id.data(), static_cast<std::int32_t>(id.size()), &parsed, &error);
    if (failed(error) || length <= 0) return tag;
    return std::string(id.data(), static_cast<std::size_t>(length));
}
std::string language_tag(const Icu& api, const char* id) {
    std::array<char, 157> tag{};
    UErrorCode error = 0;
    const auto length = api.uloc_toLanguageTag(id, tag.data(), static_cast<std::int32_t>(tag.size()), 0, &error);
    if (failed(error) || length <= 0) return {};
    return std::string(tag.data(), static_cast<std::size_t>(length));
}

struct Collator {
    void* value{};
    const Icu* api{};
    ~Collator() { if (value) api->ucol_close(value); }
};
// A collator of `locale` with the Option bits: case and width/kana differences are tertiary
// (ignored at secondary strength), diacritics secondary (ignored at primary strength, with the
// case level kept when case still counts), symbols shifted, digit runs numeric.
Result<std::unique_ptr<Collator>> collator(const Icu& api, std::string_view locale, std::uint32_t options) {
    UErrorCode error = 0;
    auto result = std::make_unique<Collator>();
    result->api = &api;
    result->value = api.ucol_open(locale_id(api, locale).c_str(), &error);
    if (!result->value || failed(error)) return Error::invalid_argument;
    const bool ignore_case_bits = options & (ignore_case | ignore_case_linguistic);
    const bool ignore_tertiary = ignore_case_bits || (options & (ignore_width | ignore_kana_type));
    const bool ignore_accents = options & (ignore_nonspace | ignore_diacritics);
    if (ignore_accents) {
        api.ucol_setAttribute(result->value, col_strength, col_primary, &error);
        if (!ignore_case_bits) api.ucol_setAttribute(result->value, col_case_level, col_on, &error);
    } else if (ignore_tertiary) {
        api.ucol_setAttribute(result->value, col_strength, col_secondary, &error);
    }
    if (options & ignore_symbols) api.ucol_setAttribute(result->value, col_alternate_handling, col_shifted, &error);
    if (options & digits_as_numbers) api.ucol_setAttribute(result->value, col_numeric, col_on, &error);
    if (failed(error)) return Error::invalid_argument;
    return result;
}

Result<std::int32_t> size_of(std::size_t size) {
    if (size > static_cast<std::size_t>(INT32_MAX)) return Error::out_of_range;
    return static_cast<std::int32_t>(size);
}

// A date format of `locale` (styles as udat_open), closed with the object.
struct DateFormat {
    void* value{};
    const Icu* api{};
    ~DateFormat() { if (value) api->udat_close(value); }
};
DateFormat date_format(const Icu& api, const std::string& id, std::int32_t time, std::int32_t date) {
    UErrorCode error = 0;
    DateFormat result{api.udat_open(time, date, id.c_str(), nullptr, -1, nullptr, -1, &error), &api};
    if (failed(error) && result.value) { api.udat_close(result.value); result.value = nullptr; }
    return result;
}
struct NumberFormat {
    void* value{};
    const Icu* api{};
    ~NumberFormat() { if (value) api->unum_close(value); }
};
NumberFormat number_format(const Icu& api, const std::string& id, std::int32_t style) {
    UErrorCode error = 0;
    NumberFormat result{api.unum_open(style, nullptr, 0, id.c_str(), nullptr, &error), &api};
    if (failed(error) && result.value) { api.unum_close(result.value); result.value = nullptr; }
    return result;
}

// `locale`'s ISO 4217 currency ("" when it has none).
std::u16string currency_of(const Icu& api, const std::string& id) {
    std::array<UChar, 8> code{};
    UErrorCode error = 0;
    const auto length = api.ucurr_forLocale(id.c_str(), code.data(), static_cast<std::int32_t>(code.size()), &error);
    if (failed(error) || length <= 0) return {};
    return std::u16string(code.data(), static_cast<std::size_t>(length));
}
std::string currency_long_name(const Icu& api, const std::u16string& code, const std::string& display) {
    UBool choice = 0;
    std::int32_t length = 0;
    UErrorCode error = 0;
    const auto* name = api.ucurr_getName(code.c_str(), display.c_str(), curr_long_name, &choice, &length, &error);
    if (!name || failed(error)) return {};
    return utf8(api, std::u16string_view(name, static_cast<std::size_t>(length)));
}

// "-1234.50" -> sign, integer digits and fraction digits; nullopt when it is not that shape.
struct Plain {
    bool negative{};
    std::string integer, fraction;
};
std::optional<Plain> parse_plain(std::string_view text) {
    Plain value;
    if (!text.empty() && text.front() == '-') { value.negative = true; text.remove_prefix(1); }
    const auto dot = text.find('.');
    value.integer = std::string(text.substr(0, dot));
    if (dot != std::string_view::npos) value.fraction = std::string(text.substr(dot + 1));
    const auto digits = [](const std::string& part) { return std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    if ((value.integer.empty() && value.fraction.empty()) || !digits(value.integer) || !digits(value.fraction)) return std::nullopt;
    return value;
}
// The digits of `plain` with `digits` fraction digits (rounded half away from zero), grouped by
// three with `grouping` when it is not empty, a leading zero kept: the magnitude only.
std::string layout_number(Plain plain, unsigned digits, std::string_view decimal, std::string_view grouping) {
    if (plain.integer.empty()) plain.integer = "0";
    if (plain.fraction.size() > digits) {
        const bool up = plain.fraction[digits] >= '5';
        plain.fraction.resize(digits);
        if (up) {
            std::string all = plain.integer + plain.fraction;
            int at = static_cast<int>(all.size()) - 1;
            for (; at >= 0; --at) {
                if (all[static_cast<std::size_t>(at)] == '9') all[static_cast<std::size_t>(at)] = '0';
                else { ++all[static_cast<std::size_t>(at)]; break; }
            }
            if (at < 0) all.insert(all.begin(), '1');
            plain.integer = all.substr(0, all.size() - digits);
            plain.fraction = all.substr(all.size() - digits);
        }
    }
    plain.fraction.resize(digits, '0');
    plain.integer.erase(0, std::min(plain.integer.find_first_not_of('0'), plain.integer.size() - 1));
    std::string grouped;
    for (std::size_t index = 0; index < plain.integer.size(); ++index) {
        if (index && !grouping.empty() && (plain.integer.size() - index) % 3 == 0) grouped += grouping;
        grouped += plain.integer[index];
    }
    if (digits) { grouped += decimal; grouped += plain.fraction; }
    return grouped;
}

} // namespace

Result<std::u16string> normalize(std::u16string_view text, Form form) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    if (text.empty()) return std::u16string{};
    const auto size = size_of(text.size());
    if (!size) return size.error();
    UErrorCode error = 0;
    const void* normalizer = form == Form::c ? api->unorm2_getNFCInstance(&error) : form == Form::d ? api->unorm2_getNFDInstance(&error)
                           : form == Form::kc ? api->unorm2_getNFKCInstance(&error) : api->unorm2_getNFKDInstance(&error);
    if (!normalizer || failed(error)) return Error::unsupported;
    // ICU replaces nothing: unpaired surrogates pass through, so reject them as the contract says.
    for (std::size_t at = 0; at < text.size(); ++at) {
        const auto unit = text[at];
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (at + 1 >= text.size() || text[at + 1] < 0xdc00 || text[at + 1] > 0xdfff) return Error::invalid_argument;
            ++at;
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
            return Error::invalid_argument;
        }
    }
    return uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
        return api->unorm2_normalize(normalizer, text.data(), *size, out, capacity, status);
    }, text.size() * 3 + 1);
}

Result<int> compare(std::string_view locale, std::u16string_view left, std::u16string_view right, std::uint32_t options) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    const auto left_size = size_of(left.size()), right_size = size_of(right.size());
    if (!left_size || !right_size) return Error::out_of_range;
    auto value = collator(*api, locale, options);
    if (!value) return value.error();
    const auto order = api->ucol_strcoll((*value)->value, left.data(), *left_size, right.data(), *right_size);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
}

Result<std::vector<std::uint8_t>> sort_key(std::string_view locale, std::u16string_view text, std::uint32_t options) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    const auto size = size_of(text.size());
    if (!size) return size.error();
    auto value = collator(*api, locale, options);
    if (!value) return value.error();
    std::vector<std::uint8_t> key(64);
    auto length = api->ucol_getSortKey((*value)->value, text.data(), *size, key.data(), static_cast<std::int32_t>(key.size()));
    if (length > static_cast<std::int32_t>(key.size())) {
        key.resize(static_cast<std::size_t>(length));
        length = api->ucol_getSortKey((*value)->value, text.data(), *size, key.data(), static_cast<std::int32_t>(key.size()));
    }
    if (length <= 0) return Error::invalid_argument;
    key.resize(static_cast<std::size_t>(length));
    return key;
}

Result<std::u16string> map(std::string_view locale, std::u16string_view text, Mapping mapping) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    if (text.empty()) return std::u16string{};
    const auto size = size_of(text.size());
    if (!size) return size.error();
    if (mapping == Mapping::half_width || mapping == Mapping::full_width) {
        const std::u16string id = mapping == Mapping::half_width ? u"Fullwidth-Halfwidth" : u"Halfwidth-Fullwidth";
        UErrorCode error = 0;
        void* transliterator = api->utrans_openU(id.data(), static_cast<std::int32_t>(id.size()), 0, nullptr, 0, nullptr, &error);
        if (!transliterator || failed(error)) return Error::unsupported;
        // Width forms map one to one or merge (half-width kana with voicing marks): the text never grows past twice.
        std::u16string out(text);
        out.resize(text.size() * 2 + 1);
        std::int32_t length = *size, limit = *size;
        api->utrans_transUChars(transliterator, out.data(), &length, static_cast<std::int32_t>(out.size()), 0, &limit, &error);
        api->utrans_close(transliterator);
        if (failed(error) || length < 0) return Error::invalid_argument;
        out.resize(static_cast<std::size_t>(length));
        return out;
    }
    // The plain mappings are the root's (locale-independent); the linguistic ones the locale's.
    const bool linguistic = mapping == Mapping::linguistic_lower || mapping == Mapping::linguistic_upper;
    const auto id = linguistic ? locale_id(*api, locale) : std::string("root");
    const bool lower = mapping == Mapping::lower || mapping == Mapping::linguistic_lower;
    return uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
        return lower ? api->u_strToLower(out, capacity, text.data(), *size, id.c_str(), status)
                     : api->u_strToUpper(out, capacity, text.data(), *size, id.c_str(), status);
    }, text.size() + 8);
}

Result<Match> find(std::string_view locale, std::u16string_view text, std::u16string_view pattern, Search where, std::uint32_t options) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    const auto text_size = size_of(text.size()), pattern_size = size_of(pattern.size());
    if (!text_size || !pattern_size) return Error::out_of_range;
    if (pattern.empty() || text.empty()) return Error::not_found;
    auto value = collator(*api, locale, options);
    if (!value) return value.error();
    UErrorCode error = 0;
    void* search = api->usearch_openFromCollator(pattern.data(), *pattern_size, text.data(), *text_size, (*value)->value, nullptr, &error);
    if (!search || failed(error)) return Error::invalid_argument;
    const auto offset = where == Search::last || where == Search::suffix ? api->usearch_last(search, &error) : api->usearch_first(search, &error);
    const auto matched = offset >= 0 ? api->usearch_getMatchedLength(search) : 0;
    api->usearch_close(search);
    if (failed(error)) return Error::invalid_argument;
    if (offset < 0) return Error::not_found;
    const Match match{static_cast<std::size_t>(offset), static_cast<std::size_t>(matched)};
    if (where == Search::prefix && match.offset != 0) return Error::not_found;
    if (where == Search::suffix && match.offset + match.length != text.size()) return Error::not_found;
    return match;
}

Status classify(std::u16string_view text, std::span<std::uint16_t> classes) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    if (classes.size() < text.size()) return Error::invalid_argument;
    for (std::size_t at = 0; at < text.size(); ++at) {
        // A surrogate pair is one character: both units get its classes.
        UChar32 c = text[at];
        std::size_t units = 1;
        if (c >= 0xd800 && c <= 0xdbff && at + 1 < text.size() && text[at + 1] >= 0xdc00 && text[at + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (text[at + 1] - 0xdc00);
            units = 2;
        }
        const auto type = api->u_charType(c);
        const bool symbol_type = type == math_symbol || type == currency_symbol_type || type == modifier_symbol || type == other_symbol;
        std::uint16_t value = 0;
        if (api->u_isupper(c)) value |= upper;
        if (api->u_islower(c)) value |= lower;
        if (api->u_isdigit(c)) value |= digit;
        if (api->u_isspace(c)) value |= space;
        if (api->u_ispunct(c) || symbol_type) value |= punctuation;
        if (api->u_iscntrl(c)) value |= control;
        if (api->u_isblank(c)) value |= blank;
        if (api->u_isxdigit(c)) value |= hex_digit;
        if (api->u_isalpha(c)) value |= alpha;
        if (api->u_isdefined(c)) value |= defined;
        if (type == nonspacing_mark) value |= nonspacing;
        if (api->u_hasBinaryProperty(c, prop_diacritic)) value |= diacritic;
        if (type == spacing_mark) value |= vowel_mark;   // spacing combining marks (Indic dependent vowels)
        if (symbol_type) value |= symbol;
        for (std::size_t unit = 0; unit < units; ++unit) classes[at + unit] = value;
        at += units - 1;
    }
    return {};
}

Result<std::string> decode_mac_roman(std::string_view bytes) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    if (bytes.empty()) return std::string{};
    const auto size = size_of(bytes.size());
    if (!size) return size.error();
    UErrorCode error = 0;
    void* converter = api->ucnv_open("macos-0_2-10.2", &error);
    if (!converter || failed(error)) return Error::unsupported;
    auto text = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
        return api->ucnv_toUChars(converter, out, capacity, bytes.data(), *size, status);
    }, bytes.size() + 1);
    api->ucnv_close(converter);
    if (!text) return text.error();
    return utf8(*api, *text);
}

bool valid(std::string_view locale) {
    const auto* api = icu();
    if (!api || locale.empty()) return false;
    // A well-formed tag, entirely parsed, whose language some host locale has.
    const std::string tag(locale);
    std::array<char, 157> id{};
    std::int32_t parsed = 0;
    UErrorCode error = 0;
    const auto length = api->uloc_forLanguageTag(tag.c_str(), id.data(), static_cast<std::int32_t>(id.size()), &parsed, &error);
    if (failed(error) || length <= 0 || parsed != static_cast<std::int32_t>(tag.size())) return false;
    std::array<char, 16> language{};
    error = 0;
    if (api->uloc_getLanguage(id.data(), language.data(), static_cast<std::int32_t>(language.size()), &error) <= 0 || failed(error)) return false;
    for (std::int32_t index = 0, count = api->uloc_countAvailable(); index < count; ++index) {
        std::array<char, 16> other{};
        error = 0;
        api->uloc_getLanguage(api->uloc_getAvailable(index), other.data(), static_cast<std::int32_t>(other.size()), &error);
        if (!failed(error) && std::strcmp(other.data(), language.data()) == 0) return true;
    }
    return false;
}

Result<std::string> user_locale() {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    // ICU's default: LC_ALL, LC_MESSAGES or LANG; the POSIX ("C") locale is en_US_POSIX, which is en-US.
    auto tag = language_tag(*api, api->uloc_getDefault());
    if (const auto posix = tag.find("-u-va-posix"); posix != std::string::npos) tag.erase(posix);
    if (tag.empty() || tag == "und") return std::string("en-US");
    return tag;
}

Result<std::vector<std::string>> locales() {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    std::vector<std::string> names;
    for (std::int32_t index = 0, count = api->uloc_countAvailable(); index < count; ++index) {
        const char* id = api->uloc_getAvailable(index);
        auto tag = language_tag(*api, id);
        // No root, no keyword variants (en_US_POSIX).
        if (tag.empty() || tag == "und" || tag.find("-u-") != std::string::npos || tag.find("-x-") != std::string::npos) continue;
        names.push_back(tag);
        // ICU lists zh_Hans_CN; hosts and apps also name it zh-CN: a language-region name whose likely
        // script is the listed one.
        std::array<char, 16> language{}, script{}, region{};
        UErrorCode error = 0;
        api->uloc_getLanguage(id, language.data(), static_cast<std::int32_t>(language.size()), &error);
        api->uloc_getScript(id, script.data(), static_cast<std::int32_t>(script.size()), &error);
        api->uloc_getCountry(id, region.data(), static_cast<std::int32_t>(region.size()), &error);
        if (failed(error) || !script[0] || !region[0]) continue;
        const std::string short_id = std::string(language.data()) + "_" + region.data();
        std::array<char, 157> likely{};
        error = 0;
        const auto length = api->uloc_addLikelySubtags(short_id.c_str(), likely.data(), static_cast<std::int32_t>(likely.size()), &error);
        if (!failed(error) && length > 0 && std::string(likely.data(), static_cast<std::size_t>(length)) == id)
            names.push_back(std::string(language.data()) + "-" + region.data());
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

Result<std::string> text(std::string_view locale, Text field, unsigned index) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    const auto id = locale_id(*api, locale);
    const auto pattern_of = [&](std::int32_t time, std::int32_t date) -> Result<std::string> {
        const auto format = date_format(*api, id, time, date);
        if (!format.value) return Error::invalid_argument;
        auto value = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
            return api->udat_toPattern(format.value, 0, out, capacity, status);
        });
        if (!value) return value.error();
        return utf8(*api, *value);
    };
    const auto symbol_of = [&](std::int32_t type, std::int32_t at) -> Result<std::string> {
        const auto format = date_format(*api, id, dat_none, dat_short);
        if (!format.value) return Error::invalid_argument;
        auto value = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
            return api->udat_getSymbols(format.value, type, at, out, capacity, status);
        });
        if (!value) return value.error();
        return utf8(*api, *value);
    };
    const auto number_symbol = [&](std::int32_t style, std::int32_t symbol) -> Result<std::string> {
        const auto format = number_format(*api, id, style);
        if (!format.value) return Error::invalid_argument;
        auto value = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
            return api->unum_getSymbol(format.value, symbol, out, capacity, status);
        });
        if (!value) return value.error();
        return utf8(*api, *value);
    };
    const auto display = [&](auto call, const std::string& in) -> Result<std::string> {
        auto value = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) { return call(id.c_str(), in.c_str(), out, capacity, status); });
        if (!value) return value.error();
        return utf8(*api, *value);
    };
    const std::string user = api->uloc_getDefault();
    switch (field) {
    case Text::short_date_pattern: return pattern_of(dat_none, dat_short);
    case Text::long_date_pattern: return pattern_of(dat_none, dat_full);
    case Text::short_time_pattern: return pattern_of(dat_short, dat_none);
    case Text::long_time_pattern: return pattern_of(dat_medium, dat_none);
    case Text::month_name: if (index > 11) return Error::invalid_argument; return symbol_of(dat_months, static_cast<std::int32_t>(index));
    case Text::month_abbreviation: if (index > 11) return Error::invalid_argument; return symbol_of(dat_short_months, static_cast<std::int32_t>(index));
    // ICU's weekday symbols start at 1 (Sunday).
    case Text::day_name: if (index > 6) return Error::invalid_argument; return symbol_of(dat_weekdays, static_cast<std::int32_t>(index) + 1);
    case Text::day_abbreviation: if (index > 6) return Error::invalid_argument; return symbol_of(dat_short_weekdays, static_cast<std::int32_t>(index) + 1);
    case Text::day_shortest: if (index > 6) return Error::invalid_argument; return symbol_of(dat_shorter_weekdays, static_cast<std::int32_t>(index) + 1);
    case Text::am: return symbol_of(dat_am_pms, 0);
    case Text::pm: return symbol_of(dat_am_pms, 1);
    case Text::decimal_separator: return number_symbol(num_decimal, num_decimal_separator);
    case Text::grouping_separator: return number_symbol(num_decimal, num_grouping_separator);
    case Text::minus_sign: return number_symbol(num_decimal, num_minus);
    case Text::percent_sign: return number_symbol(num_decimal, num_percent);
    case Text::currency_symbol: return number_symbol(num_currency, num_currency_symbol);
    case Text::currency_code: return utf8(*api, currency_of(*api, id));
    case Text::english_currency_name: case Text::native_currency_name: {
        const auto code = currency_of(*api, id);
        if (code.empty()) return std::string{};
        return currency_long_name(*api, code, field == Text::english_currency_name ? std::string("en") : id);
    }
    case Text::english_display_name: return display(api->uloc_getDisplayName, "en");
    case Text::native_display_name: return display(api->uloc_getDisplayName, id);
    case Text::localized_display_name: return display(api->uloc_getDisplayName, user);
    case Text::english_language_name: return display(api->uloc_getDisplayLanguage, "en");
    case Text::native_language_name: return display(api->uloc_getDisplayLanguage, id);
    case Text::localized_language_name: return display(api->uloc_getDisplayLanguage, user);
    }
    return Error::invalid_argument;
}

Result<std::uint32_t> number(std::string_view locale, Number field) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    const auto id = locale_id(*api, locale);
    if (field == Number::currency_digits) {
        const auto code = currency_of(*api, id);
        if (code.empty()) return Error::not_found;
        UErrorCode error = 0;
        const auto digits = api->ucurr_getDefaultFractionDigits(code.c_str(), &error);
        if (failed(error) || digits < 0) return Error::invalid_argument;
        return static_cast<std::uint32_t>(digits);
    }
    UErrorCode error = 0;
    void* calendar = api->ucal_open(nullptr, 0, id.c_str(), cal_gregorian, &error);
    if (!calendar || failed(error)) return Error::invalid_argument;
    const auto value = api->ucal_getAttribute(calendar, field == Number::first_weekday ? cal_first_day : cal_minimal_days);
    api->ucal_close(calendar);
    return static_cast<std::uint32_t>(value);   // ICU counts weekdays 1 = Sunday, as the contract does
}

Result<std::string> format_number(std::string_view, std::string_view plain, const Format& format) {
    const auto value = parse_plain(plain);
    if (!value) return Error::invalid_argument;
    auto text = layout_number(*value, format.digits, format.decimal, format.grouping);
    // Windows' NegativeOrder 1, as the Windows backend asks: "-1.1".
    if (value->negative && text.find_first_not_of("0.,") != std::string::npos) text.insert(0, "-");
    return text;
}

Result<std::string> format_currency(std::string_view locale, std::string_view plain, const Format& format, std::string_view symbol) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    const auto value = parse_plain(plain);
    if (!value) return Error::invalid_argument;
    const auto digits = layout_number(*value, format.digits, format.decimal, format.grouping);
    // The locale's currency pattern: its positive and (if it has one) negative subpatterns, with
    // the number part (#, 0, separators) replaced by the digits and '¤' by the symbol.
    const auto id = locale_id(*api, locale);
    const auto currency = number_format(*api, id, num_currency);
    std::string pattern = "\xc2\xa4#,##0.00";
    if (currency.value) {
        if (auto text = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
                return api->unum_toPattern(currency.value, 0, out, capacity, status);
            })) pattern = utf8(*api, *text);
    }
    const auto semicolon = pattern.find(';');
    const bool negative = value->negative && digits.find_first_not_of("0.,") != std::string::npos;
    std::string sub = semicolon == std::string::npos ? pattern : negative ? pattern.substr(semicolon + 1) : pattern.substr(0, semicolon);
    if (negative && semicolon == std::string::npos) sub.insert(0, "-");
    const auto first = sub.find_first_of("#0"), last = sub.find_last_of("#0");
    if (first == std::string::npos) return Error::invalid_argument;
    std::string out = sub.substr(0, first) + digits + sub.substr(last + 1);
    for (std::size_t at; (at = out.find("\xc2\xa4")) != std::string::npos;) out.replace(at, 2, symbol);
    std::erase(out, '\'');
    return out;
}

Result<std::string> region_name(std::string_view region) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    if (region.empty()) return Error::invalid_argument;
    const auto id = "und_" + std::string(region);
    const std::string user = api->uloc_getDefault();
    auto value = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) {
        return api->uloc_getDisplayCountry(id.c_str(), user.c_str(), out, capacity, status);
    });
    if (!value) return value.error();
    // ICU echoes an unknown code: no name.
    const auto name = utf8(*api, *value);
    if (name.empty() || name == region) return Error::not_found;
    return name;
}

Result<std::string> currency_name(std::string_view code, bool english) {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    const auto wanted = utf16(*api, code);
    // As on Windows: the name a host locale that uses `code` gives it; empty when none does.
    for (std::int32_t index = 0, count = api->uloc_countAvailable(); index < count; ++index) {
        const std::string id = api->uloc_getAvailable(index);
        std::array<char, 8> region{};
        UErrorCode error = 0;
        if (api->uloc_getCountry(id.c_str(), region.data(), static_cast<std::int32_t>(region.size()), &error) <= 0 || failed(error)) continue;
        if (currency_of(*api, id) != wanted) continue;
        return currency_long_name(*api, wanted, english ? std::string("en") : id);
    }
    return std::string{};
}

Result<std::string> time_zone() {
    const auto* api = icu();
    if (!api) return Error::unsupported;
    // ICU reads TZ and /etc/localtime; the zone it cannot place is "Etc/Unknown".
    auto value = uchars([&](UChar* out, std::int32_t capacity, UErrorCode* status) { return api->ucal_getDefaultTimeZone(out, capacity, status); });
    if (!value) return value.error();
    const auto name = utf8(*api, *value);
    if (name.empty() || name == "Etc/Unknown") return Error::not_found;
    return name;
}

} // namespace alchemy::host::locale
