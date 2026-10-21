#include "transmute/jit.hpp"
#include "jit_simd.hpp"
#include "jit_fp.hpp"
#include "jit_atomic.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace transmute {
namespace {

std::optional<std::uint64_t> decode_logical_immediate(std::uint32_t word,
                                                       bool wide) {
    const auto n = (word >> 22) & 1U;
    const auto immr = (word >> 16) & 63U;
    const auto imms = (word >> 10) & 63U;
    const auto pattern = (n << 6) | (~imms & 63U);
    const auto length = std::bit_width(pattern);
    if ((!wide && n != 0) || length < 2 || length > (wide ? 7 : 6)) {
        return std::nullopt;
    }
    const auto element_width = 1U << (length - 1);
    const auto levels = element_width - 1;
    const auto ones = imms & levels;
    if (ones == levels) return std::nullopt;
    const auto rotation = immr & levels;
    const auto element_mask = element_width == 64 ? UINT64_MAX
                                                   : (1ULL << element_width) - 1;
    const auto element = (1ULL << (ones + 1)) - 1;
    const auto rotated = rotation == 0 ? element
        : ((element >> rotation) |
           (element << (element_width - rotation))) & element_mask;
    std::uint64_t mask = 0;
    for (unsigned bit = 0; bit < (wide ? 64U : 32U); bit += element_width) {
        mask |= rotated << bit;
    }
    return mask;
}

constexpr std::uint64_t low_mask(unsigned bits) {
    return bits == 64 ? UINT64_MAX : (1ULL << bits) - 1;
}

std::optional<BlockOp> integer_memory_operation(unsigned size, unsigned access) {
    if (access >= 2) {
        if (size == 0) return BlockOp::load_signed_byte;
        if (size == 1) return BlockOp::load_signed_halfword;
        if (size == 2 && access == 2) return BlockOp::load_signed_word;
        return std::nullopt; // Reserved encodings and prefetch hints.
    }
    switch (size) {
    case 0: return access == 0 ? BlockOp::store_byte : BlockOp::load_byte;
    case 1: return access == 0 ? BlockOp::store_halfword : BlockOp::load_halfword;
    case 2: return access == 0 ? BlockOp::store_word : BlockOp::load_word;
    case 3: return access == 0 ? BlockOp::store_doubleword : BlockOp::load_doubleword;
    }
    return std::nullopt;
}

std::optional<DecodedInstruction> decode_native_vector_memory(
    std::uint64_t pc, std::uint32_t word) {
    DecodedInstruction decoded{};
    decoded.destination = static_cast<std::uint8_t>(word & 31U);
    decoded.source = static_cast<std::uint8_t>((word >> 5) & 31U);
    const auto size = word >> 30;
    const auto form = word & 0x3f200c00U;
    const auto pair_form = word & 0x3fc00000U;
    if ((word & 0x3f000000U) == 0x1c000000U) {
        if (size == 3) return std::nullopt;
        decoded.op = BlockOp::load_vector;
        decoded.memory_bytes = static_cast<std::uint8_t>(4U << size);
        decoded.memory_literal = true;
        auto offset = static_cast<std::int64_t>((word >> 5) & 0x7ffffU);
        if (offset >= (1LL << 18)) offset -= 1LL << 19;
        decoded.immediate = pc + static_cast<std::uint64_t>(offset * 4);
    } else if (pair_form == 0x2c000000U || pair_form == 0x2c400000U ||
               pair_form == 0x2c800000U || pair_form == 0x2cc00000U ||
               pair_form == 0x2d000000U || pair_form == 0x2d400000U ||
               pair_form == 0x2d800000U || pair_form == 0x2dc00000U) {
        if (size == 3) return std::nullopt;
        decoded.op = (word & 0x00400000U) != 0
            ? BlockOp::load_vector_pair : BlockOp::store_vector_pair;
        decoded.memory_bytes = static_cast<std::uint8_t>(4U << size);
        decoded.second = static_cast<std::uint8_t>((word >> 10) & 31U);
        auto offset = static_cast<std::int64_t>((word >> 15) & 0x7fU);
        if (offset >= 64) offset -= 128;
        decoded.immediate = static_cast<std::uint64_t>(offset * decoded.memory_bytes);
        decoded.memory_writeback = (word & 0x01800000U) == 0x01800000U ||
                                   (word & 0x01800000U) == 0x00800000U;
        decoded.memory_preindex = (word & 0x01800000U) == 0x01800000U;
    } else if ((word & 0x3f000000U) == 0x3d000000U ||
               form == 0x3c000000U || form == 0x3c000400U ||
               form == 0x3c000c00U || form == 0x3c200800U) {
        const bool quad = (word & 0x00800000U) != 0;
        if (quad && size != 0) return std::nullopt;
        decoded.op = (word & 0x00400000U) != 0
            ? BlockOp::load_vector : BlockOp::store_vector;
        const auto scale = quad ? 4U : size;
        decoded.memory_bytes = static_cast<std::uint8_t>(1U << scale);
        if ((word & 0x3f000000U) == 0x3d000000U) {
            decoded.immediate = static_cast<std::uint64_t>((word >> 10) & 0xfffU)
                << scale;
        } else if (form == 0x3c200800U) {
            const auto option = (word >> 13) & 7U;
            if (option != 2 && option != 3 && option != 6 && option != 7)
                return std::nullopt;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.extend_option = static_cast<std::uint8_t>(option);
            decoded.shift = static_cast<std::uint8_t>(scale);
            decoded.memory_register_offset = true;
            decoded.memory_scaled = (word & 0x1000U) != 0;
        } else {
            auto offset = static_cast<std::int64_t>((word >> 12) & 0x1ffU);
            if (offset >= 256) offset -= 512;
            decoded.immediate = static_cast<std::uint64_t>(offset);
            decoded.memory_writeback = form != 0x3c000000U;
            decoded.memory_preindex = form == 0x3c000c00U;
        }
    } else {
        return std::nullopt;
    }
    decoded.wide = decoded.memory_bytes >= 8;
    return decoded;
}

std::optional<DecodedBlock> decode_integer_block_impl(
    std::uint64_t guest_pc, std::span<const std::uint32_t> words,
    DecodedInstruction* single = nullptr) {
    if ((guest_pc & 3U) != 0 || words.empty() || words.size() > 64 ||
        guest_pc > std::numeric_limits<std::uint64_t>::max() - words.size() * 4) {
        return std::nullopt;
    }
    DecodedBlock block;
    block.guest_pc = guest_pc;
    for (const auto word : words) {
        DecodedInstruction decoded{};
        decoded.destination = static_cast<std::uint8_t>(word & 31U);
        decoded.source = static_cast<std::uint8_t>((word >> 5) & 31U);
        decoded.wide = (word & 0x80000000U) != 0;
        if (word == 0xd503201fU || word == 0xd50320ffU ||
            word == 0xd503233fU || word == 0xd503237fU ||
            word == 0xd50323bfU || word == 0xd50323ffU) {
            // Match ReferenceEngine's exact PAuth-disabled HINT policy.
            // Authenticated branches/returns and barriers remain outside it.
            decoded.op = BlockOp::nop;
        } else if (jit_detail::accepts_integer_simd(word)) {
            decoded.op = BlockOp::simd_integer;
        } else if ((word & 0xfffffc1fU) == 0xd65f0000U) {
            decoded.op = BlockOp::ret;
        } else if ((word & 0xfffffc1fU) == 0xd61f0000U) {
            decoded.op = BlockOp::branch_register;
        } else if ((word & 0xfffffc1fU) == 0xd63f0000U) {
            decoded.op = BlockOp::branch_link_register;
        } else if ((word & 0xff201fe0U) == 0x1e201000U) {
            const auto immediate = (word >> 13) & 0xffU;
            const auto sign = immediate >> 7;
            const auto exponent_bit = (immediate >> 6) & 1U;
            decoded.op = BlockOp::float_immediate;
            decoded.wide = (word & 0x00400000U) != 0;
            if (decoded.wide) {
                decoded.immediate = (static_cast<std::uint64_t>(sign) << 63) |
                    (static_cast<std::uint64_t>(exponent_bit ^ 1U) << 62) |
                    (exponent_bit != 0 ? 0x3fc0000000000000ULL : 0ULL) |
                    (static_cast<std::uint64_t>((immediate >> 4) & 3U) << 52) |
                    (static_cast<std::uint64_t>(immediate & 15U) << 48);
            } else {
                decoded.immediate = (sign << 31) |
                    ((exponent_bit ^ 1U) << 30) |
                    (exponent_bit != 0 ? 0x3e000000U : 0U) |
                    (((immediate >> 4) & 3U) << 23) |
                    ((immediate & 15U) << 19);
            }
        } else if (const auto move_form = word & 0xfffffc00U;
                   move_form == 0x1e260000U ||
                   move_form == 0x9e660000U ||
                   move_form == 0x1e270000U ||
                   move_form == 0x9e670000U) {
            decoded.op = (word & 0x00010000U) != 0
                ? BlockOp::integer_to_float_bits : BlockOp::float_to_integer_bits;
            decoded.wide = (word & 0x80000000U) != 0;
        } else if (const auto unary_form = word & 0xff3ffc00U;
                   unary_form == 0x1e204000U ||
                   unary_form == 0x1e214000U ||
                   unary_form == 0x1e20c000U) {
            decoded.op = unary_form == 0x1e204000U ? BlockOp::float_register
                : unary_form == 0x1e214000U ? BlockOp::float_negate_bits
                                               : BlockOp::float_absolute_bits;
            decoded.wide = (word & 0x00400000U) != 0;
        } else if ((word & 0x9f000000U) == 0x10000000U ||
                   (word & 0x9f000000U) == 0x90000000U) {
            const auto encoded = static_cast<std::int64_t>(
                (((word >> 5) & 0x7ffffU) << 2) | ((word >> 29) & 3U));
            const auto displacement = encoded >= (1LL << 20)
                ? encoded - (1LL << 21) : encoded;
            const auto current_pc = guest_pc + block.words.size() * 4;
            const bool page = (word & 0x80000000U) != 0;
            decoded.op = BlockOp::address;
            decoded.immediate = (page ? current_pc & ~0xfffULL : current_pc) +
                static_cast<std::uint64_t>(displacement *
                    (page ? 4096LL : 1LL));
        } else if ((word & 0x3f000000U) == 0x39000000U) {
            // Bit 26 excludes SIMD/FP accesses, whose size/opc fields differ.
            // Keep the memory access first so the C++ memory contract can run
            // before emitted code changes any architectural register.
            if (!block.words.empty()) break;
            const auto size = (word >> 30) & 3U;
            const auto access = (word >> 22) & 3U;
            const auto operation = integer_memory_operation(size, access);
            if (!operation) return std::nullopt;
            decoded.op = *operation;
            decoded.wide = access == 2 || size == 3;
            decoded.immediate = static_cast<std::uint64_t>(
                (word >> 10) & 0xfffU) << size;
        } else if ((word & 0x3f200c00U) == 0x38000000U ||
                   (word & 0x3f200c00U) == 0x38000400U ||
                   (word & 0x3f200c00U) == 0x38000c00U) {
            if (!block.words.empty()) break;
            const auto size = (word >> 30) & 3U;
            const auto access = (word >> 22) & 3U;
            const auto operation = integer_memory_operation(size, access);
            if (!operation) return std::nullopt;
            decoded.op = *operation;
            decoded.wide = access == 2 || size == 3;
            const auto imm9 = static_cast<std::int64_t>((word >> 12) & 0x1ffU);
            decoded.immediate = static_cast<std::uint64_t>(
                imm9 >= 256 ? imm9 - 512 : imm9);
            decoded.memory_writeback = (word & 0x00000400U) != 0;
            decoded.memory_preindex = (word & 0x00000800U) != 0;
            // Newly lowered writeback overlaps are constrained unpredictable.
            // Keep them in the reference engine; retain its existing LDRB/STRB
            // convention for the byte forms already supported by this backend.
            if (decoded.memory_writeback && decoded.source != 31 &&
                decoded.destination == decoded.source &&
                !(size == 0 && access <= 1)) return std::nullopt;
        } else if ((word & 0x3f200c00U) == 0x38200800U) {
            if (!block.words.empty()) break;
            const auto size = (word >> 30) & 3U;
            const auto access = (word >> 22) & 3U;
            const auto option = (word >> 13) & 7U;
            const auto operation = integer_memory_operation(size, access);
            if (!operation ||
                (option != 2 && option != 3 && option != 6 && option != 7)) {
                return std::nullopt;
            }
            decoded.op = *operation;
            decoded.wide = access == 2 || size == 3;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.extend_option = static_cast<std::uint8_t>(option);
            decoded.shift = static_cast<std::uint8_t>(size);
            decoded.memory_register_offset = true;
            decoded.memory_scaled = (word & 0x1000U) != 0;
        } else if (const auto pair_form = word & 0xffc00000U;
                   pair_form == 0x28000000U || pair_form == 0x28400000U ||
                   pair_form == 0x28800000U || pair_form == 0x28c00000U ||
                   pair_form == 0x29000000U || pair_form == 0x29400000U ||
                   pair_form == 0x29800000U || pair_form == 0x29c00000U ||
                   pair_form == 0xa8000000U || pair_form == 0xa8400000U ||
                   pair_form == 0xa8800000U || pair_form == 0xa8c00000U ||
                   pair_form == 0xa9000000U || pair_form == 0xa9400000U ||
                   pair_form == 0xa9800000U || pair_form == 0xa9c00000U) {
            if (!block.words.empty()) break;
            const auto imm7 = static_cast<std::int64_t>((word >> 15) & 0x7fU);
            const auto displacement = (imm7 >= 64 ? imm7 - 128 : imm7) *
                (decoded.wide ? 8LL : 4LL);
            decoded.op = (word & 0x00400000U) != 0
                ? BlockOp::load_pair : BlockOp::store_pair;
            decoded.second = static_cast<std::uint8_t>((word >> 10) & 31U);
            decoded.immediate = static_cast<std::uint64_t>(displacement);
            decoded.memory_writeback = (word & 0x01800000U) == 0x01800000U ||
                                       (word & 0x01800000U) == 0x00800000U;
            decoded.memory_preindex =
                (word & 0x01800000U) == 0x01800000U;
        } else if (const auto form = word & 0x7f800000U;
                   form == 0x12800000U || form == 0x52800000U ||
                   form == 0x72800000U) {
            const auto shift = ((word >> 21) & 3U) * 16U;
            if (!decoded.wide && shift > 16) {
                if (block.words.empty()) return std::nullopt;
                break;
            }
            decoded.shift = static_cast<std::uint8_t>(shift);
            decoded.op = form == 0x12800000U ? BlockOp::move_inverse
                : form == 0x52800000U ? BlockOp::move_zero : BlockOp::move_keep;
            decoded.immediate = static_cast<std::uint64_t>(
                (word >> 5) & 0xffffU) << shift;
            if (decoded.op == BlockOp::move_inverse) {
                decoded.immediate = ~decoded.immediate &
                    (decoded.wide ? UINT64_MAX : UINT32_MAX);
            }
        } else if ((word & 0x1f800000U) == 0x11000000U) {
            decoded.op = (word & 0x40000000U) != 0
                ? BlockOp::subtract_immediate : BlockOp::add_immediate;
            decoded.set_flags = (word & 0x20000000U) != 0;
            decoded.immediate = static_cast<std::uint64_t>(
                (word >> 10) & 0xfffU) << (((word >> 22) & 1U) ? 12U : 0U);
        } else if ((word & 0x1f200000U) == 0x0b000000U) {
            const auto shift_type = (word >> 22) & 3U;
            const auto shift = (word >> 10) & 63U;
            if (shift_type == 3 || (!decoded.wide && shift >= 32)) {
                if (block.words.empty()) return std::nullopt;
                break;
            }
            decoded.op = (word & 0x40000000U) != 0
                ? BlockOp::subtract_register : BlockOp::add_register;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.shift_type = static_cast<std::uint8_t>(shift_type);
            decoded.shift = static_cast<std::uint8_t>(shift);
            decoded.set_flags = (word & 0x20000000U) != 0;
        } else if ((word & 0x7fe0f000U) == 0x1ac02000U) {
            decoded.op = BlockOp::shift_register;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.shift_type = static_cast<std::uint8_t>((word >> 10) & 3U);
        } else if (const auto divide_form = word & 0x7fe0fc00U;
                   divide_form == 0x1ac00800U || divide_form == 0x1ac00c00U) {
            decoded.op = divide_form == 0x1ac00800U
                ? BlockOp::divide_unsigned : BlockOp::divide_signed;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
        } else if ((word & 0x7ffffc00U) == 0x5ac01000U) {
            decoded.op = BlockOp::count_leading_zero;
        } else if ((word & 0x1fe00000U) == 0x0b200000U) {
            const auto shift = (word >> 10) & 7U;
            if (shift > 4) {
                if (block.words.empty()) return std::nullopt;
                break;
            }
            decoded.op = (word & 0x40000000U) != 0
                ? BlockOp::subtract_extended : BlockOp::add_extended;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.extend_option = static_cast<std::uint8_t>((word >> 13) & 7U);
            decoded.shift = static_cast<std::uint8_t>(shift);
            decoded.set_flags = (word & 0x20000000U) != 0;
        } else if (const auto reverse_form = word & 0xfffffc00U;
                   reverse_form == 0x5ac00400U || reverse_form == 0xdac00400U ||
                   reverse_form == 0x5ac00800U || reverse_form == 0xdac00800U ||
                   reverse_form == 0xdac00c00U) {
            decoded.op = BlockOp::reverse_bytes;
            decoded.field_width = static_cast<std::uint8_t>(
                reverse_form == 0x5ac00400U || reverse_form == 0xdac00400U ? 2U
                : reverse_form == 0xdac00c00U ? 8U : 4U);
        } else if ((word & 0x7fa00000U) == 0x13800000U) {
            const auto shift = (word >> 10) & 63U;
            const bool n = (word & 0x00400000U) != 0;
            if (n != decoded.wide || (!decoded.wide && shift >= 32)) {
                if (block.words.empty()) return std::nullopt;
                break;
            }
            decoded.op = BlockOp::extract_registers;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.shift = static_cast<std::uint8_t>(shift);
        } else if ((word & 0x1f000000U) == 0x0a000000U) {
            const auto shift = (word >> 10) & 63U;
            if (!decoded.wide && shift >= 32) {
                if (block.words.empty()) return std::nullopt;
                break;
            }
            decoded.op = BlockOp::logical_register;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.logical_operation = static_cast<std::uint8_t>((word >> 29) & 3U);
            decoded.set_flags = decoded.logical_operation == 3;
            decoded.invert_second = (word & 0x00200000U) != 0;
            decoded.shift = static_cast<std::uint8_t>(shift);
            decoded.shift_type = static_cast<std::uint8_t>((word >> 22) & 3U);
        } else if ((word & 0x1fe00800U) == 0x1a800000U) {
            decoded.op = BlockOp::conditional_select;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.condition = static_cast<std::uint8_t>((word >> 12) & 15U);
            decoded.invert_second = (word & 0x40000000U) != 0;
            decoded.increment_second = (word & 0x00000400U) != 0;
        } else if ((word & 0x1f800000U) == 0x12000000U) {
            const auto mask = decode_logical_immediate(word, decoded.wide);
            if (!mask) {
                if (block.words.empty()) return std::nullopt;
                break;
            }
            decoded.op = BlockOp::logical_immediate;
            decoded.logical_operation = static_cast<std::uint8_t>((word >> 29) & 3U);
            decoded.set_flags = decoded.logical_operation == 3;
            decoded.immediate = *mask;
        } else if (const auto bitfield_form = word & 0x7f800000U;
                   bitfield_form == 0x13000000U || bitfield_form == 0x33000000U ||
                   bitfield_form == 0x53000000U) {
            const auto width = decoded.wide ? 64U : 32U;
            const auto immr = (word >> 16) & 63U;
            const auto imms = (word >> 10) & 63U;
            const bool n = (word & 0x00400000U) != 0;
            if (n != decoded.wide || immr >= width || imms >= width) {
                if (block.words.empty()) return std::nullopt;
                break;
            }
            decoded.op = bitfield_form == 0x13000000U ? BlockOp::signed_bitfield
                : bitfield_form == 0x33000000U ? BlockOp::merge_bitfield
                                       : BlockOp::unsigned_bitfield;
            decoded.bitfield_left = imms < immr;
            decoded.shift = static_cast<std::uint8_t>(
                decoded.bitfield_left ? width - immr : immr);
            decoded.field_width = static_cast<std::uint8_t>(decoded.bitfield_left
                ? imms + 1 : imms - immr + 1);
            decoded.immediate = low_mask(decoded.field_width);
        } else if (const auto compare_form = word & 0x3fe00c10U;
                   compare_form == 0x3a400000U || compare_form == 0x3a400800U) {
            // S, reserved opcode bits and bit 4 are fixed; Rn/Rm name ZR.
            decoded.op = (word & 0x40000000U) != 0
                ? BlockOp::conditional_compare_subtract : BlockOp::conditional_compare_add;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.condition = static_cast<std::uint8_t>((word >> 12) & 15U);
            decoded.compare_immediate = compare_form == 0x3a400800U;
            decoded.immediate = static_cast<std::uint64_t>(word & 15U) << 28;
        } else if (const auto multiply_form = word & 0x7fe08000U;
                   multiply_form == 0x1b000000U || multiply_form == 0x1b008000U) {
            decoded.op = multiply_form == 0x1b000000U
                ? BlockOp::multiply_add : BlockOp::multiply_subtract;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.accumulator = static_cast<std::uint8_t>((word >> 10) & 31U);
        } else if (const auto long_multiply_form = word & 0xffe08000U;
                   long_multiply_form == 0x9b200000U || long_multiply_form == 0x9b208000U ||
                   long_multiply_form == 0x9ba00000U || long_multiply_form == 0x9ba08000U) {
            decoded.op = (word & 0x8000U) != 0
                ? BlockOp::widening_multiply_subtract : BlockOp::widening_multiply_add;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
            decoded.accumulator = static_cast<std::uint8_t>((word >> 10) & 31U);
            decoded.unsigned_operands = (word & 0x00800000U) != 0;
        } else if (const auto high_multiply_form = word & 0xffe0fc00U;
                   high_multiply_form == 0x9b407c00U || high_multiply_form == 0x9bc07c00U) {
            decoded.op = high_multiply_form == 0x9b407c00U
                ? BlockOp::multiply_high_signed : BlockOp::multiply_high_unsigned;
            decoded.second = static_cast<std::uint8_t>((word >> 16) & 31U);
        } else if ((word & 0x7e000000U) == 0x34000000U) {
            decoded.op = (word & 0x01000000U) != 0
                ? BlockOp::compare_branch_nonzero : BlockOp::compare_branch_zero;
            decoded.source = static_cast<std::uint8_t>(word & 31U);
            auto displacement = static_cast<std::int64_t>((word >> 5) & 0x7ffffU);
            if (displacement >= (1LL << 18)) displacement -= 1LL << 19;
            decoded.immediate = guest_pc + block.words.size() * 4 +
                static_cast<std::uint64_t>(displacement * 4);
        } else if ((word & 0x7e000000U) == 0x36000000U) {
            decoded.op = (word & 0x01000000U) != 0
                ? BlockOp::test_branch_nonzero : BlockOp::test_branch_zero;
            decoded.source = static_cast<std::uint8_t>(word & 31U);
            decoded.shift = static_cast<std::uint8_t>(
                ((word >> 19) & 31U) | (((word >> 31) & 1U) << 5));
            auto displacement = static_cast<std::int64_t>((word >> 5) & 0x3fffU);
            if (displacement >= (1LL << 13)) displacement -= 1LL << 14;
            decoded.immediate = guest_pc + block.words.size() * 4 +
                static_cast<std::uint64_t>(displacement * 4);
        } else if ((word & 0x7c000000U) == 0x14000000U) {
            decoded.op = (word & 0x80000000U) != 0
                ? BlockOp::branch_link : BlockOp::branch;
            auto displacement = static_cast<std::int64_t>(word & 0x03ffffffU);
            if (displacement >= (1LL << 25)) displacement -= 1LL << 26;
            decoded.immediate = guest_pc + block.words.size() * 4 +
                static_cast<std::uint64_t>(displacement * 4);
        } else if ((word & 0xff000010U) == 0x54000000U) {
            decoded.op = BlockOp::conditional_branch;
            decoded.condition = static_cast<std::uint8_t>(word & 15U);
            auto displacement = static_cast<std::int64_t>((word >> 5) & 0x7ffffU);
            if (displacement >= (1LL << 18)) displacement -= 1LL << 19;
            decoded.immediate = guest_pc + block.words.size() * 4 +
                static_cast<std::uint64_t>(displacement * 4);
        } else {
            if (block.words.empty()) return std::nullopt;
            break;
        }
        if (single != nullptr) {
            *single = decoded;
            return block; // No per-word vectors/allocations in region decode.
        }
        block.words.push_back(word);
        block.instructions.push_back(decoded);
        if (decoded.op == BlockOp::ret ||
            decoded.op == BlockOp::store_byte ||
            decoded.op == BlockOp::store_halfword ||
            decoded.op == BlockOp::store_word ||
            decoded.op == BlockOp::store_doubleword ||
            decoded.op == BlockOp::store_pair ||
            decoded.op == BlockOp::branch_register ||
            decoded.op == BlockOp::branch_link_register ||
            decoded.op == BlockOp::compare_branch_zero ||
            decoded.op == BlockOp::compare_branch_nonzero ||
            decoded.op == BlockOp::test_branch_zero ||
            decoded.op == BlockOp::test_branch_nonzero ||
            decoded.op == BlockOp::branch ||
            decoded.op == BlockOp::branch_link ||
            decoded.op == BlockOp::conditional_branch) break;
    }
    return block;
}

} // namespace

