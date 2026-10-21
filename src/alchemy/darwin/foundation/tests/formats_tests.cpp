// Host-only tests of Foundation's format cores: NSNumberFormatter's and
// NSDateFormatter's formatting and parsing (number_format.cpp,
// date_format.cpp) and JSON (json.cpp). The cookie jar's tests are
// darwin/network/tests/cookie_tests.cpp.
#include "alchemy/darwin/foundation/number_format.hpp"
#include "alchemy/darwin/foundation/date_format.hpp"
#include "alchemy/darwin/foundation/json.hpp"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
namespace numbers = alchemy::gui::completion_kit::numbers;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
std::string narrow(const std::u16string& value) {
    std::string out;
    for (const auto c : value) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}
void expect(const numbers::Format& format, double value, const std::u16string& wanted, const std::string& what) {
    const auto got = numbers::format(format, value);
    require(got == wanted, what + ": got \"" + narrow(got) + "\", wanted \"" + narrow(wanted) + "\"");
}

void styles() {
    numbers::Format f;
    f.apply_style(numbers::Style::none, u"$");
    expect(f, 0, u"0", "none 0");
    expect(f, 1234.5, u"1234", "none rounds half-even to an integer (1234.5 -> 1234)");
    expect(f, 1235.5, u"1236", "none half-even (1235.5 -> 1236)");
    expect(f, -7, u"-7", "none negative");
    f.apply_style(numbers::Style::decimal, u"$");
    expect(f, 1234567.891, u"1,234,567.891", "decimal grouping and three fraction digits");
    expect(f, 0.5, u"0.5", "decimal trims trailing zeros");
    expect(f, -0.0004, u"0", "decimal: a value that rounds to zero has no sign");
    f.apply_style(numbers::Style::percent, u"$");
    expect(f, 0.256, u"26%", "percent multiplies and rounds");
    f.apply_style(numbers::Style::currency, u"$");
    expect(f, 1234.5, u"$1,234.50", "currency");
    expect(f, -3, u"-$3.00", "negative currency");
}

void digits_and_rounding() {
    numbers::Format f;
    f.apply_style(numbers::Style::decimal, u"$");
    f.min_fraction = 2; f.max_fraction = 2;
    expect(f, 2.675, u"2.68", "half-even at the shortest decimal (2.675)");
    expect(f, 2.665, u"2.66", "half-even keeps an even digit (2.665)");
    f.rounding = numbers::Rounding::down;
    expect(f, 9.999, u"9.99", "down");
    f.rounding = numbers::Rounding::up;
    expect(f, 9.991, u"10.00", "up carries into the integer");
    f.rounding = numbers::Rounding::ceiling;
    expect(f, -1.001, u"-1.00", "ceiling of a negative value");
    f.rounding = numbers::Rounding::floor;
    expect(f, -1.001, u"-1.01", "floor of a negative value");
    f.rounding = numbers::Rounding::half_even;
    f.min_integer = 3;
    f.grouping = false;
    expect(f, 7.5, u"007.50", "minimum integer digits");
    f.format_width = 9; f.padding = u"*";
    expect(f, 7.5, u"***007.50", "padding before the prefix");
    f.format_width = 0;
    f.zero_symbol = u"zero";
    expect(f, 0.001, u"zero", "zero symbol after rounding");
    f.decimal_separator = u",";
    f.grouping_separator = u".";
    f.zero_symbol.reset();
    f.min_integer = 1;
    f.grouping = true;
    expect(f, 12345.25, u"12.345,25", "locale separators");
}

void parsing() {
    numbers::Format f;
    f.apply_style(numbers::Style::decimal, u"$");
    require(numbers::parse(f, u"1,234.5") == 1234.5, "parse grouping and decimal");
    require(numbers::parse(f, u" -42 ") == -42.0, "parse a negative value with spaces");
    require(!numbers::parse(f, u"12a"), "garbage does not parse");
    require(!numbers::parse(f, u""), "empty does not parse");
    require(!numbers::parse(f, u"1.2.3"), "two decimal separators do not parse");
    f.allows_floats = false;
    require(!numbers::parse(f, u"1.5"), "no floats");
    f.allows_floats = true;
    f.apply_style(numbers::Style::percent, u"$");
    require(numbers::parse(f, u"50%") == 0.5, "parse a percentage");
    f.apply_style(numbers::Style::currency, u"$");
    require(numbers::parse(f, u"-$3.25") == -3.25, "parse negative currency");
}

