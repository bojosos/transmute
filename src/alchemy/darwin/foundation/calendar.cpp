#include "alchemy/darwin/foundation/calendar.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace alchemy::gui::completion_kit::calendar {
namespace {

constexpr std::int64_t epoch_days = 11323;   // 1970-01-01 to 2001-01-01
constexpr double day_seconds = 86400.0;

std::int64_t floor_div(std::int64_t a, std::int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
std::int64_t floor_mod(std::int64_t a, std::int64_t b) { return a - floor_div(a, b) * b; }

// Days since 1970-01-01 (proleptic Gregorian; H. Hinnant's algorithms).
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2;
    const auto era = floor_div(y, 400);
    const auto yoe = y - era * 400;
    const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
struct Ymd { std::int64_t y, m, d; };
Ymd civil_from_days(std::int64_t z) {
    z += 719468;
    const auto era = floor_div(z, 146097);
    const auto doe = z - era * 146097;
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const auto y = yoe + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto d = doy - (153 * mp + 2) / 5 + 1;
    const auto m = mp + (mp < 10 ? 3 : -9);
    return {y + (m <= 2), m, d};
}
std::int64_t weekday_of(std::int64_t days) { return floor_mod(days + 4, 7) + 1; }   // 1970-01-01 was a Thursday (5)
bool leap(std::int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

struct Local { std::int64_t days; double seconds; };   // days since 1970-01-01, seconds into the day
Local local(double reference, const Offset& offset) {
    const auto l = reference + static_cast<double>(offset(reference)) + static_cast<double>(epoch_days) * day_seconds;
    const auto days = static_cast<std::int64_t>(std::floor(l / day_seconds));
    return {days, l - static_cast<double>(days) * day_seconds};
}
// Local wall-clock seconds (since 1970-01-01 local) to a reference time.
double to_reference(double local_seconds, const Offset& offset) {
    const auto base = local_seconds - static_cast<double>(epoch_days) * day_seconds;
    const auto t0 = base - static_cast<double>(offset(base));
    const auto t1 = base - static_cast<double>(offset(t0));
    return t1;
}

// (week, year for week) of a day.
std::pair<std::int64_t, std::int64_t> week_number_of_year(std::int64_t days, const Rules& rules) {
    const auto ymd = civil_from_days(days);
    const auto first = [&](std::int64_t y) {
        const auto jan1 = days_from_civil(y, 1, 1);
        const auto rel = floor_mod(weekday_of(jan1) - rules.first_weekday, 7);
        // Start of week 1.
        return (7 - rel >= rules.minimum_days) ? jan1 - rel : jan1 - rel + 7;
    };
    const auto start_next = first(ymd.y + 1);
    if (days >= start_next) return {1, ymd.y + 1};
    const auto start = first(ymd.y);
    if (days < start) {
        const auto previous = first(ymd.y - 1);
        return {(days - previous) / 7 + 1, ymd.y - 1};
    }
    return {(days - start) / 7 + 1, ymd.y};
}
std::int64_t week_number_of_month(std::int64_t days, const Rules& rules) {
    const auto ymd = civil_from_days(days);
    const auto first_day = days_from_civil(ymd.y, ymd.m, 1);
    const auto rel = floor_mod(weekday_of(first_day) - rules.first_weekday, 7);
    const bool full = 7 - rel >= rules.minimum_days;
    return (ymd.d - 1 + rel) / 7 + (full ? 1 : 0);
}

std::int64_t value_or(std::int64_t v, std::int64_t fallback) { return v == undefined ? fallback : v; }

} // namespace

std::int64_t days_in_month(std::int64_t y, std::int64_t m) {
    static constexpr int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : lengths[(m - 1) % 12];
}

std::int64_t Components::get(Unit unit) const {
    switch (unit) {
    case Unit::era: return era;
    case Unit::year: return year;
    case Unit::month: return month;
    case Unit::day: return day;
    case Unit::hour: return hour;
    case Unit::minute: return minute;
    case Unit::second: return second;
    case Unit::nanosecond: return nanosecond;
    case Unit::weekday: return weekday;
    case Unit::weekday_ordinal: return weekday_ordinal;
    case Unit::quarter: return quarter;
    case Unit::week_of_month: return week_of_month;
    case Unit::week_of_year: return week_of_year;
    case Unit::year_for_week_of_year: return year_for_week_of_year;
    default: return undefined;
    }
}

void Components::set(Unit unit, std::int64_t value) {
    switch (unit) {
    case Unit::era: era = value; break;
    case Unit::year: year = value; break;
    case Unit::month: month = value; break;
    case Unit::day: day = value; break;
    case Unit::hour: hour = value; break;
    case Unit::minute: minute = value; break;
    case Unit::second: second = value; break;
    case Unit::nanosecond: nanosecond = value; break;
    case Unit::weekday: weekday = value; break;
    case Unit::weekday_ordinal: weekday_ordinal = value; break;
    case Unit::quarter: quarter = value; break;
    case Unit::week_of_month: week_of_month = value; break;
    case Unit::week_of_year: week_of_year = value; break;
    case Unit::year_for_week_of_year: year_for_week_of_year = value; break;
    default: break;
    }
}

Components split(double reference, std::uint64_t units, const Offset& offset, const Rules& rules) {
    const auto l = local(reference, offset);
    const auto ymd = civil_from_days(l.days);
    Components c;
    const auto whole = static_cast<std::int64_t>(std::floor(l.seconds));
    if (units & era) c.era = ymd.y > 0 ? 1 : 0;
    if (units & year) c.year = ymd.y > 0 ? ymd.y : 1 - ymd.y;
    if (units & month) c.month = ymd.m;
    if (units & day) c.day = ymd.d;
    if (units & hour) c.hour = whole / 3600;
    if (units & minute) c.minute = whole / 60 % 60;
    if (units & second) c.second = whole % 60;
    if (units & nanosecond) c.nanosecond = static_cast<std::int64_t>(std::llround((l.seconds - static_cast<double>(whole)) * 1e9));
    if (units & weekday) c.weekday = weekday_of(l.days);
    if (units & weekday_ordinal) c.weekday_ordinal = (ymd.d - 1) / 7 + 1;
    if (units & quarter) c.quarter = (ymd.m - 1) / 3 + 1;
    if (units & week_of_month) c.week_of_month = week_number_of_month(l.days, rules);
    if (units & (week_of_year | year_for_week_of_year)) {
        const auto [w, y] = week_number_of_year(l.days, rules);
        if (units & week_of_year) c.week_of_year = w;
        if (units & year_for_week_of_year) c.year_for_week_of_year = y;
    }
    return c;
}

double combine(const Components& c, const Offset& offset, const Rules& rules) {
    auto y = value_or(c.year, 1);
    if (c.era == 0) y = 1 - y;
    std::int64_t days;
    if (c.week_of_year != undefined && c.year_for_week_of_year != undefined && c.month == undefined && c.day == undefined) {
        const auto yfw = c.era == 0 ? 1 - c.year_for_week_of_year : c.year_for_week_of_year;
        const auto jan1 = days_from_civil(yfw, 1, 1);
        const auto rel = floor_mod(weekday_of(jan1) - rules.first_weekday, 7);
        const auto start = (7 - rel >= rules.minimum_days) ? jan1 - rel : jan1 - rel + 7;
        const auto wd = value_or(c.weekday, rules.first_weekday);
        days = start + (c.week_of_year - 1) * 7 + floor_mod(wd - rules.first_weekday, 7);
    } else {
        auto m = value_or(c.month, 1);
        y += floor_div(m - 1, 12);
        m = floor_mod(m - 1, 12) + 1;
        if (c.day == undefined && c.weekday != undefined && c.weekday_ordinal != undefined) {
            // The n-th given weekday of the month (negative: from the end).
            const auto first = days_from_civil(y, m, 1);
            if (c.weekday_ordinal > 0) days = first + floor_mod(c.weekday - weekday_of(first), 7) + (c.weekday_ordinal - 1) * 7;
            else {
                const auto last = first + days_in_month(y, m) - 1;
                days = last - floor_mod(weekday_of(last) - c.weekday, 7) + (c.weekday_ordinal + 1) * 7;
            }
        } else days = days_from_civil(y, m, 1) + value_or(c.day, 1) - 1;
    }
    const auto seconds = static_cast<double>(value_or(c.hour, 0) * 3600 + value_or(c.minute, 0) * 60 + value_or(c.second, 0)) +
                         static_cast<double>(value_or(c.nanosecond, 0)) * 1e-9;
    return to_reference(static_cast<double>(days) * day_seconds + seconds, offset);
}

double add(double reference, const Components& delta, const Offset& offset) {
    const auto l = local(reference, offset);
    auto days = l.days;
    const auto months = value_or(delta.year, 0) * 12 + value_or(delta.quarter, 0) * 3 + value_or(delta.month, 0);
    if (months) {
        const auto ymd = civil_from_days(days);
        const auto total = ymd.y * 12 + (ymd.m - 1) + months;
        const auto y = floor_div(total, 12), m = floor_mod(total, 12) + 1;
        days = days_from_civil(y, m, std::min(ymd.d, days_in_month(y, m)));
    }
    days += value_or(delta.day, 0) + value_or(delta.weekday, 0) + 7 * (value_or(delta.week_of_year, 0) + value_or(delta.week_of_month, 0) + value_or(delta.weekday_ordinal, 0));
    auto result = (days == l.days) ? reference : to_reference(static_cast<double>(days) * day_seconds + l.seconds, offset);
    result += static_cast<double>(value_or(delta.hour, 0) * 3600 + value_or(delta.minute, 0) * 60 + value_or(delta.second, 0)) +
              static_cast<double>(value_or(delta.nanosecond, 0)) * 1e-9;
    return result;
}

Components difference(double from, double to, std::uint64_t units, const Offset& offset, const Rules& rules) {
    Components out;
    const std::int64_t sign = to >= from ? 1 : -1;
    auto current = from;
    const auto overshoots = [&](double t) { return sign > 0 ? t > to : t < to; };
    const auto step = [&](Unit unit, std::int64_t estimate) {
        const auto moved = [&](std::int64_t n) {
            Components d;
            if (unit == Unit::week_of_year || unit == Unit::week_of_month) d.day = n * 7;
            else d.set(unit, n);
            return add(current, d, offset);
        };
        auto n = estimate;
        while (n != 0 && overshoots(moved(n))) n -= sign;
        while (!overshoots(moved(n + sign))) n += sign;
        out.set(unit, n);
        current = moved(n);
    };
    const auto lf = local(current, offset), lt = local(to, offset);
    if (units & era) out.era = 0;
    if (units & year) {
        const auto a = civil_from_days(local(current, offset).days), b = civil_from_days(lt.days);
        step(Unit::year, b.y - a.y);
    }
    if (units & quarter) {
        const auto a = civil_from_days(local(current, offset).days), b = civil_from_days(lt.days);
        step(Unit::quarter, ((b.y - a.y) * 12 + (b.m - a.m)) / 3);
    }
    if (units & month) {
        const auto a = civil_from_days(local(current, offset).days), b = civil_from_days(lt.days);
        step(Unit::month, (b.y - a.y) * 12 + (b.m - a.m));
    }
    for (const auto week : {Unit::week_of_year, Unit::week_of_month})
        if (units & week) step(week, (lt.days - local(current, offset).days) / 7);
    if (units & day) step(Unit::day, lt.days - local(current, offset).days);
    static_cast<void>(lf);
    static_cast<void>(rules);
    auto rest = to - current;
    if (units & hour) { const auto n = static_cast<std::int64_t>(std::trunc(rest / 3600 + (sign > 0 ? 1e-9 : -1e-9))); out.hour = n; rest -= static_cast<double>(n) * 3600; }
    if (units & minute) { const auto n = static_cast<std::int64_t>(std::trunc(rest / 60 + (sign > 0 ? 1e-9 : -1e-9))); out.minute = n; rest -= static_cast<double>(n) * 60; }
    if (units & second) { const auto n = static_cast<std::int64_t>(std::trunc(rest + (sign > 0 ? 1e-9 : -1e-9))); out.second = n; rest -= static_cast<double>(n); }
    if (units & nanosecond) out.nanosecond = static_cast<std::int64_t>(std::llround(rest * 1e9));
    return out;
}

std::optional<std::pair<std::int64_t, std::int64_t>> range(Unit smaller, Unit larger, double reference, const Offset& offset, const Rules& rules) {
    const auto l = local(reference, offset);
    const auto ymd = civil_from_days(l.days);
    if (smaller == Unit::day && larger == Unit::month) return std::pair{std::int64_t{1}, days_in_month(ymd.y, ymd.m)};
    if (smaller == Unit::day && larger == Unit::year) return std::pair{std::int64_t{1}, std::int64_t{leap(ymd.y) ? 366 : 365}};
    if (smaller == Unit::month && larger == Unit::year) return std::pair{std::int64_t{1}, std::int64_t{12}};
    if (smaller == Unit::hour && larger == Unit::day) return std::pair{std::int64_t{0}, std::int64_t{24}};
    if (smaller == Unit::minute && larger == Unit::hour) return std::pair{std::int64_t{0}, std::int64_t{60}};
    if (smaller == Unit::second && larger == Unit::minute) return std::pair{std::int64_t{0}, std::int64_t{60}};
    if (smaller == Unit::weekday && (larger == Unit::week_of_year || larger == Unit::week_of_month)) return std::pair{std::int64_t{1}, std::int64_t{7}};
    if (smaller == Unit::week_of_year && larger == Unit::year) {
        // Weeks of the year: the week of the last day that still belongs to it.
        auto last = days_from_civil(ymd.y, 12, 31);
        auto [w, y] = week_number_of_year(last, rules);
        while (y != ymd.y) { --last; std::tie(w, y) = week_number_of_year(last, rules); }
        return std::pair{std::int64_t{1}, w};
    }
    return std::nullopt;
}

std::optional<std::int64_t> ordinality(Unit smaller, Unit larger, double reference, const Offset& offset, const Rules& rules) {
    const auto l = local(reference, offset);
    const auto ymd = civil_from_days(l.days);
    if (smaller == Unit::day && larger == Unit::month) return ymd.d;
    if (smaller == Unit::day && larger == Unit::year) return l.days - days_from_civil(ymd.y, 1, 1) + 1;
    if (smaller == Unit::month && larger == Unit::year) return ymd.m;
    if (smaller == Unit::hour && larger == Unit::day) return static_cast<std::int64_t>(l.seconds) / 3600 + 1;
    if (smaller == Unit::minute && larger == Unit::hour) return static_cast<std::int64_t>(l.seconds) / 60 % 60 + 1;
    if (smaller == Unit::weekday && (larger == Unit::week_of_year || larger == Unit::week_of_month)) return floor_mod(weekday_of(l.days) - rules.first_weekday, 7) + 1;
    if (smaller == Unit::week_of_year && larger == Unit::year) return week_number_of_year(l.days, rules).first;
    if (smaller == Unit::week_of_month && larger == Unit::month) return week_number_of_month(l.days, rules);
    if (smaller == Unit::day && (larger == Unit::week_of_year || larger == Unit::week_of_month)) return floor_mod(weekday_of(l.days) - rules.first_weekday, 7) + 1;
    return std::nullopt;
}

double start_of_day(double reference, const Offset& offset) {
    const auto l = local(reference, offset);
    return to_reference(static_cast<double>(l.days) * day_seconds, offset);
}

} // namespace alchemy::gui::completion_kit::calendar
