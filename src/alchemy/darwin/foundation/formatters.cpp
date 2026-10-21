#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/foundation/number_format.hpp"
#include "alchemy/darwin/foundation/date_format.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <span>
#include <string>
#include <tuple>
#include <vector>

// NSFormatter / NSNumberFormatter (Foundation; QLab formats times, levels and
// counts with them), added when Foundation lacks them. The formatting rules
// are number_format.cpp's. The locale gives the decimal and grouping
// separators and the currency symbol (until they are set explicitly).
// Not provided (explicit failures): the scientific, spell-out, ordinal and
// ISO/plural/accounting currency styles, significant-digit formatting,
// NSDecimalNumber results, the 10.0 formatter behaviour and format patterns
// beyond prefix, "#,##0" integer and ".00#" fraction parts and suffix.
namespace alchemy::gui::completion_kit {
namespace {

using numbers::Format;
using numbers::Style;

struct FormatterState final : ObjectPayload {
    Format format;
    Address locale{};                       // +1
    Address minimum{}, maximum{};           // +1 NSNumber
    Address nil_symbol{};                   // +1
    bool separators_set{}, currency_set{}, generates_decimal{}, lenient{}, partial_validation{}, always_decimal{};
    std::u16string currency_symbol{u"$"};
};

std::u16string units16(Support& support, Address string) {
    if (!string) return {};
    const auto units = support.cf.units(string);
    return std::u16string(units.begin(), units.end());
}
Address make_string(Support& support, const std::u16string& value) {
    return support.send_id(support.cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(value.data()), value.size())), "autorelease");
}

} // namespace


// NSDateFormatter over date_format.cpp: the date/time styles (en_US
// patterns), -dateFormat patterns, the time zone (default: the local zone)
// for offsets, and relative formatting ("Today", "Yesterday", "Tomorrow" in
// place of the date part). Locales other than en_US keep the en_US names
// and patterns, also for templates (dateFormatFromTemplate:). Not provided
// (explicit failures): non-Gregorian calendars.
namespace {
struct DateFormatterState final : ObjectPayload {
    std::int64_t date_style{}, time_style{};
    std::u16string pattern;                  // explicit -dateFormat ("" = from the styles)
    Address time_zone{}, locale{};           // +1
    Address default_date{};                  // +1, -defaultDate (parsing fills unnamed fields from it)
    bool lenient{}, relative{};
};
} // namespace

