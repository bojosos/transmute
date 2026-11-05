#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/foundation/date_format.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The newer Foundation date classes, after Apple's public Foundation
// reference, added when Foundation lacks them:
//   - NSDateInterval: a start date and a duration (containment,
//     intersection, comparison).
//   - NSISO8601DateFormatter: patterns built from the format options
//     (calendar dates, ordinal dates, times with fractional seconds, "Z" or
//     +hh[:]mm zones) in the formatter's time zone (GMT by default), over
//     date_format.cpp. Week-of-year dates are not provided (explicit failure).
//   - NSDateComponentsFormatter, NSRelativeDateTimeFormatter and
//     NSDateIntervalFormatter: English text (every locale), with calendar
//     differences from NSCalendar's components:fromDate:toDate:options:.
//     NSDateIntervalFormatter collapses an interval within one day to the
//     date and the two times ("1/2/24, 3:00 PM - 4:00 PM", the dash an en
//     dash between thin spaces); templates give
//     en_US patterns, as NSDateFormatter's do.
namespace alchemy::gui::completion_kit {
namespace {

// NSCalendarUnit.
constexpr std::uint64_t unit_year = 4, unit_month = 8, unit_day = 16, unit_hour = 32, unit_minute = 64, unit_second = 128,
    unit_week_of_month = 4096;
constexpr std::uint64_t default_units = unit_year | unit_month | unit_week_of_month | unit_day | unit_hour | unit_minute | unit_second;

double d0(const CpuState& s) { return std::bit_cast<double>(s.v[0][0]); }
void return_double(CpuState& s, double value) { s.v[0][0] = std::bit_cast<std::uint64_t>(value); s.v[0][1] = 0; }

std::u16string units16(Support& support, Address string) {
    if (!string) return {};
    const auto units = support.cf.units(string);
    return std::u16string(units.begin(), units.end());
}
Address make_string(Support& support, const std::u16string& value) {
    return support.send_id(support.cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(value.data()), value.size())), "autorelease");
}
Address make_string(Support& support, const std::string& value) {
    return support.send_id(support.cf.string(value), "autorelease");
}
Address autoreleased_date(Support& support, double when) { return support.send_id(support.cf.date(when), "autorelease"); }

// ---- calendar differences ----
struct Unit { std::uint64_t flag; const char* selector; };
constexpr Unit units_in_order[] = {{unit_year, "year"}, {unit_month, "month"}, {unit_week_of_month, "weekOfMonth"},
                                   {unit_day, "day"}, {unit_hour, "hour"}, {unit_minute, "minute"}, {unit_second, "second"}};

// The components between two dates in the current calendar, for `allowed` units ({flag, value} in order).
std::vector<std::pair<std::uint64_t, std::int64_t>> difference(Support& support, double from, double to, std::uint64_t allowed) {
    const auto calendar_class = support.objc.find_class("NSCalendar");
    if (!calendar_class) unsupported("date differences without NSCalendar");
    const auto calendar = support.send_id(calendar_class, "currentCalendar");
    CallRegs registers{};
    registers.x[2] = allowed;
    registers.x[3] = autoreleased_date(support, from);
    registers.x[4] = autoreleased_date(support, to);
    registers.x[5] = 0;
    const auto components = support.send(calendar, "components:fromDate:toDate:options:", registers).x0;
    std::vector<std::pair<std::uint64_t, std::int64_t>> out;
    for (const auto& unit : units_in_order)
        if (allowed & unit.flag) out.emplace_back(unit.flag, components ? static_cast<std::int64_t>(support.send_id(components, unit.selector)) : 0);
    return out;
}

// English unit names: {singular, plural} for full, {short}, {abbreviated}, {brief}.
struct UnitNames { const char* one; const char* many; const char* short_one; const char* short_many; const char* abbreviated; const char* brief; };
const UnitNames& names_of(std::uint64_t flag) {
    static const UnitNames year{"year", "years", "yr", "yrs", "y", "yr"}, month{"month", "months", "mth", "mths", "mo", "mth"},
        week{"week", "weeks", "wk", "wks", "w", "wk"}, day{"day", "days", "day", "days", "d", "d"}, hour{"hour", "hours", "hr", "hr", "h", "hr"},
        minute{"minute", "minutes", "min", "min", "m", "min"}, second{"second", "seconds", "sec", "sec", "s", "sec"};
    switch (flag) {
    case unit_year: return year;
    case unit_month: return month;
    case unit_week_of_month: return week;
    case unit_day: return day;
    case unit_hour: return hour;
    case unit_minute: return minute;
    default: return second;
    }
}

