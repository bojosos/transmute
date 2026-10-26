#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/host/locale.hpp"
#include "alchemy/object_store.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Locale-dependent CoreFoundation formatting Qt Core's system locale uses:
// CFLocaleCopyDisplayNameForPropertyValue, CFDateFormatter
// (style patterns, symbol properties, formatting a CFDate) and
// CFNumberFormatter (styles, properties, formatting numbers). CFCalendar is
// NSCalendar (foundation/calendar_provider.cpp).
//
// The locale data comes from the host (host::locale) for the CFLocale's
// identifier (en_US -> "en-US"): month and day names, AM/PM, date and time
// patterns (ICU/CLDR), separators, currency formats, first day of week.
// Display names use the host's English, native or UI-language names,
// whichever matches the display locale. Dates are formatted in the default time zone through the
// host time-zone database. Spell-out number formatting and non-Gregorian
// calendars are unsupported and fail explicitly.
namespace alchemy::export_fill {
namespace {

namespace locale = host::locale;

// Locale text; empty when the locale or field is unknown.
std::string info(const std::string& name, locale::Text field, unsigned index = 0) { return locale::text(name, field, index).value_or({}); }

// CFLocale identifier ("de_DE", "zh_Hans_CN") to a host locale name ("de-DE").
std::string host_locale_name(const std::string& identifier) {
    std::string name = identifier.empty() ? "en_US" : identifier;
    if (const auto at = name.find('@'); at != std::string::npos) name.resize(at);
    for (auto& c : name) if (c == '_') c = '-';
    if (locale::valid(name)) return name;
    const auto dash = name.find('-');
    return dash == std::string::npos ? "en-US" : name.substr(0, dash);
}

// The host's locales and the codes in their names, read once.
struct LocaleLists {
    std::vector<std::string> names;                           // BCP 47 ("de-DE")
    std::set<std::string> languages, regions, currencies;
};

const LocaleLists& locale_lists() {
    static const LocaleLists lists = [] {
        const auto names = locale::locales();
        if (!names) throw std::runtime_error("Alchemy CFLocale lists require the host locale database");
        LocaleLists result;
        result.names = *names;
        const auto letters = [](std::string_view text, bool upper) {
            return std::all_of(text.begin(), text.end(), [upper](char c) { return upper ? c >= 'A' && c <= 'Z' : c >= 'a' && c <= 'z'; });
        };
        for (const auto& name : result.names) {
            std::vector<std::string_view> tags;
            for (std::string_view rest = name; !rest.empty();) {
                const auto dash = rest.find('-');
                tags.push_back(rest.substr(0, dash));
                rest = dash == std::string_view::npos ? std::string_view() : rest.substr(dash + 1);
            }
            if (tags.empty() || tags[0].size() < 2 || tags[0].size() > 3 || !letters(tags[0], false)) continue;
            result.languages.emplace(tags[0]);
            for (std::size_t index = 1; index < tags.size(); ++index) {
                if (tags[index].size() != 2 || !letters(tags[index], true)) continue;   // a script, a UN M.49 area or a variant
                result.regions.emplace(tags[index]);
                const auto currency = locale::text(name, locale::Text::currency_code).value_or(std::string());
                if (currency.size() == 3 && letters(currency, true)) result.currencies.insert(currency);
                break;
            }
        }
        return result;
    }();
    return lists;
}

struct LocaleData {
    std::string name;
    std::string date_short, date_long, time_short, time_long;
    std::array<std::string, 12> months, short_months;
    std::array<std::string, 7> days, short_days, shortest_days;   // Sunday first
    std::string am{"AM"}, pm{"PM"};
    std::string decimal{"."}, grouping{","}, minus{"-"}, percent{"%"}, currency_symbol{"$"};
    std::uint32_t currency_digits{2};
    std::uint32_t first_weekday{1}, minimum_days{1};   // CF numbering (1 = Sunday)
};

LocaleData load_locale(const std::string& identifier) {
    LocaleData data;
    data.name = host_locale_name(identifier);
    const auto& n = data.name;
    if (const auto available = locale::text(n, locale::Text::short_date_pattern); !available && available.error() == host::Error::unsupported)
        throw std::runtime_error("Alchemy CFLocale formatting requires the host locale database");
    data.date_short = info(n, locale::Text::short_date_pattern);
    data.date_long = info(n, locale::Text::long_date_pattern);
    data.time_short = info(n, locale::Text::short_time_pattern);
    data.time_long = info(n, locale::Text::long_time_pattern);
    for (unsigned i = 0; i < 12; ++i) {
        data.months[i] = info(n, locale::Text::month_name, i);
        data.short_months[i] = info(n, locale::Text::month_abbreviation, i);
    }
    for (unsigned i = 0; i < 7; ++i) {   // Sunday first
        data.days[i] = info(n, locale::Text::day_name, i);
        data.short_days[i] = info(n, locale::Text::day_abbreviation, i);
        data.shortest_days[i] = info(n, locale::Text::day_shortest, i);
    }
    if (auto value = info(n, locale::Text::am); !value.empty()) data.am = value;
    if (auto value = info(n, locale::Text::pm); !value.empty()) data.pm = value;
    if (auto value = info(n, locale::Text::decimal_separator); !value.empty()) data.decimal = value;
    data.grouping = info(n, locale::Text::grouping_separator);
    if (auto value = info(n, locale::Text::minus_sign); !value.empty()) data.minus = value;
    if (auto value = info(n, locale::Text::percent_sign); !value.empty()) data.percent = value;
    if (auto value = info(n, locale::Text::currency_symbol); !value.empty()) data.currency_symbol = value;
    data.currency_digits = locale::number(n, locale::Number::currency_digits).value_or(2);
    data.first_weekday = locale::number(n, locale::Number::first_weekday).value_or(1);
    data.minimum_days = locale::number(n, locale::Number::minimum_days_in_first_week).value_or(1);
    return data;
}

std::string locale_identifier(CoreFoundation& cf, std::uint64_t locale) {
    if (!locale) locale = cf.current_locale();
    return cf.utf8(cf.invoke("CFLocaleGetIdentifier", std::array<std::uint64_t, 1>{locale}));
}

// ---- date formatting ----
struct LocalTime {
    std::int64_t year{}; unsigned month{}, day{}, weekday{}, hour{}, minute{}, second{}, day_of_year{};
    double fraction{};
    std::int64_t offset{};   // seconds east of UTC
    std::string abbreviation;
};

LocalTime local_time(double absolute) {
    using namespace std::chrono;
    const auto unix_seconds = absolute + 978307200.0;
    const auto whole = static_cast<std::int64_t>(std::floor(unix_seconds));
    LocalTime out;
    out.fraction = unix_seconds - static_cast<double>(whole);
    const sys_seconds instant{seconds{whole}};
    local_seconds local{seconds{whole}};
    try {
        const auto* zone = current_zone();
        const auto zone_info = zone->get_info(instant);
        out.offset = zone_info.offset.count();
        out.abbreviation = zone_info.abbrev;
        local = zone->to_local(instant);
    } catch (const std::exception&) {
        out.abbreviation = "GMT";
    }
    const auto days = floor<std::chrono::days>(local);
    const year_month_day date{days};
    const hh_mm_ss<seconds> clock{local - days};
    out.year = static_cast<int>(date.year());
    out.month = static_cast<unsigned>(date.month());
    out.day = static_cast<unsigned>(date.day());
    out.weekday = weekday{days}.c_encoding();   // 0 = Sunday
    out.hour = static_cast<unsigned>(clock.hours().count());
    out.minute = static_cast<unsigned>(clock.minutes().count());
    out.second = static_cast<unsigned>(clock.seconds().count());
    out.day_of_year = static_cast<unsigned>((days - local_days{date.year() / January / 1}).count()) + 1;
    return out;
}

std::string padded(std::int64_t value, std::size_t width) {
    auto text = std::to_string(value < 0 ? -value : value);
    if (text.size() < width) text.insert(0, width - text.size(), '0');
    return (value < 0 ? "-" : "") + text;
}

std::string offset_text(std::int64_t offset, bool colon) {
    const auto magnitude = offset < 0 ? -offset : offset;
    return std::string(offset < 0 ? "-" : "+") + padded(magnitude / 3600, 2) + (colon ? ":" : "") + padded(magnitude / 60 % 60, 2);
}

std::string format_date(const LocaleData& data, std::string_view pattern, double absolute) {
    const auto t = local_time(absolute);
    std::string out;
    for (std::size_t i = 0; i < pattern.size();) {
        const char c = pattern[i];
        if (c == '\'') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '\'') { out += '\''; i += 2; continue; }
            const auto end = pattern.find('\'', i + 1);
            for (std::size_t j = i + 1; j < (end == std::string_view::npos ? pattern.size() : end); ++j) out += pattern[j];
            i = end == std::string_view::npos ? pattern.size() : end + 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { out += c; ++i; continue; }
        std::size_t n = 1;
        while (i + n < pattern.size() && pattern[i + n] == c) ++n;
        const auto hour12 = t.hour % 12 == 0 ? 12U : t.hour % 12;
        switch (c) {
        case 'G': out += t.year > 0 ? (n >= 4 ? "Anno Domini" : "AD") : (n >= 4 ? "Before Christ" : "BC"); break;
        case 'y': case 'Y': case 'u': out += n == 2 ? padded(t.year % 100, 2) : padded(t.year, n); break;
        case 'M': case 'L':
            if (n >= 4) out += data.months[t.month - 1];
            else if (n == 3) out += data.short_months[t.month - 1];
            else out += padded(t.month, n);
            break;
        case 'd': out += padded(t.day, n); break;
        case 'D': out += padded(t.day_of_year, n); break;
        case 'E': case 'e': case 'c':
            if ((c != 'E' && n <= 2)) out += padded(t.weekday == 0 ? 7 : t.weekday, n);
            else if (n >= 6) out += data.short_days[t.weekday];
            else if (n == 5) out += data.shortest_days[t.weekday];
            else if (n == 4) out += data.days[t.weekday];
            else out += data.short_days[t.weekday];
            break;
        case 'a': out += t.hour < 12 ? data.am : data.pm; break;
        case 'h': out += padded(hour12, n); break;
        case 'H': out += padded(t.hour, n); break;
        case 'k': out += padded(t.hour == 0 ? 24 : t.hour, n); break;
        case 'K': out += padded(t.hour % 12, n); break;
        case 'm': out += padded(t.minute, n); break;
        case 's': out += padded(t.second, n); break;
        case 'S': {
            auto digits = std::to_string(static_cast<std::int64_t>(std::floor(t.fraction * std::pow(10.0, static_cast<double>(n)))));
            if (digits.size() < n) digits.insert(0, n - digits.size(), '0');
            out += digits;
            break;
        }
        case 'z': case 'v': case 'V': out += t.abbreviation.empty() ? "GMT" + offset_text(t.offset, true) : t.abbreviation; break;
        case 'Z': out += n == 4 ? "GMT" + offset_text(t.offset, true) : n == 5 ? offset_text(t.offset, true) : offset_text(t.offset, false); break;
        case 'O': out += "GMT" + offset_text(t.offset, true); break;
        case 'x': case 'X': out += t.offset == 0 && c == 'X' ? "Z" : offset_text(t.offset, n >= 3); break;
        case 'Q': case 'q': out += n >= 3 ? "Q" + std::to_string((t.month - 1) / 3 + 1) : padded((t.month - 1) / 3 + 1, n); break;
        default: throw std::runtime_error("Alchemy CFDateFormatter: unsupported pattern field '" + std::string(n, c) + "'");
        }
        i += n;
    }
    return out;
}

struct DateFormatter : ObjectPayload {
    std::string identifier;
    LocaleData data;
    std::int64_t date_style{}, time_style{};
    std::uint64_t pattern{};   // CFString owned by the formatter (Get rule)
    std::string pattern_text;
};

struct NumberFormatter : ObjectPayload {
    std::string identifier;
    LocaleData data;
    std::int64_t style{};
    std::string currency_symbol;   // kCFNumberFormatterCurrencySymbol override
};


std::string style_pattern(const LocaleData& data, std::int64_t date_style, std::int64_t time_style) {
    std::string date, time;
    switch (date_style) {
    case 0: break;
    case 1: date = data.date_short; break;
    case 2: {
        // Medium: the long pattern with abbreviated month and without the weekday.
        date = data.date_long;
        for (auto at = date.find("MMMM"); at != std::string::npos; at = date.find("MMMM", at)) date.erase(at, 1);
        if (const auto at = date.find("EEEE"); at != std::string::npos) {
            auto end = at + 4;
            while (end < date.size() && (date[end] == ',' || date[end] == ' ')) ++end;
            date.erase(at, end - at);
        }
        break;
    }
    default: date = data.date_long; break;
    }
    switch (time_style) {
    case 0: break;
    case 1: time = data.time_short; break;
    case 2: time = data.time_long; break;
    case 3: time = data.time_long + " z"; break;
    default: time = data.time_long + " zzzz"; break;
    }
    if (date.empty()) return time;
    if (time.empty()) return date;
    return date + " " + time;
}

// Locale number formatting; `digits` < 0 keeps up to three significant fraction digits.
std::string format_number(const LocaleData& data, double value, int digits, bool grouping) {
    char plain[64];
    if (digits < 0) {
        std::snprintf(plain, sizeof(plain), "%.3f", value);
        std::string text = plain;
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
        digits = static_cast<int>(text.find('.') == std::string::npos ? 0 : text.size() - text.find('.') - 1);
    }
    std::snprintf(plain, sizeof(plain), "%.*f", digits, value);
    const auto result = locale::format_number(data.name, plain, {static_cast<unsigned>(digits), data.decimal, grouping ? data.grouping : std::string{}});
    if (!result) throw std::runtime_error("Alchemy CFNumberFormatter: the host could not format the number");
    return *result;
}
std::string format_currency(const LocaleData& data, double value, const std::string& symbol) {
    char plain[64];
    std::snprintf(plain, sizeof(plain), "%.*f", static_cast<int>(data.currency_digits), value);
    const auto result = locale::format_currency(data.name, plain, {data.currency_digits, data.decimal, data.grouping}, symbol);
    if (!result) throw std::runtime_error("Alchemy CFNumberFormatter: the host could not format the currency value");
    return *result;
}

} // namespace

