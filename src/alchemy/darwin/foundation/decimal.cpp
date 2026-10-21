#include "alchemy/darwin/foundation/decimal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace alchemy::gui::completion_kit::decimal {
namespace {

// ---- unsigned decimal digit strings ('0'..'9', most significant first) ----
void strip_leading(std::string& a) {
    std::size_t i = 0;
    while (i < a.size() && a[i] == '0') ++i;
    a.erase(0, i);
}
int compare_magnitude(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    const auto r = a.compare(b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
std::string add_magnitude(const std::string& a, const std::string& b) {
    std::string out;
    int carry = 0;
    for (std::size_t i = 0; i < std::max(a.size(), b.size()) || carry; ++i) {
        int d = carry;
        if (i < a.size()) d += a[a.size() - 1 - i] - '0';
        if (i < b.size()) d += b[b.size() - 1 - i] - '0';
        out.push_back(static_cast<char>('0' + d % 10));
        carry = d / 10;
    }
    std::reverse(out.begin(), out.end());
    strip_leading(out);
    return out;
}
// a - b with a >= b.
std::string subtract_magnitude(const std::string& a, const std::string& b) {
    std::string out;
    int borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        int d = (a[a.size() - 1 - i] - '0') - borrow - (i < b.size() ? b[b.size() - 1 - i] - '0' : 0);
        borrow = d < 0;
        if (d < 0) d += 10;
        out.push_back(static_cast<char>('0' + d));
    }
    std::reverse(out.begin(), out.end());
    strip_leading(out);
    return out;
}
std::string multiply_magnitude(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return {};
    std::vector<int> sum(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < b.size(); ++j) sum[i + j + 1] += (a[i] - '0') * (b[j] - '0');
    for (std::size_t k = sum.size(); k-- > 1;) { sum[k - 1] += sum[k] / 10; sum[k] %= 10; }
    std::string out;
    for (const auto d : sum) out.push_back(static_cast<char>('0' + d));
    strip_leading(out);
    return out;
}
// Quotient of a / b (b nonzero); `remainder_nonzero` tells whether the division was inexact.
std::string divide_magnitude(const std::string& a, const std::string& b, bool& remainder_nonzero) {
    std::string quotient, rest;
    for (const auto c : a) {
        rest.push_back(c);
        strip_leading(rest);
        int count = 0;
        while (compare_magnitude(rest, b) >= 0) { rest = subtract_magnitude(rest, b); ++count; }
        quotient.push_back(static_cast<char>('0' + count));
    }
    strip_leading(quotient);
    remainder_nonzero = !rest.empty();
    return quotient;
}
// Adds one unit in the last place.
std::string increment(const std::string& a) { return add_magnitude(a, "1"); }

// Rounds `value` so that `drop` trailing digits are removed (exponent += drop).
// `nonzero_below` marks discarded digits beyond the string (sticky bit).
void round_away(Decimal& value, std::size_t drop, Rounding mode, bool nonzero_below = false) {
    if (drop == 0) return;
    std::string kept, removed;
    if (drop >= value.digits.size()) {
        removed = std::string(drop - value.digits.size(), '0') + value.digits;
    } else {
        kept = value.digits.substr(0, value.digits.size() - drop);
        removed = value.digits.substr(value.digits.size() - drop);
    }
    const bool any = nonzero_below || removed.find_first_not_of('0') != std::string::npos;
    // removed vs half: compare to "5000...".
    int half = 0;
    if (!removed.empty()) {
        if (removed[0] > '5') half = 1;
        else if (removed[0] < '5') half = -1;
        else half = (nonzero_below || removed.find_first_not_of('0', 1) != std::string::npos) ? 1 : 0;
    } else half = -1;
    bool up = false;
    switch (mode) {
    case Rounding::plain: up = half >= 0 && any; break;
    case Rounding::bankers: up = half > 0 || (half == 0 && !kept.empty() && ((kept.back() - '0') % 2 == 1)); break;
    case Rounding::down: up = value.negative && any; break;
    case Rounding::up: up = !value.negative && any; break;
    }
    value.exponent += static_cast<int>(drop);
    value.digits = up ? increment(kept) : kept;
    strip_leading(value.digits);
}

// Brings both operands to the smaller exponent.
void align(std::string& a, int ea, std::string& b, int eb, int& exponent) {
    exponent = std::min(ea, eb);
    a.append(static_cast<std::size_t>(ea - exponent), '0');
    b.append(static_cast<std::size_t>(eb - exponent), '0');
    strip_leading(a);
    strip_leading(b);
}

} // namespace

Decimal Decimal::from_unsigned(std::uint64_t value) {
    Decimal d;
    d.digits = value ? std::to_string(value) : std::string();
    normalize(d);
    return d;
}
Decimal Decimal::from_integer(std::int64_t value) {
    Decimal d;
    if (value < 0) {
        d.negative = true;
        d.digits = std::to_string(static_cast<std::uint64_t>(-(value + 1)) + 1);
    } else if (value) d.digits = std::to_string(value);
    normalize(d);
    return d;
}
Decimal Decimal::from_parts(std::uint64_t mantissa, int exponent, bool negative) {
    Decimal d;
    d.digits = mantissa ? std::to_string(mantissa) : std::string();
    d.exponent = exponent;
    d.negative = negative && mantissa;
    normalize(d);
    return d;
}
Decimal Decimal::from_double(double value) {
    if (std::isnan(value) || std::isinf(value)) return not_a_number();
    char buffer[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*e", precision - 1, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    std::u16string text;
    for (const char* c = buffer; *c; ++c) text.push_back(static_cast<char16_t>(*c));
    auto parsed = parse(text);
    return parsed ? *parsed : not_a_number();
}

Error normalize(Decimal& value) {
    if (value.nan) { value.digits.clear(); value.exponent = 0; value.negative = true; return Error::none; }
    strip_leading(value.digits);
    if (value.digits.empty()) { value.exponent = 0; value.negative = false; return Error::none; }
    Error error = Error::none;
    while (!value.digits.empty() && value.digits.back() == '0') { value.digits.pop_back(); ++value.exponent; }
    if (value.digits.size() > max_digits) {
        round_away(value, value.digits.size() - max_digits, Rounding::plain);
        error = Error::loss_of_precision;
        while (!value.digits.empty() && value.digits.back() == '0') { value.digits.pop_back(); ++value.exponent; }
    }
    // Exponent range: move zeros into the mantissa, or drop precision.
    while (value.exponent > 127 && value.digits.size() < max_digits) { value.digits.push_back('0'); --value.exponent; }
    if (value.exponent > 127) { value = Decimal::not_a_number(); return Error::overflow; }
    if (value.exponent < -128) {
        const auto drop = static_cast<std::size_t>(-128 - value.exponent);
        round_away(value, drop, Rounding::plain);
        if (value.digits.empty()) { value.exponent = 0; value.negative = false; return Error::underflow; }
        error = Error::loss_of_precision;
        while (!value.digits.empty() && value.digits.back() == '0') { value.digits.pop_back(); ++value.exponent; }
    }
    return error;
}

Error add(Decimal& result, const Decimal& left, const Decimal& right) {
    if (left.nan || right.nan) { result = Decimal::not_a_number(); return Error::none; }
    auto a = left.digits, b = right.digits;
    Decimal r;
    align(a, left.exponent, b, right.exponent, r.exponent);
    if (left.negative == right.negative) { r.digits = add_magnitude(a, b); r.negative = left.negative; }
    else {
        const auto c = compare_magnitude(a, b);
        if (c >= 0) { r.digits = subtract_magnitude(a, b); r.negative = left.negative; }
        else { r.digits = subtract_magnitude(b, a); r.negative = right.negative; }
    }
    const auto e = normalize(r);
    result = r;
    return e;
}

Error subtract(Decimal& result, const Decimal& left, const Decimal& right) {
    auto negated = right;
    if (!negated.nan && !negated.digits.empty()) negated.negative = !negated.negative;
    return add(result, left, negated);
}

Error multiply(Decimal& result, const Decimal& left, const Decimal& right) {
    if (left.nan || right.nan) { result = Decimal::not_a_number(); return Error::none; }
    Decimal r;
    r.digits = multiply_magnitude(left.digits, right.digits);
    r.exponent = left.exponent + right.exponent;
    r.negative = left.negative != right.negative;
    const auto e = normalize(r);
    result = r;
    return e;
}

Error divide(Decimal& result, const Decimal& left, const Decimal& right) {
    if (left.nan || right.nan) { result = Decimal::not_a_number(); return Error::none; }
    if (right.digits.empty()) { result = Decimal::not_a_number(); return Error::divide_by_zero; }
    if (left.digits.empty()) { result = Decimal{}; return Error::none; }
    // Enough digits for max_digits + 1 significant quotient digits.
    const auto k = static_cast<int>(max_digits) + 2 + static_cast<int>(right.digits.size()) - static_cast<int>(left.digits.size());
    const auto scale = std::max(k, 0);
    bool inexact = false;
    Decimal r;
    r.digits = divide_magnitude(left.digits + std::string(static_cast<std::size_t>(scale), '0'), right.digits, inexact);
    r.exponent = left.exponent - right.exponent - scale;
    r.negative = left.negative != right.negative;
    if (inexact) {
        // A sticky digit keeps plain rounding correct for remainders exactly at .5.
        r.digits.push_back('1');
        --r.exponent;
    }
    auto e = normalize(r);
    if (inexact && e == Error::none) e = Error::loss_of_precision;
    result = r;
    return e;
}

Error power(Decimal& result, const Decimal& value, unsigned exponent) {
    if (value.nan) { result = Decimal::not_a_number(); return Error::none; }
    Decimal r = Decimal::from_integer(1), base = value;
    Error error = Error::none;
    const auto note = [&](Error e) { if (e != Error::none && (error == Error::none || error == Error::loss_of_precision)) error = e; };
    while (exponent) {
        if (exponent & 1) note(multiply(r, r, base));
        exponent >>= 1;
        if (exponent) note(multiply(base, base, base));
        if (r.nan) break;
    }
    result = r;
    return error;
}

Error multiply_by_power_of_10(Decimal& result, const Decimal& value, int power) {
    if (value.nan) { result = Decimal::not_a_number(); return Error::none; }
    Decimal r = value;
    if (!r.digits.empty()) r.exponent += power;
    const auto e = normalize(r);
    result = r;
    return e;
}

void round(Decimal& result, const Decimal& value, int scale, Rounding mode) {
    Decimal r = value;
    if (r.nan || scale == no_scale || r.digits.empty() || -r.exponent <= scale) { result = r; return; }
    const auto drop = static_cast<std::size_t>(-r.exponent - scale);
    round_away(r, drop, mode);
    if (r.digits.empty()) r.negative = false;
    normalize(r);
    result = r;
}

int compare(const Decimal& left, const Decimal& right) {
    if (left.nan || right.nan) return left.nan && right.nan ? 0 : left.nan ? -1 : 1;
    const bool lz = left.digits.empty(), rz = right.digits.empty();
    if (lz && rz) return 0;
    const bool ln = !lz && left.negative, rn = !rz && right.negative;
    if (ln != rn) return ln ? -1 : 1;
    // Same sign: compare magnitudes (zero is the smallest magnitude).
    int magnitude;
    if (lz || rz) magnitude = lz ? -1 : 1;
    else {
        auto a = left.digits, b = right.digits;
        int e;
        align(a, left.exponent, b, right.exponent, e);
        magnitude = compare_magnitude(a, b);
    }
    return ln ? -magnitude : magnitude;
}

double to_double(const Decimal& value) {
    if (value.nan) return std::numeric_limits<double>::quiet_NaN();
    if (value.digits.empty()) return 0.0;
    const auto text = (value.negative ? "-" : "") + value.digits + "e" + std::to_string(value.exponent);
    return std::strtod(text.c_str(), nullptr);
}

std::uint64_t to_uint64(const Decimal& value) {
    if (value.nan || value.digits.empty() || value.negative) return 0;
    std::string whole = value.digits;
    if (value.exponent >= 0) {
        if (whole.size() + static_cast<std::size_t>(value.exponent) > 20) return std::numeric_limits<std::uint64_t>::max();
        whole.append(static_cast<std::size_t>(value.exponent), '0');
    } else if (static_cast<std::size_t>(-value.exponent) >= whole.size()) return 0;
    else whole.resize(whole.size() - static_cast<std::size_t>(-value.exponent));
    if (compare_magnitude(whole, "18446744073709551615") > 0) return std::numeric_limits<std::uint64_t>::max();
    return std::strtoull(whole.c_str(), nullptr, 10);
}

std::int64_t to_int64(const Decimal& value) {
    if (value.nan || value.digits.empty()) return 0;
    Decimal magnitude = value;
    magnitude.negative = false;
    const auto m = to_uint64(magnitude);
    if (!value.negative) return m > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(m);
    if (m > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1) return std::numeric_limits<std::int64_t>::min();
    return m == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1 ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(m);
}

std::string to_string(const Decimal& value, std::string_view separator) {
    if (value.nan) return "NaN";
    if (value.digits.empty()) return "0";
    std::string out = value.negative ? "-" : "";
    if (value.exponent >= 0) return out + value.digits + std::string(static_cast<std::size_t>(value.exponent), '0');
    const auto fraction = static_cast<std::size_t>(-value.exponent);
    if (fraction >= value.digits.size()) return out + "0" + std::string(separator) + std::string(fraction - value.digits.size(), '0') + value.digits;
    return out + value.digits.substr(0, value.digits.size() - fraction) + std::string(separator) + value.digits.substr(value.digits.size() - fraction);
}

std::optional<Decimal> parse(std::u16string_view text, std::u16string_view separator, std::size_t* consumed) {
    std::size_t at = 0;
    while (at < text.size() && (text[at] == u' ' || text[at] == u'\t' || text[at] == u'\n' || text[at] == u'\r')) ++at;
    Decimal d;
    if (at < text.size() && (text[at] == u'+' || text[at] == u'-')) { d.negative = text[at] == u'-'; ++at; }
    bool any = false;
    int dropped = 0;        // integer digits beyond the precision: they scale the exponent
    bool sticky = false;
    const auto take = [&](char16_t c, bool fraction) {
        any = true;
        if (d.digits.empty() && c == u'0') { if (fraction) --d.exponent; return; }
        if (d.digits.size() < max_digits + 1) { d.digits.push_back(static_cast<char>(c)); if (fraction) --d.exponent; }
        else { if (!fraction) ++dropped; if (c != u'0') sticky = true; }
    };
    while (at < text.size() && text[at] >= u'0' && text[at] <= u'9') take(text[at++], false);
    if (!separator.empty() && text.substr(at, separator.size()) == separator) {
        const auto save = at;
        at += separator.size();
        const bool had = any;
        while (at < text.size() && text[at] >= u'0' && text[at] <= u'9') take(text[at++], true);
        if (!any && !had) at = save;
    }
    if (!any) return std::nullopt;
    if (at < text.size() && (text[at] == u'e' || text[at] == u'E')) {
        auto e = at + 1;
        bool negative_exponent = false;
        if (e < text.size() && (text[e] == u'+' || text[e] == u'-')) { negative_exponent = text[e] == u'-'; ++e; }
        if (e < text.size() && text[e] >= u'0' && text[e] <= u'9') {
            long long v = 0;
            while (e < text.size() && text[e] >= u'0' && text[e] <= u'9') { if (v < 100000) v = v * 10 + (text[e] - u'0'); ++e; }
            d.exponent += static_cast<int>(negative_exponent ? -v : v);
            at = e;
        }
    }
    d.exponent += dropped;
    if (consumed) *consumed = at;
    if (sticky && d.digits.size() > max_digits) {
        // Keep the precision rounding exact: the sticky digit marks discarded nonzero digits.
        d.digits.push_back('1');
        --d.exponent;
    }
    if (d.digits.empty()) d.negative = false;
    normalize(d);
    return d;
}

namespace {
using Limbs = std::array<std::uint32_t, 4>;   // little-endian 128-bit
void mul_add(Limbs& v, std::uint32_t m, std::uint32_t a) {
    std::uint64_t carry = a;
    for (auto& limb : v) {
        const std::uint64_t x = static_cast<std::uint64_t>(limb) * m + carry;
        limb = static_cast<std::uint32_t>(x);
        carry = x >> 32;
    }
}
std::uint32_t div_small(Limbs& v, std::uint32_t d) {
    std::uint64_t rest = 0;
    for (auto i = v.size(); i-- > 0;) {
        const std::uint64_t x = (rest << 32) | v[i];
        v[i] = static_cast<std::uint32_t>(x / d);
        rest = x % d;
    }
    return static_cast<std::uint32_t>(rest);
}
bool is_zero(const Limbs& v) { return !v[0] && !v[1] && !v[2] && !v[3]; }
} // namespace

Bytes pack(const Decimal& value) {
    Bytes out{};
    Limbs mantissa{};
    if (!value.nan) for (const auto c : value.digits) mul_add(mantissa, 10, static_cast<std::uint32_t>(c - '0'));
    std::uint16_t words[8];
    for (int i = 0; i < 4; ++i) { words[2 * i] = static_cast<std::uint16_t>(mantissa[static_cast<std::size_t>(i)]); words[2 * i + 1] = static_cast<std::uint16_t>(mantissa[static_cast<std::size_t>(i)] >> 16); }
    unsigned length = 0;
    for (unsigned i = 0; i < 8; ++i) if (words[i]) length = i + 1;
    const auto exponent = value.nan || value.digits.empty() ? 0 : value.exponent;
    std::uint32_t head = static_cast<std::uint8_t>(static_cast<std::int8_t>(exponent));
    head |= (length & 0xf) << 8;
    if (value.nan || (value.negative && !value.digits.empty())) head |= 1u << 12;
    head |= 1u << 13;   // compact
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(head >> (8 * i));
    for (int i = 0; i < 8; ++i) { out[static_cast<std::size_t>(4 + 2 * i)] = static_cast<std::uint8_t>(words[i]); out[static_cast<std::size_t>(5 + 2 * i)] = static_cast<std::uint8_t>(words[i] >> 8); }
    return out;
}

Decimal unpack(const Bytes& bytes) {
    std::uint32_t head = 0;
    for (int i = 0; i < 4; ++i) head |= static_cast<std::uint32_t>(bytes[static_cast<std::size_t>(i)]) << (8 * i);
    const auto exponent = static_cast<std::int8_t>(head & 0xff);
    const auto length = (head >> 8) & 0xf;
    const bool negative = (head >> 12) & 1;
    if (length == 0) return negative ? Decimal::not_a_number() : Decimal{};
    Limbs mantissa{};
    for (unsigned i = 0; i < 8; ++i) {
        const std::uint32_t word = i < length ? static_cast<std::uint32_t>(bytes[4 + 2 * i] | (bytes[5 + 2 * i] << 8)) : 0;
        mantissa[i / 2] |= word << (16 * (i % 2));
    }
    Decimal d;
    while (!is_zero(mantissa)) d.digits.push_back(static_cast<char>('0' + div_small(mantissa, 10)));
    std::reverse(d.digits.begin(), d.digits.end());
    d.exponent = exponent;
    d.negative = negative && !d.digits.empty();
    normalize(d);
    return d;
}

} // namespace alchemy::gui::completion_kit::decimal
