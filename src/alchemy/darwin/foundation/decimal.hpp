#pragma once

// NSDecimal arithmetic (host-only): a sign, a decimal mantissa of at most 38
// significant digits (it must fit NSDecimal's 128-bit mantissa) and a
// base-10 exponent in [-128, 127]. NaN is NSDecimal's {length 0, negative}.
// Results are compacted (trailing zeros moved into the exponent) and
// rounded to 38 digits with NSRoundPlain, as Foundation's NSDecimal
// functions do; errors are NSCalculationError values.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace alchemy::gui::completion_kit::decimal {

enum class Rounding { plain = 0, down = 1, up = 2, bankers = 3 };          // NSRoundingMode
enum class Error { none = 0, loss_of_precision = 1, underflow = 2, overflow = 3, divide_by_zero = 4 };   // NSCalculationError
inline constexpr int no_scale = 32767;                                      // NSDecimalNoScale
inline constexpr std::size_t max_digits = 38;

struct Decimal {
    bool negative{};
    bool nan{};
    int exponent{};
    std::string digits;   // most significant first, no leading zeros; empty: zero

    [[nodiscard]] bool is_zero() const { return !nan && digits.empty(); }
    static Decimal not_a_number() { Decimal d; d.nan = true; d.negative = true; return d; }
    static Decimal from_integer(std::int64_t value);
    static Decimal from_unsigned(std::uint64_t value);
    // Shortest decimal that round-trips the double (17 significant digits at most).
    static Decimal from_double(double value);
    // mantissa x 10^exponent.
    static Decimal from_parts(std::uint64_t mantissa, int exponent, bool negative);
};

// Removes trailing zeros (NSDecimalCompact) and limits the precision.
Error normalize(Decimal& value);

Error add(Decimal& result, const Decimal& left, const Decimal& right);
Error subtract(Decimal& result, const Decimal& left, const Decimal& right);
Error multiply(Decimal& result, const Decimal& left, const Decimal& right);
Error divide(Decimal& result, const Decimal& left, const Decimal& right);
Error power(Decimal& result, const Decimal& value, unsigned exponent);
Error multiply_by_power_of_10(Decimal& result, const Decimal& value, int power);
// NSDecimalRound: `scale` digits after the decimal point (no_scale: unchanged).
void round(Decimal& result, const Decimal& value, int scale, Rounding mode);
// NSOrderedAscending -1, NSOrderedSame 0, NSOrderedDescending 1; NaN sorts first.
int compare(const Decimal& left, const Decimal& right);

[[nodiscard]] double to_double(const Decimal& value);
// Saturating conversions (Foundation truncates toward zero).
[[nodiscard]] std::int64_t to_int64(const Decimal& value);
[[nodiscard]] std::uint64_t to_uint64(const Decimal& value);
// NSDecimalString: plain notation, `separator` between integer and fraction ("NaN" for NaN).
[[nodiscard]] std::string to_string(const Decimal& value, std::string_view separator = ".");
// -[NSDecimalNumber initWithString:]: optional sign, digits, separator,
// digits, optional exponent ("1.5e3"). `consumed` (when given) receives the
// characters used; nullopt when no number starts the text.
[[nodiscard]] std::optional<Decimal> parse(std::u16string_view text, std::u16string_view separator = u".", std::size_t* consumed = nullptr);

// NSDecimal's 20-byte layout: int _exponent:8, unsigned _length:4,
// _isNegative:1, _isCompact:1, _reserved:18, then unsigned short _mantissa[8]
// (128-bit little-endian mantissa).
using Bytes = std::array<std::uint8_t, 20>;
[[nodiscard]] Bytes pack(const Decimal& value);
[[nodiscard]] Decimal unpack(const Bytes& bytes);

} // namespace alchemy::gui::completion_kit::decimal
