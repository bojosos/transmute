#pragma once

// NSCalendar's Gregorian arithmetic (host-only), on NSDate reference times
// (seconds since 2001-01-01 00:00:00 UTC) with a time-zone offset function.
// The calendar is the proleptic Gregorian one (no Julian dates before
// 1582-10-15). Week numbering follows the calendar's firstWeekday and
// minimumDaysInFirstWeek. Out-of-range components (month 13, day 0,
// hour 25) are normalized leniently, as Foundation does.
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace alchemy::gui::completion_kit::calendar {

inline constexpr std::int64_t undefined = std::numeric_limits<std::int64_t>::max();   // NSDateComponentUndefined

// NSCalendarUnit.
enum Unit : std::uint64_t {
    era = 1u << 1, year = 1u << 2, month = 1u << 3, day = 1u << 4, hour = 1u << 5, minute = 1u << 6, second = 1u << 7,
    weekday = 1u << 9, weekday_ordinal = 1u << 10, quarter = 1u << 11, week_of_month = 1u << 12, week_of_year = 1u << 13,
    year_for_week_of_year = 1u << 14, nanosecond = 1u << 15, calendar_unit = 1u << 20, time_zone_unit = 1u << 21,
};

struct Components {
    std::int64_t era{undefined}, year{undefined}, month{undefined}, day{undefined};
    std::int64_t hour{undefined}, minute{undefined}, second{undefined}, nanosecond{undefined};
    std::int64_t weekday{undefined}, weekday_ordinal{undefined}, quarter{undefined};
    std::int64_t week_of_month{undefined}, week_of_year{undefined}, year_for_week_of_year{undefined};

    [[nodiscard]] std::int64_t get(Unit unit) const;
    void set(Unit unit, std::int64_t value);
};

struct Rules {
    int first_weekday{1};        // 1 Sunday ... 7 Saturday
    int minimum_days{1};         // minimumDaysInFirstWeek
};

// Seconds east of UTC at a UTC reference time.
using Offset = std::function<std::int64_t(double reference)>;

[[nodiscard]] Components split(double reference, std::uint64_t units, const Offset& offset, const Rules& rules);
// -dateFromComponents: (missing fields: era AD, year 1, January 1, 00:00:00).
[[nodiscard]] double combine(const Components& value, const Offset& offset, const Rules& rules);
// -dateByAddingComponents:toDate:options: (no wrapping): years and months
// keep the day of month when it exists (clamped otherwise), days keep the
// wall-clock time, and smaller units add elapsed time.
[[nodiscard]] double add(double reference, const Components& delta, const Offset& offset);
// -components:fromDate:toDate:options: for `units`.
[[nodiscard]] Components difference(double from, double to, std::uint64_t units, const Offset& offset, const Rules& rules);
// -rangeOfUnit:inUnit:forDate: and -ordinalityOfUnit:inUnit:forDate:;
// nullopt for combinations that are not provided.
[[nodiscard]] std::optional<std::pair<std::int64_t, std::int64_t>> range(Unit smaller, Unit larger, double reference, const Offset& offset, const Rules& rules);
[[nodiscard]] std::optional<std::int64_t> ordinality(Unit smaller, Unit larger, double reference, const Offset& offset, const Rules& rules);
// The start of the local day containing `reference`.
[[nodiscard]] double start_of_day(double reference, const Offset& offset);

[[nodiscard]] std::int64_t days_in_month(std::int64_t year, std::int64_t month);

} // namespace alchemy::gui::completion_kit::calendar
