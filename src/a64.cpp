#include "transmute/a64.hpp"
#include "transmute/jit.hpp"
#include "native_memory_context.hpp"
#include "native_topology_history.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <typeinfo>
#include <utility>

namespace transmute {
namespace {

constexpr std::uint64_t stack_top = 0x0000700000000000ULL;
constexpr std::uint64_t default_stack_size = 1024U * 1024U;
constexpr std::uint64_t maximum_stack_size = 1024ULL * 1024U * 1024U;
constexpr std::uint64_t stack_granularity = 64U * 1024U;
// Unmapped gap below every worker stack, catching overflow into a neighbour.
constexpr std::uint64_t worker_stack_gap = 1024U * 1024U;
constexpr std::uint64_t worker_stack_floor = 0x0000602000000000ULL;

std::uint64_t normalize_stack_size(std::uint64_t requested, std::uint64_t fallback) {
    auto size = requested == 0 ? fallback : requested;
    if (size == 0) size = default_stack_size;
    if (size > maximum_stack_size) {
        throw std::runtime_error("requested guest stack exceeds 1 GiB");
    }
    return (size + stack_granularity - 1) & ~(stack_granularity - 1);
}
// Above guest stacks and below the Windows PEB/TEB mappings.
constexpr std::uint64_t guest_call_base = 0x0000710000000000ULL;
constexpr std::uint64_t guest_call_stride = 16;
constexpr std::uint64_t maximum_guest_calls = 65'536;
constexpr std::uint64_t guest_call_end =
    guest_call_base + guest_call_stride * maximum_guest_calls;
thread_local ReferenceEngine* active_engine = nullptr;
constexpr std::uint64_t return_sentinel = 0xfffffffffffffff0ULL;
// Guest DCZID_EL0.BS=4 advertises a 16-word (64-byte) DC ZVA block.
constexpr std::uint32_t guest_dczid_el0 = 4;
constexpr std::uint64_t guest_dczva_bytes = 4ULL << guest_dczid_el0;
constexpr std::uintptr_t native_image_write_tag = 1;

constexpr std::size_t exclusive_epoch_bucket(std::uint64_t address) {
    const auto granule = address >> 4;
    return static_cast<std::size_t>(
        (granule ^ (granule >> 16) ^ (granule >> 32)) & 0xffffU);
}

template<class Table>
NativeSpanMissKind diagnose_span_miss(const Table& table,
    const std::bitset<Table::entry_count>& invalidated_slots,
    std::uint64_t address, unsigned bytes) {
    const auto index = static_cast<std::size_t>(
        (address >> 12) & (Table::entry_count - 1));
    const auto& entry = table.entries[index];
    if (entry.topology_epoch == 0)
        return invalidated_slots.test(index)
            ? NativeSpanMissKind::stale : NativeSpanMissKind::empty;
    if (entry.topology_epoch != table.current_epoch)
        return NativeSpanMissKind::stale;
    if (address < entry.guest_begin || address - entry.guest_begin >= entry.guest_size)
        return NativeSpanMissKind::collision;
    if (bytes > entry.guest_size - (address - entry.guest_begin))
        return NativeSpanMissKind::range;
    return NativeSpanMissKind::covered;
}

bool overlaps_guest_call_window(std::uint64_t base, std::uint64_t size) {
    return size != 0 && base < guest_call_end &&
           (size > UINT64_MAX - base || base + size > guest_call_base);
}

std::int64_t signed_branch_offset(std::uint32_t bits) {
    const auto value = static_cast<std::int64_t>(bits & 0x03ffffffU);
    return (value >= (1LL << 25) ? value - (1LL << 26) : value) * 4;
}

bool condition_holds(unsigned condition, std::uint32_t nzcv) {
    const bool n = (nzcv & 0x80000000U) != 0;
    const bool z = (nzcv & 0x40000000U) != 0;
    const bool c = (nzcv & 0x20000000U) != 0;
    const bool v = (nzcv & 0x10000000U) != 0;
    switch (condition) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 10: return n == v;
    case 11: return n != v;
    case 12: return !z && n == v;
    case 13: return z || n != v;
    case 14: return true;
    case 15: return true;
    default: throw std::runtime_error("invalid ARM64 branch condition");
    }
}

std::uint32_t arithmetic_flags(std::uint64_t lhs, std::uint64_t rhs,
                               std::uint64_t result, bool subtract, bool wide) {
    const auto sign_bit = wide ? 1ULL << 63 : 1ULL << 31;
    const bool n = (result & sign_bit) != 0;
    const bool z = result == 0;
    const bool c = subtract ? lhs >= rhs : result < lhs;
    const bool v = subtract ? ((lhs ^ rhs) & (lhs ^ result) & sign_bit) != 0
                            : ((~(lhs ^ rhs)) & (lhs ^ result) & sign_bit) != 0;
    return (n ? 0x80000000U : 0) | (z ? 0x40000000U : 0) |
           (c ? 0x20000000U : 0) | (v ? 0x10000000U : 0);
}

std::uint64_t logical_immediate_mask(std::uint32_t instruction, bool wide) {
    const auto n = (instruction >> 22) & 1U;
    const auto immr = (instruction >> 16) & 63U;
    const auto imms = (instruction >> 10) & 63U;
    const auto pattern = (n << 6) | (~imms & 63U);
    const auto length = std::bit_width(pattern);
    if ((!wide && n != 0) || length < 2 || length > (wide ? 7 : 6)) {
        throw std::runtime_error("invalid ARM64 logical immediate");
    }
    const auto element_width = 1U << (length - 1);
    const auto levels = element_width - 1;
    const auto ones = imms & levels;
    if (ones == levels) throw std::runtime_error("invalid ARM64 logical immediate");
    const auto rotation = immr & levels;
    const auto element_mask = element_width == 64 ? UINT64_MAX
                                                   : (1ULL << element_width) - 1;
    const auto element = (1ULL << (ones + 1)) - 1;
    const auto rotated = rotation == 0
                             ? element
                             : ((element >> rotation) |
                                (element << (element_width - rotation))) & element_mask;
    std::uint64_t mask = 0;
    for (unsigned bit = 0; bit < (wide ? 64U : 32U); bit += element_width) {
        mask |= rotated << bit;
    }
    return mask;
}

enum class IntegerRounding { nearest_even, nearest_away, positive, negative, zero };

std::uint64_t unsigned_multiply_high(std::uint64_t lhs, std::uint64_t rhs) {
    const auto low_product = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(lhs)) * static_cast<std::uint32_t>(rhs);
    const auto cross_left = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(lhs)) * (rhs >> 32);
    const auto cross_right = (lhs >> 32) * static_cast<std::uint32_t>(rhs);
    const auto middle = (low_product >> 32) +
                        static_cast<std::uint32_t>(cross_left) +
                        static_cast<std::uint32_t>(cross_right);
    return (lhs >> 32) * (rhs >> 32) + (cross_left >> 32) +
           (cross_right >> 32) + (middle >> 32);
}

void record_fp_exception(CpuState& state, unsigned status_bit,
                         unsigned enable_bit);
std::uint64_t floating_add_sub_bits(std::uint64_t lhs, std::uint64_t rhs,
                                    bool wide, bool subtract, CpuState& state);

std::uint64_t floating_minmax_bits(std::uint64_t lhs, std::uint64_t rhs,
                                   bool is_double, bool minimum,
                                   bool numeric_if_quiet_nan, CpuState& state) {
    const auto sign = is_double ? 0x8000000000000000ULL : 0x80000000ULL;
    const auto exponent = is_double ? 0x7ff0000000000000ULL : 0x7f800000ULL;
    const auto fraction = is_double ? 0x000fffffffffffffULL : 0x007fffffULL;
    const auto quiet_bit = is_double ? 0x0008000000000000ULL : 0x00400000ULL;
    const auto nan = [exponent, fraction](std::uint64_t bits) {
        return (bits & exponent) == exponent && (bits & fraction) != 0;
    };
    const auto flush_input = [exponent, fraction, sign, &state](
                                 std::uint64_t bits) {
        if ((state.fpcr & (1U << 24)) != 0 &&
            (bits & exponent) == 0 && (bits & fraction) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE
            return bits & sign;
        }
        return bits;
    };
    lhs = flush_input(lhs);
    rhs = flush_input(rhs);
    const bool left_nan = nan(lhs);
    const bool right_nan = nan(rhs);
    if (left_nan || right_nan) {
        const bool left_signaling = left_nan && (lhs & quiet_bit) == 0;
        const bool right_signaling = right_nan && (rhs & quiet_bit) == 0;
        if (left_signaling || right_signaling) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        if ((state.fpcr & (1U << 25)) != 0 &&
            (left_signaling || right_signaling ||
             !numeric_if_quiet_nan || (left_nan && right_nan))) {
            return exponent | quiet_bit; // FPCR.DN
        }
        if (left_signaling) return lhs | quiet_bit;
        if (right_signaling) return rhs | quiet_bit;
        if (!numeric_if_quiet_nan || (left_nan && right_nan)) {
            return (left_nan ? lhs : rhs) | quiet_bit;
        }
        return left_nan ? rhs : lhs;
    }
    if ((lhs & ~sign) == 0 && (rhs & ~sign) == 0) {
        return minimum ? lhs | rhs : lhs & rhs;
    }
    const auto left = is_double ? std::bit_cast<double>(lhs)
                                : static_cast<double>(std::bit_cast<float>(
                                      static_cast<std::uint32_t>(lhs)));
    const auto right = is_double ? std::bit_cast<double>(rhs)
                                 : static_cast<double>(std::bit_cast<float>(
                                       static_cast<std::uint32_t>(rhs)));
    if (left == right) return lhs;
    return minimum ? (left < right ? lhs : rhs)
                   : (left > right ? lhs : rhs);
}

void record_fp_exception(CpuState& state, unsigned status_bit,
                         unsigned enable_bit) {
    if ((state.fpcr & (1U << enable_bit)) != 0) {
        throw std::runtime_error("guest floating-point exception trap is unsupported");
    }
    state.fpsr |= 1U << status_bit;
}

std::uint32_t floating_compare_flags(std::uint64_t lhs, std::uint64_t rhs,
                                     bool wide, bool signal_all_nans,
                                     CpuState& state) {
    const auto sign = wide ? 0x8000000000000000ULL : 0x80000000ULL;
    const auto exponent = wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
    const auto fraction = wide ? 0x000fffffffffffffULL : 0x007fffffULL;
    const auto quiet = wide ? 0x0008000000000000ULL : 0x00400000ULL;
    if (!wide) {
        lhs = static_cast<std::uint32_t>(lhs);
        rhs = static_cast<std::uint32_t>(rhs);
    }
    const auto flush_input = [exponent, fraction, sign, &state](
                                 std::uint64_t bits) {
        if ((state.fpcr & (1U << 24)) != 0 &&
            (bits & exponent) == 0 && (bits & fraction) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE
            return bits & sign;
        }
        return bits;
    };
    lhs = flush_input(lhs);
    rhs = flush_input(rhs);
    const bool lhs_nan = (lhs & exponent) == exponent && (lhs & fraction) != 0;
    const bool rhs_nan = (rhs & exponent) == exponent && (rhs & fraction) != 0;
    if (lhs_nan || rhs_nan) {
        if (signal_all_nans || (lhs_nan && (lhs & quiet) == 0) ||
            (rhs_nan && (rhs & quiet) == 0)) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        return 0x30000000U; // Unordered: C and V.
    }
    if ((lhs & ~sign) == 0 && (rhs & ~sign) == 0) return 0x60000000U;
    if (lhs == rhs) return 0x60000000U;
    const bool lhs_negative = (lhs & sign) != 0;
    const bool rhs_negative = (rhs & sign) != 0;
    const bool less = lhs_negative != rhs_negative ? lhs_negative :
                      lhs_negative ? lhs > rhs : lhs < rhs;
    return less ? 0x80000000U : 0x20000000U;
}

std::uint64_t floating_absolute_difference_bits(std::uint64_t first,
                                                std::uint64_t second,
                                                bool wide, CpuState& state) {
    const auto sign = wide ? 0x8000000000000000ULL : 0x80000000ULL;
    // FABD is architectural FPSub followed by sign-clearing FPAbs. Reusing
    // subtraction also preserves FPCR rounding and its FPSR exceptions.
    return floating_add_sub_bits(first, second, wide, true, state) & ~sign;
}

std::uint64_t floating_round_integral_bits(std::uint64_t raw, bool wide,
                                           IntegerRounding rounding, bool exact,
                                           CpuState& state) {
    if (!wide) raw = static_cast<std::uint32_t>(raw);
    const auto sign = wide ? 0x8000000000000000ULL : 0x80000000ULL;
    const auto exponent = wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
    const auto fraction = wide ? 0x000fffffffffffffULL : 0x007fffffULL;
    const auto quiet = wide ? 0x0008000000000000ULL : 0x00400000ULL;
    const auto fraction_bits = wide ? 52U : 23U;
    const auto bias = wide ? 1023U : 127U;
    if ((raw & exponent) == exponent) {
        if ((raw & fraction) == 0) return raw; // Infinity is already integral.
        if ((raw & quiet) == 0) record_fp_exception(state, 0, 8); // IOC / IOE
        return (state.fpcr & (1U << 25)) != 0 ? exponent | quiet : raw | quiet;
    }
    if ((raw & ~sign) == 0) return raw;
    if ((state.fpcr & (1U << 24)) != 0 &&
        (raw & exponent) == 0 && (raw & fraction) != 0) {
        record_fp_exception(state, 7, 15); // IDC / IDE
        return raw & sign;
    }
    const auto biased_exponent = static_cast<unsigned>(
        (raw & exponent) >> fraction_bits);
    if (biased_exponent >= bias + fraction_bits) return raw;
    const bool negative = (raw & sign) != 0;
    const auto increment_directed = [&]() {
        return (rounding == IntegerRounding::positive && !negative) ||
               (rounding == IntegerRounding::negative && negative);
    };
    std::uint64_t result;
    if (biased_exponent < bias) {
        // All nonzero magnitudes here are below one. Only exponent bias-1
        // can reach the halfway point; the fraction distinguishes a tie.
        const bool halfway_or_more = biased_exponent == bias - 1;
        const bool more_than_halfway = halfway_or_more && (raw & fraction) != 0;
        const bool increment = increment_directed() ||
            (rounding == IntegerRounding::nearest_away && halfway_or_more) ||
            (rounding == IntegerRounding::nearest_even && more_than_halfway);
        result = (raw & sign) |
                 (increment ? static_cast<std::uint64_t>(bias) << fraction_bits : 0);
    } else {
        const auto discarded_bits = bias + fraction_bits - biased_exponent;
        const auto discarded_mask = (1ULL << discarded_bits) - 1;
        const auto discarded = raw & discarded_mask;
        if (discarded == 0) return raw;
        const auto half = 1ULL << (discarded_bits - 1);
        const bool odd_integer = discarded_bits == fraction_bits ||
            ((raw >> discarded_bits) & 1U) != 0;
        const bool increment = increment_directed() ||
            (rounding == IntegerRounding::nearest_away && discarded >= half) ||
            (rounding == IntegerRounding::nearest_even &&
             (discarded > half || (discarded == half && odd_integer)));
        result = (raw & ~discarded_mask) +
                 (increment ? 1ULL << discarded_bits : 0);
    }
    if (exact) record_fp_exception(state, 4, 12); // IXC / IXE
    return result;
}

// FEAT_FRINTTS: FPRoundIntN. NaN, infinity and results outside the
// `intsize`-bit signed range give the most negative integer with IOC;
// otherwise the integral value (a zero keeps the sign of the input) with IXC
// if it differs from the input (both the X and Z forms).
std::uint64_t floating_round_int_n_bits(std::uint64_t raw, bool wide, IntegerRounding rounding,
                                        unsigned intsize, CpuState& state) {
    if (!wide) raw = static_cast<std::uint32_t>(raw);
    const auto sign = wide ? 0x8000000000000000ULL : 0x80000000ULL;
    const auto exponent = wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
    const auto fraction = wide ? 0x000fffffffffffffULL : 0x007fffffULL;
    // 2^(intsize-1) and -2^(intsize-1) in the element format.
    const auto limit = wide ? (intsize == 32 ? 0x41e0000000000000ULL : 0x43e0000000000000ULL)
                            : (intsize == 32 ? 0x4f000000ULL : 0x5f000000ULL);
    const auto most_negative = limit | sign;
    if ((raw & exponent) == exponent) {   // NaN or infinity
        record_fp_exception(state, 0, 8); // IOC / IOE
        return most_negative;
    }
    if ((state.fpcr & (1U << 24)) != 0 && (raw & exponent) == 0 && (raw & fraction) != 0) {
        record_fp_exception(state, 7, 15); // IDC / IDE, FPCR.FZ
        return raw & sign;
    }
    if ((raw & ~sign) == 0) return raw;
    const auto result = floating_round_integral_bits(raw, wide, rounding, false, state);
    const bool negative = (result & sign) != 0;
    const auto magnitude = result & ~sign;
    if ((!negative && magnitude >= limit) || (negative && magnitude > limit)) {
        record_fp_exception(state, 0, 8); // IOC / IOE (no IXC)
        return most_negative;
    }
    if (result != raw) record_fp_exception(state, 4, 12); // IXC / IXE
    return result;
}

std::uint64_t floating_widen_bits(std::uint64_t raw, bool from_half,
                                  CpuState& state) {
    if (from_half) {
        const auto sign = (raw & 0x8000ULL) << 16;
        const auto exponent = (raw >> 10) & 31U;
        const auto fraction = raw & 0x3ffU;
        if (exponent == 0) {
            if (fraction == 0) return sign;
            // FPConvert ignores FZ16 for a half-precision source.
            const auto leading = std::bit_width(fraction) - 1U;
            return sign | ((leading + 103ULL) << 23) |
                   ((fraction ^ (1ULL << leading)) << (23 - leading));
        }
        if (exponent == 31 && (state.fpcr & (1U << 26)) == 0) {
            if (fraction == 0) return sign | 0x7f800000ULL;
            if ((fraction & 0x200U) == 0) {
                record_fp_exception(state, 0, 8); // IOC / IOE
            }
            if ((state.fpcr & (1U << 25)) != 0) return 0x7fc00000ULL;
            return sign | 0x7f800000ULL | (fraction << 13) | 0x00400000ULL;
        }
        return sign | ((exponent + 112ULL) << 23) | (fraction << 13);
    }
    const auto sign = (raw & 0x80000000ULL) << 32;
    const auto exponent = (raw >> 23) & 255U;
    const auto fraction = raw & 0x7fffffU;
    if (exponent == 0) {
        if (fraction == 0) return sign;
        if ((state.fpcr & (1U << 24)) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE, FPCR.FZ
            return sign;
        }
        const auto leading = std::bit_width(fraction) - 1U;
        return sign | ((leading + 874ULL) << 52) |
               ((fraction ^ (1ULL << leading)) << (52 - leading));
    }
    if (exponent == 255) {
        if (fraction == 0) return sign | 0x7ff0000000000000ULL;
        if ((fraction & 0x400000U) == 0) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        if ((state.fpcr & (1U << 25)) != 0) return 0x7ff8000000000000ULL;
        return sign | 0x7ff0000000000000ULL |
               (fraction << 29) | 0x0008000000000000ULL;
    }
    return sign | ((exponent + 896ULL) << 52) | (fraction << 29);
}

// FEAT_FHM operand: a half-precision value as the exactly equal single
// (FPUnpack semantics: FPCR.FZ16 flushes denormals, FPCR.AHP does not apply;
// a signalling NaN stays signalling for the fused multiply-add).
std::uint64_t floating_fhm_widen_bits(std::uint64_t raw, const CpuState& state) {
    const auto sign = (raw & 0x8000ULL) << 16;
    const auto exponent = (raw >> 10) & 31U;
    const auto fraction = raw & 0x3ffU;
    if (exponent == 0) {
        if (fraction == 0 || (state.fpcr & (1U << 19)) != 0) return sign;
        const auto leading = std::bit_width(fraction) - 1U;
        return sign | ((leading + 103ULL) << 23) | ((fraction ^ (1ULL << leading)) << (23 - leading));
    }
    if (exponent == 31) return sign | 0x7f800000ULL | (fraction << 13);
    return sign | ((exponent + 112ULL) << 23) | (fraction << 13);
}

std::uint32_t floating_narrow_double_to_single_bits(std::uint64_t raw,
                                                    CpuState& state) {
    const auto sign = static_cast<std::uint32_t>(raw >> 32) & 0x80000000U;
    const auto exponent = static_cast<unsigned>((raw >> 52) & 0x7ffU);
    const auto fraction = raw & 0x000fffffffffffffULL;
    if (exponent == 0x7ffU) {
        if (fraction == 0) return sign | 0x7f800000U;
        if ((fraction & 0x0008000000000000ULL) == 0) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        if ((state.fpcr & (1U << 25)) != 0) return 0x7fc00000U;
        return sign | 0x7f800000U |
               static_cast<std::uint32_t>(fraction >> 29) | 0x00400000U;
    }
    if (exponent == 0 && fraction == 0) return sign;
    if (exponent == 0 && (state.fpcr & (1U << 24)) != 0) {
        record_fp_exception(state, 7, 15); // IDC / IDE, FPCR.FZ input
        return sign;
    }

    const auto significand = exponent == 0 ? fraction :
        (1ULL << 52) | fraction;
    const auto power = exponent == 0 ? -1074 :
        static_cast<int>(exponent) - 1075;
    const auto leading = static_cast<int>(std::bit_width(significand)) - 1;
    auto unbiased = power + leading;
    const auto rounding = (state.fpcr >> 22) & 3U;
    const bool negative = sign != 0;
    const auto round_up = [rounding, negative](std::uint64_t quotient,
                                              std::uint64_t remainder,
                                              unsigned shift) {
        if (remainder == 0) return false;
        if (rounding == 1) return !negative;
        if (rounding == 2) return negative;
        if (rounding == 3 || shift >= 64) return false;
        const auto halfway = 1ULL << (shift - 1);
        return remainder > halfway ||
               (remainder == halfway && (quotient & 1U) != 0);
    };
    const auto overflow = [&state, sign, rounding, negative]() {
        record_fp_exception(state, 2, 10); // OFC / OFE
        record_fp_exception(state, 4, 12); // IXC / IXE
        const bool infinity = rounding == 0 ||
                              (rounding == 1 && !negative) ||
                              (rounding == 2 && negative);
        return sign | (infinity ? 0x7f800000U : 0x7f7fffffU);
    };
    if (unbiased > 127) return overflow();
    if (unbiased < -126 && (state.fpcr & (1U << 24)) != 0) {
        state.fpsr |= 1U << 3; // FPCR.FZ output flush sets UFC without a trap.
        return sign;
    }

    std::uint64_t quotient;
    std::uint64_t remainder = 0;
    unsigned shift = 0;
    if (unbiased >= -126) {
        const auto discard = leading - 23;
        if (discard > 0) {
            shift = static_cast<unsigned>(discard);
            quotient = significand >> shift;
            remainder = significand & ((1ULL << shift) - 1);
        } else {
            quotient = significand << -discard;
        }
        quotient += round_up(quotient, remainder, shift);
        if (quotient == (1ULL << 24)) {
            quotient >>= 1;
            ++unbiased;
        }
        if (unbiased > 127) return overflow();
        if (remainder != 0) record_fp_exception(state, 4, 12); // IXC / IXE
        return sign | (static_cast<std::uint32_t>(unbiased + 127) << 23) |
               static_cast<std::uint32_t>(quotient & 0x7fffffU);
    }

    // Subnormal units are 2^-149. A very small double can need a shift > 64.
    const auto discard = -149 - power;
    if (discard <= 0) {
        quotient = significand << -discard;
    } else {
        shift = static_cast<unsigned>(discard);
        quotient = shift >= 64 ? 0 : significand >> shift;
        remainder = shift >= 64 ? significand :
            significand & ((1ULL << shift) - 1);
    }
    quotient += round_up(quotient, remainder, shift);
    if (remainder != 0 || (state.fpcr & (1U << 11)) != 0) {
        record_fp_exception(state, 3, 11); // UFC / UFE, pre-round tininess
    }
    if (remainder != 0) record_fp_exception(state, 4, 12); // IXC / IXE
    return sign | static_cast<std::uint32_t>(quotient);
}

std::uint16_t floating_narrow_to_half_bits(std::uint64_t raw,
                                           bool source_double,
                                           CpuState& state) {
    const bool alternative_half = (state.fpcr & (1U << 26)) != 0;
    const auto sign = static_cast<std::uint16_t>(
        (raw >> (source_double ? 48 : 16)) & 0x8000U);
    const auto exponent = source_double ? static_cast<unsigned>((raw >> 52) & 0x7ffU)
                                        : static_cast<unsigned>((raw >> 23) & 0xffU);
    const auto fraction = raw & (source_double ? 0x000fffffffffffffULL
                                             : 0x007fffffULL);
    const auto source_fraction_bits = source_double ? 52U : 23U;
    const auto source_exponent_max = source_double ? 0x7ffU : 0xffU;
    if (exponent == source_exponent_max) {
        if (fraction == 0) {
            if (alternative_half) {
                record_fp_exception(state, 0, 8); // No infinities in AHP.
                return sign | 0x7fffU;
            }
            return sign | 0x7c00U;
        }
        if (alternative_half) {
            record_fp_exception(state, 0, 8); // NaN converts to signed zero.
            return sign;
        }
        if ((fraction & (1ULL << (source_fraction_bits - 1))) == 0) {
            record_fp_exception(state, 0, 8); // IOC / IOE for signaling NaN.
        }
        if ((state.fpcr & (1U << 25)) != 0) return 0x7e00U;
        return sign | 0x7c00U |
               static_cast<std::uint16_t>(fraction >>
                                          (source_fraction_bits - 10)) | 0x0200U;
    }
    if (exponent == 0 && fraction == 0) return sign;
    if (exponent == 0 && (state.fpcr & (1U << 24)) != 0) {
        record_fp_exception(state, 7, 15); // IDC / IDE, FPCR.FZ input.
        return sign;
    }
    const auto significand = exponent == 0 ? fraction :
        (1ULL << source_fraction_bits) | fraction;
    const auto power = exponent == 0 ? (source_double ? -1074 : -149) :
        static_cast<int>(exponent) - (source_double ? 1075 : 150);
    const auto leading = static_cast<int>(std::bit_width(significand)) - 1;
    auto unbiased = power + leading;
    const auto rounding = (state.fpcr >> 22) & 3U;
    const bool negative = sign != 0;
    const auto round_up = [rounding, negative](std::uint64_t quotient,
                                               std::uint64_t remainder,
                                               unsigned shift) {
        if (remainder == 0) return false;
        if (rounding == 1) return !negative;
        if (rounding == 2) return negative;
        if (rounding == 3 || shift >= 64) return false;
        const auto halfway = 1ULL << (shift - 1);
        return remainder > halfway ||
               (remainder == halfway && (quotient & 1U) != 0);
    };
    const auto overflow = [&state, sign, rounding, negative,
                           alternative_half]() {
        if (alternative_half) {
            record_fp_exception(state, 0, 8); // AHP cannot encode exponent 32.
            return static_cast<std::uint16_t>(sign | 0x7fffU);
        }
        record_fp_exception(state, 2, 10); // OFC / OFE
        record_fp_exception(state, 4, 12); // IXC / IXE
        const bool infinity = rounding == 0 ||
                              (rounding == 1 && !negative) ||
                              (rounding == 2 && negative);
        return static_cast<std::uint16_t>(
            sign | (infinity ? 0x7c00U : 0x7bffU));
    };
    if (unbiased > (alternative_half ? 16 : 15)) return overflow();

    std::uint64_t quotient;
    std::uint64_t remainder = 0;
    unsigned shift = 0;
    if (unbiased >= -14) {
        const auto discard = leading - 10;
        if (discard > 0) {
            shift = static_cast<unsigned>(discard);
            quotient = significand >> shift;
            remainder = significand & ((1ULL << shift) - 1);
        } else {
            quotient = significand << -discard;
        }
        quotient += round_up(quotient, remainder, shift);
        if (quotient == (1ULL << 11)) {
            quotient >>= 1;
            ++unbiased;
        }
        if (unbiased > (alternative_half ? 16 : 15)) return overflow();
        if (remainder != 0) record_fp_exception(state, 4, 12); // IXC / IXE
        return sign | static_cast<std::uint16_t>((unbiased + 15) << 10) |
               static_cast<std::uint16_t>(quotient & 0x3ffU);
    }

    // Half subnormal units are 2^-24; a source double can need >64 shift bits.
    const auto discard = -24 - power;
    if (discard <= 0) {
        quotient = significand << -discard;
    } else {
        shift = static_cast<unsigned>(discard);
        quotient = shift >= 64 ? 0 : significand >> shift;
        remainder = shift >= 64 ? significand :
            significand & ((1ULL << shift) - 1);
    }
    quotient += round_up(quotient, remainder, shift);
    // FPConvert ignores FZ16 for a half-precision destination.
    if (remainder != 0 || (state.fpcr & (1U << 11)) != 0) {
        record_fp_exception(state, 3, 11); // UFC / UFE, pre-round tininess.
    }
    if (remainder != 0) record_fp_exception(state, 4, 12); // IXC / IXE
    return sign | static_cast<std::uint16_t>(quotient);
}

std::uint64_t integer_to_floating_bits(std::uint64_t raw, bool source_wide,
                                       bool unsigned_source, bool result_double,
                                       CpuState& state,
                                       unsigned fractional_bits = 0) {
    if (!source_wide) raw = static_cast<std::uint32_t>(raw);
    const bool negative = !unsigned_source &&
        (raw & (source_wide ? 1ULL << 63 : 1ULL << 31)) != 0;
    const auto magnitude = negative ?
        (~raw + 1ULL) & (source_wide ? UINT64_MAX : UINT32_MAX) : raw;
    const auto sign = negative ?
        (result_double ? 1ULL << 63 : 1ULL << 31) : 0;
    if (magnitude == 0) return 0;
    const auto fraction_bits = result_double ? 52U : 23U;
    const auto bias = result_double ? 1023U : 127U;
    const auto leading = std::bit_width(magnitude) - 1U;
    auto exponent = static_cast<int>(leading) -
                    static_cast<int>(fractional_bits);
    std::uint64_t significand;
    std::uint64_t discarded = 0;
    if (leading <= fraction_bits) {
        significand = magnitude << (fraction_bits - leading);
    } else {
        const auto shift = leading - fraction_bits;
        significand = magnitude >> shift;
        discarded = magnitude & ((1ULL << shift) - 1);
        if (discarded != 0) {
            const auto mode = (state.fpcr >> 22) & 3U;
            const auto halfway = 1ULL << (shift - 1);
            const bool increment = mode == 0
                ? discarded > halfway ||
                  (discarded == halfway && (significand & 1U) != 0)
                : mode == 1 ? !negative : mode == 2 && negative;
            significand += increment;
            if (significand == (1ULL << (fraction_bits + 1))) {
                significand >>= 1;
                ++exponent;
            }
            record_fp_exception(state, 4, 12); // IXC / IXE
        }
    }
    return sign | (static_cast<std::uint64_t>(exponent +
                                            static_cast<int>(bias)) <<
                   fraction_bits) |
           (significand & ((1ULL << fraction_bits) - 1));
}

std::uint64_t floating_to_integer_bits(std::uint64_t raw, bool input_double,
                                       bool result_wide, bool unsigned_result,
                                       IntegerRounding rounding, CpuState& state,
                                       unsigned fractional_bits = 0) {
    if (!input_double) raw = static_cast<std::uint32_t>(raw);
    const auto sign_bit = input_double ? 1ULL << 63 : 1ULL << 31;
    const auto exponent_bits = input_double ? 11U : 8U;
    const auto fraction_bits = input_double ? 52U : 23U;
    const auto exponent_mask = (1U << exponent_bits) - 1U;
    const auto fraction_mask = (1ULL << fraction_bits) - 1;
    const bool negative = (raw & sign_bit) != 0;
    const auto exponent = static_cast<unsigned>(
        (raw >> fraction_bits) & exponent_mask);
    const auto fraction = raw & fraction_mask;
    const auto result_mask = result_wide ? UINT64_MAX : UINT32_MAX;
    const auto saturated = [&state, negative, unsigned_result, result_wide,
                            result_mask]() -> std::uint64_t {
        record_fp_exception(state, 0, 8); // IOC / IOE
        if (unsigned_result) return negative ? 0 : result_mask;
        if (negative) return result_wide ? 1ULL << 63 : 1ULL << 31;
        return result_wide ? INT64_MAX : INT32_MAX;
    };
    if (exponent == exponent_mask) {
        if (fraction != 0) {
            record_fp_exception(state, 0, 8); // NaNs convert to integer zero.
            return 0;
        }
        return saturated();
    }
    if (exponent == 0 && fraction == 0) return 0;
    if (exponent == 0 && (state.fpcr & (1U << 24)) != 0) {
        record_fp_exception(state, 7, 15); // IDC / IDE, FPCR.FZ
        return 0;
    }

    const auto significand = exponent == 0 ? fraction :
        (1ULL << fraction_bits) | fraction;
    const auto bias = input_double ? 1023 : 127;
    const auto power = (exponent == 0 ? 1 : static_cast<int>(exponent)) -
                       bias - static_cast<int>(fraction_bits) +
                       static_cast<int>(fractional_bits);
    std::uint64_t integer = 0;
    std::uint64_t remainder = 0;
    unsigned shift = 0;
    if (power >= 0) {
        if (power >= 64 || significand > (UINT64_MAX >> power)) {
            return saturated();
        }
        integer = significand << power;
    } else {
        shift = static_cast<unsigned>(-power);
        integer = shift >= 64 ? 0 : significand >> shift;
        remainder = shift >= 64 ? significand :
            significand & ((1ULL << shift) - 1);
        if (remainder != 0) {
            const bool increment = rounding == IntegerRounding::positive
                ? !negative : rounding == IntegerRounding::negative
                ? negative : rounding == IntegerRounding::zero
                ? false : shift < 64 &&
                  (remainder > (1ULL << (shift - 1)) ||
                   (remainder == (1ULL << (shift - 1)) &&
                    (rounding == IntegerRounding::nearest_away ||
                     (integer & 1U) != 0)));
            integer += increment;
        }
    }
    const auto signed_limit = result_wide ? 1ULL << 63 : 1ULL << 31;
    if (unsigned_result) {
        if ((negative && integer != 0) || integer > result_mask) {
            return saturated();
        }
    } else if (negative ? integer > signed_limit :
               integer >= signed_limit) {
        return saturated();
    }
    if (remainder != 0) record_fp_exception(state, 4, 12); // IXC / IXE
    return negative ? (~integer + 1) & result_mask : integer;
}

std::uint64_t floating_square_root_bits(std::uint64_t raw, bool wide,
                                        CpuState& state) {
    if (!wide) raw = static_cast<std::uint32_t>(raw);
    const auto sign = wide ? 1ULL << 63 : 1ULL << 31;
    const auto fraction_bits = wide ? 52U : 23U;
    const auto exponent_bits = wide ? 11U : 8U;
    const auto exponent_mask = (1U << exponent_bits) - 1U;
    const auto fraction_mask = (1ULL << fraction_bits) - 1;
    const auto exponent = static_cast<unsigned>(
        (raw >> fraction_bits) & exponent_mask);
    const auto fraction = raw & fraction_mask;
    const auto default_nan = (static_cast<std::uint64_t>(exponent_mask) <<
                              fraction_bits) | (1ULL << (fraction_bits - 1));
    if (exponent == exponent_mask) {
        if (fraction != 0) {
            if ((fraction & (1ULL << (fraction_bits - 1))) == 0) {
                record_fp_exception(state, 0, 8); // IOC / IOE
            }
            return (state.fpcr & (1U << 25)) != 0 ? default_nan :
                raw | (1ULL << (fraction_bits - 1));
        }
        if ((raw & sign) == 0) return raw;
        record_fp_exception(state, 0, 8); // sqrt(-infinity)
        return default_nan;
    }
    if (exponent == 0 && fraction == 0) return raw; // Preserve -0.
    if (exponent == 0 && (state.fpcr & (1U << 24)) != 0) {
        record_fp_exception(state, 7, 15); // IDC / IDE
        return raw & sign;
    }
    if ((raw & sign) != 0) {
        record_fp_exception(state, 0, 8); // sqrt(negative finite value)
        return default_nan;
    }

    const auto significand = exponent == 0 ? fraction :
        (1ULL << fraction_bits) | fraction;
    const auto leading = static_cast<int>(std::bit_width(significand)) - 1;
    const auto bias = wide ? 1023 : 127;
    const auto power = (exponent == 0 ? 1 : static_cast<int>(exponent)) -
                       bias - static_cast<int>(fraction_bits);
    const auto unbiased = power + leading;
    auto result_exponent = unbiased >= 0 ? unbiased / 2 :
                           -((-unbiased + 1) / 2);
    const auto parity = unbiased - 2 * result_exponent;
    const auto shift = 2 * static_cast<int>(fraction_bits) - leading + parity;
    struct WideInteger { std::uint64_t high, low; };
    const WideInteger target = shift >= 64
        ? WideInteger{significand << (shift - 64), 0}
        : shift == 0 ? WideInteger{0, significand}
        : WideInteger{significand >> (64 - shift), significand << shift};
    const auto square = [](std::uint64_t value) {
        return WideInteger{unsigned_multiply_high(value, value),
                           value * value};
    };
    const auto less_equal = [](WideInteger lhs, WideInteger rhs) {
        return lhs.high < rhs.high ||
               (lhs.high == rhs.high && lhs.low <= rhs.low);
    };
    auto lower = 1ULL << fraction_bits;
    auto upper = 1ULL << (fraction_bits + 1);
    while (lower + 1 < upper) {
        const auto candidate = lower + (upper - lower) / 2;
        if (less_equal(square(candidate), target)) lower = candidate;
        else upper = candidate;
    }
    auto result_significand = lower;
    const auto squared = square(lower);
    const bool inexact = squared.high != target.high ||
                         squared.low != target.low;
    if (inexact) {
        const auto rounding = (state.fpcr >> 22) & 3U;
        const auto midpoint_low = squared.low + lower;
        const WideInteger midpoint{
            squared.high + (midpoint_low < squared.low), midpoint_low};
        if (rounding == 1 ||
            (rounding == 0 && !less_equal(target, midpoint))) {
            ++result_significand;
        }
        record_fp_exception(state, 4, 12); // IXC / IXE
    }
    if (result_significand == (1ULL << (fraction_bits + 1))) {
        result_significand >>= 1;
        ++result_exponent;
    }
    return (static_cast<std::uint64_t>(result_exponent + bias) <<
            fraction_bits) | (result_significand & fraction_mask);
}

std::uint64_t floating_multiply_bits(std::uint64_t lhs, std::uint64_t rhs,
                                     bool wide, CpuState& state) {
    if (!wide) {
        lhs = static_cast<std::uint32_t>(lhs);
        rhs = static_cast<std::uint32_t>(rhs);
    }
    const auto sign_mask = wide ? 1ULL << 63 : 1ULL << 31;
    const auto fraction_bits = wide ? 52U : 23U;
    const auto exponent_bits = wide ? 11U : 8U;
    const auto exponent_mask = (1U << exponent_bits) - 1U;
    const auto fraction_mask = (1ULL << fraction_bits) - 1;
    const auto quiet = 1ULL << (fraction_bits - 1);
    const auto infinity = static_cast<std::uint64_t>(exponent_mask) <<
                          fraction_bits;
    const auto default_nan = infinity | quiet;
    const auto sign = (lhs ^ rhs) & sign_mask;
    const auto flush_input = [&state, infinity, fraction_mask, sign_mask](
                                 std::uint64_t bits) {
        if ((state.fpcr & (1U << 24)) != 0 &&
            (bits & infinity) == 0 && (bits & fraction_mask) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE
            return bits & sign_mask;
        }
        return bits;
    };
    lhs = flush_input(lhs);
    rhs = flush_input(rhs);
    const auto left_exp = static_cast<unsigned>(
        (lhs >> fraction_bits) & exponent_mask);
    const auto right_exp = static_cast<unsigned>(
        (rhs >> fraction_bits) & exponent_mask);
    const auto left_frac = lhs & fraction_mask;
    const auto right_frac = rhs & fraction_mask;
    const bool left_nan = left_exp == exponent_mask && left_frac != 0;
    const bool right_nan = right_exp == exponent_mask && right_frac != 0;
    if (left_nan || right_nan) {
        const bool left_signaling = left_nan && (left_frac & quiet) == 0;
        const bool right_signaling = right_nan && (right_frac & quiet) == 0;
        if (left_signaling || right_signaling) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        if ((state.fpcr & (1U << 25)) != 0) return default_nan;
        return (left_signaling ? lhs : right_signaling ? rhs :
                left_nan ? lhs : rhs) | quiet;
    }
    const bool left_infinite = left_exp == exponent_mask;
    const bool right_infinite = right_exp == exponent_mask;
    const bool left_zero = left_exp == 0 && left_frac == 0;
    const bool right_zero = right_exp == 0 && right_frac == 0;
    if ((left_infinite && right_zero) ||
        (right_infinite && left_zero)) {
        record_fp_exception(state, 0, 8); // 0 * infinity
        return default_nan;
    }
    if (left_infinite || right_infinite) return sign | infinity;
    if (left_zero || right_zero) return sign;

    const auto left_sig = (left_exp == 0 ? 0 : 1ULL << fraction_bits) |
                          left_frac;
    const auto right_sig = (right_exp == 0 ? 0 : 1ULL << fraction_bits) |
                           right_frac;
    const auto bias = wide ? 1023 : 127;
    const auto power = (left_exp == 0 ? 1 : static_cast<int>(left_exp)) +
                       (right_exp == 0 ? 1 : static_cast<int>(right_exp)) -
                       2 * bias - 2 * static_cast<int>(fraction_bits);
    struct WideInteger { std::uint64_t high, low; };
    const WideInteger product{unsigned_multiply_high(left_sig, right_sig),
                              left_sig * right_sig};
    const auto highest = product.high != 0
        ? 64 + static_cast<int>(std::bit_width(product.high)) - 1
        : static_cast<int>(std::bit_width(product.low)) - 1;
    auto result_exponent = power + highest;
    const auto minimum_exponent = 1 - bias;
    const auto maximum_exponent = bias;
    const auto overflow = [&state, sign, infinity, fraction_mask,
                           fraction_bits]() {
        record_fp_exception(state, 2, 10); // OFC / OFE
        record_fp_exception(state, 4, 12); // IXC / IXE
        const auto mode = (state.fpcr >> 22) & 3U;
        const bool negative = sign != 0;
        const bool to_infinity = mode == 0 ||
            (mode == 1 && !negative) || (mode == 2 && negative);
        return sign | (to_infinity ? infinity :
                       (infinity - (1ULL << fraction_bits)) | fraction_mask);
    };
    if (result_exponent > maximum_exponent) return overflow();
    if (result_exponent < minimum_exponent &&
        (state.fpcr & (1U << 24)) != 0) {
        state.fpsr |= 1U << 3; // FPCR.FZ output flush never traps.
        return sign;
    }
    const auto bit_at = [product](unsigned bit) {
        return bit >= 128 ? false : bit >= 64
            ? ((product.high >> (bit - 64)) & 1U) != 0
            : ((product.low >> bit) & 1U) != 0;
    };
    const auto any_low = [product](unsigned count) {
        if (count == 0) return false;
        if (count >= 128) return product.high != 0 || product.low != 0;
        if (count > 64) {
            return product.low != 0 ||
                   (product.high & ((1ULL << (count - 64)) - 1)) != 0;
        }
        if (count == 64) return product.low != 0;
        return (product.low & ((1ULL << count) - 1)) != 0;
    };
    const auto shift_right = [product](unsigned shift) {
        if (shift >= 128) return 0ULL;
        if (shift >= 64) return product.high >> (shift - 64);
        if (shift == 0) return product.low;
        return (product.low >> shift) |
               (product.high << (64 - shift));
    };
    const auto discard = result_exponent >= minimum_exponent
        ? highest - static_cast<int>(fraction_bits)
        : minimum_exponent - static_cast<int>(fraction_bits) - power;
    auto significand = discard <= 0
        ? product.low << -discard
        : shift_right(static_cast<unsigned>(discard));
    const bool inexact = discard > 0 && any_low(static_cast<unsigned>(discard));
    if (inexact) {
        const auto mode = (state.fpcr >> 22) & 3U;
        const auto shift = static_cast<unsigned>(discard);
        const bool guard = bit_at(shift - 1);
        const bool sticky = any_low(shift - 1);
        const bool increment = mode == 0
            ? guard && (sticky || (significand & 1U) != 0)
            : mode == 1 ? sign == 0 : mode == 2 && sign != 0;
        significand += increment;
    }
    if (result_exponent >= minimum_exponent) {
        if (significand == (1ULL << (fraction_bits + 1))) {
            significand >>= 1;
            ++result_exponent;
        }
        if (result_exponent > maximum_exponent) return overflow();
        if (inexact) record_fp_exception(state, 4, 12); // IXC / IXE
        return sign | (static_cast<std::uint64_t>(result_exponent + bias) <<
                       fraction_bits) | (significand & fraction_mask);
    }
    if (inexact || (state.fpcr & (1U << 11)) != 0) {
        record_fp_exception(state, 3, 11); // UFC / UFE, pre-round tininess
    }
    if (inexact) record_fp_exception(state, 4, 12); // IXC / IXE
    return sign | significand; // Rounded 1<<fraction_bits is minimum normal.
}

std::uint64_t floating_add_sub_bits(std::uint64_t lhs, std::uint64_t rhs,
                                    bool wide, bool subtract, CpuState& state) {
    if (!wide) {
        lhs = static_cast<std::uint32_t>(lhs);
        rhs = static_cast<std::uint32_t>(rhs);
    }
    const auto fraction_bits = wide ? 52U : 23U;
    const auto exponent_mask = wide ? 0x7ffU : 0xffU;
    const auto fraction_mask = (1ULL << fraction_bits) - 1;
    const auto sign_mask = 1ULL << (fraction_bits + (wide ? 11U : 8U));
    const auto infinity = static_cast<std::uint64_t>(exponent_mask) <<
                          fraction_bits;
    const auto quiet = 1ULL << (fraction_bits - 1);
    const auto flush_input = [&state, infinity, fraction_mask, sign_mask](
                                 std::uint64_t bits) {
        if ((state.fpcr & (1U << 24)) != 0 &&
            (bits & infinity) == 0 && (bits & fraction_mask) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE
            return bits & sign_mask;
        }
        return bits;
    };
    lhs = flush_input(lhs);
    rhs = flush_input(rhs);
    const auto left_exp = static_cast<unsigned>(
        (lhs >> fraction_bits) & exponent_mask);
    const auto right_exp = static_cast<unsigned>(
        (rhs >> fraction_bits) & exponent_mask);
    const auto left_frac = lhs & fraction_mask;
    const auto right_frac = rhs & fraction_mask;
    const bool left_nan = left_exp == exponent_mask && left_frac != 0;
    const bool right_nan = right_exp == exponent_mask && right_frac != 0;
    if (left_nan || right_nan) {
        const bool left_signaling = left_nan && (left_frac & quiet) == 0;
        const bool right_signaling = right_nan && (right_frac & quiet) == 0;
        if (left_signaling || right_signaling) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        if ((state.fpcr & (1U << 25)) != 0) return infinity | quiet;
        return (left_signaling ? lhs : right_signaling ? rhs :
                left_nan ? lhs : rhs) | quiet;
    }
    const auto left_sign = lhs & sign_mask;
    const auto right_sign = (rhs & sign_mask) ^ (subtract ? sign_mask : 0);
    if (left_exp == exponent_mask || right_exp == exponent_mask) {
        if (left_exp == exponent_mask && right_exp == exponent_mask &&
            left_sign != right_sign) {
            record_fp_exception(state, 0, 8); // infinity - infinity
            return infinity | quiet;
        }
        return (left_exp == exponent_mask ? left_sign : right_sign) | infinity;
    }
    const auto left_sig = (left_exp == 0 ? 0 : 1ULL << fraction_bits) |
                          left_frac;
    const auto right_sig = (right_exp == 0 ? 0 : 1ULL << fraction_bits) |
                           right_frac;
    const auto rounding = (state.fpcr >> 22) & 3U;
    if (left_sig == 0 && right_sig == 0) {
        return left_sign == right_sign ? left_sign :
               rounding == 2 ? sign_mask : 0;
    }
    const auto bias = wide ? 1023 : 127;
    const auto minimum_exponent = 1 - bias;
    const auto maximum_exponent = bias;
    const auto left_power = left_exp == 0 ? minimum_exponent :
                            static_cast<int>(left_exp) - bias;
    const auto right_power = right_exp == 0 ? minimum_exponent :
                             static_cast<int>(right_exp) - bias;
    auto exponent = std::max(left_power, right_power);
    const auto shift_right_jam = [](std::uint64_t value, unsigned distance) {
        if (distance == 0) return value;
        if (distance >= 64) return value != 0 ? 1ULL : 0ULL;
        return (value >> distance) |
               static_cast<std::uint64_t>(
                   (value & ((1ULL << distance) - 1)) != 0);
    };
    const auto left_aligned = shift_right_jam(
        left_sig << 3, static_cast<unsigned>(exponent - left_power));
    const auto right_aligned = shift_right_jam(
        right_sig << 3, static_cast<unsigned>(exponent - right_power));
    std::uint64_t significand;
    std::uint64_t sign;
    if (left_sign == right_sign) {
        significand = left_aligned + right_aligned;
        sign = left_sign;
    } else if (left_aligned >= right_aligned) {
        significand = left_aligned - right_aligned;
        sign = left_sign;
    } else {
        significand = right_aligned - left_aligned;
        sign = right_sign;
    }
    if (significand == 0) return rounding == 2 ? sign_mask : 0;
    const auto normal_bit = 1ULL << (fraction_bits + 3);
    if (significand >= (normal_bit << 1)) {
        significand = shift_right_jam(significand, 1);
        ++exponent;
    }
    while (significand < normal_bit && exponent > minimum_exponent) {
        significand <<= 1;
        --exponent;
    }
    const auto overflow = [&state, sign, infinity, fraction_mask,
                           fraction_bits, rounding]() {
        record_fp_exception(state, 2, 10); // OFC / OFE
        record_fp_exception(state, 4, 12); // IXC / IXE
        const bool to_infinity = rounding == 0 ||
            (rounding == 1 && sign == 0) ||
            (rounding == 2 && sign != 0);
        return sign | (to_infinity ? infinity :
                       (infinity - (1ULL << fraction_bits)) | fraction_mask);
    };
    if (exponent > maximum_exponent) return overflow();
    const bool tiny = exponent == minimum_exponent &&
                      significand < normal_bit;
    if (tiny && (state.fpcr & (1U << 24)) != 0) {
        state.fpsr |= 1U << 3; // FPCR.FZ output flush never traps.
        return sign;
    }
    auto rounded = significand >> 3;
    const auto discarded = significand & 7U;
    if (discarded != 0) {
        const bool increment = rounding == 0
            ? discarded > 4 || (discarded == 4 && (rounded & 1U) != 0)
            : rounding == 1 ? sign == 0 : rounding == 2 && sign != 0;
        rounded += increment;
    }
    if (rounded == (1ULL << (fraction_bits + 1))) {
        rounded >>= 1;
        ++exponent;
    }
    if (exponent > maximum_exponent) return overflow();
    if (tiny && (discarded != 0 || (state.fpcr & (1U << 11)) != 0)) {
        record_fp_exception(state, 3, 11); // UFC / UFE, pre-round tininess
    }
    if (discarded != 0) record_fp_exception(state, 4, 12); // IXC / IXE
    const auto encoded_exponent = exponent == minimum_exponent &&
                                  rounded < (1ULL << fraction_bits)
        ? 0 : exponent + bias;
    return sign | (static_cast<std::uint64_t>(encoded_exponent) <<
                   fraction_bits) | (rounded & fraction_mask);
}

std::uint64_t floating_divide_bits(std::uint64_t lhs, std::uint64_t rhs,
                                   bool wide, CpuState& state) {
    if (!wide) {
        lhs = static_cast<std::uint32_t>(lhs);
        rhs = static_cast<std::uint32_t>(rhs);
    }
    const auto fraction_bits = wide ? 52U : 23U;
    const auto exponent_mask = wide ? 0x7ffU : 0xffU;
    const auto fraction_mask = (1ULL << fraction_bits) - 1;
    const auto sign_mask = 1ULL << (fraction_bits + (wide ? 11U : 8U));
    const auto infinity = static_cast<std::uint64_t>(exponent_mask) <<
                          fraction_bits;
    const auto quiet = 1ULL << (fraction_bits - 1);
    const auto flush_input = [&state, infinity, fraction_mask, sign_mask](
                                 std::uint64_t bits) {
        if ((state.fpcr & (1U << 24)) != 0 &&
            (bits & infinity) == 0 && (bits & fraction_mask) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE
            return bits & sign_mask;
        }
        return bits;
    };
    lhs = flush_input(lhs);
    rhs = flush_input(rhs);
    const auto left_exp = static_cast<unsigned>(
        (lhs >> fraction_bits) & exponent_mask);
    const auto right_exp = static_cast<unsigned>(
        (rhs >> fraction_bits) & exponent_mask);
    const auto left_frac = lhs & fraction_mask;
    const auto right_frac = rhs & fraction_mask;
    const bool left_nan = left_exp == exponent_mask && left_frac != 0;
    const bool right_nan = right_exp == exponent_mask && right_frac != 0;
    if (left_nan || right_nan) {
        const bool left_signaling = left_nan && (left_frac & quiet) == 0;
        const bool right_signaling = right_nan && (right_frac & quiet) == 0;
        if (left_signaling || right_signaling) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        if ((state.fpcr & (1U << 25)) != 0) return infinity | quiet;
        return (left_signaling ? lhs : right_signaling ? rhs :
                left_nan ? lhs : rhs) | quiet;
    }
    const auto sign = (lhs ^ rhs) & sign_mask;
    const bool left_infinite = left_exp == exponent_mask;
    const bool right_infinite = right_exp == exponent_mask;
    const bool left_zero = left_exp == 0 && left_frac == 0;
    const bool right_zero = right_exp == 0 && right_frac == 0;
    if ((left_infinite && right_infinite) || (left_zero && right_zero)) {
        record_fp_exception(state, 0, 8); // infinity/infinity or zero/zero
        return infinity | quiet;
    }
    if (left_infinite || right_zero) {
        if (!left_infinite) record_fp_exception(state, 1, 9); // DZC / DZE
        return sign | infinity;
    }
    if (left_zero || right_infinite) return sign;

    const auto bias = wide ? 1023 : 127;
    const auto minimum_exponent = 1 - bias;
    const auto maximum_exponent = bias;
    auto left_power = left_exp == 0 ? minimum_exponent :
                      static_cast<int>(left_exp) - bias;
    auto right_power = right_exp == 0 ? minimum_exponent :
                       static_cast<int>(right_exp) - bias;
    auto left_sig = (left_exp == 0 ? 0 : 1ULL << fraction_bits) | left_frac;
    auto right_sig = (right_exp == 0 ? 0 : 1ULL << fraction_bits) | right_frac;
    const auto leading_bit = 1ULL << fraction_bits;
    while (left_sig < leading_bit) {
        left_sig <<= 1;
        --left_power;
    }
    while (right_sig < leading_bit) {
        right_sig <<= 1;
        --right_power;
    }
    auto exponent = left_power - right_power;
    if (left_sig < right_sig) {
        left_sig <<= 1;
        --exponent;
    }
    // Long division produces the significand and three rounding bits. The
    // nonzero remainder is folded into the sticky bit without host FP math.
    auto quotient = 1ULL;
    auto remainder = left_sig - right_sig;
    for (unsigned bit = 0; bit < fraction_bits + 3; ++bit) {
        remainder <<= 1;
        quotient <<= 1;
        if (remainder >= right_sig) {
            remainder -= right_sig;
            quotient |= 1;
        }
    }
    if (remainder != 0) quotient |= 1;
    const auto rounding = (state.fpcr >> 22) & 3U;
    const auto overflow = [&state, sign, infinity, fraction_mask,
                           fraction_bits, rounding]() {
        record_fp_exception(state, 2, 10); // OFC / OFE
        record_fp_exception(state, 4, 12); // IXC / IXE
        const bool to_infinity = rounding == 0 ||
            (rounding == 1 && sign == 0) ||
            (rounding == 2 && sign != 0);
        return sign | (to_infinity ? infinity :
                       (infinity - (1ULL << fraction_bits)) | fraction_mask);
    };
    if (exponent > maximum_exponent) return overflow();
    const auto shift_right_jam = [](std::uint64_t value, unsigned distance) {
        if (distance == 0) return value;
        if (distance >= 64) return value != 0 ? 1ULL : 0ULL;
        return (value >> distance) |
               static_cast<std::uint64_t>(
                   (value & ((1ULL << distance) - 1)) != 0);
    };
    if (exponent < minimum_exponent) {
        quotient = shift_right_jam(
            quotient, static_cast<unsigned>(minimum_exponent - exponent));
        exponent = minimum_exponent;
    }
    const bool tiny = quotient < (1ULL << (fraction_bits + 3));
    if (tiny && (state.fpcr & (1U << 24)) != 0) {
        state.fpsr |= 1U << 3; // FPCR.FZ output flush never traps.
        return sign;
    }
    auto rounded = quotient >> 3;
    const auto discarded = quotient & 7U;
    if (discarded != 0) {
        const bool increment = rounding == 0
            ? discarded > 4 || (discarded == 4 && (rounded & 1U) != 0)
            : rounding == 1 ? sign == 0 : rounding == 2 && sign != 0;
        rounded += increment;
    }
    if (rounded == (1ULL << (fraction_bits + 1))) {
        rounded >>= 1;
        ++exponent;
    }
    if (exponent > maximum_exponent) return overflow();
    if (tiny && (discarded != 0 || (state.fpcr & (1U << 11)) != 0)) {
        record_fp_exception(state, 3, 11); // UFC / UFE, pre-round tininess
    }
    if (discarded != 0) record_fp_exception(state, 4, 12); // IXC / IXE
    const auto encoded_exponent = exponent == minimum_exponent &&
                                  rounded < leading_bit
        ? 0 : exponent + bias;
    return sign | (static_cast<std::uint64_t>(encoded_exponent) <<
                   fraction_bits) | (rounded & fraction_mask);
}

struct WideFpInteger {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

WideFpInteger wide_shift_left(WideFpInteger value, unsigned shift) {
    if (shift == 0) return value;
    if (shift >= 128) return {};
    if (shift >= 64) return {value.low << (shift - 64), 0};
    return {(value.high << shift) | (value.low >> (64 - shift)),
            value.low << shift};
}

WideFpInteger wide_shift_right_jam(WideFpInteger value, unsigned shift) {
    if (shift == 0) return value;
    if (shift >= 128) return {0, (value.high | value.low) != 0};
    if (shift >= 64) {
        const auto distance = shift - 64;
        const auto discarded = value.low != 0 ||
            (distance != 0 &&
             (value.high & ((1ULL << distance) - 1)) != 0);
        return {0, (value.high >> distance) |
                   static_cast<std::uint64_t>(discarded)};
    }
    const auto discarded = (value.low & ((1ULL << shift) - 1)) != 0;
    return {value.high >> shift,
            (value.low >> shift) | (value.high << (64 - shift)) |
            static_cast<std::uint64_t>(discarded)};
}

std::uint64_t wide_shift_right_low(WideFpInteger value, unsigned shift) {
    if (shift >= 128) return 0;
    if (shift >= 64) return value.high >> (shift - 64);
    if (shift == 0) return value.low;
    return (value.low >> shift) | (value.high << (64 - shift));
}

bool wide_less(WideFpInteger left, WideFpInteger right) {
    return left.high != right.high ? left.high < right.high :
           left.low < right.low;
}

WideFpInteger wide_add(WideFpInteger left, WideFpInteger right) {
    const auto low = left.low + right.low;
    return {left.high + right.high + (low < left.low), low};
}

WideFpInteger wide_subtract(WideFpInteger left, WideFpInteger right) {
    return {left.high - right.high - (left.low < right.low),
            left.low - right.low};
}

std::uint64_t floating_multiply_add_bits(std::uint64_t addend,
                                         std::uint64_t lhs,
                                         std::uint64_t rhs, bool wide,
                                         bool negate_product,
                                         bool negate_addend,
                                         CpuState& state,
                                         bool halve_result = false) {
    if (!wide) {
        addend = static_cast<std::uint32_t>(addend);
        lhs = static_cast<std::uint32_t>(lhs);
        rhs = static_cast<std::uint32_t>(rhs);
    }
    const auto fraction_bits = wide ? 52U : 23U;
    const auto exponent_mask = wide ? 0x7ffU : 0xffU;
    const auto fraction_mask = (1ULL << fraction_bits) - 1;
    const auto sign_mask = 1ULL << (fraction_bits + (wide ? 11U : 8U));
    const auto infinity = static_cast<std::uint64_t>(exponent_mask) <<
                          fraction_bits;
    const auto quiet = 1ULL << (fraction_bits - 1);
    const auto flush_input = [&state, infinity, fraction_mask, sign_mask](
                                 std::uint64_t bits) {
        if ((state.fpcr & (1U << 24)) != 0 &&
            (bits & infinity) == 0 && (bits & fraction_mask) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE
            return bits & sign_mask;
        }
        return bits;
    };
    addend = flush_input(addend);
    lhs = flush_input(lhs);
    rhs = flush_input(rhs);
    const auto exponent_of = [fraction_bits, exponent_mask](
                                 std::uint64_t bits) {
        return static_cast<unsigned>((bits >> fraction_bits) & exponent_mask);
    };
    const auto add_exp = exponent_of(addend);
    const auto left_exp = exponent_of(lhs);
    const auto right_exp = exponent_of(rhs);
    const auto add_frac = addend & fraction_mask;
    const auto left_frac = lhs & fraction_mask;
    const auto right_frac = rhs & fraction_mask;
    const bool add_nan = add_exp == exponent_mask && add_frac != 0;
    const bool left_nan = left_exp == exponent_mask && left_frac != 0;
    const bool right_nan = right_exp == exponent_mask && right_frac != 0;
    const bool product_invalid =
        (left_exp == exponent_mask && left_frac == 0 &&
         right_exp == 0 && right_frac == 0) ||
        (right_exp == exponent_mask && right_frac == 0 &&
         left_exp == 0 && left_frac == 0);
    if (add_nan || left_nan || right_nan) {
        const bool add_signaling = add_nan && (add_frac & quiet) == 0;
        const bool left_signaling = left_nan && (left_frac & quiet) == 0;
        const bool right_signaling = right_nan && (right_frac & quiet) == 0;
        if (add_signaling || left_signaling || right_signaling ||
            (add_nan && !add_signaling && product_invalid)) {
            record_fp_exception(state, 0, 8); // IOC / IOE
        }
        if ((state.fpcr & (1U << 25)) != 0 ||
            (add_nan && !add_signaling && product_invalid)) {
            return infinity | quiet;
        }
        return (add_signaling ? addend : left_signaling ? lhs :
                right_signaling ? rhs : add_nan ? addend :
                left_nan ? lhs : rhs) | quiet;
    }
    const auto product_sign = ((lhs ^ rhs) & sign_mask) ^
                              (negate_product ? sign_mask : 0);
    const auto add_sign = (addend & sign_mask) ^
                          (negate_addend ? sign_mask : 0);
    const bool product_infinite = left_exp == exponent_mask ||
                                  right_exp == exponent_mask;
    const bool add_infinite = add_exp == exponent_mask;
    if (product_invalid ||
        (product_infinite && add_infinite && product_sign != add_sign)) {
        record_fp_exception(state, 0, 8); // Invalid fused operation.
        return infinity | quiet;
    }
    if (product_infinite || add_infinite) {
        return (product_infinite ? product_sign : add_sign) | infinity;
    }
    const auto left_sig = (left_exp == 0 ? 0 : 1ULL << fraction_bits) |
                          left_frac;
    const auto right_sig = (right_exp == 0 ? 0 : 1ULL << fraction_bits) |
                           right_frac;
    const auto add_sig = (add_exp == 0 ? 0 : 1ULL << fraction_bits) |
                         add_frac;
    const WideFpInteger product{unsigned_multiply_high(left_sig, right_sig),
                                left_sig * right_sig};
    const bool product_zero = product.high == 0 && product.low == 0;
    const auto rounding = (state.fpcr >> 22) & 3U;
    if (product_zero && add_sig == 0) {
        return product_sign == add_sign ? product_sign :
               rounding == 2 ? sign_mask : 0;
    }
    const auto bias = wide ? 1023 : 127;
    const auto minimum_exponent = 1 - bias;
    const auto maximum_exponent = bias;
    const auto left_power = (left_exp == 0 ? 1 : static_cast<int>(left_exp)) -
                            bias - static_cast<int>(fraction_bits);
    const auto right_power = (right_exp == 0 ? 1 : static_cast<int>(right_exp)) -
                             bias - static_cast<int>(fraction_bits);
    const auto add_power = (add_exp == 0 ? 1 : static_cast<int>(add_exp)) -
                           bias - static_cast<int>(fraction_bits);
    const auto product_highest = product.high != 0
        ? 64 + static_cast<int>(std::bit_width(product.high)) - 1
        : product_zero ? -1 : static_cast<int>(std::bit_width(product.low)) - 1;
    const auto add_highest = add_sig == 0 ? -1 :
                             static_cast<int>(std::bit_width(add_sig)) - 1;
    const auto product_top = product_zero ? -100000 :
                             left_power + right_power + product_highest;
    const auto add_top = add_sig == 0 ? -100000 :
                         add_power + add_highest;
    const auto common_top = std::max(product_top, add_top);
    constexpr unsigned alignment_bit = 110;
    // Binary64 products use <=106 bits. Five low alignment bits remain, and
    // cancellation requires top exponents within one, where alignment is exact.
    const auto align = [common_top, alignment_bit](WideFpInteger value,
                                                   int highest, int top) {
        if (highest < 0) return WideFpInteger{};
        value = wide_shift_left(value, alignment_bit -
                               static_cast<unsigned>(highest));
        return wide_shift_right_jam(
            value, static_cast<unsigned>(common_top - top));
    };
    const auto product_aligned = align(product, product_highest, product_top);
    const auto add_aligned = align({0, add_sig}, add_highest, add_top);
    WideFpInteger sum;
    std::uint64_t sign;
    if (product_sign == add_sign) {
        sum = wide_add(product_aligned, add_aligned);
        sign = product_sign;
    } else if (wide_less(product_aligned, add_aligned)) {
        sum = wide_subtract(add_aligned, product_aligned);
        sign = add_sign;
    } else {
        sum = wide_subtract(product_aligned, add_aligned);
        sign = product_sign;
    }
    if (sum.high == 0 && sum.low == 0) {
        return rounding == 2 ? sign_mask : 0;
    }
    const auto highest = sum.high != 0
        ? 64 + static_cast<int>(std::bit_width(sum.high)) - 1
        : static_cast<int>(std::bit_width(sum.low)) - 1;
    const auto result_top = common_top - (halve_result ? 1 : 0);
    auto exponent = result_top + highest - static_cast<int>(alignment_bit);
    const auto overflow = [&state, sign, infinity, fraction_mask,
                           fraction_bits, rounding]() {
        record_fp_exception(state, 2, 10); // OFC / OFE
        record_fp_exception(state, 4, 12); // IXC / IXE
        const bool to_infinity = rounding == 0 ||
            (rounding == 1 && sign == 0) ||
            (rounding == 2 && sign != 0);
        return sign | (to_infinity ? infinity :
                       (infinity - (1ULL << fraction_bits)) | fraction_mask);
    };
    if (exponent > maximum_exponent) return overflow();
    const auto discard = exponent >= minimum_exponent
        ? highest - static_cast<int>(fraction_bits)
        : minimum_exponent - static_cast<int>(fraction_bits) -
          (result_top - static_cast<int>(alignment_bit));
    const bool tiny = exponent < minimum_exponent;
    if (tiny && (state.fpcr & (1U << 24)) != 0) {
        state.fpsr |= 1U << 3; // FPCR.FZ output flush never traps.
        return sign;
    }
    const auto bit_at = [sum](unsigned bit) {
        return bit >= 128 ? false : bit >= 64
            ? ((sum.high >> (bit - 64)) & 1U) != 0
            : ((sum.low >> bit) & 1U) != 0;
    };
    const auto any_low = [sum](unsigned count) {
        if (count == 0) return false;
        if (count >= 128) return sum.high != 0 || sum.low != 0;
        if (count > 64) return sum.low != 0 ||
            (sum.high & ((1ULL << (count - 64)) - 1)) != 0;
        if (count == 64) return sum.low != 0;
        return (sum.low & ((1ULL << count) - 1)) != 0;
    };
    auto significand = discard <= 0
        ? sum.low << -discard : wide_shift_right_low(
            sum, static_cast<unsigned>(discard));
    const bool inexact = discard > 0 && any_low(static_cast<unsigned>(discard));
    if (inexact) {
        const auto shift = static_cast<unsigned>(discard);
        const bool guard = bit_at(shift - 1);
        const bool sticky = any_low(shift - 1);
        const bool increment = rounding == 0
            ? guard && (sticky || (significand & 1U) != 0)
            : rounding == 1 ? sign == 0 : rounding == 2 && sign != 0;
        significand += increment;
    }
    if (!tiny && significand == (1ULL << (fraction_bits + 1))) {
        significand >>= 1;
        ++exponent;
    }
    if (exponent > maximum_exponent) return overflow();
    if (tiny && (inexact || (state.fpcr & (1U << 11)) != 0)) {
        record_fp_exception(state, 3, 11); // UFC / UFE, pre-round tininess
    }
    if (inexact) record_fp_exception(state, 4, 12); // IXC / IXE
    const auto encoded_exponent = tiny
        ? (significand < (1ULL << fraction_bits) ? 0 : 1)
        : exponent + bias;
    return sign | (static_cast<std::uint64_t>(encoded_exponent) <<
                   fraction_bits) | (significand & fraction_mask);
}

std::uint64_t floating_reciprocal_step_bits(std::uint64_t lhs,
                                            std::uint64_t rhs, bool wide,
                                            bool square_root,
                                            CpuState& state) {
    if (!wide) {
        lhs = static_cast<std::uint32_t>(lhs);
        rhs = static_cast<std::uint32_t>(rhs);
    }
    const auto sign_mask = wide ? 1ULL << 63 : 1ULL << 31;
    const auto infinity = wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
    const auto fraction = wide ? 0x000fffffffffffffULL : 0x007fffffULL;
    const auto flush_input = [&state, sign_mask, infinity, fraction](
                                 std::uint64_t bits) {
        if ((state.fpcr & (1U << 24)) != 0 &&
            (bits & infinity) == 0 && (bits & fraction) != 0) {
            record_fp_exception(state, 7, 15); // IDC / IDE
            return bits & sign_mask;
        }
        return bits;
    };
    lhs = flush_input(lhs);
    rhs = flush_input(rhs);
    const auto nan = [infinity, fraction](std::uint64_t bits) {
        return (bits & infinity) == infinity && (bits & fraction) != 0;
    };
    const auto zero = [sign_mask](std::uint64_t bits) {
        return (bits & ~sign_mask) == 0;
    };
    const auto infinite = [infinity, sign_mask](std::uint64_t bits) {
        return (bits & ~sign_mask) == infinity;
    };
    if (!nan(lhs) && !nan(rhs) &&
        ((zero(lhs) && infinite(rhs)) ||
         (infinite(lhs) && zero(rhs)))) {
        return square_root
            ? (wide ? 0x3ff8000000000000ULL : 0x3fc00000ULL)
            : (wide ? 0x4000000000000000ULL : 0x40000000ULL);
    }
    const auto addend = square_root
        ? (wide ? 0x4008000000000000ULL : 0x40400000ULL)
        : (wide ? 0x4000000000000000ULL : 0x40000000ULL);
    // AArch64 reciprocal steps fuse the product and subtraction. Halving the
    // square-root step shifts the exact result before its one rounding step.
    return floating_multiply_add_bits(addend, lhs, rhs, wide, true, false,
                                      state, square_root);
}

unsigned reciprocal_estimate(unsigned scaled) {
    // Arm's baseline 9-bit fixed-point estimate, not host division by a float.
    const auto rounded = scaled * 2U + 1U;
    const auto quotient = (1U << 19) / rounded;
    return (quotient + 1U) / 2U;
}

unsigned reciprocal_sqrt_estimate(unsigned scaled) {
    // The baseline ISA defines this integer search for each 9-bit input.
    static const auto estimates = [] {
        std::array<unsigned, 512> result{};
        for (unsigned input = 128; input < result.size(); ++input) {
            const auto rounded = input < 256 ? input * 2U + 1U
                                             : ((input & ~1U) + 1U) * 2U;
            unsigned trial = 512;
            while (static_cast<std::uint64_t>(rounded) * (trial + 1U) *
                       (trial + 1U) < (1ULL << 28)) {
                ++trial;
            }
            result[input] = (trial + 1U) / 2U;
        }
        return result;
    }();
    return estimates[scaled];
}

std::uint64_t floating_reciprocal_estimate(std::uint64_t raw, bool wide,
                                           bool square_root, CpuState& state) {
    const auto fraction_bits = wide ? 52U : 23U;
    const auto exponent_bits = wide ? 11U : 8U;
    const auto exponent_mask = (1U << exponent_bits) - 1U;
    const auto fraction_mask = (1ULL << fraction_bits) - 1ULL;
    const auto sign_mask = 1ULL << (fraction_bits + exponent_bits);
    const auto sign = raw & sign_mask;
    const auto exponent = static_cast<unsigned>((raw >> fraction_bits) &
                                                exponent_mask);
    const auto fraction = raw & fraction_mask;
    const auto infinity = sign | (static_cast<std::uint64_t>(exponent_mask) <<
                                  fraction_bits);
    const auto default_nan = (static_cast<std::uint64_t>(exponent_mask) <<
                              fraction_bits) | (1ULL << (fraction_bits - 1));
    if (exponent == exponent_mask && fraction != 0) {
        const bool signaling = (fraction & (1ULL << (fraction_bits - 1))) == 0;
        if (signaling) record_fp_exception(state, 0, 8); // IOC / IOE
        return (state.fpcr & (1U << 25)) != 0
                   ? default_nan
                   : raw | (1ULL << (fraction_bits - 1));
    }
    if (exponent == 0 && fraction != 0 &&
        (state.fpcr & (1U << 24)) != 0) {
        record_fp_exception(state, 7, 15); // IDC / IDE
        record_fp_exception(state, 1, 9);  // DZC / DZE
        return infinity;
    }
    if (exponent == 0 && fraction == 0) {
        record_fp_exception(state, 1, 9); // DZC / DZE
        return infinity;
    }
    if (square_root) {
        if (sign != 0) {
            record_fp_exception(state, 0, 8); // IOC / IOE
            return default_nan;
        }
        if (exponent == exponent_mask) return 0;
        auto normalized_fraction = fraction << (52U - fraction_bits);
        int normalized_exponent = static_cast<int>(exponent);
        if (normalized_exponent == 0) {
            while ((normalized_fraction & (1ULL << 51)) == 0) {
                normalized_fraction = (normalized_fraction << 1) & ((1ULL << 52) - 1);
                --normalized_exponent;
            }
            normalized_fraction = (normalized_fraction << 1) & ((1ULL << 52) - 1);
        }
        const auto scaled = (normalized_exponent & 1) == 0
            ? 256U | static_cast<unsigned>(normalized_fraction >> 44)
            : 128U | static_cast<unsigned>(normalized_fraction >> 45);
        const auto estimated = reciprocal_sqrt_estimate(scaled);
        const auto result_exponent = static_cast<unsigned>(
            ((wide ? 3068 : 380) - normalized_exponent) / 2);
        return (static_cast<std::uint64_t>(result_exponent) << fraction_bits) |
               (static_cast<std::uint64_t>(estimated & 255U) <<
                (fraction_bits - 8));
    }
    if (exponent == exponent_mask) return sign;
    const auto overflow_fraction = wide ? (1ULL << 50) : (1ULL << 21);
    if (exponent == 0 && fraction < overflow_fraction) {
        const auto rounding = (state.fpcr >> 22) & 3U;
        const bool to_infinity = rounding == 0 ||
            (rounding == 1 && sign == 0) ||
            (rounding == 2 && sign != 0);
        record_fp_exception(state, 2, 10); // OFC / OFE
        record_fp_exception(state, 4, 12); // IXC / IXE
        return to_infinity ? infinity
                           : sign | (static_cast<std::uint64_t>(exponent_mask - 1U) <<
                                     fraction_bits) | fraction_mask;
    }
    if ((state.fpcr & (1U << 24)) != 0 &&
        exponent >= (wide ? 2045U : 253U)) {
        state.fpsr |= 1U << 3; // Flush-to-zero records UFC without trapping.
        return sign;
    }
    auto normalized_fraction = fraction << (52U - fraction_bits);
    int normalized_exponent = static_cast<int>(exponent);
    if (normalized_exponent == 0) {
        if ((normalized_fraction & (1ULL << 51)) == 0) {
            normalized_exponent = -1;
            normalized_fraction = (normalized_fraction << 2) & ((1ULL << 52) - 1);
        } else {
            normalized_fraction = (normalized_fraction << 1) & ((1ULL << 52) - 1);
        }
    }
    const auto scaled = 256U | static_cast<unsigned>(normalized_fraction >> 44);
    const auto estimated = reciprocal_estimate(scaled);
    int result_exponent = (wide ? 2045 : 253) - normalized_exponent;
    auto result_fraction = static_cast<std::uint64_t>(estimated & 255U) << 44;
    if (result_exponent == 0) {
        result_fraction = (1ULL << 51) | (result_fraction >> 1);
    } else if (result_exponent == -1) {
        result_fraction = (1ULL << 50) | (result_fraction >> 2);
        result_exponent = 0;
    }
    return sign | (static_cast<std::uint64_t>(result_exponent) << fraction_bits) |
           (result_fraction >> (52U - fraction_bits));
}

} // namespace

std::uint32_t GuestMemory::read32(std::uint64_t address) const {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result |= static_cast<std::uint32_t>(read8(address + i)) << (8 * i);
    }
    return result;
}

std::uint64_t GuestMemory::read64(std::uint64_t address) const {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        result |= static_cast<std::uint64_t>(read8(address + i)) << (8 * i);
    }
    return result;
}

void GuestMemory::reserve_memory(std::uint64_t, std::uint64_t) {
    throw std::runtime_error("guest memory reservations are unsupported");
}

void GuestMemory::write32(std::uint64_t address, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        write8(address + i, static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void GuestMemory::write64(std::uint64_t address, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        write8(address + i, static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void GuestMemory::write_scalar_fields(std::span<const GuestScalarWrite> fields) {
    for (const auto& field : fields) {
        switch (field.width) {
        case 1:
            write8(field.address, static_cast<std::uint8_t>(field.value));
            break;
        case 2:
            write8(field.address, static_cast<std::uint8_t>(field.value));
            write8(field.address + 1, static_cast<std::uint8_t>(field.value >> 8));
            break;
        case 4:
            write32(field.address, static_cast<std::uint32_t>(field.value));
            break;
        case 8:
            write64(field.address, field.value);
            break;
        default:
            throw std::runtime_error("invalid guest scalar write width");
        }
    }
}

std::optional<std::uint64_t> GuestMemory::try_read_mapped(
    std::uint64_t, unsigned) const {
    return std::nullopt;
}

bool GuestMemory::try_write_mapped(std::uint64_t, unsigned,
                                   std::uint64_t) {
    return false;
}

std::optional<std::uint64_t> GuestMemory::try_read_contiguous(
    std::uint64_t address, unsigned bytes) const {
    return try_read_mapped(address, bytes);
}

bool GuestMemory::try_write_contiguous(std::uint64_t address, unsigned bytes,
                                       std::uint64_t value) {
    return try_write_mapped(address, bytes, value);
}

std::optional<std::uint64_t> GuestMemory::write_revision(
    std::uint64_t, std::uint64_t) const {
    return std::nullopt;
}

void GuestMemory::read_bytes(std::uint64_t address, std::span<std::uint8_t> bytes) const {
    for (std::size_t index = 0; index < bytes.size();) {
        if (bytes.size() - index >= 8) {
            if (const auto word = try_read_contiguous(address + index, 8)) {
                for (unsigned part = 0; part < 8; ++part)
                    bytes[index + part] = static_cast<std::uint8_t>(*word >> (8 * part));
                index += 8;
                continue;
            }
        }
        bytes[index] = read8(address + index);
        ++index;
    }
}

void GuestMemory::write_bytes(std::uint64_t address, std::span<const std::uint8_t> bytes) {
    for (std::size_t index = 0; index < bytes.size();) {
        if (bytes.size() - index >= 8) {
            std::uint64_t word{};
            for (unsigned part = 0; part < 8; ++part)
                word |= static_cast<std::uint64_t>(bytes[index + part]) << (8 * part);
            if (try_write_contiguous(address + index, 8, word)) {
                index += 8;
                continue;
            }
        }
        write8(address + index, bytes[index]);
        ++index;
    }
}

void GuestMemory::fill_bytes(std::uint64_t address, std::uint8_t value, std::uint64_t count) {
    const auto word = 0x0101010101010101ULL * value;
    for (std::uint64_t index = 0; index < count;) {
        if (count - index >= 8 && try_write_contiguous(address + index, 8, word)) {
            index += 8;
        } else {
            write8(address + index, value);
            ++index;
        }
    }
}

void GuestMemory::copy_bytes(std::uint64_t destination, std::uint64_t source,
                              std::uint64_t count, bool move) {
    const bool backward = move && destination > source && destination - source < count;
    const bool chunks = backward || destination <= source || destination - source >= 8;
    for (std::uint64_t completed = 0; completed < count;) {
        if (chunks && count - completed >= 8) {
            const auto offset = backward ? count - completed - 8 : completed;
            if (const auto word = try_read_contiguous(source + offset, 8)) {
                if (try_write_contiguous(destination + offset, 8, *word)) {
                    completed += 8;
                    continue;
                }
            }
        }
        const auto offset = backward ? count - completed - 1 : completed;
        write8(destination + offset, read8(source + offset));
        ++completed;
    }
}

std::vector<std::uint8_t> GuestMemory::read_string(
    std::uint64_t address, unsigned character_bytes, std::size_t maximum_characters,
    bool* terminated) const {
    if (character_bytes != 1 && character_bytes != 2)
        throw std::runtime_error("invalid guest string character width");
    if (maximum_characters > SIZE_MAX / character_bytes)
        throw std::runtime_error("guest string span is too large");
    if (terminated) *terminated = false;
    std::vector<std::uint8_t> result;
    for (std::size_t index = 0; index < maximum_characters; ++index) {
        const auto low = read8(address + index * character_bytes);
        const auto high = character_bytes == 2 ? read8(address + index * 2 + 1) : 0;
        if (low == 0 && high == 0) {
            if (terminated) *terminated = true;
            return result;
        }
        result.push_back(low);
        if (character_bytes == 2) result.push_back(static_cast<std::uint8_t>(high));
    }
    return result;
}

std::size_t GuestMemory::string_length(std::uint64_t address, unsigned character_bytes,
    std::size_t maximum_characters, bool* terminated) const {
    if (character_bytes != 1 && character_bytes != 2)
        throw std::runtime_error("invalid guest string character width");
    if (maximum_characters > SIZE_MAX / character_bytes)
        throw std::runtime_error("guest string span is too large");
    if (terminated) *terminated = false;
    for (std::size_t index = 0; index < maximum_characters; ++index) {
        const auto low = read8(address + index * character_bytes);
        const auto high = character_bytes == 2 ? read8(address + index * 2 + 1) : 0;
        if (low == 0 && high == 0) {
            if (terminated) *terminated = true;
            return index;
        }
    }
    return maximum_characters;
}

NativeReadLease* GuestMemory::native_read_lease() { return nullptr; }
NativeWriteLease* GuestMemory::native_write_lease() { return nullptr; }

ReferenceEngine::SharedAddressSpace::SharedAddressSpace()
    : execution_gate(std::make_unique<ExecutionGate>()),
      topology_history(std::make_unique<NativeTopologyHistory>()) {}
ReferenceEngine::SharedAddressSpace::~SharedAddressSpace() = default;

class ReferenceEngine::AddressSpaceLock final {
public:
    explicit AddressSpaceLock(SharedAddressSpace& space)
        : execution_(*space.execution_gate), mutex_(space.mutex) {}
    AddressSpaceLock(const AddressSpaceLock&) = delete;
    AddressSpaceLock& operator=(const AddressSpaceLock&) = delete;

private:
    ExecutionGate::Exclusive execution_;
    std::unique_lock<std::recursive_mutex> mutex_;
};

NativeReadLease::NativeReadLease(
    ReferenceEngine& engine,
    const NativeMemoryContext& context)
    : engine_(engine), context_(context) {
    assert(context_.owns_lock());
    engine_.refresh_native_reads();
}

const NativeReadTable& NativeReadLease::table() const {
    assert(context_.owns_lock());
    engine_.refresh_native_reads();
    return *engine_.native_read_table_;
}

bool NativeReadLease::requires_atomic_access() const noexcept {
    assert(context_.owns_lock());
    return !context_.is_exclusive();
}

bool NativeReadLease::refill_read(std::uint64_t address, unsigned bytes,
    NativeSpanRefillDiagnostic* diagnostic) {
    assert(context_.owns_lock());
    engine_.refresh_native_reads();
    return engine_.refill_native_read(address, bytes, diagnostic);
}

NativeWriteLease::NativeWriteLease(
    ReferenceEngine& engine,
    const NativeMemoryContext& context)
    : engine_(engine), context_(context) {
    assert(context_.owns_lock());
}

NativeWriteLease::~NativeWriteLease() {
    // Normal native returns commit in the cache. This also covers unwinding
    // while the adapter still owns its lock.
    if (journal_.count != 0) commit();
}

const NativeWriteTable& NativeWriteLease::table() const {
    assert(context_.owns_lock());
    assert(journal_.count == 0);
    engine_.refresh_native_reads();
    return *engine_.native_write_table_;
}

NativeWriteJournal& NativeWriteLease::journal() {
    assert(context_.owns_lock());
    return journal_;
}

bool NativeWriteLease::refill_write(std::uint64_t address, unsigned bytes,
    NativeSpanRefillDiagnostic* diagnostic) {
    assert(context_.owns_lock());
    commit();
    engine_.refresh_native_reads();
    return engine_.refill_native_write(address, bytes, diagnostic);
}

void NativeWriteLease::commit() noexcept {
    if (journal_.count == 0) return;
    assert(context_.owns_lock());
    // The shared execution lease pins every region token. All checked and
    // exclusive observers wait for this lease, so journals can serialize only
    // their metadata publication without stopping other native scalar work.
    std::unique_lock lock(engine_.address_space_->mutex, std::defer_lock);
    if (!context_.is_exclusive()) lock.lock();
    engine_.commit_native_writes(journal_);
}

// Only ReferenceEngine::run creates this proxy. Its context owns the complete
// instruction/chain lifetime; no unchecked memory API or pointer escapes it.
class ReferenceEngine::ScopedMemoryAccess final : public GuestMemory {
public:
    explicit ScopedMemoryAccess(ReferenceEngine& engine, bool allow_shared)
        : engine_(engine), context_(engine, allow_shared),
          native_reads_(engine, context_), native_writes_(engine, context_) {}
    ScopedMemoryAccess(const ScopedMemoryAccess&) = delete;
    ScopedMemoryAccess& operator=(const ScopedMemoryAccess&) = delete;
    ScopedMemoryAccess(ScopedMemoryAccess&&) = delete;
    ScopedMemoryAccess& operator=(ScopedMemoryAccess&&) = delete;

    void unlock_for_dispatch() {
        native_writes_.commit();
        context_.unlock();
    }
    void promote_exclusive() const {
        native_writes_.commit();
        context_.promote_exclusive();
    }
    bool permits_cached_region(std::uint64_t pc, std::uint64_t generation) const {
        return context_.permits_cached_region(pc, generation);
    }
    std::size_t maximum_chain_steps() const noexcept {
        return context_.maximum_chain_steps();
    }
    std::size_t maximum_chain_blocks() const noexcept {
        return context_.maximum_chain_blocks();
    }

    std::uint8_t read8(std::uint64_t address) const override {
        promote_exclusive();
        return engine_.read8(address);
    }
    void write8(std::uint64_t address, std::uint8_t value) override {
        promote_exclusive();
        engine_.write8(address, value);
    }
    std::uint32_t read32(std::uint64_t address) const override {
        promote_exclusive();
        if (const auto value = engine_.read_contiguous(address, 4)) {
            return static_cast<std::uint32_t>(*value);
        }
        return engine_.read32(address);
    }
    std::uint64_t read64(std::uint64_t address) const override {
        promote_exclusive();
        if (const auto value = engine_.read_contiguous(address, 8)) return *value;
        return engine_.read64(address);
    }
    void write32(std::uint64_t address, std::uint32_t value) override {
        promote_exclusive();
        if (!engine_.write_contiguous(address, 4, value)) engine_.write32(address, value);
    }
    void write64(std::uint64_t address, std::uint64_t value) override {
        promote_exclusive();
        if (!engine_.write_contiguous(address, 8, value)) engine_.write64(address, value);
    }
    std::optional<std::uint64_t> try_read_mapped(
        std::uint64_t address, unsigned bytes) const override {
        promote_exclusive();
        return engine_.read_mapped(address, bytes);
    }
    bool try_write_mapped(std::uint64_t address, unsigned bytes,
                          std::uint64_t value) override {
        promote_exclusive();
        return engine_.write_mapped(address, bytes, value);
    }
    std::optional<std::uint64_t> try_read_contiguous(
        std::uint64_t address, unsigned bytes) const override {
        promote_exclusive();
        return engine_.read_contiguous(address, bytes);
    }
    bool try_write_contiguous(std::uint64_t address, unsigned bytes,
                              std::uint64_t value) override {
        promote_exclusive();
        return engine_.write_contiguous(address, bytes, value);
    }
    std::optional<std::uint64_t> write_revision(
        std::uint64_t address, std::uint64_t size) const override {
        promote_exclusive();
        return engine_.mapped_write_revision(address, size);
    }
    void read_bytes(std::uint64_t address, std::span<std::uint8_t> bytes) const override {
        promote_exclusive();
        engine_.read_bytes(address, bytes);
    }
    void write_bytes(std::uint64_t address, std::span<const std::uint8_t> bytes) override {
        promote_exclusive();
        engine_.write_bytes(address, bytes);
    }
    void fill_bytes(std::uint64_t address, std::uint8_t value, std::uint64_t count) override {
        promote_exclusive();
        engine_.fill_bytes(address, value, count);
    }
    void copy_bytes(std::uint64_t destination, std::uint64_t source,
                     std::uint64_t count, bool move) override {
        promote_exclusive();
        engine_.copy_bytes(destination, source, count, move);
    }
    std::vector<std::uint8_t> read_string(std::uint64_t address,
        unsigned character_bytes, std::size_t maximum_characters,
        bool* terminated) const override {
        promote_exclusive();
        return engine_.read_string(address, character_bytes, maximum_characters, terminated);
    }
    std::size_t string_length(std::uint64_t address, unsigned character_bytes,
        std::size_t maximum_characters, bool* terminated) const override {
        promote_exclusive();
        return engine_.string_length(address, character_bytes, maximum_characters, terminated);
    }
    void map_memory(std::uint64_t base, std::size_t size) override {
        promote_exclusive();
        engine_.map_memory(base, size);
    }
    void reserve_memory(std::uint64_t base, std::uint64_t size) override {
        promote_exclusive();
        engine_.reserve_memory(base, size);
    }
    void map_external_memory(std::uint64_t base, std::uint8_t* bytes,
                             std::size_t size, bool writable) override {
        promote_exclusive();
        engine_.map_external_memory(base, bytes, size, writable);
    }
    void unmap_memory(std::uint64_t base) override {
        promote_exclusive();
        engine_.unmap_memory(base);
    }
    bool is_range_available(std::uint64_t base, std::uint64_t size) const override {
        promote_exclusive();
        return engine_.is_range_available(base, size);
    }
    std::optional<RegionInfo> query_region(std::uint64_t address) const override {
        promote_exclusive();
        return engine_.query_region(address);
    }
    void set_memory_access(std::uint64_t base, bool readable,
                           bool writable, bool executable) override {
        promote_exclusive();
        engine_.set_memory_access(base, readable, writable, executable);
    }

private:
    NativeReadLease* native_read_lease() override {
        assert(context_.owns_lock());
        return &native_reads_;
    }
    NativeWriteLease* native_write_lease() override {
        assert(context_.owns_lock());
        return &native_writes_;
    }
    ReferenceEngine& engine_;
    mutable NativeMemoryContext context_;
    NativeReadLease native_reads_;
    mutable NativeWriteLease native_writes_;
};

ReferenceEngine::ReferenceEngine(PeImage& image, ImportDispatcher dispatcher)
    : ReferenceEngine(ExecutionStart{image.entry(), image.stack_reserve()}) {
    image_ = &image;
    address_space_->default_worker_stack_size = stack_size_;
    dispatcher_ = std::move(dispatcher);
    if (image.is_dll()) throw std::runtime_error("cannot run a DLL as an executable");
    if (!dispatcher_ &&
        std::any_of(image.imports().begin(), image.imports().end(),
                    [](const Import& imported) { return imported.entry != 0; })) {
        throw std::runtime_error("imported Windows APIs require a bridge");
    }
    if (image.base() < stack_top && image.base() + image.size() > stack_top - stack_size_) {
        throw std::runtime_error("PE image overlaps the guest stack");
    }
    if (overlaps_guest_call_window(image.base(), image.size())) {
        throw std::runtime_error("PE image overlaps synthetic guest call addresses");
    }
    address_space_->modules.push_back(&image);
    address_space_->modules_by_base.emplace(image.base(), &image);
    address_space_->module_identities.emplace(&image, advance_backing_identity());
    for (const auto& imported : image.imports()) {
        if (imported.entry != 0)
            address_space_->import_entries.emplace(imported.entry, make_import_binding(imported));
    }
    record_topology_change(image.base(), image.size());
}

ReferenceEngine::ReferenceEngine(ExecutionStart start)
    : address_space_(std::make_shared<SharedAddressSpace>()),
      stack_top_(stack_top),
      stack_size_(normalize_stack_size(start.stack_size, default_stack_size)) {
    initial_state_.pc = start.entry_pc;
    initial_state_.sp = stack_top_;
    initial_state_.x[30] = return_sentinel;
    state_ = initial_state_;
    address_space_->main_stack_size = stack_size_;
    address_space_->default_worker_stack_size = default_stack_size;
    address_space_->regions.emplace(stack_top_ - stack_size_, MemoryRegion{
        stack_top_ - stack_size_,
        std::vector<std::uint8_t>(static_cast<std::size_t>(stack_size_), 0),
        nullptr, 0, true, true, false, false, advance_write_revision(),
        advance_backing_identity()});
    record_topology_change(stack_top_ - stack_size_, stack_size_);
}

ReferenceEngine::ReferenceEngine(ReferenceEngine& parent, std::uint64_t stack_size)
    : image_(parent.image_), dispatcher_(parent.dispatcher_),
      supervisor_call_handler_(parent.supervisor_call_handler_),
      address_space_(parent.address_space_),
      instruction_limit_(parent.instruction_limit_) {
    AddressSpaceLock lock(*address_space_);
    stack_size_ = normalize_stack_size(stack_size, address_space_->default_worker_stack_size);
    // Worker stacks are allocated downwards below the main stack, each with
    // an unmapped gap beneath it. Addresses are not reused.
    auto& cursor = address_space_->worker_stack_cursor;
    if (cursor == 0) cursor = stack_top - address_space_->main_stack_size - worker_stack_gap;
    if (cursor < worker_stack_floor + stack_size_ + worker_stack_gap) {
        throw std::runtime_error("guest thread stack address space exhausted");
    }
    stack_top_ = cursor;
    worker_stack_ = true;
    const auto base = stack_top_ - stack_size_;
    if (!is_range_available(base, stack_size_)) {
        throw std::runtime_error("guest thread stack overlaps guest memory");
    }
    cursor = base - worker_stack_gap;
    ++address_space_->next_stack_index;
    address_space_->regions.emplace(base, MemoryRegion{
        base, std::vector<std::uint8_t>(static_cast<std::size_t>(stack_size_), 0),
        nullptr, 0, true, true, false, false, advance_write_revision(),
        advance_backing_identity()});
    record_topology_change(base, stack_size_);
    initial_state_.sp = stack_top_;
    initial_state_.x[18] = parent.initial_state_.x[18];
    initial_state_.x[30] = return_sentinel;
    state_ = initial_state_;
}

ReferenceEngine::~ReferenceEngine() {
    if (!worker_stack_) return;
    AddressSpaceLock lock(*address_space_);
    address_space_->regions.erase(stack_top_ - stack_size_);
    record_topology_change(stack_top_ - stack_size_, stack_size_);
}

std::unique_ptr<ReferenceEngine> ReferenceEngine::fork_thread(
    std::uint64_t entry, std::uint64_t argument, std::uint64_t stack_size) {
    auto worker = std::unique_ptr<ReferenceEngine>(new ReferenceEngine(*this, stack_size));
    worker->set_jit_enabled(jit_enabled_);
    if (jit_enabled_ && jit_cache_) {
        // This caller's generated frame has returned. Quiesce every other
        // thread while validating PE provenance and retiring shared RX chunks.
        AddressSpaceLock lock(*address_space_);
        const auto generation = address_space_->code_generation;
        const auto inherited = worker->jit_cache_->inherit_immutable(
            *jit_cache_, generation, address_space_->modules);
        worker->jit_seen_generation_ = generation;
        if (jit_profiling_) {
            address_space_->profiled_worker_caches.fetch_add(1, std::memory_order_relaxed);
            address_space_->inherited_worker_native_regions.fetch_add(
                inherited, std::memory_order_relaxed);
        }
    }
    if (jit_profiling_) worker->set_jit_profiling(true);
    worker->initial_state_.pc = entry;
    worker->initial_state_.x[0] = argument;
    worker->state_ = worker->initial_state_;
    return worker;
}

ReferenceEngine::BoundImport ReferenceEngine::make_import_binding(
    const Import& imported) const {
    const auto resolver = address_space_->native_import_resolver;
    return {std::make_shared<const Import>(imported),
            resolver == nullptr ? nullptr : resolver(imported)};
}

void ReferenceEngine::set_native_import_resolver(NativeImportResolver resolver) {
    AddressSpaceLock lock(*address_space_);
    address_space_->native_import_resolver = resolver;
    const auto bind = [resolver](auto& entries) {
        for (auto& [address, binding] : entries) {
            static_cast<void>(address);
            binding.native = resolver == nullptr ? nullptr : resolver(*binding.imported);
        }
    };
    bind(address_space_->import_entries);
    bind(staged_import_entries_);
    record_topology_change();
}

void ReferenceEngine::add_module(PeImage& module) {
    AddressSpaceLock lock(*address_space_);
    if (!module.is_dll()) throw std::runtime_error("guest module is not a DLL");
    if (overlaps_guest_call_window(module.base(), module.size())) {
        throw std::runtime_error("DLL overlaps synthetic guest call addresses");
    }
    for (const auto& [base, size] : address_space_->reserved_ranges) {
        if (module.base() < base + size && base < module.base() + module.size()) {
            throw std::runtime_error("DLL overlaps reserved guest memory");
        }
    }
    if (module.base() < stack_top &&
        module.base() + module.size() > stack_top - address_space_->main_stack_size) {
        throw std::runtime_error("DLL overlaps the guest stack");
    }
    for (const auto* loaded : address_space_->modules) {
        if (module.base() < loaded->base() + loaded->size() &&
            loaded->base() < module.base() + module.size()) {
            throw std::runtime_error("guest modules overlap in memory");
        }
    }
    for (const auto& [base, region] : address_space_->regions) {
        static_cast<void>(base);
        if (module.base() < region.base + region.size() &&
            region.base < module.base() + module.size()) {
            throw std::runtime_error("DLL overlaps mapped guest memory");
        }
    }
    for (const auto& imported : module.imports()) {
        if (imported.entry != 0 && address_space_->import_entries.contains(imported.entry)) {
            throw std::runtime_error("guest import bridge entries overlap");
        }
    }
    address_space_->modules.push_back(&module);
    address_space_->modules_by_base.emplace(module.base(), &module);
    address_space_->module_identities.emplace(&module, advance_backing_identity());
    for (const auto& imported : module.imports()) {
        if (imported.entry != 0 &&
            !address_space_->import_entries.emplace(
                imported.entry, make_import_binding(imported)).second) {
            throw std::runtime_error("guest import bridge entries overlap");
        }
    }
    record_topology_change(module.base(), module.size());
}

void ReferenceEngine::stage_module(PeImage& module) {
    AddressSpaceLock lock(*address_space_);
    if (!module.is_dll()) throw std::runtime_error("guest module is not a DLL");
    if (overlaps_guest_call_window(module.base(), module.size())) {
        throw std::runtime_error("DLL overlaps synthetic guest call addresses");
    }
    for (const auto& [base, size] : address_space_->reserved_ranges) {
        if (module.base() < base + size && base < module.base() + module.size()) {
            throw std::runtime_error("DLL overlaps reserved guest memory");
        }
    }
    if (module.base() < stack_top_ &&
        module.base() + module.size() > stack_top_ - stack_size_) {
        throw std::runtime_error("DLL overlaps the guest stack");
    }
    const auto overlaps_module = [&](const PeImage* loaded) {
        return module.base() < loaded->base() + loaded->size() &&
               loaded->base() < module.base() + module.size();
    };
    if (std::any_of(address_space_->modules.begin(), address_space_->modules.end(),
                    overlaps_module) ||
        std::any_of(staged_modules_.begin(), staged_modules_.end(),
                    overlaps_module)) {
        throw std::runtime_error("guest modules overlap in memory");
    }
    for (const auto& [base, region] : address_space_->regions) {
        static_cast<void>(base);
        if (module.base() < region.base + region.size() &&
            region.base < module.base() + module.size()) {
            throw std::runtime_error("DLL overlaps mapped guest memory");
        }
    }
    for (const auto& imported : module.imports()) {
        if (imported.entry != 0 &&
            (address_space_->import_entries.contains(imported.entry) ||
             staged_import_entries_.contains(imported.entry))) {
            throw std::runtime_error("guest import bridge entries overlap");
        }
    }
    staged_modules_.push_back(&module);
    staged_modules_by_base_.emplace(module.base(), &module);
    staged_module_identities_.emplace(&module, advance_backing_identity());
    for (const auto& imported : module.imports()) {
        if (imported.entry != 0) {
            staged_import_entries_.emplace(imported.entry, make_import_binding(imported));
        }
    }
    record_topology_change(module.base(), module.size());
}

void ReferenceEngine::commit_staged_modules() {
    AddressSpaceLock lock(*address_space_);
    auto modules = address_space_->modules;
    auto by_base = address_space_->modules_by_base;
    auto identities = address_space_->module_identities;
    auto imports = address_space_->import_entries;
    modules.insert(modules.end(), staged_modules_.begin(), staged_modules_.end());
    by_base.insert(staged_modules_by_base_.begin(), staged_modules_by_base_.end());
    identities.insert(staged_module_identities_.begin(), staged_module_identities_.end());
    imports.insert(staged_import_entries_.begin(), staged_import_entries_.end());
    address_space_->modules.swap(modules);
    address_space_->modules_by_base.swap(by_base);
    address_space_->module_identities.swap(identities);
    address_space_->import_entries.swap(imports);
    if (staged_modules_.empty()) record_topology_change();
    for (const auto* module : staged_modules_)
        record_topology_change(module->base(), module->size());
    ++address_space_->code_generation;
    discard_staged_modules();
}

void ReferenceEngine::discard_staged_modules() {
    AddressSpaceLock lock(*address_space_);
    if (!staged_modules_.empty()) {
        for (const auto* module : staged_modules_)
            record_topology_change(module->base(), module->size());
        ++address_space_->code_generation;
    }
    staged_modules_.clear();
    staged_modules_by_base_.clear();
    staged_module_identities_.clear();
    staged_import_entries_.clear();
}

PeImage* ReferenceEngine::module_at(std::uint64_t address) const {
    const auto lookup = [address](const auto& by_base) -> PeImage* {
        auto found = by_base.upper_bound(address);
        if (found == by_base.begin()) return nullptr;
        --found;
        return address - found->first < found->second->size()
            ? found->second : nullptr;
    };
    if (auto* staged = lookup(staged_modules_by_base_)) return staged;
    return lookup(address_space_->modules_by_base);
}

void ReferenceEngine::map_memory(std::uint64_t base, std::size_t size) {
    AddressSpaceLock lock(*address_space_);
    if (!is_range_available(base, size)) {
        throw std::runtime_error("invalid guest memory mapping");
    }
    address_space_->regions.emplace(base, MemoryRegion{base,
                                        std::vector<std::uint8_t>(size, 0),
                                        nullptr, 0, true, true, false, false,
                                        advance_write_revision(), advance_backing_identity()});
    record_topology_change(base, size);
}

void ReferenceEngine::reserve_memory(std::uint64_t base, std::uint64_t size) {
    AddressSpaceLock lock(*address_space_);
    if (!is_range_available(base, size)) {
        throw std::runtime_error("invalid guest memory reservation");
    }
    address_space_->reserved_ranges.emplace(base, size);
    record_topology_change();
}

void ReferenceEngine::map_external_memory(std::uint64_t base,
                                          std::uint8_t* bytes,
                                          std::size_t size, bool writable) {
    AddressSpaceLock lock(*address_space_);
    if (bytes == nullptr || !is_range_available(base, size)) {
        throw std::runtime_error("invalid external guest memory mapping");
    }
    address_space_->regions.emplace(base, MemoryRegion{base, {}, bytes, size,
                                        true, writable, false, true, 0,
                                        advance_backing_identity()});
    record_topology_change(base, size);
}

bool ReferenceEngine::is_range_available(std::uint64_t base,
                                          std::uint64_t size) const {
    AddressSpaceLock lock(*address_space_);
    if (size == 0 || base > UINT64_MAX - size) return false;
    if (overlaps_guest_call_window(base, size)) return false;
    const auto end = base + size;
    const auto reserved_next = address_space_->reserved_ranges.lower_bound(base);
    if ((reserved_next != address_space_->reserved_ranges.end() &&
         end > reserved_next->first) ||
        (reserved_next != address_space_->reserved_ranges.begin() &&
         std::prev(reserved_next)->first + std::prev(reserved_next)->second > base)) {
        return false;
    }
    for (const auto* module : address_space_->modules) {
        if (base < module->base() + module->size() && module->base() < end) {
            return false;
        }
    }
    for (const auto* module : staged_modules_) {
        if (base < module->base() + module->size() && module->base() < end) {
            return false;
        }
    }
    const auto next = address_space_->regions.lower_bound(base);
    return (next == address_space_->regions.end() || end <= next->first) &&
           (next == address_space_->regions.begin() ||
            std::prev(next)->second.base +
                std::prev(next)->second.size() <= base);
}

std::optional<GuestMemory::RegionInfo> ReferenceEngine::query_region(
    std::uint64_t address) const {
    AddressSpaceLock lock(*address_space_);
    auto entry = address_space_->regions.upper_bound(address);
    if (entry != address_space_->regions.begin()) {
        --entry;
        const auto& region = entry->second;
        if (address >= region.base && address - region.base < region.size()) {
            return RegionInfo{region.base, region.size(), region.readable,
                              region.writable, region.executable,
                              false, region.mapped};
        }
    }
    auto reserved = address_space_->reserved_ranges.upper_bound(address);
    if (reserved != address_space_->reserved_ranges.begin()) {
        --reserved;
        if (address - reserved->first < reserved->second) {
            return RegionInfo{reserved->first, reserved->second,
                              false, false, false, false, false};
        }
    }
    if (const auto* module = module_at(address)) {
            bool executable = false;
            bool writable = false;
            const auto rva = address - module->base();
            for (const auto& section : module->sections()) {
                if (rva >= section.rva &&
                    rva < static_cast<std::uint64_t>(section.rva) + section.size) {
                    executable = section.executable;
                    writable = section.writable;
                    break;
                }
            }
            return RegionInfo{module->base(), module->size(), true, writable,
                              executable, true, false};
    }
    return std::nullopt;
}

void ReferenceEngine::unmap_memory(std::uint64_t base) {
    AddressSpaceLock lock(*address_space_);
    const auto found = address_space_->regions.find(base);
    if (found == address_space_->regions.end()) {
        if (address_space_->reserved_ranges.erase(base) != 0) {
            record_topology_change();
            return;
        }
        throw std::runtime_error("guest memory mapping was not found");
    }
    read_region_hint_ = nullptr;
    write_region_hint_ = nullptr;
    if (found->second.executable) ++address_space_->code_generation;
    const auto size = found->second.size();
    address_space_->regions.erase(found);
    record_topology_change(base, size);
}

void ReferenceEngine::set_memory_writable(std::uint64_t base, bool writable) {
    AddressSpaceLock lock(*address_space_);
    const auto found = address_space_->regions.find(base);
    if (found == address_space_->regions.end()) {
        throw std::runtime_error("guest memory mapping was not found");
    }
    set_memory_access(base, found->second.readable, writable,
                      found->second.executable);
}

void ReferenceEngine::set_memory_access(std::uint64_t base, bool readable,
                                        bool writable, bool executable) {
    AddressSpaceLock lock(*address_space_);
    const auto found = address_space_->regions.find(base);
    if (found == address_space_->regions.end()) {
        throw std::runtime_error("guest memory mapping was not found");
    }
    if (found->second.executable || executable) {
        ++address_space_->code_generation;
    }
    found->second.readable = readable;
    found->second.writable = writable;
    found->second.executable = executable;
    record_topology_change(base, found->second.size());
}

void ReferenceEngine::set_initial_register(unsigned index, std::uint64_t value) {
    if (index >= initial_state_.x.size()) {
        throw std::runtime_error("invalid initial guest register");
    }
    initial_state_.x[index] = value;
    state_.x[index] = value;
}

void ReferenceEngine::set_initial_vector(
    unsigned index, std::array<std::uint64_t, 2> value) {
    if (index >= initial_state_.v.size()) {
        throw std::runtime_error("invalid initial guest vector register");
    }
    initial_state_.v[index] = value;
    state_.v[index] = value;
}

void ReferenceEngine::set_initial_stack_pointer(std::uint64_t value) {
    const auto [base, top] = stack_bounds();
    if ((value & 15U) != 0 || value < base || value > top) {
        throw std::runtime_error("initial guest stack pointer is outside the aligned stack bounds");
    }
    initial_state_.sp = value;
    state_.sp = value;
}

void ReferenceEngine::set_supervisor_call_handler(SupervisorCallHandler handler) {
    supervisor_call_handler_ = std::move(handler);
}

void ReferenceEngine::set_instruction_limit(std::uint64_t limit) {
    if (limit == 0 || limit == UINT64_MAX) {
        throw std::runtime_error("invalid guest instruction limit");
    }
    instruction_limit_ = limit;
}

void ReferenceEngine::set_jit_enabled(bool enabled) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    jit_enabled_ = enabled;
    if (enabled && !jit_cache_) jit_cache_ = std::make_unique<X64BlockCache>();
    if (!enabled) {
        jit_cache_.reset();
        jit_profiling_ = false;
    }
#else
    if (enabled) throw std::runtime_error("x64 JIT is unavailable on this host");
    jit_enabled_ = false;
#endif
}

void ReferenceEngine::set_jit_profiling(bool enabled) {
    if (enabled && !jit_enabled_) {
        throw std::runtime_error("JIT profiling requires the opt-in JIT");
    }
    if (jit_profiling_ != enabled) {
        native_read_invalidated_slots_.reset();
        native_write_invalidated_slots_.reset();
    }
    jit_profiling_ = enabled;
    if (jit_cache_) jit_cache_->set_profiling(enabled);
}

JitStats ReferenceEngine::jit_stats(std::size_t hottest) const {
    auto result = jit_cache_ ? jit_cache_->stats(hottest) : JitStats{};
    result.fallback_instructions = jit_fallback_instructions_;
    return result;
}

NativeConcurrencyStats ReferenceEngine::native_concurrency_stats() const noexcept {
    return {address_space_->profiled_shared_read_leases.load(std::memory_order_relaxed),
            address_space_->peak_profiled_shared_readers.load(std::memory_order_relaxed),
            address_space_->profiled_worker_caches.load(std::memory_order_relaxed),
            address_space_->inherited_worker_native_regions.load(std::memory_order_relaxed)};
}

NativeTopologyStats ReferenceEngine::native_topology_stats() const noexcept {
    return native_topology_stats_;
}

std::pair<std::uint64_t, std::uint64_t> ReferenceEngine::stack_bounds() const noexcept {
    return {stack_top_ - stack_size_, stack_top_};
}

void ReferenceEngine::resize_stack(std::uint64_t size) {
    AddressSpaceLock lock(*address_space_);
    if (worker_stack_ || address_space_->worker_stack_cursor != 0) {
        throw std::logic_error("guest main stack can only be resized before workers start");
    }
    const auto replacement = normalize_stack_size(size, default_stack_size);
    if (replacement == stack_size_) return;
    const auto old_base = stack_top_ - stack_size_;
    const auto new_base = stack_top_ - replacement;
    auto old_region = address_space_->regions.extract(old_base);
    if (!is_range_available(new_base, replacement)) {
        address_space_->regions.insert(std::move(old_region));
        throw std::runtime_error("resized guest stack overlaps guest memory");
    }
    read_region_hint_ = nullptr;
    write_region_hint_ = nullptr;
    record_topology_change(old_base, stack_size_);
    address_space_->regions.emplace(new_base, MemoryRegion{
        new_base, std::vector<std::uint8_t>(static_cast<std::size_t>(replacement), 0),
        nullptr, 0, true, true, false, false, advance_write_revision(),
        advance_backing_identity()});
    record_topology_change(new_base, replacement);
    stack_size_ = replacement;
    address_space_->main_stack_size = replacement;
}

void ReferenceEngine::set_default_worker_stack_size(std::uint64_t size) {
    AddressSpaceLock lock(*address_space_);
    address_space_->default_worker_stack_size = normalize_stack_size(size, default_stack_size);
}

std::uint64_t ReferenceEngine::default_worker_stack_size() const noexcept {
    return address_space_->default_worker_stack_size;
}

void ReferenceEngine::initialize_dlls(std::span<PeImage* const> dependency_order) {
    if (image_ == nullptr) {
        throw std::runtime_error("DLL initialization requires a PE executable");
    }
    if (dependency_order.size() != address_space_->modules.size() - 1) {
        throw std::runtime_error("incomplete DLL initialization order");
    }
    std::vector<bool> seen(address_space_->modules.size(), false);
    const auto call_attach = [this](PeImage& module, std::uint64_t address,
                                    bool check_result) {
        state_ = initial_state_;
        reservation_ = {};
        state_.pc = address;
        state_.sp = stack_top_;
        state_.x[0] = module.base();
        state_.x[1] = 1; // DLL_PROCESS_ATTACH
        state_.x[2] = 0;
        state_.x[30] = return_sentinel;
        if (run().exit_code == 0 && check_result) {
            throw std::runtime_error("guest DLL rejected process attach");
        }
    };
    for (auto* selected : dependency_order) {
        const auto found = std::find(address_space_->modules.begin() + 1, address_space_->modules.end(), selected);
        if (found == address_space_->modules.end() || seen[found - address_space_->modules.begin()]) {
            throw std::runtime_error("invalid DLL initialization order");
        }
        seen[found - address_space_->modules.begin()] = true;
        auto& module = *selected;
        if (module.tls()) {
            for (const auto callback : module.tls()->callbacks) {
                call_attach(module, callback, false);
            }
        }
        if (module.entry_rva() != 0) call_attach(module, module.entry(), true);
    }
    if (image_->tls()) {
        for (const auto callback : image_->tls()->callbacks) {
            call_attach(*image_, callback, false);
        }
    }
    state_ = initial_state_;
    reservation_ = {};
    state_.pc = image_->entry();
    state_.sp = stack_top_;
    state_.x[30] = return_sentinel;
}

std::uint64_t ReferenceEngine::invoke_callback(std::uint64_t address,
                                               std::uint64_t argument) {
    return invoke_callback(address, std::span(&argument, 1));
}

std::uint64_t ReferenceEngine::invoke_callback(
    std::uint64_t address, std::span<const std::uint64_t> arguments) {
    if (arguments.size() > 8) {
        throw std::runtime_error("guest callback has too many register arguments");
    }
    const auto saved = state_;
    if (saved.sp < stack_top_ - stack_size_ + 256 || saved.sp > stack_top_) {
        throw std::runtime_error("guest callback has no stack space");
    }
    state_.pc = address;
    state_.sp = (saved.sp - 256) & ~15ULL;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        state_.x[index] = arguments[index];
    }
    state_.x[30] = return_sentinel;
    reservation_ = {};
    try {
        static_cast<void>(run());
    } catch (...) {
        state_ = saved;
        reservation_ = {};
        throw;
    }
    const auto result = state_.x[0];
    state_ = saved;
    reservation_ = {};
    return result;
}

std::uint64_t ReferenceEngine::register_guest_call(GuestCallHandler handler) {
    if (!handler) throw std::runtime_error("synthetic guest call handler is empty");
    AddressSpaceLock lock(*address_space_);
    if (address_space_->next_guest_call_index == maximum_guest_calls) {
        throw std::runtime_error("synthetic guest call address space exhausted");
    }
    const auto address = guest_call_base +
        address_space_->next_guest_call_index * guest_call_stride;
    address_space_->guest_calls.emplace(address, std::move(handler));
    ++address_space_->next_guest_call_index;
    return address;
}

void ReferenceEngine::unregister_guest_call(std::uint64_t address) {
    AddressSpaceLock lock(*address_space_);
    if (address_space_->guest_calls.erase(address) == 0) {
        throw std::runtime_error("unknown synthetic guest call address");
    }
}

std::uint32_t ReferenceEngine::read32(std::uint64_t address) const {
    AddressSpaceLock lock(*address_space_);
    if (const auto value = read_contiguous(address, 4)) {
        return static_cast<std::uint32_t>(*value);
    }
    return GuestMemory::read32(address);
}

std::uint64_t ReferenceEngine::read64(std::uint64_t address) const {
    AddressSpaceLock lock(*address_space_);
    if (const auto value = read_contiguous(address, 8)) return *value;
    return GuestMemory::read64(address);
}

void ReferenceEngine::write32(std::uint64_t address, std::uint32_t value) {
    AddressSpaceLock lock(*address_space_);
    if (write_contiguous(address, 4, value)) return;
    GuestMemory::write32(address, value);
}

void ReferenceEngine::write64(std::uint64_t address, std::uint64_t value) {
    AddressSpaceLock lock(*address_space_);
    if (write_contiguous(address, 8, value)) return;
    GuestMemory::write64(address, value);
}

void ReferenceEngine::write_scalar_fields(std::span<const GuestScalarWrite> fields) {
    if (typeid(*this) != typeid(ReferenceEngine))
        return GuestMemory::write_scalar_fields(fields);
    if (fields.empty()) return;
    AddressSpaceLock lock(*address_space_);
    GuestMemory::write_scalar_fields(fields);
}

std::optional<std::uint64_t> ReferenceEngine::try_read_mapped(
    std::uint64_t address, unsigned bytes) const {
    AddressSpaceLock lock(*address_space_);
    return read_mapped(address, bytes);
}

std::optional<std::uint64_t> ReferenceEngine::read_mapped(
    std::uint64_t address, unsigned bytes) const {
    if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) {
        return std::nullopt;
    }
    const MemoryRegion* region = nullptr;
    if (address_space_->checked_hints_usable && read_region_hint_ != nullptr &&
        read_hint_generation_ == address_space_->generation) {
        const auto* hint = read_region_hint_;
        if (address >= hint->base && address - hint->base < hint->size()) {
            region = hint;
        }
    }
    if (region == nullptr) {
        auto entry = address_space_->regions.upper_bound(address);
        if (entry != address_space_->regions.begin()) {
            region = &std::prev(entry)->second;
        }
    }
    if (region == nullptr || address < region->base ||
        address - region->base >= region->size() ||
        bytes > region->size() - static_cast<std::size_t>(address - region->base) ||
        !region->readable) {
        return std::nullopt;
    }
    read_region_hint_ = region;
    read_hint_generation_ = address_space_->generation;
    const auto offset = static_cast<std::size_t>(address - region->base);
    std::uint64_t value = 0;
    if constexpr (std::endian::native == std::endian::little) {
        const auto* data = region->external == nullptr
            ? region->bytes.data() : region->external;
        std::memcpy(&value, data + offset, bytes);
    } else {
        for (unsigned index = 0; index < bytes; ++index) {
            value |= static_cast<std::uint64_t>(region->read(offset + index)) << (8 * index);
        }
    }
    return value;
}

void ReferenceEngine::record_topology_change(std::uint64_t base, std::uint64_t size) noexcept {
    assert(ExecutionGate::Exclusive::held(*address_space_->execution_gate) ||
           address_space_.use_count() == 1);
    if (++address_space_->generation == 0) address_space_->checked_hints_usable = false;
    address_space_->topology_history->append(base, size);
}

void ReferenceEngine::refresh_native_reads() {
    const auto& history = *address_space_->topology_history;
    const auto stamp = history.stamp();
    if (native_topology_seen_ && history.usable() &&
        native_read_generation_ == stamp.sequence && native_topology_era_ == stamp.era) return;
    if (!native_topology_seen_) {
        // New engines have empty private tables; synchronization must not
        // pretend that an arbitrary sentinel identifies retained history.
        native_topology_seen_ = true;
        native_read_generation_ = stamp.sequence;
        native_topology_era_ = stamp.era;
        return;
    }
    const auto replay = history.since({native_read_generation_, native_topology_era_});
    // Inspect integer metadata only. Entries may contain dead pointers from a
    // discarded module or freed mapping; no host pointer or token is followed.
    if (replay.complete) {
        bool has_ranges = false;
        for (std::size_t index = 0; index < replay.count; ++index)
            has_ranges |= replay[index].size != 0;
        const auto refresh_table = [&](auto& table, auto& invalidated,
                                       auto& retained, auto& evicted) {
            for (std::size_t index = 0; index < table.entries.size(); ++index) {
                auto& entry = table.entries[index];
                if (entry.topology_epoch != table.current_epoch) continue;
                bool changed = false;
                for (std::size_t change = 0; change < replay.count && !changed; ++change)
                    changed = NativeTopologyHistory::overlaps(
                        entry.guest_begin, entry.guest_size, replay[change]);
                if (changed) {
                    entry = {};
                    if (jit_profiling_) { invalidated.set(index); ++evicted; }
                } else if (jit_profiling_) {
                    ++retained;
                }
            }
        };
        // Metadata-only edits require no table scan during normal execution.
        if (has_ranges || jit_profiling_) {
            refresh_table(*native_read_table_, native_read_invalidated_slots_,
                native_topology_stats_.retained_read_entries,
                native_topology_stats_.evicted_read_entries);
            refresh_table(*native_write_table_, native_write_invalidated_slots_,
                native_topology_stats_.retained_write_entries,
                native_topology_stats_.evicted_write_entries);
        }
        if (jit_profiling_) {
            ++native_topology_stats_.selective_refreshes;
            native_topology_stats_.replayed_changes += replay.count;
        }
    } else {
        if (jit_profiling_) {
            ++native_topology_stats_.full_flushes;
            for (std::size_t index = 0; index < NativeReadTable::entry_count; ++index)
                if (native_read_table_->entries[index].topology_epoch != 0)
                    native_read_invalidated_slots_.set(index);
            for (std::size_t index = 0; index < NativeWriteTable::entry_count; ++index)
                if (native_write_table_->entries[index].topology_epoch != 0)
                    native_write_invalidated_slots_.set(index);
        }
        // Incomplete history never revalidates an old pointer. Keep the former
        // global epoch proof and clear both tables before an epoch is reused.
        if (++native_read_table_->current_epoch == 0) {
            native_read_table_->entries = {};
            native_write_table_->entries = {};
            native_read_table_->current_epoch = 1;
        }
        native_write_table_->current_epoch = native_read_table_->current_epoch;
    }
    native_read_generation_ = stamp.sequence;
    native_topology_era_ = stamp.era;
}

bool ReferenceEngine::refill_native_read(std::uint64_t address, unsigned bytes,
    NativeSpanRefillDiagnostic* diagnostic) {
    if (diagnostic) *diagnostic = {diagnose_span_miss(*native_read_table_,
        native_read_invalidated_slots_, address, bytes), NativeSpanRefillRejection::none};
    const auto reject = [diagnostic](NativeSpanRefillRejection reason) {
        if (diagnostic) diagnostic->rejection = reason;
        return false;
    };
    if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8 &&
        bytes != 16 && bytes != 32) {
        return reject(NativeSpanRefillRejection::unsupported_width);
    }
    NativeReadEntry entry{};
    auto found = address_space_->regions.upper_bound(address);
    if (found != address_space_->regions.begin()) {
        auto& region = std::prev(found)->second;
        if (address >= region.base && address - region.base < region.size()) {
            if (!region.readable)
                return reject(NativeSpanRefillRejection::permissions);
            if (region.external != nullptr)
                return reject(NativeSpanRefillRejection::external);
            if (bytes > region.size() - (address - region.base))
                return reject(NativeSpanRefillRejection::boundary);
            entry.guest_begin = region.base;
            entry.guest_size = region.size();
            entry.host_begin = region.bytes.data();
        }
    }
    if (entry.host_begin == nullptr) {
        const auto* module = module_at(address);
        if (module == nullptr) return reject(NativeSpanRefillRejection::unmapped);
        const auto visible = visible_pe_interval(*module, address);
        if (!visible || bytes > visible->size - (address - visible->base))
            return reject(NativeSpanRefillRejection::boundary);
        // Checked PE reads allow the entire image extent, including headers
        // and zero-filled gaps. Preserve that contract rather than imposing
        // section permissions that the existing engine does not enforce.
        entry.guest_begin = visible->base;
        entry.guest_size = visible->size;
        entry.host_begin = module->bytes_.data() + (visible->base - module->base());
    }
    entry.page_tag = address >> 12;
    entry.topology_epoch = native_read_table_->current_epoch;
    const auto index = static_cast<std::size_t>(entry.page_tag &
        (NativeReadTable::entry_count - 1));
    native_read_table_->entries[index] = entry;
    if (jit_profiling_) native_read_invalidated_slots_.reset(index);
    return true;
}

bool ReferenceEngine::refill_native_write(std::uint64_t address, unsigned bytes,
    NativeSpanRefillDiagnostic* diagnostic) {
    if (diagnostic) *diagnostic = {diagnose_span_miss(*native_write_table_,
        native_write_invalidated_slots_, address, bytes), NativeSpanRefillRejection::none};
    const auto reject = [diagnostic](NativeSpanRefillRejection reason) {
        if (diagnostic) diagnostic->rejection = reason;
        return false;
    };
    if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8 &&
        bytes != 16 && bytes != 32) {
        return reject(NativeSpanRefillRejection::unsupported_width);
    }
    auto found = address_space_->regions.upper_bound(address);
    const auto fill_image = [&] {
        auto* module = module_at(address);
        if (module == nullptr) return reject(NativeSpanRefillRejection::unmapped);
        // Private DLL initialization can be discarded. Keep its writes on
        // the checked path until publication pins it in the shared space.
        if (staged_module_identities_.contains(module) ||
            !address_space_->module_identities.contains(module))
            return reject(NativeSpanRefillRejection::image);
        const auto rva = address - module->base();
        for (const auto& section : module->sections()) {
            if (rva < section.rva || rva - section.rva >= section.size) continue;
            if (!section.writable) return reject(NativeSpanRefillRejection::permissions);
            if (section.executable) return reject(NativeSpanRefillRejection::executable);
            if (bytes > section.size - (rva - section.rva))
                return reject(NativeSpanRefillRejection::boundary);
            static_assert(alignof(PeImage) > native_image_write_tag);
            const auto token = reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(module) | native_image_write_tag);
            const auto page = address >> 12;
            const auto index = static_cast<std::size_t>(
                page & (NativeWriteTable::entry_count - 1));
            native_write_table_->entries[index] = {
                page, native_write_table_->current_epoch, module->base() + section.rva,
                section.size, module->bytes_.data() + section.rva, token};
            if (jit_profiling_) native_write_invalidated_slots_.reset(index);
            return true;
        }
        // Headers and zero-filled gaps retain PE's checked write rejection.
        return reject(NativeSpanRefillRejection::permissions);
    };
    if (found == address_space_->regions.begin()) return fill_image();
    auto& region = std::prev(found)->second;
    if (address < region.base || address - region.base >= region.size())
        return fill_image();
    if (!region.readable || !region.writable)
        return reject(NativeSpanRefillRejection::permissions);
    if (region.executable) return reject(NativeSpanRefillRejection::executable);
    if (region.external != nullptr) return reject(NativeSpanRefillRejection::external);
    if (bytes > region.size() - (address - region.base))
        return reject(NativeSpanRefillRejection::boundary);
    const auto page = address >> 12;
    const auto index = static_cast<std::size_t>(page & (NativeWriteTable::entry_count - 1));
    native_write_table_->entries[index] = {
        page, native_write_table_->current_epoch, region.base, region.size(),
        region.bytes.data(), &region};
    if (jit_profiling_) native_write_invalidated_slots_.reset(index);
    return true;
}

void ReferenceEngine::commit_native_writes(NativeWriteJournal& journal) noexcept {
    assert(journal.count <= NativeWriteJournal::capacity);
    for (std::size_t index = 0; index < journal.count; ++index) {
        const auto& write = journal.entries[index];
        const auto token = reinterpret_cast<std::uintptr_t>(write.region_token);
        if ((token & native_image_write_tag) != 0) {
            [[maybe_unused]] auto* module = reinterpret_cast<PeImage*>(
                token & ~native_image_write_tag);
            assert(address_space_->module_identities.contains(module) &&
                   !staged_module_identities_.contains(module) &&
                   std::any_of(module->sections().begin(), module->sections().end(),
                       [&](const Section& section) {
                           return section.writable && !section.executable &&
                               write.guest_address >= module->base() + section.rva &&
                               write.guest_address - module->base() - section.rva < section.size &&
                               write.bytes != 0 && write.bytes <= section.size -
                                   (write.guest_address - module->base() - section.rva);
                       }));
            // PE buffers have no tracked write_revision today. Preserve that
            // contract, while publishing every byte to the shared exclusive
            // monitor even for an identical or net-zero data write.
            record_memory_write(write.guest_address, write.bytes, false);
            continue;
        }
        static_assert(alignof(MemoryRegion) > native_image_write_tag);
        auto* region = static_cast<MemoryRegion*>(write.region_token);
        assert(region != nullptr && region->external == nullptr &&
               region->writable && !region->executable &&
               write.guest_address >= region->base &&
               write.guest_address - region->base < region->size() &&
               write.bytes != 0 && write.bytes <=
                   region->size() - (write.guest_address - region->base));
        region->revision = advance_write_revision();
        record_memory_write(write.guest_address, write.bytes, false);
    }
    journal.count = 0;
}

bool ReferenceEngine::try_write_mapped(std::uint64_t address,
                                        unsigned bytes,
                                        std::uint64_t value) {
    AddressSpaceLock lock(*address_space_);
    return write_mapped(address, bytes, value);
}

bool ReferenceEngine::write_mapped(std::uint64_t address, unsigned bytes,
                                   std::uint64_t value) {
    if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) return false;
    MemoryRegion* region = nullptr;
    if (address_space_->checked_hints_usable && write_region_hint_ != nullptr &&
        write_hint_generation_ == address_space_->generation) {
        auto* hint = write_region_hint_;
        if (address >= hint->base && address - hint->base < hint->size()) {
            region = hint;
        }
    }
    if (region == nullptr) {
        auto entry = address_space_->regions.upper_bound(address);
        if (entry != address_space_->regions.begin()) {
            region = &std::prev(entry)->second;
        }
    }
    if (region == nullptr || address < region->base ||
        address - region->base >= region->size() ||
        bytes > region->size() - static_cast<std::size_t>(address - region->base) ||
        !region->writable) {
        return false;
    }
    write_region_hint_ = region;
    write_hint_generation_ = address_space_->generation;
    const auto offset = static_cast<std::size_t>(address - region->base);
    if constexpr (std::endian::native == std::endian::little) {
        auto* data = region->external == nullptr ? region->bytes.data() : region->external;
        std::memcpy(data + offset, &value, bytes);
    } else {
        for (unsigned index = 0; index < bytes; ++index) {
            region->write(offset + index, static_cast<std::uint8_t>(value >> (8 * index)));
        }
    }
    if (region->external == nullptr) region->revision = advance_write_revision();
    record_memory_write(address, bytes, region->executable);
    return true;
}

void ReferenceEngine::record_memory_write(std::uint64_t address, unsigned bytes,
                                           bool executable) {
    if (executable) address_space_->code_generation += bytes;
    while (bytes != 0) {
        const auto in_granule = std::min(bytes,
            16U - static_cast<unsigned>(address & 15U));
        if (reservation_.valid && (address & ~15ULL) ==
                                      (reservation_.address & ~15ULL)) {
            reservation_.valid = false;
        }
        // Preserve the existing per-byte epoch increments, grouped by the
        // granule that shares one epoch bucket. This also covers unaligned
        // stores that touch two granules.
        address_space_->exclusive_write_epochs[
            exclusive_epoch_bucket(address)] += in_granule;
        address += in_granule;
        bytes -= in_granule;
    }
}

std::optional<ReferenceEngine::VisiblePeInterval> ReferenceEngine::visible_pe_interval(
    const PeImage& module, std::uint64_t address) const {
    if (address < module.base() || address - module.base() >= module.size())
        return std::nullopt;
    if (staged_modules_.empty() || !staged_module_identities_.contains(&module))
        return VisiblePeInterval{module.base(), module.size()};
    auto begin = module.base();
    auto end = begin + module.size();
    const auto next = address_space_->regions.upper_bound(address);
    if (next != address_space_->regions.begin()) {
        const auto& preceding = std::prev(next)->second;
        // Mapped backing wins even when its permission or remaining width
        // makes a whole-width mapped access fail. Do not fall through to PE.
        if (address >= preceding.base && address - preceding.base < preceding.size())
            return std::nullopt;
        begin = std::max(begin, preceding.base + preceding.size());
    }
    if (next != address_space_->regions.end()) end = std::min(end, next->first);
    return VisiblePeInterval{begin, end - begin};
}

std::optional<std::uint64_t> ReferenceEngine::read_contiguous(
    std::uint64_t address, unsigned bytes) const {
    if (const auto value = read_mapped(address, bytes)) return value;
    if (const auto* module = module_at(address)) {
        const auto visible = visible_pe_interval(*module, address);
        if (!visible || bytes > visible->size - (address - visible->base))
            return std::nullopt;
        return module->try_read_contiguous(address, bytes);
    }
    return std::nullopt;
}

bool ReferenceEngine::write_contiguous(std::uint64_t address, unsigned bytes,
                                       std::uint64_t value) {
    if (write_mapped(address, bytes, value)) return true;
    auto* module = module_at(address);
    if (module == nullptr) return false;
    const auto visible = visible_pe_interval(*module, address);
    if (!visible || bytes > visible->size - (address - visible->base) ||
        !module->try_write_contiguous(address, bytes, value)) return false;
    const auto rva = address - module->base();
    // Preserve one generation increment for each byte in executable sections.
    unsigned executable_mask{};
    for (const auto& section : module->sections()) {
        if (!section.executable) continue;
        const auto first = std::max<std::uint64_t>(rva, section.rva);
        const auto last = std::min(rva + bytes,
            static_cast<std::uint64_t>(section.rva) + section.size);
        if (last > first) {
            executable_mask |= ((1U << (last - first)) - 1U) << (first - rva);
        }
    }
    record_memory_write(address, bytes, false);
    address_space_->code_generation += std::popcount(executable_mask);
    return true;
}

std::optional<std::uint64_t> ReferenceEngine::try_read_contiguous(
    std::uint64_t address, unsigned bytes) const {
    AddressSpaceLock lock(*address_space_);
    return read_contiguous(address, bytes);
}

bool ReferenceEngine::try_write_contiguous(std::uint64_t address, unsigned bytes,
                                           std::uint64_t value) {
    AddressSpaceLock lock(*address_space_);
    return write_contiguous(address, bytes, value);
}

std::uint64_t ReferenceEngine::advance_backing_identity() {
    if (address_space_->next_backing_identity == 0) {
        throw std::runtime_error("guest backing identity space exhausted");
    }
    return address_space_->next_backing_identity++;
}

std::array<std::uint64_t, 16> ReferenceEngine::reservation_backings(
    std::uint64_t address, unsigned bytes) const {
    assert(bytes <= 16);
    std::array<std::uint64_t, 16> identities{};
    for (unsigned byte = 0; byte < bytes; ++byte) {
        const auto current = address + byte;
        auto region = address_space_->regions.upper_bound(current);
        if (region != address_space_->regions.begin()) {
            const auto& backing = std::prev(region)->second;
            if (current - backing.base < backing.size()) {
                identities[byte] = backing.backing_identity;
                continue;
            }
        }
        if (const auto* module = module_at(current)) {
            if (const auto staged = staged_module_identities_.find(module);
                staged != staged_module_identities_.end()) {
                identities[byte] = staged->second;
            } else if (const auto published = address_space_->module_identities.find(module);
                       published != address_space_->module_identities.end()) {
                identities[byte] = published->second;
            }
        }
    }
    return identities;
}

std::uint64_t ReferenceEngine::advance_write_revision() {
    // On counter exhaustion permanently disable tracking rather than reuse an
    // identity that could falsely validate old buffer contents.
    if (address_space_->next_write_revision == 0) return 0;
    return ++address_space_->next_write_revision;
}

std::optional<std::uint64_t> ReferenceEngine::mapped_write_revision(
    std::uint64_t address, std::uint64_t size) const {
    if (size == 0 || address_space_->next_write_revision == 0) return std::nullopt;
    const MemoryRegion* region = nullptr;
    const auto contains_start = [address](const MemoryRegion* candidate) {
        return address >= candidate->base && address - candidate->base < candidate->size();
    };
    if (address_space_->checked_hints_usable && read_region_hint_ != nullptr &&
        read_hint_generation_ == address_space_->generation &&
        contains_start(read_region_hint_)) {
        region = read_region_hint_;
    } else if (address_space_->checked_hints_usable && write_region_hint_ != nullptr &&
               write_hint_generation_ == address_space_->generation &&
               contains_start(write_region_hint_)) {
        region = write_region_hint_;
    } else {
        auto entry = address_space_->regions.upper_bound(address);
        if (entry != address_space_->regions.begin()) region = &std::prev(entry)->second;
    }
    if (region == nullptr || !contains_start(region) || !region->readable ||
        region->external != nullptr || region->revision == 0 ||
        size > region->size() - static_cast<std::size_t>(address - region->base)) {
        return std::nullopt;
    }
    return region->revision;
}

std::optional<std::uint64_t> ReferenceEngine::write_revision(
    std::uint64_t address, std::uint64_t size) const {
    AddressSpaceLock lock(*address_space_);
    return mapped_write_revision(address, size);
}

std::optional<ReferenceEngine::BulkMemoryView> ReferenceEngine::bulk_memory_view(
    std::uint64_t address, bool writable) const {
    auto entry = address_space_->regions.upper_bound(address);
    if (entry != address_space_->regions.begin()) {
        auto& region = std::prev(entry)->second;
        if (address >= region.base && address - region.base < region.size()) {
            if (writable ? !region.writable : !region.readable) return std::nullopt;
            return BulkMemoryView{region.base, region.size(),
                region.external == nullptr ? region.bytes.data() : region.external,
                &region, nullptr};
        }
    }
    auto* module = module_at(address);
    if (module == nullptr) return std::nullopt;
    const auto visible = visible_pe_interval(*module, address);
    if (!visible) return std::nullopt;
    if (!writable) {
        return BulkMemoryView{visible->base, static_cast<std::size_t>(visible->size),
            module->bytes_.data() + (visible->base - module->base()), nullptr, module};
    }
    const auto rva = address - module->base();
    std::uint64_t extent_begin = rva;
    std::uint64_t extent_end = rva;
    for (const auto& section : module->sections()) {
        if (section.writable && rva >= section.rva &&
            rva - section.rva < section.size) {
            extent_end = std::max(extent_end, std::min<std::uint64_t>(
                module->bytes_.size(), static_cast<std::uint64_t>(section.rva) + section.size));
            extent_begin = std::min<std::uint64_t>(extent_begin, section.rva);
        }
    }
    if (extent_end == rva) return std::nullopt;
    extent_begin = std::max(extent_begin, visible->base - module->base());
    extent_end = std::min(extent_end, visible->base - module->base() + visible->size);
    return BulkMemoryView{module->base() + extent_begin,
        static_cast<std::size_t>(extent_end - extent_begin),
        module->bytes_.data() + static_cast<std::size_t>(extent_begin), nullptr, module};
}

void ReferenceEngine::record_bulk_write(const BulkMemoryView& view,
    std::uint64_t address, std::size_t bytes, std::uint64_t operations) {
    if (view.region != nullptr && view.region->external == nullptr) {
        auto& clock = address_space_->next_write_revision;
        if (clock != 0) clock = operations > UINT64_MAX - clock ? 0 : clock + operations;
        view.region->revision = clock;
    }
    // Dirty identity retains the bridge's successful 8-byte chunk/byte counts;
    // reservation epochs and executable generations retain every written byte.
    std::size_t remaining = bytes;
    auto current = address;
    while (remaining != 0) {
        const auto extent = static_cast<unsigned>(
            std::min<std::size_t>(remaining, UINT32_MAX));
        record_memory_write(current, extent, view.region && view.region->executable);
        current += extent;
        remaining -= extent;
    }
    if (view.image != nullptr) {
        const auto rva = address - view.image->base();
        // Count the union: overlapping executable sections do not count a
        // single byte twice, matching the existing checked PE write behavior.
        std::uint64_t cursor = rva;
        const auto end = rva + bytes;
        while (cursor < end) {
            std::uint64_t covered_end = cursor;
            std::uint64_t next = end;
            for (const auto& section : view.image->sections()) {
                if (!section.executable) continue;
                const auto first = static_cast<std::uint64_t>(section.rva);
                const auto last = first + section.size;
                if (first <= cursor && cursor < last) covered_end = std::max(covered_end, last);
                else if (first > cursor) next = std::min(next, first);
            }
            if (covered_end > cursor) {
                covered_end = std::min(covered_end, end);
                address_space_->code_generation += covered_end - cursor;
                cursor = covered_end;
            } else {
                cursor = next;
            }
        }
    }
}

void ReferenceEngine::read_bytes(std::uint64_t address,
                                 std::span<std::uint8_t> bytes) const {
    if (typeid(*this) != typeid(ReferenceEngine)) return GuestMemory::read_bytes(address, bytes);
    if (bytes.empty()) return;
    AddressSpaceLock lock(*address_space_);
    for (std::size_t completed = 0; completed < bytes.size();) {
        const auto current = address + completed;
        const auto view = bulk_memory_view(current, false);
        if (!view) {
            bytes[completed] = read8(current); // Preserve the checked fault text/order.
            ++completed;
            continue;
        }
        const auto offset = static_cast<std::size_t>(current - view->base);
        const auto count = std::min(bytes.size() - completed, view->size - offset);
        std::memmove(bytes.data() + completed, view->data + offset, count);
        completed += count;
    }
}

void ReferenceEngine::write_bytes(std::uint64_t address,
                                  std::span<const std::uint8_t> bytes) {
    if (typeid(*this) != typeid(ReferenceEngine)) return GuestMemory::write_bytes(address, bytes);
    if (bytes.empty()) return;
    AddressSpaceLock lock(*address_space_);
    for (std::size_t completed = 0; completed < bytes.size();) {
        const auto current = address + completed;
        const auto view = bulk_memory_view(current, true);
        if (!view) {
            write8(current, bytes[completed]);
            ++completed;
            continue;
        }
        const auto offset = static_cast<std::size_t>(current - view->base);
        const auto count = std::min(bytes.size() - completed, view->size - offset);
        std::memmove(view->data + offset, bytes.data() + completed, count);
        record_bulk_write(*view, current, count, count / 8 + count % 8);
        completed += count;
    }
}

void ReferenceEngine::fill_bytes(std::uint64_t address, std::uint8_t value,
                                 std::uint64_t count) {
    if (typeid(*this) != typeid(ReferenceEngine)) return GuestMemory::fill_bytes(address, value, count);
    if (count == 0) return;
    AddressSpaceLock lock(*address_space_);
    for (std::uint64_t completed = 0; completed < count;) {
        const auto current = address + completed;
        const auto view = bulk_memory_view(current, true);
        if (!view) {
            write8(current, value);
            ++completed;
            continue;
        }
        const auto offset = static_cast<std::size_t>(current - view->base);
        const auto extent = static_cast<std::size_t>(
            std::min<std::uint64_t>(count - completed, view->size - offset));
        std::memset(view->data + offset, value, extent);
        record_bulk_write(*view, current, extent, extent / 8 + extent % 8);
        completed += extent;
    }
}

void ReferenceEngine::copy_bytes(std::uint64_t destination, std::uint64_t source,
                                 std::uint64_t count, bool move) {
    if (typeid(*this) != typeid(ReferenceEngine))
        return GuestMemory::copy_bytes(destination, source, count, move);
    if (count == 0) return;
    AddressSpaceLock lock(*address_space_);
    const bool backward = move && destination > source && destination - source < count;
    const bool forward_overlap = !backward && destination > source && destination - source < count;
    for (std::uint64_t completed = 0; completed < count;) {
        const auto offset = backward ? count - completed - 1 : completed;
        const auto read_address = source + offset;
        const auto write_address = destination + offset;
        const auto from = bulk_memory_view(read_address, false);
        const auto to = bulk_memory_view(write_address, true);
        if (!from || !to) {
            write8(write_address, read8(read_address));
            ++completed;
            continue;
        }
        const auto from_offset = static_cast<std::size_t>(read_address - from->base);
        const auto to_offset = static_cast<std::size_t>(write_address - to->base);
        const auto extent = static_cast<std::size_t>(std::min<std::uint64_t>(count - completed,
            std::min(backward ? from_offset + 1 : from->size - from_offset,
                     backward ? to_offset + 1 : to->size - to_offset)));
        const auto start = backward ? offset + 1 - extent : offset;
        auto* output = to->data + static_cast<std::size_t>(destination + start - to->base);
        const auto* input = from->data + static_cast<std::size_t>(source + start - from->base);
        const auto host_from = reinterpret_cast<std::uintptr_t>(input);
        const auto host_to = reinterpret_cast<std::uintptr_t>(output);
        const bool aliased_external =
            ((from->region && from->region->external) || (to->region && to->region->external)) &&
            host_from < host_to + extent && host_to < host_from + extent &&
            host_to - host_from != destination - source;
        if (aliased_external) {
            // Different guest mappings can alias arbitrary host offsets. Keep
            // the existing chunk snapshots in that unusual case instead of
            // changing their forward/backward propagation through the alias.
            if (count - completed >= 8 &&
                (backward || destination <= source || destination - source >= 8)) {
                const auto chunk_offset = backward ? count - completed - 8 : completed;
                if (const auto word = read_contiguous(source + chunk_offset, 8)) {
                    if (write_contiguous(destination + chunk_offset, 8, *word)) {
                        completed += 8;
                        continue;
                    }
                }
            }
            write8(write_address, read8(read_address));
            ++completed;
            continue;
        }
        if (forward_overlap) {
            // Retain the established forward-byte memcpy result. Seed the
            // repeated pattern, then double the completed prefix using host
            // memmove; no source byte is fetched after it has been overwritten.
            const auto distance = static_cast<std::size_t>(destination - source);
            auto copied = std::min(extent, distance);
            std::memmove(output, input, copied);
            while (copied < extent) {
                const auto part = std::min(copied, extent - copied);
                std::memmove(output + copied, output, part);
                copied += part;
            }
        } else {
            std::memmove(output, input, extent);
        }
        const auto operations = forward_overlap && destination - source < 8
            ? extent : extent / 8 + extent % 8;
        record_bulk_write(*to, destination + start, extent, operations);
        completed += extent;
    }
}

std::vector<std::uint8_t> ReferenceEngine::read_string(
    std::uint64_t address, unsigned character_bytes, std::size_t maximum_characters,
    bool* terminated) const {
    if (typeid(*this) != typeid(ReferenceEngine))
        return GuestMemory::read_string(address, character_bytes, maximum_characters, terminated);
    std::vector<std::uint8_t> result;
    static_cast<void>(scan_string(address, character_bytes, maximum_characters, &result, terminated));
    return result;
}

std::size_t ReferenceEngine::string_length(std::uint64_t address,
    unsigned character_bytes, std::size_t maximum_characters, bool* terminated) const {
    if (typeid(*this) != typeid(ReferenceEngine))
        return GuestMemory::string_length(address, character_bytes, maximum_characters, terminated);
    return scan_string(address, character_bytes, maximum_characters, nullptr, terminated);
}

std::size_t ReferenceEngine::scan_string(std::uint64_t address,
    unsigned character_bytes, std::size_t maximum_characters,
    std::vector<std::uint8_t>* snapshot, bool* terminated) const {
    if (character_bytes != 1 && character_bytes != 2)
        throw std::runtime_error("invalid guest string character width");
    if (maximum_characters > SIZE_MAX / character_bytes)
        throw std::runtime_error("guest string span is too large");
    if (terminated) *terminated = false;
    if (maximum_characters == 0) return 0;
    AddressSpaceLock lock(*address_space_);
    for (std::size_t characters = 0; characters < maximum_characters;) {
        const auto current = address + characters * character_bytes;
        const auto view = bulk_memory_view(current, false);
        const auto offset = view ? static_cast<std::size_t>(current - view->base) : 0;
        const auto available = view ? (view->size - offset) / character_bytes : 0;
        if (available == 0) {
            const auto low = read8(current);
            const auto high = character_bytes == 2 ? read8(current + 1) : 0;
            if (low == 0 && high == 0) {
                if (terminated) *terminated = true;
                return characters;
            }
            if (snapshot) {
                snapshot->push_back(low);
                if (character_bytes == 2) snapshot->push_back(static_cast<std::uint8_t>(high));
            }
            ++characters;
            continue;
        }
        const auto count = std::min(maximum_characters - characters, available);
        const auto* input = view->data + offset;
        std::size_t nonzero = 0;
        if (character_bytes == 1) {
            const auto* zero = static_cast<const std::uint8_t*>(std::memchr(input, 0, count));
            nonzero = zero ? static_cast<std::size_t>(zero - input) : count;
        } else {
            while (nonzero < count && (input[nonzero * 2] != 0 || input[nonzero * 2 + 1] != 0))
                ++nonzero;
        }
        if (snapshot) snapshot->insert(snapshot->end(), input, input + nonzero * character_bytes);
        if (nonzero != count) {
            if (terminated) *terminated = true;
            return characters + nonzero;
        }
        characters += count;
    }
    return maximum_characters;
}

std::uint8_t ReferenceEngine::read8(std::uint64_t address) const {
    AddressSpaceLock lock(*address_space_);
    if (address_space_->checked_hints_usable && read_region_hint_ != nullptr &&
        read_hint_generation_ == address_space_->generation) {
        const auto& region = *read_region_hint_;
        if (address >= region.base && address - region.base < region.size()) {
            if (!region.readable) {
                throw std::runtime_error("guest read from inaccessible mapped memory");
            }
            return region.read(static_cast<std::size_t>(address - region.base));
        }
    }
    auto region_entry = address_space_->regions.upper_bound(address);
    if (region_entry != address_space_->regions.begin()) {
        --region_entry;
        const auto& region = region_entry->second;
        if (address >= region.base && address - region.base < region.size()) {
            if (!region.readable) {
                throw std::runtime_error("guest read from inaccessible mapped memory");
            }
            read_region_hint_ = &region;
            read_hint_generation_ = address_space_->generation;
            return region.read(static_cast<std::size_t>(address - region.base));
        }
    }
    if (const auto* module = module_at(address)) return module->read8(address);
    std::ostringstream message;
    message << "guest read outside mapped memory at 0x" << std::hex << address
            << " (pc 0x" << state_.pc << ')';
    if (const auto* module = module_at(state_.pc)) {
        message << " in " << module->source() << "+0x"
                << state_.pc - module->base();
    }
    throw std::runtime_error(message.str());
}

void ReferenceEngine::write8(std::uint64_t address, std::uint8_t value) {
    AddressSpaceLock lock(*address_space_);
    if (reservation_.valid && (address & ~15ULL) ==
                                  (reservation_.address & ~15ULL)) {
        reservation_.valid = false;
    }
    if (address_space_->checked_hints_usable && write_region_hint_ != nullptr &&
        write_hint_generation_ == address_space_->generation) {
        auto& region = *write_region_hint_;
        if (address >= region.base && address - region.base < region.size()) {
            if (!region.writable) {
                throw std::runtime_error("guest write to read-only mapped memory");
            }
            region.write(static_cast<std::size_t>(address - region.base), value);
            if (region.external == nullptr) region.revision = advance_write_revision();
            if (region.executable) ++address_space_->code_generation;
            ++address_space_->exclusive_write_epochs[
                exclusive_epoch_bucket(address)];
            return;
        }
    }
    auto region_entry = address_space_->regions.upper_bound(address);
    if (region_entry != address_space_->regions.begin()) {
        --region_entry;
        auto& region = region_entry->second;
        if (address >= region.base && address - region.base < region.size()) {
            if (!region.writable) {
                throw std::runtime_error("guest write to read-only mapped memory");
            }
            region.write(static_cast<std::size_t>(address - region.base), value);
            if (region.external == nullptr) region.revision = advance_write_revision();
            if (region.executable) ++address_space_->code_generation;
            write_region_hint_ = &region;
            write_hint_generation_ = address_space_->generation;
            ++address_space_->exclusive_write_epochs[
                exclusive_epoch_bucket(address)];
            return;
        }
    }
    if (auto* module = module_at(address)) {
            module->write8(address, value);
            const auto rva = address - module->base();
            for (const auto& section : module->sections()) {
                if (section.executable && rva >= section.rva &&
                    rva < static_cast<std::uint64_t>(section.rva) + section.size) {
                    ++address_space_->code_generation;
                    break;
                }
            }
            ++address_space_->exclusive_write_epochs[
                exclusive_epoch_bucket(address)];
            return;
    }
    std::ostringstream message;
    message << "guest write outside mapped memory at 0x" << std::hex << address
            << " (pc 0x" << state_.pc << ')';
    if (const auto* module = module_at(state_.pc)) {
        message << " in " << module->source() << "+0x"
                << state_.pc - module->base();
    }
    throw std::runtime_error(message.str());
}

std::uint64_t ReferenceEngine::reg(unsigned index, bool stack_pointer) const {
    if (index == 31) return stack_pointer ? state_.sp : 0;
    return state_.x[index];
}

void ReferenceEngine::set_reg(unsigned index, std::uint64_t value, bool is_64_bit,
                              bool stack_pointer) {
    if (!is_64_bit) value = static_cast<std::uint32_t>(value);
    if (index == 31) {
        if (stack_pointer) state_.sp = value;
    } else {
        state_.x[index] = value;
    }
}

bool ReferenceEngine::execute(std::uint32_t instruction) {
    const auto current_pc = state_.pc;
    const bool is_64_bit = (instruction & 0x80000000U) != 0;
    const auto rd = instruction & 31U;
    const auto rn = (instruction >> 5) & 31U;
    state_.pc += 4;

    if ((instruction & 0xbfa00000U) == 0x88200000U) {
        const bool load = (instruction & 0x00400000U) != 0;
        const bool wide = (instruction & 0x40000000U) != 0;
        const auto element_bytes = wide ? 8U : 4U;
        const auto bytes = element_bytes * 2U;
        const auto rt2 = (instruction >> 10) & 31U;
        const auto status = (instruction >> 16) & 31U;
        if (load ? (status != 31U || rd == rt2)
                 : (status == rd || status == rt2 ||
                    (status == rn && rn != 31U))) {
            throw std::runtime_error("invalid pair-exclusive register encoding");
        }
        const auto address = reg(rn, true);
        if ((address & (bytes - 1U)) != 0) {
            throw std::runtime_error("unaligned pair-exclusive access");
        }
        if (load) {
            std::array<std::uint64_t, 2> value{};
            for (unsigned byte = 0; byte < bytes; ++byte) {
                const auto element = byte / element_bytes;
                value[element] |= static_cast<std::uint64_t>(
                    read8(address + byte)) << ((byte % element_bytes) * 8);
            }
            if ((instruction & 0x00008000U) != 0) {
                std::atomic_thread_fence(std::memory_order_acquire);
            }
            set_reg(rd, value[0], wide);
            set_reg(rt2, value[1], wide);
            reservation_ = {address, bytes, true,
                            address_space_->exclusive_write_epochs[
                                exclusive_epoch_bucket(address)],
                            reservation_backings(address, bytes)};
        } else {
            const std::array<std::uint64_t, 2> value{reg(rd), reg(rt2)};
            const bool reserved = reservation_.valid &&
                reservation_.address == address && reservation_.bytes == bytes &&
                reservation_.granule_epoch ==
                    address_space_->exclusive_write_epochs[
                        exclusive_epoch_bucket(address)];
            const bool success = reserved &&
                reservation_.backing_identities == reservation_backings(address, bytes);
            reservation_.valid = false;
            if (reserved) {
                // An existing monitor still validates the complete target
                // before reporting replacement failure. Missing/protected
                // bytes retain the checked pair-store fault contract.
                for (unsigned byte = 0; byte < bytes; ++byte) {
                    const auto region = query_region(address + byte);
                    if (!region || !region->writable) {
                        throw std::runtime_error(
                            "guest pair-exclusive store to inaccessible memory");
                    }
                }
            }
            if (success) {
                if ((instruction & 0x00008000U) != 0) {
                    std::atomic_thread_fence(std::memory_order_release);
                }
                for (unsigned byte = 0; byte < bytes; ++byte) {
                    write8(address + byte, static_cast<std::uint8_t>(
                        value[byte / element_bytes] >>
                        ((byte % element_bytes) * 8)));
                }
            }
            set_reg(status, success ? 0 : 1, false);
        }
        return false;
    }

    const auto exclusive_class = instruction & 0x3fe07c00U;
    if (exclusive_class == 0x08407c00U &&
        ((instruction >> 16) & 31U) == 31U) {
        const auto address = reg(rn, true);
        const auto bytes = 1U << ((instruction >> 30) & 3U);
        if ((address & (bytes - 1U)) != 0) {
            throw std::runtime_error("unaligned exclusive access");
        }
        std::uint64_t value = 0;
        for (unsigned byte = 0; byte < bytes; ++byte) {
            value |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
        }
        if ((instruction & 0x00008000U) != 0) {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        set_reg(rd, value, bytes == 8);
        reservation_ = {address, bytes, true,
                        address_space_->exclusive_write_epochs[
                            exclusive_epoch_bucket(address)],
                        reservation_backings(address, bytes)};
        return false;
    }
    if (exclusive_class == 0x08007c00U) {
        const auto address = reg(rn, true);
        const auto bytes = 1U << ((instruction >> 30) & 3U);
        if ((address & (bytes - 1U)) != 0) {
            throw std::runtime_error("unaligned exclusive access");
        }
        const auto status = (instruction >> 16) & 31U;
        const auto value = reg(rd);
        const bool success = reservation_.valid && reservation_.address == address &&
                             reservation_.bytes == bytes &&
                             reservation_.backing_identities ==
                                 reservation_backings(address, bytes) &&
                             reservation_.granule_epoch ==
                                 address_space_->exclusive_write_epochs[
                                     exclusive_epoch_bucket(address)];
        reservation_.valid = false;
        if (success) {
            if ((instruction & 0x00008000U) != 0) {
                std::atomic_thread_fence(std::memory_order_release);
            }
            for (unsigned byte = 0; byte < bytes; ++byte) {
                write8(address + byte,
                       static_cast<std::uint8_t>(value >> (byte * 8)));
            }
        }
        set_reg(status, success ? 0 : 1, false);
        return false;
    }
    const auto atomic_cas_class = instruction & 0x3fa07c00U;
    if (atomic_cas_class == 0x08a07c00U ||
        atomic_cas_class == 0x08207c00U) {
        const bool pair = atomic_cas_class == 0x08207c00U;
        if (pair && (instruction & 0x80000000U) != 0) {
            throw std::runtime_error("invalid atomic compare-and-swap pair size");
        }
        const auto bytes = pair ? ((instruction & 0x40000000U) != 0 ? 8U : 4U)
                                : 1U << ((instruction >> 30) & 3U);
        const auto address = reg(rn, true);
        if ((address & (bytes * (pair ? 2U : 1U) - 1U)) != 0) {
            throw std::runtime_error("unaligned atomic compare-and-swap");
        }
        const auto expected_register = (instruction >> 16) & 31U;
        if (pair && ((expected_register | rd) & 1U) != 0) {
            throw std::runtime_error("invalid atomic compare-and-swap register pair");
        }
        const bool wide = bytes == 8;
        const auto mask = bytes == 8 ? UINT64_MAX : (1ULL << (bytes * 8)) - 1;
        const auto expected_low = reg(expected_register) & mask;
        const auto expected_high = pair ? reg(expected_register + 1) & mask : 0;
        const auto desired_low = reg(rd) & mask;
        const auto desired_high = pair ? reg(rd + 1) & mask : 0;
        const auto memory_low = wide ? read64(address) :
            bytes == 4 ? read32(address) :
            bytes == 2 ? static_cast<std::uint64_t>(read8(address) |
                (static_cast<unsigned>(read8(address + 1)) << 8)) : read8(address);
        const auto memory_high = pair ?
            (wide ? read64(address + bytes) : read32(address + bytes)) : 0;
        if ((instruction & 0x00400000U) != 0) {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        if (memory_low == expected_low &&
            (!pair || memory_high == expected_high)) {
            if ((instruction & 0x00008000U) != 0) {
                std::atomic_thread_fence(std::memory_order_release);
            }
            for (unsigned byte = 0; byte < bytes; ++byte) {
                write8(address + byte,
                       static_cast<std::uint8_t>(desired_low >> (byte * 8)));
                if (pair) {
                    write8(address + bytes + byte,
                           static_cast<std::uint8_t>(desired_high >> (byte * 8)));
                }
            }
        }
        set_reg(expected_register, memory_low, wide);
        if (pair) set_reg(expected_register + 1, memory_high, wide);
        return false;
    }
    const auto atomic_rmw_class = instruction & 0x3f208c00U;
    if (atomic_rmw_class == 0x38200000U || // LDADD/CLR/EOR/SET/SMAX/SMIN/UMAX/UMIN
        (atomic_rmw_class == 0x38208000U &&
         (instruction & 0x00007000U) == 0)) { // SWP
        const auto bytes = 1U << ((instruction >> 30) & 3U);
        const auto address = reg(rn, true);
        if ((address & (bytes - 1U)) != 0) {
            throw std::runtime_error("unaligned atomic read-modify-write");
        }
        const auto source_register = (instruction >> 16) & 31U;
        const auto mask = bytes == 8 ? UINT64_MAX : (1ULL << (bytes * 8)) - 1;
        const auto source = reg(source_register) & mask;
        std::uint64_t old = 0;
        for (unsigned byte = 0; byte < bytes; ++byte) {
            old |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
        }
        if ((instruction & 0x00800000U) != 0) {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        const auto operation = (instruction >> 12) & 7U;
        std::uint64_t replacement = 0;
        if (atomic_rmw_class == 0x38208000U) {
            replacement = source;
        } else {
            const auto sign_bit = 1ULL << (bytes * 8 - 1);
            switch (operation) {
            case 0: replacement = old + source; break;
            case 1: replacement = old & ~source; break;
            case 2: replacement = old ^ source; break;
            case 3: replacement = old | source; break;
            case 4: replacement = (old ^ sign_bit) >= (source ^ sign_bit)
                                      ? old : source; break;
            case 5: replacement = (old ^ sign_bit) <= (source ^ sign_bit)
                                      ? old : source; break;
            case 6: replacement = old >= source ? old : source; break;
            default: replacement = old <= source ? old : source; break;
            }
            replacement &= mask;
        }
        if ((instruction & 0x00400000U) != 0) {
            std::atomic_thread_fence(std::memory_order_release);
        }
        for (unsigned byte = 0; byte < bytes; ++byte) {
            write8(address + byte,
                   static_cast<std::uint8_t>(replacement >> (byte * 8)));
        }
        set_reg(rd, old, bytes == 8);
        return false;
    }
    const auto ordered_class = instruction & 0x3ffffc00U;
    if (ordered_class == 0x089ffc00U || ordered_class == 0x08dffc00U) {
        const auto bytes = 1U << (instruction >> 30);
        const auto address = reg(rn, true);
        if ((address & (bytes - 1U)) != 0) {
            throw std::runtime_error("unaligned ordered access");
        }
        if (ordered_class == 0x08dffc00U) {
            std::uint64_t value = 0;
            for (unsigned byte = 0; byte < bytes; ++byte) {
                value |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            set_reg(rd, value, bytes == 8);
        } else {
            std::atomic_thread_fence(std::memory_order_release);
            for (unsigned byte = 0; byte < bytes; ++byte) {
                write8(address + byte,
                       static_cast<std::uint8_t>(reg(rd) >> (byte * 8)));
            }
        }
        return false;
    }
    // FEAT_LRCPC LDAPR{B,H,,}: RCpc load-acquire. RCpc only relaxes LDAR's
    // ordering against earlier STLRs, so LDAR's acquire fence is sufficient.
    if (ordered_class == 0x38bfc000U) {
        const auto bytes = 1U << (instruction >> 30);
        const auto address = reg(rn, true);
        if ((address & (bytes - 1U)) != 0) {
            throw std::runtime_error("unaligned ordered access");
        }
        std::uint64_t value = 0;
        for (unsigned byte = 0; byte < bytes; ++byte) {
            value |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        set_reg(rd, value, bytes == 8);
        return false;
    }
    // FEAT_CRC32 CRC32{B,H,W,X} and CRC32C{B,H,W,X}: bit-reflected ISO and
    // Castagnoli polynomials with no initial or final inversion. Only the X
    // forms use sf = 1; other sf/size pairs are unallocated.
    if ((instruction & 0x7fe0e000U) == 0x1ac04000U &&
        ((instruction >> 31) != 0) == (((instruction >> 10) & 3U) == 3U)) {
        const auto bytes = 1U << ((instruction >> 10) & 3U);
        const std::uint32_t polynomial =
            (instruction & 0x00001000U) != 0 ? 0x82f63b78U : 0xedb88320U;
        auto crc = static_cast<std::uint32_t>(reg(rn));
        const auto data = reg((instruction >> 16) & 31U);
        for (unsigned byte = 0; byte < bytes; ++byte) {
            crc ^= static_cast<std::uint8_t>(data >> (byte * 8));
            for (unsigned bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (polynomial & (0U - (crc & 1U)));
            }
        }
        set_reg(rd, crc, false);
        return false;
    }
    // FEAT_LRCPC2 STLUR{B,H,,} / LDAPUR{B,H,,} / LDAPURS{B,H,W}: signed 9-bit
    // unscaled offset, no writeback. opc 00 store-release, 01 zero-extending
    // load, 10 sign-extend to X, 11 sign-extend to W. Unallocated sizes/opcs
    // fall through to the unsupported-instruction report.
    if ((instruction & 0x3f200c00U) == 0x19000000U) {
        const auto size = instruction >> 30;
        const auto opc = (instruction >> 22) & 3U;
        if (!((size == 2 && opc == 3) || (size == 3 && opc >= 2))) {
            const auto bytes = 1U << size;
            const auto offset = static_cast<std::int64_t>(
                static_cast<std::int32_t>(instruction << 11) >> 23);
            const auto address = reg(rn, true) + static_cast<std::uint64_t>(offset);
            if ((address & (bytes - 1U)) != 0) {
                throw std::runtime_error("unaligned ordered access");
            }
            if (opc == 0) {
                std::atomic_thread_fence(std::memory_order_release);
                for (unsigned byte = 0; byte < bytes; ++byte) {
                    write8(address + byte,
                           static_cast<std::uint8_t>(reg(rd) >> (byte * 8)));
                }
                return false;
            }
            std::uint64_t value = 0;
            for (unsigned byte = 0; byte < bytes; ++byte) {
                value |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (opc == 1) {
                set_reg(rd, value, bytes == 8);
            } else {
                const unsigned shift = 64U - bytes * 8U;
                const auto extended = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(value << shift) >> shift);
                set_reg(rd, extended, opc == 2);
            }
            return false;
        }
    }

    if ((instruction & 0x9ff80c00U) == 0x0f000400U) {
        const auto immediate = static_cast<std::uint8_t>(
            (((instruction >> 16) & 7U) << 5) | ((instruction >> 5) & 31U));
        const auto cmode = (instruction >> 12) & 15U;
        const bool op = (instruction & 0x20000000U) != 0;
        const bool quad = (instruction & 0x40000000U) != 0;
        std::uint64_t pattern = 0;
        bool modify_destination = false;
        if (cmode < 8) {
            auto word = static_cast<std::uint32_t>(immediate) << ((cmode / 2) * 8);
            modify_destination = (cmode & 1U) != 0; // ORR or BIC immediate.
            if (op && !modify_destination) word = ~word; // MVNI.
            pattern = static_cast<std::uint64_t>(word) |
                      (static_cast<std::uint64_t>(word) << 32);
        } else if (cmode < 12) {
            auto halfword = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(immediate) <<
                (((cmode - 8) / 2) * 8));
            modify_destination = (cmode & 1U) != 0;
            if (op && !modify_destination) halfword = ~halfword;
            pattern = static_cast<std::uint64_t>(halfword) *
                      0x0001000100010001ULL;
        } else if (cmode < 14) {
            const auto shift = cmode == 12 ? 8U : 16U;
            auto word = (static_cast<std::uint32_t>(immediate) << shift) |
                        ((1U << shift) - 1);
            if (op) word = ~word; // MVNI with a shifting mask.
            pattern = static_cast<std::uint64_t>(word) |
                      (static_cast<std::uint64_t>(word) << 32);
        } else if (cmode == 14) {
            if (!op) {
                pattern = static_cast<std::uint64_t>(immediate) *
                          0x0101010101010101ULL;
            } else {
                // Each immediate bit expands to an all-zero or all-one byte.
                for (unsigned byte = 0; byte < 8; ++byte) {
                    if (((immediate >> byte) & 1U) != 0) {
                        pattern |= 0xffULL << (byte * 8);
                    }
                }
            }
        } else {
            const auto sign = static_cast<std::uint64_t>(immediate >> 7);
            const auto exponent_bit = (immediate >> 6) & 1U;
            if (op) {
                if (!quad) {
                    throw std::runtime_error("invalid vector double immediate arrangement");
                }
                pattern = (sign << 63) |
                    (static_cast<std::uint64_t>(exponent_bit ^ 1U) << 62) |
                    (exponent_bit != 0 ? 0x3fc0000000000000ULL : 0ULL) |
                    (static_cast<std::uint64_t>((immediate >> 4) & 3U) << 52) |
                    (static_cast<std::uint64_t>(immediate & 15U) << 48);
            } else {
                const auto word = (static_cast<std::uint32_t>(sign) << 31) |
                    ((exponent_bit ^ 1U) << 30) |
                    (exponent_bit != 0 ? 0x3e000000U : 0U) |
                    (static_cast<std::uint32_t>((immediate >> 4) & 3U) << 23) |
                    (static_cast<std::uint32_t>(immediate & 15U) << 19);
                pattern = static_cast<std::uint64_t>(word) |
                          (static_cast<std::uint64_t>(word) << 32);
            }
        }
        if (modify_destination) {
            const auto old = state_.v[rd];
            state_.v[rd] = {op ? old[0] & ~pattern : old[0] | pattern,
                            quad ? (op ? old[1] & ~pattern : old[1] | pattern) : 0};
        } else {
            state_.v[rd] = {pattern, quad ? pattern : 0};
        }
        return false;
    }
    if ((instruction & 0xff201fe0U) == 0x1e201000U) {
        const auto immediate = (instruction >> 13) & 0xffU;
        const auto sign = immediate >> 7;
        const auto exponent_bit = (immediate >> 6) & 1U;
        const bool is_double = (instruction & 0x00400000U) != 0;
        if (is_double) {
            const auto pattern = (static_cast<std::uint64_t>(sign) << 63) |
                (static_cast<std::uint64_t>(exponent_bit ^ 1U) << 62) |
                (exponent_bit != 0 ? 0x3fc0000000000000ULL : 0ULL) |
                (static_cast<std::uint64_t>((immediate >> 4) & 3U) << 52) |
                (static_cast<std::uint64_t>(immediate & 15U) << 48);
            state_.v[rd] = {pattern, 0};
        } else {
            const auto pattern = (sign << 31) |
                ((exponent_bit ^ 1U) << 30) |
                (exponent_bit != 0 ? 0x3e000000U : 0U) |
                (((immediate >> 4) & 3U) << 23) |
                ((immediate & 15U) << 19);
            state_.v[rd] = {pattern, 0};
        }
        return false;
    }
    const auto float_move_class = instruction & 0xfffffc00U;
    if (float_move_class == 0x1e260000U ||
        float_move_class == 0x9e660000U) {
        const bool wide = (instruction & 0x80000000U) != 0;
        set_reg(rd, state_.v[rn][0], wide);
        return false;
    }
    if (float_move_class == 0x1e270000U ||
        float_move_class == 0x9e670000U) {
        const bool wide = (instruction & 0x80000000U) != 0;
        state_.v[rd] = {wide ? reg(rn) :
                           static_cast<std::uint32_t>(reg(rn)), 0};
        return false;
    }
    if ((instruction & 0x7f3e0000U) == 0x1e020000U) {
        const bool wide = (instruction & 0x80000000U) != 0;
        const bool is_double = (instruction & 0x00400000U) != 0;
        const bool is_unsigned = (instruction & 0x00010000U) != 0;
        const auto fractional_bits = 64U - ((instruction >> 10) & 63U);
        if (!wide && fractional_bits > 32) {
            throw std::runtime_error("invalid fixed-point conversion scale");
        }
        const auto source = wide ? reg(rn) :
            static_cast<std::uint32_t>(reg(rn));
        state_.v[rd] = {integer_to_floating_bits(source, wide, is_unsigned,
                                                is_double, state_,
                                                fractional_bits), 0};
        return false;
    }
    if ((instruction & 0x7f3e0000U) == 0x1e180000U) {
        const bool wide = (instruction & 0x80000000U) != 0;
        const bool is_double = (instruction & 0x00400000U) != 0;
        const bool is_unsigned = (instruction & 0x00010000U) != 0;
        const auto fractional_bits = 64U - ((instruction >> 10) & 63U);
        if (!wide && fractional_bits > 32) {
            throw std::runtime_error("invalid fixed-point conversion scale");
        }
        set_reg(rd, floating_to_integer_bits(
                    state_.v[rn][0], is_double, wide, is_unsigned,
                    IntegerRounding::zero, state_, fractional_bits), wide);
        return false;
    }
    const auto float_integer_class = instruction & 0x7fbefc00U;
    if (float_integer_class == 0x1e220000U ||
        float_integer_class == 0x1e200000U ||
        float_integer_class == 0x1e240000U ||
        float_integer_class == 0x1e280000U ||
        float_integer_class == 0x1e300000U ||
        float_integer_class == 0x1e380000U) {
        const bool wide = (instruction & 0x80000000U) != 0;
        const bool is_double = (instruction & 0x00400000U) != 0;
        const bool is_unsigned = (instruction & 0x00010000U) != 0;
        if (float_integer_class == 0x1e220000U) {
            const auto source = wide ? reg(rn) :
                static_cast<std::uint32_t>(reg(rn));
            state_.v[rd] = {integer_to_floating_bits(source, wide, is_unsigned,
                                                    is_double, state_), 0};
        } else {
            const auto rounding = float_integer_class == 0x1e200000U
                ? IntegerRounding::nearest_even :
                float_integer_class == 0x1e240000U
                ? IntegerRounding::nearest_away :
                float_integer_class == 0x1e280000U
                ? IntegerRounding::positive :
                float_integer_class == 0x1e300000U
                ? IntegerRounding::negative : IntegerRounding::zero;
            set_reg(rd, floating_to_integer_bits(state_.v[rn][0], is_double,
                                                 wide, is_unsigned, rounding,
                                                 state_), wide);
        }
        return false;
    }
    if ((instruction & 0xfffffc00U) == 0x1e624000U) {
        state_.v[rd] = {floating_narrow_double_to_single_bits(state_.v[rn][0],
                                                             state_), 0};
        return false;
    }
    if ((instruction & 0xfffffc00U) == 0x1e22c000U) {
        state_.v[rd] = {floating_widen_bits(
            static_cast<std::uint32_t>(state_.v[rn][0]), false, state_), 0};
        return false;
    }
    const auto half_precision_convert_class = instruction & 0xfffffc00U;
    if (half_precision_convert_class == 0x1e23c000U || // FCVT H, S
        half_precision_convert_class == 0x1e63c000U || // FCVT H, D
        half_precision_convert_class == 0x1ee24000U || // FCVT S, H
        half_precision_convert_class == 0x1ee2c000U) { // FCVT D, H
        const auto source = state_.v[rn][0];
        if (half_precision_convert_class == 0x1e23c000U ||
            half_precision_convert_class == 0x1e63c000U) {
            state_.v[rd] = {floating_narrow_to_half_bits(
                source, half_precision_convert_class == 0x1e63c000U,
                state_), 0};
        } else {
            const auto single = floating_widen_bits(source & 0xffffU, true,
                                                    state_);
            state_.v[rd] = {half_precision_convert_class == 0x1ee2c000U
                ? floating_widen_bits(single, false, state_) : single, 0};
        }
        return false;
    }
    const auto float_unary_class = instruction & 0xff3ffc00U;
    if (float_unary_class == 0x1e204000U ||
        float_unary_class == 0x1e214000U ||
        float_unary_class == 0x1e20c000U ||
        float_unary_class == 0x1e21c000U) {
        const bool is_double = (instruction & 0x00400000U) != 0;
        const auto sign = is_double ? 1ULL << 63 : 1ULL << 31;
        auto value = state_.v[rn][0];
        if (!is_double) value = static_cast<std::uint32_t>(value);
        if (float_unary_class == 0x1e214000U) value ^= sign;
        if (float_unary_class == 0x1e20c000U) value &= ~sign;
        if (float_unary_class == 0x1e21c000U) {
            value = floating_square_root_bits(value, is_double, state_);
        }
        state_.v[rd] = {value, 0};
        return false;
    }
    const auto float_binary_class = instruction & 0xff20fc00U;
    if (float_binary_class == 0x1e200800U ||
        float_binary_class == 0x1e201800U ||
        float_binary_class == 0x1e202800U ||
        float_binary_class == 0x1e203800U) {
        const auto rm = (instruction >> 16) & 31U;
        const bool is_double = (instruction & 0x00400000U) != 0;
        if (float_binary_class == 0x1e200800U) {
            state_.v[rd] = {floating_multiply_bits(state_.v[rn][0],
                                                   state_.v[rm][0], is_double,
                                                   state_), 0};
            return false;
        }
        if (float_binary_class == 0x1e202800U ||
            float_binary_class == 0x1e203800U) {
            state_.v[rd] = {floating_add_sub_bits(
                state_.v[rn][0], state_.v[rm][0], is_double,
                float_binary_class == 0x1e203800U, state_), 0};
            return false;
        }
        state_.v[rd] = {floating_divide_bits(state_.v[rn][0],
                                             state_.v[rm][0], is_double,
                                             state_), 0};
        return false;
    }
    if ((instruction & 0xff800000U) == 0x1f000000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto ra = (instruction >> 10) & 31U;
        const bool is_double = (instruction & 0x00400000U) != 0;
        const bool negate_addend = (instruction & 0x00200000U) != 0;
        const bool negate_product = negate_addend !=
                                    ((instruction & 0x00008000U) != 0);
        state_.v[rd] = {floating_multiply_add_bits(
            state_.v[ra][0], state_.v[rn][0], state_.v[rm][0], is_double,
            negate_product, negate_addend, state_), 0};
        return false;
    }
    if (float_binary_class == 0x1e208800U) {
        const auto rm = (instruction >> 16) & 31U;
        const bool is_double = (instruction & 0x00400000U) != 0;
        const auto product = floating_multiply_bits(state_.v[rn][0],
                                                    state_.v[rm][0], is_double,
                                                    state_);
        state_.v[rd] = {product ^ (is_double ? 1ULL << 63 : 1ULL << 31), 0};
        return false;
    }
    if (float_binary_class == 0x1e204800U ||
        float_binary_class == 0x1e205800U ||
        float_binary_class == 0x1e206800U ||
        float_binary_class == 0x1e207800U) {
        const auto rm = (instruction >> 16) & 31U;
        const bool is_double = (instruction & 0x00400000U) != 0;
        const bool minimum = (instruction & 0x00001000U) != 0;
        const bool numeric_if_quiet_nan = (instruction & 0x00002000U) != 0;
        state_.v[rd] = {floating_minmax_bits(state_.v[rn][0], state_.v[rm][0],
                                            is_double, minimum,
                                            numeric_if_quiet_nan, state_), 0};
        return false;
    }
    const auto conditional_float_compare_class = instruction & 0xff200c10U;
    if (conditional_float_compare_class == 0x1e200400U ||
        conditional_float_compare_class == 0x1e200410U) {
        const auto fp_type = (instruction >> 22) & 3U;
        if (fp_type > 1) {
            throw std::runtime_error("unsupported conditional float comparison type");
        }
        if (condition_holds((instruction >> 12) & 15U, state_.nzcv)) {
            const bool is_double = fp_type == 1;
            const bool signal_all_nans = conditional_float_compare_class == 0x1e200410U;
            const auto rm = (instruction >> 16) & 31U;
            state_.nzcv = floating_compare_flags(state_.v[rn][0],
                                                 state_.v[rm][0], is_double,
                                                 signal_all_nans, state_);
        } else {
            state_.nzcv = (instruction & 15U) << 28;
        }
        return false;
    }
    if ((instruction & 0xff20fc07U) == 0x1e202000U) {
        const bool is_double = (instruction & 0x00400000U) != 0;
        const bool compare_zero = (instruction & 8U) != 0;
        const auto rm = (instruction >> 16) & 31U;
        state_.nzcv = floating_compare_flags(
            state_.v[rn][0], compare_zero ? 0 : state_.v[rm][0],
            is_double, (instruction & 0x10U) != 0, state_);
        return false;
    }
    if ((instruction & 0xff200c00U) == 0x1e200c00U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto selected = condition_holds((instruction >> 12) & 15U,
                                              state_.nzcv) ? rn : rm;
        const bool is_double = (instruction & 0x00400000U) != 0;
        state_.v[rd] = {is_double ? state_.v[selected][0] :
                           static_cast<std::uint32_t>(state_.v[selected][0]), 0};
        return false;
    }
    if ((instruction & 0xffe0fc00U) == 0x5e000400U) {
        const auto imm5 = (instruction >> 16) & 31U;
        if (imm5 == 0) throw std::runtime_error("invalid scalar lane source");
        const auto size_shift = std::countr_zero(imm5);
        const auto element_bytes = 1U << size_shift;
        const auto lane_index = imm5 >> (size_shift + 1);
        const auto source_byte = lane_index * element_bytes;
        if (element_bytes > 8 || source_byte + element_bytes > 16) {
            throw std::runtime_error("invalid scalar lane arrangement");
        }
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        const auto value = (state_.v[rn][source_byte / 8] >>
                            ((source_byte % 8) * 8)) & mask;
        state_.v[rd] = {value, 0};
        return false;
    }
    if ((instruction & 0xbfe0fc00U) == 0x0e000c00U) {
        const auto element_bytes = (instruction >> 16) & 31U;
        if (element_bytes != 1 && element_bytes != 2 &&
            element_bytes != 4 && element_bytes != 8) {
            throw std::runtime_error("invalid vector DUP element size");
        }
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto source = reg(rn);
        std::array<std::uint64_t, 2> value{};
        for (unsigned byte = 0; byte < vector_bytes; ++byte) {
            const auto selected = byte % element_bytes;
            value[byte / 8] |= ((source >> (selected * 8)) & 0xffULL)
                               << ((byte % 8) * 8);
        }
        state_.v[rd] = value;
        return false;
    }
    if ((instruction & 0xbf20fc00U) == 0x0e000400U) {
        const auto imm5 = (instruction >> 16) & 31U;
        if (imm5 == 0) throw std::runtime_error("invalid vector DUP lane");
        const auto element_bytes = 1U << std::countr_zero(imm5);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto lane_index = imm5 >> (std::countr_zero(imm5) + 1);
        const auto source_byte = lane_index * element_bytes;
        // The source element is selected from all 128 bits of Vn even when
        // the destination arrangement has only 64 bits (8B/4H/2S).
        if (element_bytes > 8 || (element_bytes == 8 && vector_bytes == 8) ||
            source_byte + element_bytes > 16) {
            throw std::runtime_error("invalid vector DUP source lane");
        }
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> value{};
        for (unsigned byte = 0; byte < vector_bytes; ++byte) {
            const auto selected = source_byte + byte % element_bytes;
            const auto fragment = (source[selected / 8] >> ((selected % 8) * 8)) & 0xffULL;
            value[byte / 8] |= fragment << ((byte % 8) * 8);
        }
        state_.v[rd] = value;
        return false;
    }
    if ((instruction & 0xffe08400U) == 0x6e000400U) {
        const auto imm5 = (instruction >> 16) & 31U;
        if (imm5 == 0) throw std::runtime_error("invalid vector INS lane");
        const auto size_shift = std::countr_zero(imm5);
        const auto element_bytes = 1U << size_shift;
        const auto destination_lane = imm5 >> (size_shift + 1);
        const auto imm4 = (instruction >> 11) & 15U;
        const auto source_lane = imm4 >> size_shift;
        if (element_bytes > 8 || destination_lane * element_bytes >= 16 ||
            source_lane * element_bytes >= 16) {
            throw std::runtime_error("invalid vector INS element index");
        }
        const auto source = state_.v[rn];
        for (unsigned byte = 0; byte < element_bytes; ++byte) {
            const auto from = source_lane * element_bytes + byte;
            const auto to = destination_lane * element_bytes + byte;
            const auto fragment = (source[from / 8] >> ((from % 8) * 8)) & 0xffULL;
            auto& word = state_.v[rd][to / 8];
            const auto shift = (to % 8) * 8;
            word = (word & ~(0xffULL << shift)) | (fragment << shift);
        }
        return false;
    }
    if ((instruction & 0xffe0fc00U) == 0x4e001c00U) {
        const auto imm5 = (instruction >> 16) & 31U;
        if (imm5 == 0) throw std::runtime_error("invalid vector INS register lane");
        const auto size_shift = std::countr_zero(imm5);
        const auto element_bytes = 1U << size_shift;
        const auto destination_lane = imm5 >> (size_shift + 1);
        if (element_bytes > 8 || destination_lane * element_bytes >= 16) {
            throw std::runtime_error("invalid vector INS register index");
        }
        const auto source = reg(rn);
        for (unsigned byte = 0; byte < element_bytes; ++byte) {
            const auto to = destination_lane * element_bytes + byte;
            const auto fragment = (source >> (byte * 8)) & 0xffULL;
            auto& word = state_.v[rd][to / 8];
            const auto shift = (to % 8) * 8;
            word = (word & ~(0xffULL << shift)) | (fragment << shift);
        }
        return false;
    }
    if ((instruction & 0xdfbffc00U) == 0x5e21d800U) {
        const bool unsigned_source = (instruction & 0x20000000U) != 0;
        const bool double_lane = (instruction & 0x00400000U) != 0;
        state_.v[rd] = {integer_to_floating_bits(state_.v[rn][0],
                                                 double_lane, unsigned_source,
                                                 double_lane, state_), 0};
        return false;
    }
    const auto scalar_float_to_integer_class =
        (instruction & 0xdfbffc00U) & ~0x20000000U;
    if (scalar_float_to_integer_class == 0x5e21a800U || // FCVTNS/FCVTNU
        scalar_float_to_integer_class == 0x5e21c800U || // FCVTAS/FCVTAU
        scalar_float_to_integer_class == 0x5ea1a800U || // FCVTPS/FCVTPU
        scalar_float_to_integer_class == 0x5e21b800U || // FCVTMS/FCVTMU
        scalar_float_to_integer_class == 0x5ea1b800U) { // FCVTZS/FCVTZU
        const bool double_lane = (instruction & 0x00400000U) != 0;
        const bool unsigned_result = (instruction & 0x20000000U) != 0;
        const auto rounding = scalar_float_to_integer_class == 0x5e21a800U
            ? IntegerRounding::nearest_even :
            scalar_float_to_integer_class == 0x5e21c800U
            ? IntegerRounding::nearest_away :
            scalar_float_to_integer_class == 0x5ea1a800U
            ? IntegerRounding::positive :
            scalar_float_to_integer_class == 0x5e21b800U
            ? IntegerRounding::negative : IntegerRounding::zero;
        const auto source = state_.v[rn][0];
        state_.v[rd] = {floating_to_integer_bits(
                            source, double_lane, double_lane,
                            unsigned_result, rounding, state_), 0};
        return false;
    }
    if ((instruction & 0xdf80fc00U) == 0x5f00fc00U) {
        const auto immh = (instruction >> 19) & 15U;
        if (immh == 0 || immh == 1) {
            throw std::runtime_error("invalid scalar fixed-point conversion arrangement");
        }
        if (immh < 4) {
            throw std::runtime_error("unsupported scalar fixed-point half-precision conversion");
        }
        const bool double_lane = immh >= 8;
        const bool unsigned_result = (instruction & 0x20000000U) != 0;
        const auto fractional_bits = (double_lane ? 128U : 64U) -
                                     ((instruction >> 16) & 127U);
        const auto source = state_.v[rn][0];
        state_.v[rd] = {floating_to_integer_bits(
                            source, double_lane, double_lane,
                            unsigned_result, IntegerRounding::zero,
                            state_, fractional_bits), 0};
        return false;
    }
    if ((instruction & 0x9fbffc00U) == 0x0e21d800U) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool unsigned_source = (instruction & 0x20000000U) != 0;
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        if (double_lanes && !quad) {
            throw std::runtime_error("invalid vector integer-to-float arrangement");
        }
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto raw = source[byte / 8] >> ((byte % 8) * 8);
            const auto converted = integer_to_floating_bits(
                raw, double_lanes, unsigned_source, double_lanes, state_);
            result[byte / 8] |= converted << ((byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_fixed_conversion_class = instruction & 0x9f80fc00U;
    if (vector_fixed_conversion_class == 0x0f00e400U || // SCVTF/UCVTF
        vector_fixed_conversion_class == 0x0f00fc00U) { // FCVTZS/FCVTZU
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool unsigned_conversion = (instruction & 0x20000000U) != 0;
        const auto immh = (instruction >> 19) & 15U;
        const auto imm7 = (instruction >> 16) & 127U;
        if (immh == 0 || (immh >= 8 && !quad)) {
            throw std::runtime_error("invalid vector fixed-point conversion arrangement");
        }
        if (immh < 4) {
            throw std::runtime_error("unsupported vector fixed-point half-precision conversion");
        }
        const bool double_lanes = immh >= 8;
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const auto fractional_bits = (double_lanes ? 128U : 64U) - imm7;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto raw = source[byte / 8] >> shift;
            const auto converted = vector_fixed_conversion_class == 0x0f00e400U
                ? integer_to_floating_bits(raw, double_lanes,
                                           unsigned_conversion, double_lanes,
                                           state_, fractional_bits)
                : floating_to_integer_bits(raw, double_lanes,
                                           double_lanes, unsigned_conversion,
                                           IntegerRounding::zero, state_,
                                           fractional_bits);
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto indexed_float_class = instruction & 0xbfc0f400U;
    const auto indexed_float_base = indexed_float_class & ~0x10400000U;
    if (indexed_float_base == 0x0f801000U ||
        indexed_float_base == 0x0f805000U ||
        indexed_float_base == 0x0f809000U) {
        const bool scalar = (instruction & 0x10000000U) != 0;
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        if ((scalar && !quad) ||
            (double_lanes && (!quad || (instruction & 0x00200000U) != 0))) {
            throw std::runtime_error("invalid indexed floating-point arrangement");
        }
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = scalar ? 1U : (quad ? 16U : 8U) / lane_bytes;
        const auto rm = (instruction >> 16) & 31U;
        const auto index = double_lanes
            ? (instruction >> 11) & 1U
            : (((instruction >> 11) & 1U) << 1) |
              ((instruction >> 21) & 1U);
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto prior = state_.v[rd];
        const auto scalar_raw = right[(index * lane_bytes) / 8] >>
                                (((index * lane_bytes) % 8) * 8);
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto raw = left[byte / 8] >> ((byte % 8) * 8);
            const auto accumulator_raw = prior[byte / 8] >> ((byte % 8) * 8);
            std::uint64_t converted = 0;
            if (indexed_float_base == 0x0f809000U) {
                converted = floating_multiply_bits(raw, scalar_raw,
                                                   double_lanes, state_);
            } else {
                converted = floating_multiply_add_bits(
                    accumulator_raw, raw, scalar_raw, double_lanes,
                    indexed_float_base == 0x0f805000U, false, state_);
            }
            result[byte / 8] |= converted << ((byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto permutation_class = instruction & 0xbf20fc00U;
    if (permutation_class == 0x0e003800U || // ZIP1
        permutation_class == 0x0e007800U || // ZIP2
        permutation_class == 0x0e002800U || // TRN1
        permutation_class == 0x0e006800U || // TRN2
        permutation_class == 0x0e001800U || // UZP1
        permutation_class == 0x0e005800U) { // UZP2
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto lanes = vector_bytes / element_bytes;
        if (lanes < 2) {
            throw std::runtime_error("invalid vector permutation arrangement");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        const bool unzip = permutation_class == 0x0e001800U ||
                           permutation_class == 0x0e005800U;
        const bool transpose = permutation_class == 0x0e002800U ||
                               permutation_class == 0x0e006800U;
        const bool second = permutation_class == 0x0e007800U ||
                            permutation_class == 0x0e006800U ||
                            permutation_class == 0x0e005800U;
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto source_lane = unzip
                ? 2 * (lane % (lanes / 2)) + (second ? 1U : 0U)
                : transpose
                    ? 2 * (lane / 2) + (second ? 1U : 0U)
                    : lane / 2 + (second ? lanes / 2 : 0U);
            const auto& source = unzip ? (lane < lanes / 2 ? left : right)
                                       : (lane % 2 == 0 ? left : right);
            const auto source_byte = source_lane * element_bytes;
            const auto source_shift = (source_byte % 8) * 8;
            const auto value = (source[source_byte / 8] >> source_shift) & mask;
            const auto destination_byte = lane * element_bytes;
            result[destination_byte / 8] |=
                value << ((destination_byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_reverse_class = instruction & 0xbf3ffc00U;
    if (vector_reverse_class == 0x0e201800U || // REV16
        vector_reverse_class == 0x2e200800U || // REV32
        vector_reverse_class == 0x0e200800U) { // REV64
        const auto group_bytes = vector_reverse_class == 0x0e201800U ? 2U :
                                 vector_reverse_class == 0x2e200800U ? 4U : 8U;
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        if (element_bytes >= group_bytes) {
            throw std::runtime_error(
                group_bytes == 2 ? "invalid vector REV16 arrangement" :
                group_bytes == 4 ? "invalid vector REV32 arrangement" :
                                   "invalid vector REV64 arrangement");
        }
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto lanes_per_group = group_bytes / element_bytes;
        const auto mask = (1ULL << (element_bytes * 8)) - 1;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto group = (byte / group_bytes) * group_bytes;
            const auto lane = (byte - group) / element_bytes;
            const auto source_byte = group +
                (lanes_per_group - 1 - lane) * element_bytes;
            const auto value = (source[source_byte / 8] >>
                                ((source_byte % 8) * 8)) & mask;
            result[byte / 8] |= value << ((byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xbffffc00U) == 0x2e205800U) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const auto source = state_.v[rn];
        state_.v[rd] = {~source[0], quad ? ~source[1] : 0};
        return false;
    }
    if ((instruction & 0xbffffc00U) == 0x0e205800U) {
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; ++byte) {
            const auto value = static_cast<std::uint8_t>(
                source[byte / 8] >> ((byte % 8) * 8));
            result[byte / 8] |= static_cast<std::uint64_t>(std::popcount(value))
                                << ((byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_integer_unary_class = instruction & 0xbf3ffc00U;
    if ((instruction & 0xff3ffc00U) == 0x5e207800U) { // scalar SQABS
        const auto bits = 8U << ((instruction >> 22) & 3U);
        const auto mask = bits == 64 ? UINT64_MAX : (1ULL << bits) - 1;
        const auto sign = 1ULL << (bits - 1);
        const auto value = state_.v[rn][0] & mask;
        auto magnitude = (value & sign) != 0 ? (~value + 1) & mask : value;
        if (value == sign) {
            magnitude = sign - 1;
            state_.fpsr |= 1U << 27;
        }
        state_.v[rd] = {magnitude, 0};
        return false;
    }
    if (vector_integer_unary_class == 0x0e20b800U || // ABS
        vector_integer_unary_class == 0x2e20b800U) { // NEG
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes == 8 && vector_bytes == 8) {
            throw std::runtime_error("invalid vector ABS/NEG arrangement");
        }
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        const auto sign = 1ULL << (element_bytes * 8 - 1);
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto shift = (byte % 8) * 8;
            const auto raw = (source[word] >> shift) & mask;
            const bool negate = vector_integer_unary_class == 0x2e20b800U ||
                                (raw & sign) != 0;
            const auto value = negate ? (~raw + 1) & mask : raw;
            result[word] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xbfe08400U) == 0x2e000000U) {
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto offset = (instruction >> 11) & 15U;
        if (offset >= vector_bytes) {
            throw std::runtime_error("invalid vector EXT offset");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; ++byte) {
            const auto selected = byte + offset;
            const auto source_byte = selected % vector_bytes;
            const auto& source = selected < vector_bytes ? left : right;
            const auto value = (source[source_byte / 8] >>
                                ((source_byte % 8) * 8)) & 0xffU;
            result[byte / 8] |= value << ((byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_narrow_class = instruction & 0xbf3ffc00U;
    if (vector_narrow_class == 0x0e212800U || // XTN
        vector_narrow_class == 0x0e214800U || // SQXTN
        vector_narrow_class == 0x2e214800U || // UQXTN
        vector_narrow_class == 0x2e212800U) { // SQXTUN
        const auto size = (instruction >> 22) & 3U;
        if (size == 3) throw std::runtime_error("invalid vector narrowing arrangement");
        const bool upper = (instruction & 0x40000000U) != 0;
        const auto source_bytes = 2U << size;
        const auto destination_bytes = source_bytes / 2;
        const auto mask = (1ULL << (destination_bytes * 8)) - 1;
        const auto source_mask = source_bytes == 8 ? UINT64_MAX
                                                   : (1ULL << (source_bytes * 8)) - 1;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result = upper
            ? state_.v[rd] : std::array<std::uint64_t, 2>{};
        if (upper) result[1] = 0;
        for (unsigned lane = 0; lane < 8 / destination_bytes; ++lane) {
            const auto source_byte = lane * source_bytes;
            const auto raw = (source[source_byte / 8] >>
                              ((source_byte % 8) * 8)) & source_mask;
            auto value = raw & mask;
            if (vector_narrow_class == 0x2e214800U) {
                if (raw > mask) {
                    value = mask;
                    state_.fpsr |= 1U << 27;
                }
            } else if (vector_narrow_class == 0x0e214800U ||
                       vector_narrow_class == 0x2e212800U) {
                const auto source_bits = source_bytes * 8;
                const auto sign = 1ULL << (source_bits - 1);
                const auto signed_value = std::bit_cast<std::int64_t>((raw ^ sign) - sign);
                const auto minimum = vector_narrow_class == 0x0e214800U
                    ? -(1LL << (destination_bytes * 8 - 1)) : 0LL;
                const auto maximum = vector_narrow_class == 0x0e214800U
                    ? (1LL << (destination_bytes * 8 - 1)) - 1
                    : static_cast<std::int64_t>(mask);
                const auto clamped = std::clamp(signed_value, minimum, maximum);
                if (clamped != signed_value) state_.fpsr |= 1U << 27;
                value = static_cast<std::uint64_t>(clamped) & mask;
            }
            const auto destination_byte = (upper ? 8U : 0U) +
                                          lane * destination_bytes;
            result[destination_byte / 8] |= value <<
                ((destination_byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_float_class = instruction & 0xbfa0fc00U;
    const auto vector_float_narrow_class = instruction & 0xbffffc00U;
    if (vector_float_narrow_class == 0x0e216800U || // S to H
        vector_float_narrow_class == 0x0e616800U) { // D to S
        const auto source = state_.v[rn];
        std::uint64_t packed = 0;
        if (vector_float_narrow_class == 0x0e216800U) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                const auto raw = static_cast<std::uint32_t>(
                    source[lane / 2] >> ((lane % 2) * 32));
                packed |= static_cast<std::uint64_t>(
                    floating_narrow_to_half_bits(raw, false, state_)) <<
                    (lane * 16);
            }
        } else {
            const auto first = floating_narrow_double_to_single_bits(source[0],
                                                                    state_);
            const auto second = floating_narrow_double_to_single_bits(source[1],
                                                                     state_);
            packed = static_cast<std::uint64_t>(first) |
                     (static_cast<std::uint64_t>(second) << 32);
        }
        if ((instruction & 0x40000000U) != 0) {
            state_.v[rd][1] = packed; // FCVTN2 preserves the lower half.
        } else {
            state_.v[rd] = {packed, 0};
        }
        return false;
    }
    if ((instruction & 0xbf3ffc00U) == 0x0e217800U) {
        const auto size = (instruction >> 22) & 3U;
        if (size > 1) {
            throw std::runtime_error("invalid FCVTL widening arrangement");
        }
        const bool upper = (instruction & 0x40000000U) != 0;
        const auto source_bytes = 2U << size;
        const auto destination_bytes = source_bytes * 2;
        const auto lanes = 16U / destination_bytes;
        const auto source_mask = source_bytes == 2 ? 0xffffULL : UINT32_MAX;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto source_byte = (upper ? 8U : 0U) + lane * source_bytes;
            const auto raw = (source[source_byte / 8] >>
                              ((source_byte % 8) * 8)) & source_mask;
            const auto converted = floating_widen_bits(raw, source_bytes == 2,
                                                       state_);
            const auto destination_byte = lane * destination_bytes;
            result[destination_byte / 8] |= converted <<
                ((destination_byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_unary_float_class = instruction & 0xbffffc00U;
    if (vector_unary_float_class == 0x2ea0f800U ||
        vector_unary_float_class == 0x2ee0f800U ||
        vector_unary_float_class == 0x0ea0f800U ||
        vector_unary_float_class == 0x0ee0f800U ||
        vector_unary_float_class == 0x2ea1f800U ||
        vector_unary_float_class == 0x2ee1f800U) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        if (double_lanes && !quad) {
            throw std::runtime_error("invalid vector unary float arrangement");
        }
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const bool negate = vector_unary_float_class == 0x2ea0f800U ||
                            vector_unary_float_class == 0x2ee0f800U;
        const bool absolute = vector_unary_float_class == 0x0ea0f800U ||
                              vector_unary_float_class == 0x0ee0f800U;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto raw = source[byte / 8] >> shift;
            std::uint64_t converted = 0;
            if (double_lanes) {
                if (negate) converted = raw ^ (1ULL << 63);
                else if (absolute) converted = raw & ~(1ULL << 63);
                else converted = floating_square_root_bits(raw, true, state_);
            } else {
                const auto word = static_cast<std::uint32_t>(raw);
                if (negate) converted = word ^ (1U << 31);
                else if (absolute) converted = word & ~(1U << 31);
                else converted = floating_square_root_bits(word, false, state_);
            }
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_estimate_class = instruction & 0xbfbffc00U;
    if (vector_estimate_class == 0x0ea1d800U ||
        vector_estimate_class == 0x2ea1d800U) {
        const bool square_root = vector_estimate_class == 0x2ea1d800U;
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool wide = (instruction & 0x00400000U) != 0;
        if (wide && !quad) {
            throw std::runtime_error("invalid vector reciprocal-estimate arrangement");
        }
        const auto lane_bytes = wide ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto raw = source[byte / 8] >> shift;
            const auto converted = floating_reciprocal_estimate(
                wide ? raw : static_cast<std::uint32_t>(raw),
                wide, square_root, state_);
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto scalar_estimate_class = instruction & 0xffbffc00U;
    if (scalar_estimate_class == 0x5ea1d800U ||
        scalar_estimate_class == 0x7ea1d800U) {
        const bool square_root = scalar_estimate_class == 0x7ea1d800U;
        const bool wide = (instruction & 0x00400000U) != 0;
        const auto raw = wide ? state_.v[rn][0]
                              : static_cast<std::uint32_t>(state_.v[rn][0]);
        state_.v[rd] = {floating_reciprocal_estimate(raw, wide, square_root,
                                                     state_), 0};
        return false;
    }
    if (vector_float_class == 0x0e20fc00U ||
        vector_float_class == 0x0ea0fc00U) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        if (double_lanes && !quad) {
            throw std::runtime_error("invalid vector reciprocal-step arrangement");
        }
        const bool square_root = vector_float_class == 0x0ea0fc00U;
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto lhs_raw = left[byte / 8] >> shift;
            const auto rhs_raw = right[byte / 8] >> shift;
            const auto converted = floating_reciprocal_step_bits(
                lhs_raw, rhs_raw, double_lanes, square_root, state_);
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto scalar_reciprocal_step_class = instruction & 0xffa0fc00U;
    if (scalar_reciprocal_step_class == 0x5e20fc00U ||
        scalar_reciprocal_step_class == 0x5ea0fc00U) {
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        const bool square_root = scalar_reciprocal_step_class == 0x5ea0fc00U;
        const auto rm = (instruction >> 16) & 31U;
        state_.v[rd] = {floating_reciprocal_step_bits(
            state_.v[rn][0], state_.v[rm][0], double_lanes, square_root,
            state_), 0};
        return false;
    }
    const bool scalar_absolute_difference =
        (instruction & 0xffa0fc00U) == 0x7ea0d400U;
    const bool vector_absolute_difference =
        (instruction & 0xbfa0fc00U) == 0x2ea0d400U;
    if (scalar_absolute_difference || vector_absolute_difference) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool wide = (instruction & 0x00400000U) != 0;
        if (vector_absolute_difference && wide && !quad) {
            throw std::runtime_error("invalid vector FABD arrangement");
        }
        const auto lane_bytes = wide ? 8U : 4U;
        const auto lanes = scalar_absolute_difference ? 1U :
                           (quad ? 16U : 8U) / lane_bytes;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto lhs = left[byte / 8] >> shift;
            const auto rhs = right[byte / 8] >> shift;
            const auto converted = floating_absolute_difference_bits(
                wide ? lhs : static_cast<std::uint32_t>(lhs),
                wide ? rhs : static_cast<std::uint32_t>(rhs), wide, state_);
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    // FEAT_FRINTTS: FRINT32Z/FRINT32X/FRINT64Z/FRINT64X (scalar S/D, vector
    // 2S/4S/2D). X uses the FPCR rounding mode, Z rounds toward zero.
    {
        const auto scalar_class = instruction & 0xffbffc00U;
        const auto vector_class = instruction & 0xbfbffc00U;
        const bool scalar = scalar_class == 0x1e284000U || scalar_class == 0x1e28c000U ||
                            scalar_class == 0x1e294000U || scalar_class == 0x1e29c000U;
        const bool vector = vector_class == 0x0e21e800U || vector_class == 0x2e21e800U ||
                            vector_class == 0x0e21f800U || vector_class == 0x2e21f800U;
        if (scalar || vector) {
            const bool quad = (instruction & 0x40000000U) != 0;
            const bool wide = (instruction & 0x00400000U) != 0;
            if (vector && wide && !quad) throw std::runtime_error("invalid vector FRINT32/64 arrangement");
            const bool exact_mode = scalar ? (instruction & 0x00008000U) != 0 : (instruction & 0x20000000U) != 0;
            const unsigned intsize = (scalar ? (instruction & 0x00010000U) != 0 : (instruction & 0x00001000U) != 0)
                                         ? 64U : 32U;
            const auto current_mode = (state_.fpcr >> 22) & 3U;
            const auto rounding = !exact_mode ? IntegerRounding::zero
                : current_mode == 1 ? IntegerRounding::positive
                : current_mode == 2 ? IntegerRounding::negative
                : current_mode == 3 ? IntegerRounding::zero
                                    : IntegerRounding::nearest_even;
            const auto lane_bytes = wide ? 8U : 4U;
            const auto lanes = scalar ? 1U : (quad ? 16U : 8U) / lane_bytes;
            const auto source = state_.v[rn];
            std::array<std::uint64_t, 2> result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto byte = lane * lane_bytes;
                const auto shift = (byte % 8) * 8;
                const auto raw = source[byte / 8] >> shift;
                const auto converted = floating_round_int_n_bits(wide ? raw : static_cast<std::uint32_t>(raw),
                                                                 wide, rounding, intsize, state_);
                result[byte / 8] |= converted << shift;
            }
            state_.v[rd] = result;
            return false;
        }
    }
    // FEAT_FHM: FMLAL/FMLSL (lower halves), FMLAL2/FMLSL2 (upper halves),
    // vector and by element: Vd.2S/4S += (-)widen(Vn.H) * widen(Vm.H) fused.
    {
        const auto vector_class = instruction & 0xbf60fc00U;
        const auto element_class = instruction & 0xbfc0f400U;
        const bool vector = vector_class == 0x0e20ec00U || vector_class == 0x2e20cc00U;
        const bool element = element_class == 0x0f800000U || element_class == 0x0f804000U ||
                             element_class == 0x2f808000U || element_class == 0x2f80c000U;
        if (vector || element) {
            const bool quad = (instruction & 0x40000000U) != 0;
            const bool upper = (instruction & 0x20000000U) != 0;   // FMLAL2 / FMLSL2
            const bool subtract = vector ? (instruction & 0x00800000U) != 0 : (instruction & 0x00004000U) != 0;
            const unsigned lanes = quad ? 4U : 2U;
            const unsigned part = upper ? lanes : 0U;   // first half-precision element used
            const auto rm = vector ? (instruction >> 16) & 31U : (instruction >> 16) & 15U;
            const unsigned index = ((instruction >> 9) & 4U) | ((instruction >> 20) & 3U);   // H:L:M
            const auto half = [](const std::array<std::uint64_t, 2>& v, unsigned i) {
                return (v[i / 4] >> (16 * (i % 4))) & 0xffffU;
            };
            const auto n = state_.v[rn];
            const auto m = state_.v[rm];
            const auto acc = state_.v[rd];
            std::array<std::uint64_t, 2> result{};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const auto a = floating_fhm_widen_bits(half(n, part + lane), state_);
                const auto b = floating_fhm_widen_bits(half(m, element ? index : part + lane), state_);
                const auto addend = (acc[lane / 2] >> (32 * (lane % 2))) & 0xffffffffULL;
                const auto value = floating_multiply_add_bits(addend, a, b, false, subtract, false, state_);
                result[lane / 2] |= (value & 0xffffffffULL) << (32 * (lane % 2));
            }
            state_.v[rd] = result;
            return false;
        }
    }
    const auto scalar_round_class = instruction & 0xffbffc00U;
    const auto vector_round_class = instruction & 0xbfbffc00U;
    const bool scalar_round =
        scalar_round_class == 0x1e244000U || // FRINTN
        scalar_round_class == 0x1e254000U || // FRINTM
        scalar_round_class == 0x1e24c000U || // FRINTP
        scalar_round_class == 0x1e25c000U || // FRINTZ
        scalar_round_class == 0x1e264000U || // FRINTA
        scalar_round_class == 0x1e274000U || // FRINTX
        scalar_round_class == 0x1e27c000U;   // FRINTI
    const bool vector_round =
        vector_round_class == 0x0e218800U || // FRINTN
        vector_round_class == 0x0e219800U || // FRINTM
        vector_round_class == 0x0ea18800U || // FRINTP
        vector_round_class == 0x0ea19800U || // FRINTZ
        vector_round_class == 0x2e218800U || // FRINTA
        vector_round_class == 0x2e219800U || // FRINTX
        vector_round_class == 0x2ea19800U;   // FRINTI
    if (scalar_round || vector_round) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool wide = (instruction & 0x00400000U) != 0;
        if (vector_round && wide && !quad) {
            throw std::runtime_error("invalid vector FRINT arrangement");
        }
        const bool nearest_even = scalar_round
            ? scalar_round_class == 0x1e244000U
            : vector_round_class == 0x0e218800U;
        const bool nearest_away = scalar_round
            ? scalar_round_class == 0x1e264000U
            : vector_round_class == 0x2e218800U;
        const bool positive = scalar_round
            ? scalar_round_class == 0x1e24c000U
            : vector_round_class == 0x0ea18800U;
        const bool negative = scalar_round
            ? scalar_round_class == 0x1e254000U
            : vector_round_class == 0x0e219800U;
        const bool exact = scalar_round
            ? scalar_round_class == 0x1e274000U
            : vector_round_class == 0x2e219800U;
        const bool current = exact || (scalar_round
            ? scalar_round_class == 0x1e27c000U
            : vector_round_class == 0x2ea19800U);
        const auto current_mode = (state_.fpcr >> 22) & 3U;
        const auto rounding = current
            ? current_mode == 1 ? IntegerRounding::positive
            : current_mode == 2 ? IntegerRounding::negative
            : current_mode == 3 ? IntegerRounding::zero
                                : IntegerRounding::nearest_even
            : nearest_even ? IntegerRounding::nearest_even
            : nearest_away ? IntegerRounding::nearest_away
            : positive ? IntegerRounding::positive
            : negative ? IntegerRounding::negative
                       : IntegerRounding::zero;
        const auto lane_bytes = wide ? 8U : 4U;
        const auto lanes = scalar_round ? 1U :
                           (quad ? 16U : 8U) / lane_bytes;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto raw = source[byte / 8] >> shift;
            const auto converted = floating_round_integral_bits(
                wide ? raw : static_cast<std::uint32_t>(raw), wide,
                rounding, exact, state_);
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if (vector_float_class == 0x0e20d400U ||
        vector_float_class == 0x0ea0d400U ||
        vector_float_class == 0x2e20dc00U ||
        vector_float_class == 0x2e20fc00U ||
        vector_float_class == 0x0e20cc00U ||
        vector_float_class == 0x0ea0cc00U) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        if (double_lanes && !quad) {
            throw std::runtime_error("invalid vector floating-point arrangement");
        }
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto lhs_raw = left[byte / 8] >> shift;
            const auto rhs_raw = right[byte / 8] >> shift;
            const auto prior_raw = prior[byte / 8] >> shift;
            std::uint64_t converted = 0;
            if (vector_float_class == 0x0e20d400U ||
                vector_float_class == 0x0ea0d400U) {
                converted = floating_add_sub_bits(
                    lhs_raw, rhs_raw, double_lanes,
                    vector_float_class == 0x0ea0d400U, state_);
            } else if (vector_float_class == 0x2e20dc00U) {
                converted = floating_multiply_bits(lhs_raw, rhs_raw,
                                                   double_lanes, state_);
            } else if (vector_float_class == 0x2e20fc00U) {
                converted = floating_divide_bits(lhs_raw, rhs_raw,
                                                 double_lanes, state_);
            } else {
                converted = floating_multiply_add_bits(
                    prior_raw, lhs_raw, rhs_raw, double_lanes,
                    vector_float_class == 0x0ea0cc00U, false, state_);
            }
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if (vector_float_class == 0x0e20f400U ||
        vector_float_class == 0x0ea0f400U ||
        vector_float_class == 0x0e20c400U ||
        vector_float_class == 0x0ea0c400U) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool is_double = (instruction & 0x00400000U) != 0;
        if (is_double && !quad) {
            throw std::runtime_error("invalid vector floating-point arrangement");
        }
        const bool minimum = (instruction & 0x00800000U) != 0;
        const bool numeric_if_quiet_nan = (instruction & 0x00003000U) == 0;
        const auto lane_bytes = is_double ? 8U : 4U;
        const auto lane_count = (quad ? 16U : 8U) / lane_bytes;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lane_count; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto mask = is_double ? UINT64_MAX : UINT32_MAX;
            const auto lhs = (left[byte / 8] >> shift) & mask;
            const auto rhs = (right[byte / 8] >> shift) & mask;
            result[byte / 8] |= floating_minmax_bits(
                lhs, rhs, is_double, minimum, numeric_if_quiet_nan,
                state_) << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto scalar_absolute_compare_class = instruction & 0xffa0fc00U;
    const auto vector_absolute_compare_class = instruction & 0xbfa0fc00U;
    const bool scalar_absolute_compare =
        scalar_absolute_compare_class == 0x7ea0ec00U || // FACGT
        scalar_absolute_compare_class == 0x7e20ec00U;   // FACGE
    const bool vector_absolute_compare =
        vector_absolute_compare_class == 0x2ea0ec00U || // FACGT
        vector_absolute_compare_class == 0x2e20ec00U;   // FACGE
    if (scalar_absolute_compare || vector_absolute_compare) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool wide = (instruction & 0x00400000U) != 0;
        if (vector_absolute_compare && wide && !quad) {
            throw std::runtime_error("invalid vector FACG arrangement");
        }
        const bool greater = scalar_absolute_compare
            ? scalar_absolute_compare_class == 0x7ea0ec00U
            : vector_absolute_compare_class == 0x2ea0ec00U;
        const auto lane_bytes = wide ? 8U : 4U;
        const auto lanes = scalar_absolute_compare ? 1U :
                           (quad ? 16U : 8U) / lane_bytes;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto sign = wide ? 0x8000000000000000ULL : 0x80000000ULL;
        const auto exponent = wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
        const auto fraction = wide ? 0x000fffffffffffffULL : 0x007fffffULL;
        const auto result_mask = wide ? UINT64_MAX : UINT32_MAX;
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            auto lhs = (left[byte / 8] >> shift) & result_mask;
            auto rhs = (right[byte / 8] >> shift) & result_mask;
            const bool lhs_nan = (lhs & exponent) == exponent &&
                                 (lhs & fraction) != 0;
            const bool rhs_nan = (rhs & exponent) == exponent &&
                                 (rhs & fraction) != 0;
            if ((state_.fpcr & (1U << 24)) != 0) {
                if ((lhs & exponent) == 0 && (lhs & fraction) != 0) {
                    record_fp_exception(state_, 7, 15); // IDC / IDE
                    lhs &= sign;
                }
                if ((rhs & exponent) == 0 && (rhs & fraction) != 0) {
                    record_fp_exception(state_, 7, 15);
                    rhs &= sign;
                }
            }
            if (lhs_nan || rhs_nan) {
                // FACGT/FACGE are ordered comparisons: either kind of NaN
                // raises Invalid Operation, unlike FCMEQ.
                record_fp_exception(state_, 0, 8); // IOC / IOE
                continue;
            }
            const auto lhs_magnitude = lhs & ~sign;
            const auto rhs_magnitude = rhs & ~sign;
            const bool match = greater ? lhs_magnitude > rhs_magnitude
                                       : lhs_magnitude >= rhs_magnitude;
            if (match) result[byte / 8] |= result_mask << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_compare_register = instruction & 0xbfa0fc00U;
    const auto vector_compare_zero = instruction & 0xbfbffc00U;
    const bool floating_compare_register = vector_compare_register == 0x0e20e400U ||
                                           vector_compare_register == 0x2e20e400U ||
                                           vector_compare_register == 0x2ea0e400U;
    const bool floating_compare_zero = vector_compare_zero == 0x0ea0d800U ||
                                       vector_compare_zero == 0x2ea0c800U ||
                                       vector_compare_zero == 0x0ea0c800U ||
                                       vector_compare_zero == 0x2ea0d800U ||
                                       vector_compare_zero == 0x0ea0e800U;
    if (floating_compare_register || floating_compare_zero) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool is_double = (instruction & 0x00400000U) != 0;
        if (is_double && !quad) {
            throw std::runtime_error("invalid vector floating-point comparison arrangement");
        }
        const auto lane_bytes = is_double ? 8U : 4U;
        const auto lane_count = (quad ? 16U : 8U) / lane_bytes;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = floating_compare_register ? state_.v[rm]
                                                     : std::array<std::uint64_t, 2>{};
        const bool equal = vector_compare_register == 0x0e20e400U ||
                           vector_compare_zero == 0x0ea0d800U;
        const bool greater_equal = vector_compare_register == 0x2e20e400U ||
                                   vector_compare_zero == 0x2ea0c800U;
        const bool greater = vector_compare_register == 0x2ea0e400U ||
                             vector_compare_zero == 0x0ea0c800U;
        const bool less_equal = vector_compare_zero == 0x2ea0d800U;
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lane_count; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto lhs_raw = left[byte / 8] >> shift;
            const auto rhs_raw = right[byte / 8] >> shift;
            const auto flags = floating_compare_flags(
                lhs_raw, rhs_raw, is_double, !equal, state_);
            if (flags == 0x30000000U) continue;
            const bool matches = equal ? flags == 0x60000000U :
                                 greater_equal ? flags != 0x80000000U :
                                 greater ? flags == 0x20000000U :
                                 less_equal ? flags != 0x20000000U :
                                              flags == 0x80000000U;
            if (matches) {
                result[byte / 8] |= (is_double ? UINT64_MAX : UINT32_MAX) << shift;
            }
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_integer_conversion_class = instruction & 0xbfbffc00U;
    const auto signed_conversion_class =
        vector_integer_conversion_class & ~0x20000000U;
    if (signed_conversion_class == 0x0e21a800U || // FCVTNS/FCVTNU
        signed_conversion_class == 0x0e21c800U || // FCVTAS/FCVTAU
        signed_conversion_class == 0x0ea1a800U || // FCVTPS/FCVTPU
        signed_conversion_class == 0x0e21b800U || // FCVTMS/FCVTMU
        signed_conversion_class == 0x0ea1b800U) { // FCVTZS/FCVTZU
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        if (double_lanes && !quad) {
            throw std::runtime_error("invalid vector floating-to-integer arrangement");
        }
        const auto rounding = signed_conversion_class == 0x0e21a800U
            ? IntegerRounding::nearest_even :
            signed_conversion_class == 0x0e21c800U
            ? IntegerRounding::nearest_away :
            signed_conversion_class == 0x0ea1a800U
            ? IntegerRounding::positive :
            signed_conversion_class == 0x0e21b800U
            ? IntegerRounding::negative : IntegerRounding::zero;
        const bool unsigned_result = (instruction & 0x20000000U) != 0;
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto byte = lane * lane_bytes;
            const auto shift = (byte % 8) * 8;
            const auto raw = source[byte / 8] >> shift;
            const auto converted = floating_to_integer_bits(
                raw, double_lanes, double_lanes, unsigned_result, rounding,
                state_);
            result[byte / 8] |= converted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_add_sub_class = instruction & 0xbf20fc00U;
    if ((instruction & 0xff20fc00U) == 0x5e208400U) { // scalar SIMD ADD
        if (((instruction >> 22) & 3U) != 3) {
            throw std::runtime_error("invalid scalar SIMD ADD element size");
        }
        const auto left = state_.v[rn][0];
        const auto right = state_.v[(instruction >> 16) & 31U][0];
        state_.v[rd] = {left + right, 0};
        return false;
    }
    if (vector_add_sub_class == 0x0e208400U ||
        vector_add_sub_class == 0x2e208400U) {
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes == 8 && vector_bytes == 8) {
            throw std::runtime_error("invalid vector ADD/SUB arrangement");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto shift = (byte % 8) * 8;
            const auto lhs = (state_.v[rn][word] >> shift) & mask;
            const auto rhs = (state_.v[rm][word] >> shift) & mask;
            const auto value = vector_add_sub_class == 0x0e208400U
                ? lhs + rhs : lhs - rhs;
            result[word] |= (value & mask) << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto halving_arithmetic_class = instruction & 0xbf20fc00U;
    if (halving_arithmetic_class == 0x0e200400U || // SHADD
        halving_arithmetic_class == 0x2e200400U || // UHADD
        halving_arithmetic_class == 0x0e201400U || // SRHADD
        halving_arithmetic_class == 0x2e201400U || // URHADD
        halving_arithmetic_class == 0x0e202400U || // SHSUB
        halving_arithmetic_class == 0x2e202400U) { // UHSUB
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        if (element_bytes == 8) {
            throw std::runtime_error("invalid vector halving arithmetic arrangement");
        }
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto bits = element_bytes * 8;
        const auto mask = (1ULL << bits) - 1;
        const auto sign = 1ULL << (bits - 1);
        const bool signed_operands = (instruction & 0x20000000U) == 0;
        const bool subtract = (instruction & 0x00002000U) != 0;
        const bool rounding = (instruction & 0x00001000U) != 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto shift = (byte % 8) * 8;
            const auto lhs = (left[byte / 8] >> shift) & mask;
            const auto rhs = (right[byte / 8] >> shift) & mask;
            const auto signed_lhs = static_cast<std::int64_t>(lhs) -
                (signed_operands && (lhs & sign) != 0 ? 1LL << bits : 0);
            const auto signed_rhs = static_cast<std::int64_t>(rhs) -
                (signed_operands && (rhs & sign) != 0 ? 1LL << bits : 0);
            const auto numerator = signed_lhs +
                (subtract ? -signed_rhs : signed_rhs) + (rounding ? 1 : 0);
            auto halved = numerator / 2;
            if (numerator < 0 && numerator % 2 != 0) --halved;
            result[byte / 8] |= (static_cast<std::uint64_t>(halved) & mask)
                                << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xbf208c00U) == 0x0e000000U) {
        const bool extension = (instruction & 0x00001000U) != 0;
        const auto registers = ((instruction >> 13) & 3U) + 1U;
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto rm = (instruction >> 16) & 31U;
        const auto indices = state_.v[rm];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; ++byte) {
            const auto shift = (byte % 8) * 8;
            const auto selected = static_cast<unsigned>(
                (indices[byte / 8] >> shift) & 0xffU);
            std::uint64_t value = 0;
            if (selected < registers * 16) {
                const auto& table = state_.v[(rn + selected / 16) & 31U];
                const auto offset = selected % 16;
                value = (table[offset / 8] >> ((offset % 8) * 8)) & 0xffU;
            } else if (extension) {
                value = (prior[byte / 8] >> shift) & 0xffU;
            }
            result[byte / 8] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_logic_class = instruction & 0xbfe0fc00U;
    if (vector_logic_class == 0x0e201c00U ||
        vector_logic_class == 0x0e601c00U ||
        vector_logic_class == 0x0ea01c00U ||
        vector_logic_class == 0x0ee01c00U ||
        vector_logic_class == 0x2e201c00U ||
        vector_logic_class == 0x2e601c00U ||
        vector_logic_class == 0x2ea01c00U ||
        vector_logic_class == 0x2ee01c00U) {
        const auto rm = (instruction >> 16) & 31U;
        const bool quad = (instruction & 0x40000000U) != 0;
        std::array<std::uint64_t, 2> value{};
        for (unsigned word = 0; word < (quad ? 2U : 1U); ++word) {
            const auto lhs = state_.v[rn][word];
            const auto rhs = state_.v[rm][word];
            const auto prior = state_.v[rd][word];
            value[word] = vector_logic_class == 0x0e201c00U ? lhs & rhs :
                          vector_logic_class == 0x0e601c00U ? lhs & ~rhs :
                          vector_logic_class == 0x0ea01c00U ? lhs | rhs :
                          vector_logic_class == 0x0ee01c00U ? lhs | ~rhs :
                          vector_logic_class == 0x2e201c00U ? lhs ^ rhs :
                          vector_logic_class == 0x2e601c00U
                              ? (prior & lhs) | (~prior & rhs) :
                          vector_logic_class == 0x2ea01c00U
                              ? (prior & ~rhs) | (lhs & rhs) :
                                (prior & rhs) | (lhs & ~rhs);
        }
        state_.v[rd] = value;
        return false;
    }
    const auto vector_leading_count_class = instruction & 0xbf3ffc00U;
    if (vector_leading_count_class == 0x0e204800U || // CLS
        vector_leading_count_class == 0x2e204800U) { // CLZ
        const auto size = (instruction >> 22) & 3U;
        if (size == 3) {
            throw std::runtime_error("invalid vector leading-count element size");
        }
        const auto element_bits = 8U << size;
        const auto lanes = (instruction & 0x40000000U ? 128U : 64U) /
                           element_bits;
        const auto mask = (1ULL << element_bits) - 1;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto bit = lane * element_bits;
            const auto value = (source[bit / 64] >> (bit % 64)) & mask;
            const auto aligned = value << (64U - element_bits);
            unsigned count = std::min<unsigned>(
                element_bits, vector_leading_count_class == 0x2e204800U ||
                (value & (1ULL << (element_bits - 1))) == 0
                    ? std::countl_zero(aligned) : std::countl_one(aligned));
            if (vector_leading_count_class == 0x0e204800U) --count;
            result[bit / 64] |= static_cast<std::uint64_t>(count) <<
                                (bit % 64);
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xbf3ffc00U) == 0x2e213800U) {
        // SHLL/SHLL2 shift each source element by its entire width. This is
        // a distinct encoding from the immediate SSHLL/USHLL family below.
        const auto size = (instruction >> 22) & 3U;
        if (size == 3) throw std::runtime_error("invalid SHLL element size");
        const auto source_bits = 8U << size;
        const auto destination_bits = source_bits * 2;
        const auto source_mask = (1ULL << source_bits) - 1;
        const bool upper_half = (instruction & 0x40000000U) != 0;
        const auto source = state_.v[rn][upper_half ? 1 : 0];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < 64U / source_bits; ++lane) {
            const auto element = (source >> (lane * source_bits)) & source_mask;
            const auto output_bit = lane * destination_bits;
            result[output_bit / 64] |= (element << source_bits) <<
                                       (output_bit % 64);
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0x9f80fc00U) == 0x0f00a400U) {
        const auto immh = (instruction >> 19) & 15U;
        const auto immb = (instruction >> 16) & 7U;
        if (immh == 0 || immh >= 8) {
            throw std::runtime_error("invalid widening-shift element size");
        }
        const auto source_bits = 1U << (std::bit_width(immh) + 2U);
        const auto shift = ((immh << 3) | immb) - source_bits;
        const auto output_bits = source_bits * 2;
        const auto source_mask = (1ULL << source_bits) - 1;
        const auto output_mask = output_bits == 64 ? UINT64_MAX
                                                    : (1ULL << output_bits) - 1;
        const bool upper_half = (instruction & 0x40000000U) != 0;
        const bool unsigned_source = (instruction & 0x20000000U) != 0;
        const auto source = state_.v[rn][upper_half ? 1 : 0];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < 64U / source_bits; ++lane) {
            const auto raw = (source >> (lane * source_bits)) & source_mask;
            const auto sign = 1ULL << (source_bits - 1);
            const auto widened = unsigned_source ? raw : (raw ^ sign) - sign;
            const auto shifted = (widened << shift) & output_mask;
            const auto output_bit = lane * output_bits;
            result[output_bit / 64] |= shifted << (output_bit % 64);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_right_shift_class = instruction & 0x9f80fc00U;
    if (vector_right_shift_class == 0x0f000400U || // SSHR/USHR
        vector_right_shift_class == 0x0f002400U || // SRSHR/URSHR
        vector_right_shift_class == 0x1f000400U || // scalar SSHR/USHR
        vector_right_shift_class == 0x1f002400U) { // scalar SRSHR/URSHR
        const auto immh = (instruction >> 19) & 15U;
        const auto immb = (instruction >> 16) & 7U;
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool scalar = (vector_right_shift_class & 0x10000000U) != 0;
        if (immh == 0 || (scalar ? immh < 8 : immh >= 8 && !quad)) {
            throw std::runtime_error("invalid vector right-shift arrangement");
        }
        const auto element_bits = 1U << (std::bit_width(immh) + 2U);
        const auto shift = element_bits * 2 - ((immh << 3) | immb);
        const auto element_bytes = element_bits / 8;
        const auto vector_bytes = scalar ? 8U : quad ? 16U : 8U;
        const auto mask = element_bits == 64 ? UINT64_MAX
                                             : (1ULL << element_bits) - 1;
        const auto sign = 1ULL << (element_bits - 1);
        const bool signed_source = (instruction & 0x20000000U) == 0;
        const bool rounding = (vector_right_shift_class & 0x00002000U) != 0;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto lane_shift = (byte % 8) * 8;
            const auto raw = (source[byte / 8] >> lane_shift) & mask;
            auto value = shift == element_bits ? 0ULL : raw >> shift;
            if (signed_source && (raw & sign) != 0) {
                value |= shift == element_bits ? mask
                                               : (mask << (element_bits - shift)) & mask;
            }
            if (rounding) {
                const auto remainder_mask = shift == element_bits
                    ? mask : (1ULL << shift) - 1;
                if ((raw & remainder_mask) >= (1ULL << (shift - 1))) {
                    value = (value + 1) & mask;
                }
            }
            result[byte / 8] |= value << lane_shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto shift_accumulate_class = instruction & 0xbf80fc00U;
    if (shift_accumulate_class == 0x0f001400U || // SSRA
        shift_accumulate_class == 0x2f001400U || // USRA
        shift_accumulate_class == 0x0f003400U || // SRSRA
        shift_accumulate_class == 0x2f003400U || // URSRA
        shift_accumulate_class == 0x1f001400U || // scalar SSRA
        shift_accumulate_class == 0x3f001400U || // scalar USRA
        shift_accumulate_class == 0x1f003400U || // scalar SRSRA
        shift_accumulate_class == 0x3f003400U) { // scalar URSRA
        const auto immh = (instruction >> 19) & 15U;
        const auto immb = (instruction >> 16) & 7U;
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool scalar = (instruction & 0x10000000U) != 0;
        if (immh == 0 || (scalar ? immh < 8 : immh >= 8 && !quad)) {
            throw std::runtime_error("invalid vector shift-accumulate arrangement");
        }
        const auto element_bits = 1U << (std::bit_width(immh) + 2U);
        const auto amount = element_bits * 2 - ((immh << 3) | immb);
        const auto element_bytes = element_bits / 8;
        const auto vector_bytes = scalar ? 8U : quad ? 16U : 8U;
        const auto mask = element_bits == 64 ? UINT64_MAX
                                             : (1ULL << element_bits) - 1;
        const auto sign = 1ULL << (element_bits - 1);
        const bool signed_source = (instruction & 0x20000000U) == 0;
        const bool rounding = (instruction & 0x00002000U) != 0;
        const auto source = state_.v[rn];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto lane_shift = (byte % 8) * 8;
            const auto raw = (source[byte / 8] >> lane_shift) & mask;
            const auto old = (prior[byte / 8] >> lane_shift) & mask;
            auto shifted = amount == element_bits ? 0ULL : raw >> amount;
            if (signed_source && (raw & sign) != 0) {
                shifted |= amount == element_bits ? mask
                    : (mask << (element_bits - amount)) & mask;
            }
            if (rounding) {
                const auto remainder_mask = amount == element_bits
                    ? mask : (1ULL << amount) - 1;
                if ((raw & remainder_mask) >= (1ULL << (amount - 1))) {
                    shifted = (shifted + 1) & mask;
                }
            }
            result[byte / 8] |= ((old + shifted) & mask) << lane_shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto narrowing_shift_class = instruction & 0xbf80fc00U;
    if (narrowing_shift_class == 0x0f008400U || // SHRN/SHRN2
        narrowing_shift_class == 0x0f008c00U) { // RSHRN/RSHRN2
        const auto immh = (instruction >> 19) & 15U;
        const auto immb = (instruction >> 16) & 7U;
        if (immh == 0 || immh >= 8) {
            throw std::runtime_error("invalid vector narrowing-shift element size");
        }
        const auto source_bits = 1U << (std::bit_width(immh) + 3U);
        const auto amount = source_bits - ((immh << 3) | immb);
        const auto source_bytes = source_bits / 8;
        const auto destination_bytes = source_bytes / 2;
        const auto destination_mask = (1ULL << (destination_bytes * 8)) - 1;
        const bool upper = (instruction & 0x40000000U) != 0;
        const bool rounding = narrowing_shift_class == 0x0f008c00U;
        const auto source = state_.v[rn];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result = upper
            ? prior : std::array<std::uint64_t, 2>{};
        if (upper) result[1] = 0;
        for (unsigned lane = 0; lane < 16U / source_bytes; ++lane) {
            const auto source_byte = lane * source_bytes;
            const auto raw = source[source_byte / 8] >>
                             ((source_byte % 8) * 8);
            auto value = raw >> amount;
            if (rounding) {
                const auto remainder_mask = (1ULL << amount) - 1;
                if ((raw & remainder_mask) >= (1ULL << (amount - 1))) {
                    ++value;
                }
            }
            const auto destination_byte = (upper ? 8U : 0U) +
                                          lane * destination_bytes;
            result[destination_byte / 8] |= (value & destination_mask) <<
                ((destination_byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto saturating_narrow_vector_class = instruction & 0xbf80fc00U;
    const auto saturating_narrow_scalar_class = instruction & 0xff80fc00U;
    const bool saturating_narrow_vector =
        saturating_narrow_vector_class == 0x0f009400U || // SQSHRN
        saturating_narrow_vector_class == 0x0f009c00U || // SQRSHRN
        saturating_narrow_vector_class == 0x2f009400U || // UQSHRN
        saturating_narrow_vector_class == 0x2f009c00U || // UQRSHRN
        saturating_narrow_vector_class == 0x2f008400U || // SQSHRUN
        saturating_narrow_vector_class == 0x2f008c00U;   // SQRSHRUN
    const bool saturating_narrow_scalar =
        saturating_narrow_scalar_class == 0x5f009400U || // SQSHRN
        saturating_narrow_scalar_class == 0x5f009c00U || // SQRSHRN
        saturating_narrow_scalar_class == 0x7f009400U || // UQSHRN
        saturating_narrow_scalar_class == 0x7f009c00U || // UQRSHRN
        saturating_narrow_scalar_class == 0x7f008400U || // SQSHRUN
        saturating_narrow_scalar_class == 0x7f008c00U;   // SQRSHRUN
    if (saturating_narrow_vector || saturating_narrow_scalar) {
        const auto immh = (instruction >> 19) & 15U;
        const auto immb = (instruction >> 16) & 7U;
        if (immh == 0 || immh >= 8) {
            throw std::runtime_error("invalid saturating narrowing-shift element size");
        }
        const auto source_bits = 1U << (std::bit_width(immh) + 3U);
        const auto amount = source_bits - ((immh << 3) | immb);
        const auto source_bytes = source_bits / 8;
        const auto destination_bytes = source_bytes / 2;
        const auto destination_bits = destination_bytes * 8;
        const auto source_mask = source_bits == 64 ? UINT64_MAX
                                                   : (1ULL << source_bits) - 1;
        const auto destination_mask = (1ULL << destination_bits) - 1;
        const auto operation = instruction & 0x0000fc00U;
        const bool rounding = (operation & 0x00000800U) != 0;
        const bool signed_to_unsigned = (operation & 0x00001000U) == 0;
        const bool unsigned_destination = (instruction & 0x20000000U) != 0;
        const bool unsigned_source = unsigned_destination && !signed_to_unsigned;
        const bool scalar = saturating_narrow_scalar;
        const bool upper = !scalar && (instruction & 0x40000000U) != 0;
        const auto source = state_.v[rn];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result = upper
            ? prior : std::array<std::uint64_t, 2>{};
        if (upper) result[1] = 0;
        const auto lanes = scalar ? 1U : 16U / source_bytes;
        const auto divisor = 1LL << amount;
        const auto minimum = unsigned_destination ? 0LL
            : -(1LL << (destination_bits - 1));
        const auto maximum = unsigned_destination
            ? static_cast<std::int64_t>(destination_mask)
            : (1LL << (destination_bits - 1)) - 1;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto source_byte = lane * source_bytes;
            const auto raw = (source[source_byte / 8] >>
                              ((source_byte % 8) * 8)) & source_mask;
            std::int64_t shifted;
            if (unsigned_source) {
                const auto remainder_mask = static_cast<std::uint64_t>(divisor - 1);
                shifted = static_cast<std::int64_t>(raw >> amount);
                if (rounding && (raw & remainder_mask) >=
                                    static_cast<std::uint64_t>(divisor / 2)) {
                    ++shifted;
                }
            } else {
                const auto sign = 1ULL << (source_bits - 1);
                const auto signed_raw = source_bits == 64
                    ? std::bit_cast<std::int64_t>(raw)
                    : static_cast<std::int64_t>(raw) -
                      ((raw & sign) != 0 ? 1LL << source_bits : 0);
                shifted = signed_raw / divisor;
                auto remainder = signed_raw % divisor;
                if (remainder < 0) {
                    --shifted;
                    remainder += divisor;
                }
                if (rounding && remainder >= divisor / 2) ++shifted;
            }
            const auto clamped = std::clamp(shifted, minimum, maximum);
            if (clamped != shifted) state_.fpsr |= 1U << 27;
            const auto destination_byte = (upper ? 8U : 0U) +
                                          lane * destination_bytes;
            result[destination_byte / 8] |=
                (static_cast<std::uint64_t>(clamped) & destination_mask) <<
                ((destination_byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_saturating_shift_class = instruction & 0xbf80fc00U;
    const auto scalar_saturating_shift_class = instruction & 0xff80fc00U;
    const bool vector_saturating_shift =
        vector_saturating_shift_class == 0x0f007400U || // SQSHL
        vector_saturating_shift_class == 0x2f007400U || // UQSHL
        vector_saturating_shift_class == 0x2f006400U;   // SQSHLU
    const bool scalar_saturating_shift =
        scalar_saturating_shift_class == 0x5f007400U || // SQSHL
        scalar_saturating_shift_class == 0x7f007400U || // UQSHL
        scalar_saturating_shift_class == 0x7f006400U;   // SQSHLU
    if (vector_saturating_shift || scalar_saturating_shift) {
        const auto immh = (instruction >> 19) & 15U;
        const auto immb = (instruction >> 16) & 7U;
        const bool quad = (instruction & 0x40000000U) != 0;
        if (immh == 0 || (vector_saturating_shift && immh >= 8 && !quad)) {
            throw std::runtime_error("invalid saturating left-shift arrangement");
        }
        const auto element_bits = 1U << (std::bit_width(immh) + 2U);
        const auto amount = ((immh << 3) | immb) - element_bits;
        const auto element_bytes = element_bits / 8;
        const auto vector_bytes = scalar_saturating_shift ? element_bytes :
                                  (quad ? 16U : 8U);
        const auto mask = element_bits == 64 ? UINT64_MAX
                                             : (1ULL << element_bits) - 1;
        const auto sign = 1ULL << (element_bits - 1);
        const bool signed_destination =
            vector_saturating_shift_class == 0x0f007400U ||
            scalar_saturating_shift_class == 0x5f007400U;
        const bool signed_to_unsigned =
            vector_saturating_shift_class == 0x2f006400U ||
            scalar_saturating_shift_class == 0x7f006400U;
        const auto source = state_.v[rn];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto lane_shift = (byte % 8) * 8;
            const auto value = (source[byte / 8] >> lane_shift) & mask;
            std::uint64_t shifted = 0;
            if (signed_destination) {
                if ((value & sign) == 0) {
                    if (value > ((sign - 1) >> amount)) {
                        shifted = sign - 1;
                        state_.fpsr |= 1U << 27;
                    } else {
                        shifted = value << amount;
                    }
                } else {
                    const auto magnitude = (~value + 1) & mask;
                    if (magnitude > (sign >> amount)) {
                        shifted = sign;
                        state_.fpsr |= 1U << 27;
                    } else {
                        shifted = (0 - (magnitude << amount)) & mask;
                    }
                }
            } else if (signed_to_unsigned && (value & sign) != 0) {
                state_.fpsr |= 1U << 27;
            } else if (value > (mask >> amount)) {
                shifted = mask;
                state_.fpsr |= 1U << 27;
            } else {
                shifted = value << amount;
            }
            result[byte / 8] |= shifted << lane_shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_insert_shift_class = instruction & 0xbf80fc00U;
    if (vector_insert_shift_class == 0x0f005400U || // SHL
        vector_insert_shift_class == 0x2f005400U || // SLI
        vector_insert_shift_class == 0x2f004400U || // SRI
        vector_insert_shift_class == 0x1f005400U || // scalar SHL
        vector_insert_shift_class == 0x3f005400U || // scalar SLI
        vector_insert_shift_class == 0x3f004400U) { // scalar SRI
        const auto immh = (instruction >> 19) & 15U;
        const auto immb = (instruction >> 16) & 7U;
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool scalar = (instruction & 0x10000000U) != 0;
        if (immh == 0 || (scalar ? immh < 8 : immh >= 8 && !quad)) {
            throw std::runtime_error("invalid vector insert-shift arrangement");
        }
        const auto element_bits = 1U << (std::bit_width(immh) + 2U);
        const auto encoded_shift = (immh << 3) | immb;
        const bool right = vector_insert_shift_class == 0x2f004400U ||
                           vector_insert_shift_class == 0x3f004400U;
        const auto amount = right ? element_bits * 2 - encoded_shift
                                  : encoded_shift - element_bits;
        const auto element_bytes = element_bits / 8;
        const auto vector_bytes = scalar ? 8U : quad ? 16U : 8U;
        const auto mask = element_bits == 64 ? UINT64_MAX
                                             : (1ULL << element_bits) - 1;
        const auto source = state_.v[rn];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto lane_shift = (byte % 8) * 8;
            const auto input = (source[word] >> lane_shift) & mask;
            const auto old = (prior[word] >> lane_shift) & mask;
            std::uint64_t value = 0;
            if (right) {
                const auto inserted = amount == element_bits ? 0 : mask >> amount;
                value = (old & ~inserted) |
                        (amount == element_bits ? 0 : input >> amount);
            } else {
                const auto inserted = (mask << amount) & mask;
                value = ((input << amount) & mask) |
                        ((vector_insert_shift_class == 0x2f005400U ||
                          vector_insert_shift_class == 0x3f005400U)
                             ? old & ~inserted : 0);
            }
            result[word] |= (value & mask) << lane_shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto pairwise_widen_class = instruction & 0x9f3ffc00U;
    if (pairwise_widen_class == 0x0e202800U ||
        pairwise_widen_class == 0x0e206800U) {
        const auto size = (instruction >> 22) & 3U;
        if (size == 3) {
            throw std::runtime_error("invalid pairwise widening arrangement");
        }
        const auto source_bytes = 1U << size;
        const auto destination_bytes = source_bytes * 2;
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto source_mask = (1ULL << (source_bytes * 8)) - 1;
        const auto destination_mask = destination_bytes == 8 ? UINT64_MAX
                                                             : (1ULL << (destination_bytes * 8)) - 1;
        const bool unsigned_source = (instruction & 0x20000000U) != 0;
        const bool accumulate = pairwise_widen_class == 0x0e206800U;
        const auto source = state_.v[rn];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < vector_bytes / destination_bytes; ++lane) {
            const auto first_byte = lane * destination_bytes;
            const auto second_byte = first_byte + source_bytes;
            const auto first_raw = (source[first_byte / 8] >>
                                    ((first_byte % 8) * 8)) & source_mask;
            const auto second_raw = (source[second_byte / 8] >>
                                     ((second_byte % 8) * 8)) & source_mask;
            const auto sign = 1ULL << (source_bytes * 8 - 1);
            const auto first = unsigned_source ? first_raw
                                               : (first_raw ^ sign) - sign;
            const auto second = unsigned_source ? second_raw
                                                : (second_raw ^ sign) - sign;
            const auto destination_shift = (first_byte % 8) * 8;
            const auto previous = accumulate
                ? (prior[first_byte / 8] >> destination_shift) & destination_mask
                : 0;
            const auto value = (previous + first + second) & destination_mask;
            result[first_byte / 8] |= value << destination_shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_reduce_or_shift_class = instruction & 0xbf20fc00U;
    if (vector_reduce_or_shift_class == 0x0e20b800U) {
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes == 8 || (element_bytes == 4 && vector_bytes == 8)) {
            throw std::runtime_error("invalid vector ADDV arrangement");
        }
        const auto mask = (1ULL << (element_bytes * 8)) - 1;
        std::uint64_t sum = 0;
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            sum += (state_.v[rn][byte / 8] >> ((byte % 8) * 8)) & mask;
        }
        state_.v[rd] = {sum & mask, 0};
        return false;
    }
    const auto sum_long_class = instruction & 0xbf3ffc00U;
    if (sum_long_class == 0x0e303800U || // SADDLV
        sum_long_class == 0x2e303800U) { // UADDLV
        const auto size = (instruction >> 22) & 3U;
        const bool quad = (instruction & 0x40000000U) != 0;
        if (size == 3 || (size == 2 && !quad)) {
            throw std::runtime_error("invalid vector sum-long arrangement");
        }
        const auto source_bytes = 1U << size;
        const auto destination_bytes = source_bytes * 2;
        const auto source_mask = (1ULL << (source_bytes * 8)) - 1;
        const auto destination_mask = destination_bytes == 8 ? UINT64_MAX
            : (1ULL << (destination_bytes * 8)) - 1;
        const auto sign = 1ULL << (source_bytes * 8 - 1);
        const bool unsigned_source = (instruction & 0x20000000U) != 0;
        const auto source = state_.v[rn];
        std::uint64_t sum = 0;
        for (unsigned byte = 0; byte < (quad ? 16U : 8U);
             byte += source_bytes) {
            const auto raw = (source[byte / 8] >> ((byte % 8) * 8)) &
                             source_mask;
            sum += unsigned_source ? raw : (raw ^ sign) - sign;
        }
        state_.v[rd] = {sum & destination_mask, 0};
        return false;
    }
    const auto integer_reduce_class = instruction & 0xbf3ffc00U;
    if (integer_reduce_class == 0x0e30a800U || // SMAXV
        integer_reduce_class == 0x0e31a800U || // SMINV
        integer_reduce_class == 0x2e30a800U || // UMAXV
        integer_reduce_class == 0x2e31a800U) { // UMINV
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes == 8 || (element_bytes == 4 && vector_bytes == 8)) {
            throw std::runtime_error("invalid vector integer reduction arrangement");
        }
        const auto element_bits = element_bytes * 8;
        const auto mask = (1ULL << element_bits) - 1;
        const auto sign = 1ULL << (element_bits - 1);
        const bool unsigned_source = (instruction & 0x20000000U) != 0;
        const bool minimum = (instruction & 0x00010000U) != 0;
        const auto source = state_.v[rn];
        auto selected = source[0] & mask;
        for (unsigned byte = element_bytes; byte < vector_bytes;
             byte += element_bytes) {
            const auto candidate = (source[byte / 8] >> ((byte % 8) * 8)) & mask;
            const auto ordered_selected = unsigned_source ? selected
                                                          : selected ^ sign;
            const auto ordered_candidate = unsigned_source ? candidate
                                                           : candidate ^ sign;
            if (minimum ? ordered_candidate < ordered_selected
                        : ordered_candidate > ordered_selected) {
                selected = candidate;
            }
        }
        state_.v[rd] = {selected, 0};
        return false;
    }
    const auto saturating_register_shift_class = instruction & 0x9f20ec00U;
    if (saturating_register_shift_class == 0x0e204c00U ||
        (instruction & 0xdf20ec00U) == 0x5e204c00U) {
        // SQSHL/UQSHL and SQRSHL/UQRSHL use only the signed low byte of
        // each shift-count element, including the scalar B/H/S/D forms.
        const bool scalar = (instruction & 0x10000000U) != 0;
        const bool quad = (instruction & 0x40000000U) != 0;
        const auto element_bits = 8U << ((instruction >> 22) & 3U);
        if (!scalar && element_bits == 64 && !quad) {
            throw std::runtime_error("invalid saturating register-shift arrangement");
        }
        const auto element_bytes = element_bits / 8;
        const auto vector_bytes = scalar ? element_bytes : quad ? 16U : 8U;
        const auto mask = element_bits == 64 ? UINT64_MAX
                                             : (1ULL << element_bits) - 1;
        const auto sign = 1ULL << (element_bits - 1);
        const bool signed_elements = (instruction & 0x20000000U) == 0;
        const bool rounding = (instruction & 0x1000U) != 0;
        const auto source = state_.v[rn];
        const auto counts = state_.v[(instruction >> 16) & 31U];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto lane_shift = (byte % 8) * 8;
            const auto value = (source[word] >> lane_shift) & mask;
            const auto count = std::bit_cast<std::int8_t>(
                static_cast<std::uint8_t>(counts[word] >> lane_shift));
            std::uint64_t shifted = value;
            bool saturated = false;
            if (count > 0 && value != 0) {
                const auto amount = static_cast<unsigned>(count);
                const bool negative = signed_elements && (value & sign) != 0;
                const auto magnitude = negative ? (~value + 1) & mask : value;
                const auto maximum = signed_elements
                    ? negative ? sign : sign - 1 : mask;
                if (amount >= element_bits || magnitude > (maximum >> amount)) {
                    shifted = negative ? sign : maximum;
                    saturated = true;
                } else {
                    const auto scaled = magnitude << amount;
                    shifted = negative ? (0 - scaled) & mask : scaled;
                }
            } else if (count < 0) {
                const auto amount = static_cast<unsigned>(-static_cast<int>(count));
                shifted = amount >= element_bits ? 0 : value >> amount;
                if (signed_elements && (value & sign) != 0) {
                    shifted |= amount >= element_bits ? mask
                        : (mask << (element_bits - amount)) & mask;
                }
                if (rounding) {
                    if (amount > element_bits) {
                        shifted = 0;
                    } else if ((value & (1ULL << (amount - 1))) != 0) {
                        shifted = (shifted + 1) & mask;
                    }
                }
            }
            if (saturated) state_.fpsr |= 1U << 27;
            result[word] |= shifted << lane_shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if (vector_reduce_or_shift_class == 0x0e204400U || // SSHL
        vector_reduce_or_shift_class == 0x2e204400U || // USHL
        vector_reduce_or_shift_class == 0x0e205400U || // SRSHL
        vector_reduce_or_shift_class == 0x2e205400U) { // URSHL
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes == 8 && vector_bytes == 8) {
            throw std::runtime_error("invalid vector register-shift arrangement");
        }
        const auto bits = element_bytes * 8;
        const auto mask = element_bytes == 8 ? UINT64_MAX :
                          (1ULL << bits) - 1;
        const auto rm = (instruction >> 16) & 31U;
        const bool signed_source = (instruction & 0x20000000U) == 0;
        const bool rounding = (instruction & 0x00001000U) != 0;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto shift = (byte % 8) * 8;
            const auto value = (left[byte / 8] >> shift) & mask;
            const auto amount_byte = static_cast<unsigned>(
                (right[byte / 8] >> shift) & 0xffU);
            const auto amount = amount_byte < 128
                ? static_cast<int>(amount_byte)
                : static_cast<int>(amount_byte) - 256;
            std::uint64_t shifted;
            if (amount >= 0) {
                shifted = amount >= static_cast<int>(bits)
                    ? 0 : (value << amount) & mask;
            } else {
                const auto right_amount = static_cast<unsigned>(-amount);
                shifted = right_amount >= bits ? 0 : value >> right_amount;
                if (signed_source && (value & (1ULL << (bits - 1))) != 0 &&
                    right_amount <= bits) {
                    shifted |= right_amount == bits ? mask
                        : (mask << (bits - right_amount)) & mask;
                } else if (signed_source && !rounding && right_amount > bits &&
                           (value & (1ULL << (bits - 1))) != 0) {
                    shifted = mask;
                }
                if (rounding && right_amount <= bits) {
                    const auto remainder_mask = right_amount == bits
                        ? mask : (1ULL << right_amount) - 1;
                    if ((value & remainder_mask) >=
                        (1ULL << (right_amount - 1))) {
                        shifted = (shifted + 1) & mask;
                    }
                }
            }
            result[byte / 8] |= shifted << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xfffffc00U) == 0x5ef1b800U) {
        const auto source = state_.v[rn];
        state_.v[rd] = {source[0] + source[1], 0};
        return false;
    }
    if ((instruction & 0xbfa0fc00U) == 0x2e20d400U) {
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        if (double_lanes && !quad) {
            throw std::runtime_error("invalid vector FADDP arrangement");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto lane_bytes = double_lanes ? 8U : 4U;
        const auto lanes = (quad ? 16U : 8U) / lane_bytes;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto lane = [lane_bytes](const auto& vector, unsigned index) {
            const auto byte = index * lane_bytes;
            const auto raw = vector[byte / 8] >> ((byte % 8) * 8);
            return lane_bytes == 8 ? raw : raw & UINT32_MAX;
        };
        std::array<std::uint64_t, 2> result{};
        for (unsigned index = 0; index < lanes; ++index) {
            const auto& source = index < lanes / 2 ? left : right;
            const auto pair = 2 * (index % (lanes / 2));
            const auto first = lane(source, pair);
            const auto second = lane(source, pair + 1);
            const auto sum = floating_add_sub_bits(first, second,
                                                   double_lanes, false,
                                                   state_);
            const auto byte = index * lane_bytes;
            result[byte / 8] |= sum << ((byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xffbffc00U) == 0x7e30d800U) {
        const bool double_lanes = (instruction & 0x00400000U) != 0;
        const auto source = state_.v[rn];
        const auto first = source[0];
        const auto second = double_lanes ? source[1] : source[0] >> 32;
        state_.v[rd] = {floating_add_sub_bits(first, second, double_lanes,
                                              false, state_), 0};
        return false;
    }
    const auto floating_reduce_class = instruction & 0xfffffc00U;
    if (floating_reduce_class == 0x6eb0f800U ||
        floating_reduce_class == 0x6e30f800U ||
        floating_reduce_class == 0x6eb0c800U ||
        floating_reduce_class == 0x6e30c800U) {
        const bool minimum = (instruction & 0x00800000U) != 0;
        const bool numeric_if_quiet_nan = (instruction & 0x00003000U) == 0;
        const auto source = state_.v[rn];
        const auto get_lane = [&source](unsigned index) {
            return (source[index / 2] >> ((index % 2) * 32)) & UINT32_MAX;
        };
        const auto low = floating_minmax_bits(get_lane(0), get_lane(1),
                                              false, minimum,
                                              numeric_if_quiet_nan, state_);
        const auto high = floating_minmax_bits(get_lane(2), get_lane(3),
                                               false, minimum,
                                               numeric_if_quiet_nan, state_);
        state_.v[rd] = {floating_minmax_bits(low, high, false, minimum,
                                             numeric_if_quiet_nan, state_), 0};
        return false;
    }
    const auto pairwise_class = instruction & 0xbf20fc00U;
    if (pairwise_class == 0x0e20bc00U || // ADDP
        pairwise_class == 0x0e20a400U || // SMAXP
        pairwise_class == 0x0e20ac00U || // SMINP
        pairwise_class == 0x2e20a400U || // UMAXP
        pairwise_class == 0x2e20ac00U) { // UMINP
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto lanes = vector_bytes / element_bytes;
        const bool minmax = pairwise_class != 0x0e20bc00U;
        if (lanes < 2 || (minmax && element_bytes == 8)) {
            throw std::runtime_error("invalid vector pairwise arrangement");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        const auto sign = 1ULL << (element_bytes * 8 - 1);
        const bool signed_operands = (instruction & 0x20000000U) == 0;
        const bool minimum = (instruction & 0x00000800U) != 0;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto lane = [element_bytes, mask](const auto& vector, unsigned index) {
            const auto byte = index * element_bytes;
            return (vector[byte / 8] >> ((byte % 8) * 8)) & mask;
        };
        std::array<std::uint64_t, 2> result{};
        for (unsigned index = 0; index < lanes; ++index) {
            const auto& source = index < lanes / 2 ? left : right;
            const auto pair = 2 * (index % (lanes / 2));
            const auto first = lane(source, pair);
            const auto second = lane(source, pair + 1);
            const auto ordered_first = signed_operands ? first ^ sign : first;
            const auto ordered_second = signed_operands ? second ^ sign : second;
            const auto value = !minmax ? (first + second) & mask
                : minimum ? (ordered_first <= ordered_second ? first : second)
                          : (ordered_first >= ordered_second ? first : second);
            const auto byte = index * element_bytes;
            result[byte / 8] |= value << ((byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto saturating_vector_class = instruction & 0xbf20fc00U;
    const auto saturating_scalar_class = instruction & 0xff20fc00U;
    const bool saturating_vector =
        saturating_vector_class == 0x0e200c00U || // SQADD
        saturating_vector_class == 0x0e202c00U || // SQSUB
        saturating_vector_class == 0x2e200c00U || // UQADD
        saturating_vector_class == 0x2e202c00U;   // UQSUB
    const bool saturating_scalar =
        saturating_scalar_class == 0x5e200c00U || // SQADD
        saturating_scalar_class == 0x5e202c00U || // SQSUB
        saturating_scalar_class == 0x7e200c00U || // UQADD
        saturating_scalar_class == 0x7e202c00U;   // UQSUB
    if (saturating_vector || saturating_scalar) {
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = saturating_scalar ? element_bytes
            : (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes > vector_bytes ||
            (saturating_vector && element_bytes == 8 && vector_bytes == 8)) {
            throw std::runtime_error("invalid vector saturating arithmetic arrangement");
        }
        const auto bits = element_bytes * 8;
        const auto mask = element_bytes == 8 ? UINT64_MAX : (1ULL << bits) - 1;
        const auto sign = 1ULL << (bits - 1);
        const bool unsigned_operands = (instruction & 0x20000000U) != 0;
        const bool subtract = (instruction & 0x00002000U) != 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto shift = (byte % 8) * 8;
            const auto lhs = (left[byte / 8] >> shift) & mask;
            const auto rhs = (right[byte / 8] >> shift) & mask;
            auto value = (subtract ? lhs - rhs : lhs + rhs) & mask;
            bool saturated = false;
            if (unsigned_operands) {
                saturated = subtract ? lhs < rhs : value < lhs;
                if (saturated) value = subtract ? 0 : mask;
            } else {
                saturated = subtract
                    ? ((lhs ^ rhs) & (lhs ^ value) & sign) != 0
                    : ((~(lhs ^ rhs)) & (lhs ^ value) & sign) != 0;
                if (saturated) value = (lhs & sign) != 0 ? sign : sign - 1;
            }
            if (saturated) state_.fpsr |= 1U << 27;
            result[byte / 8] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto vector_to_gpr_class = instruction & 0xffe0fc00U;
    if (vector_to_gpr_class == 0x0e003c00U ||
        vector_to_gpr_class == 0x4e003c00U ||
        vector_to_gpr_class == 0x0e002c00U ||
        vector_to_gpr_class == 0x4e002c00U) {
        const auto imm5 = (instruction >> 16) & 31U;
        if (imm5 == 0) throw std::runtime_error("invalid vector-to-GPR lane");
        const auto size_shift = std::countr_zero(imm5);
        const auto element_bytes = 1U << size_shift;
        const auto lane_index = imm5 >> (size_shift + 1);
        const auto source_byte = lane_index * element_bytes;
        const bool wide = (instruction & 0x40000000U) != 0;
        const bool signed_element = (instruction & 0x00001000U) == 0;
        if (source_byte + element_bytes > 16 ||
            (signed_element ? (wide ? element_bytes > 4 : element_bytes > 2)
                            : (wide ? element_bytes != 8 : element_bytes > 4))) {
            throw std::runtime_error("invalid vector-to-GPR arrangement");
        }
        const auto shift = (source_byte % 8) * 8;
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        auto value = (state_.v[rn][source_byte / 8] >> shift) & mask;
        if (signed_element && (value & (1ULL << (element_bytes * 8 - 1))) != 0) {
            value |= ~mask;
        }
        set_reg(rd, value, wide);
        return false;
    }
    const bool compare_zero = (instruction & 0xbf3ffc00U) == 0x0e209800U;
    const auto equality_class = instruction & 0xbf20fc00U;
    const bool compare_register = equality_class == 0x2e208c00U;
    const bool bit_test_register = equality_class == 0x0e208c00U;
    if (compare_zero || compare_register || bit_test_register) {
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes > vector_bytes ||
            (element_bytes == 8 && vector_bytes == 8)) {
            throw std::runtime_error("invalid vector comparison arrangement");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto shift = (byte % 8) * 8;
            const auto lhs = (state_.v[rn][word] >> shift) & mask;
            const auto rhs = compare_zero ? 0 :
                             (state_.v[rm][word] >> shift) & mask;
            if (bit_test_register ? (lhs & rhs) != 0 : lhs == rhs) {
                result[word] |= mask << shift;
            }
        }
        state_.v[rd] = result;
        return false;
    }
    const auto scalar_integer_compare_class = instruction & 0xff20fc00U;
    if (scalar_integer_compare_class == 0x5e208c00U || // CMTST
        scalar_integer_compare_class == 0x7e208c00U || // CMEQ
        scalar_integer_compare_class == 0x5e203400U || // CMGT
        scalar_integer_compare_class == 0x5e203c00U || // CMGE
        scalar_integer_compare_class == 0x7e203400U || // CMHI
        scalar_integer_compare_class == 0x7e203c00U) { // CMHS
        const auto rm = (instruction >> 16) & 31U;
        const auto lhs = state_.v[rn][0];
        const auto rhs = state_.v[rm][0];
        const auto signed_lhs = std::bit_cast<std::int64_t>(lhs);
        const auto signed_rhs = std::bit_cast<std::int64_t>(rhs);
        const bool matches = scalar_integer_compare_class == 0x5e208c00U
            ? (lhs & rhs) != 0 :
            scalar_integer_compare_class == 0x7e208c00U
            ? lhs == rhs :
            scalar_integer_compare_class == 0x5e203400U
            ? signed_lhs > signed_rhs :
            scalar_integer_compare_class == 0x5e203c00U
            ? signed_lhs >= signed_rhs :
            scalar_integer_compare_class == 0x7e203400U
            ? lhs > rhs : lhs >= rhs;
        state_.v[rd] = {matches ? UINT64_MAX : 0, 0};
        return false;
    }
    const auto integer_zero_compare_class = instruction & 0xbf3ffc00U;
    const bool integer_zero_compare =
        integer_zero_compare_class == 0x0e20a800U || // CMLT
        integer_zero_compare_class == 0x0e208800U || // CMGT
        integer_zero_compare_class == 0x2e209800U || // CMLE
        integer_zero_compare_class == 0x2e208800U;   // CMGE
    const auto integer_register_class = instruction & 0xbf20fc00U;
    const bool integer_register_compare =
        integer_register_class == 0x0e203400U || // CMGT
        integer_register_class == 0x0e203c00U || // CMGE
        integer_register_class == 0x2e203400U || // CMHI
        integer_register_class == 0x2e203c00U;   // CMHS
    const bool integer_register_minmax =
        integer_register_class == 0x0e206400U || // SMAX
        integer_register_class == 0x0e206c00U || // SMIN
        integer_register_class == 0x2e206400U || // UMAX
        integer_register_class == 0x2e206c00U;   // UMIN
    if (integer_zero_compare || integer_register_compare ||
        integer_register_minmax) {
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if ((element_bytes == 8 && vector_bytes == 8) ||
            (integer_register_minmax && element_bytes == 8)) {
            throw std::runtime_error("invalid vector integer comparison arrangement");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto mask = element_bytes == 8 ? UINT64_MAX
                                             : (1ULL << (element_bytes * 8)) - 1;
        const auto sign_bit = 1ULL << (element_bytes * 8 - 1);
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto shift = (byte % 8) * 8;
            const auto lhs = (left[word] >> shift) & mask;
            const auto rhs = (right[word] >> shift) & mask;
            const auto signed_lhs = lhs ^ sign_bit;
            const auto signed_rhs = rhs ^ sign_bit;
            std::uint64_t value = 0;
            if (integer_zero_compare) {
                bool match = false;
                switch (integer_zero_compare_class) {
                case 0x0e20a800U: match = (lhs & sign_bit) != 0; break;
                case 0x0e208800U: match = (lhs & sign_bit) == 0 && lhs != 0; break;
                case 0x2e209800U: match = (lhs & sign_bit) != 0 || lhs == 0; break;
                case 0x2e208800U: match = (lhs & sign_bit) == 0; break;
                }
                value = match ? mask : 0;
            } else if (integer_register_compare) {
                bool match = false;
                switch (integer_register_class) {
                case 0x0e203400U: match = signed_lhs > signed_rhs; break;
                case 0x0e203c00U: match = signed_lhs >= signed_rhs; break;
                case 0x2e203400U: match = lhs > rhs; break;
                case 0x2e203c00U: match = lhs >= rhs; break;
                }
                value = match ? mask : 0;
            } else {
                bool use_left = false;
                switch (integer_register_class) {
                case 0x0e206400U: use_left = signed_lhs >= signed_rhs; break;
                case 0x0e206c00U: use_left = signed_lhs <= signed_rhs; break;
                case 0x2e206400U: use_left = lhs >= rhs; break;
                case 0x2e206c00U: use_left = lhs <= rhs; break;
                }
                value = use_left ? lhs : rhs;
            }
            result[word] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xbf20fc00U) == 0x0e20e000U) {
        const auto size = (instruction >> 22) & 3U;
        if (size != 0 && size != 3) {
            throw std::runtime_error("invalid vector PMULL arrangement");
        }
        const bool upper = (instruction & 0x40000000U) != 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        std::array<std::uint64_t, 2> result{};
        if (size == 3) {
            const auto lhs = left[upper ? 1 : 0];
            const auto rhs = right[upper ? 1 : 0];
            for (unsigned bit = 0; bit < 64; ++bit) {
                if (((rhs >> bit) & 1U) == 0) continue;
                result[0] ^= lhs << bit;
                if (bit != 0) result[1] ^= lhs >> (64 - bit);
            }
        } else {
            for (unsigned lane = 0; lane < 8; ++lane) {
                const auto source_byte = (upper ? 8U : 0U) + lane;
                const auto lhs = static_cast<std::uint8_t>(
                    left[source_byte / 8] >> ((source_byte % 8) * 8));
                const auto rhs = static_cast<std::uint8_t>(
                    right[source_byte / 8] >> ((source_byte % 8) * 8));
                unsigned product = 0;
                for (unsigned bit = 0; bit < 8; ++bit) {
                    if (((rhs >> bit) & 1U) != 0) product ^= lhs << bit;
                }
                const auto destination_byte = lane * 2;
                result[destination_byte / 8] |=
                    static_cast<std::uint64_t>(product) <<
                    ((destination_byte % 8) * 8);
            }
        }
        state_.v[rd] = result;
        return false;
    }
    const auto widening_arithmetic_class = instruction & 0xbf20fc00U;
    if (widening_arithmetic_class == 0x0e200000U || // SADDL/SADDL2
        widening_arithmetic_class == 0x2e200000U || // UADDL/UADDL2
        widening_arithmetic_class == 0x0e201000U || // SADDW/SADDW2
        widening_arithmetic_class == 0x2e201000U || // UADDW/UADDW2
        widening_arithmetic_class == 0x0e202000U || // SSUBL/SSUBL2
        widening_arithmetic_class == 0x2e202000U || // USUBL/USUBL2
        widening_arithmetic_class == 0x0e203000U || // SSUBW/SSUBW2
        widening_arithmetic_class == 0x2e203000U) { // USUBW/USUBW2
        const auto size = (instruction >> 22) & 3U;
        if (size == 3) {
            throw std::runtime_error("invalid vector widening arithmetic arrangement");
        }
        const auto source_bytes = 1U << size;
        const auto destination_bytes = source_bytes * 2;
        const auto lanes = 16U / destination_bytes;
        const bool upper = (instruction & 0x40000000U) != 0;
        const bool unsigned_operands = (instruction & 0x20000000U) != 0;
        const bool wide_first = (instruction & 0x00001000U) != 0;
        const bool subtract = (instruction & 0x00002000U) != 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto source_mask = (1ULL << (source_bytes * 8)) - 1;
        const auto destination_mask = destination_bytes == 8
            ? UINT64_MAX : (1ULL << (destination_bytes * 8)) - 1;
        const auto sign = 1ULL << (source_bytes * 8 - 1);
        const auto extend = [=](std::uint64_t value) {
            return !unsigned_operands && (value & sign) != 0
                ? value | ~source_mask : value;
        };
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto first_byte = wide_first ? lane * destination_bytes
                : (upper ? 8U : 0U) + lane * source_bytes;
            const auto second_byte = (upper ? 8U : 0U) + lane * source_bytes;
            const auto lhs = (left[first_byte / 8] >>
                              ((first_byte % 8) * 8)) &
                             (wide_first ? destination_mask : source_mask);
            const auto rhs = (right[second_byte / 8] >>
                              ((second_byte % 8) * 8)) & source_mask;
            const auto first = wide_first ? lhs : extend(lhs);
            const auto second = extend(rhs);
            const auto value = (subtract ? first - second : first + second) &
                               destination_mask;
            const auto destination_byte = lane * destination_bytes;
            result[destination_byte / 8] |= value <<
                ((destination_byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto narrowing_addsub_class = instruction & 0xbf20fc00U;
    if (narrowing_addsub_class == 0x0e204000U || // ADDHN/ADDHN2
        narrowing_addsub_class == 0x2e204000U || // RADDHN/RADDHN2
        narrowing_addsub_class == 0x0e206000U || // SUBHN/SUBHN2
        narrowing_addsub_class == 0x2e206000U) { // RSUBHN/RSUBHN2
        const auto size = (instruction >> 22) & 3U;
        if (size == 3) {
            throw std::runtime_error("invalid vector narrowing add/subtract arrangement");
        }
        const auto destination_bytes = 1U << size;
        const auto source_bytes = destination_bytes * 2;
        const auto lanes = 8U / destination_bytes;
        const bool upper = (instruction & 0x40000000U) != 0;
        const bool rounded = (instruction & 0x20000000U) != 0;
        const bool subtract = (instruction & 0x00002000U) != 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto source_mask = source_bytes == 8
            ? UINT64_MAX : (1ULL << (source_bytes * 8)) - 1;
        const auto round = rounded ? 1ULL << (destination_bytes * 8 - 1) : 0;
        std::array<std::uint64_t, 2> result{upper ? state_.v[rd][0] : 0, 0};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto source_byte = lane * source_bytes;
            const auto source_shift = (source_byte % 8) * 8;
            const auto lhs = (left[source_byte / 8] >> source_shift) & source_mask;
            const auto rhs = (right[source_byte / 8] >> source_shift) & source_mask;
            const auto arithmetic = subtract ? lhs - rhs : lhs + rhs;
            const auto narrowed = ((arithmetic + round) & source_mask) >>
                                  (destination_bytes * 8);
            const auto destination_byte = (upper ? 8U : 0U) +
                                          lane * destination_bytes;
            result[destination_byte / 8] |= narrowed <<
                ((destination_byte % 8) * 8);
        }
        state_.v[rd] = result;
        return false;
    }
    const auto integer_difference_class = instruction & 0xbf20fc00U;
    if (integer_difference_class == 0x0e207400U || // SABD
        integer_difference_class == 0x2e207400U || // UABD
        integer_difference_class == 0x0e207c00U || // SABA
        integer_difference_class == 0x2e207c00U) { // UABA
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        if (element_bytes == 8) {
            throw std::runtime_error("invalid vector integer absolute-difference arrangement");
        }
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto bits = element_bytes * 8;
        const auto mask = (1ULL << bits) - 1;
        const auto sign = 1ULL << (bits - 1);
        const bool signed_operands = (instruction & 0x20000000U) == 0;
        const bool accumulate = (instruction & 0x00000800U) != 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto shift = (byte % 8) * 8;
            const auto lhs = (left[byte / 8] >> shift) & mask;
            const auto rhs = (right[byte / 8] >> shift) & mask;
            std::uint64_t difference;
            if (signed_operands) {
                const auto signed_lhs = static_cast<std::int64_t>(lhs) -
                    ((lhs & sign) != 0 ? 1LL << bits : 0);
                const auto signed_rhs = static_cast<std::int64_t>(rhs) -
                    ((rhs & sign) != 0 ? 1LL << bits : 0);
                difference = signed_lhs >= signed_rhs
                    ? static_cast<std::uint64_t>(signed_lhs - signed_rhs)
                    : static_cast<std::uint64_t>(signed_rhs - signed_lhs);
            } else {
                difference = lhs >= rhs ? lhs - rhs : rhs - lhs;
            }
            const auto old = (prior[byte / 8] >> shift) & mask;
            const auto value = (difference + (accumulate ? old : 0)) & mask;
            result[byte / 8] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto widening_difference_class = instruction & 0xbf20fc00U;
    if (widening_difference_class == 0x0e207000U || // SABDL/SABDL2
        widening_difference_class == 0x2e207000U || // UABDL/UABDL2
        widening_difference_class == 0x0e205000U || // SABAL/SABAL2
        widening_difference_class == 0x2e205000U) { // UABAL/UABAL2
        const auto size = (instruction >> 22) & 3U;
        if (size == 3) {
            throw std::runtime_error("invalid vector widening absolute-difference arrangement");
        }
        const auto source_bytes = 1U << size;
        const auto destination_bytes = source_bytes * 2;
        const auto source_mask = (1ULL << (source_bytes * 8)) - 1;
        const auto destination_mask = destination_bytes == 8 ? UINT64_MAX
            : (1ULL << (destination_bytes * 8)) - 1;
        const auto sign = 1ULL << (source_bytes * 8 - 1);
        const bool upper = (instruction & 0x40000000U) != 0;
        const bool signed_operands = (instruction & 0x20000000U) == 0;
        const bool accumulate = (instruction & 0x00002000U) == 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto prior = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < 8U / source_bytes; ++lane) {
            const auto source_byte = (upper ? 8U : 0U) + lane * source_bytes;
            const auto source_shift = (source_byte % 8) * 8;
            const auto lhs_raw = (left[source_byte / 8] >> source_shift) &
                                 source_mask;
            const auto rhs_raw = (right[source_byte / 8] >> source_shift) &
                                 source_mask;
            std::uint64_t difference;
            if (signed_operands) {
                const auto lhs = static_cast<std::int64_t>(lhs_raw) -
                    ((lhs_raw & sign) != 0 ? 1LL << (source_bytes * 8) : 0);
                const auto rhs = static_cast<std::int64_t>(rhs_raw) -
                    ((rhs_raw & sign) != 0 ? 1LL << (source_bytes * 8) : 0);
                difference = lhs >= rhs
                    ? static_cast<std::uint64_t>(lhs - rhs)
                    : static_cast<std::uint64_t>(rhs - lhs);
            } else {
                difference = lhs_raw >= rhs_raw ? lhs_raw - rhs_raw
                                                 : rhs_raw - lhs_raw;
            }
            const auto destination_byte = lane * destination_bytes;
            const auto destination_shift = (destination_byte % 8) * 8;
            const auto old = accumulate
                ? (prior[destination_byte / 8] >> destination_shift) &
                      destination_mask
                : 0;
            result[destination_byte / 8] |=
                ((old + difference) & destination_mask) << destination_shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto integer_multiply_class = instruction & 0xbf20fc00U;
    if (integer_multiply_class == 0x0e209c00U || // MUL
        integer_multiply_class == 0x0e209400U || // MLA
        integer_multiply_class == 0x2e209400U) { // MLS
        const auto element_bytes = 1U << ((instruction >> 22) & 3U);
        if (element_bytes == 8) {
            throw std::runtime_error("invalid vector integer multiply arrangement");
        }
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto rm = (instruction >> 16) & 31U;
        const auto mask = (1ULL << (element_bytes * 8)) - 1;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto accumulator = state_.v[rd];
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto shift = (byte % 8) * 8;
            const auto lhs = (left[word] >> shift) & mask;
            const auto rhs = (right[word] >> shift) & mask;
            const auto product = (lhs * rhs) & mask;
            const auto old = (accumulator[word] >> shift) & mask;
            const auto value = integer_multiply_class == 0x0e209c00U ? product :
                               integer_multiply_class == 0x0e209400U
                                   ? (old + product) & mask
                                   : (old - product) & mask;
            result[word] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto integer_multiply_indexed_class = instruction & 0xbf00f400U;
    if (integer_multiply_indexed_class == 0x0f008000U || // MUL by element
        integer_multiply_indexed_class == 0x2f000000U || // MLA by element
        integer_multiply_indexed_class == 0x2f004000U) { // MLS by element
        const auto size = (instruction >> 22) & 3U;
        if (size != 1 && size != 2) {
            throw std::runtime_error("invalid vector indexed integer multiply arrangement");
        }
        const auto element_bytes = 1U << size;
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto mask = (1ULL << (element_bytes * 8)) - 1;
        const auto rm = size == 1 ? (instruction >> 16) & 15U
                                  : (instruction >> 16) & 31U;
        const auto index = (((instruction >> 11) & 1U) <<
                            (size == 1 ? 2 : 1)) |
                           (((instruction >> 21) & 1U) <<
                            (size == 1 ? 1 : 0)) |
                           (size == 1 ? ((instruction >> 20) & 1U) : 0U);
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto accumulator = state_.v[rd];
        const auto indexed_byte = index * element_bytes;
        const auto multiplier =
            (right[indexed_byte / 8] >> ((indexed_byte % 8) * 8)) & mask;
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto word = byte / 8;
            const auto shift = (byte % 8) * 8;
            const auto lhs = (left[word] >> shift) & mask;
            const auto product = (lhs * multiplier) & mask;
            const auto old = (accumulator[word] >> shift) & mask;
            const auto value = integer_multiply_indexed_class == 0x0f008000U
                ? product
                : integer_multiply_indexed_class == 0x2f000000U
                    ? (old + product) & mask : (old - product) & mask;
            result[word] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto dot_vector_class = instruction & 0xbf20fc00U;
    const auto dot_indexed_class = instruction & 0xbf00f400U;
    const bool mixed_dot_vector = dot_vector_class == 0x0e009c00U; // USDOT
    // Bit 22 distinguishes integer dot products from BFDOT/BFMLALB.
    const bool mixed_dot_indexed = dot_indexed_class == 0x0f00f000U &&
                                   (instruction & 0x00400000U) == 0;
    const bool dot_vector = dot_vector_class == 0x0e009400U || // SDOT
                            dot_vector_class == 0x2e009400U || // UDOT
                            mixed_dot_vector;
    const bool dot_indexed = dot_indexed_class == 0x0f00e000U || // SDOT
                             dot_indexed_class == 0x2f00e000U || // UDOT
                             mixed_dot_indexed;
    if (dot_vector || dot_indexed) {
        if (!mixed_dot_indexed && ((instruction >> 22) & 3U) != 2U) {
            throw std::runtime_error("invalid vector dot-product arrangement");
        }
        const bool unsigned_first = (instruction & 0x00800000U) != 0;
        const bool left_signed = mixed_dot_indexed ? !unsigned_first :
            !mixed_dot_vector && (instruction & 0x20000000U) == 0;
        const bool right_signed = mixed_dot_indexed ? unsigned_first :
            (instruction & 0x20000000U) == 0;
        const auto rm = (instruction >> 16) & 31U;
        const auto index = dot_indexed ?
            (((instruction >> 11) & 1U) << 1) |
            ((instruction >> 21) & 1U) : 0U;
        const auto lanes = (instruction & 0x40000000U) != 0 ? 4U : 2U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto accumulator = state_.v[rd];
        const auto byte_at = [](const auto& vector, unsigned byte) {
            return static_cast<unsigned>((vector[byte / 8] >>
                                          ((byte % 8) * 8)) & 0xffU);
        };
        std::array<std::uint64_t, 2> result{};
        for (unsigned lane = 0; lane < lanes; ++lane) {
            std::int64_t product_sum = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const auto lhs = byte_at(left, lane * 4 + i);
                const auto rhs = byte_at(right,
                    (dot_indexed ? index : lane) * 4 + i);
                const auto first = left_signed
                    ? static_cast<int>(std::bit_cast<std::int8_t>(
                          static_cast<std::uint8_t>(lhs)))
                    : static_cast<int>(lhs);
                const auto second = right_signed
                    ? static_cast<int>(std::bit_cast<std::int8_t>(
                          static_cast<std::uint8_t>(rhs)))
                    : static_cast<int>(rhs);
                product_sum += first * second;
            }
            const auto word = lane / 2;
            const auto shift = (lane % 2) * 32;
            const auto old = (accumulator[word] >> shift) & UINT32_MAX;
            const auto value = (old + static_cast<std::uint64_t>(product_sum)) &
                               UINT32_MAX;
            result[word] |= value << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto doubling_high_vector_class = instruction & 0xbf20fc00U;
    const auto doubling_high_scalar_class = instruction & 0xff20fc00U;
    const auto doubling_high_indexed_vector_class = instruction & 0xbf00f400U;
    const auto doubling_high_indexed_scalar_class = instruction & 0xff00f400U;
    const bool doubling_high_vector =
        doubling_high_vector_class == 0x0e20b400U || // SQDMULH
        doubling_high_vector_class == 0x2e20b400U;   // SQRDMULH
    const bool doubling_high_scalar =
        doubling_high_scalar_class == 0x5e20b400U || // SQDMULH
        doubling_high_scalar_class == 0x7e20b400U;   // SQRDMULH
    const bool doubling_high_indexed_vector =
        doubling_high_indexed_vector_class == 0x0f00c000U || // SQDMULH
        doubling_high_indexed_vector_class == 0x0f00d000U;   // SQRDMULH
    const bool doubling_high_indexed_scalar =
        doubling_high_indexed_scalar_class == 0x5f00c000U || // SQDMULH
        doubling_high_indexed_scalar_class == 0x5f00d000U;   // SQRDMULH
    if (doubling_high_vector || doubling_high_scalar ||
        doubling_high_indexed_vector || doubling_high_indexed_scalar) {
        const auto size = (instruction >> 22) & 3U;
        if (size != 1 && size != 2) {
            throw std::runtime_error("invalid saturating doubling multiply-high arrangement");
        }
        const auto element_bytes = 1U << size;
        const auto bits = element_bytes * 8;
        const auto mask = (1ULL << bits) - 1;
        const auto sign = 1ULL << (bits - 1);
        const bool indexed = doubling_high_indexed_vector ||
                             doubling_high_indexed_scalar;
        const bool scalar = doubling_high_scalar || doubling_high_indexed_scalar;
        const bool rounding = indexed ? (instruction & 0x00001000U) != 0
                                      : (instruction & 0x20000000U) != 0;
        const auto rm = indexed && size == 1
            ? (instruction >> 16) & 15U : (instruction >> 16) & 31U;
        const auto index = indexed
            ? (((instruction >> 11) & 1U) << (size == 1 ? 2 : 1)) |
              (((instruction >> 21) & 1U) << (size == 1 ? 1 : 0)) |
              (size == 1 ? ((instruction >> 20) & 1U) : 0U)
            : 0U;
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto vector_bytes = scalar ? element_bytes
            : (instruction & 0x40000000U) != 0 ? 16U : 8U;
        const auto to_signed = [sign, bits](std::uint64_t raw) {
            return static_cast<std::int64_t>(raw) -
                ((raw & sign) != 0 ? 1LL << bits : 0);
        };
        const auto divisor = 1LL << (bits - 1);
        const auto minimum = -divisor;
        const auto maximum = divisor - 1;
        std::array<std::uint64_t, 2> result{};
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            const auto shift = (byte % 8) * 8;
            const auto lhs = (left[byte / 8] >> shift) & mask;
            const auto rhs_byte = indexed ? index * element_bytes : byte;
            const auto rhs = (right[rhs_byte / 8] >>
                              ((rhs_byte % 8) * 8)) & mask;
            const auto product = to_signed(lhs) * to_signed(rhs);
            const auto numerator = product +
                (rounding ? 1LL << (bits - 2) : 0);
            auto quotient = numerator / divisor;
            if (numerator < 0 && numerator % divisor != 0) --quotient;
            const auto saturated = std::clamp(quotient, minimum, maximum);
            if (saturated != quotient) state_.fpsr |= 1U << 27;
            result[byte / 8] |= (static_cast<std::uint64_t>(saturated) & mask)
                                << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    const auto widen_multiply_register_class = instruction & 0xbf20fc00U;
    const auto widen_multiply_indexed_class = instruction & 0xbf00f400U;
    const bool widen_multiply_register =
        widen_multiply_register_class == 0x0e20c000U || // SMULL/SMULL2
        widen_multiply_register_class == 0x2e20c000U || // UMULL/UMULL2
        widen_multiply_register_class == 0x0e208000U || // SMLAL/SMLAL2
        widen_multiply_register_class == 0x2e208000U || // UMLAL/UMLAL2
        widen_multiply_register_class == 0x0e20a000U || // SMLSL/SMLSL2
        widen_multiply_register_class == 0x2e20a000U;   // UMLSL/UMLSL2
    const bool widen_multiply_indexed =
        widen_multiply_indexed_class == 0x0f00a000U ||
        widen_multiply_indexed_class == 0x2f00a000U ||
        widen_multiply_indexed_class == 0x0f002000U ||
        widen_multiply_indexed_class == 0x2f002000U ||
        widen_multiply_indexed_class == 0x0f006000U ||
        widen_multiply_indexed_class == 0x2f006000U;
    if (widen_multiply_register || widen_multiply_indexed) {
        const auto size = (instruction >> 22) & 3U;
        if (size == 3 || (widen_multiply_indexed && size == 0)) {
            throw std::runtime_error("invalid vector widening multiply arrangement");
        }
        const auto source_bytes = 1U << size;
        const auto destination_bytes = source_bytes * 2;
        const auto lanes = 16U / destination_bytes;
        const bool upper = (instruction & 0x40000000U) != 0;
        const bool is_unsigned = (instruction & 0x20000000U) != 0;
        const auto opcode = widen_multiply_indexed
            ? widen_multiply_indexed_class : widen_multiply_register_class;
        const bool accumulate = opcode == 0x0e208000U || opcode == 0x2e208000U ||
                                opcode == 0x0f002000U || opcode == 0x2f002000U;
        const bool subtract = opcode == 0x0e20a000U || opcode == 0x2e20a000U ||
                              opcode == 0x0f006000U || opcode == 0x2f006000U;
        const auto rm = widen_multiply_indexed && size == 1
            ? (instruction >> 16) & 15U : (instruction >> 16) & 31U;
        const auto index = widen_multiply_indexed
            ? (((instruction >> 11) & 1U) << (size == 1 ? 2 : 1)) |
              (((instruction >> 21) & 1U) << (size == 1 ? 1 : 0)) |
              (size == 1 ? ((instruction >> 20) & 1U) : 0U)
            : 0U;
        const auto source_mask = (1ULL << (source_bytes * 8)) - 1;
        const auto destination_mask = destination_bytes == 8
            ? UINT64_MAX : (1ULL << (destination_bytes * 8)) - 1;
        const auto sign_bit = 1ULL << (source_bytes * 8 - 1);
        const auto left = state_.v[rn];
        const auto right = state_.v[rm];
        const auto accumulator = state_.v[rd];
        const auto lane = [source_bytes, source_mask](const auto& vector,
                                                      unsigned byte) {
            return (vector[byte / 8] >> ((byte % 8) * 8)) & source_mask;
        };
        const auto signed_value = [sign_bit, source_bytes](std::uint64_t value) {
            return static_cast<std::int64_t>(value) -
                ((value & sign_bit) != 0 ? (1LL << (source_bytes * 8)) : 0LL);
        };
        std::array<std::uint64_t, 2> result{};
        for (unsigned i = 0; i < lanes; ++i) {
            const auto left_byte = (upper ? 8U : 0U) + i * source_bytes;
            const auto right_byte = widen_multiply_indexed
                ? index * source_bytes : left_byte;
            const auto lhs = lane(left, left_byte);
            const auto rhs = lane(right, right_byte);
            const auto product = is_unsigned ? lhs * rhs :
                static_cast<std::uint64_t>(signed_value(lhs) * signed_value(rhs));
            const auto destination_byte = i * destination_bytes;
            const auto word = destination_byte / 8;
            const auto shift = (destination_byte % 8) * 8;
            const auto old = (accumulator[word] >> shift) & destination_mask;
            const auto value = subtract ? old - product :
                               accumulate ? old + product : product;
            result[word] |= (value & destination_mask) << shift;
        }
        state_.v[rd] = result;
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd5384240U) {
        set_reg(rd, 0, true); // CurrentEL is EL0 in a user-mode guest.
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd53b00e0U) {
        set_reg(rd, guest_dczid_el0, true); // DCZID_EL0: ZVA permitted, 64 bytes.
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd53b4200U) {
        set_reg(rd, state_.nzcv & 0xf0000000U, true);
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd51b4200U) {
        state_.nzcv = static_cast<std::uint32_t>(reg(rd)) & 0xf0000000U;
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd5380400U || // ID_AA64PFR0_EL1
        (instruction & 0xffffffe0U) == 0xd5380600U || // ID_AA64ISAR0_EL1
        (instruction & 0xffffffe0U) == 0xd5380620U) { // ID_AA64ISAR1_EL1
        throw std::runtime_error("privileged ARM64 feature-register read from EL0");
    }
    if ((instruction & 0xffffffe0U) == 0xd53b4400U) {
        set_reg(rd, state_.fpcr, true);
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd51b4400U) {
        state_.fpcr = static_cast<std::uint32_t>(reg(rd));
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd53b4420U) {
        set_reg(rd, state_.fpsr, true);
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd51b4420U) {
        state_.fpsr = static_cast<std::uint32_t>(reg(rd));
        return false;
    }

    const auto structure_class = instruction & 0xbf20f000U;
    const auto structure_lane_class = instruction & 0xbf20c000U;
    const auto structure_replicate_class = instruction & 0xbf20f000U;
    if (structure_replicate_class == 0x0d00c000U ||
        structure_replicate_class == 0x0d20c000U ||
        structure_replicate_class == 0x0d00e000U ||
        structure_replicate_class == 0x0d20e000U) {
        const auto registers = 1U +
            ((instruction & 0x00200000U) != 0 ? 1U : 0U) +
            ((instruction & 0x00002000U) != 0 ? 2U : 0U);
        const auto element_bytes = 1U << ((instruction >> 10) & 3U);
        const bool quad = (instruction & 0x40000000U) != 0;
        const bool post_index = (instruction & 0x00800000U) != 0;
        const auto offset_register = (instruction >> 16) & 31U;
        if (!post_index && offset_register != 0) {
            throw std::runtime_error("invalid structure replicate load address");
        }
        const auto vector_bytes = quad ? 16U : 8U;
        const auto address = reg(rn, true);
        for (unsigned index = 0; index < registers; ++index) {
            std::uint64_t element = 0;
            for (unsigned byte = 0; byte < element_bytes; ++byte) {
                element |= static_cast<std::uint64_t>(
                    read8(address + index * element_bytes + byte)) << (byte * 8);
            }
            std::array<std::uint64_t, 2> result{};
            for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
                result[byte / 8] |= element << ((byte % 8) * 8);
            }
            state_.v[(rd + index) & 31U] = result;
        }
        if (post_index) {
            const auto offset = offset_register == 31
                ? registers * element_bytes : reg(offset_register);
            set_reg(rn, address + offset, true, true);
        }
        return false;
    }
    if (structure_class == 0x0c008000U || // LD2/ST2
        structure_class == 0x0c004000U || // LD3/ST3
        structure_class == 0x0c000000U) { // LD4/ST4
        const bool load = (instruction & 0x00400000U) != 0;
        const bool post_index = (instruction & 0x00800000U) != 0;
        const auto offset_register = (instruction >> 16) & 31U;
        if (!post_index && offset_register != 0) {
            throw std::runtime_error("invalid interleaved structure address");
        }
        const auto registers = structure_class == 0x0c008000U ? 2U :
                               structure_class == 0x0c004000U ? 3U : 4U;
        const auto element_bytes = 1U << ((instruction >> 10) & 3U);
        const auto vector_bytes = (instruction & 0x40000000U) != 0 ? 16U : 8U;
        if (element_bytes == 8 && vector_bytes == 8) {
            throw std::runtime_error("invalid interleaved structure arrangement");
        }
        const auto address = reg(rn, true);
        std::array<std::array<std::uint64_t, 2>, 4> values{};
        if (!load) {
            for (unsigned index = 0; index < registers; ++index) {
                values[index] = state_.v[(rd + index) & 31U];
            }
        }
        for (unsigned byte = 0; byte < vector_bytes; byte += element_bytes) {
            for (unsigned index = 0; index < registers; ++index) {
                const auto memory_address = address + byte * registers +
                                            index * element_bytes;
                for (unsigned part = 0; part < element_bytes; ++part) {
                    const auto vector_byte = byte + part;
                    const auto word = vector_byte / 8;
                    const auto shift = (vector_byte % 8) * 8;
                    if (load) {
                        values[index][word] |=
                            static_cast<std::uint64_t>(read8(memory_address + part))
                            << shift;
                    } else {
                        write8(memory_address + part,
                               static_cast<std::uint8_t>(values[index][word] >> shift));
                    }
                }
            }
        }
        if (load) {
            for (unsigned index = 0; index < registers; ++index) {
                state_.v[(rd + index) & 31U] = values[index];
            }
        }
        if (post_index) {
            const auto offset = offset_register == 31
                ? registers * vector_bytes : reg(offset_register);
            set_reg(rn, address + offset, true, true);
        }
        return false;
    }
    if (structure_lane_class == 0x0d000000U ||
        structure_lane_class == 0x0d004000U ||
        structure_lane_class == 0x0d008000U ||
        structure_lane_class == 0x0d200000U ||
        structure_lane_class == 0x0d204000U ||
        structure_lane_class == 0x0d208000U) {
        const bool load = (instruction & 0x00400000U) != 0;
        const bool post_index = (instruction & 0x00800000U) != 0;
        const auto offset_register = (instruction >> 16) & 31U;
        if (!post_index && offset_register != 0) {
            throw std::runtime_error("invalid structure lane address");
        }
        const auto registers = (instruction & 0x00200000U) != 0
                                   ? ((instruction & 0x00002000U) != 0 ? 4U : 2U)
                                   : ((instruction & 0x00002000U) != 0 ? 3U : 1U);
        const auto opcode = (instruction >> 14) & 3U;
        const auto size = (instruction >> 10) & 3U;
        const auto q = (instruction >> 30) & 1U;
        const auto s = (instruction >> 12) & 1U;
        unsigned element_bytes = 0;
        unsigned lane_index = 0;
        if (opcode == 0) {
            element_bytes = 1;
            lane_index = (q << 3) | (s << 2) | size;
        } else if (opcode == 1 && (size & 1U) == 0) {
            element_bytes = 2;
            lane_index = (q << 2) | (s << 1) | (size >> 1);
        } else if (opcode == 2 && size <= 1 && (size == 0 || s == 0)) {
            element_bytes = size == 0 ? 4U : 8U;
            lane_index = size == 0 ? (q << 1) | s : q;
        } else {
            throw std::runtime_error("invalid structure lane arrangement");
        }
        const auto lane_byte = lane_index * element_bytes;
        const auto address = reg(rn, true);
        for (unsigned index = 0; index < registers; ++index) {
            auto& vector = state_.v[(rd + index) & 31U];
            for (unsigned byte = 0; byte < element_bytes; ++byte) {
                const auto vector_byte = lane_byte + byte;
                auto& word = vector[vector_byte / 8];
                const auto shift = (vector_byte % 8) * 8;
                const auto memory_address = address + index * element_bytes + byte;
                if (load) {
                    word = (word & ~(0xffULL << shift)) |
                           (static_cast<std::uint64_t>(read8(memory_address)) << shift);
                } else {
                    write8(memory_address, static_cast<std::uint8_t>(word >> shift));
                }
            }
        }
        if (post_index) {
            const auto offset = offset_register == 31
                ? registers * element_bytes : reg(offset_register);
            set_reg(rn, address + offset, true, true);
        }
        return false;
    }
    if (structure_class == 0x0c002000U || structure_class == 0x0c006000U ||
        structure_class == 0x0c00a000U || structure_class == 0x0c007000U) {
        const bool load = (instruction & 0x00400000U) != 0;
        const bool post_index = (instruction & 0x00800000U) != 0;
        const bool quad = (instruction & 0x40000000U) != 0;
        const auto bytes = quad ? 16U : 8U;
        const auto opcode = instruction & 0x0000f000U;
        const auto registers = opcode == 0x2000U ? 4U :
                               opcode == 0x6000U ? 3U :
                               opcode == 0xa000U ? 2U : 1U;
        const auto address = reg(rn, true);
        for (unsigned index = 0; index < registers; ++index) {
            auto& vector = state_.v[(rd + index) & 31U];
            const auto element = address + index * bytes;
            if (load) {
                vector[0] = read64(element);
                vector[1] = quad ? read64(element + 8) : 0;
            } else {
                write64(element, vector[0]);
                if (quad) write64(element + 8, vector[1]);
            }
        }
        if (post_index) {
            const auto offset_register = (instruction >> 16) & 31U;
            const auto offset = offset_register == 31 ? registers * bytes
                                                      : reg(offset_register);
            set_reg(rn, address + offset, true, true);
        }
        return false;
    }

    const auto address_class = instruction & 0x9f000000U;
    if (address_class == 0x10000000U || address_class == 0x90000000U) {
        const auto immediate = static_cast<std::int64_t>(
            ((instruction >> 5) & 0x7ffffU) << 2 | ((instruction >> 29) & 3U));
        const auto signed_immediate = immediate >= (1LL << 20)
                                          ? immediate - (1LL << 21) : immediate;
        const auto base = address_class == 0x90000000U ? current_pc & ~0xfffULL : current_pc;
        set_reg(rd, base + signed_immediate *
                             (address_class == 0x90000000U ? 4096LL : 1LL), true);
        return false;
    }

    const auto move_class = instruction & 0x7f800000U;
    if (move_class == 0x12800000U || move_class == 0x52800000U ||
        move_class == 0x72800000U) {
        const auto shift = ((instruction >> 21) & 3U) * 16U;
        if (!is_64_bit && shift > 16) throw std::runtime_error("invalid 32-bit MOV shift");
        const auto immediate = static_cast<std::uint64_t>((instruction >> 5) & 0xffffU) << shift;
        std::uint64_t value = immediate;
        if (move_class == 0x12800000U) value = ~immediate;
        if (move_class == 0x72800000U) {
            const auto mask = 0xffffULL << shift;
            value = (reg(rd) & ~mask) | immediate;
        }
        set_reg(rd, value, is_64_bit);
        return false;
    }

    if ((instruction & 0x7f800000U) == 0x53000000U ||
        (instruction & 0x7f800000U) == 0x13000000U ||
        (instruction & 0x7f800000U) == 0x33000000U) {
        const bool signed_field = (instruction & 0x40000000U) == 0;
        const bool merge_field = (instruction & 0x7f800000U) == 0x33000000U;
        const auto width = is_64_bit ? 64U : 32U;
        const auto immr = (instruction >> 16) & 63U;
        const auto imms = (instruction >> 10) & 63U;
        const bool n = (instruction & 0x00400000U) != 0;
        if (n != is_64_bit || immr >= width || imms >= width) {
            throw std::runtime_error("invalid ARM64 unsigned bitfield form");
        }
        const auto source = reg(rn);
        const auto low_mask = [](unsigned bits) {
            return bits == 64 ? UINT64_MAX : (1ULL << bits) - 1;
        };
        std::uint64_t value = 0;
        if (merge_field) {
            const auto shift = imms >= immr ? 0U : width - immr;
            const auto field_width = imms >= immr ? imms - immr + 1
                                                   : imms + 1;
            const auto destination_mask = low_mask(field_width) << shift;
            const auto selected = imms >= immr
                ? source >> immr : source << shift;
            value = (reg(rd) & ~destination_mask) |
                    (selected & destination_mask);
        } else {
            value = imms >= immr
                ? (source >> immr) & low_mask(imms - immr + 1)
                : (source & low_mask(imms + 1)) << (width - immr);
        }
        if (signed_field && !merge_field) {
            const auto sign_index = imms >= immr ? imms - immr
                                                  : width - immr + imms;
            if ((value & (1ULL << sign_index)) != 0) {
                value |= ~low_mask(sign_index + 1);
            }
        }
        set_reg(rd, value, is_64_bit);
        return false;
    }

    if ((instruction & 0x7f800000U) == 0x13800000U) {
        const auto width = is_64_bit ? 64U : 32U;
        const auto shift = (instruction >> 10) & 63U;
        const bool n = (instruction & 0x00400000U) != 0;
        if (n != is_64_bit || shift >= width) {
            throw std::runtime_error("invalid ARM64 extract shift");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto mask = is_64_bit ? UINT64_MAX : UINT32_MAX;
        const auto low = reg(rm) & mask;
        const auto high = reg(rn) & mask;
        const auto value = shift == 0 ? low
            : (low >> shift) | ((high << (width - shift)) & mask);
        set_reg(rd, value, is_64_bit);
        return false;
    }

    if ((instruction & 0x1f800000U) == 0x12000000U) {
        const auto mask = logical_immediate_mask(instruction, is_64_bit);
        const auto source = reg(rn);
        const auto operation = (instruction >> 29) & 3U;
        const auto result = operation == 0 || operation == 3 ? source & mask
                            : operation == 1 ? source | mask : source ^ mask;
        // Logical immediates without flag updates write SP when Rd is 31;
        // ANDS instead writes XZR (the TST alias).
        set_reg(rd, result, is_64_bit, operation != 3);
        if (operation == 3) {
            const auto value = is_64_bit ? result : static_cast<std::uint32_t>(result);
            const auto sign = is_64_bit ? 1ULL << 63 : 1ULL << 31;
            state_.nzcv = ((value & sign) != 0 ? 0x80000000U : 0) |
                          (value == 0 ? 0x40000000U : 0);
        }
        return false;
    }

    if ((instruction & 0x1f800000U) == 0x11000000U) {
        const bool subtract = (instruction & 0x40000000U) != 0;
        const bool set_flags = (instruction & 0x20000000U) != 0;
        const auto shift = ((instruction >> 22) & 1U) ? 12U : 0U;
        const auto immediate = static_cast<std::uint64_t>((instruction >> 10) & 0xfffU) << shift;
        const auto mask = is_64_bit ? UINT64_MAX : UINT32_MAX;
        const auto lhs = reg(rn, true) & mask;
        const auto result = (subtract ? lhs - immediate : lhs + immediate) & mask;
        if (set_flags) {
            state_.nzcv = arithmetic_flags(lhs, immediate, result, subtract, is_64_bit);
        }
        set_reg(rd, result, is_64_bit, !set_flags);
        return false;
    }

    const auto conditional_compare_class = instruction & 0x1fe00c10U;
    if (conditional_compare_class == 0x1a400800U ||
        conditional_compare_class == 0x1a400000U) {
        const bool subtract = (instruction & 0x40000000U) != 0;
        const auto condition = (instruction >> 12) & 15U;
        if (condition_holds(condition, state_.nzcv)) {
            const auto mask = is_64_bit ? UINT64_MAX : UINT32_MAX;
            const auto lhs = reg(rn) & mask;
            const auto operand = (instruction >> 16) & 31U;
            const auto rhs = conditional_compare_class == 0x1a400800U
                                 ? static_cast<std::uint64_t>(operand)
                                 : reg(operand) & mask;
            const auto result = (subtract ? lhs - rhs : lhs + rhs) & mask;
            state_.nzcv = arithmetic_flags(lhs, rhs, result, subtract, is_64_bit);
        } else {
            state_.nzcv = (instruction & 15U) << 28;
        }
        return false;
    }

    if ((instruction & 0x1f000000U) == 0x0a000000U) {
        const auto operation = (instruction >> 29) & 3U;
        const auto shift_type = (instruction >> 22) & 3U;
        const auto shift = (instruction >> 10) & 63U;
        const auto rm = (instruction >> 16) & 31U;
        if (!is_64_bit && shift >= 32) {
            throw std::runtime_error("unsupported ARM64 logical shifted-register form");
        }
        std::uint64_t rhs = is_64_bit ? reg(rm) : static_cast<std::uint32_t>(reg(rm));
        if (shift_type == 0) rhs <<= shift;
        if (shift_type == 1) rhs >>= shift;
        if (shift_type == 2) {
            rhs = is_64_bit
                      ? static_cast<std::uint64_t>(static_cast<std::int64_t>(rhs) >> shift)
                      : static_cast<std::uint32_t>(static_cast<std::int32_t>(rhs) >> shift);
        }
        if (shift_type == 3) {
            rhs = is_64_bit ? std::rotr(rhs, static_cast<int>(shift))
                            : std::rotr(static_cast<std::uint32_t>(rhs),
                                        static_cast<int>(shift));
        }
        if (!is_64_bit) rhs = static_cast<std::uint32_t>(rhs);
        if ((instruction & 0x00200000U) != 0) rhs = ~rhs;
        const auto lhs = is_64_bit ? reg(rn) : static_cast<std::uint32_t>(reg(rn));
        const auto result = operation == 0 || operation == 3 ? lhs & rhs
                            : operation == 1 ? lhs | rhs : lhs ^ rhs;
        set_reg(rd, result, is_64_bit);
        if (operation == 3) {
            const auto value = is_64_bit ? result : static_cast<std::uint32_t>(result);
            const auto sign = is_64_bit ? 1ULL << 63 : 1ULL << 31;
            state_.nzcv = ((value & sign) != 0 ? 0x80000000U : 0) |
                          (value == 0 ? 0x40000000U : 0);
        }
        return false;
    }

    if ((instruction & 0x1f200000U) == 0x0b000000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto shift_type = (instruction >> 22) & 3U;
        const auto shift = (instruction >> 10) & 63U;
        if (shift_type == 3 || (!is_64_bit && shift >= 32)) {
            throw std::runtime_error("invalid ARM64 shifted-register addition");
        }
        const auto lhs = is_64_bit ? reg(rn) : static_cast<std::uint32_t>(reg(rn));
        std::uint64_t rhs = is_64_bit ? reg(rm) : static_cast<std::uint32_t>(reg(rm));
        if (shift_type == 0) rhs <<= shift;
        if (shift_type == 1) rhs >>= shift;
        if (shift_type == 2) {
            rhs = is_64_bit
                      ? static_cast<std::uint64_t>(static_cast<std::int64_t>(rhs) >> shift)
                      : static_cast<std::uint32_t>(static_cast<std::int32_t>(rhs) >> shift);
        }
        const auto mask = is_64_bit ? UINT64_MAX : UINT32_MAX;
        rhs &= mask;
        const bool subtract = (instruction & 0x40000000U) != 0;
        const auto result = (subtract ? lhs - rhs : lhs + rhs) & mask;
        if ((instruction & 0x20000000U) != 0) {
            state_.nzcv = arithmetic_flags(lhs, rhs, result, subtract, is_64_bit);
        }
        set_reg(rd, result, is_64_bit);
        return false;
    }

    const auto multiply_add_class = instruction & 0x7fe08000U;
    if (multiply_add_class == 0x1b000000U ||
        multiply_add_class == 0x1b008000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto ra = (instruction >> 10) & 31U;
        const auto product = is_64_bit ? reg(rn) * reg(rm)
                                       : static_cast<std::uint32_t>(reg(rn)) *
                                         static_cast<std::uint32_t>(reg(rm));
        const auto result = multiply_add_class == 0x1b008000U
            ? reg(ra) - product : reg(ra) + product;
        set_reg(rd, result, is_64_bit);
        return false;
    }
    const auto widening_multiply_class = instruction & 0xffe08000U;
    if (widening_multiply_class == 0x9b200000U ||
        widening_multiply_class == 0x9b208000U ||
        widening_multiply_class == 0x9ba00000U ||
        widening_multiply_class == 0x9ba08000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto ra = (instruction >> 10) & 31U;
        const bool unsigned_operands = (instruction & 0x00800000U) != 0;
        const auto left = static_cast<std::uint32_t>(reg(rn));
        const auto right = static_cast<std::uint32_t>(reg(rm));
        const auto product = unsigned_operands
            ? static_cast<std::uint64_t>(left) * right
            : static_cast<std::uint64_t>(
                static_cast<std::int64_t>(std::bit_cast<std::int32_t>(left)) *
                std::bit_cast<std::int32_t>(right));
        const auto result = (instruction & 0x00008000U) != 0
            ? reg(ra) - product : reg(ra) + product;
        set_reg(rd, result, true);
        return false;
    }
    const auto multiply_high_class = instruction & 0xffe0fc00U;
    if (multiply_high_class == 0x9b407c00U ||
        multiply_high_class == 0x9bc07c00U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto lhs = reg(rn);
        const auto rhs = reg(rm);
        auto high = unsigned_multiply_high(lhs, rhs);
        if (multiply_high_class == 0x9b407c00U) {
            if (std::bit_cast<std::int64_t>(lhs) < 0) high -= rhs;
            if (std::bit_cast<std::int64_t>(rhs) < 0) high -= lhs;
        }
        set_reg(rd, high, true);
        return false;
    }

    const auto divide_class = instruction & 0x7fe0fc00U;
    if (divide_class == 0x1ac00800U || divide_class == 0x1ac00c00U) {
        const auto rm = (instruction >> 16) & 31U;
        const bool wide = (instruction & 0x80000000U) != 0;
        const auto lhs = wide ? reg(rn) : static_cast<std::uint32_t>(reg(rn));
        const auto rhs = wide ? reg(rm) : static_cast<std::uint32_t>(reg(rm));
        std::uint64_t result = 0;
        if (rhs != 0) {
            if (divide_class == 0x1ac00800U) {
                result = lhs / rhs;
            } else if (wide) {
                const auto dividend = std::bit_cast<std::int64_t>(lhs);
                const auto divisor = std::bit_cast<std::int64_t>(rhs);
                result = dividend == INT64_MIN && divisor == -1
                             ? lhs : static_cast<std::uint64_t>(dividend / divisor);
            } else {
                const auto dividend = std::bit_cast<std::int32_t>(
                    static_cast<std::uint32_t>(lhs));
                const auto divisor = std::bit_cast<std::int32_t>(
                    static_cast<std::uint32_t>(rhs));
                result = dividend == INT32_MIN && divisor == -1
                             ? lhs : static_cast<std::uint32_t>(dividend / divisor);
            }
        }
        set_reg(rd, result, wide);
        return false;
    }

    if ((instruction & 0x1fe0fc00U) == 0x1a000000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto mask = is_64_bit ? UINT64_MAX : UINT32_MAX;
        const auto sign = is_64_bit ? 1ULL << 63 : 1ULL << 31;
        const auto lhs = reg(rn) & mask;
        const auto rhs = reg(rm) & mask;
        const bool subtract = (instruction & 0x40000000U) != 0;
        const bool set_flags = (instruction & 0x20000000U) != 0;
        const auto carry_in = (state_.nzcv & 0x20000000U) != 0 ? 1ULL : 0ULL;
        const auto addend = subtract ? (~rhs & mask) : rhs;
        const auto partial = (lhs + addend) & mask;
        const bool first_carry = partial < lhs;
        const auto result = (partial + carry_in) & mask;
        const bool carry_out = first_carry || result < partial;
        if (set_flags) {
            const bool overflow = subtract
                ? ((lhs ^ rhs) & (lhs ^ result) & sign) != 0
                : ((~(lhs ^ rhs)) & (lhs ^ result) & sign) != 0;
            state_.nzcv = ((result & sign) != 0 ? 0x80000000U : 0) |
                          (result == 0 ? 0x40000000U : 0) |
                          (carry_out ? 0x20000000U : 0) |
                          (overflow ? 0x10000000U : 0);
        }
        set_reg(rd, result, is_64_bit);
        return false;
    }
    if ((instruction & 0x7fe0f000U) == 0x1ac02000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto shift = static_cast<unsigned>(reg(rm) & (is_64_bit ? 63U : 31U));
        const auto operation = (instruction >> 10) & 3U;
        const auto source = is_64_bit ? reg(rn) :
                            static_cast<std::uint32_t>(reg(rn));
        std::uint64_t result = 0;
        if (operation == 0) result = source << shift;
        else if (operation == 1) result = source >> shift;
        else if (operation == 2) {
            result = is_64_bit
                ? static_cast<std::uint64_t>(static_cast<std::int64_t>(source) >> shift)
                : static_cast<std::uint32_t>(
                      static_cast<std::int32_t>(source) >> shift);
        } else {
            result = is_64_bit ? std::rotr(source, static_cast<int>(shift))
                               : std::rotr(static_cast<std::uint32_t>(source),
                                           static_cast<int>(shift));
        }
        set_reg(rd, result, is_64_bit);
        return false;
    }
    if ((instruction & 0x7ffffc00U) == 0x5ac01000U) {
        const auto count = is_64_bit
                               ? std::countl_zero(reg(rn))
                               : std::countl_zero(
                                     static_cast<std::uint32_t>(reg(rn)));
        set_reg(rd, static_cast<std::uint64_t>(count), is_64_bit);
        return false;
    }
    const auto reverse_class = instruction & 0xfffffc00U;
    if (reverse_class == 0x5ac00400U || reverse_class == 0xdac00400U ||
        reverse_class == 0x5ac00800U || reverse_class == 0xdac00800U ||
        reverse_class == 0xdac00c00U) {
        const auto source = reg(rn);
        const bool wide = (instruction & 0x80000000U) != 0;
        const auto bytes = wide ? 8U : 4U;
        const auto group = reverse_class == 0x5ac00400U ||
                                   reverse_class == 0xdac00400U ? 2U :
                           reverse_class == 0xdac00800U ? 4U : bytes;
        std::uint64_t result = 0;
        for (unsigned byte = 0; byte < bytes; ++byte) {
            const auto from = (byte / group) * group + group - 1 - byte % group;
            result |= ((source >> (from * 8)) & 0xffULL) << (byte * 8);
        }
        set_reg(rd, result, wide);
        return false;
    }
    if (reverse_class == 0x5ac00000U || reverse_class == 0xdac00000U) {
        const bool wide = (instruction & 0x80000000U) != 0;
        const auto bits = wide ? 64U : 32U;
        const auto source = reg(rn);
        std::uint64_t result = 0;
        for (unsigned bit = 0; bit < bits; ++bit) {
            result |= ((source >> bit) & 1ULL) << (bits - 1 - bit);
        }
        set_reg(rd, result, wide);
        return false;
    }

    if ((instruction & 0x1fe00000U) == 0x0b200000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto option = (instruction >> 13) & 7U;
        const auto shift = (instruction >> 10) & 7U;
        if (shift > 4) throw std::runtime_error("invalid ARM64 extended-register shift");
        const auto source_bits = 8U << (option & 3U);
        const auto source_mask = source_bits == 64 ? UINT64_MAX
                                                   : (1ULL << source_bits) - 1;
        auto rhs = reg(rm) & source_mask;
        if ((option & 4U) != 0 && source_bits < 64 &&
            (rhs & (1ULL << (source_bits - 1))) != 0) {
            rhs |= ~source_mask;
        }
        rhs <<= shift;
        const auto mask = is_64_bit ? UINT64_MAX : UINT32_MAX;
        rhs &= mask;
        const auto lhs = reg(rn, true) & mask;
        const bool subtract = (instruction & 0x40000000U) != 0;
        const bool set_flags = (instruction & 0x20000000U) != 0;
        const auto result = (subtract ? lhs - rhs : lhs + rhs) & mask;
        if (set_flags) {
            state_.nzcv = arithmetic_flags(lhs, rhs, result, subtract, is_64_bit);
        }
        set_reg(rd, result, is_64_bit, !set_flags);
        return false;
    }

    if ((instruction & 0x1fe00800U) == 0x1a800000U) {
        const auto rm = (instruction >> 16) & 31U;
        const auto condition = (instruction >> 12) & 15U;
        const bool invert = (instruction & 0x40000000U) != 0;
        const bool increment = (instruction & 0x00000400U) != 0;
        const bool selected = condition_holds(condition, state_.nzcv);
        auto value = selected ? reg(rn) : reg(rm);
        if (!selected) {
            if (invert) value = ~value;
            if (increment) ++value;
        }
        set_reg(rd, value, is_64_bit);
        return false;
    }

    const auto memory_class = instruction & 0xffc00000U;
    if ((instruction & 0x3f000000U) == 0x18000000U) {
        const auto encoded = static_cast<std::int64_t>((instruction >> 5) & 0x7ffffU);
        const auto displacement = (encoded >= (1LL << 18) ? encoded - (1LL << 19)
                                                          : encoded) * 4;
        const auto address = current_pc + displacement;
        const auto operation = instruction >> 30;
        if (operation == 0) set_reg(rd, read32(address), false);
        if (operation == 1) set_reg(rd, read64(address), true);
        if (operation == 2) {
            set_reg(rd, static_cast<std::uint64_t>(
                            static_cast<std::int64_t>(static_cast<std::int32_t>(read32(address)))),
                    true);
        }
        // The remaining encoding is PRFM (literal), an architectural hint.
        return false;
    }
    if ((instruction & 0x3f000000U) == 0x1c000000U) {
        const auto encoded = static_cast<std::int64_t>((instruction >> 5) & 0x7ffffU);
        const auto displacement = (encoded >= (1LL << 18) ? encoded - (1LL << 19)
                                                          : encoded) * 4;
        const auto address = current_pc + displacement;
        const auto size = instruction >> 30;
        if (size > 2) throw std::runtime_error("invalid vector literal load");
        state_.v[rd] = {size == 0 ? read32(address) : read64(address),
                        size == 2 ? read64(address + 8) : 0};
        return false;
    }
    const bool vector_memory = memory_class == 0x3d000000U ||
                               memory_class == 0x3d400000U ||
                               memory_class == 0x7d000000U ||
                               memory_class == 0x7d400000U ||
                               memory_class == 0xbd000000U ||
                               memory_class == 0xbd400000U ||
                               memory_class == 0xfd000000U ||
                               memory_class == 0xfd400000U ||
                               memory_class == 0x3d800000U ||
                               memory_class == 0x3dc00000U;
    if (vector_memory) {
        const bool load = (instruction & 0x00400000U) != 0;
        const bool quad = (instruction & 0x00800000U) != 0;
        const auto bytes = quad ? 16U : 1U << (instruction >> 30);
        const auto address = reg(rn, true) + ((instruction >> 10) & 0xfffU) * bytes;
        if (load) {
            std::uint64_t low = 0;
            for (unsigned byte = 0; byte < std::min(bytes, 8U); ++byte) {
                low |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
            }
            state_.v[rd] = {low, quad ? read64(address + 8) : 0};
        } else {
            for (unsigned byte = 0; byte < std::min(bytes, 8U); ++byte) {
                write8(address + byte,
                       static_cast<std::uint8_t>(state_.v[rd][0] >> (byte * 8)));
            }
            if (quad) write64(address + 8, state_.v[rd][1]);
        }
        return false;
    }
    if (memory_class == 0x39000000U || memory_class == 0x39400000U ||
        memory_class == 0x79000000U || memory_class == 0x79400000U) {
        const bool load = (instruction & 0x00400000U) != 0;
        const bool halfword = (instruction & 0x40000000U) != 0;
        const auto address = reg(rn, true) +
                             ((instruction >> 10) & 0xfffU) * (halfword ? 2U : 1U);
        if (load) {
            const auto value = halfword ? static_cast<std::uint32_t>(read8(address)) |
                                              (static_cast<std::uint32_t>(read8(address + 1)) << 8)
                                        : read8(address);
            set_reg(rd, value, false);
        } else {
            write8(address, static_cast<std::uint8_t>(reg(rd)));
            if (halfword) {
                write8(address + 1, static_cast<std::uint8_t>(reg(rd) >> 8));
            }
        }
        return false;
    }
    // LDRSB/LDRSH (unsigned offset): opc<0> (bit 22) set sign-extends into W and
    // clears the upper half, clear sign-extends into X. The W result is formed
    // here, from the opc bit: built by MSVC 19.51 (Visual Studio 2026) on the CI
    // runner, the earlier form of these two branches returned W results
    // sign-extended to 64 bits, while the other LDRSB/LDRSH forms were right
    // (test_reference_signed_narrow_loads).
    if (memory_class == 0x39c00000U || memory_class == 0x39800000U) {
        const auto address = reg(rn, true) + ((instruction >> 10) & 0xfffU);
        const auto loaded = static_cast<std::int8_t>(read8(address));
        const std::uint64_t value = (instruction & 0x00400000U) != 0
            ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(loaded)))
            : static_cast<std::uint64_t>(static_cast<std::int64_t>(loaded));
        set_reg(rd, value, true);
        return false;
    }
    if (memory_class == 0x79c00000U || memory_class == 0x79800000U) {
        const auto address = reg(rn, true) +
                             ((instruction >> 10) & 0xfffU) * 2U;
        const auto raw = static_cast<std::uint16_t>(read8(address)) |
                         static_cast<std::uint16_t>(read8(address + 1) << 8);
        const auto loaded = static_cast<std::int16_t>(raw);
        const std::uint64_t value = (instruction & 0x00400000U) != 0
            ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(loaded)))
            : static_cast<std::uint64_t>(static_cast<std::int64_t>(loaded));
        set_reg(rd, value, true);
        return false;
    }
    if (memory_class == 0xf9400000U || memory_class == 0xf9000000U ||
        memory_class == 0xb9400000U || memory_class == 0xb9000000U) {
        const bool wide = (instruction & 0x40000000U) != 0;
        const auto scale = wide ? 8U : 4U;
        const auto address = reg(rn, true) + ((instruction >> 10) & 0xfffU) * scale;
        if ((instruction & 0x00400000U) != 0) {
            set_reg(rd, wide ? read64(address) : read32(address), wide);
        } else if (wide) {
            write64(address, reg(rd));
        } else {
            write32(address, static_cast<std::uint32_t>(reg(rd)));
        }
        return false;
    }
    if (memory_class == 0xb9800000U) {
        const auto address = reg(rn, true) + ((instruction >> 10) & 0xfffU) * 4U;
        set_reg(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(
                        static_cast<std::int32_t>(read32(address)))), true);
        return false;
    }
    if (memory_class == 0xf9800000U) {
        return false; // PRFM is a performance hint, with no architectural state.
    }

    const auto indexed_vector_class = instruction & 0x3f200c00U;
    if (indexed_vector_class == 0x3c000400U ||
        indexed_vector_class == 0x3c000c00U) {
        const bool load = (instruction & 0x00400000U) != 0;
        const bool quad = (instruction & 0x00800000U) != 0;
        if (quad && (instruction >> 30) != 0) {
            throw std::runtime_error("invalid indexed vector memory size");
        }
        const auto bytes = quad ? 16U : 1U << (instruction >> 30);
        const auto encoded = static_cast<std::int64_t>((instruction >> 12) & 0x1ffU);
        const auto displacement = encoded >= 256 ? encoded - 512 : encoded;
        const bool pre_index = indexed_vector_class == 0x3c000c00U;
        const auto old_base = reg(rn, true);
        const auto address = pre_index ? old_base + displacement : old_base;
        if (load) {
            std::array<std::uint64_t, 2> value{};
            for (unsigned byte = 0; byte < bytes; ++byte) {
                value[byte / 8] |= static_cast<std::uint64_t>(read8(address + byte))
                                   << ((byte % 8) * 8);
            }
            state_.v[rd] = value;
        } else {
            for (unsigned byte = 0; byte < bytes; ++byte) {
                write8(address + byte, static_cast<std::uint8_t>(
                    state_.v[rd][byte / 8] >> ((byte % 8) * 8)));
            }
        }
        set_reg(rn, pre_index ? address : old_base + displacement, true, true);
        return false;
    }

    if ((instruction & 0x3f200c00U) == 0x3c000000U) {
        const bool load = (instruction & 0x00400000U) != 0;
        const auto bytes = (instruction & 0x00800000U) != 0
                               ? 16U : 1U << (instruction >> 30);
        const auto encoded = static_cast<std::int64_t>((instruction >> 12) & 0x1ffU);
        const auto displacement = encoded >= 256 ? encoded - 512 : encoded;
        const auto address = reg(rn, true) + displacement;
        if (load) {
            std::array<std::uint64_t, 2> value{};
            for (unsigned byte = 0; byte < bytes; ++byte) {
                value[byte / 8] |= static_cast<std::uint64_t>(read8(address + byte))
                                   << ((byte % 8) * 8);
            }
            state_.v[rd] = value;
        } else {
            for (unsigned byte = 0; byte < bytes; ++byte) {
                write8(address + byte, static_cast<std::uint8_t>(
                    state_.v[rd][byte / 8] >> ((byte % 8) * 8)));
            }
        }
        return false;
    }

    if ((instruction & 0x3f200c00U) == 0x3c200800U) {
        const auto operation = (instruction >> 22) & 3U;
        const bool quad = (operation & 2U) != 0;
        if (quad && (instruction >> 30) != 0) {
            throw std::runtime_error("invalid vector register-offset size");
        }
        const auto bytes = quad ? 16U : 1U << (instruction >> 30);
        const auto rm = (instruction >> 16) & 31U;
        const auto option = (instruction >> 13) & 7U;
        std::uint64_t offset = 0;
        if (option == 2) offset = static_cast<std::uint32_t>(reg(rm));
        else if (option == 3 || option == 7) offset = reg(rm);
        else if (option == 6) {
            offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(reg(rm))));
        } else {
            throw std::runtime_error("invalid vector register-offset extension");
        }
        if ((instruction & 0x1000U) != 0) {
            offset <<= (quad ? 4U : instruction >> 30);
        }
        const auto address = reg(rn, true) + offset;
        if ((operation & 1U) != 0) {
            std::array<std::uint64_t, 2> value{};
            for (unsigned byte = 0; byte < bytes; ++byte) {
                value[byte / 8] |= static_cast<std::uint64_t>(read8(address + byte))
                                   << ((byte % 8) * 8);
            }
            state_.v[rd] = value;
        } else {
            for (unsigned byte = 0; byte < bytes; ++byte) {
                write8(address + byte, static_cast<std::uint8_t>(
                    state_.v[rd][byte / 8] >> ((byte % 8) * 8)));
            }
        }
        return false;
    }

    if ((instruction & 0x3f200c00U) == 0x38200800U) {
        const auto access = (instruction >> 22) & 3U;
        const auto size = 1U << (instruction >> 30);
        if (size == 8 && access == 2) {
            const auto option = (instruction >> 13) & 7U;
            if ((option & 2U) == 0) {
                throw std::runtime_error("invalid PRFM register-offset extension");
            }
            return false; // PRFM has no required architectural memory access.
        }
        if (access >= 2 && (size == 8 || (size == 4 && access == 3))) {
            throw std::runtime_error("invalid signed register-offset load");
        }
        const auto rm = (instruction >> 16) & 31U;
        const auto option = (instruction >> 13) & 7U;
        std::uint64_t offset = 0;
        if (option == 2) offset = static_cast<std::uint32_t>(reg(rm));
        else if (option == 3 || option == 7) offset = reg(rm);
        else if (option == 6) {
            offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(reg(rm))));
        } else {
            throw std::runtime_error("invalid register-offset extension");
        }
        if ((instruction & 0x1000U) != 0) offset <<= (instruction >> 30);
        const auto address = reg(rn, true) + offset;
        if (access != 0) {
            std::uint64_t value = 0;
            for (unsigned byte = 0; byte < size; ++byte) {
                value |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
            }
            if (access >= 2) {
                const auto sign = 1ULL << (size * 8 - 1);
                if ((value & sign) != 0) value |= UINT64_MAX << (size * 8);
            }
            set_reg(rd, value, access == 2 || size == 8);
        } else {
            for (unsigned byte = 0; byte < size; ++byte) {
                write8(address + byte,
                       static_cast<std::uint8_t>(reg(rd) >> (byte * 8)));
            }
        }
        return false;
    }

    const auto indexed_integer_class = instruction & 0x3f200c00U;
    if (indexed_integer_class == 0x38000400U ||
        indexed_integer_class == 0x38000c00U) {
        const auto size = 1U << (instruction >> 30);
        const auto operation = (instruction >> 22) & 3U;
        if ((size == 8 && operation >= 2) ||
            (size == 4 && operation == 3)) {
            throw std::runtime_error("invalid indexed integer load/store");
        }
        const bool pre_index = (instruction & 0x00000800U) != 0;
        const auto immediate = static_cast<std::int64_t>((instruction >> 12) & 0x1ffU);
        const auto displacement = immediate >= 256 ? immediate - 512 : immediate;
        const auto old_base = reg(rn, true);
        const auto address = pre_index ? old_base + displacement : old_base;
        if (operation != 0) {
            std::uint64_t value = 0;
            for (unsigned byte = 0; byte < size; ++byte) {
                value |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
            }
            if (operation >= 2) {
                const auto sign = 1ULL << (size * 8 - 1);
                if ((value & sign) != 0) value |= UINT64_MAX << (size * 8);
            }
            set_reg(rd, value, operation == 2 || size == 8);
        } else {
            for (unsigned byte = 0; byte < size; ++byte) {
                write8(address + byte,
                       static_cast<std::uint8_t>(reg(rd) >> (byte * 8)));
            }
        }
        set_reg(rn, pre_index ? address : old_base + displacement, true, true);
        return false;
    }

    if (indexed_integer_class == 0x38000000U) {
        const auto size = 1U << (instruction >> 30);
        const auto operation = (instruction >> 22) & 3U;
        if (size == 8 && operation == 2) {
            return false; // PRFUM is an unscaled prefetch hint.
        }
        if ((size == 8 && operation >= 2) ||
            (size == 4 && operation == 3)) {
            throw std::runtime_error("invalid unscaled integer load/store");
        }
        const auto encoded = static_cast<std::int64_t>((instruction >> 12) & 0x1ffU);
        const auto displacement = encoded >= 256 ? encoded - 512 : encoded;
        const auto address = reg(rn, true) + displacement;
        if (operation != 0) {
            std::uint64_t value = 0;
            for (unsigned byte = 0; byte < size; ++byte) {
                value |= static_cast<std::uint64_t>(read8(address + byte)) << (byte * 8);
            }
            if (operation >= 2) {
                const auto sign = 1ULL << (size * 8 - 1);
                if ((value & sign) != 0) value |= UINT64_MAX << (size * 8);
            }
            set_reg(rd, value, operation == 2 || size == 8);
        } else {
            for (unsigned byte = 0; byte < size; ++byte) {
                write8(address + byte,
                       static_cast<std::uint8_t>(reg(rd) >> (byte * 8)));
            }
        }
        return false;
    }

    const auto pair_class = instruction & 0xffc00000U;
    if (pair_class == 0x69400000U || pair_class == 0x69c00000U ||
        pair_class == 0x68c00000U) {
        const auto rt2 = (instruction >> 10) & 31U;
        const auto imm7 = static_cast<std::int64_t>((instruction >> 15) & 0x7fU);
        const auto displacement = (imm7 >= 64 ? imm7 - 128 : imm7) * 4;
        const bool pre_index = pair_class == 0x69c00000U;
        const bool post_index = pair_class == 0x68c00000U;
        const auto old_base = reg(rn, true);
        const auto address = post_index ? old_base : old_base + displacement;
        const auto first = static_cast<std::int64_t>(
            static_cast<std::int32_t>(read32(address)));
        const auto second = static_cast<std::int64_t>(
            static_cast<std::int32_t>(read32(address + 4)));
        set_reg(rd, static_cast<std::uint64_t>(first), true);
        set_reg(rt2, static_cast<std::uint64_t>(second), true);
        if (pre_index || post_index) {
            set_reg(rn, pre_index ? address : old_base + displacement,
                    true, true);
        }
        return false;
    }
    if (pair_class == 0x28000000U || pair_class == 0x28400000U ||
        pair_class == 0xa8000000U || pair_class == 0xa8400000U ||
        pair_class == 0x29000000U || pair_class == 0x29400000U ||
        pair_class == 0x29800000U || pair_class == 0x29c00000U ||
        pair_class == 0x28800000U || pair_class == 0x28c00000U ||
        pair_class == 0xa9000000U || pair_class == 0xa9400000U ||
        pair_class == 0xa9800000U || pair_class == 0xa9c00000U ||
        pair_class == 0xa8800000U || pair_class == 0xa8c00000U) {
        const bool wide = (instruction & 0x80000000U) != 0;
        const bool load = (instruction & 0x00400000U) != 0;
        const bool pre_index = (instruction & 0x01800000U) == 0x01800000U;
        const bool post_index = (instruction & 0x01800000U) == 0x00800000U;
        const auto rt2 = (instruction >> 10) & 31U;
        const auto imm7 = static_cast<std::int64_t>((instruction >> 15) & 0x7fU);
        const auto displacement = (imm7 >= 64 ? imm7 - 128 : imm7) *
                                  (wide ? 8 : 4);
        const auto old_base = reg(rn, true);
        const auto address = pre_index || !post_index ? old_base + displacement : old_base;
        if (load) {
            const auto first = wide ? read64(address) : read32(address);
            const auto second = wide ? read64(address + (wide ? 8 : 4))
                                     : read32(address + 4);
            set_reg(rd, first, wide);
            set_reg(rt2, second, wide);
        } else {
            const auto first = reg(rd);
            const auto second = reg(rt2);
            if (wide) {
                write64(address, first);
                write64(address + 8, second);
            } else {
                write32(address, static_cast<std::uint32_t>(first));
                write32(address + 4, static_cast<std::uint32_t>(second));
            }
        }
        if (pre_index || post_index) {
            set_reg(rn, pre_index ? address : old_base + displacement, true, true);
        }
        return false;
    }

    if (pair_class == 0x2c000000U || pair_class == 0x2c400000U ||
        pair_class == 0x6c000000U || pair_class == 0x6c400000U ||
        pair_class == 0xac000000U || pair_class == 0xac400000U ||
        pair_class == 0x2d000000U || pair_class == 0x2d400000U ||
        pair_class == 0x2d800000U || pair_class == 0x2dc00000U ||
        pair_class == 0x2c800000U || pair_class == 0x2cc00000U ||
        pair_class == 0x6d000000U || pair_class == 0x6d400000U ||
        pair_class == 0x6d800000U || pair_class == 0x6dc00000U ||
        pair_class == 0x6c800000U || pair_class == 0x6cc00000U ||
        pair_class == 0xad000000U || pair_class == 0xad400000U ||
        pair_class == 0xad800000U || pair_class == 0xadc00000U ||
        pair_class == 0xac800000U || pair_class == 0xacc00000U) {
        const auto pair_size = (instruction >> 30) & 3U;
        const auto bytes = pair_size == 0 ? 4U : pair_size == 1 ? 8U : 16U;
        const bool load = (instruction & 0x00400000U) != 0;
        const bool pre_index = (instruction & 0x01800000U) == 0x01800000U;
        const bool post_index = (instruction & 0x01800000U) == 0x00800000U;
        const auto rt2 = (instruction >> 10) & 31U;
        const auto imm7 = static_cast<std::int64_t>((instruction >> 15) & 0x7fU);
        const auto displacement = (imm7 >= 64 ? imm7 - 128 : imm7) * bytes;
        const auto old_base = reg(rn, true);
        const auto address = pre_index || !post_index ? old_base + displacement : old_base;
        if (load) {
            const auto first_low = bytes == 4 ? read32(address) : read64(address);
            const auto first_high = bytes == 16 ? read64(address + 8) : 0;
            const auto second_low = bytes == 4 ? read32(address + bytes)
                                               : read64(address + bytes);
            const auto second_high = bytes == 16 ? read64(address + bytes + 8) : 0;
            state_.v[rd] = {first_low, first_high};
            state_.v[rt2] = {second_low, second_high};
        } else {
            const auto first = state_.v[rd];
            const auto second = state_.v[rt2];
            if (bytes == 4) {
                write32(address, static_cast<std::uint32_t>(first[0]));
                write32(address + bytes, static_cast<std::uint32_t>(second[0]));
            } else {
                write64(address, first[0]);
                if (bytes == 16) write64(address + 8, first[1]);
                write64(address + bytes, second[0]);
                if (bytes == 16) write64(address + bytes + 8, second[1]);
            }
        }
        if (pre_index || post_index) {
            set_reg(rn, pre_index ? address : old_base + displacement, true, true);
        }
        return false;
    }

    if ((instruction & 0xfc000000U) == 0x14000000U ||
        (instruction & 0xfc000000U) == 0x94000000U) {
        if ((instruction & 0xfc000000U) == 0x94000000U) state_.x[30] = state_.pc;
        state_.pc = current_pc + signed_branch_offset(instruction);
        return false;
    }
    if ((instruction & 0xff000010U) == 0x54000000U) {
        const auto encoded = static_cast<std::int64_t>((instruction >> 5) & 0x7ffffU);
        const auto displacement = (encoded >= (1LL << 18) ? encoded - (1LL << 19)
                                                          : encoded) * 4;
        if (condition_holds(instruction & 15U, state_.nzcv)) {
            state_.pc = current_pc + displacement;
        }
        return false;
    }
    if ((instruction & 0x7e000000U) == 0x34000000U) {
        const auto encoded = static_cast<std::int64_t>((instruction >> 5) & 0x7ffffU);
        const auto displacement = (encoded >= (1LL << 18) ? encoded - (1LL << 19)
                                                          : encoded) * 4;
        const auto value = is_64_bit ? reg(rd) : static_cast<std::uint32_t>(reg(rd));
        const bool nonzero = (instruction & 0x01000000U) != 0;
        if ((value != 0) == nonzero) state_.pc = current_pc + displacement;
        return false;
    }
    if ((instruction & 0x7e000000U) == 0x36000000U) {
        const auto bit = ((instruction >> 31) & 1U) * 32U +
                         ((instruction >> 19) & 31U);
        const auto encoded = static_cast<std::int64_t>((instruction >> 5) & 0x3fffU);
        const auto displacement = (encoded >= (1LL << 13) ? encoded - (1LL << 14)
                                                          : encoded) * 4;
        const bool nonzero = (instruction & 0x01000000U) != 0;
        if ((((reg(rd) >> bit) & 1U) != 0) == nonzero) {
            state_.pc = current_pc + displacement;
        }
        return false;
    }
    const auto register_branch = instruction & 0xfffffc1fU;
    if (register_branch == 0xd61f0000U || register_branch == 0xd63f0000U) {
        const auto target = reg(rn);
        if (register_branch == 0xd63f0000U) state_.x[30] = state_.pc;
        state_.pc = target;
        return state_.pc == return_sentinel;
    }
    if ((instruction & 0xfffffc1fU) == 0xd65f0000U) {
        state_.pc = reg(rn);
        return state_.pc == return_sentinel;
    }
    if (instruction == 0xd503203fU) { // YIELD
        std::this_thread::yield();
        return false;
    }
    if ((instruction & 0xffffffe0U) == 0xd50b7420U) { // DC ZVA, Xt
        const auto block = reg(rd) & ~(guest_dczva_bytes - 1);
        for (std::uint64_t offset = 0; offset < guest_dczva_bytes; ++offset) {
            write8(block + offset, 0);
        }
        return false;
    }
    if (instruction == 0xd503205fU || // WFE
        instruction == 0xd503207fU || // WFI
        instruction == 0xd503209fU || // SEV
        instruction == 0xd50320bfU) { // SEVL
        throw std::runtime_error("guest event/interrupt hint is unsupported");
    }
    // These pointer-authentication HINT encodings execute as NOPs on a guest
    // CPU that does not advertise PAuth. Do not generalize this to RETAA/RETAB.
    if (instruction == 0xd503201fU || // NOP
        instruction == 0xd50320ffU || // XPACLRI
        instruction == 0xd503233fU ||
        instruction == 0xd503237fU || instruction == 0xd50323bfU ||
        instruction == 0xd50323ffU) {
        return false;
    }
    if ((instruction & 0xfffff0ffU) == 0xd50330bfU) {
        std::atomic_thread_fence(std::memory_order_seq_cst); // DMB
        return false;
    }
    if ((instruction & 0xfffff0ffU) == 0xd503309fU) { // DSB
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return false;
    }
    if ((instruction & 0xfffff0ffU) == 0xd50330dfU) { // ISB
        // Instruction fetch is per dispatch. The JIT ends a block before ISB
        // and invalidates translated code when guest executable memory changes.
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return false;
    }
    if ((instruction & 0xfffff0ffU) == 0xd503305fU) { // CLREX
        reservation_ = {};
        return false;
    }

    std::ostringstream message;
    message << "unsupported ARM64 instruction 0x" << std::hex << std::setw(8)
            << std::setfill('0') << instruction << " at guest address 0x" << current_pc;
    if (const auto* module = module_at(current_pc)) {
        message << " in " << module->source() << "+0x"
                << current_pc - module->base();
    }
    throw std::runtime_error(message.str());
}

RunResult ReferenceEngine::run(std::uint64_t maximum_instructions) {
    struct RunningEngine {
        ReferenceEngine* previous;
        ~RunningEngine() { active_engine = previous; }
    } running{active_engine};
    active_engine = this;
    if (maximum_instructions == 0) maximum_instructions = instruction_limit_;
    if (maximum_instructions == UINT64_MAX) {
        throw std::runtime_error("invalid guest instruction limit");
    }
    PeImage* code_module_hint = nullptr;
    std::uint64_t code_module_hint_generation = UINT64_MAX;
    std::uint64_t previous_pc = 0;
    // Derived engines may override the checked virtual memory contract.
    const bool use_scoped_memory = typeid(*this) == typeid(ReferenceEngine);
    for (std::uint64_t count = 1; count <= maximum_instructions; ++count) {
        const auto call_address = state_.pc;
        if (call_address >= guest_call_base && call_address < guest_call_end) {
            GuestCallHandler handler;
            {
                AddressSpaceLock lock(*address_space_);
                const auto found = address_space_->guest_calls.find(call_address);
                if (found == address_space_->guest_calls.end()) {
                    std::ostringstream message;
                    message << "unknown synthetic guest call address 0x"
                            << std::hex << call_address;
                    throw std::runtime_error(message.str());
                }
                handler = found->second;
            }
            previous_pc = call_address;
            try {
                handler(state_, *this);
            } catch (const std::exception& error) {
                std::ostringstream message;
                message << error.what() << " during synthetic guest call 0x"
                        << std::hex << call_address << std::dec
                        << " (run step " << count << ')';
                throw std::runtime_error(message.str());
            }
            state_.pc = state_.x[30];
            if (state_.pc == return_sentinel) {
                return {static_cast<std::uint32_t>(state_.x[0]), count};
            }
            continue;
        }
        ScopedMemoryAccess instruction_memory(*this, use_scoped_memory);
        GuestMemory* const jit_memory = use_scoped_memory
            ? static_cast<GuestMemory*>(&instruction_memory) : this;
        const auto staged_import = staged_import_entries_.find(state_.pc);
        const auto shared_import = address_space_->import_entries.find(state_.pc);
        if (staged_import != staged_import_entries_.end() ||
            shared_import != address_space_->import_entries.end()) {
            const auto& binding = staged_import != staged_import_entries_.end()
                ? staged_import->second : shared_import->second;
            previous_pc = state_.pc;
            std::optional<std::uint32_t> exit_code;
            if (const auto native = binding.native) {
                instruction_memory.unlock_for_dispatch();
                native(state_);
            } else {
                // Pin copied metadata before unlocking: a callback can discard
                // or unload its originating module while the import is active.
                const auto import = binding.imported;
                instruction_memory.unlock_for_dispatch();
                if (!dispatcher_) throw std::runtime_error("imported Windows APIs require a bridge");
                try {
                    exit_code = dispatcher_(*import, state_, *this);
                } catch (const std::exception& error) {
                    std::ostringstream message;
                    message << error.what() << " during Windows import "
                            << import->dll << '!'
                            << import->function << " (guest LR 0x"
                            << std::hex << state_.x[30] << std::dec
                            << ", run step " << count << ')';
                    throw std::runtime_error(message.str());
                }
            }
            if (exit_code) return {*exit_code, count};
            state_.pc = state_.x[30];
            if (state_.pc == return_sentinel) {
                return {static_cast<std::uint32_t>(state_.x[0]), count};
            }
            continue;
        }
        bool known_jit_fallback = false;
        if (jit_enabled_ && jit_cache_) {
            if (jit_seen_generation_ != address_space_->code_generation) {
                jit_cache_->invalidate_all();
                jit_seen_generation_ = address_space_->code_generation;
            }
            const auto budget = static_cast<std::size_t>(
                maximum_instructions - count + 1);
            bool requires_exclusive = false;
            auto executed = jit_cache_->execute_cached(
                state_, state_.pc, address_space_->code_generation, budget,
                &known_jit_fallback, jit_memory, &requires_exclusive);
            force_exclusive_jit_ = requires_exclusive;
            if (executed != 0) {
                // Parallel chains retain their shared lease to amortize
                // cacheline contention. Pending writers end the chain before
                // its next invocation; each native region still caps256steps.
                // Exclusive chains retain their original short fairness bound.
                const auto maximum_chain_blocks = instruction_memory.maximum_chain_blocks();
                const auto maximum_chain_instructions = instruction_memory.maximum_chain_steps();
                for (std::size_t blocks = 1;
                     blocks < maximum_chain_blocks && executed < budget &&
                     executed < maximum_chain_instructions &&
                     !force_exclusive_jit_ &&
                     state_.pc != return_sentinel; ++blocks) {
                    if (!instruction_memory.permits_cached_region(
                            state_.pc, address_space_->code_generation)) break;
                    const auto following = jit_cache_->execute_cached(
                        state_, state_.pc, address_space_->code_generation,
                        std::min(budget - executed,
                                 maximum_chain_instructions - executed),
                        nullptr, jit_memory, &requires_exclusive);
                    force_exclusive_jit_ = requires_exclusive;
                    if (following == 0) break;
                    executed += following;
                }
                jit_executed_instructions_ += executed;
                count += executed - 1;
                if (state_.pc == return_sentinel) {
                    return {static_cast<std::uint32_t>(state_.x[0]), count};
                }
                continue;
            }
        }
        // A shared region miss returns before its effect. Quiesce readers
        // before checked decode/access, and refresh code identity after the
        // promotion gap in which executable memory may have changed.
        const auto promotion_generation = address_space_->generation;
        const auto promotion_code_generation = address_space_->code_generation;
        instruction_memory.promote_exclusive();
        if (promotion_generation != address_space_->generation ||
            promotion_code_generation != address_space_->code_generation) {
            // Imports and module publication were inspected before the gap.
            // Retry the unchanged current PC without charging a guest step.
            --count;
            continue;
        }
        if (jit_enabled_ && jit_cache_ &&
            jit_seen_generation_ != address_space_->code_generation) {
            jit_cache_->invalidate_all();
            jit_seen_generation_ = address_space_->code_generation;
            known_jit_fallback = false;
        }
        // Import/callback dispatch releases the address-space lock and can
        // destroy a staged image. Validate topology before dereferencing its
        // cached pointer on the next checked instruction fetch.
        if (code_module_hint_generation != address_space_->generation) {
            code_module_hint = nullptr;
            code_module_hint_generation = address_space_->generation;
        }
        PeImage* code_module = code_module_hint;
        if (code_module == nullptr || state_.pc < code_module->base() ||
            state_.pc - code_module->base() >= code_module->size()) {
            code_module = nullptr;
            code_module = module_at(state_.pc);
            code_module_hint = code_module;
        }
        const auto pc = state_.pc;
        const auto execution_fault = [this, pc, previous_pc](const char* reason) {
            std::ostringstream message;
            message << reason << " at guest address 0x" << std::hex << pc;
            message << " (previous PC 0x" << previous_pc << ", LR 0x"
                    << state_.x[30] << ", FP 0x" << state_.x[29]
                    << ", SP 0x" << state_.sp << ')';
            if (const auto region = query_region(pc)) {
                message << " in region 0x" << region->base << " size 0x"
                        << region->size << " r=" << region->readable
                        << " w=" << region->writable
                        << " x=" << region->executable << " bytes=";
                if (region->readable) {
                    const auto available = std::min<std::uint64_t>(
                        16, region->size - (pc - region->base));
                    try {
                        for (std::uint64_t index = 0; index < available; ++index) {
                            message << std::setw(2) << std::setfill('0')
                                    << static_cast<unsigned>(read8(pc + index));
                        }
                    } catch (const std::exception&) {
                        message << "<unreadable>";
                    }
                } else {
                    message << "<unreadable>";
                }
            }
            throw std::runtime_error(message.str());
        };
        previous_pc = pc;
        std::uint32_t instruction = 0;
        const MemoryRegion* code_region = nullptr;
        std::uint64_t code_end = pc + 4;
        if (code_module != nullptr) {
            instruction = code_module->instruction(pc);
            const auto rva = pc - code_module->base();
            for (const auto& section : code_module->sections()) {
                if (section.executable && rva >= section.rva &&
                    rva < static_cast<std::uint64_t>(section.rva) + section.size) {
                    code_end = code_module->base() + section.rva + section.size;
                    break;
                }
            }
        } else {
            if ((pc & 3U) != 0) {
                execution_fault("invalid guest instruction address");
            }
            auto region = address_space_->regions.upper_bound(pc);
            if (region == address_space_->regions.begin()) {
                execution_fault("guest attempted to execute unmapped memory");
            }
            --region;
            const auto& code = region->second;
            if (pc < code.base || pc - code.base > code.size() ||
                code.size() - static_cast<std::size_t>(pc - code.base) < 4 ||
                !code.executable) {
                execution_fault("guest attempted to execute non-code memory");
            }
            code_region = &code;
            code_end = code.base + code.size();
            for (unsigned byte = 0; byte < 4; ++byte) {
                instruction |= static_cast<std::uint32_t>(
                    code.read(static_cast<std::size_t>(pc - code.base) + byte)) <<
                    (byte * 8);
            }
        }
        if ((instruction & 0xffe0001fU) == 0xd4000001U) {
            // Native blocks end before SVC. Release their memory lease
            // before entering personality code, including topology edits.
            const auto handler = supervisor_call_handler_;
            const SupervisorCall call{pc, static_cast<std::uint16_t>(
                (instruction >> 5) & 0xffffU)};
            instruction_memory.unlock_for_dispatch();
            try {
                if (!handler) {
                    throw std::runtime_error("guest supervisor call has no handler");
                }
                const auto exit_code = handler(call, state_, *this);
                if (exit_code) return {*exit_code, count};
                state_.pc = pc + 4;
            } catch (const std::exception& error) {
                state_.pc = pc;
                // Personality code can unload the image owning this trap.
                // Diagnostics must not dereference its former module pointer.
                std::ostringstream message;
                message << error.what() << " during supervisor call #0x"
                        << std::hex << call.immediate << " at guest address 0x"
                        << pc << std::dec << " (run step " << count << ')';
                throw std::runtime_error(message.str());
            } catch (...) {
                state_.pc = pc;
                throw;
            }
            continue;
        }
        try {
            if (jit_enabled_ && jit_cache_ && !known_jit_fallback) {
                std::array<std::uint32_t, 64> candidate{};
                candidate[0] = instruction;
                auto available = std::min<std::uint64_t>(
                    64, (code_end - pc) / 4);
                for (std::size_t index = 1; index < available; ++index) {
                    const auto address = pc + index * 4;
                    if (code_module != nullptr) {
                        candidate[index] = code_module->instruction(address);
                    } else {
                        const auto offset = static_cast<std::size_t>(
                            address - code_region->base);
                        for (unsigned byte = 0; byte < 4; ++byte) {
                            candidate[index] |= static_cast<std::uint32_t>(
                                code_region->read(offset + byte)) << (byte * 8);
                        }
                    }
                    if ((candidate[index] & 0xffe0001fU) == 0xd4000001U) {
                        available = index;
                        break;
                    }
                }
                const auto block_words = std::span(candidate.data(),
                    static_cast<std::size_t>(available));
                const auto native_executed = jit_cache_->execute_native_region(
                    state_, pc, block_words, address_space_->code_generation,
                    static_cast<std::size_t>(maximum_instructions - count + 1),
                    jit_memory);
                if (native_executed != 0) {
                    jit_executed_instructions_ += native_executed;
                    count += native_executed - 1;
                    if (state_.pc == return_sentinel) {
                        return {static_cast<std::uint32_t>(state_.x[0]), count};
                    }
                    continue;
                }
                // A short checked leaf can execute below a region's complete
                // span guard. Cache::execute preserves the canonical native
                // owner instead of replacing it with this budget suffix.
                const auto legacy_words = block_words.first(std::min<std::size_t>(
                    block_words.size(), static_cast<std::size_t>(maximum_instructions - count + 1)));
                const auto decoded = decode_integer_block(pc, legacy_words);
                if (decoded && !decoded->words.empty() &&
                    decoded->words.size() <= maximum_instructions - count + 1 &&
                    jit_cache_->execute(state_, pc, legacy_words,
                                        address_space_->code_generation, jit_memory)) {
                    const auto executed = decoded->words.size();
                    jit_executed_instructions_ += executed;
                    count += executed - 1;
                    if (state_.pc == return_sentinel) {
                        return {static_cast<std::uint32_t>(state_.x[0]), count};
                    }
                    continue;
                }
            }
            if (jit_enabled_ && jit_cache_ && !known_jit_fallback &&
                maximum_instructions - count + 1 >= 2) {
                jit_cache_->remember_fallback(pc, address_space_->code_generation);
            }
            if (jit_enabled_) {
                ++jit_fallback_instructions_;
                if (jit_profiling_) jit_cache_->record_fallback(pc, instruction);
            }
            try {
                if (execute(instruction)) {
                    return {static_cast<std::uint32_t>(state_.x[0]), count};
                }
            } catch (...) {
                // execute advances PC before checked memory access. A fault
                // must remain at that instruction so a repaired mapping can
                // resume without skipping the load or replaying its prefix.
                state_.pc = pc;
                throw;
            }
        } catch (const std::exception& error) {
            std::ostringstream message;
            message << error.what() << " (instruction 0x" << std::hex
                    << std::setw(8) << std::setfill('0') << instruction
                    << " in ";
            if (code_module != nullptr) {
                message << code_module->source() << "+0x"
                        << pc - code_module->base();
            } else {
                message << "guest executable memory at 0x" << pc;
            }
            message << "; run step " << std::dec << count << ')';
            throw std::runtime_error(message.str());
        }
    }
    std::ostringstream message;
    message << "guest exceeded instruction limit at 0x" << std::hex
            << state_.pc;
    AddressSpaceLock module_lock(*address_space_);
    if (const auto* module = module_at(state_.pc)) {
        message << " in " << module->source() << "+0x"
                << state_.pc - module->base();
    }
    throw std::runtime_error(message.str());
}

ReferenceEngine* ReferenceEngine::current() noexcept {
    return active_engine;
}

bool ReferenceEngine::step_external(CpuState& state, std::uint32_t instruction) {
    const CpuState saved = state_;
    state_ = state;
    try {
        const bool exit = execute(instruction);
        state = state_;
        state_ = saved;
        return exit;
    } catch (...) {
        state_ = saved;
        throw;
    }
}

} // namespace transmute