static void install_date_formatter(Support& support) {
    if (support.objc.find_class("NSDateFormatter")) return;
    if (!support.objc.find_class("NSFormatter")) static_cast<void>(support.declare_class("NSFormatter", "NSObject", foundation_library));
    const auto df = support.declare_class("NSDateFormatter", "NSFormatter", foundation_library);
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<DateFormatterState>(self); };
    const auto zone_of = [sp](DateFormatterState& state) {
        if (state.time_zone) return state.time_zone;
        const auto zone_class = sp->objc.find_class("NSTimeZone");
        if (!zone_class) unsupported("NSDateFormatter without NSTimeZone");
        return sp->send_id(zone_class, "localTimeZone");
    };
    const auto offset_for = [sp, zone_of](DateFormatterState& state, Address date) -> std::int64_t {
        return static_cast<std::int64_t>(sp->send_id(zone_of(state), "secondsFromGMTForDate:", {date}));
    };
    const auto pattern_of = [](const DateFormatterState& state) {
        if (!state.pattern.empty()) return state.pattern;
        const auto text = dates::style_pattern(static_cast<dates::Style>(state.date_style), static_cast<dates::Style>(state.time_style));
        return std::u16string(text.begin(), text.end());
    };
    support.method(df, "init", [sp, df](CpuState& s, GuestMemory&) {
        s.x[0] = sp->objc.send_super(s.x[0], df, sp->objc.selector("init")).x0;
    }, "@@:");
    for (const auto& [getter, setter, field] : {std::tuple{"dateStyle", "setDateStyle:", &DateFormatterState::date_style},
             std::tuple{"timeStyle", "setTimeStyle:", &DateFormatterState::time_style}}) {
        const auto member = field;
        support.method(df, getter, [state_of, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>((*state_of(s.x[0])).*member); }, "Q@:");
        support.method(df, setter, [state_of, member](CpuState& s, GuestMemory&) {
            if (s.x[2] > 4) fail("NSDateFormatter style " + std::to_string(s.x[2]));
            const auto state = state_of(s.x[0]);
            (*state).*member = static_cast<std::int64_t>(s.x[2]);
            state->pattern.clear();   // a style replaces an explicit format
        }, "v@:Q");
    }
    support.method(df, "dateFormat", [sp, state_of, pattern_of](CpuState& s, GuestMemory&) { s.x[0] = make_string(*sp, pattern_of(*state_of(s.x[0]))); }, "@@:");
    support.method(df, "setDateFormat:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto pattern = units16(*sp, s.x[2]);
        // Validate now: unsupported letters fail at the call that sets them.
        try { static_cast<void>(dates::format(pattern, 0, 0)); } catch (const std::exception& error) { unsupported(std::string("NSDateFormatter ") + error.what()); }
        state_of(s.x[0])->pattern = pattern;
    }, "v@:@");
    // Templates (TR35 skeletons) give en_US patterns, as the styles do, whatever the locale.
    const auto from_template = [sp](Address skeleton) -> std::u16string {
        try { return dates::pattern_from_template(units16(*sp, skeleton)); }
        catch (const std::exception& error) { unsupported(std::string("NSDateFormatter template: ") + error.what()); }
    };
    support.method(df, "setLocalizedDateFormatFromTemplate:", [state_of, from_template](CpuState& s, GuestMemory&) {
        if (s.x[2]) state_of(s.x[0])->pattern = from_template(s.x[2]);
    }, "v@:@");
    support.method(df, "dateFormatFromTemplate:options:locale:", [sp, from_template](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? make_string(*sp, from_template(s.x[2])) : 0;   // nil for a nil template
    }, "@@:@Q@", true);
    for (const auto& [getter, setter, field] : {std::tuple{"timeZone", "setTimeZone:", &DateFormatterState::time_zone},
             std::tuple{"locale", "setLocale:", &DateFormatterState::locale}}) {
        const auto member = field;
        support.method(df, getter, [state_of, member](CpuState& s, GuestMemory&) { s.x[0] = (*state_of(s.x[0])).*member; }, "@@:");
        support.method(df, setter, [sp, state_of, member](CpuState& s, GuestMemory&) {
            auto& slot = (*state_of(s.x[0])).*member;
            const auto retained = s.x[2] ? sp->send_id(s.x[2], "retain") : 0;
            if (slot) sp->send_id(slot, "release");
            slot = retained;
        }, "v@:@");
    }
    support.method(df, "calendar", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(df, "setCalendar:", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto identifier = sp->send_id(s.x[2], "calendarIdentifier");
        if (!identifier || sp->cf.utf8(identifier) != "gregorian") unsupported("NSDateFormatter with a non-Gregorian calendar");
    }, "v@:@");
    for (const auto& [getter, setter, field] : {std::tuple{"isLenient", "setLenient:", &DateFormatterState::lenient},
             std::tuple{"doesRelativeDateFormatting", "setDoesRelativeDateFormatting:", &DateFormatterState::relative}}) {
        const auto member = field;
        support.method(df, getter, [state_of, member](CpuState& s, GuestMemory&) { s.x[0] = (*state_of(s.x[0])).*member; }, "B@:");
        support.method(df, setter, [state_of, member](CpuState& s, GuestMemory&) { (*state_of(s.x[0])).*member = (s.x[2] & 0xff) != 0; }, "v@:B");
    }
    support.method(df, "formatterBehavior", [](CpuState& s, GuestMemory&) { s.x[0] = 1040; }, "Q@:");
    support.method(df, "setFormatterBehavior:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] != 1040 && s.x[2] != 0) unsupported("NSDateFormatter behavior " + std::to_string(s.x[2]) + " (only 10.4+)");
    }, "v@:Q");
    // Name arrays (en_US, as the formatter's names are; Blink's date fields read them): each name formatted from a
    // date that has it, months January first, weekdays Sunday first (2001-01-07 is a Sunday).
    for (const auto& [selector, pattern, months] : std::initializer_list<std::tuple<const char*, const char16_t*, bool>>{
             {"monthSymbols", u"MMMM", true}, {"shortMonthSymbols", u"MMM", true}, {"veryShortMonthSymbols", u"MMMMM", true},
             {"standaloneMonthSymbols", u"LLLL", true}, {"shortStandaloneMonthSymbols", u"LLL", true},
             {"veryShortStandaloneMonthSymbols", u"LLLLL", true}, {"weekdaySymbols", u"EEEE", false},
             {"shortWeekdaySymbols", u"EEE", false}, {"veryShortWeekdaySymbols", u"EEEEE", false},
             {"standaloneWeekdaySymbols", u"cccc", false}, {"shortStandaloneWeekdaySymbols", u"ccc", false},
             {"veryShortStandaloneWeekdaySymbols", u"ccccc", false}, {"eraSymbols", u"G", false}, {"longEraSymbols", u"GGGG", false}}) {
        const std::u16string letters = pattern;
        const bool by_month = months, eras = letters[0] == u'G';
        support.method(df, selector, [sp, letters, by_month, eras](CpuState& s, GuestMemory&) {
            std::vector<Address> names;
            for (int index = 0; index < (eras ? 2 : by_month ? 12 : 7); ++index) {
                const dates::Civil when{eras ? (index ? 2001 : -1) : 2001, by_month ? index + 1 : 1, by_month ? 1 : 7 + index, 12, 0, 0, 0, 0, 0};
                names.push_back(make_string(*sp, dates::format(letters, dates::reference_seconds(when, 0), 0)));
            }
            s.x[0] = sp->send_id(sp->cf.array(names), "autorelease");
        }, "@@:");
    }
    support.method(df, "AMSymbol", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->cf.constant_string("AM"); }, "@@:");
    support.method(df, "PMSymbol", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->cf.constant_string("PM"); }, "@@:");

    const auto string_for = [sp, state_of, offset_for, pattern_of](Address self, Address date) -> Address {
        if (!date) return 0;
        const auto state = state_of(self);
        const auto reference = sp->cf.date_time(date);
        const auto offset = offset_for(*state, date);
        if (state->relative && state->pattern.empty() && state->date_style != 0) {
            const auto now = sp->cf.absolute_time_now();
            const auto day = [offset](double when) { return std::floor((when + static_cast<double>(offset)) / 86400.0); };
            const auto delta = day(reference) - day(now);
            const char16_t* word = delta == 0 ? u"Today" : delta == -1 ? u"Yesterday" : delta == 1 ? u"Tomorrow" : nullptr;
            if (word) {
                std::u16string text = word;
                if (state->time_style != 0) {
                    const auto time = dates::style_pattern(dates::Style::none, static_cast<dates::Style>(state->time_style));
                    text += (state->date_style >= 3 ? u" at " : u", ") + dates::format(std::u16string(time.begin(), time.end()), reference, offset);
                }
                return make_string(*sp, text);
            }
        }
        return make_string(*sp, dates::format(pattern_of(*state), reference, offset));
    };
    support.method(df, "stringFromDate:", [string_for](CpuState& s, GuestMemory&) { s.x[0] = string_for(s.x[0], s.x[2]); }, "@@:@");
    support.method(df, "stringForObjectValue:", [string_for](CpuState& s, GuestMemory&) { s.x[0] = string_for(s.x[0], s.x[2]); }, "@@:@");
    const auto date_for = [sp, state_of, zone_of, pattern_of](Address self, Address text) -> Address {
        if (!text) return 0;
        const auto state = state_of(self);
        // The zone's current offset (DST transitions inside the text are not resolved).
        const auto now = sp->send_id(sp->cf.date(sp->cf.absolute_time_now()), "autorelease");
        const auto offset = static_cast<std::int64_t>(sp->send_id(zone_of(*state), "secondsFromGMTForDate:", {now}));
        std::optional<double> fallback;
        if (state->default_date) fallback = sp->cf.date_time(state->default_date);
        const auto parsed = dates::parse(pattern_of(*state), units16(*sp, text), offset, fallback);
        return parsed ? sp->send_id(sp->cf.date(*parsed), "autorelease") : 0;
    };
    support.method(df, "defaultDate", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->default_date; }, "@@:");
    support.method(df, "setDefaultDate:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto retained = s.x[2] ? sp->send_id(s.x[2], "copy") : 0;
        if (state->default_date) sp->send_id(state->default_date, "release");
        state->default_date = retained;
    }, "v@:@");
    support.method(df, "dateFromString:", [date_for](CpuState& s, GuestMemory&) { s.x[0] = date_for(s.x[0], s.x[2]); }, "@@:@");
    support.method(df, "getObjectValue:forString:errorDescription:", [sp, date_for](CpuState& s, GuestMemory& m) {
        const auto date = date_for(s.x[0], s.x[3]);
        if (date && s.x[2]) m.write64(s.x[2], date);
        if (!date && s.x[4]) m.write64(s.x[4], sp->cf.constant_string("Invalid date"));
        s.x[0] = date != 0;
    }, "B@:^@@^@");
    support.method(df, "localizedStringFromDate:dateStyle:timeStyle:", [sp, df](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(sp->send_id(df, "alloc"), "init");
        CallRegs date_style, time_style;
        date_style.x[2] = s.x[3];
        time_style.x[2] = s.x[4];
        static_cast<void>(sp->send(made, "setDateStyle:", date_style));
        static_cast<void>(sp->send(made, "setTimeStyle:", time_style));
        const auto text = sp->send_id(made, "stringFromDate:", {s.x[2]});
        const auto kept = text ? sp->send_id(sp->send_id(text, "retain"), "autorelease") : 0;
        sp->send_id(made, "release");
        s.x[0] = kept;
    }, "@@:@QQ", true);
    support.method(df, "copyWithZone:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto from = state_of(s.x[0]);
        const auto copy = sp->send_id(sp->send_id(sp->objc.object_class(s.x[0]), "alloc"), "init");
        const auto to = state_of(copy);
        to->date_style = from->date_style; to->time_style = from->time_style; to->pattern = from->pattern;
        to->lenient = from->lenient; to->relative = from->relative;
        to->time_zone = from->time_zone ? sp->send_id(from->time_zone, "retain") : 0;
        to->locale = from->locale ? sp->send_id(from->locale, "retain") : 0;
        to->default_date = from->default_date ? sp->send_id(from->default_date, "retain") : 0;
        s.x[0] = copy;
    }, "@@:^v");
    support.method(df, "dealloc", [sp, df](CpuState& s, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<DateFormatterState>(s.x[0]))
            for (auto* slot : {&state->time_zone, &state->locale, &state->default_date}) if (*slot) sp->send_id(std::exchange(*slot, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], df, sp->objc.selector("dealloc")));
    }, "v@:");
}

