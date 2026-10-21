#include "alchemy/darwin/foundation/date_format.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

// NSDateFormatter core; see date_format.hpp.
namespace alchemy::gui::completion_kit::dates {
namespace {

constexpr std::int64_t reference_epoch_days = 11323;   // 2001-01-01 in days since 1970-01-01

constexpr std::array<const char16_t*, 12> month_names{u"January", u"February", u"March", u"April", u"May", u"June", u"July",
                                                      u"August", u"September", u"October", u"November", u"December"};
constexpr std::array<const char16_t*, 7> weekday_names{u"Sunday", u"Monday", u"Tuesday", u"Wednesday", u"Thursday", u"Friday", u"Saturday"};

// Days since 1970-01-01 <-> civil date (proleptic Gregorian).
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2;
    const auto era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = y - era * 400;
    const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
void civil_from_days(std::int64_t z, std::int64_t& y, std::int64_t& m, std::int64_t& d) {
    z += 719468;
    const auto era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = z - era * 146097;
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

std::u16string widen(const std::string& ascii) { return std::u16string(ascii.begin(), ascii.end()); }
std::u16string number(std::int64_t value, std::size_t width) {
    auto text = std::to_string(value < 0 ? -value : value);
    while (text.size() < width) text.insert(text.begin(), '0');
    return (value < 0 ? u"-" : u"") + widen(text);
}

struct Token { char16_t letter{}; std::size_t count{}; std::u16string literal; };

std::vector<Token> tokens(const std::u16string& pattern) {
    std::vector<Token> out;
    for (std::size_t at = 0; at < pattern.size();) {
        const auto c = pattern[at];
        if (c == u'\'') {
            // '' is a quote; 'text' is literal (with '' inside meaning a quote).
            if (at + 1 < pattern.size() && pattern[at + 1] == u'\'') { out.push_back({0, 0, u"'"}); at += 2; continue; }
            std::u16string literal;
            ++at;
            for (;;) {
                if (at >= pattern.size()) throw std::runtime_error("date pattern: unterminated quote");
                if (pattern[at] == u'\'') {
                    if (at + 1 < pattern.size() && pattern[at + 1] == u'\'') { literal.push_back(u'\''); at += 2; continue; }
                    ++at;
                    break;
                }
                literal.push_back(pattern[at++]);
            }
            out.push_back({0, 0, literal});
            continue;
        }
        if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')) {
            std::size_t count = 0;
            while (at < pattern.size() && pattern[at] == c) { ++count; ++at; }
            out.push_back({c, count, {}});
            continue;
        }
        out.push_back({0, 0, std::u16string(1, c)});
        ++at;
    }
    return out;
}

std::u16string offset_text(std::int64_t offset, char16_t letter, std::size_t count) {
    const auto sign = offset < 0 ? u"-" : u"+";
    const auto magnitude = offset < 0 ? -offset : offset;
    const auto hours = magnitude / 3600, minutes = magnitude / 60 % 60;
    if (letter == u'z' || (letter == u'Z' && count == 4)) {
        if (!offset) return u"GMT";
        return u"GMT" + std::u16string(sign) + number(hours, 1) + (minutes ? u":" + number(minutes, 2) : u"");
    }
    if ((letter == u'Z' && count == 5) || ((letter == u'X' || letter == u'x') && count >= 3)) {
        if (!offset && letter != u'x') return u"Z";
        return std::u16string(sign) + number(hours, 2) + u":" + number(minutes, 2);
    }
    if ((letter == u'X') && !offset) return u"Z";
    return std::u16string(sign) + number(hours, 2) + number(minutes, 2);
}

[[noreturn]] void unsupported_letter(char16_t letter) {
    throw std::runtime_error(std::string("date pattern letter '") + static_cast<char>(letter) + "' is not supported");
}

} // namespace

Civil civil(double reference, std::int64_t offset) {
    const double local = reference + static_cast<double>(offset);
    const double whole = std::floor(local);
    Civil c;
    c.fraction = local - whole;
    const auto seconds = static_cast<std::int64_t>(whole);
    auto days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) { rest += 86400; --days; }
    const auto epoch_days = days + reference_epoch_days;
    civil_from_days(epoch_days, c.year, c.month, c.day);
    c.hour = rest / 3600; c.minute = rest / 60 % 60; c.second = rest % 60;
    c.weekday = ((epoch_days % 7) + 7 + 4) % 7 + 1;   // 1970-01-01 was a Thursday
    c.day_of_year = epoch_days - days_from_civil(c.year, 1, 1) + 1;
    return c;
}

