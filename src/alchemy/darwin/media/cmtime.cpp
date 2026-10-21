#include "alchemy/darwin/media/cmtime.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <optional>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace alchemy::gui::completion_kit::media {
namespace {

using namespace cmtime_flags;

struct Scaled {
    std::int64_t value{};
    bool rounded{};
};

// round(value * multiplier / divisor), half away from zero; nullopt when the
// result does not fit in int64. multiplier and divisor are positive.
std::optional<Scaled> scale(std::int64_t value, std::uint64_t multiplier, std::uint64_t divisor) {
    const bool negative = value < 0;
    const std::uint64_t magnitude = negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    std::uint64_t high{}, low{}, quotient{}, remainder{};
#ifdef _MSC_VER
    low = _umul128(magnitude, multiplier, &high);
    if (high >= divisor) return std::nullopt;
    quotient = _udiv128(high, low, divisor, &remainder);
#else
    const auto product = static_cast<unsigned __int128>(magnitude) * multiplier;
    high = static_cast<std::uint64_t>(product >> 64);
    if (high >= divisor) return std::nullopt;
    quotient = static_cast<std::uint64_t>(product / divisor);
    remainder = static_cast<std::uint64_t>(product % divisor);
    static_cast<void>(low);
#endif
    if (remainder >= divisor - remainder) {
        if (quotient == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        ++quotient;
    }
    const std::uint64_t limit = negative ? (std::uint64_t{1} << 63) : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (quotient > limit) return std::nullopt;
    const auto signed_value = negative ? static_cast<std::int64_t>(0 - quotient) : static_cast<std::int64_t>(quotient);
    return Scaled{signed_value, remainder != 0};
}

bool add_overflows(std::int64_t a, std::int64_t b, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &out);
#else
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) || (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) return true;
    out = a + b;
    return false;
#endif
}

CMTime infinity(bool negative) { return negative ? time_negative_infinity : time_positive_infinity; }

// lhs + sign * rhs for numeric times in the same epoch.
CMTime combine(const CMTime& lhs, const CMTime& rhs, int sign) {
    const std::int64_t a = lhs.timescale, b = rhs.timescale;
    const auto lcm = a / std::gcd(a, b) * b;
    std::int64_t target = lcm <= max_timescale ? lcm : std::max(a, b);
    const bool inherited = ((lhs.flags | rhs.flags) & rounded) != 0;
    while (target > 0) {
        const auto left = scale(lhs.value, static_cast<std::uint64_t>(target), static_cast<std::uint64_t>(a));
        auto right = scale(rhs.value, static_cast<std::uint64_t>(target), static_cast<std::uint64_t>(b));
        if (left && right && !(sign < 0 && right->value == std::numeric_limits<std::int64_t>::min())) {
            std::int64_t sum{};
            if (!add_overflows(left->value, sign < 0 ? -right->value : right->value, sum)) {
                const bool was_rounded = inherited || left->rounded || right->rounded;
                return {sum, static_cast<std::int32_t>(target), valid | (was_rounded ? rounded : 0), lhs.epoch};
            }
        }
        target /= 2;
    }
    // No timescale holds the sum: it is beyond int64 in every scale.
    return infinity(seconds(lhs) + sign * seconds(rhs) < 0);
}

std::int32_t sign_of(std::int64_t value) { return value < 0 ? -1 : value > 0 ? 1 : 0; }

} // namespace

bool is_valid(const CMTime& time) { return (time.flags & valid) != 0; }
bool is_numeric(const CMTime& time) { return (time.flags & (valid | implied)) == valid; }

CMTime make(std::int64_t value, std::int32_t timescale) {
    // CMTimeMake does not validate: a non-positive timescale yields an invalid time.
    return {value, timescale, timescale > 0 ? valid : 0U, 0};
}