static void install_number_formatter(Support& support) {
    if (support.objc.find_class("NSNumberFormatter")) return;
    const auto formatter = support.objc.find_class("NSFormatter") ? support.objc.find_class("NSFormatter")
                                                                  : support.declare_class("NSFormatter", "NSObject", foundation_library);
    const auto nf = support.declare_class("NSNumberFormatter", "NSFormatter", foundation_library);
    static_cast<void>(formatter);
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<FormatterState>(self); };
    // Separators and currency from the locale (unless set explicitly).
    const auto adopt_locale = [sp](FormatterState& state) {
        if (!state.locale) return;
        const auto value = [&](const char* key) { return sp->send_id(state.locale, "objectForKey:", {sp->send_id(sp->cf.string(key), "autorelease")}); };
        if (!state.separators_set) {
            if (const auto decimal = value("decimal")) state.format.decimal_separator = units16(*sp, decimal);
            if (const auto grouping = value("grouping")) state.format.grouping_separator = units16(*sp, grouping);
        }
        if (!state.currency_set)
            if (const auto currency = value("currency")) state.currency_symbol = units16(*sp, currency);
    };
    const auto set_style = [](FormatterState& state, std::int64_t style) {
        switch (static_cast<Style>(style)) {
        case Style::none: case Style::decimal: case Style::currency: case Style::percent: break;
        default: unsupported("NSNumberFormatter style " + std::to_string(style) + " (scientific, spell-out, ordinal, ISO/plural/accounting currency)");
        }
        state.format.apply_style(static_cast<Style>(style), state.currency_symbol);
    };

    support.method(nf, "init", [sp, nf, state_of, adopt_locale](CpuState& s, GuestMemory&) {
        const auto self = sp->objc.send_super(s.x[0], nf, sp->objc.selector("init")).x0;
        s.x[0] = self;
        if (!self) return;
        const auto state = state_of(self);
        state->locale = sp->send_id(sp->objc.find_class("NSLocale"), "currentLocale");
        if (state->locale) state->locale = sp->send_id(state->locale, "retain");
        adopt_locale(*state);
    }, "@@:");
    support.method(nf, "numberStyle", [state_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state_of(s.x[0])->format.style); }, "Q@:");
    support.method(nf, "setNumberStyle:", [state_of, set_style](CpuState& s, GuestMemory&) {
        set_style(*state_of(s.x[0]), static_cast<std::int64_t>(s.x[2]));
    }, "v@:Q");
    support.method(nf, "locale", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->locale; }, "@@:");
    support.method(nf, "setLocale:", [sp, state_of, adopt_locale](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto retained = s.x[2] ? sp->send_id(s.x[2], "retain") : 0;
        if (state->locale) sp->send_id(state->locale, "release");
        state->locale = retained;
        adopt_locale(*state);
        // The currency affixes follow the new symbol.
        if (state->format.style == Style::currency) state->format.apply_style(Style::currency, state->currency_symbol);
    }, "v@:@");
    // Integer properties.
    struct IntegerField { const char* getter; const char* setter; std::int64_t Format::*field; };
    for (const auto& [getter, setter, field] : {IntegerField{"minimumIntegerDigits", "setMinimumIntegerDigits:", &Format::min_integer},
             IntegerField{"maximumIntegerDigits", "setMaximumIntegerDigits:", &Format::max_integer},
             IntegerField{"minimumFractionDigits", "setMinimumFractionDigits:", &Format::min_fraction},
             IntegerField{"maximumFractionDigits", "setMaximumFractionDigits:", &Format::max_fraction},
             IntegerField{"groupingSize", "setGroupingSize:", &Format::grouping_size},
             IntegerField{"formatWidth", "setFormatWidth:", &Format::format_width}}) {
        const auto member = field;
        const std::string name = setter;
        support.method(nf, getter, [state_of, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state_of(s.x[0])->format.*member); }, "Q@:");
        support.method(nf, setter, [state_of, member, name](CpuState& s, GuestMemory&) {
            auto& format = state_of(s.x[0])->format;
            format.*member = static_cast<std::int64_t>(s.x[2]);
            // Apple keeps min <= max when either moves.
            if (name == "setMinimumFractionDigits:" && format.max_fraction < format.min_fraction) format.max_fraction = format.min_fraction;
            if (name == "setMaximumFractionDigits:" && format.min_fraction > format.max_fraction) format.min_fraction = format.max_fraction;
            if (name == "setMinimumIntegerDigits:" && format.max_integer < format.min_integer) format.max_integer = format.min_integer;
            if (name == "setMaximumIntegerDigits:" && format.min_integer > format.max_integer) format.min_integer = format.max_integer;
        }, "v@:Q");
    }
    support.method(nf, "roundingMode", [state_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state_of(s.x[0])->format.rounding); }, "Q@:");
    support.method(nf, "setRoundingMode:", [state_of](CpuState& s, GuestMemory&) {
        if (s.x[2] > 6) fail("NSNumberFormatter rounding mode " + std::to_string(s.x[2]));
        state_of(s.x[0])->format.rounding = static_cast<numbers::Rounding>(s.x[2]);
    }, "v@:Q");
    // The 10.0-era names: thousand separators are the grouping separator.
    support.method(nf, "hasThousandSeparators", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->format.grouping; }, "B@:");
    support.method(nf, "setHasThousandSeparators:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->format.grouping = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(nf, "thousandSeparator", [sp, state_of](CpuState& s, GuestMemory&) { s.x[0] = make_string(*sp, state_of(s.x[0])->format.grouping_separator); }, "@@:");
    support.method(nf, "setThousandSeparator:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        state->format.grouping_separator = units16(*sp, s.x[2]);
        state->separators_set = true;
    }, "v@:@");
    for (const auto positive : {true, false}) {
        support.method(nf, positive ? "positiveInfinitySymbol" : "negativeInfinitySymbol", [sp, state_of, positive](CpuState& s, GuestMemory&) {
            const auto& f = state_of(s.x[0])->format;
            const auto& set = positive ? f.positive_infinity : f.negative_infinity;
            s.x[0] = make_string(*sp, set ? *set : std::u16string(positive ? u"+\u221e" : u"-\u221e"));
        }, "@@:");
        support.method(nf, positive ? "setPositiveInfinitySymbol:" : "setNegativeInfinitySymbol:", [sp, state_of, positive](CpuState& s, GuestMemory&) {
            auto& f = state_of(s.x[0])->format;
            auto& slot = positive ? f.positive_infinity : f.negative_infinity;
            if (s.x[2]) slot = units16(*sp, s.x[2]); else slot.reset();
        }, "v@:@");
    }
    support.method(nf, "roundingIncrement", [sp, state_of](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->cf.real(state_of(s.x[0])->format.rounding_increment), "autorelease");
    }, "@@:");
    support.method(nf, "setRoundingIncrement:", [sp, state_of](CpuState& s, GuestMemory&) {
        double increment = 0;
        if (s.x[2]) {
            if (const auto value = sp->cf.number_value(s.x[2])) increment = *value;
            else increment = std::bit_cast<double>(sp->send(s.x[2], "doubleValue").v[0][0]);
        }
        state_of(s.x[0])->format.rounding_increment = increment > 0 ? increment : 0;
    }, "v@:@");
    support.method(nf, "paddingPosition",[state_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state_of(s.x[0])->format.padding_position); }, "Q@:");
    support.method(nf, "setPaddingPosition:", [state_of](CpuState& s, GuestMemory&) {
        if (s.x[2] > 3) fail("NSNumberFormatter padding position " + std::to_string(s.x[2]));
        state_of(s.x[0])->format.padding_position = static_cast<numbers::Padding>(s.x[2]);
    }, "v@:Q");
    // Boolean properties.
    struct BoolField { const char* getter; const char* setter; bool Format::*field; };
    for (const auto& [getter, setter, field] : {BoolField{"usesGroupingSeparator", "setUsesGroupingSeparator:", &Format::grouping},
             BoolField{"allowsFloats", "setAllowsFloats:", &Format::allows_floats}}) {
        const auto member = field;
        support.method(nf, getter, [state_of, member](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->format.*member; }, "B@:");
        support.method(nf, setter, [state_of, member](CpuState& s, GuestMemory&) { state_of(s.x[0])->format.*member = (s.x[2] & 0xff) != 0; }, "v@:B");
    }
    struct FlagField { const char* getter; const char* setter; bool FormatterState::*field; };
    for (const auto& [getter, setter, field] : {FlagField{"isLenient", "setLenient:", &FormatterState::lenient},
             FlagField{"isPartialStringValidationEnabled", "setPartialStringValidationEnabled:", &FormatterState::partial_validation},
             FlagField{"alwaysShowsDecimalSeparator", "setAlwaysShowsDecimalSeparator:", &FormatterState::always_decimal},
             FlagField{"generatesDecimalNumbers", "setGeneratesDecimalNumbers:", &FormatterState::generates_decimal}}) {
        const auto member = field;
        support.method(nf, getter, [state_of, member](CpuState& s, GuestMemory&) { s.x[0] = (*state_of(s.x[0])).*member; }, "B@:");
        support.method(nf, setter, [state_of, member](CpuState& s, GuestMemory&) { (*state_of(s.x[0])).*member = (s.x[2] & 0xff) != 0; }, "v@:B");
    }
    support.method(nf, "usesSignificantDigits", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(nf, "setUsesSignificantDigits:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] & 0xff) unsupported("NSNumberFormatter significant-digit formatting");
    }, "v@:B");
    support.method(nf, "formatterBehavior", [](CpuState& s, GuestMemory&) { s.x[0] = 1040; }, "Q@:");
    support.method(nf, "setFormatterBehavior:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] != 1040 && s.x[2] != 0) unsupported("NSNumberFormatter behavior " + std::to_string(s.x[2]) + " (only 10.4+)");
    }, "v@:Q");
    // String properties.
    struct StringField { const char* getter; const char* setter; std::u16string Format::*field; bool separator; };
    for (const auto& [getter, setter, field, separator] : {StringField{"decimalSeparator", "setDecimalSeparator:", &Format::decimal_separator, true},
             StringField{"groupingSeparator", "setGroupingSeparator:", &Format::grouping_separator, true},
             StringField{"positivePrefix", "setPositivePrefix:", &Format::positive_prefix, false},
             StringField{"positiveSuffix", "setPositiveSuffix:", &Format::positive_suffix, false},
             StringField{"negativePrefix", "setNegativePrefix:", &Format::negative_prefix, false},
             StringField{"negativeSuffix", "setNegativeSuffix:", &Format::negative_suffix, false},
             StringField{"paddingCharacter", "setPaddingCharacter:", &Format::padding, false}}) {
        const auto member = field;
        const bool marks_separators = separator;
        support.method(nf, getter, [sp, state_of, member](CpuState& s, GuestMemory&) { s.x[0] = make_string(*sp, state_of(s.x[0])->format.*member); }, "@@:");
        support.method(nf, setter, [sp, state_of, member, marks_separators](CpuState& s, GuestMemory&) {
            const auto state = state_of(s.x[0]);
            state->format.*member = units16(*sp, s.x[2]);
            if (marks_separators) state->separators_set = true;
        }, "v@:@");
    }
    support.method(nf, "currencySymbol", [sp, state_of](CpuState& s, GuestMemory&) { s.x[0] = make_string(*sp, state_of(s.x[0])->currency_symbol); }, "@@:");
    support.method(nf, "setCurrencySymbol:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        state->currency_symbol = units16(*sp, s.x[2]);
        state->currency_set = true;
        if (state->format.style == Style::currency) state->format.apply_style(Style::currency, state->currency_symbol);
    }, "v@:@");
    support.method(nf, "zeroSymbol", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto& value = state_of(s.x[0])->format.zero_symbol;
        s.x[0] = value ? make_string(*sp, *value) : 0;
    }, "@@:");
    support.method(nf, "setZeroSymbol:", [sp, state_of](CpuState& s, GuestMemory&) {
        auto& value = state_of(s.x[0])->format.zero_symbol;
        if (s.x[2]) value = units16(*sp, s.x[2]); else value.reset();
    }, "v@:@");
    // Object properties (retained).
    struct ObjectField { const char* getter; const char* setter; Address FormatterState::*field; };
    for (const auto& [getter, setter, field] : {ObjectField{"minimum", "setMinimum:", &FormatterState::minimum},
             ObjectField{"maximum", "setMaximum:", &FormatterState::maximum}, ObjectField{"nilSymbol", "setNilSymbol:", &FormatterState::nil_symbol}}) {
        const auto member = field;
        support.method(nf, getter, [state_of, member](CpuState& s, GuestMemory&) { s.x[0] = (*state_of(s.x[0])).*member; }, "@@:");
        support.method(nf, setter, [sp, state_of, member](CpuState& s, GuestMemory&) {
            auto& slot = (*state_of(s.x[0])).*member;
            const auto retained = s.x[2] ? sp->send_id(s.x[2], "copy") : 0;
            if (slot) sp->send_id(slot, "release");
            slot = retained;
        }, "v@:@");
    }
    support.method(nf, "multiplier", [sp, state_of](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->cf.real(state_of(s.x[0])->format.multiplier), "autorelease");
    }, "@@:");
    support.method(nf, "setMultiplier:", [sp, state_of](CpuState& s, GuestMemory&) {
        state_of(s.x[0])->format.multiplier = s.x[2] ? sp->cf.number_value(s.x[2]).value_or(1) : 1;
    }, "v@:@");
    // Simple patterns: prefix, integer part ("#,##0"), fraction (".00##"), suffix.
    const auto set_pattern = [sp, state_of](Address self, Address pattern_object, bool positive) {
        const auto pattern = units16(*sp, pattern_object);
        const auto start = pattern.find_first_of(u"#0");
        if (start == std::u16string::npos) unsupported("NSNumberFormatter format without digits");
        auto end = start;
        while (end < pattern.size() && std::u16string_view(u"#0,.").find(pattern[end]) != std::u16string_view::npos) ++end;
        const auto number = pattern.substr(start, end - start);
        if (pattern.find_first_of(u"E@*';") != std::u16string::npos) unsupported("NSNumberFormatter format pattern features (exponent, padding, quoting, sections)");
        auto& format = state_of(self)->format;
        const auto prefix = pattern.substr(0, start), suffix = pattern.substr(end);
        if (positive) { format.positive_prefix = prefix; format.positive_suffix = suffix; }
        else { format.negative_prefix = prefix; format.negative_suffix = suffix; return; }
        const auto dot = number.find(u'.');
        const auto integer = number.substr(0, dot);
        const auto fraction = dot == std::u16string::npos ? std::u16string() : number.substr(dot + 1);
        format.grouping = integer.find(u',') != std::u16string::npos;
        if (format.grouping) format.grouping_size = static_cast<std::int64_t>(integer.size() - integer.find_last_of(u',') - 1);
        format.min_integer = static_cast<std::int64_t>(std::count(integer.begin(), integer.end(), u'0'));
        format.min_fraction = static_cast<std::int64_t>(std::count(fraction.begin(), fraction.end(), u'0'));
        format.max_fraction = static_cast<std::int64_t>(fraction.size());
        if (!positive) return;
        if (format.negative_prefix == u"-" && format.negative_suffix.empty()) { format.negative_prefix = u"-" + prefix; format.negative_suffix = suffix; }
    };
    support.method(nf, "setPositiveFormat:", [set_pattern](CpuState& s, GuestMemory&) { if (s.x[2]) set_pattern(s.x[0], s.x[2], true); }, "v@:@");
    support.method(nf, "setNegativeFormat:", [set_pattern](CpuState& s, GuestMemory&) { if (s.x[2]) set_pattern(s.x[0], s.x[2], false); }, "v@:@");
    support.method(nf, "setFormat:", [set_pattern](CpuState& s, GuestMemory&) { if (s.x[2]) set_pattern(s.x[0], s.x[2], true); }, "v@:@");

    // Formatting.
    const auto string_for = [sp, state_of](Address self, Address number) -> Address {
        const auto state = state_of(self);
        if (!number) return state->nil_symbol;
        const auto value = sp->cf.number_value(number);
        if (!value) return 0;   // not a number: no string, as NSNumberFormatter
        auto text = numbers::format(state->format, *value);
        if (state->always_decimal && text.find(state->format.decimal_separator) == std::u16string::npos)
            text += state->format.decimal_separator;
        return make_string(*sp, text);
    };
    support.method(nf, "stringFromNumber:", [string_for](CpuState& s, GuestMemory&) { s.x[0] = string_for(s.x[0], s.x[2]); }, "@@:@");
    support.method(nf, "stringForObjectValue:", [string_for](CpuState& s, GuestMemory&) { s.x[0] = string_for(s.x[0], s.x[2]); }, "@@:@");
    support.method(nf, "editingStringForObjectValue:", [string_for](CpuState& s, GuestMemory&) { s.x[0] = string_for(s.x[0], s.x[2]); }, "@@:@");
    const auto number_for = [sp, state_of](Address self, Address string) -> Address {
        const auto state = state_of(self);
        if (!string) return 0;
        const auto parsed = numbers::parse(state->format, units16(*sp, string));
        if (!parsed) return 0;
        if (state->generates_decimal) unsupported("NSNumberFormatter generatesDecimalNumbers (NSDecimalNumber)");
        const auto in_range = [&](Address bound, bool lower) {
            if (!bound) return true;
            const auto limit = sp->cf.number_value(bound);
            return !limit || (lower ? *parsed >= *limit : *parsed <= *limit);
        };
        if (!in_range(state->minimum, true) || !in_range(state->maximum, false)) return 0;
        const bool integral = *parsed == std::floor(*parsed) && std::fabs(*parsed) < 9.2e18;
        const auto number = integral ? sp->cf.integer(static_cast<std::int64_t>(*parsed)) : sp->cf.real(*parsed);
        return sp->send_id(number, "autorelease");
    };
    support.method(nf, "numberFromString:", [number_for](CpuState& s, GuestMemory&) { s.x[0] = number_for(s.x[0], s.x[2]); }, "@@:@");
    support.method(nf, "getObjectValue:forString:errorDescription:", [sp, number_for](CpuState& s, GuestMemory& m) {
        const auto number = number_for(s.x[0], s.x[3]);
        if (number && s.x[2]) m.write64(s.x[2], number);
        if (!number && s.x[4]) m.write64(s.x[4], sp->send_id(sp->cf.string("Invalid number"), "autorelease"));
        s.x[0] = number != 0;
    }, "B@:^@@^@");
    support.method(nf, "getObjectValue:forString:range:error:", [number_for](CpuState& s, GuestMemory& m) {
        const auto number = number_for(s.x[0], s.x[3]);
        if (number && s.x[2]) m.write64(s.x[2], number);
        if (s.x[5]) m.write64(s.x[5], 0);
        s.x[0] = number != 0;
    }, "B@:^@@^{_NSRange=QQ}^@");
    support.method(nf, "isPartialStringValid:newEditingString:errorDescription:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@^@^@");
    support.method(nf, "copyWithZone:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto from = state_of(s.x[0]);
        const auto copy = sp->send_id(sp->send_id(sp->objc.object_class(s.x[0]), "alloc"), "init");
        const auto to = state_of(copy);
        to->format = from->format;
        to->currency_symbol = from->currency_symbol;
        to->separators_set = from->separators_set; to->currency_set = from->currency_set;
        to->generates_decimal = from->generates_decimal; to->lenient = from->lenient;
        to->partial_validation = from->partial_validation; to->always_decimal = from->always_decimal;
        for (const auto& [target, source] : {std::pair{&to->locale, from->locale}, std::pair{&to->minimum, from->minimum},
                 std::pair{&to->maximum, from->maximum}, std::pair{&to->nil_symbol, from->nil_symbol}}) {
            const auto retained = source ? sp->send_id(source, "retain") : 0;
            if (*target) sp->send_id(*target, "release");
            *target = retained;
        }
        s.x[0] = copy;
    }, "@@:^v");
    support.method(nf, "dealloc", [sp, nf, state_of](CpuState& s, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<FormatterState>(s.x[0]))
            for (auto* slot : {&state->locale, &state->minimum, &state->maximum, &state->nil_symbol})
                if (*slot) sp->send_id(std::exchange(*slot, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], nf, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(nf, "localizedStringFromNumber:numberStyle:", [sp, nf](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(sp->send_id(nf, "alloc"), "init");
        CallRegs style;
        style.x[2] = s.x[3];
        static_cast<void>(sp->send(made, "setNumberStyle:", style));
        const auto text = sp->send_id(made, "stringFromNumber:", {s.x[2]});
        const auto kept = text ? sp->send_id(sp->send_id(text, "retain"), "autorelease") : 0;
        sp->send_id(made, "release");
        s.x[0] = kept;
    }, "@@:@Q", true);
}

void install_formatters(Support& support) {
    install_number_formatter(support);
    install_date_formatter(support);
}

} // namespace alchemy::gui::completion_kit