std::optional<DecodedBlock> decode_integer_block(
    std::uint64_t guest_pc, std::span<const std::uint32_t> words) {
    return decode_integer_block_impl(guest_pc, words);
}

std::optional<DecodedBlock> decode_native_read_region(
    std::uint64_t guest_pc, std::span<const std::uint32_t> words) {
    if ((guest_pc & 3U) != 0 || words.empty() || words.size() > 64 ||
        guest_pc > std::numeric_limits<std::uint64_t>::max() - words.size() * 4)
        return std::nullopt;
    DecodedBlock region;
    region.guest_pc = guest_pc;
    region.words.reserve(words.size());
    region.instructions.reserve(words.size());
    for (std::size_t index = 0; index < words.size(); ++index) {
        DecodedInstruction instruction{};
        if (const auto vector = decode_native_vector_memory(guest_pc + index * 4,
                                                           words[index])) {
            instruction = *vector;
        } else if (jit_detail::accepts_guarded_fp(words[index])) {
            instruction.op = BlockOp::guarded_fp;
        } else if (const auto atomic = jit_detail::decode_native_lse(words[index])) {
            instruction.op = BlockOp::atomic_lse;
            instruction.source = static_cast<std::uint8_t>(atomic->base);
            instruction.memory_bytes = static_cast<std::uint8_t>(atomic->bytes);
        } else {
            const auto decoded = decode_integer_block_impl(guest_pc + index * 4,
                words.subspan(index, 1), &instruction);
            if (!decoded) break;
        }
        region.words.push_back(words[index]);
        region.instructions.push_back(instruction);
        if (instruction.op == BlockOp::ret ||
            instruction.op == BlockOp::branch_register ||
            instruction.op == BlockOp::branch_link_register) break;
        if ((instruction.op == BlockOp::branch ||
             instruction.op == BlockOp::branch_link) &&
            (instruction.immediate < guest_pc ||
             instruction.immediate - guest_pc >= words.size() * 4)) break;
    }
    // Pure register/control regions use the same counted/chaining ABI, so
    // hot native memory/FP traces no longer return through C++ at every pure
    // block boundary. No private lease means the legacy decoder still applies.
    if (region.instructions.empty()) return std::nullopt;
    return region;
}

