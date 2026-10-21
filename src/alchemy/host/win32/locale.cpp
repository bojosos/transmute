#include "alchemy/host/locale.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

#pragma comment(lib, "Normaliz.lib")

namespace alchemy::host::locale {
namespace {

// "" is the user's default locale, "und" the invariant one; other names are BCP 47 tags.
class Name {
public:
    explicit Name(std::string_view locale) : invariant_(locale == "und"), wide_(win32::widen(locale)) {}
    [[nodiscard]] LPCWSTR get() const noexcept {
        if (invariant_) return LOCALE_NAME_INVARIANT;
        return wide_.empty() ? LOCALE_NAME_USER_DEFAULT : wide_.c_str();
    }

private:
    bool invariant_{};
    std::wstring wide_;
};

[[nodiscard]] LPCWSTR wide(std::u16string_view text) noexcept { return reinterpret_cast<LPCWSTR>(text.data()); }
[[nodiscard]] Result<int> length(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) return Error::out_of_range;
    return static_cast<int>(size);
}

DWORD flags_of(std::uint32_t options) {
    DWORD flags = 0;
    if (options & ignore_case) flags |= NORM_IGNORECASE;
    if (options & ignore_case_linguistic) flags |= LINGUISTIC_IGNORECASE;
    if (options & ignore_nonspace) flags |= NORM_IGNORENONSPACE;
    if (options & ignore_diacritics) flags |= LINGUISTIC_IGNOREDIACRITIC;
    if (options & ignore_width) flags |= NORM_IGNOREWIDTH;
    if (options & ignore_kana_type) flags |= NORM_IGNOREKANATYPE;
    if (options & ignore_symbols) flags |= NORM_IGNORESYMBOLS;
    if (options & digits_as_numbers) flags |= SORT_DIGITSASNUMBERS;
    return flags;
}

// LCMapStringEx of text.
Result<std::u16string> lcmap(LPCWSTR locale, DWORD flags, std::u16string_view text) {
    if (text.empty()) return std::u16string{};
    const auto size = length(text.size());
    if (!size) return size.error();
    const auto needed = LCMapStringEx(locale, flags, wide(text), *size, nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) return win32::last_error();
    std::u16string out(static_cast<std::size_t>(needed), u'\0');
    const auto written = LCMapStringEx(locale, flags, wide(text), *size, reinterpret_cast<LPWSTR>(out.data()), needed, nullptr, nullptr, 0);
    if (written <= 0) return win32::last_error();
    out.resize(static_cast<std::size_t>(written));
    return out;
}

// GetLocaleInfoEx text ("" when the locale leaves the field empty).
Result<std::string> info(LPCWSTR locale, LCTYPE type) {
    const auto needed = GetLocaleInfoEx(locale, type, nullptr, 0);
    if (needed <= 0) return win32::last_error();
    std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
    const auto written = GetLocaleInfoEx(locale, type, buffer.data(), needed);
    if (written <= 0) return win32::last_error();
    buffer.resize(static_cast<std::size_t>(written - 1));
    return win32::narrow(buffer);
}
Result<DWORD> info_number(LPCWSTR locale, LCTYPE type) {
    DWORD value{};
    if (!GetLocaleInfoEx(locale, type | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&value), sizeof(value) / sizeof(wchar_t)))
        return win32::last_error();
    return value;
}

// Windows date/time picture -> ICU pattern (d dd ddd dddd M.. y.. g h H m s t tt and quotes).
std::string icu_pattern(std::string_view picture) {
    std::string out;
    for (std::size_t i = 0; i < picture.size();) {
        const char c = picture[i];
        if (c == '\'') {
            const auto end = picture.find('\'', i + 1);
            const auto literal = picture.substr(i + 1, (end == std::string_view::npos ? picture.size() : end) - i - 1);
            if (literal.empty()) out += "''";
            else { out += '\''; for (const auto l : literal) { out += l; if (l == '\'') out += '\''; } out += '\''; }
            i = end == std::string_view::npos ? picture.size() : end + 1;
            continue;
        }
        std::size_t run = 1;
        while (i + run < picture.size() && picture[i + run] == c) ++run;
        switch (c) {
        case 'd': out += run >= 4 ? "EEEE" : run == 3 ? "EEE" : std::string(run, 'd'); break;
        case 'y': out += run == 2 ? "yy" : run == 1 ? "y" : "y"; break;
        case 'g': out += "G"; break;
        case 't': out += "a"; break;
        case 'M': case 'h': case 'H': case 'm': case 's': out += std::string(run, c); break;
        default:
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { out += '\''; out += std::string(run, c); out += '\''; }
            else out += std::string(run, c);
        }
        i += run;
    }
    return out;
}