std::string spelled(std::int64_t value) {
    static const char* small[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
                                  "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    if (value < 0) return "minus " + spelled(-value);
    if (value < 20) return small[value];
    if (value < 100) return std::string(tens[value / 10]) + (value % 10 ? "-" + std::string(small[value % 10]) : "");
    if (value < 1000) return spelled(value / 100) + " hundred" + (value % 100 ? " " + spelled(value % 100) : "");
    if (value < 1000000) return spelled(value / 1000) + " thousand" + (value % 1000 ? " " + spelled(value % 1000) : "");
    return std::to_string(value);
}

// ---- NSDateComponentsFormatter ----
struct ComponentsFormatterState final : ObjectPayload {
    std::int64_t units_style{};             // positional 0, abbreviated 1, short 2, full 3, spellOut 4, brief 5
    std::uint64_t allowed_units{};          // 0: the default set
    std::uint64_t zero_behavior{1};         // NSDateComponentsFormatterZeroFormattingBehaviorDefault
    std::int64_t maximum_unit_count{};
    bool approximation{}, remaining{}, collapses{}, fractional{};
};

std::string format_components(const ComponentsFormatterState& state, std::vector<std::pair<std::uint64_t, std::int64_t>> values) {
    constexpr std::uint64_t drop_leading = 2, drop_middle = 4, drop_trailing = 8, pad = 0x10000;
    auto behavior = state.zero_behavior;
    if (behavior == 1) behavior = state.units_style == 0 ? drop_leading : drop_leading | drop_middle | drop_trailing;   // Default
    bool negative = false;
    for (const auto& [flag, value] : values) if (value < 0) negative = true;
    for (auto& [flag, value] : values) value = value < 0 ? -value : value;
    // Drop zero units at the start, middle and end as the behavior asks.
    std::size_t first = 0, last = values.size();
    if (behavior & drop_leading) while (first < last && values[first].second == 0) ++first;
    if (behavior & drop_trailing) while (last > first && values[last - 1].second == 0) --last;
    std::vector<std::pair<std::uint64_t, std::int64_t>> shown;
    for (std::size_t index = first; index < last; ++index)
        if (!(behavior & drop_middle) || values[index].second != 0 || index == first || index + 1 == last) shown.push_back(values[index]);
    if ((behavior & drop_middle) && shown.size() > 1) std::erase_if(shown, [](const auto& entry) { return entry.second == 0; });
    if (shown.empty() && !values.empty()) shown.push_back({values.back().first, 0});   // "0s"
    if (state.maximum_unit_count > 0 && static_cast<std::int64_t>(shown.size()) > state.maximum_unit_count)
        shown.resize(static_cast<std::size_t>(state.maximum_unit_count));
    std::string text;
    if (state.units_style == 0) {   // positional: h:mm:ss
        for (std::size_t index = 0; index < shown.size(); ++index) {
            auto number = std::to_string(shown[index].second);
            if ((index > 0 || (behavior & pad)) && number.size() < 2) number.insert(0, 1, '0');
            text += (index ? ":" : "") + number;
        }
    } else {
        for (std::size_t index = 0; index < shown.size(); ++index) {
            const auto& [flag, value] = shown[index];
            const auto& names = names_of(flag);
            std::string part;
            switch (state.units_style) {
            case 1: part = std::to_string(value) + names.abbreviated; break;
            case 2: part = std::to_string(value) + " " + (value == 1 ? names.short_one : names.short_many); break;
            case 4: part = spelled(value) + " " + (value == 1 ? names.one : names.many); break;
            case 5: part = std::to_string(value) + names.brief; break;
            default: part = std::to_string(value) + " " + (value == 1 ? names.one : names.many); break;
            }
            const bool spaced = state.units_style == 1 || state.units_style == 5;
            text += (index ? (spaced ? " " : ", ") : "") + part;
        }
    }
    if (negative) text = "-" + text;
    if (state.approximation) text = "About " + text;
    if (state.remaining) text += " remaining";
    return text;
}

// ---- NSRelativeDateTimeFormatter ----
struct RelativeFormatterState final : ObjectPayload {
    std::int64_t date_time_style{};   // numeric 0, named 1
    std::int64_t units_style{};       // full 0, spellOut 1, short 2, abbreviated 3
};

std::string relative_text(const RelativeFormatterState& state, std::uint64_t flag, std::int64_t value) {
    if (state.date_time_style == 1) {
        if (value == 0 && (flag == unit_second || flag == unit_minute)) return "now";
        const char* named = nullptr;
        if (value == 0) named = flag == unit_day ? "today" : flag == unit_week_of_month ? "this week" : flag == unit_month ? "this month"
                              : flag == unit_year ? "this year" : flag == unit_hour ? "this hour" : nullptr;
        if (value == 1) named = flag == unit_day ? "tomorrow" : flag == unit_week_of_month ? "next week" : flag == unit_month ? "next month"
                              : flag == unit_year ? "next year" : nullptr;
        if (value == -1) named = flag == unit_day ? "yesterday" : flag == unit_week_of_month ? "last week" : flag == unit_month ? "last month"
                               : flag == unit_year ? "last year" : nullptr;
        if (named) return named;
    }
    const auto magnitude = value < 0 ? -value : value;
    const auto& names = names_of(flag);
    std::string amount;
    switch (state.units_style) {
    case 1: amount = spelled(magnitude) + " " + (magnitude == 1 ? names.one : names.many); break;
    case 2: amount = std::to_string(magnitude) + " " + (magnitude == 1 ? names.short_one : names.short_many) + "."; break;
    case 3: amount = std::to_string(magnitude) + names.abbreviated; break;
    default: amount = std::to_string(magnitude) + " " + (magnitude == 1 ? names.one : names.many); break;
    }
    return value < 0 ? amount + " ago" : "in " + amount;
}

// ---- NSDateInterval ----
struct DateIntervalState final : ObjectPayload {
    double start{}, duration{};
};

// ---- NSISO8601DateFormatter ----
// NSISO8601DateFormatOptions.
constexpr std::uint64_t iso_year = 1 << 0, iso_month = 1 << 1, iso_week = 1 << 2, iso_day = 1 << 4, iso_time = 1 << 5, iso_zone = 1 << 6,
    iso_space = 1 << 7, iso_dash = 1 << 8, iso_colon_time = 1 << 9, iso_colon_zone = 1 << 10, iso_fraction = 1 << 11;
constexpr std::uint64_t iso_internet = iso_year | iso_month | iso_day | iso_dash | iso_time | iso_colon_time | iso_zone | iso_colon_zone;

std::u16string iso_pattern(std::uint64_t options) {
    if (options & iso_week) unsupported("NSISO8601DateFormatter week-of-year dates");
    std::u16string date, time;
    const std::u16string dash = (options & iso_dash) ? u"-" : u"";
    if (options & iso_year) date += u"yyyy";
    if (options & iso_month) date += (date.empty() ? u"" : dash) + std::u16string(u"MM");
    if (options & iso_day) date += (date.empty() ? u"" : dash) + std::u16string((options & iso_month) ? u"dd" : u"DDD");
    if (options & iso_time) {
        const std::u16string colon = (options & iso_colon_time) ? u":" : u"";
        time = u"HH" + colon + u"mm" + colon + u"ss";
        if (options & iso_fraction) time += u".SSS";
    }
    std::u16string pattern = date;
    if (!time.empty()) pattern += (pattern.empty() ? u"" : ((options & iso_space) ? u" " : u"'T'")) + time;
    if (options & iso_zone) pattern += (options & iso_colon_zone) ? u"XXX" : u"XX";
    return pattern;
}

struct IsoFormatterState final : ObjectPayload {
    std::uint64_t options{iso_internet};
    Address time_zone{};   // +1; nil: GMT
};

// ---- NSDateIntervalFormatter ----
struct IntervalFormatterState final : ObjectPayload {
    std::int64_t date_style{2}, time_style{2};   // medium, medium
    std::u16string date_template;
    Address time_zone{}, locale{}, calendar{};   // +1
};

std::int64_t zone_offset(Support& support, Address zone, double when) {
    if (!zone) {
        const auto zone_class = support.objc.find_class("NSTimeZone");
        if (!zone_class) unsupported("date formatting without NSTimeZone");
        zone = support.send_id(zone_class, "localTimeZone");
    }
    return static_cast<std::int64_t>(support.send_id(zone, "secondsFromGMTForDate:", {autoreleased_date(support, when)}));
}

// Object properties of a formatter state: getter, setter (retaining) over one Address member.
template <class State>
void address_property(Support& support, Address cls, const char* getter, const char* setter, Address State::* member) {
    auto* sp = &support;
    support.method(cls, getter, [sp, member](CpuState& s, GuestMemory&) { s.x[0] = (*sp->objc.host_state<State>(s.x[0])).*member; }, "@@:");
    support.method(cls, setter, [sp, member](CpuState& s, GuestMemory&) {
        auto state = sp->objc.host_state<State>(s.x[0]);
        const auto value = s.x[2] ? sp->send_id(s.x[2], "retain") : 0;
        if ((*state).*member) sp->send_id((*state).*member, "release");
        (*state).*member = value;
    }, "v@:@");
}
template <class State, class Member>
void integer_property(Support& support, Address cls, const char* getter, const char* setter, Member State::* member) {
    auto* sp = &support;
    support.method(cls, getter, [sp, member](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>((*sp->objc.host_state<State>(s.x[0])).*member);
    }, "q@:");
    support.method(cls, setter, [sp, member](CpuState& s, GuestMemory&) {
        (*sp->objc.host_state<State>(s.x[0])).*member = static_cast<Member>(s.x[2]);
    }, "v@:q");
}

void install_date_interval(Support& support) {
    if (support.objc.find_class("NSDateInterval")) return;
    const auto cls = support.declare_class("NSDateInterval", "NSObject", foundation_library);
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<DateIntervalState>(self); };
    support.method(cls, "init", [sp, state_of](CpuState& s, GuestMemory&) {
        state_of(s.x[0])->start = sp->cf.absolute_time_now();
    }, "@@:");
    support.method(cls, "initWithStartDate:duration:", [sp, state_of](CpuState& s, GuestMemory&) {
        if (!s.x[2] || d0(s) < 0) fail("-[NSDateInterval initWithStartDate:duration:] needs a start date and a duration of at least 0");
        const auto state = state_of(s.x[0]);
        state->start = sp->cf.date_time(s.x[2]);
        state->duration = d0(s);
    }, "@@:@d");
    support.method(cls, "initWithStartDate:endDate:", [sp, state_of](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) fail("-[NSDateInterval initWithStartDate:endDate:] needs two dates");
        const auto start = sp->cf.date_time(s.x[2]), end = sp->cf.date_time(s.x[3]);
        if (end < start) fail("-[NSDateInterval initWithStartDate:endDate:]: the end date is before the start date");
        const auto state = state_of(s.x[0]);
        state->start = start;
        state->duration = end - start;
    }, "@@:@@");
    support.method(cls, "startDate", [sp, state_of](CpuState& s, GuestMemory&) { s.x[0] = autoreleased_date(*sp, state_of(s.x[0])->start); }, "@@:");
    support.method(cls, "endDate", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        s.x[0] = autoreleased_date(*sp, state->start + state->duration);
    }, "@@:");
    support.method(cls, "duration", [state_of](CpuState& s, GuestMemory&) { return_double(s, state_of(s.x[0])->duration); }, "d@:");
    support.method(cls, "containsDate:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto when = s.x[2] ? sp->cf.date_time(s.x[2]) : NAN;
        s.x[0] = when >= state->start && when <= state->start + state->duration ? 1 : 0;
    }, "B@:@");
    const auto overlap = [state_of](Address a, Address b) -> std::optional<std::pair<double, double>> {
        const auto x = state_of(a), y = state_of(b);
        const auto start = std::max(x->start, y->start), end = std::min(x->start + x->duration, y->start + y->duration);
        if (end < start) return std::nullopt;
        return std::pair{start, end};
    };
    support.method(cls, "intersectsDateInterval:", [overlap](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] && overlap(s.x[0], s.x[2]) ? 1 : 0;
    }, "B@:@");
    support.method(cls, "intersectionWithDateInterval:", [sp, cls, state_of, overlap](CpuState& s, GuestMemory&) {
        const auto range = s.x[2] ? overlap(s.x[0], s.x[2]) : std::nullopt;
        if (!range) { s.x[0] = 0; return; }
        const auto result = sp->send_id(sp->send_id(cls, "alloc"), "init");
        const auto state = state_of(result);
        state->start = range->first;
        state->duration = range->second - range->first;
        s.x[0] = sp->send_id(result, "autorelease");
    }, "@@:@");
    // NSComparisonResult -compare: by the start date, then the duration.
    support.method(cls, "compare:", [state_of](CpuState& s, GuestMemory&) {
        const auto x = state_of(s.x[0]), y = state_of(s.x[2]);
        const auto order = x->start != y->start ? (x->start < y->start ? -1 : 1) : x->duration != y->duration ? (x->duration < y->duration ? -1 : 1) : 0;
        s.x[0] = static_cast<Address>(static_cast<std::int64_t>(order));
    }, "q@:@");
    const auto equal = [sp, cls, state_of](Address a, Address b) {
        if (!b || !sp->send_id(b, "isKindOfClass:", {cls})) return false;
        const auto x = state_of(a), y = state_of(b);
        return x->start == y->start && x->duration == y->duration;
    };
    support.method(cls, "isEqualToDateInterval:", [equal](CpuState& s, GuestMemory&) { s.x[0] = equal(s.x[0], s.x[2]) ? 1 : 0; }, "B@:@");
    support.method(cls, "isEqual:", [equal](CpuState& s, GuestMemory&) { s.x[0] = equal(s.x[0], s.x[2]) ? 1 : 0; }, "B@:@");
    support.method(cls, "hash", [state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        s.x[0] = std::bit_cast<std::uint64_t>(state->start) ^ (std::bit_cast<std::uint64_t>(state->duration) * 31);
    }, "Q@:");
    support.method(cls, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "retain"); }, "@@:^v");   // immutable
    support.method(cls, "description", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto seconds = std::to_string(state->duration);
        const auto text = dates::format(u"yyyy-MM-dd HH:mm:ss Z", state->start, 0) + u" + " + std::u16string(seconds.begin(), seconds.end()) + u"s";
        s.x[0] = make_string(*sp, text);
    }, "@@:");
}

