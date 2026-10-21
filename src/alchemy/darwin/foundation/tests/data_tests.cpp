// Host-only tests of Foundation's data cores: text helpers, regular
// expressions, decimal numbers, scanning, predicate formats, calendars, byte
// counts, XML and the date formatter's default date. Split out of the QLab
// data group's tests (kit/completion/data/tests/data_tests.cpp).
#include "alchemy/darwin/foundation/byte_count.hpp"
#include "alchemy/darwin/foundation/date_format.hpp"
#include "alchemy/darwin/foundation/calendar.hpp"
#include "alchemy/darwin/foundation/decimal.hpp"
#include "alchemy/darwin/foundation/predicate.hpp"
#include "alchemy/darwin/foundation/regex.hpp"
#include "alchemy/darwin/foundation/scan.hpp"
#include "alchemy/darwin/kit/completion/data/text.hpp"
#include "alchemy/darwin/foundation/xml.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
std::string narrow(std::u16string_view value) {
    std::string out;
    for (const auto c : value) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

// ---- text ----
namespace text = alchemy::gui::completion_kit::text;

void text_helpers() {
    require(text::to_upper(U'a') == U'A' && text::to_lower(U'\u00c9') == U'\u00e9' && text::to_lower(U'\u0416') == U'\u0436', "case pairs");
    require(text::to_lower(U'\u0100') == U'\u0101' && text::to_upper(U'\u013a') == U'\u0139', "Latin Extended-A pairs");
    require(text::fold(U'\u03c2') == text::fold(U'\u03a3'), "final sigma folds with sigma");
    require(text::capitalized(u"hello wORLD-foo o'neil 3d") == u"Hello World-Foo O'neil 3d", "capitalized");
    require(text::upper(u"stra\u00dfe") == u"STRASSE", "upper with sharp s");
    using namespace text;
    require(compare(u"abc", u"abd", 0) == -1 && compare(u"b", u"a", 0) == 1 && compare(u"a", u"a", 0) == 0, "literal compare");
    require(compare(u"ABC", u"abc", case_insensitive_search) == 0, "case-insensitive compare");
    require(compare(u"file10", u"file9", 0) == -1 && compare(u"file10", u"file9", numeric_search) == 1, "numeric compare");
    require(compare(u"file010", u"file10", numeric_search) == 0, "numeric leading zeros");
    require(compare(u"r\u00e9sum\u00e9", u"resume", diacritic_insensitive_search) == 0, "diacritic-insensitive compare");
    require(compare(u"A", u"a", case_insensitive_search | forced_ordering_search) == -1, "forced ordering");
    auto f = find(u"Hello hello", u"HELLO", case_insensitive_search, 0, 11);
    require(f.location == 0 && f.length == 5, "find case-insensitive");
    f = find(u"Hello hello", u"hello", backwards_search, 0, 11);
    require(f.location == 6, "find backwards");
    require(find(u"Hello", u"ello", anchored_search, 0, 5).location == -1, "anchored find fails off the start");
    require(find(u"Hello", u"", 0, 0, 5).location == -1, "empty target is not found");
    require(pad(u"abc", 7, u"12", 1) == u"abc2121" && pad(u"abcdef", 3, u"x", 0) == u"abc", "padding");
    require(percent_encode(u"a b/\u00e9", [](char32_t c) { return c < 0x80 && c != ' '; }) == u"a%20b/%C3%A9", "percent encoding");
}

// ---- regex ----
namespace rx = alchemy::gui::completion_kit::regex;

rx::Pattern compile(std::u16string_view pattern, std::uint64_t options = 0) {
    rx::Status status;
    std::string message;
    auto p = rx::Pattern::compile(pattern, options, status, message);
    require(p.has_value(), "compile /" + narrow(pattern) + "/: " + message);
    return std::move(*p);
}
rx::Status compile_status(std::u16string_view pattern, std::uint64_t options = 0) {
    rx::Status status;
    std::string message;
    static_cast<void>(rx::Pattern::compile(pattern, options, status, message));
    return status;
}
// The whole match and groups as "text|g1|g2" (unmatched groups: "-").
std::string first(std::u16string_view pattern, std::u16string_view text, std::uint64_t options = 0, std::uint64_t match_options = 0) {
    const auto p = compile(pattern, options);
    const auto m = p.search(text, {0, static_cast<std::int64_t>(text.size())}, 0, match_options);
    if (!m) return "<none>";
    std::string out;
    for (std::size_t g = 0; g < m->groups.size(); ++g) {
        if (g) out += "|";
        const auto r = m->groups[g];
        out += r.location < 0 ? "-" : narrow(text.substr(static_cast<std::size_t>(r.location), static_cast<std::size_t>(r.length)));
    }
    return out;
}
void expect(std::u16string_view pattern, std::u16string_view text, const std::string& wanted, std::uint64_t options = 0, std::uint64_t match_options = 0) {
    const auto got = first(pattern, text, options, match_options);
    require(got == wanted, "/" + narrow(pattern) + "/ on \"" + narrow(text) + "\": got \"" + got + "\", wanted \"" + wanted + "\"");
}

void regex_matching() {
    expect(u"b+", u"aabbbc", "bbb");
    expect(u"a.c", u"xabcx", "abc");
    expect(u"a.c", u"a\nc", "<none>");
    expect(u"a.c", u"a\nc", "a\nc", rx::dot_matches_line_separators);
    expect(u"(\\d+)-(\\d+)", u"tel 555-1234", "555-1234|555|1234");
    expect(u"(a|ab)(c|bcd)(d*)", u"abcd", "abcd|a|bcd|");
    expect(u"x*", u"abc", "");
    expect(u"a{2,3}", u"aaaa", "aaa");
    expect(u"a{2,3}?", u"aaaa", "aa");
    expect(u"a{2,}", u"aaaaa", "aaaaa");
    expect(u"<.+?>", u"<a><b>", "<a>");
    expect(u"^abc$", u"abc", "abc");
    expect(u"^b", u"a\nb", "<none>");
    expect(u"^b", u"a\nb", "b", rx::anchors_match_lines);
    expect(u"a$", u"a\n", "a");
    expect(u"a\\z", u"a\n", "<none>");
    expect(u"\\bfoo\\b", u"a foo b", "foo");
    expect(u"\\bfoo\\b", u"afoob", "<none>");
    expect(u"[a-c]+", u"xxabcabd", "abcab");
    expect(u"[^a-c]+", u"abcxyzabc", "xyz");
    expect(u"[\\d.]+", u"v1.25b", "1.25");
    expect(u"[[:alpha:]]+", u"12ab3", "ab");
    expect(u"[]a]+", u"x]a]x", "]a]");
    expect(u"HELLO", u"say hello", "hello", rx::case_insensitive);
    expect(u"(?i)hello", u"HeLLo", "HeLLo");
    expect(u"(?i:a)b", u"Ab AB", "Ab");
    expect(u"(\\w)\\1", u"abccd", "cc|c");
    expect(u"(?<word>\\w+) \\k<word>", u"is is it", "is is|is");
    expect(u"foo(?=bar)", u"foobaz foobar", "foo");
    expect(u"foo(?!bar)", u"foobar foobaz", "foo");
    expect(u"(?<=\\$)\\d+", u"cost $42", "42");
    expect(u"(?<!\\$)\\b\\d+", u"$42 17", "17");
    expect(u"(?>a+)b", u"aaab", "aaab");
    expect(u"(?>a+)a", u"aaaa", "<none>");
    expect(u"a++a", u"aaaa", "<none>");
    expect(u"(a*)*b", u"aaab", "aaab|aaa");
    expect(u"(a*)+$", u"b", "|");
    expect(u"\\Qa.b\\E", u"axb a.b", "a.b");
    expect(u"a.b", u"axb a.b", "a.b", rx::ignore_metacharacters);
    expect(u"\\x41\\u0042\\x{43}", u"ABC", "ABC");
    expect(u"a # comment\n b", u"ab", "ab", rx::allow_comments);
    expect(u"\\s+", u"a \t\nb", " \t\n");
    expect(u".", u"\U0001F600", "??");   // one code point, two UTF-16 units
    expect(u"^\\d", u"x1", "<none>");
    // Ranges and match options.
    const auto p = compile(u"\\d+");
    const std::u16string text = u"12 345 6789";
    const auto all = p.all(text, {0, static_cast<std::int64_t>(text.size())}, 0);
    require(all.size() == 3 && all[1].groups[0].location == 3 && all[1].groups[0].length == 3, "all matches");
    const auto inside = p.all(text, {4, 5}, 0);
    require(inside.size() == 2 && inside[0].groups[0].location == 4 && inside[0].groups[0].length == 2 && inside[1].groups[0].location == 7, "search range");
    require(!compile(u"\\d+").search(text, {0, 11}, 2, rx::anchored), "anchored at the range start");
    const auto empty = compile(u"x*").all(u"ab", {0, 2}, 0);
    require(empty.size() == 3, "empty matches advance one character");
    require(compile(u"^5").search(text, {5, 2}, 5, 0).has_value(), "^ anchors at the range start");
    require(!compile(u"^5").search(text, {5, 2}, 5, rx::without_anchoring_bounds).has_value(), "without anchoring bounds");
    require(!compile(u"\\b5").search(text, {5, 2}, 5, rx::transparent_bounds).has_value(), "transparent bounds see the 4");
    // Templates and escaping.
    require(rx::escape_pattern(u"a.b*c") == u"a\\.b\\*c", "escapedPatternForString");
    const auto swap = compile(u"(\\w+) (\\w+)");
    const std::u16string words = u"hello world";
    const auto m = swap.search(words, {0, 11}, 0, 0);
    require(m && rx::expand_template(swap, words, *m, u"$2 $1 \\$0") == u"world hello $0", "template expansion");
    // Errors.
    require(compile_status(u"(abc") == rx::Status::invalid, "unbalanced group");
    require(compile_status(u"a{3,2}") == rx::Status::invalid, "bad interval");
    require(compile_status(u"*a") == rx::Status::invalid, "dangling quantifier");
    require(compile_status(u"\\p{L}") == rx::Status::unsupported, "Unicode properties are unsupported");
    require(compile_status(u"[a-z&&[^b]]") == rx::Status::unsupported, "set operations are unsupported");
    require(compile_status(u"(?<=a+)b") == rx::Status::invalid, "unbounded look-behind");
    require(compile_status(u"\\2(a)") == rx::Status::invalid, "back reference to a missing group");
}


// ---- decimal ----
namespace dec = alchemy::gui::completion_kit::decimal;

dec::Decimal d(const char16_t* text) {
    const auto parsed = dec::parse(text);
    require(parsed.has_value(), "parse " + narrow(text));
    return *parsed;
}
std::string str(const dec::Decimal& value) { return dec::to_string(value); }

void decimal_arithmetic() {
    require(str(d(u"123.4500")) == "123.45" && str(d(u"-0.005")) == "-0.005" && str(d(u"1.5e3")) == "1500", "parse and print");
    require(str(d(u"  +12abc")) == "12", "parse a prefix");
    require(!dec::parse(u"abc"), "no number");
    dec::Decimal r;
    require(dec::add(r, d(u"0.1"), d(u"0.2")) == dec::Error::none && str(r) == "0.3", "0.1 + 0.2 is exact");
    require(dec::subtract(r, d(u"1"), d(u"1.25")) == dec::Error::none && str(r) == "-0.25", "subtract");
    require(dec::multiply(r, d(u"1.5"), d(u"-2.25")) == dec::Error::none && str(r) == "-3.375", "multiply");
    require(dec::divide(r, d(u"1"), d(u"4")) == dec::Error::none && str(r) == "0.25", "exact division");
    require(dec::divide(r, d(u"1"), d(u"3")) == dec::Error::loss_of_precision && str(r) == "0.33333333333333333333333333333333333333", "1/3 to 38 digits");
    require(dec::divide(r, d(u"2"), d(u"3")) == dec::Error::loss_of_precision && str(r) == "0.66666666666666666666666666666666666667", "2/3 rounds plain");
    require(dec::divide(r, d(u"1"), d(u"0")) == dec::Error::divide_by_zero && r.nan, "divide by zero");
    require(dec::power(r, d(u"1.1"), 3) == dec::Error::none && str(r) == "1.331", "power");
    require(dec::multiply_by_power_of_10(r, d(u"1.5"), 2) == dec::Error::none && str(r) == "150", "power of ten");
    const auto rounded = [&](const char16_t* v, int scale, dec::Rounding mode) { dec::Decimal out; dec::round(out, d(v), scale, mode); return str(out); };
    // Foundation's NSRoundingMode table.
    require(rounded(u"1.2345", 2, dec::Rounding::plain) == "1.23" && rounded(u"1.2355", 2, dec::Rounding::plain) == "1.24", "plain");
    require(rounded(u"1.2345", 2, dec::Rounding::down) == "1.23" && rounded(u"-1.2345", 2, dec::Rounding::down) == "-1.24", "down");
    require(rounded(u"1.2345", 2, dec::Rounding::up) == "1.24" && rounded(u"-1.2345", 2, dec::Rounding::up) == "-1.23", "up");
    require(rounded(u"1.2350", 2, dec::Rounding::bankers) == "1.24" && rounded(u"1.2450", 2, dec::Rounding::bankers) == "1.24", "bankers");
    require(rounded(u"-1.235", 2, dec::Rounding::plain) == "-1.24", "plain rounds half away from zero");
    require(rounded(u"1234.5", -2, dec::Rounding::plain) == "1200" && rounded(u"0.004", 2, dec::Rounding::plain) == "0", "negative scale, to zero");
    require(dec::compare(d(u"1.50"), d(u"1.5")) == 0 && dec::compare(d(u"-2"), d(u"1")) == -1 && dec::compare(d(u"10"), d(u"9.99")) == 1, "compare");
    require(dec::to_int64(d(u"-7.9")) == -7 && dec::to_double(d(u"2.5")) == 2.5, "conversions");
    require(str(dec::Decimal::from_double(0.1)) == "0.1" && str(dec::Decimal::from_parts(12345, -2, true)) == "-123.45", "from double and parts");
    const auto big = d(u"123456789012345678901234567890.123456789");   // 39 digits: rounds to 38
    require(str(big) == "123456789012345678901234567890.12345679", "precision limit");
    // NSDecimal bytes round trip.
    for (const auto* v : {u"0", u"-123.45", u"340282366920938463463374607431768211", u"1e-100"}) {
        const auto original = d(v);
        require(dec::compare(dec::unpack(dec::pack(original)), original) == 0, "pack/unpack " + narrow(v));
    }
    require(dec::unpack(dec::pack(dec::Decimal::not_a_number())).nan, "NaN bytes");
    const auto bytes = dec::pack(d(u"-1.5"));
    require(bytes[0] == 0xff && (bytes[1] & 0x0f) == 1 && (bytes[1] & 0x10) && bytes[4] == 15 && bytes[5] == 0, "NSDecimal layout");
}

// ---- NSScanner numbers ----
namespace scan = alchemy::gui::completion_kit::scan;

void scanner_numbers() {
    const auto i = scan::integer(u"-42abc", 0, INT32_MIN, INT32_MAX);
    require(i && i->first == -42 && i->second == 3, "integer");
    require(!scan::integer(u"abc", 0, INT32_MIN, INT32_MAX), "no integer");
    require(scan::integer(u"99999999999", 0, INT32_MIN, INT32_MAX)->first == INT32_MAX, "overflow saturates");
    require(scan::integer(u"-99999999999", 0, INT32_MIN, INT32_MAX)->first == INT32_MIN, "underflow saturates");
    require(scan::hex(u"0x1Fg", 0, UINT32_MAX)->first == 0x1f && scan::hex(u"ff", 0, UINT32_MAX)->second == 2, "hex");
    const auto f = scan::floating(u"-1.5e2x", 0);
    require(f && f->first == -150.0 && f->second == 6, "floating");
    require(scan::floating(u"3,25", 0, u',')->first == 3.25, "locale separator");
    require(scan::floating(u".5", 0)->first == 0.5 && !scan::floating(u".", 0), "fraction only");
    require(scan::floating(u"7e", 0)->second == 1, "a dangling exponent is not consumed");
}

// ---- NSPredicate format ----
namespace pred = alchemy::gui::completion_kit::predicate;

std::string round_trip(const char16_t* format, std::vector<pred::Expression> arguments = {}) {
    std::size_t next = 0;
    pred::Status status;
    std::string message;
    const auto node = pred::parse(format, [&](char, std::string_view) { return arguments.at(next++); }, status, message);
    if (!node) return "<" + std::string(status == pred::Status::unsupported ? "unsupported" : "invalid") + ">";
    return narrow(pred::format(*node, [](std::uint64_t h) { return u"<object " + std::u16string(1, static_cast<char16_t>(u'0' + h)) + u">"; }));
}

void predicate_format() {
    require(round_trip(u"name == 'John'") == "name == \"John\"", "simple equality");
    require(round_trip(u"age >= 21 AND age < 65") == "age >= 21 AND age < 65", "conjunction");
    require(round_trip(u"(a = 1 OR b = 2) && NOT c != 3") == "(a == 1 OR b == 2) AND NOT c != 3", "nesting and synonyms");
    require(round_trip(u"name BEGINSWITH[cd] \"j\"") == "name BEGINSWITH[cd] \"j\"", "string options");
    require(round_trip(u"ANY items.price > 10.5") == "ANY items.price > 10.5", "ANY modifier");
    require(round_trip(u"NONE tags IN {'a', 'b'}") == "NONE tags IN {\"a\", \"b\"}", "NONE and aggregates");
    require(round_trip(u"x BETWEEN {1, 5}") == "x BETWEEN {1, 5}", "BETWEEN");
    require(round_trip(u"SELF == $value") == "SELF == $value", "SELF and variables");
    require(round_trip(u"SELF.name == nil") == "name == nil", "SELF. prefix and nil");
    require(round_trip(u"flag == YES") == "flag == 1", "booleans");
    require(round_trip(u"TRUEPREDICATE") == "TRUEPREDICATE", "TRUEPREDICATE");
    require(round_trip(u"a NOT IN %@", {pred::Expression{pred::Expression::Kind::object, false, 0, 0.0, {}, 3, {}}}) == "NOT a IN <object 3>", "NOT IN and %@");
    pred::Expression key;
    key.kind = pred::Expression::Kind::key_path;
    key.text = u"title";
    pred::Expression five;
    five.kind = pred::Expression::Kind::integer;
    five.integer = 5;
    require(round_trip(u"%K.length > %d", {key, five}) == "title.length > 5", "%K and %d");
    require(round_trip(u"a ==") == "<invalid>", "missing operand");
    require(round_trip(u"a == 'x") == "<invalid>", "unterminated string");
    require(round_trip(u"SUBQUERY(items, $i, $i.x > 1).@count > 0") == "<unsupported>", "SUBQUERY is unsupported");
    require(round_trip(u"a + 1 > 2") == "<unsupported>", "arithmetic is unsupported");
    require(pred::like(u"QLab.app", u"*.app", 0) && pred::like(u"QLAB", u"ql?b", pred::case_insensitive) && !pred::like(u"QLab", u"Q*x", 0), "LIKE");
    require(pred::like(u"a*b", u"a\\*b", 0) && !pred::like(u"axb", u"a\\*b", 0), "LIKE escapes");
}

// ---- NSCalendar ----
namespace cal = alchemy::gui::completion_kit::calendar;

void calendar_arithmetic() {
    const cal::Offset utc = [](double) { return std::int64_t{0}; };
    const cal::Offset cet = [](double) { return std::int64_t{3600}; };
    const cal::Rules us{1, 1}, iso{2, 4};
    constexpr auto all = cal::era | cal::year | cal::month | cal::day | cal::hour | cal::minute | cal::second | cal::weekday | cal::quarter | cal::week_of_year | cal::year_for_week_of_year;
    // 2026-10-13 14:05:09 UTC.
    cal::Components when;
    when.year = 2026; when.month = 10; when.day = 13; when.hour = 14; when.minute = 5; when.second = 9;
    const auto t = cal::combine(when, utc, us);
    require(t == 813593109.0, "combine: 2026-10-13 14:05:09 UTC");
    const auto c = cal::split(t, all, utc, us);
    require(c.era == 1 && c.year == 2026 && c.month == 10 && c.day == 13 && c.hour == 14 && c.minute == 5 && c.second == 9, "split");
    require(c.weekday == 3 && c.quarter == 4 && c.week_of_year == 42 && c.year_for_week_of_year == 2026, "weekday, quarter, week");
    require(cal::split(t, cal::hour, cet, us).hour == 15, "time zone offset");
    require(cal::combine(when, cet, us) == t - 3600, "local components to UTC");
    require(cal::split(0, all, utc, us).weekday == 2, "2001-01-01 is a Monday");
    // ISO 8601 weeks: 2021-01-01 (a Friday) is in week 53 of 2020.
    cal::Components jan1;
    jan1.year = 2021; jan1.month = 1; jan1.day = 1;
    const auto iso_week = cal::split(cal::combine(jan1, utc, iso), cal::week_of_year | cal::year_for_week_of_year, utc, iso);
    require(iso_week.week_of_year == 53 && iso_week.year_for_week_of_year == 2020, "ISO week of 2021-01-01");
    // Lenient components and month arithmetic.
    cal::Components lenient;
    lenient.year = 2026; lenient.month = 13; lenient.day = 1;
    require(cal::split(cal::combine(lenient, utc, us), cal::year | cal::month, utc, us).year == 2027, "month 13 rolls over");
    cal::Components jan31;
    jan31.year = 2024; jan31.month = 1; jan31.day = 31;
    cal::Components one_month;
    one_month.month = 1;
    const auto feb = cal::split(cal::add(cal::combine(jan31, utc, us), one_month, utc), cal::month | cal::day, utc, us);
    require(feb.month == 2 && feb.day == 29, "Jan 31 + 1 month clamps to Feb 29 in a leap year");
    cal::Components one_day;
    one_day.day = -1;
    require(cal::split(cal::add(t, one_day, utc), cal::day, utc, us).day == 12, "minus one day");
    // Differences.
    cal::Components start;
    start.year = 2020; start.month = 2; start.day = 29;
    const auto from = cal::combine(start, utc, us);
    const auto diff = cal::difference(from, t, cal::year | cal::month | cal::day | cal::hour, utc, us);
    require(diff.year == 6 && diff.month == 7 && diff.day == 15 && diff.hour == 14, "difference y/m/d/h (Feb 29 pins to Feb 28, as ICU)");
    const auto back = cal::difference(t, from, cal::day, utc, us);
    require(back.day < 0 && cal::difference(from, t, cal::day, utc, us).day == -back.day, "negative differences mirror");
    require(cal::difference(t, t + 90.5, cal::minute | cal::second, utc, us).minute == 1 && cal::difference(t, t + 90.5, cal::minute | cal::second, utc, us).second == 30, "minutes and seconds truncate");
    require(cal::range(cal::day, cal::month, cal::combine(jan31, utc, us) + 86400 * 5, utc, us)->second == 29, "days in February 2024");
    require(*cal::ordinality(cal::day, cal::year, t, utc, us) == 286, "day of year");
    require(cal::start_of_day(t, utc) == t - (14 * 3600 + 5 * 60 + 9), "start of day");
}

// ---- NSByteCountFormatter ----
namespace bytes = alchemy::gui::completion_kit::byte_count;

void byte_counts() {
    const auto f = [](std::int64_t n, bytes::Options o = {}) { return narrow(bytes::format(n, o)); };
    require(f(0) == "Zero KB" && f(1) == "1 byte" && f(999) == "999 bytes", "small counts");
    require(f(1000) == "1 KB" && f(1500) == "2 KB" && f(999499) == "999 KB" && f(999999) == "1 MB", "kilobytes round half up and move up");
    require(f(1500000) == "1.5 MB" && f(1234567) == "1.2 MB" && f(1234567890) == "1.23 GB" && f(5000000000000) == "5 TB", "adaptive precision");
    require(f(-2048) == "-2 KB", "negative counts");
    bytes::Options memory;
    memory.style = bytes::Style::memory;
    require(f(1024, memory) == "1 KB" && f(1536 * 1024, memory) == "1.5 MB", "memory style (1024)");
    bytes::Options pads;
    pads.zero_pads = true;
    require(f(1500000000, pads) == "1.50 GB", "zero-padded fraction digits");
    bytes::Options mb_only;
    mb_only.allowed = bytes::mb;
    require(f(1500000000, mb_only) == "1,500 MB" && f(1000, mb_only) == "0 MB", "allowed units (grouping, below the unit)");
    bytes::Options plain;
    plain.nonnumeric = false;
    require(f(0, plain) == "0 bytes", "numeric zero");
    bytes::Options actual;
    actual.includes_actual = true;
    require(f(1500000, actual) == "1.5 MB (1,500,000 bytes)", "actual byte count");
    bytes::Options no_unit;
    no_unit.includes_unit = false;
    require(f(1500000, no_unit) == "1.5", "count only");
}

// ---- NSXMLDocument ----
namespace xml = alchemy::gui::completion_kit::xml;

void xml_documents() {
    const char* appcast = R"(<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>QLab &amp; friends</title>
    <item>
      <title>5.6.3</title>
      <sparkle:version>5603</sparkle:version>
      <enclosure url="https://example.invalid/QLab.zip" sparkle:version="5603" length="42"/>
    </item>
    <item><title><![CDATA[5.6.2 <beta>]]></title><enclosure url="old.zip" sparkle:version="5602"/></item>
    <!-- a comment -->
  </channel>
</rss>)";
    xml::ParseError error;
    const auto doc = xml::parse(appcast, error);
    require(doc != nullptr, "parse an appcast: " + error.message);
    require(doc->encoding == "utf-8", "declared encoding");
    auto* rss = doc->root_element();
    require(rss && rss->name == "rss" && rss->attribute("version")->value == "2.0", "root element and attribute");
    xml::Status status;
    std::string message;
    const auto items = xml::select(doc->root(), "/rss/channel/item", status, message);
    require(status == xml::Status::ok && items.size() == 2, "absolute path");
    const auto versions = xml::select(rss, "//enclosure/@sparkle:version", status, message);
    require(versions.size() == 2 && versions[0]->value == "5603" && versions[0]->uri == "http://www.andymatuschak.org/xml-namespaces/sparkle", "descendant attributes with a namespace");
    require(versions[0]->local_name() == "version" && versions[0]->prefix() == "sparkle", "local name and prefix");
    require(xml::select(rss, "channel/title", status, message).at(0)->string_value() == "QLab & friends", "entities");
    require(xml::select(rss, "channel/item[2]/title", status, message).at(0)->string_value() == "5.6.2 <beta>", "CDATA and positions");
    require(xml::select(rss, "channel/item[last()]/enclosure", status, message).at(0)->attribute("url")->value == "old.zip", "last()");
    require(xml::select(rss, "channel/item[title='5.6.3']", status, message).size() == 1, "child value predicate");
    require(xml::select(rss, "channel/item/enclosure[@length]", status, message).size() == 1, "attribute predicate");
    require(xml::select(items[0], "../title/text()", status, message).at(0)->kind == xml::Kind::text, "parent and text()");
    require(items[0]->children.size() == 3 && items[0]->next_sibling() == items[1], "whitespace dropped; siblings");
    static_cast<void>(xml::select(rss, "count(//item)", status, message));
    require(status == xml::Status::unsupported, "XPath functions are unsupported");
    require(xml::serialize(*items[1]->children[1]) == "<enclosure url=\"old.zip\" sparkle:version=\"5602\"/>", "serialize");
    require(!xml::parse("<a><b></a>", error) && error.message.find("mismatched") != std::string::npos, "mismatched tags");
    require(!xml::parse("<a>&nbsp;</a>", error), "undefined entities");
    require(!xml::parse("<p:a/>", error), "undeclared prefixes");
}

// ---- -[NSDateFormatter defaultDate] ----
void date_default() {
    namespace dates = alchemy::gui::completion_kit::dates;
    const double reference = 813593109.0;   // 2026-10-13 14:05:09 UTC
    const auto parsed = dates::parse(u"HH:mm", u"09:30", 0, reference);
    const auto c = dates::civil(*parsed, 0);
    require(parsed && c.year == 2026 && c.month == 10 && c.day == 13 && c.hour == 9 && c.minute == 30 && c.second == 9, "unnamed fields come from the default date");
    require(dates::civil(*dates::parse(u"HH:mm", u"09:30", 0), 0).year == 2000, "without a default date: 2000-01-01");
}


struct Test { const char* name; std::function<void()> run; };

} // namespace

int main() {
    const std::vector<Test> tests = {
        {"text helpers", text_helpers},
        {"regular expressions", regex_matching},
        {"decimal arithmetic", decimal_arithmetic},
        {"scanner numbers", scanner_numbers},
        {"predicate format", predicate_format},
        {"calendar arithmetic", calendar_arithmetic},
        {"byte counts", byte_counts},
        {"XML documents", xml_documents},
        {"date formatter default date", date_default},
    };
    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "ok   " << test.name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << e.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