std::optional<LCTYPE> lctype(Text field, unsigned index) {
    const auto month = [&](LCTYPE first) -> std::optional<LCTYPE> {
        if (index > 11) return std::nullopt;
        return static_cast<LCTYPE>(first + index);
    };
    // LOCALE_SDAYNAME1 is Monday; index 0 is Sunday.
    const auto day = [&](LCTYPE first) -> std::optional<LCTYPE> {
        if (index > 6) return std::nullopt;
        return static_cast<LCTYPE>(first + (index + 6) % 7);
    };
    switch (field) {
    case Text::short_date_pattern: return LOCALE_SSHORTDATE;
    case Text::long_date_pattern: return LOCALE_SLONGDATE;
    case Text::short_time_pattern: return LOCALE_SSHORTTIME;
    case Text::long_time_pattern: return LOCALE_STIMEFORMAT;
    case Text::month_name: return month(LOCALE_SMONTHNAME1);
    case Text::month_abbreviation: return month(LOCALE_SABBREVMONTHNAME1);
    case Text::day_name: return day(LOCALE_SDAYNAME1);
    case Text::day_abbreviation: return day(LOCALE_SABBREVDAYNAME1);
    case Text::day_shortest: return day(LOCALE_SSHORTESTDAYNAME1);
    case Text::am: return LOCALE_SAM;
    case Text::pm: return LOCALE_SPM;
    case Text::decimal_separator: return LOCALE_SDECIMAL;
    case Text::grouping_separator: return LOCALE_STHOUSAND;
    case Text::minus_sign: return LOCALE_SNEGATIVESIGN;
    case Text::percent_sign: return LOCALE_SPERCENT;
    case Text::currency_symbol: return LOCALE_SCURRENCY;
    case Text::currency_code: return LOCALE_SINTLSYMBOL;
    case Text::english_currency_name: return LOCALE_SENGCURRNAME;
    case Text::native_currency_name: return LOCALE_SNATIVECURRNAME;
    case Text::english_display_name: return LOCALE_SENGLISHDISPLAYNAME;
    case Text::native_display_name: return LOCALE_SNATIVEDISPLAYNAME;
    case Text::localized_display_name: return LOCALE_SLOCALIZEDDISPLAYNAME;
    case Text::english_language_name: return LOCALE_SENGLISHLANGUAGENAME;
    case Text::native_language_name: return LOCALE_SNATIVELANGUAGENAME;
    case Text::localized_language_name: return LOCALE_SLOCALIZEDLANGUAGENAME;
    }
    return std::nullopt;
}

bool pattern(Text field) {
    return field == Text::short_date_pattern || field == Text::long_date_pattern || field == Text::short_time_pattern ||
           field == Text::long_time_pattern;
}

// GetNumberFormatEx / GetCurrencyFormatEx output.
Result<std::string> formatted(int size, const std::array<wchar_t, 128>& out) {
    if (size <= 1) return size == 1 ? Error::invalid_argument : win32::last_error();
    return win32::narrow(std::wstring_view(out.data(), static_cast<std::size_t>(size - 1)));
}

} // namespace