void install_cf_locale(Provider& p, ObjcRuntime& objc, CoreFoundation& cf) {
    auto& process = p.process();
    const std::string cf_library(core_foundation_library);
    const auto function = [&](const char* name, transmute::GuestCallHandler handler) {
        if (provided(process, cf_library, name)) return;
        static_cast<void>(process.install_function(name, std::move(handler), cf_library));
    };
    const auto native_class = [&objc](const char* name) {
        if (const auto found = objc.find_class(name)) return found;
        return objc.register_native_class(name, objc.find_class("NSObject"), 16);
    };
    const auto date_formatter_class = native_class("__NSCFDateFormatter");
    const auto number_formatter_class = native_class("__NSCFNumberFormatter");
    const auto no_allocator = [](std::uint64_t allocator) {
        if (allocator) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
    };
    const auto payload = [&process]<class T>(std::uint64_t object, const char* what) {
        const auto record = object ? process.objects().find(object) : nullptr;
        auto value = record ? std::dynamic_pointer_cast<T>(record->payload) : nullptr;
        if (!value) throw std::runtime_error(std::string("Alchemy CoreFoundation: object is not a ") + what);
        return value;
    };
    const auto strings = [&process, &cf](const auto& values) {
        std::vector<std::uint64_t> items;
        for (const auto& value : values) items.push_back(cf.string(value));
        const auto array = cf.array(items);
        for (const auto item : items) process.objects().release(item);
        return array;
    };

    // ---- Locale lists ----
    // The host's locales stand in for ICU's: the available identifiers ("de", "de_DE", "zh_Hans") and
    // the ISO 639 language, ISO 3166 region and ISO 4217 currency codes they use, each sorted and unique.
    function("_CFLocaleCopyAvailableLocaleIdentifiers", [strings](CpuState& s, GuestMemory&) {
        std::vector<std::string> identifiers;
        for (auto name : locale_lists().names) {
            std::replace(name.begin(), name.end(), '-', '_');
            identifiers.push_back(std::move(name));
        }
        s.x[0] = strings(identifiers);
    });
    function("_CFLocaleCopyISOLanguageCodes", [strings](CpuState& s, GuestMemory&) { s.x[0] = strings(locale_lists().languages); });
    function("_CFLocaleCopyISOCountryCodes", [strings](CpuState& s, GuestMemory&) { s.x[0] = strings(locale_lists().regions); });
    for (const auto* name : {"_CFLocaleCopyISOCurrencyCodes", "_CFLocaleCopyCommonISOCurrencyCodes"})
        function(name, [strings](CpuState& s, GuestMemory&) { s.x[0] = strings(locale_lists().currencies); });

    // ---- CFLocaleCopyDisplayNameForPropertyValue ----
    // Names come from the host in English, natively, or in the host UI
    // language, chosen by the display locale's language.
    function("_CFLocaleCopyDisplayNameForPropertyValue", [&cf](CpuState& s, GuestMemory&) {
        if (const auto available = locale::user_locale(); !available && available.error() == host::Error::unsupported)
            throw std::runtime_error("Alchemy CFLocaleCopyDisplayNameForPropertyValue requires the host locale database");
        const auto display = locale_identifier(cf, s.x[0]);
        const auto key = cf.utf8(s.x[1]);
        const auto value = cf.utf8(s.x[2]);
        const auto display_language = display.substr(0, display.find('_'));
        std::string name;
        if (key == "locale:id" || key == "locale:language code") {
            const auto host_name = host_locale_name(value);
            const auto language = value.substr(0, value.find('_'));
            const bool identifier = key == "locale:id";
            const auto english = identifier ? locale::Text::english_display_name : locale::Text::english_language_name;
            const auto native = identifier ? locale::Text::native_display_name : locale::Text::native_language_name;
            const auto localized = identifier ? locale::Text::localized_display_name : locale::Text::localized_language_name;
            name = info(host_name, display_language == "en" ? english : display_language == language ? native : localized);
        } else if (key == "locale:country code") {
            name = locale::region_name(value).value_or({});
        } else if (key == "currencyCode") {
            name = locale::currency_name(value, display_language == "en").value_or({});
        }
        s.x[0] = name.empty() ? 0 : cf.string(name);
    });

    // ---- CFDateFormatter ----
    // CFDateFormatterRef CFDateFormatterCreate(alloc, CFLocaleRef, dateStyle, timeStyle)
    function("_CFDateFormatterCreate", [&process, &cf, date_formatter_class, no_allocator](CpuState& s, GuestMemory&) {
        no_allocator(s.x[0]);
        auto value = std::make_shared<DateFormatter>();
        value->identifier = locale_identifier(cf, s.x[1]);
        value->data = load_locale(value->identifier);
        value->date_style = static_cast<std::int64_t>(s.x[2]);
        value->time_style = static_cast<std::int64_t>(s.x[3]);
        value->pattern_text = style_pattern(value->data, value->date_style, value->time_style);
        value->pattern = cf.string(value->pattern_text);
        const auto pattern = value->pattern;
        s.x[0] = process.objects().create(16, date_formatter_class, ObjectKind::instance, std::move(value),
                                          [&process, pattern](std::uint64_t) { process.objects().release(pattern); });
    });
    const auto date_formatter = [payload](std::uint64_t object) { return payload.template operator()<DateFormatter>(object, "CFDateFormatter"); };
    function("_CFDateFormatterGetFormat", [date_formatter](CpuState& s, GuestMemory&) { s.x[0] = date_formatter(s.x[0])->pattern; });
    // void CFDateFormatterSetFormat(CFDateFormatterRef, CFStringRef)
    function("_CFDateFormatterSetFormat", [&process, &cf, date_formatter](CpuState& s, GuestMemory&) {
        auto value = date_formatter(s.x[0]);
        const auto text = cf.utf8(s.x[1]);
        const auto previous = value->pattern;
        value->pattern_text = text;
        value->pattern = cf.string(text);
        process.objects().release(previous);
        s.x[0] = 0;
    });
    function("_CFDateFormatterGetDateStyle", [date_formatter](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(date_formatter(s.x[0])->date_style); });
    function("_CFDateFormatterGetTimeStyle", [date_formatter](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(date_formatter(s.x[0])->time_style); });
    // CFTypeRef CFDateFormatterCopyProperty(CFDateFormatterRef, CFStringRef key)
    function("_CFDateFormatterCopyProperty", [&cf, date_formatter, strings](CpuState& s, GuestMemory&) {
        const auto value = date_formatter(s.x[0]);
        const auto& d = value->data;
        auto key = cf.utf8(s.x[1]);
        if (key.starts_with("kCFDateFormatter") && key.ends_with("Key")) key = key.substr(16, key.size() - 19);
        const auto first_letters = [](const auto& names) {
            std::vector<std::string> out;
            for (const auto& name : names) {
                // The first UTF-8 character.
                std::size_t length = 1;
                if (!name.empty()) { const auto c = static_cast<unsigned char>(name[0]); length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
                out.push_back(name.substr(0, length));
            }
            return out;
        };
        if (key == "MonthSymbols" || key == "StandaloneMonthSymbols") s.x[0] = strings(d.months);
        else if (key == "ShortMonthSymbols" || key == "ShortStandaloneMonthSymbols") s.x[0] = strings(d.short_months);
        else if (key == "VeryShortMonthSymbols" || key == "VeryShortStandaloneMonthSymbols") s.x[0] = strings(first_letters(d.months));
        else if (key == "WeekdaySymbols" || key == "StandaloneWeekdaySymbols") s.x[0] = strings(d.days);
        else if (key == "ShortWeekdaySymbols" || key == "ShortStandaloneWeekdaySymbols") s.x[0] = strings(d.short_days);
        else if (key == "VeryShortWeekdaySymbols" || key == "VeryShortStandaloneWeekdaySymbols") s.x[0] = strings(first_letters(d.days));
        else if (key == "AMSymbol") s.x[0] = cf.string(d.am);
        else if (key == "PMSymbol") s.x[0] = cf.string(d.pm);
        else if (key == "EraSymbols") s.x[0] = strings(std::array<std::string, 2>{"BC", "AD"});
        else if (key == "LongEraSymbols") s.x[0] = strings(std::array<std::string, 2>{"Before Christ", "Anno Domini"});
        else if (key == "QuarterSymbols") s.x[0] = strings(std::array<std::string, 4>{"1st quarter", "2nd quarter", "3rd quarter", "4th quarter"});
        else if (key == "ShortQuarterSymbols") s.x[0] = strings(std::array<std::string, 4>{"Q1", "Q2", "Q3", "Q4"});
        else s.x[0] = 0;
    });
    // CFStringRef CFDateFormatterCreateStringWithDate(alloc, CFDateFormatterRef, CFDateRef)
    function("_CFDateFormatterCreateStringWithDate", [&cf, date_formatter, no_allocator](CpuState& s, GuestMemory&) {
        no_allocator(s.x[0]);
        const auto value = date_formatter(s.x[1]);
        s.x[0] = cf.string(format_date(value->data, value->pattern_text, cf.date_time(s.x[2])));
    });
    // CFStringRef CFDateFormatterCreateStringWithAbsoluteTime(alloc, CFDateFormatterRef, CFAbsoluteTime d0)
    function("_CFDateFormatterCreateStringWithAbsoluteTime", [&cf, date_formatter, no_allocator](CpuState& s, GuestMemory&) {
        no_allocator(s.x[0]);
        const auto value = date_formatter(s.x[1]);
        s.x[0] = cf.string(format_date(value->data, value->pattern_text, std::bit_cast<double>(s.v[0][0])));
    });

    // ---- CFNumberFormatter ----
    // CFNumberFormatterRef CFNumberFormatterCreate(alloc, CFLocaleRef, CFNumberFormatterStyle)
    function("_CFNumberFormatterCreate", [&process, &cf, number_formatter_class, no_allocator](CpuState& s, GuestMemory&) {
        no_allocator(s.x[0]);
        const auto style = static_cast<std::int64_t>(s.x[2]);
        if (style < 0 || style > 4) throw std::runtime_error("Alchemy CFNumberFormatter: spell-out and ordinal styles are unsupported");
        auto value = std::make_shared<NumberFormatter>();
        value->identifier = locale_identifier(cf, s.x[1]);
        value->data = load_locale(value->identifier);
        value->style = style;
        value->currency_symbol = value->data.currency_symbol;
        s.x[0] = process.objects().create(16, number_formatter_class, ObjectKind::instance, std::move(value));
    });
    const auto number_formatter = [payload](std::uint64_t object) { return payload.template operator()<NumberFormatter>(object, "CFNumberFormatter"); };
    // void CFNumberFormatterSetProperty(CFNumberFormatterRef, CFStringRef key, CFTypeRef value)
    function("_CFNumberFormatterSetProperty", [&cf, number_formatter](CpuState& s, GuestMemory&) {
        auto value = number_formatter(s.x[0]);
        const auto key = cf.utf8(s.x[1]);
        if (key == "kCFNumberFormatterCurrencySymbolKey") value->currency_symbol = cf.utf8(s.x[2]);
        else if (key == "kCFNumberFormatterDecimalSeparatorKey") value->data.decimal = cf.utf8(s.x[2]);
        else if (key == "kCFNumberFormatterGroupingSeparatorKey") value->data.grouping = cf.utf8(s.x[2]);
        else throw std::runtime_error("Alchemy CFNumberFormatterSetProperty: unsupported property " + key);
        s.x[0] = 0;
    });
    // CFTypeRef CFNumberFormatterCopyProperty(CFNumberFormatterRef, CFStringRef key)
    function("_CFNumberFormatterCopyProperty", [&cf, number_formatter](CpuState& s, GuestMemory&) {
        const auto value = number_formatter(s.x[0]);
        const auto key = cf.utf8(s.x[1]);
        if (key == "kCFNumberFormatterCurrencySymbolKey") s.x[0] = cf.string(value->currency_symbol);
        else if (key == "kCFNumberFormatterDecimalSeparatorKey") s.x[0] = cf.string(value->data.decimal);
        else if (key == "kCFNumberFormatterGroupingSeparatorKey") s.x[0] = cf.string(value->data.grouping);
        else if (key == "kCFNumberFormatterMinusSignKey") s.x[0] = cf.string(value->data.minus);
        else if (key == "kCFNumberFormatterPercentSymbolKey") s.x[0] = cf.string(value->data.percent);
        else s.x[0] = 0;
    });
    const auto format = [](const NumberFormatter& formatter, double value) -> std::string {
        switch (formatter.style) {
        case 0: return format_number(formatter.data, std::nearbyint(value), 0, false);
        case 1: return format_number(formatter.data, value, -1, true);
        case 2: return format_currency(formatter.data, value, formatter.currency_symbol);
        case 3: return format_number(formatter.data, std::nearbyint(value * 100), 0, true) + formatter.data.percent;
        case 4: {
            char text[64];
            std::snprintf(text, sizeof(text), "%.6E", value);
            std::string out = text;
            // ICU "#E0": mantissa without trailing zeros, exponent without padding.
            const auto e = out.find('E');
            auto mantissa = out.substr(0, e);
            while (mantissa.back() == '0') mantissa.pop_back();
            if (mantissa.back() == '.') mantissa.pop_back();
            if (const auto point = mantissa.find('.'); point != std::string::npos) mantissa.replace(point, 1, formatter.data.decimal);
            auto exponent = std::stoi(out.substr(e + 1));
            return mantissa + "E" + std::to_string(exponent);
        }
        default: break;
        }
        throw std::runtime_error("Alchemy CFNumberFormatter: unsupported style");
    };
    // CFStringRef CFNumberFormatterCreateStringWithNumber(alloc, CFNumberFormatterRef, CFNumberRef)
    function("_CFNumberFormatterCreateStringWithNumber", [&cf, number_formatter, format, no_allocator](CpuState& s, GuestMemory&) {
        no_allocator(s.x[0]);
        const auto value = cf.number_value(s.x[2]);
        if (!value) throw std::runtime_error("Alchemy CFNumberFormatter: not a number");
        s.x[0] = cf.string(format(*number_formatter(s.x[1]), *value));
    });
    // CFStringRef CFNumberFormatterCreateStringWithValue(alloc, CFNumberFormatterRef, CFNumberType, const void*)
    function("_CFNumberFormatterCreateStringWithValue", [&process, &cf, number_formatter, format, no_allocator](CpuState& s, GuestMemory&) {
        no_allocator(s.x[0]);
        const auto number = cf.invoke("CFNumberCreate", std::array<std::uint64_t, 3>{0, s.x[2], s.x[3]});
        const auto value = cf.number_value(number);
        process.objects().release(number);
        if (!value) throw std::runtime_error("Alchemy CFNumberFormatter: not a number");
        s.x[0] = cf.string(format(*number_formatter(s.x[1]), *value));
    });
}

} // namespace alchemy::export_fill
