#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/calendar.hpp"
#include "alchemy/darwin/foundation/date_format.hpp"

#include <cmath>

// NSCalendar (Gregorian and ISO 8601 identifiers) and NSDateComponents over
// the calendar core (calendar.cpp), with time-zone offsets from the
// calendar's NSTimeZone (-secondsFromGMTForDate:). Other calendar
// identifiers Foundation knows (hebrew, islamic, ...) fail explicitly;
// unknown ones give nil. NSCalendarWrapComponents is not provided.
// NSDate additions: +dateWithTimeInterval:sinceDate:, +now,
// -descriptionWithLocale: (NSDate and NSNumber).
namespace alchemy::gui::completion_kit::data {
namespace {

using calendar::Components;

struct CalendarState final : ObjectPayload {
    std::string identifier{"gregorian"};
    Address zone{};     // +1
    Address locale{};   // +1, may be nil
    calendar::Rules rules;
};
struct ComponentsState final : ObjectPayload {
    Components value;
    Address calendar{};   // +1
    Address zone{};       // +1
    bool leap_month{};
};

constexpr std::uint64_t wrap_components = 1;

struct Calendars {
    Support& s;
    Guest g{s};
    Address calendar_class{}, components_class{}, zone_class{}, date_class{};

    std::shared_ptr<CalendarState> state(Address self) { return s.objc.host_state<CalendarState>(self); }
    // false: unknown identifier.
    bool configure(Address self, const std::string& identifier) {
        if (identifier != "gregorian" && identifier != "iso8601") {
            static constexpr const char* known[] = {"buddhist", "chinese", "coptic", "ethiopic-amete-alem", "ethiopic", "hebrew", "indian", "islamic",
                                                    "islamic-civil", "islamic-tbla", "islamic-umalqura", "islamicc", "japanese", "persian", "roc"};
            for (const auto* k : known) if (identifier == k) unsupported("NSCalendar identifier \"" + identifier + "\" (only gregorian and iso8601)");
            return false;
        }
        auto st = state(self);
        st->identifier = identifier;
        st->zone = g.retain(s.send_id(zone_class, "defaultTimeZone"));
        st->locale = g.retain(s.send_id(s.objc.find_class("NSLocale"), "currentLocale"));
        st->rules = identifier == "iso8601" ? calendar::Rules{2, 4} : calendar::Rules{1, 1};
        return true;
    }
    calendar::Offset offset(Address zone) {
        return [this, zone](double reference) -> std::int64_t {
            if (!zone) return 0;
            const auto date = s.cf.date(reference);
            const auto seconds = static_cast<std::int64_t>(s.send_id(zone, "secondsFromGMTForDate:", {date}));
            s.release(date);
            return seconds;
        };
    }
    double date_value(Address date) {
        if (!date) fail("NSInvalidArgumentException: NSCalendar: nil date");
        if (s.cf.kind(date) == ObjectKind::date) return s.cf.date_time(date);
        return std::bit_cast<double>(s.send(date, "timeIntervalSinceReferenceDate").v[0][0]);
    }
    Address date(double reference) { return g.autorelease(s.cf.date(reference)); }
    std::shared_ptr<ComponentsState> components(Address self) { return s.objc.host_state<ComponentsState>(self); }
    Address make_components(const Components& c, Address calendar_object, Address zone) {
        const auto made = s.objc.allocate(components_class);
        auto st = components(made);
        st->value = c;
        st->calendar = g.retain(calendar_object);
        st->zone = g.retain(zone);
        return made;   // +1
    }
    Components components_of(Address object) {
        if (const auto st = s.objc.find_host_state<ComponentsState>(object)) return st->value;
        if (!object) fail("NSInvalidArgumentException: nil NSDateComponents");
        fail("NSInvalidArgumentException: an NSDateComponents of another implementation");
    }
};

const std::pair<const char*, calendar::Unit> component_names[] = {
    {"era", calendar::era}, {"year", calendar::year}, {"month", calendar::month}, {"day", calendar::day}, {"hour", calendar::hour},
    {"minute", calendar::minute}, {"second", calendar::second}, {"nanosecond", calendar::nanosecond}, {"weekday", calendar::weekday},
    {"weekdayOrdinal", calendar::weekday_ordinal}, {"quarter", calendar::quarter}, {"weekOfMonth", calendar::week_of_month},
    {"weekOfYear", calendar::week_of_year}, {"yearForWeekOfYear", calendar::year_for_week_of_year},
};

} // namespace

void install_calendars(Support& support) {
    auto cal = std::make_shared<Calendars>(Calendars{support});
    auto* sp = &support;
    const auto nc = support.declare_class("NSCalendar", "NSObject", foundation_library);
    const auto dcm = support.declare_class("NSDateComponents", "NSObject", foundation_library);
    cal->calendar_class = nc;
    cal->components_class = dcm;
    cal->zone_class = support.objc.find_class("NSTimeZone");
    cal->date_class = support.objc.find_class("NSDate");
    if (!cal->zone_class || !cal->date_class) fail("NSCalendar needs NSTimeZone and NSDate");

    // ---- NSCalendar ----
    for (const auto* name : {"currentCalendar", "autoupdatingCurrentCalendar"})
        support.method(nc, name, [cal](CpuState& s, GuestMemory&) {
            const auto self = cal->s.objc.allocate(s.x[0]);
            cal->configure(self, "gregorian");
            s.x[0] = cal->g.autorelease(self);
        }, "@@:", true);
    support.method(nc, "calendarWithIdentifier:", [cal](CpuState& s, GuestMemory&) {
        const auto self = cal->s.objc.allocate(s.x[0]);
        if (!cal->configure(self, cal->g.utf8(s.x[2]))) { cal->g.release(self); s.x[0] = 0; return; }
        s.x[0] = cal->g.autorelease(self);
    }, "@@:@", true);
    support.method(nc, "initWithCalendarIdentifier:", [cal](CpuState& s, GuestMemory&) {
        if (!cal->configure(s.x[0], cal->g.utf8(s.x[2]))) { cal->g.release(s.x[0]); s.x[0] = 0; }
    }, "@@:@");
    support.method(nc, "init", [cal](CpuState& s, GuestMemory&) { cal->configure(s.x[0], "gregorian"); }, "@@:");
    support.method(nc, "calendarIdentifier", [cal](CpuState& s, GuestMemory&) { s.x[0] = cal->s.cf.constant_string(cal->state(s.x[0])->identifier); }, "@@:");
    support.method(nc, "timeZone", [cal](CpuState& s, GuestMemory&) { s.x[0] = cal->state(s.x[0])->zone; }, "@@:");
    support.method(nc, "setTimeZone:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto zone = s.x[2] ? cal->g.retain(s.x[2]) : cal->g.retain(cal->s.send_id(cal->zone_class, "defaultTimeZone"));
        cal->g.release(std::exchange(st->zone, zone));
    }, "v@:@");
    support.method(nc, "locale", [cal](CpuState& s, GuestMemory&) { s.x[0] = cal->state(s.x[0])->locale; }, "@@:");
    support.method(nc, "setLocale:", [cal](CpuState& s, GuestMemory&) { cal->g.release(std::exchange(cal->state(s.x[0])->locale, cal->g.retain(s.x[2]))); }, "v@:@");
    support.method(nc, "firstWeekday", [cal](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cal->state(s.x[0])->rules.first_weekday); }, "Q@:");
    support.method(nc, "setFirstWeekday:", [cal](CpuState& s, GuestMemory&) {
        if (s.x[2] < 1 || s.x[2] > 7) return;
        cal->state(s.x[0])->rules.first_weekday = static_cast<int>(s.x[2]);
    }, "v@:Q");
    support.method(nc, "minimumDaysInFirstWeek", [cal](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cal->state(s.x[0])->rules.minimum_days); }, "Q@:");
    support.method(nc, "setMinimumDaysInFirstWeek:", [cal](CpuState& s, GuestMemory&) {
        if (s.x[2] < 1 || s.x[2] > 7) return;
        cal->state(s.x[0])->rules.minimum_days = static_cast<int>(s.x[2]);
    }, "v@:Q");
    support.method(nc, "components:fromDate:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto c = calendar::split(cal->date_value(s.x[3]), s.x[2], cal->offset(st->zone), st->rules);
        s.x[0] = cal->g.autorelease(cal->make_components(c, (s.x[2] & calendar::calendar_unit) ? s.x[0] : 0, (s.x[2] & calendar::time_zone_unit) ? st->zone : 0));
    }, "@@:Q@");
    support.method(nc, "componentsInTimeZone:fromDate:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        constexpr std::uint64_t all = calendar::era | calendar::year | calendar::month | calendar::day | calendar::hour | calendar::minute | calendar::second |
                                      calendar::nanosecond | calendar::weekday | calendar::weekday_ordinal | calendar::quarter | calendar::week_of_month |
                                      calendar::week_of_year | calendar::year_for_week_of_year;
        const auto c = calendar::split(cal->date_value(s.x[3]), all, cal->offset(s.x[2]), st->rules);
        s.x[0] = cal->g.autorelease(cal->make_components(c, s.x[0], s.x[2]));
    }, "@@:@@");
    support.method(nc, "component:fromDate:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto unit = static_cast<calendar::Unit>(s.x[2]);
        const auto c = calendar::split(cal->date_value(s.x[3]), s.x[2], cal->offset(st->zone), st->rules);
        s.x[0] = static_cast<Address>(c.get(unit));
    }, "q@:Q@");
    support.method(nc, "components:fromDate:toDate:options:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto c = calendar::difference(cal->date_value(s.x[3]), cal->date_value(s.x[4]), s.x[2], cal->offset(st->zone), st->rules);
        s.x[0] = cal->g.autorelease(cal->make_components(c, (s.x[2] & calendar::calendar_unit) ? s.x[0] : 0, 0));
    }, "@@:Q@@Q");
    support.method(nc, "dateFromComponents:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto c = cal->components(s.x[2]);
        const auto zone = c->zone ? c->zone : st->zone;
        s.x[0] = cal->date(calendar::combine(c->value, cal->offset(zone), st->rules));
    }, "@@:@");
    support.method(nc, "dateByAddingComponents:toDate:options:", [cal](CpuState& s, GuestMemory&) {
        if (s.x[4] & wrap_components) unsupported("-[NSCalendar dateByAddingComponents:toDate:options:] with NSCalendarWrapComponents");
        const auto st = cal->state(s.x[0]);
        s.x[0] = cal->date(calendar::add(cal->date_value(s.x[3]), cal->components_of(s.x[2]), cal->offset(st->zone)));
    }, "@@:@@Q");
    support.method(nc, "dateByAddingUnit:value:toDate:options:", [cal](CpuState& s, GuestMemory&) {
        if (s.x[5] & wrap_components) unsupported("-[NSCalendar dateByAddingUnit:value:toDate:options:] with NSCalendarWrapComponents");
        const auto st = cal->state(s.x[0]);
        Components delta;
        delta.set(static_cast<calendar::Unit>(s.x[2]), static_cast<std::int64_t>(s.x[3]));
        s.x[0] = cal->date(calendar::add(cal->date_value(s.x[4]), delta, cal->offset(st->zone)));
    }, "@@:Qq@Q");
    support.method(nc, "dateWithEra:year:month:day:hour:minute:second:nanosecond:", [cal](CpuState& s, GuestMemory& m) {
        const auto st = cal->state(s.x[0]);
        Components c;
        c.era = static_cast<std::int64_t>(s.x[2]);
        c.year = static_cast<std::int64_t>(s.x[3]);
        c.month = static_cast<std::int64_t>(s.x[4]);
        c.day = static_cast<std::int64_t>(s.x[5]);
        c.hour = static_cast<std::int64_t>(s.x[6]);
        c.minute = static_cast<std::int64_t>(s.x[7]);
        c.second = static_cast<std::int64_t>(m.read64(s.sp));
        c.nanosecond = static_cast<std::int64_t>(m.read64(s.sp + 8));
        s.x[0] = cal->date(calendar::combine(c, cal->offset(st->zone), st->rules));
    }, "@@:qqqqqqqq");
    support.method(nc, "startOfDayForDate:", [cal](CpuState& s, GuestMemory&) {
        s.x[0] = cal->date(calendar::start_of_day(cal->date_value(s.x[2]), cal->offset(cal->state(s.x[0])->zone)));
    }, "@@:@");
    const auto same_day = [cal](Address self, double a, double b) {
        const auto off = cal->offset(cal->state(self)->zone);
        return calendar::start_of_day(a, off) == calendar::start_of_day(b, off);
    };
    support.method(nc, "isDate:inSameDayAsDate:", [cal, same_day](CpuState& s, GuestMemory&) { return_bool(s, same_day(s.x[0], cal->date_value(s.x[2]), cal->date_value(s.x[3]))); }, "B@:@@");
    for (const auto& [name, days] : std::initializer_list<std::pair<const char*, int>>{{"isDateInToday:", 0}, {"isDateInYesterday:", -1}, {"isDateInTomorrow:", 1}}) {
        const auto shift = days;
        support.method(nc, name, [cal, same_day, shift](CpuState& s, GuestMemory&) {
            Components d;
            d.day = shift;
            const auto st = cal->state(s.x[0]);
            const auto target = calendar::add(CoreFoundation::absolute_time_now(), d, cal->offset(st->zone));
            return_bool(s, same_day(s.x[0], cal->date_value(s.x[2]), target));
        }, "B@:@");
    }
    support.method(nc, "isDateInWeekend:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto c = calendar::split(cal->date_value(s.x[2]), calendar::weekday, cal->offset(st->zone), st->rules);
        return_bool(s, c.weekday == 1 || c.weekday == 7);
    }, "B@:@");
    support.method(nc, "dateBySettingHour:minute:second:ofDate:options:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        auto c = calendar::split(cal->date_value(s.x[5]), calendar::era | calendar::year | calendar::month | calendar::day, cal->offset(st->zone), st->rules);
        c.hour = static_cast<std::int64_t>(s.x[2]);
        c.minute = static_cast<std::int64_t>(s.x[3]);
        c.second = static_cast<std::int64_t>(s.x[4]);
        if (c.hour < 0 || c.hour > 23 || c.minute < 0 || c.minute > 59 || c.second < 0 || c.second > 59) { s.x[0] = 0; return; }
        s.x[0] = cal->date(calendar::combine(c, cal->offset(st->zone), st->rules));
    }, "@@:qqq@Q");
    support.method(nc, "rangeOfUnit:inUnit:forDate:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto r = calendar::range(static_cast<calendar::Unit>(s.x[2]), static_cast<calendar::Unit>(s.x[3]), cal->date_value(s.x[4]), cal->offset(st->zone), st->rules);
        if (!r) unsupported("-[NSCalendar rangeOfUnit:inUnit:forDate:] for units " + std::to_string(s.x[2]) + " in " + std::to_string(s.x[3]));
        return_range(s, r->first, r->second);
    }, "{_NSRange=QQ}@:QQ@");
    support.method(nc, "ordinalityOfUnit:inUnit:forDate:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto r = calendar::ordinality(static_cast<calendar::Unit>(s.x[2]), static_cast<calendar::Unit>(s.x[3]), cal->date_value(s.x[4]), cal->offset(st->zone), st->rules);
        if (!r) unsupported("-[NSCalendar ordinalityOfUnit:inUnit:forDate:] for units " + std::to_string(s.x[2]) + " in " + std::to_string(s.x[3]));
        s.x[0] = static_cast<Address>(*r);
    }, "Q@:QQ@");
    support.method(nc, "compareDate:toDate:toUnitGranularity:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto unit = s.x[4];
        std::uint64_t units = calendar::era | calendar::year;
        for (const auto u : {calendar::month, calendar::day, calendar::hour, calendar::minute, calendar::second}) {
            if (unit < u) break;
            units |= u;
        }
        if (unit == calendar::week_of_year || unit == calendar::week_of_month) units = calendar::year_for_week_of_year | calendar::week_of_year;
        const auto a = calendar::split(cal->date_value(s.x[2]), units, cal->offset(st->zone), st->rules);
        const auto b = calendar::split(cal->date_value(s.x[3]), units, cal->offset(st->zone), st->rules);
        std::int64_t r = 0;
        for (const auto u : {calendar::era, calendar::year, calendar::year_for_week_of_year, calendar::week_of_year, calendar::month, calendar::day, calendar::hour, calendar::minute, calendar::second}) {
            if (!(units & u) || r) continue;
            const auto x = a.get(u), y = b.get(u);
            if (x != y) r = x < y ? -1 : 1;
        }
        s.x[0] = static_cast<Address>(r);
    }, "q@:@@Q");
    support.method(nc, "copyWithZone:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->state(s.x[0]);
        const auto copy = cal->s.objc.allocate(cal->s.objc.object_class(s.x[0]));
        auto other = cal->state(copy);
        other->identifier = st->identifier;
        other->zone = cal->g.retain(st->zone);
        other->locale = cal->g.retain(st->locale);
        other->rules = st->rules;
        s.x[0] = copy;
    }, "@@:^v");
    support.method(nc, "isEqual:", [cal](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        const auto a = cal->state(s.x[0]);
        const auto b = s.x[2] ? cal->s.objc.find_host_state<CalendarState>(s.x[2]) : nullptr;
        return_bool(s, b && a->identifier == b->identifier && a->rules.first_weekday == b->rules.first_weekday &&
                           a->rules.minimum_days == b->rules.minimum_days && cal->g.is_equal(a->zone, b->zone));
    }, "B@:@");
    support.method(nc, "hash", [cal](CpuState& s, GuestMemory&) { s.x[0] = std::hash<std::string>{}(cal->state(s.x[0])->identifier); }, "Q@:");
    support.method(nc, "dealloc", [sp, nc](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<CalendarState>(s.x[0]))
            for (auto* held : {&st->zone, &st->locale}) if (*held) sp->send_id(std::exchange(*held, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], nc, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSDateComponents ----
    for (const auto& [name, unit] : component_names) {
        const std::string getter = name;
        std::string setter = "set" + getter + ":";
        setter[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(setter[3])));
        const auto u = unit;
        support.method(dcm, getter, [cal, u](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cal->components(s.x[0])->value.get(u)); }, "q@:");
        support.method(dcm, setter, [cal, u](CpuState& s, GuestMemory&) { cal->components(s.x[0])->value.set(u, static_cast<std::int64_t>(s.x[2])); }, "v@:q");
    }
    support.method(dcm, "init", [cal](CpuState& s, GuestMemory&) { cal->components(s.x[0]); }, "@@:");
    support.method(dcm, "valueForComponent:", [cal](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cal->components(s.x[0])->value.get(static_cast<calendar::Unit>(s.x[2]))); }, "q@:Q");
    support.method(dcm, "setValue:forComponent:", [cal](CpuState& s, GuestMemory&) { cal->components(s.x[0])->value.set(static_cast<calendar::Unit>(s.x[3]), static_cast<std::int64_t>(s.x[2])); }, "v@:qQ");
    support.method(dcm, "isLeapMonth", [cal](CpuState& s, GuestMemory&) { return_bool(s, cal->components(s.x[0])->leap_month); }, "B@:");
    support.method(dcm, "setLeapMonth:", [cal](CpuState& s, GuestMemory&) {
        if (arg_bool(s.x[2])) unsupported("NSDateComponents leap months (no lunisolar calendars)");
        cal->components(s.x[0])->leap_month = false;
    }, "v@:B");
    support.method(dcm, "calendar", [cal](CpuState& s, GuestMemory&) { s.x[0] = cal->components(s.x[0])->calendar; }, "@@:");
    support.method(dcm, "setCalendar:", [cal](CpuState& s, GuestMemory&) { cal->g.release(std::exchange(cal->components(s.x[0])->calendar, cal->g.retain(s.x[2]))); }, "v@:@");
    support.method(dcm, "timeZone", [cal](CpuState& s, GuestMemory&) { s.x[0] = cal->components(s.x[0])->zone; }, "@@:");
    support.method(dcm, "setTimeZone:", [cal](CpuState& s, GuestMemory&) { cal->g.release(std::exchange(cal->components(s.x[0])->zone, cal->g.retain(s.x[2]))); }, "v@:@");
    support.method(dcm, "date", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->components(s.x[0]);
        if (!st->calendar) { s.x[0] = 0; return; }
        s.x[0] = cal->s.send_id(st->calendar, "dateFromComponents:", {s.x[0]});
    }, "@@:");
    support.method(dcm, "isValidDate", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->components(s.x[0]);
        const auto& c = st->value;
        bool valid = st->calendar != 0;
        if (valid && c.month != calendar::undefined) valid = c.month >= 1 && c.month <= 12;
        if (valid && c.day != calendar::undefined) valid = c.day >= 1 && c.day <= (c.month != calendar::undefined && c.year != calendar::undefined ? calendar::days_in_month(c.year, c.month) : 31);
        if (valid && c.hour != calendar::undefined) valid = c.hour >= 0 && c.hour <= 23;
        if (valid && c.minute != calendar::undefined) valid = c.minute >= 0 && c.minute <= 59;
        if (valid && c.second != calendar::undefined) valid = c.second >= 0 && c.second <= 59;
        return_bool(s, valid);
    }, "B@:");
    support.method(dcm, "copyWithZone:", [cal](CpuState& s, GuestMemory&) {
        const auto st = cal->components(s.x[0]);
        s.x[0] = cal->make_components(st->value, st->calendar, st->zone);
    }, "@@:^v");
    support.method(dcm, "isEqual:", [cal](CpuState& s, GuestMemory&) {
        const auto b = s.x[2] ? cal->s.objc.find_host_state<ComponentsState>(s.x[2]) : nullptr;
        if (!b) { s.x[0] = s.x[0] == s.x[2]; return; }
        const auto a = cal->components(s.x[0]);
        bool equal = true;
        for (const auto& [name, unit] : component_names) equal = equal && a->value.get(unit) == b->value.get(unit);
        return_bool(s, equal);
    }, "B@:@");
    support.method(dcm, "hash", [cal](CpuState& s, GuestMemory&) {
        const auto a = cal->components(s.x[0]);
        std::uint64_t h = 0;
        for (const auto& [name, unit] : component_names) h = h * 31 + static_cast<std::uint64_t>(a->value.get(unit));
        s.x[0] = h;
    }, "Q@:");
    support.method(dcm, "description", [cal](CpuState& s, GuestMemory&) {
        const auto a = cal->components(s.x[0]);
        std::string text = "<NSDateComponents>";
        for (const auto& [name, unit] : component_names)
            if (const auto v = a->value.get(unit); v != calendar::undefined) text += std::string("\n    ") + name + ": " + std::to_string(v);
        s.x[0] = cal->g.auto_string(std::string_view(text));
    }, "@@:");
    support.method(dcm, "dealloc", [sp, dcm](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<ComponentsState>(s.x[0]))
            for (auto* held : {&st->calendar, &st->zone}) if (*held) sp->send_id(std::exchange(*held, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], dcm, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSDate / NSNumber additions ----
    const auto nd = cal->date_class;
    fill_method(support, nd, "dateWithTimeInterval:sinceDate:", [cal](CpuState& s, GuestMemory&) {
        s.x[0] = cal->date(cal->date_value(s.x[2]) + std::bit_cast<double>(s.v[0][0]));
    }, "@@:d@", true);
    fill_method(support, nd, "initWithTimeInterval:sinceDate:", [cal](CpuState& s, GuestMemory&) {
        const auto value = cal->date_value(s.x[2]) + std::bit_cast<double>(s.v[0][0]);
        cal->g.release(s.x[0]);
        s.x[0] = cal->s.cf.date(value);
    }, "@@:d@");
    fill_method(support, nd, "now", [cal](CpuState& s, GuestMemory&) { s.x[0] = cal->date(CoreFoundation::absolute_time_now()); }, "@@:", true);
    fill_method(support, nd, "descriptionWithLocale:", [cal](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = cal->s.send_id(s.x[0], "description"); return; }
        // A locale: the full date and long time styles in the default time zone (English names).
        const auto reference = cal->date_value(s.x[0]);
        const auto zone = cal->s.send_id(cal->zone_class, "defaultTimeZone");
        const auto offset = cal->offset(zone)(reference);
        const auto pattern = dates::style_pattern(dates::Style::full, dates::Style::long_);
        s.x[0] = cal->g.auto_string(dates::format(std::u16string(pattern.begin(), pattern.end()), reference, offset));
    }, "@@:@");
    if (const auto number = support.objc.find_class("NSNumber"))
        fill_method(support, number, "descriptionWithLocale:", [cal](CpuState& s, GuestMemory&) {
            auto text = cal->g.text(cal->s.send_id(s.x[0], "stringValue"));
            if (s.x[2]) {
                const auto separator = cal->g.text(cal->s.send_id(s.x[2], "decimalSeparator"));
                if (!separator.empty()) { const auto dot = text.find(u'.'); if (dot != std::u16string::npos) text.replace(dot, 1, separator); }
            }
            s.x[0] = cal->g.auto_string(text);
        }, "@@:@");
}

} // namespace alchemy::gui::completion_kit::data