#if !defined(_WIN32) || !(defined(_M_X64) || defined(__x86_64__))
struct X64BlockCache::Impl {};
X64BlockCache::X64BlockCache() : impl_(std::make_unique<Impl>()) {}
X64BlockCache::~X64BlockCache() = default;
std::size_t X64BlockCache::inherit_immutable(X64BlockCache&, std::uint64_t,
    std::span<PeImage* const>) {
    return 0;
}
bool X64BlockCache::execute(CpuState&, std::uint64_t,
                            std::span<const std::uint32_t>, std::uint64_t,
                            GuestMemory*) {
    return false;
}
std::size_t X64BlockCache::execute_native_region(CpuState&, std::uint64_t,
    std::span<const std::uint32_t>, std::uint64_t, std::size_t, GuestMemory*,
    bool* requires_exclusive) {
    if (requires_exclusive != nullptr) *requires_exclusive = false;
    return 0;
}
std::size_t X64BlockCache::execute_cached(CpuState&, std::uint64_t,
                                          std::uint64_t, std::size_t, bool*,
                                          GuestMemory*, bool* requires_exclusive) {
    if (requires_exclusive != nullptr) *requires_exclusive = false;
    return 0;
}
bool X64BlockCache::has_read_only_native_region(std::uint64_t,
                                               std::uint64_t) const noexcept {
    return false;
}
bool X64BlockCache::has_read_only_cached_block(std::uint64_t,
                                              std::uint64_t) const noexcept {
    return false;
}
bool X64BlockCache::has_parallel_cached_block(std::uint64_t,
                                             std::uint64_t) const noexcept {
    return false;
}
void X64BlockCache::remember_fallback(std::uint64_t, std::uint64_t) {}
void X64BlockCache::record_fallback(std::uint64_t, std::uint32_t) {}
void X64BlockCache::invalidate_range(std::uint64_t, std::uint64_t) {}
void X64BlockCache::invalidate_all() {}
std::size_t X64BlockCache::cache_size() const { return 0; }
const void* X64BlockCache::last_code_address() const noexcept { return nullptr; }
void X64BlockCache::set_profiling(bool) {}
JitStats X64BlockCache::stats(std::size_t) const { return {}; }
#endif

} // namespace transmute
