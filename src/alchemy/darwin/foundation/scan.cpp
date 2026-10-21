#include "alchemy/darwin/foundation/scan.hpp"

#include <cstdlib>
#include <limits>
#include <string>

namespace alchemy::gui::completion_kit::scan {

std::optional<std::pair<std::int64_t, std::size_t>> integer(std::u16string_view text, std::size_t at, std::int64_t min, std::int64_t max) {
    auto i = at;
    bool negative = false;
    if (i < text.size() && (text[i] == u'+' || text[i] == u'-')) { negative = text[i] == u'-'; ++i; }
    if (i >= text.size() || text[i] < u'0' || text[i] > u'9') return std::nullopt;
    std::uint64_t magnitude = 0;
    bool overflow = false;
    const std::uint64_t limit = negative ? static_cast<std::uint64_t>(-(min + 1)) + 1 : static_cast<std::uint64_t>(max);
    while (i < text.size() && text[i] >= u'0' && text[i] <= u'9') {
        const auto d = static_cast<std::uint64_t>(text[i] - u'0');
        if (!overflow && (magnitude > (limit - d) / 10)) overflow = true;
        if (!overflow) magnitude = magnitude * 10 + d;
        ++i;
    }
    if (overflow) return std::pair{negative ? min : max, i};
    const auto value = negative ? (magnitude == static_cast<std::uint64_t>(-(min + 1)) + 1 ? min : -static_cast<std::int64_t>(magnitude)) : static_cast<std::int64_t>(magnitude);
    return std::pair{value, i};
}

std::optional<std::pair<std::uint64_t, std::size_t>> unsigned_integer(std::u16string_view text, std::size_t at) {
    auto i = at;
    bool negative = false;
    if (i < text.size() && (text[i] == u'+' || text[i] == u'-')) { negative = text[i] == u'-'; ++i; }
    if (i >= text.size() || text[i] < u'0' || text[i] > u'9') return std::nullopt;
    std::uint64_t magnitude = 0;
    bool overflow = false;
    while (i < text.size() && text[i] >= u'0' && text[i] <= u'9') {
        const auto d = static_cast<std::uint64_t>(text[i] - u'0');
        if (!overflow && magnitude > (std::numeric_limits<std::uint64_t>::max() - d) / 10) overflow = true;
        if (!overflow) magnitude = magnitude * 10 + d;
        ++i;
    }
    if (overflow) return std::pair{std::numeric_limits<std::uint64_t>::max(), i};
    return std::pair{negative ? 0 - magnitude : magnitude, i};
}

std::optional<std::pair<std::uint64_t, std::size_t>> hex(std::u16string_view text, std::size_t at, std::uint64_t max) {
    auto i = at;
    const auto digit = [&](std::size_t k) -> int {
        if (k >= text.size()) return -1;
        const auto c = text[k];
        return c >= u'0' && c <= u'9' ? c - u'0' : c >= u'a' && c <= u'f' ? c - u'a' + 10 : c >= u'A' && c <= u'F' ? c - u'A' + 10 : -1;
    };
    if (i + 1 < text.size() && text[i] == u'0' && (text[i + 1] == u'x' || text[i + 1] == u'X') && digit(i + 2) >= 0) i += 2;
    if (digit(i) < 0) return std::nullopt;
    std::uint64_t value = 0;
    bool overflow = false;
    for (int d; (d = digit(i)) >= 0; ++i) {
        if (!overflow && value > (max - static_cast<std::uint64_t>(d)) / 16) overflow = true;
        if (!overflow) value = value * 16 + static_cast<std::uint64_t>(d);
    }
    return std::pair{overflow ? max : value, i};
}

std::optional<std::pair<double, std::size_t>> floating(std::u16string_view text, std::size_t at, char16_t separator) {
    std::string ascii;
    auto i = at;
    if (i < text.size() && (text[i] == u'+' || text[i] == u'-')) ascii.push_back(static_cast<char>(text[i++]));
    bool digits = false;
    while (i < text.size() && text[i] >= u'0' && text[i] <= u'9') { ascii.push_back(static_cast<char>(text[i++])); digits = true; }
    if (i < text.size() && text[i] == separator) {
        const auto save = i;
        std::string fraction = ".";
        ++i;
        bool more = false;
        while (i < text.size() && text[i] >= u'0' && text[i] <= u'9') { fraction.push_back(static_cast<char>(text[i++])); more = true; }
        if (more || digits) { ascii += fraction; digits = digits || more; }
        else i = save;
    }
    if (!digits) return std::nullopt;
    if (i < text.size() && (text[i] == u'e' || text[i] == u'E')) {
        auto e = i + 1;
        std::string exponent = "e";
        if (e < text.size() && (text[e] == u'+' || text[e] == u'-')) exponent.push_back(static_cast<char>(text[e++]));
        if (e < text.size() && text[e] >= u'0' && text[e] <= u'9') {
            while (e < text.size() && text[e] >= u'0' && text[e] <= u'9') exponent.push_back(static_cast<char>(text[e++]));
            ascii += exponent;
            i = e;
        }
    }
    return std::pair{std::strtod(ascii.c_str(), nullptr), i};
}

} // namespace alchemy::gui::completion_kit::scan