double reference_seconds(const Civil& c, std::int64_t offset) {
    const auto days = days_from_civil(c.year, c.month, c.day) - reference_epoch_days;
    return static_cast<double>(days * 86400 + c.hour * 3600 + c.minute * 60 + c.second - offset) + c.fraction;
}

std::string style_pattern(Style date, Style time) {
    std::string d, t;
    switch (date) {
    case Style::none: break;
    case Style::short_: d = "M/d/yy"; break;
    case Style::medium: d = "MMM d, y"; break;
    case Style::long_: d = "MMMM d, y"; break;
    case Style::full: d = "EEEE, MMMM d, y"; break;
    }
    switch (time) {
    case Style::none: break;
    case Style::short_: t = "h:mm a"; break;
    case Style::medium: t = "h:mm:ss a"; break;
    case Style::long_: t = "h:mm:ss a z"; break;
    case Style::full: t = "h:mm:ss a zzzz"; break;
    }
    if (d.empty()) return t;
    if (t.empty()) return d;
    return d + (date == Style::long_ || date == Style::full ? " 'at' " : ", ") + t;
}

std::u16string pattern_from_template(const std::u16string& skeleton) {
    std::size_t era = 0, year = 0, month = 0, day = 0, weekday = 0, hour = 0, minute = 0, second = 0, fraction = 0, zone = 0;
    bool twelve = true;   // en_US: j, J and C are the 12-hour clock
    for (const auto letter : skeleton) {
        switch (letter) {
        case u'G': ++era; break;
        case u'y': case u'Y': case u'u': ++year; break;
        case u'M': case u'L': ++month; break;
        case u'd': ++day; break;
        case u'E': case u'c': case u'e': ++weekday; break;
        case u'j': case u'J': case u'C': case u'h': case u'K': ++hour; break;
        case u'H': case u'k': ++hour; twelve = false; break;
        case u'm': ++minute; break;
        case u's': ++second; break;
        case u'S': ++fraction; break;
        case u'a': case u'b': case u'B': break;   // the period follows the hour field
        case u'z': case u'Z': case u'O': case u'v': case u'V': case u'X': case u'x': ++zone; break;
        default:
            if ((letter >= u'a' && letter <= u'z') || (letter >= u'A' && letter <= u'Z'))
                throw std::runtime_error("date format template letter '" + std::string(1, static_cast<char>(letter)) + "'");
        }
    }
    const auto run = [](char16_t letter, std::size_t count) { return std::u16string(count, letter); };
    const auto weekday_text = run(u'E', weekday >= 4 ? weekday : 3);
    const auto year_text = run(u'y', year == 2 ? 2 : year >= 3 ? year : 1);
    const auto day_text = run(u'd', std::min<std::size_t>(day, 2));
    std::u16string date;
    if (month >= 3) {   // month names: "MMM d, y"
        const auto month_text = run(u'M', std::min<std::size_t>(month, 5));
        if (day && year) date = month_text + u" " + day_text + u", " + year_text;
        else if (day) date = month_text + u" " + day_text;
        else if (year) date = month_text + u" " + year_text;
        else date = run(u'L', std::min<std::size_t>(month, 5));
    } else if (month) {   // numeric: "M/d/y"
        const auto month_text = run(u'M', month);
        if (day && year) date = month_text + u"/" + day_text + u"/" + year_text;
        else if (day) date = month_text + u"/" + day_text;
        else if (year) date = month_text + u"/" + year_text;
        else date = run(u'L', month);
    } else if (day) {
        date = year ? day_text + u" " + year_text : day_text;
    } else if (year) {
        date = year_text;
    }
    if (weekday) date = date.empty() ? run(u'c', weekday >= 4 ? weekday : 3) : (month ? weekday_text + u", " + date : date + u" " + weekday_text);
    if (era && !date.empty()) date += u" " + run(u'G', std::min<std::size_t>(era, 5));
    std::u16string time;
    if (hour) {
        time = run(twelve ? u'h' : u'H', twelve ? std::min<std::size_t>(hour, 2) : 2);
        if (minute) time += u":mm";
        if (minute && second) time += u":ss";
    } else if (minute) {
        time = second ? u"mm:ss" : u"mm";
    } else if (second) {
        time = u"s";
    }
    if (second && fraction) time += u"." + run(u'S', fraction);
    if (hour && twelve) time += u" a";
    if (zone && !time.empty()) time += u" " + run(u'z', zone >= 4 ? 4 : 1);
    if (date.empty()) return time;
    if (time.empty()) return date;
    return date + u", " + time;
}