namespace dates = alchemy::gui::completion_kit::dates;

void date_formats() {
    // 2026-10-13 14:05:09.25 UTC; reference seconds since 2001-01-01.
    dates::Civil when{2026, 10, 13, 14, 5, 9, 0.25, 0, 0};
    const double reference = dates::reference_seconds(when, 0);
    const auto back = dates::civil(reference, 0);
    require(back.year == 2026 && back.month == 10 && back.day == 13 && back.hour == 14 && back.minute == 5 && back.second == 9,
            "civil round trip");
    require(back.weekday == 3, "2026-10-13 is a Tuesday");
    require(dates::civil(0, 0).year == 2001 && dates::civil(0, 0).weekday == 2, "the reference date is Monday 2001-01-01");
    require(dates::civil(-1, 0).year == 2000 && dates::civil(-1, 0).day == 31 && dates::civil(-1, 0).hour == 23, "before the reference date");
    require(dates::civil(reference, 3 * 3600).hour == 17, "offset applies");
    const auto f = [&](const char16_t* pattern, std::int64_t offset = 0) { return dates::format(pattern, reference, offset); };
    require(f(u"yyyy-MM-dd'T'HH:mm:ss.SSSZZZZZ") == u"2026-10-13T14:05:09.250Z", "ISO pattern");
    require(f(u"EEEE, MMMM d, y 'at' h:mm a") == u"Tuesday, October 13, 2026 at 2:05 PM", "names and 12-hour clock");
    require(f(u"EEE MMM dd yy HH:mm Z", -5 * 3600) == u"Tue Oct 13 26 09:05 -0500", "offset and abbreviations");
    require(f(u"''h'' 'o''clock'") == u"'2' o'clock", "quotes");
    const auto styled = [&](dates::Style d, dates::Style t) {
        const auto pattern = dates::style_pattern(d, t);
        return dates::format(std::u16string(pattern.begin(), pattern.end()), reference, 0);
    };
    require(styled(dates::Style::short_, dates::Style::short_) == u"10/13/26, 2:05 PM", "short styles");
    require(styled(dates::Style::medium, dates::Style::none) == u"Oct 13, 2026", "medium date");
    require(styled(dates::Style::long_, dates::Style::medium) == u"October 13, 2026 at 2:05:09 PM", "long date, medium time");
    require(styled(dates::Style::none, dates::Style::long_) == u"2:05:09 PM GMT", "long time");
    bool threw = false;
    try { static_cast<void>(dates::format(u"QQQ", reference, 0)); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "unsupported pattern letters fail");
    // Templates (TR35 skeletons) in en_US, as macOS gives them (Chrome's renderer asks for "yyyyMMdd" and others).
    const auto t = [](const char16_t* skeleton) { return dates::pattern_from_template(skeleton); };
    const auto narrow16 = [](const std::u16string& text) { return std::string(text.begin(), text.end()); };
    for (const auto& [skeleton, wanted] : std::initializer_list<std::pair<const char16_t*, const char16_t*>>{
             {u"yyyyMMdd", u"MM/dd/yyyy"}, {u"yMd", u"M/d/y"}, {u"yyyyMMMM", u"MMMM yyyy"}, {u"yyyyMMM", u"MMM yyyy"},
             {u"yMMMd", u"MMM d, y"}, {u"yMMMEd", u"EEE, MMM d, y"}, {u"MMMMEEEEd", u"EEEE, MMMM d"}, {u"Md", u"M/d"},
             {u"jmm", u"h:mm a"}, {u"jjmmss", u"hh:mm:ss a"}, {u"Hms", u"HH:mm:ss"}, {u"Hm", u"HH:mm"}, {u"ms", u"mm:ss"},
             {u"yMdjm", u"M/d/y, h:mm a"}, {u"jmsSSS", u"h:mm:ss.SSS a"}, {u"jmz", u"h:mm a z"}, {u"y", u"y"}, {u"MMM", u"LLL"},
             {u"E", u"ccc"}, {u"Ed", u"d EEE"}, {u"yG", u"y G"}}) {
        require(t(skeleton) == wanted, "template " + narrow16(skeleton) + ": got '" + narrow16(t(skeleton)) + "'");
    }
    require(dates::format(t(u"yMMMEd"), reference, 0) == u"Tue, Oct 13, 2026", "a template's pattern formats");
    threw = false;
    try { static_cast<void>(t(u"yQQQ")); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "unsupported template letters fail");
    // Parsing.
    require(dates::parse(u"yyyy-MM-dd HH:mm:ss", u"2026-10-13 14:05:09", 0) == std::floor(reference), "parse ISO-like");
    require(dates::parse(u"MMM d, y h:mm a", u"Oct 13, 2026 2:05 PM", 0) == std::floor(reference) - 9, "parse names and PM");
    require(dates::parse(u"yyyy-MM-dd'T'HH:mm:ssZZZZZ", u"2026-10-13T16:05:09+02:00", 0) == std::floor(reference), "parse an offset");
    require(!dates::parse(u"yyyy-MM-dd", u"2026-13-01", 0), "a bad month does not parse");
    require(!dates::parse(u"yyyy-MM-dd", u"2026-10-13x", 0), "trailing text does not parse");
}