void install_iso8601_formatter(Support& support) {
    if (support.objc.find_class("NSISO8601DateFormatter")) return;
    if (!support.objc.find_class("NSFormatter")) static_cast<void>(support.declare_class("NSFormatter", "NSObject", foundation_library));
    const auto cls = support.declare_class("NSISO8601DateFormatter", "NSFormatter", foundation_library);
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<IsoFormatterState>(self); };
    integer_property<IsoFormatterState>(support, cls, "formatOptions", "setFormatOptions:", &IsoFormatterState::options);
    address_property<IsoFormatterState>(support, cls, "timeZone", "setTimeZone:", &IsoFormatterState::time_zone);
    const auto offset_of = [sp](Address zone, double when) -> std::int64_t { return zone ? zone_offset(*sp, zone, when) : 0; };
    const auto format = [sp, offset_of](std::uint64_t options, Address zone, Address date) -> Address {
        if (!date) return 0;
        const auto when = sp->cf.date_time(date);
        return make_string(*sp, dates::format(iso_pattern(options), when, offset_of(zone, when)));
    };
    support.method(cls, "stringFromDate:", [state_of, format](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        s.x[0] = format(state->options, state->time_zone, s.x[2]);
    }, "@@:@");
    support.method(cls, "stringForObjectValue:", [state_of, format](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        s.x[0] = format(state->options, state->time_zone, s.x[2]);
    }, "@@:@");
    support.method(cls, "dateFromString:", [sp, state_of, offset_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto state = state_of(s.x[0]);
        const auto pattern = iso_pattern(state->options);
        const auto text = units16(*sp, s.x[2]);
        auto parsed = dates::parse(pattern, text, offset_of(state->time_zone, sp->cf.absolute_time_now()));
        if (parsed && state->time_zone) {
            const auto at = offset_of(state->time_zone, *parsed);
            parsed = dates::parse(pattern, text, at);
        }
        s.x[0] = parsed ? autoreleased_date(*sp, *parsed) : 0;
    }, "@@:@");
    // +stringFromDate:timeZone:formatOptions:
    support.method(cls, "stringFromDate:timeZone:formatOptions:", [format](CpuState& s, GuestMemory&) {
        s.x[0] = format(s.x[4], s.x[3], s.x[2]);
    }, "@@:@@Q", true);
    support.method(cls, "dealloc", [sp, cls](CpuState& s, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<IsoFormatterState>(s.x[0]))
            if (state->time_zone) sp->send_id(std::exchange(state->time_zone, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
    }, "v@:");
}