std::u16string format(const std::u16string& pattern, double reference, std::int64_t offset) {
    const auto c = civil(reference, offset);
    std::u16string out;
    for (const auto& token : tokens(pattern)) {
        if (!token.letter) { out += token.literal; continue; }
        const auto n = token.count;
        switch (token.letter) {
        case u'G': out += n == 4 ? (c.year > 0 ? u"Anno Domini" : u"Before Christ") : (c.year > 0 ? u"AD" : u"BC"); break;
        case u'y': case u'u': out += n == 2 ? number(c.year % 100, 2) : number(c.year, n); break;
        case u'M': case u'L':
            if (n <= 2) out += number(c.month, n);
            else if (n == 3) out += std::u16string(month_names[static_cast<std::size_t>(c.month - 1)]).substr(0, 3);
            else if (n == 4) out += month_names[static_cast<std::size_t>(c.month - 1)];
            else out += std::u16string(month_names[static_cast<std::size_t>(c.month - 1)]).substr(0, 1);
            break;
        case u'd': out += number(c.day, n); break;
        case u'D': out += number(c.day_of_year, n); break;
        case u'E': case u'c': case u'e': {
            const std::u16string name = weekday_names[static_cast<std::size_t>(c.weekday - 1)];
            if ((token.letter != u'E') && n <= 2) out += number(c.weekday, n);
            else if (n <= 3) out += name.substr(0, 3);
            else if (n == 4) out += name;
            else out += name.substr(0, n == 5 ? 1 : 2);
            break;
        }
        case u'a': out += c.hour < 12 ? u"AM" : u"PM"; break;
        case u'h': out += number(c.hour % 12 == 0 ? 12 : c.hour % 12, n); break;
        case u'K': out += number(c.hour % 12, n); break;
        case u'H': out += number(c.hour, n); break;
        case u'k': out += number(c.hour == 0 ? 24 : c.hour, n); break;
        case u'm': out += number(c.minute, n); break;
        case u's': out += number(c.second, n); break;
        case u'S': {
            const auto scaled = static_cast<std::int64_t>(std::floor(c.fraction * std::pow(10.0, static_cast<double>(n))));
            out += number(scaled, n);
            break;
        }
        case u'Z': case u'z': case u'X': case u'x': case u'O': case u'v': case u'V':
            out += offset_text(offset, token.letter == u'O' || token.letter == u'v' || token.letter == u'V' ? u'z' : token.letter, n);
            break;
        default: unsupported_letter(token.letter);
        }
    }
    return out;
}

