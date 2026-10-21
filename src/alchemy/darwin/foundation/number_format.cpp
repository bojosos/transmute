#include "alchemy/darwin/foundation/number_format.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// NSNumberFormatter core; see number_format.hpp.
namespace alchemy::gui::completion_kit::numbers {
namespace {

std::u16string widen(const std::string& ascii) { return std::u16string(ascii.begin(), ascii.end()); }

// The value rounded to `digits` fraction digits with the rounding mode, as
// an exact decimal string "<integer>.<fraction>" (no sign).
std::string round_decimal(long double magnitude, std::int64_t digits, Rounding mode, bool negative) {
    // Enough digits to see the rounding position exactly for values QLab formats.
    char buffer[512];
    std::snprintf(buffer, sizeof buffer, "%.*Lf", static_cast<int>(digits + 6), magnitude);
    std::string text = buffer;
    const auto dot = text.find('.');
    std::string integer = text.substr(0, dot), fraction = dot == std::string::npos ? "" : text.substr(dot + 1);
    std::string kept = fraction.substr(0, static_cast<std::size_t>(digits));
    const std::string rest = fraction.substr(static_cast<std::size_t>(digits));
    const bool nonzero_rest = rest.find_first_not_of('0') != std::string::npos;
    const int first = rest.empty() ? 0 : rest[0] - '0';
    const bool exactly_half = first == 5 && rest.substr(1).find_first_not_of('0') == std::string::npos;
    const int last = kept.empty() ? (integer.empty() ? 0 : integer.back() - '0') : kept.back() - '0';
    bool increment = false;
    switch (mode) {
    case Rounding::ceiling: increment = nonzero_rest && !negative; break;
    case Rounding::floor: increment = nonzero_rest && negative; break;
    case Rounding::down: increment = false; break;
    case Rounding::up: increment = nonzero_rest; break;
    case Rounding::half_up: increment = first >= 5; break;
    case Rounding::half_down: increment = first > 5 || (first == 5 && !exactly_half); break;
    case Rounding::half_even: increment = first > 5 || (first == 5 && (!exactly_half || last % 2 == 1)); break;
    }
    std::string digits_text = integer + kept;
    if (increment) {
        int at = static_cast<int>(digits_text.size()) - 1;
        while (at >= 0 && digits_text[static_cast<std::size_t>(at)] == '9') { digits_text[static_cast<std::size_t>(at)] = '0'; --at; }
        if (at < 0) digits_text.insert(digits_text.begin(), '1');
        else ++digits_text[static_cast<std::size_t>(at)];
    }
    const auto split = digits_text.size() - kept.size();
    return digits_text.substr(0, split) + "." + digits_text.substr(split);
}

} // namespace

void Format::apply_style(Style value, const std::u16string& currency_symbol) {
    style = value;
    multiplier = 1;
    positive_prefix.clear(); positive_suffix.clear(); negative_prefix = u"-"; negative_suffix.clear();
    switch (value) {
    case Style::none: min_integer = 0; min_fraction = 0; max_fraction = 0; grouping = false; break;
    case Style::decimal: min_integer = 1; min_fraction = 0; max_fraction = 3; grouping = true; break;
    case Style::currency:
        min_integer = 1; min_fraction = 2; max_fraction = 2; grouping = true;
        positive_prefix = currency_symbol; negative_prefix = u"-" + currency_symbol;
        break;
    case Style::percent:
        min_integer = 1; min_fraction = 0; max_fraction = 0; grouping = true; multiplier = 100;
        positive_suffix = u"%"; negative_suffix = u"%";
        break;
    default: break;   // the caller rejects the other styles
    }
}

std::u16string format(const Format& f, double value) {
    if (std::isnan(value)) return u"NaN";
    if (std::isinf(value) && value > 0 && f.positive_infinity) return *f.positive_infinity;
    if (std::isinf(value) && value < 0 && f.negative_infinity) return *f.negative_infinity;
    if (std::isinf(value)) return value < 0 ? f.negative_prefix + u"∞" + f.negative_suffix : f.positive_prefix + u"∞" + f.positive_suffix;
    long double scaled = static_cast<long double>(value) * static_cast<long double>(f.multiplier);
    if (f.rounding_increment > 0) {
        // The nearest multiple of the increment under the rounding mode.
        const long double increment = f.rounding_increment;
        const auto quotient = round_decimal(std::fabs(scaled / increment), 0, f.rounding, scaled < 0);
        const auto multiple = std::strtold(quotient.c_str(), nullptr) * increment;
        scaled = scaled < 0 ? -multiple : multiple;
    }
    const bool negative_input = scaled < 0;
    const auto rounded = round_decimal(std::fabs(scaled), std::max<std::int64_t>(0, f.max_fraction), f.rounding, negative_input);
    const auto dot = rounded.find('.');
    std::string integer = rounded.substr(0, dot), fraction = rounded.substr(dot + 1);
    while (static_cast<std::int64_t>(fraction.size()) > f.min_fraction && !fraction.empty() && fraction.back() == '0') fraction.pop_back();
    integer.erase(0, std::min(integer.find_first_not_of('0'), integer.size()));
    const bool zero = integer.empty() && fraction.find_first_not_of('0') == std::string::npos;
    if (zero && f.zero_symbol) return *f.zero_symbol;
    while (static_cast<std::int64_t>(integer.size()) < f.min_integer) integer.insert(integer.begin(), '0');
    if (static_cast<std::int64_t>(integer.size()) > f.max_integer) integer = integer.substr(integer.size() - static_cast<std::size_t>(f.max_integer));
    if (integer.empty() && fraction.empty()) integer = "0";
    std::u16string body;
    const auto size = integer.size();
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char16_t>(integer[i]));
        const auto remaining = size - i - 1;
        if (f.grouping && f.grouping_size > 0 && remaining > 0 && remaining % static_cast<std::size_t>(f.grouping_size) == 0)
            body += f.grouping_separator;
    }
    if (!fraction.empty()) body += f.decimal_separator + widen(fraction);
    const bool negative = negative_input && !zero;
    const auto& prefix = negative ? f.negative_prefix : f.positive_prefix;
    const auto& suffix = negative ? f.negative_suffix : f.positive_suffix;
    std::u16string result = prefix + body + suffix;
    const auto width = static_cast<std::size_t>(std::max<std::int64_t>(0, f.format_width));
    if (result.size() < width && !f.padding.empty()) {
        std::u16string pad;
        while (pad.size() + result.size() < width) pad += f.padding.front();
        switch (f.padding_position) {
        case Padding::before_prefix: result = pad + result; break;
        case Padding::after_prefix: result = prefix + pad + body + suffix; break;
        case Padding::before_suffix: result = prefix + body + pad + suffix; break;
        case Padding::after_suffix: result += pad; break;
        }
    }
    return result;
}

