#pragma once

// NSScanner's number scanning (host-only), over UTF-16 at a position (the
// characters to skip are skipped by the caller). Each returns the value and
// the position after it, or nullopt (the location does not move).
//   integer   optional sign, decimal digits; overflow saturates at the type's
//             limits (Foundation returns YES with INT_MAX / INT_MIN)
//   unsigned  as integer, '-' wraps as strtoull does
//   hex       optional "0x"/"0X", hexadecimal digits; overflow saturates
//   floating  optional sign, digits, `separator`, digits, exponent
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace alchemy::gui::completion_kit::scan {

[[nodiscard]] std::optional<std::pair<std::int64_t, std::size_t>> integer(std::u16string_view text, std::size_t at, std::int64_t min, std::int64_t max);
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::size_t>> unsigned_integer(std::u16string_view text, std::size_t at);
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::size_t>> hex(std::u16string_view text, std::size_t at, std::uint64_t max);
[[nodiscard]] std::optional<std::pair<double, std::size_t>> floating(std::u16string_view text, std::size_t at, char16_t separator = u'.');

} // namespace alchemy::gui::completion_kit::scan
