#include "alchemy/darwin/foundation/byte_count.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace alchemy::gui::completion_kit::byte_count {
namespace {

std::u16string widen(const std::string& s) { return std::u16string(s.begin(), s.end()); }
std::string grouped(std::uint64_t v) {
    auto digits = std::to_string(v);
    for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
    return digits;
}

} // namespace

std::u16string format(std::int64_t count, const Options& o) {
    static const char* names[] = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    const double base = (o.style == Style::memory || o.style == Style::binary) ? 1024.0 : 1000.0;
    const auto allowed_mask = o.allowed == 0 ? std::uint64_t{all} : o.allowed;
    const auto allowed = [&](int unit) { return unit == 8 ? (allowed_mask & yb_or_higher) != 0 : (allowed_mask & (std::uint64_t{1} << unit)) != 0; };
    const bool negative = count < 0;
    const double magnitude = std::fabs(static_cast<double>(count));

    std::u16string actual;
    if (o.includes_actual) actual = u" (" + widen((negative ? "-" : "") + grouped(static_cast<std::uint64_t>(magnitude))) + u" bytes)";
    if (count == 0 && o.nonnumeric && o.includes_count && o.includes_unit) {
        int unit = allowed(1) || !allowed(0) ? 1 : 0;
        if (!allowed(unit)) for (int u = 0; u <= 8; ++u) if (allowed(u)) { unit = u; break; }
        return widen(std::string("Zero ") + (unit == 0 ? "bytes" : names[unit])) + actual;
    }
    // The largest allowed unit the count reaches (the smallest allowed one otherwise).
    int smallest = -1, unit = -1;
    for (int u = 0; u <= 8; ++u) {
        if (!allowed(u)) continue;
        if (smallest < 0) smallest = u;
        if (magnitude >= std::pow(base, u)) unit = u;
    }
    if (smallest < 0) smallest = 0;
    if (unit < 0) unit = smallest;
    const auto digits_for = [&](int u, double value) {
        if (o.adaptive) return u <= 1 ? 0 : u == 2 ? 1 : 2;
        int integer_digits = value >= 1 ? static_cast<int>(std::floor(std::log10(value))) + 1 : 1;
        return std::max(0, 3 - integer_digits);
    };
    double value = magnitude / std::pow(base, unit);
    int digits = unit == 0 ? 0 : digits_for(unit, value);
    auto rounded = std::floor(value * std::pow(10.0, digits) + 0.5) / std::pow(10.0, digits);
    // Rounding up to a whole next unit moves to it.
    if (unit < 8 && rounded >= base && allowed(unit + 1)) {
        ++unit;
        value = magnitude / std::pow(base, unit);
        digits = digits_for(unit, value);
        rounded = std::floor(value * std::pow(10.0, digits) + 0.5) / std::pow(10.0, digits);
    }
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", digits, rounded);
    std::string number = buffer;
    if (!o.zero_pads && number.find('.') != std::string::npos) {
        while (number.back() == '0') number.pop_back();
        if (number.back() == '.') number.pop_back();
    }
    // Grouping of the integer part.
    const auto dot = number.find('.');
    auto integer = number.substr(0, dot);
    const auto fraction = dot == std::string::npos ? std::string() : number.substr(dot);
    if (integer.size() > 3) integer = grouped(std::stoull(integer));
    number = (negative && rounded != 0 ? "-" : "") + integer + fraction;
    std::string name = names[unit];
    if (unit == 0) name = (rounded == 1 && !negative) || number == "1" || number == "-1" ? "byte" : "bytes";
    std::string out;
    if (o.includes_count) out = number;
    if (o.includes_unit) out += (out.empty() ? "" : " ") + name;
    return widen(out) + actual;
}

} // namespace alchemy::gui::completion_kit::byte_count