CMTime make_with_seconds(double value, std::int32_t preferred_timescale) {
    if (std::isnan(value) || preferred_timescale <= 0) return time_invalid;
    if (std::isinf(value)) return infinity(value < 0);
    for (std::int64_t timescale = preferred_timescale; timescale > 0; timescale /= 2) {
        const double scaled = value * static_cast<double>(timescale);
        if (std::fabs(scaled) >= 9.2233720368547758e18) continue;
        const double whole = std::round(scaled);   // half away from zero
        return {static_cast<std::int64_t>(whole), static_cast<std::int32_t>(timescale), valid | (whole != scaled ? rounded : 0U), 0};
    }
    return infinity(value < 0);
}

double seconds(const CMTime& time) {
    if (!is_valid(time) || (time.flags & indefinite)) return std::numeric_limits<double>::quiet_NaN();
    if (time.flags & positive_infinity) return std::numeric_limits<double>::infinity();
    if (time.flags & negative_infinity) return -std::numeric_limits<double>::infinity();
    if (time.timescale <= 0) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(time.value) / time.timescale;
}

CMTime add(const CMTime& lhs, const CMTime& rhs) {
    if (!is_valid(lhs) || !is_valid(rhs)) return time_invalid;
    if ((lhs.flags | rhs.flags) & indefinite) return time_indefinite;
    const bool plus = ((lhs.flags | rhs.flags) & positive_infinity) != 0, minus = ((lhs.flags | rhs.flags) & negative_infinity) != 0;
    if (plus && minus) return time_invalid;
    if (plus || minus) return infinity(minus);
    if (lhs.epoch != rhs.epoch) return time_invalid;
    return combine(lhs, rhs, 1);
}

CMTime subtract(const CMTime& lhs, const CMTime& rhs) {
    if (!is_valid(lhs) || !is_valid(rhs)) return time_invalid;
    if ((lhs.flags | rhs.flags) & indefinite) return time_indefinite;
    // lhs - rhs: rhs's infinity flips sign.
    const bool plus = (lhs.flags & positive_infinity) || (rhs.flags & negative_infinity);
    const bool minus = (lhs.flags & negative_infinity) || (rhs.flags & positive_infinity);
    if (plus && minus) return time_invalid;
    if (plus || minus) return infinity(minus);
    if (lhs.epoch != rhs.epoch) return time_invalid;
    return combine(lhs, rhs, -1);
}

CMTime multiply(const CMTime& time, std::int32_t multiplier) {
    if (!is_valid(time)) return time_invalid;
    if (time.flags & indefinite) return time_indefinite;
    if (time.flags & (positive_infinity | negative_infinity)) {
        if (multiplier == 0) return time_invalid;   // infinity times zero has no value
        return infinity(((time.flags & negative_infinity) != 0) != (multiplier < 0));
    }
    const bool negative = multiplier < 0;
    const auto magnitude = static_cast<std::uint64_t>(negative ? -static_cast<std::int64_t>(multiplier) : multiplier);
    for (std::uint64_t divisor = 1, timescale = static_cast<std::uint64_t>(time.timescale); timescale > 0; divisor *= 2, timescale /= 2) {
        const auto product = scale(time.value, magnitude, divisor);
        if (!product || (negative && product->value == std::numeric_limits<std::int64_t>::min())) continue;
        const bool was_rounded = product->rounded || divisor > 1 || (time.flags & rounded);
        return {negative ? -product->value : product->value, static_cast<std::int32_t>(timescale), valid | (was_rounded ? rounded : 0U), time.epoch};
    }
    return infinity((time.value < 0) != negative);
}

CMTime multiply_by_float64(const CMTime& time, double multiplier) {
    if (!is_valid(time) || std::isnan(multiplier)) return time_invalid;
    if (time.flags & indefinite) return time_indefinite;
    if (time.flags & (positive_infinity | negative_infinity)) {
        if (multiplier == 0) return time_invalid;
        return infinity(((time.flags & negative_infinity) != 0) != (multiplier < 0));
    }
    if (std::isinf(multiplier)) return time.value == 0 ? time_invalid : infinity((time.value < 0) != (multiplier < 0));
    double product = static_cast<double>(time.value) * multiplier;
    for (std::int64_t timescale = time.timescale; timescale > 0; timescale /= 2, product /= 2) {
        if (std::fabs(product) >= 9.2233720368547758e18) continue;
        const double whole = std::round(product);
        const bool was_rounded = whole != product || timescale != time.timescale || (time.flags & rounded);
        return {static_cast<std::int64_t>(whole), static_cast<std::int32_t>(timescale), valid | (was_rounded ? rounded : 0U), time.epoch};
    }
    return infinity(product < 0);
}

