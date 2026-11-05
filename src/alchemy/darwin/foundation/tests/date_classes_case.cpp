// The newer Foundation date classes (foundation/date_formatters.cpp) and
// CFDateFormatter parsing (corefoundation/cf_locale.cpp), with the full
// framework install (NSCalendar, NSTimeZone).
#include "alchemy/gui/tests/framework_case.hpp"

#include <string>

namespace {
using namespace fill_test;

std::string text(Fixture& f, Address string) { return string ? f.cf.utf8(string) : std::string("(nil)"); }

// The newer date classes (date_formatters.cpp) and CFDateFormatter parsing.
const Case foundation_date_classes_case("foundation_date_classes", [](Fixture& f) {
    constexpr double t0 = 700000000.0;   // 2023-03-08 20:26:40 UTC
    const auto date = [&](double when) { return f.cf.date(when); };
    const auto gmt = f.send(f.cls("NSTimeZone"), "timeZoneForSecondsFromGMT:", {0});
    // NSDateInterval.
    const auto interval = f.send(f.send(f.cls("NSDateInterval"), "alloc"), "initWithStartDate:endDate:", {date(t0), date(t0 + 3600)});
    require(f.cf.date_time(f.send(interval, "endDate")) == t0 + 3600, "NSDateInterval endDate");
    require(f.send(interval, "containsDate:", {date(t0 + 60)}) == 1 && f.send(interval, "containsDate:", {date(t0 + 7200)}) == 0, "containsDate:");
    const auto later = f.send(f.send(f.cls("NSDateInterval"), "alloc"), "initWithStartDate:endDate:", {date(t0 + 1800), date(t0 + 5400)});
    const auto both = f.send(interval, "intersectionWithDateInterval:", {later});
    require(both && f.cf.date_time(f.send(both, "startDate")) == t0 + 1800 && f.cf.date_time(f.send(both, "endDate")) == t0 + 3600,
            "intersectionWithDateInterval:");
    require(static_cast<std::int64_t>(f.send(interval, "compare:", {later})) == -1, "compare:");
    // NSISO8601DateFormatter: the internet date-time in GMT, round trip, other options.
    const auto iso = f.send(f.send(f.cls("NSISO8601DateFormatter"), "alloc"), "init");
    require(text(f, f.send(iso, "stringFromDate:", {date(t0)})) == "2023-03-08T20:26:40Z", "NSISO8601DateFormatter");
    require(f.cf.date_time(f.send(iso, "dateFromString:", {f.cf.string("2023-03-08T20:26:40Z")})) == t0, "ISO 8601 parsing");
    require(f.cf.date_time(f.send(iso, "dateFromString:", {f.cf.string("2023-03-08T22:26:40+02:00")})) == t0, "ISO 8601 with an offset");
    f.send(iso, "setFormatOptions:", {1 | 2 | 16 | 256});   // full date
    require(text(f, f.send(iso, "stringFromDate:", {date(t0)})) == "2023-03-08", "full-date option");
    require(f.send(iso, "dateFromString:", {f.cf.string("not a date")}) == 0, "an invalid string");
    // NSDateComponentsFormatter.
    const auto components = f.send(f.send(f.cls("NSDateComponentsFormatter"), "alloc"), "init");
    f.send(components, "setAllowedUnits:", {32 | 64 | 128});   // hour, minute, second
    require(text(f, f.send(components, "stringFromDate:toDate:", {date(t0), date(t0 + 3661)})) == "1:01:01", "positional");
    require(text(f, f.send(components, "stringFromDate:toDate:", {date(t0), date(t0 + 61)})) == "1:01", "positional drops the leading zero");
    f.send(components, "setUnitsStyle:", {1});
    require(text(f, f.send(components, "stringFromDate:toDate:", {date(t0), date(t0 + 3610)})) == "1h 10s", "abbreviated drops zero units");
    f.send(components, "setUnitsStyle:", {3});
    require(text(f, f.send(components, "stringFromDate:toDate:", {date(t0), date(t0 + 3661)})) == "1 hour, 1 minute, 1 second", "full");
    // NSRelativeDateTimeFormatter.
    const auto relative = f.send(f.send(f.cls("NSRelativeDateTimeFormatter"), "alloc"), "init");
    require(text(f, f.send(relative, "localizedStringForDate:relativeToDate:", {date(t0 + 7200), date(t0)})) == "in 2 hours", "in 2 hours");
    require(text(f, f.send(relative, "localizedStringForDate:relativeToDate:", {date(t0 - 180), date(t0)})) == "3 minutes ago", "3 minutes ago");
    f.send(relative, "setDateTimeStyle:", {1});
    require(text(f, f.send(relative, "localizedStringForDate:relativeToDate:", {date(t0 - 86400), date(t0)})) == "yesterday", "named");
    // NSDateIntervalFormatter: one day collapses to the date and two times.
    const auto range = f.send(f.send(f.cls("NSDateIntervalFormatter"), "alloc"), "init");
    f.send(range, "setDateStyle:", {1});
    f.send(range, "setTimeStyle:", {1});
    f.send(range, "setTimeZone:", {gmt});
    // "3/8/23, 8:26 PM<thin space><en dash><thin space>9:26 PM" (UTF-8 bytes).
    const auto range_text = text(f, f.send(range, "stringFromDate:toDate:", {date(t0), date(t0 + 3600)}));
    require(range_text == "3/8/23, 8:26 PM\xe2\x80\x89\xe2\x80\x93\xe2\x80\x89" "9:26 PM", "NSDateIntervalFormatter: " + range_text);
    require(text(f, f.send(range, "stringFromDate:toDate:", {date(t0), date(t0)})).find("\xe2\x80\x93") == std::string::npos, "one date");
    // CFDateFormatter parsing.
    const auto cf_formatter = f.call(alchemy::core_foundation_library, "_CFDateFormatterCreate", {0, 0, 0, 0});
    static_cast<void>(f.call(alchemy::core_foundation_library, "_CFDateFormatterSetFormat", {cf_formatter, f.cf.string("yyyy-MM-dd HH:mm")}));
    const auto parsed = f.call(alchemy::core_foundation_library, "_CFDateFormatterCreateDateFromString", {0, cf_formatter, f.cf.string("2023-03-08 20:26"), 0});
    require(parsed != 0, "CFDateFormatterCreateDateFromString");
    const auto back = f.call(alchemy::core_foundation_library, "_CFDateFormatterCreateStringWithDate", {0, cf_formatter, parsed});
    require(text(f, back) == "2023-03-08 20:26", "parsed then formatted in the default time zone");
    require(f.call(alchemy::core_foundation_library, "_CFDateFormatterCreateDateFromString", {0, cf_formatter, f.cf.string("garbage"), 0}) == 0,
            "an unparsable string");
    const auto pattern = f.call(alchemy::core_foundation_library, "_CFDateFormatterCreateDateFormatFromTemplate", {0, f.cf.string("yyyyMMdd"), 0, 0});
    require(text(f, pattern) == "MM/dd/yyyy", "CFDateFormatterCreateDateFormatFromTemplate");
});

} // namespace