namespace json = alchemy::gui::completion_kit::json;

void json_documents() {
    const auto parsed = json::parse(R"({"name": "QLab \u00e9\ud83d\ude00", "n": [1, -2, 3.5e2, true, false, null], "o": {}, "big": 9007199254740993})", false);
    require(std::holds_alternative<json::Value>(parsed), "parse a document");
    const auto& root = std::get<json::Value>(parsed);
    const auto& object = *std::get<std::shared_ptr<json::Object>>(root.data);
    require(object.size() == 4 && object[0].first == u"name", "object keys in order");
    require(std::get<std::u16string>(object[0].second.data) == u"QLab \u00e9\U0001F600", "escapes and surrogates");
    const auto& array = *std::get<std::shared_ptr<json::Array>>(object[1].second.data);
    require(std::get<std::int64_t>(array[1].data) == -2 && std::get<double>(array[2].data) == 350.0 && std::get<bool>(array[3].data), "numbers and literals");
    require(std::get<std::int64_t>(object[3].second.data) == 9007199254740993LL, "integers stay exact");
    const auto compact = json::write(root, {false, false, true});
    require(compact && *compact == "{\"name\":\"QLab \xC3\xA9\xF0\x9F\x98\x80\",\"n\":[1,-2,350,true,false,null],\"o\":{},\"big\":9007199254740993}", "compact writing");
    json::Value small{std::make_shared<json::Object>(json::Object{{u"b", json::Value{std::int64_t{1}}}, {u"a", json::Value{std::u16string(u"x/y")}}})};
    const auto pretty = json::write(small, {true, true, true});
    require(pretty && *pretty == "{\n  \"a\" : \"x\\/y\",\n  \"b\" : 1\n}", "pretty, sorted, escaped slash");
    const auto unescaped = json::write(small, {false, true, false});
    require(unescaped && *unescaped == "{\"a\":\"x/y\",\"b\":1}", "unescaped slash");
    require(!json::write(json::Value{std::nan("")}, {}), "NaN is rejected");
    for (const auto* bad : {"[1,]", "{\"a\" 1}", "[\"\x01\"]", "01", "[1] x", "\"fragment\"", "[\"\xC3\"]", "{\"a\":tru}"}) {
        const auto result = json::parse(bad, false);
        require(std::holds_alternative<json::ParseError>(result), std::string("malformed JSON rejected: ") + bad);
    }
    require(std::holds_alternative<json::Value>(json::parse("\"fragment\"", true)), "fragments when allowed");
}

} // namespace

int main() {
    try {
        styles();
        digits_and_rounding();
        parsing();
        date_formats();
        json_documents();
    } catch (const std::exception& error) {
        std::cerr << "alchemy_gui_foundation_support_unit: " << error.what() << '\n';
        return 1;
    }
    std::cout << "alchemy_gui_foundation_support_unit: passed\n";
    return 0;
}