std::optional<double> parse(const std::u16string& pattern, const std::u16string& text, std::int64_t offset, std::optional<double> default_reference) {
    Civil c = default_reference ? civil(*default_reference, offset) : Civil{2000, 1, 1, 0, 0, 0, 0, 0, 0};
    std::optional<bool> pm;
    std::optional<std::int64_t> parsed_offset;
    std::size_t at = 0;
    const auto digits = [&](std::size_t minimum, std::size_t maximum) -> std::optional<std::int64_t> {
        std::size_t used = 0;
        std::int64_t value = 0;
        while (at + used < text.size() && used < maximum && text[at + used] >= u'0' && text[at + used] <= u'9') {
            value = value * 10 + (text[at + used] - u'0');
            ++used;
        }
        if (used < minimum) return std::nullopt;
        at += used;
        return value;
    };
    const auto name = [&](const auto& names, std::size_t length) -> std::optional<std::int64_t> {
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::u16string candidate = names[i];
            if (length) candidate = candidate.substr(0, length);
            if (text.compare(at, candidate.size(), candidate) == 0) { at += candidate.size(); return static_cast<std::int64_t>(i + 1); }
        }
        return std::nullopt;
    };
    for (const auto& token : tokens(pattern)) {
        if (!token.letter) {
            if (text.compare(at, token.literal.size(), token.literal) != 0) return std::nullopt;
            at += token.literal.size();
            continue;
        }
        const auto n = token.count;
        std::optional<std::int64_t> value;
        switch (token.letter) {
        case u'y': case u'u':
            value = digits(n == 2 ? 2 : 1, n == 2 ? 2 : 9);
            if (!value) return std::nullopt;
            c.year = n == 2 ? 2000 + *value : *value;
            break;
        case u'M': case u'L':
            value = n <= 2 ? digits(1, 2) : name(month_names, n == 3 ? 3 : 0);
            if (!value || *value < 1 || *value > 12) return std::nullopt;
            c.month = *value;
            break;
        case u'd': value = digits(1, 2); if (!value) return std::nullopt; c.day = *value; break;
        case u'E': if (!name(weekday_names, n <= 3 ? 3 : 0)) return std::nullopt; break;
        case u'a':
            if (text.compare(at, 2, u"AM") == 0) pm = false;
            else if (text.compare(at, 2, u"PM") == 0) pm = true;
            else return std::nullopt;
            at += 2;
            break;
        case u'h': case u'K': case u'H': case u'k': value = digits(1, 2); if (!value) return std::nullopt; c.hour = *value % (token.letter == u'k' ? 24 : 24); break;
        case u'm': value = digits(1, 2); if (!value) return std::nullopt; c.minute = *value; break;
        case u's': value = digits(1, 2); if (!value) return std::nullopt; c.second = *value; break;
        case u'S': {
            const auto start = at;
            value = digits(1, n);
            if (!value) return std::nullopt;
            c.fraction = static_cast<double>(*value) / std::pow(10.0, static_cast<double>(at - start));
            break;
        }
        case u'Z': case u'X': case u'x': case u'z': {
            if (text.compare(at, 1, u"Z") == 0 && token.letter != u'z') { ++at; parsed_offset = 0; break; }
            if (text.compare(at, 3, u"GMT") == 0) {
                at += 3;
                if (at >= text.size() || (text[at] != u'+' && text[at] != u'-')) { parsed_offset = 0; break; }
            }
            if (at >= text.size() || (text[at] != u'+' && text[at] != u'-')) return std::nullopt;
            const bool negative = text[at++] == u'-';
            const auto hours = digits(1, 2);
            if (!hours) return std::nullopt;
            if (at < text.size() && text[at] == u':') ++at;
            const auto minutes = digits(0, 2).value_or(0);
            parsed_offset = (negative ? -1 : 1) * (*hours * 3600 + minutes * 60);
            break;
        }
        default: unsupported_letter(token.letter);
        }
    }
    if (at != text.size()) return std::nullopt;
    if (pm) { c.hour %= 12; if (*pm) c.hour += 12; }
    if (c.day < 1 || c.day > 31 || c.hour > 23 || c.minute > 59 || c.second > 60) return std::nullopt;
    return reference_seconds(c, parsed_offset.value_or(offset));
}

} // namespace alchemy::gui::completion_kit::dates
