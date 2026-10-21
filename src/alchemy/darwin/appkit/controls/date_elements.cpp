#include "alchemy/darwin/appkit/controls/date_elements.hpp"

#include <algorithm>

// NSDatePicker element formatting and field editing (date_elements.hpp).
namespace alchemy::gui::completion_kit::controls::date_elements {
namespace {
std::int64_t wrap(std::int64_t value, std::int64_t low, std::int64_t high) {
    const auto span = high - low + 1;
    return ((value - low) % span + span) % span + low;
}
void append(Formatted& out, std::u16string_view text) { out.text.append(text); }
void append_field(Formatted& out, Field::Kind kind, std::u16string_view text) {
    out.fields.push_back({kind, out.text.size(), text.size()});
    out.text.append(text);
}
std::u16string number(std::int64_t value, std::size_t digits) {
    std::u16string text;
    for (const auto c : std::to_string(value)) text.push_back(static_cast<char16_t>(c));
    while (text.size() < digits) text.insert(text.begin(), u'0');
    return text;
}
// The valid range of a field.
std::pair<std::int64_t, std::int64_t> range_of(const Civil& value, Field::Kind field) {
    switch (field) {
    case Field::month: return {1, 12};
    case Field::day: return {1, days_in_month(value.year, value.month)};
    case Field::year: return {1, 9999};
    case Field::hour: return {1, 12};   // as shown (12-hour clock)
    case Field::minute: case Field::second: return {0, 59};
    case Field::am_pm: return {0, 1};
    }
    return {0, 0};
}
} // namespace

std::int64_t days_in_month(std::int64_t year, std::int64_t month) {
    static constexpr std::int64_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

Formatted format(std::uint64_t elements, const Civil& value) {
    Formatted out;
    const bool has_date = (elements & year_month) == year_month;
    const bool has_day = (elements & year_month_day) == year_month_day;
    const bool has_time = (elements & hour_minute) == hour_minute;
    const bool has_seconds = (elements & hour_minute_second) == hour_minute_second;
    if (has_date) {
        append_field(out, Field::month, number(value.month, 1));
        append(out, u"/");
        if (has_day) {
            append_field(out, Field::day, number(value.day, 1));
            append(out, u"/");
        }
        append_field(out, Field::year, number(value.year, 4));
    }
    if (has_time) {
        if (has_date) append(out, u", ");
        const auto hour12 = value.hour % 12 == 0 ? 12 : value.hour % 12;
        append_field(out, Field::hour, number(hour12, 1));
        append(out, u":");
        append_field(out, Field::minute, number(value.minute, 2));
        if (has_seconds) {
            append(out, u":");
            append_field(out, Field::second, number(value.second, 2));
        }
        append(out, u" ");
        append_field(out, Field::am_pm, value.hour < 12 ? u"AM" : u"PM");
    }
    return out;
}

std::int64_t field_at(const Formatted& formatted, std::size_t index) {
    if (formatted.fields.empty()) return -1;
    std::int64_t found = 0;
    for (std::size_t i = 0; i < formatted.fields.size(); ++i) {
        const auto& field = formatted.fields[i];
        if (index >= field.start && index < field.start + field.length) return static_cast<std::int64_t>(i);
        if (field.start <= index) found = static_cast<std::int64_t>(i);
    }
    return found;
}

Civil step(Civil value, Field::Kind field, std::int64_t delta) {
    switch (field) {
    case Field::month:
        value.month = wrap(value.month + delta, 1, 12);
        value.day = std::min(value.day, days_in_month(value.year, value.month));
        break;
    case Field::day: value.day = wrap(value.day + delta, 1, days_in_month(value.year, value.month)); break;
    case Field::year:
        value.year = std::clamp<std::int64_t>(value.year + delta, 1, 9999);
        value.day = std::min(value.day, days_in_month(value.year, value.month));
        break;
    case Field::hour: value.hour = wrap(value.hour + delta, 0, 23); break;
    case Field::minute: value.minute = wrap(value.minute + delta, 0, 59); break;
    case Field::second: value.second = wrap(value.second + delta, 0, 59); break;
    case Field::am_pm: value.hour = (value.hour + 12) % 24; break;
    }
    return value;
}

Typed type_digit(Civil value, Field::Kind field, std::int64_t pending, int digit) {
    Typed out{value, 0, true};
    if (field == Field::am_pm || digit < 0 || digit > 9) return out;
    const auto [low, high] = range_of(value, field);
    auto candidate = pending * 10 + digit;
    if (pending == 0 || candidate > high) candidate = digit;   // start over with this digit
    const auto apply = [&](std::int64_t shown) {
        switch (field) {
        case Field::month: out.value.month = shown; out.value.day = std::min(out.value.day, days_in_month(out.value.year, shown)); break;
        case Field::day: out.value.day = shown; break;
        case Field::year: out.value.year = shown; break;
        case Field::hour: out.value.hour = (value.hour >= 12 ? 12 : 0) + shown % 12; break;
        case Field::minute: out.value.minute = shown; break;
        case Field::second: out.value.second = shown; break;
        case Field::am_pm: break;
        }
    };
    if (candidate >= low) apply(candidate);
    // Another digit can follow while candidate * 10 stays in range.
    const bool more = candidate * 10 <= high && (field != Field::year || candidate < 1000);
    out.pending = more ? std::max<std::int64_t>(candidate, 0) : 0;
    out.complete = !more;
    if (more && candidate == 0) out.pending = 0;   // a leading zero waits for the next digit
    return out;
}

} // namespace alchemy::gui::completion_kit::controls::date_elements