std::optional<double> parse(const Format& f, const std::u16string& input) {
    std::u16string text = input;
    while (!text.empty() && (text.front() == u' ' || text.front() == u'\t')) text.erase(text.begin());
    while (!text.empty() && (text.back() == u' ' || text.back() == u'\t')) text.pop_back();
    if (f.zero_symbol && text == *f.zero_symbol) return 0.0;
    bool negative = false;
    const auto strip = [&text](const std::u16string& prefix, const std::u16string& suffix) {
        if (text.size() < prefix.size() + suffix.size()) return false;
        if (text.compare(0, prefix.size(), prefix) != 0) return false;
        if (!suffix.empty() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
        text = text.substr(prefix.size(), text.size() - prefix.size() - suffix.size());
        return true;
    };
    if (!f.negative_prefix.empty() && strip(f.negative_prefix, f.negative_suffix)) negative = true;
    else if (!strip(f.positive_prefix, f.positive_suffix)) {
        if (!text.empty() && text.front() == u'-') { negative = true; text.erase(text.begin()); }
        else if (!text.empty() && text.front() == u'+') text.erase(text.begin());
    }
    std::string digits;
    bool seen_decimal = false, seen_digit = false;
    for (std::size_t at = 0; at < text.size();) {
        const auto c = text[at];
        if (c >= u'0' && c <= u'9') { digits.push_back(static_cast<char>(c)); seen_digit = true; ++at; continue; }
        if (!f.decimal_separator.empty() && text.compare(at, f.decimal_separator.size(), f.decimal_separator) == 0) {
            if (seen_decimal || !f.allows_floats) return std::nullopt;
            seen_decimal = true;
            digits.push_back('.');
            at += f.decimal_separator.size();
            continue;
        }
        if (!f.grouping_separator.empty() && !seen_decimal && text.compare(at, f.grouping_separator.size(), f.grouping_separator) == 0) {
            at += f.grouping_separator.size();
            continue;
        }
        return std::nullopt;
    }
    if (!seen_digit) return std::nullopt;
    double value = std::stod(digits);
    if (f.multiplier != 0) value /= f.multiplier;
    return negative ? -value : value;
}

} // namespace alchemy::gui::completion_kit::numbers