void install_components_formatter(Support& support) {
    if (support.objc.find_class("NSDateComponentsFormatter")) return;
    const auto cls = support.declare_class("NSDateComponentsFormatter", "NSFormatter", foundation_library);
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<ComponentsFormatterState>(self); };
    using S = ComponentsFormatterState;
    integer_property<S>(support, cls, "unitsStyle", "setUnitsStyle:", &S::units_style);
    integer_property<S>(support, cls, "allowedUnits", "setAllowedUnits:", &S::allowed_units);
    integer_property<S>(support, cls, "zeroFormattingBehavior", "setZeroFormattingBehavior:", &S::zero_behavior);
    integer_property<S>(support, cls, "maximumUnitCount", "setMaximumUnitCount:", &S::maximum_unit_count);
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, bool S::*>>{
             {"includesApproximationPhrase", "setIncludesApproximationPhrase:", &S::approximation},
             {"includesTimeRemainingPhrase", "setIncludesTimeRemainingPhrase:", &S::remaining},
             {"collapsesLargestUnit", "setCollapsesLargestUnit:", &S::collapses},
             {"allowsFractionalUnits", "setAllowsFractionalUnits:", &S::fractional}}) {
        support.method(cls, getter, [state_of, flag = member](CpuState& s, GuestMemory&) { s.x[0] = (*state_of(s.x[0])).*flag ? 1 : 0; }, "B@:");
        support.method(cls, setter, [state_of, flag = member](CpuState& s, GuestMemory&) { (*state_of(s.x[0])).*flag = (s.x[2] & 0xff) != 0; }, "v@:B");
    }
    const auto allowed = [](const S& state) { return state.allowed_units ? state.allowed_units : default_units; };
    const auto between = [sp, state_of, allowed](Address self, double from, double to) -> Address {
        const auto state = state_of(self);
        if (std::isnan(from) || std::isnan(to)) return 0;
        auto values = to >= from ? difference(*sp, from, to, allowed(*state)) : difference(*sp, to, from, allowed(*state));
        if (to < from) for (auto& [flag, value] : values) value = -value;
        return make_string(*sp, format_components(*state, std::move(values)));
    };
    support.method(cls, "stringFromTimeInterval:", [sp, between](CpuState& s, GuestMemory&) {
        const auto now = sp->cf.absolute_time_now();
        s.x[0] = between(s.x[0], now, now + d0(s));
    }, "@@:d");
    support.method(cls, "stringFromDate:toDate:", [sp, between](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] && s.x[3] ? between(s.x[0], sp->cf.date_time(s.x[2]), sp->cf.date_time(s.x[3])) : 0;
    }, "@@:@@");
    // -stringFromDateComponents: the components' own values for the allowed units (NSDateComponentUndefined skipped).
    support.method(cls, "stringFromDateComponents:", [sp, state_of, allowed](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto state = state_of(s.x[0]);
        std::vector<std::pair<std::uint64_t, std::int64_t>> values;
        for (const auto& unit : units_in_order) {
            if (!(allowed(*state) & unit.flag)) continue;
            const auto value = static_cast<std::int64_t>(sp->send_id(s.x[2], unit.selector));
            values.emplace_back(unit.flag, value == INT64_MAX ? 0 : value);
        }
        s.x[0] = make_string(*sp, format_components(*state, std::move(values)));
    }, "@@:@");
    support.method(cls, "stringForObjectValue:", [sp](CpuState& s, GuestMemory&) {
        // NSNumber (seconds) or NSDateComponents.
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto number_class = sp->objc.find_class("NSNumber");
        if (number_class && sp->send_id(s.x[2], "isKindOfClass:", {number_class})) {
            CallRegs registers{};
            registers.v[0][0] = sp->send(s.x[2], "doubleValue").v[0][0];
            s.x[0] = sp->send(s.x[0], "stringFromTimeInterval:", registers).x0;
        } else {
            s.x[0] = sp->send_id(s.x[0], "stringFromDateComponents:", {s.x[2]});
        }
    }, "@@:@");
    // +localizedStringFromDateComponents:unitsStyle:
    support.method(cls, "localizedStringFromDateComponents:unitsStyle:", [sp, cls, state_of](CpuState& s, GuestMemory&) {
        const auto formatter = sp->send_id(sp->send_id(cls, "alloc"), "init");
        state_of(formatter)->units_style = static_cast<std::int64_t>(s.x[3]);
        const auto text = sp->send_id(formatter, "stringFromDateComponents:", {s.x[2]});
        const auto kept = text ? sp->send_id(sp->send_id(text, "retain"), "autorelease") : 0;
        sp->send_id(formatter, "release");
        s.x[0] = kept;
    }, "@@:@q", true);
}