CMTime convert_scale(const CMTime& time, std::int32_t timescale) {
    if (!is_valid(time) || timescale <= 0) return time_invalid;
    if (!is_numeric(time)) return time;
    if (time.timescale == timescale) return time;
    const auto value = scale(time.value, static_cast<std::uint64_t>(timescale), static_cast<std::uint64_t>(time.timescale));
    if (!value) return infinity(time.value < 0);
    return {value->value, timescale, valid | ((value->rounded || (time.flags & rounded)) ? rounded : 0U), time.epoch};
}

std::int32_t compare(const CMTime& lhs, const CMTime& rhs) {
    // Order: -infinity < numeric < indefinite < +infinity < invalid.
    const auto rank = [](const CMTime& time) {
        if (!is_valid(time)) return 4;
        if (time.flags & positive_infinity) return 3;
        if (time.flags & indefinite) return 2;
        if (time.flags & negative_infinity) return 0;
        return 1;
    };
    const auto left = rank(lhs), right = rank(rhs);
    if (left != right) return left < right ? -1 : 1;
    if (left != 1) return 0;
    if (lhs.epoch != rhs.epoch) return lhs.epoch < rhs.epoch ? -1 : 1;
    if (lhs.timescale == rhs.timescale) return lhs.value < rhs.value ? -1 : lhs.value > rhs.value ? 1 : 0;
    // Compare lhs.value * rhs.timescale with rhs.value * lhs.timescale exactly.
    const auto ls = sign_of(lhs.value), rs = sign_of(rhs.value);
    if (ls != rs) return ls < rs ? -1 : 1;
    if (ls == 0) return 0;
    const auto magnitude = [](std::int64_t value) { return value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value); };
    std::uint64_t lh{}, ll{}, rh{}, rl{};
#ifdef _MSC_VER
    ll = _umul128(magnitude(lhs.value), static_cast<std::uint64_t>(rhs.timescale), &lh);
    rl = _umul128(magnitude(rhs.value), static_cast<std::uint64_t>(lhs.timescale), &rh);
#else
    const auto lp = static_cast<unsigned __int128>(magnitude(lhs.value)) * static_cast<std::uint64_t>(rhs.timescale);
    const auto rp = static_cast<unsigned __int128>(magnitude(rhs.value)) * static_cast<std::uint64_t>(lhs.timescale);
    lh = static_cast<std::uint64_t>(lp >> 64); ll = static_cast<std::uint64_t>(lp);
    rh = static_cast<std::uint64_t>(rp >> 64); rl = static_cast<std::uint64_t>(rp);
#endif
    const int magnitude_order = lh != rh ? (lh < rh ? -1 : 1) : ll != rl ? (ll < rl ? -1 : 1) : 0;
    return ls > 0 ? magnitude_order : -magnitude_order;
}

CMTimeRange range_make(const CMTime& start, const CMTime& duration) { return {start, duration}; }

CMTime range_end(const CMTimeRange& range) {
    if (!is_valid(range.start) || !is_valid(range.duration)) return time_invalid;
    return add(range.start, range.duration);
}

std::string description(const CMTime& time) {
    if (!is_valid(time)) return "{INVALID}";
    if (time.flags & indefinite) return "{INDEFINITE}";
    if (time.flags & positive_infinity) return "{+INFINITY}";
    if (time.flags & negative_infinity) return "{-INFINITY}";
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "{%lld/%d = %.3f", static_cast<long long>(time.value), time.timescale, seconds(time));
    std::string text = buffer;
    if (time.flags & rounded) text += ", rounded";
    if (time.epoch) text += ", epoch = " + std::to_string(time.epoch);
    return text + "}";
}

} // namespace alchemy::gui::completion_kit::media
