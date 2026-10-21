#pragma once

// NSByteCountFormatter's strings (host-only, en_US): the unit is the largest
// allowed one the count reaches (powers of 1000 for the file and decimal
// styles, 1024 for memory and binary), "byte"/"bytes" below one KB, and
// adaptive precision (no fraction digits for bytes and KB, one for MB, two
// for GB and above; three significant digits when not adaptive). Rounding is
// half up; a value that rounds up to the next unit moves to it. Zero is
// "Zero KB" when non-numeric formatting is allowed.
#include <cstdint>
#include <string>

namespace alchemy::gui::completion_kit::byte_count {

enum class Style : std::int64_t { file = 0, memory = 1, decimal = 2, binary = 3 };
// NSByteCountFormatterUnits.
enum Units : std::uint64_t { bytes = 1, kb = 2, mb = 4, gb = 8, tb = 16, pb = 32, eb = 64, zb = 128, yb_or_higher = 0xff00, all = 0xffff };

struct Options {
    Style style{Style::file};
    std::uint64_t allowed{0};              // 0: NSByteCountFormatterUseDefault (all units)
    bool includes_unit{true}, includes_count{true}, includes_actual{false};
    bool adaptive{true}, nonnumeric{true}, zero_pads{false};
};

[[nodiscard]] std::u16string format(std::int64_t count, const Options& options);

} // namespace alchemy::gui::completion_kit::byte_count