void install_relative_formatter(Support& support) {
    if (support.objc.find_class("NSRelativeDateTimeFormatter")) return;
    const auto cls = support.declare_class("NSRelativeDateTimeFormatter", "NSFormatter", foundation_library);
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<RelativeFormatterState>(self); };
    using S = RelativeFormatterState;
    integer_property<S>(support, cls, "dateTimeStyle", "setDateTimeStyle:", &S::date_time_style);
    integer_property<S>(support, cls, "unitsStyle", "setUnitsStyle:", &S::units_style);
    // The largest unit that differs between the reference and the date.
    const auto relative = [sp, state_of](Address self, double date, double reference) -> Address {
        const auto state = state_of(self);
        auto values = date >= reference ? difference(*sp, reference, date, default_units) : difference(*sp, date, reference, default_units);
        std::pair<std::uint64_t, std::int64_t> chosen{unit_second, 0};
        for (const auto& entry : values) if (entry.second != 0) { chosen = entry; break; }
        if (date < reference) chosen.second = -chosen.second;
        return make_string(*sp, relative_text(*state, chosen.first, chosen.second));
    };
    support.method(cls, "localizedStringForDate:relativeToDate:", [sp, relative](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] && s.x[3] ? relative(s.x[0], sp->cf.date_time(s.x[2]), sp->cf.date_time(s.x[3])) : 0;
    }, "@@:@@");
    support.method(cls, "localizedStringFromTimeInterval:", [sp, relative](CpuState& s, GuestMemory&) {
        const auto now = sp->cf.absolute_time_now();
        s.x[0] = relative(s.x[0], now + d0(s), now);
    }, "@@:d");
    support.method(cls, "localizedStringFromDateComponents:", [sp, state_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        std::pair<std::uint64_t, std::int64_t> chosen{unit_second, 0};
        for (const auto& unit : units_in_order) {
            const auto value = static_cast<std::int64_t>(sp->send_id(s.x[2], unit.selector));
            if (value != 0 && value != INT64_MAX) { chosen = {unit.flag, value}; break; }
        }
        s.x[0] = make_string(*sp, relative_text(*state_of(s.x[0]), chosen.first, chosen.second));
    }, "@@:@");
    support.method(cls, "stringForObjectValue:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? sp->send_id(s.x[0], "localizedStringForDate:relativeToDate:",
                                      {s.x[2], autoreleased_date(*sp, sp->cf.absolute_time_now())}) : 0;
    }, "@@:@");
}