Result<std::u16string> normalize(std::u16string_view text, Form form) {
    if (text.empty()) return std::u16string{};
    const auto size = length(text.size());
    if (!size) return size.error();
    const auto norm = form == Form::c ? NormalizationC : form == Form::d ? NormalizationD : form == Form::kc ? NormalizationKC : NormalizationKD;
    // The first answer is an estimate; a short buffer answers a better one.
    auto estimate = NormalizeString(norm, wide(text), *size, nullptr, 0);
    for (int attempt = 0; attempt < 8 && estimate > 0; ++attempt) {
        std::u16string out(static_cast<std::size_t>(estimate), u'\0');
        const auto written = NormalizeString(norm, wide(text), *size, reinterpret_cast<LPWSTR>(out.data()), estimate);
        if (written > 0) {
            out.resize(static_cast<std::size_t>(written));
            return out;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        estimate = -written;
    }
    return GetLastError() == ERROR_NO_UNICODE_TRANSLATION ? Error::invalid_argument : win32::last_error();
}

Result<int> compare(std::string_view locale, std::u16string_view left, std::u16string_view right, std::uint32_t options) {
    const auto left_size = length(left.size()), right_size = length(right.size());
    if (!left_size || !right_size) return Error::out_of_range;
    const Name name(locale);
    const auto value = CompareStringEx(name.get(), flags_of(options), wide(left), *left_size, wide(right), *right_size, nullptr, nullptr, 0);
    if (value == 0) return win32::last_error();
    return value - CSTR_EQUAL;
}

Result<std::vector<std::uint8_t>> sort_key(std::string_view locale, std::u16string_view text, std::uint32_t options) {
    const Name name(locale);
    const auto size = length(text.size());
    if (!size) return size.error();
    const auto flags = LCMAP_SORTKEY | flags_of(options);
    const auto needed = LCMapStringEx(name.get(), flags, wide(text), *size, nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) return win32::last_error();
    std::vector<std::uint8_t> key(static_cast<std::size_t>(needed));
    const auto written = LCMapStringEx(name.get(), flags, wide(text), *size, reinterpret_cast<LPWSTR>(key.data()), needed, nullptr, nullptr, 0);
    if (written <= 0) return win32::last_error();
    key.resize(static_cast<std::size_t>(written));
    return key;
}

Result<std::u16string> map(std::string_view locale, std::u16string_view text, Mapping mapping) {
    const Name name(locale);
    const DWORD flags = mapping == Mapping::lower ? LCMAP_LOWERCASE : mapping == Mapping::upper ? LCMAP_UPPERCASE
                      : mapping == Mapping::linguistic_lower ? LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING
                      : mapping == Mapping::linguistic_upper ? LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING
                      : mapping == Mapping::half_width ? LCMAP_HALFWIDTH : LCMAP_FULLWIDTH;
    return lcmap(name.get(), flags, text);
}

Result<Match> find(std::string_view locale, std::u16string_view text, std::u16string_view pattern, Search where, std::uint32_t options) {
    const auto text_size = length(text.size()), pattern_size = length(pattern.size());
    if (!text_size || !pattern_size) return Error::out_of_range;
    const Name name(locale);
    DWORD flags = where == Search::first ? FIND_FROMSTART : where == Search::last ? FIND_FROMEND
                : where == Search::prefix ? FIND_STARTSWITH : FIND_ENDSWITH;
    flags |= flags_of(options);
    int matched = 0;
    const auto index = FindNLSStringEx(name.get(), flags, wide(text), *text_size, wide(pattern), *pattern_size, &matched, nullptr, nullptr, 0);
    if (index < 0) return GetLastError() == ERROR_SUCCESS ? Error::not_found : win32::last_error();
    return Match{static_cast<std::size_t>(index), static_cast<std::size_t>(matched)};
}

Status classify(std::u16string_view text, std::span<std::uint16_t> classes) {
    if (classes.size() < text.size()) return Error::invalid_argument;
    if (text.empty()) return {};
    const auto size = length(text.size());
    if (!size) return size.error();
    std::vector<WORD> type1(text.size()), type3(text.size());
    if (!GetStringTypeW(CT_CTYPE1, wide(text), *size, type1.data()) || !GetStringTypeW(CT_CTYPE3, wide(text), *size, type3.data()))
        return win32::last_error();
    for (std::size_t at = 0; at < text.size(); ++at) {
        const auto t1 = type1[at], t3 = type3[at];
        std::uint16_t value = 0;
        if (t1 & C1_UPPER) value |= upper;
        if (t1 & C1_LOWER) value |= lower;
        if (t1 & C1_DIGIT) value |= digit;
        if (t1 & C1_SPACE) value |= space;
        if (t1 & C1_PUNCT) value |= punctuation;
        if (t1 & C1_CNTRL) value |= control;
        if (t1 & C1_BLANK) value |= blank;
        if (t1 & C1_XDIGIT) value |= hex_digit;
        if (t1 & C1_ALPHA) value |= alpha;
        if (t1 & C1_DEFINED) value |= defined;
        if (t3 & C3_NONSPACING) value |= nonspacing;
        if (t3 & C3_DIACRITIC) value |= diacritic;
        if (t3 & C3_VOWELMARK) value |= vowel_mark;
        if (t3 & C3_SYMBOL) value |= symbol;
        classes[at] = value;
    }
    return {};
}

Result<std::string> decode_mac_roman(std::string_view bytes) {
    if (bytes.empty()) return std::string{};
    const auto size = length(bytes.size());
    if (!size) return size.error();
    constexpr UINT mac_roman = 10000;
    const auto count = MultiByteToWideChar(mac_roman, 0, bytes.data(), *size, nullptr, 0);
    if (count <= 0) return win32::last_error();
    std::wstring wide_text(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(mac_roman, 0, bytes.data(), *size, wide_text.data(), count) <= 0) return win32::last_error();
    return win32::narrow(wide_text);
}

bool valid(std::string_view locale) {
    if (locale.empty()) return false;
    return IsValidLocaleName(win32::widen(locale).c_str()) != FALSE;
}

Result<std::string> user_locale() {
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> name{};
    const auto size = GetUserDefaultLocaleName(name.data(), static_cast<int>(name.size()));
    if (size <= 0) return win32::last_error();
    auto value = win32::narrow(std::wstring_view(name.data(), static_cast<std::size_t>(size - 1)));
    if (const auto sort = value.find('_'); sort != std::string::npos) value.resize(sort); // Windows sort-order suffix.
    return value;
}

Result<std::vector<std::string>> locales() {
    std::vector<std::string> names;
    const auto collect = [](LPWSTR name, DWORD, LPARAM context) -> BOOL {
        const std::wstring_view value(name);
        // The invariant locale is the root; "de-DE_phoneb" is an alternate sort order of de-DE.
        if (!value.empty() && value.find(L'_') == std::wstring_view::npos)
            reinterpret_cast<std::vector<std::string>*>(context)->push_back(win32::narrow(value));
        return TRUE;
    };
    if (!EnumSystemLocalesEx(collect, LOCALE_WINDOWS, reinterpret_cast<LPARAM>(&names), nullptr)) return win32::last_error();
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

Result<std::string> text(std::string_view locale, Text field, unsigned index) {
    const auto type = lctype(field, index);
    if (!type) return Error::invalid_argument;
    const Name name(locale);
    auto value = info(name.get(), *type);
    if (value && pattern(field)) return icu_pattern(*value);
    return value;
}

Result<std::uint32_t> number(std::string_view locale, Number field) {
    const Name name(locale);
    switch (field) {
    case Number::currency_digits: {
        const auto value = info_number(name.get(), LOCALE_ICURRDIGITS);
        if (!value) return value.error();
        return static_cast<std::uint32_t>(*value);
    }
    case Number::first_weekday: {
        const auto value = info_number(name.get(), LOCALE_IFIRSTDAYOFWEEK);   // 0 = Monday
        if (!value) return value.error();
        return static_cast<std::uint32_t>((*value + 1) % 7 + 1);
    }
    case Number::minimum_days_in_first_week: {
        const auto value = info_number(name.get(), LOCALE_IFIRSTWEEKOFYEAR);
        if (!value) return value.error();
        return *value == 1 ? 7u : *value == 2 ? 4u : 1u;
    }
    }
    return Error::invalid_argument;
}

Result<std::string> format_number(std::string_view locale, std::string_view plain, const Format& format) {
    const Name name(locale);
    const auto decimal = win32::widen(format.decimal), grouping = win32::widen(format.grouping), value = win32::widen(plain);
    NUMBERFMTW layout{};
    layout.NumDigits = format.digits;
    layout.LeadingZero = 1;
    layout.Grouping = grouping.empty() ? 0 : 3;
    layout.lpDecimalSep = const_cast<wchar_t*>(decimal.c_str());
    layout.lpThousandSep = const_cast<wchar_t*>(grouping.c_str());
    layout.NegativeOrder = 1;
    std::array<wchar_t, 128> out{};
    return formatted(GetNumberFormatEx(name.get(), 0, value.c_str(), &layout, out.data(), static_cast<int>(out.size())), out);
}

Result<std::string> format_currency(std::string_view locale, std::string_view plain, const Format& format, std::string_view symbol) {
    const Name name(locale);
    const auto decimal = win32::widen(format.decimal), grouping = win32::widen(format.grouping), value = win32::widen(plain);
    const auto currency = win32::widen(symbol);
    CURRENCYFMTW layout{};
    layout.NumDigits = format.digits;
    layout.LeadingZero = 1;
    layout.Grouping = 3;
    layout.lpDecimalSep = const_cast<wchar_t*>(decimal.c_str());
    layout.lpThousandSep = const_cast<wchar_t*>(grouping.c_str());
    layout.NegativeOrder = info_number(name.get(), LOCALE_INEGCURR).value_or(0);
    layout.PositiveOrder = info_number(name.get(), LOCALE_ICURRENCY).value_or(0);
    layout.lpCurrencySymbol = const_cast<wchar_t*>(currency.c_str());
    std::array<wchar_t, 128> out{};
    return formatted(GetCurrencyFormatEx(name.get(), 0, value.c_str(), &layout, out.data(), static_cast<int>(out.size())), out);
}

Result<std::string> region_name(std::string_view region) {
    auto wide_region = win32::widen(region);
    const auto needed = GetGeoInfoEx(wide_region.data(), GEO_FRIENDLYNAME, nullptr, 0);
    if (needed <= 0) return win32::last_error();
    std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
    const auto written = GetGeoInfoEx(wide_region.data(), GEO_FRIENDLYNAME, buffer.data(), needed);
    if (written <= 0) return win32::last_error();
    buffer.resize(static_cast<std::size_t>(written - 1));
    return win32::narrow(buffer);
}

Result<std::string> currency_name(std::string_view code, bool english) {
    // A host locale whose currency is `code` names it.
    struct Search {
        std::wstring code;
        std::string name;
        bool english;
    } search{win32::widen(code), {}, english};
    EnumSystemLocalesEx([](LPWSTR locale, DWORD, LPARAM context) -> BOOL {
        auto& found = *reinterpret_cast<Search*>(context);
        std::array<wchar_t, 16> symbol{};
        if (GetLocaleInfoEx(locale, LOCALE_SINTLSYMBOL, symbol.data(), static_cast<int>(symbol.size())) > 1 && found.code == symbol.data()) {
            found.name = info(locale, found.english ? LOCALE_SENGCURRNAME : LOCALE_SNATIVECURRNAME).value_or({});
            return FALSE;
        }
        return TRUE;
    }, LOCALE_SPECIFICDATA, reinterpret_cast<LPARAM>(&search), nullptr);
    return search.name;
}

Result<std::string> time_zone() {
    // ICU in Windows 10 1903+ knows the IANA identifier of the system zone.
    using Getter = std::int32_t(__cdecl*)(wchar_t*, std::int32_t, int*);
    const auto icu = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!icu) return Error::unsupported;
    const auto get = reinterpret_cast<Getter>(GetProcAddress(icu, "ucal_getDefaultTimeZone"));
    if (!get) return Error::unsupported;
    std::array<wchar_t, 128> buffer{};
    int status = 0;
    const auto length = get(buffer.data(), static_cast<std::int32_t>(buffer.size() - 1), &status);
    if (status > 0 || length <= 0) return Error::not_found;
    std::string name;
    for (std::int32_t index = 0; index < length; ++index) name.push_back(static_cast<char>(buffer[static_cast<std::size_t>(index)]));
    return name;
}

} // namespace alchemy::host::locale
