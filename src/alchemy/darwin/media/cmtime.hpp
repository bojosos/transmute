#pragma once

// CMTime arithmetic (host-only core of CoreMedia's CMTime functions, QLab
// gap group G5). Semantics follow <CoreMedia/CMTime.h>:
// - a time is valid when kCMTimeFlags_Valid is set; +infinity, -infinity and
//   indefinite are valid times whose value and timescale are ignored;
// - sums and differences of times in different epochs are invalid;
// - mixed timescales use their least common multiple when it fits in
//   kCMTimeMaxTimescale and the value fits in int64, otherwise the larger
//   timescale with rounding (kCMTimeFlags_HasBeenRounded);
// - a value that overflows in the result timescale halves the timescale
//   until it fits (rounding), as CMTimeMultiply documents;
// - rounding is kCMTimeRoundingMethod_RoundHalfAwayFromZero (the default).

#include <cstdint>
#include <string>

namespace alchemy::gui::completion_kit::media {

struct CMTime {
    std::int64_t value{};
    std::int32_t timescale{};
    std::uint32_t flags{};
    std::int64_t epoch{};
};

struct CMTimeRange {
    CMTime start, duration;
};

namespace cmtime_flags {
inline constexpr std::uint32_t valid = 1, rounded = 2, positive_infinity = 4, negative_infinity = 8, indefinite = 16;
inline constexpr std::uint32_t implied = positive_infinity | negative_infinity | indefinite;
} // namespace cmtime_flags

inline constexpr std::int32_t max_timescale = 0x7fffffff;

inline constexpr CMTime time_invalid{};
inline constexpr CMTime time_zero{0, 1, cmtime_flags::valid, 0};
inline constexpr CMTime time_positive_infinity{0, 0, cmtime_flags::valid | cmtime_flags::positive_infinity, 0};
inline constexpr CMTime time_negative_infinity{0, 0, cmtime_flags::valid | cmtime_flags::negative_infinity, 0};
inline constexpr CMTime time_indefinite{0, 0, cmtime_flags::valid | cmtime_flags::indefinite, 0};

bool is_valid(const CMTime& time);              // CMTIME_IS_VALID
bool is_numeric(const CMTime& time);            // CMTIME_IS_NUMERIC (valid, not infinite or indefinite)
CMTime make(std::int64_t value, std::int32_t timescale);
CMTime make_with_seconds(double seconds, std::int32_t preferred_timescale);
double seconds(const CMTime& time);             // NaN for invalid or indefinite
CMTime add(const CMTime& lhs, const CMTime& rhs);
CMTime subtract(const CMTime& lhs, const CMTime& rhs);
CMTime multiply(const CMTime& time, std::int32_t multiplier);
CMTime multiply_by_float64(const CMTime& time, double multiplier);
CMTime convert_scale(const CMTime& time, std::int32_t timescale);   // CMTimeConvertScale, default rounding
std::int32_t compare(const CMTime& lhs, const CMTime& rhs);        // -1, 0, 1
CMTimeRange range_make(const CMTime& start, const CMTime& duration);
CMTime range_end(const CMTimeRange& range);                         // start + duration (invalid range: invalid)
std::string description(const CMTime& time);                       // CMTimeCopyDescription's text

} // namespace alchemy::gui::completion_kit::media