void install_interval_formatter(Support& support) {
    if (support.objc.find_class("NSDateIntervalFormatter")) return;
    const auto cls = support.declare_class("NSDateIntervalFormatter", "NSFormatter", foundation_library);
    auto* sp = &support;
    const auto state_of = [sp](Address self) { return sp->objc.host_state<IntervalFormatterState>(self); };
    using S = IntervalFormatterState;
    integer_property<S>(support, cls, "dateStyle", "setDateStyle:", &S::date_style);
    integer_property<S>(support, cls, "timeStyle", "setTimeStyle:", &S::time_style);
    address_property<S>(support, cls, "timeZone", "setTimeZone:", &S::time_zone);
    address_property<S>(support, cls, "locale", "setLocale:", &S::locale);
    address_property<S>(support, cls, "calendar", "setCalendar:", &S::calendar);
    support.method(cls, "dateTemplate", [sp, state_of](CpuState& s, GuestMemory&) { s.x[0] = make_string(*sp, state_of(s.x[0])->date_template); }, "@@:");
    support.method(cls, "setDateTemplate:", [sp, state_of](CpuState& s, GuestMemory&) {
        const auto skeleton = units16(*sp, s.x[2]);
        try { if (!skeleton.empty()) static_cast<void>(dates::pattern_from_template(skeleton)); }
        catch (const std::exception& error) { unsupported(std::string("NSDateIntervalFormatter template: ") + error.what()); }
        state_of(s.x[0])->date_template = skeleton;
    }, "v@:@");
    const auto format = [sp, state_of](Address self, double from, double to) -> Address {
        const auto state = state_of(self);
        const auto offset_from = zone_offset(*sp, state->time_zone, from), offset_to = zone_offset(*sp, state->time_zone, to);
        const auto style = [](std::int64_t date, std::int64_t time) {
            const auto text = dates::style_pattern(static_cast<dates::Style>(date), static_cast<dates::Style>(time));
            return std::u16string(text.begin(), text.end());
        };
        const auto pattern = !state->date_template.empty() ? dates::pattern_from_template(state->date_template) : style(state->date_style, state->time_style);
        if (pattern.empty()) return make_string(*sp, std::u16string());
        const auto first = dates::format(pattern, from, offset_from), second = dates::format(pattern, to, offset_to);
        if (first == second) return make_string(*sp, first);
        const std::u16string dash{char16_t(0x2009), char16_t(0x2013), char16_t(0x2009)};   // thin space, en dash, thin space
        const auto day = [](double when, std::int64_t offset) { return std::floor((when + static_cast<double>(offset)) / 86400.0); };
        if (state->date_template.empty() && state->date_style != 0 && state->time_style != 0 && day(from, offset_from) == day(to, offset_to)) {
            const auto date = dates::format(style(state->date_style, 0), from, offset_from);
            const auto time = style(0, state->time_style);
            return make_string(*sp, date + u", " + dates::format(time, from, offset_from) + dash + dates::format(time, to, offset_to));
        }
        return make_string(*sp, first + dash + second);
    };
    support.method(cls, "stringFromDate:toDate:", [sp, format](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] && s.x[3] ? format(s.x[0], sp->cf.date_time(s.x[2]), sp->cf.date_time(s.x[3])) : 0;
    }, "@@:@@");
    support.method(cls, "stringFromDateInterval:", [sp, format](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto start = sp->send_id(s.x[2], "startDate"), end = sp->send_id(s.x[2], "endDate");
        s.x[0] = format(s.x[0], sp->cf.date_time(start), sp->cf.date_time(end));
    }, "@@:@");
    support.method(cls, "dealloc", [sp, cls](CpuState& s, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<IntervalFormatterState>(s.x[0]))
            for (auto* slot : {&state->time_zone, &state->locale, &state->calendar}) if (*slot) sp->send_id(std::exchange(*slot, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace

void install_date_formatters(Support& support) {
    if (!support.objc.find_class("NSFormatter")) static_cast<void>(support.declare_class("NSFormatter", "NSObject", foundation_library));
    install_date_interval(support);
    install_iso8601_formatter(support);
    install_components_formatter(support);
    install_relative_formatter(support);
    install_interval_formatter(support);
}

} // namespace alchemy::gui::completion_kit
