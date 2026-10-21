#include "jit_bit_moves.hpp"

#include "transmute/a64.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace transmute::jit_detail {
namespace {

enum class Kind {
    duplicate_gp, duplicate_element, scalar_element, constant,
    immediate_or, immediate_clear, scalar_move, scalar_abs, scalar_neg,
    gp_to_fp, fp_to_gp, select
};

struct Instruction {
    Kind kind;
    unsigned rd{}, rn{}, rm{}, bytes{}, offset{}, condition{};
    bool quad{};
    std::array<std::uint64_t, 2> constant{};
};

std::uint64_t expand_float(unsigned immediate, bool wide) noexcept {
    const auto b = (immediate >> 6) & 1U;
    if (wide) {
        return (static_cast<std::uint64_t>(immediate >> 7) << 63) |
            (static_cast<std::uint64_t>(b ^ 1U) << 62) |
            (b ? 0x3fc0000000000000ULL : 0) |
            (static_cast<std::uint64_t>((immediate >> 4) & 3U) << 52) |
            (static_cast<std::uint64_t>(immediate & 15U) << 48);
    }
    return (static_cast<std::uint64_t>(immediate >> 7) << 31) |
        (static_cast<std::uint64_t>(b ^ 1U) << 30) |
        (b ? 0x3e000000ULL : 0) |
        (static_cast<std::uint64_t>((immediate >> 4) & 3U) << 23) |
        (static_cast<std::uint64_t>(immediate & 15U) << 19);
}

std::optional<Instruction> decode(std::uint32_t word) noexcept {
    Instruction op{Kind::constant};
    op.rd = word & 31U;
    op.rn = (word >> 5) & 31U;
    op.rm = (word >> 16) & 31U;
    op.quad = (word & 0x40000000U) != 0;
    if ((word & 0xbfe0fc00U) == 0x0e000c00U) {
        op.kind = Kind::duplicate_gp;
        op.bytes = (word >> 16) & 31U;
        if (!std::has_single_bit(op.bytes) || op.bytes > 8 ||
            (op.bytes == 8 && !op.quad)) return std::nullopt;
        return op;
    }
    const bool scalar_element = (word & 0xffe0fc00U) == 0x5e000400U;
    if (scalar_element || (word & 0xbfe0fc00U) == 0x0e000400U) {
        const auto imm5 = (word >> 16) & 31U;
        if (!imm5) return std::nullopt;
        const auto size = std::countr_zero(imm5);
        op.bytes = 1U << size;
        op.offset = (imm5 >> (size + 1)) * op.bytes;
        if (op.bytes > 8 || op.offset + op.bytes > 16 ||
            (!scalar_element && op.bytes == 8 && !op.quad)) return std::nullopt;
        op.kind = scalar_element ? Kind::scalar_element : Kind::duplicate_element;
        return op;
    }
    if ((word & 0x9ff80c00U) == 0x0f000400U) {
        const auto immediate = (((word >> 16) & 7U) << 5) | ((word >> 5) & 31U);
        const auto cmode = (word >> 12) & 15U;
        const bool invert = (word & 0x20000000U) != 0;
        std::uint64_t value = 0;
        const bool modifies = cmode < 12 && (cmode & 1U) != 0;
        if (cmode < 8 || cmode == 12 || cmode == 13) {
            const auto shift = cmode < 8 ? (cmode / 2) * 8 : (cmode == 12 ? 8U : 16U);
            auto lane = static_cast<std::uint32_t>(immediate << shift);
            if (cmode >= 12) lane |= (1U << shift) - 1;
            if (invert && !modifies) lane = ~lane;
            value = lane | (static_cast<std::uint64_t>(lane) << 32);
        } else if (cmode < 12) {
            auto lane = static_cast<std::uint16_t>(immediate << (((cmode - 8) / 2) * 8));
            if (invert && !modifies) lane = static_cast<std::uint16_t>(~lane);
            value = lane * 0x0001000100010001ULL;
        } else if (cmode == 14) {
            if (invert) {
                for (unsigned bit = 0; bit < 8; ++bit)
                    if (immediate & (1U << bit)) value |= 0xffULL << (bit * 8);
            } else value = immediate * 0x0101010101010101ULL;
        } else {
            if (invert && !op.quad) return std::nullopt;
            value = expand_float(immediate, invert);
            if (!invert) value |= value << 32;
        }
        op.kind = modifies ? (invert ? Kind::immediate_clear : Kind::immediate_or)
                           : Kind::constant;
        op.constant = {value, op.quad ? value : 0};
        return op;
    }
    const auto type = (word >> 22) & 3U;
    if ((word & 0xff201fe0U) == 0x1e201000U && type <= 1) {
        op.constant = {expand_float((word >> 13) & 255U, type == 1), 0};
        return op;
    }
    switch (word & 0xfffffc00U) {
    case 0x1e260000U: op.kind = Kind::fp_to_gp; op.bytes = 4; return op;
    case 0x9e660000U: op.kind = Kind::fp_to_gp; op.bytes = 8; return op;
    case 0x1e270000U: op.kind = Kind::gp_to_fp; op.bytes = 4; return op;
    case 0x9e670000U: op.kind = Kind::gp_to_fp; op.bytes = 8; return op;
    default: break;
    }
    if (type > 1) return std::nullopt;
    op.bytes = type == 1 ? 8 : 4;
    switch (word & 0xffbffc00U) {
    case 0x1e204000U: op.kind = Kind::scalar_move; return op;
    case 0x1e20c000U: op.kind = Kind::scalar_abs; return op;
    case 0x1e214000U: op.kind = Kind::scalar_neg; return op;
    default: break;
    }
    if ((word & 0xff200c00U) == 0x1e200c00U) {
        op.kind = Kind::select;
        op.condition = (word >> 12) & 15U;
        return op;
    }
    return std::nullopt;
}

struct Fragment {
    std::vector<std::uint8_t> bytes;
    void byte(std::uint8_t v) { bytes.push_back(v); }
    void word(std::uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(v >> (i * 8)));
    }
    void packed(std::uint8_t opcode, unsigned dst, unsigned src) {
        byte(0x66); byte(0x0f); byte(opcode);
        byte(static_cast<std::uint8_t>(0xc0U | (dst << 3) | src));
    }
    void load(unsigned xmm, std::size_t offset, unsigned width = 16) {
        byte(width == 4 ? 0x66 : 0xf3); byte(0x0f);
        byte(width == 4 ? 0x6e : width == 8 ? 0x7e : 0x6f);
        byte(static_cast<std::uint8_t>(0x81U | (xmm << 3)));
        word(static_cast<std::uint32_t>(offset));
    }
    void store(unsigned xmm, std::size_t offset, bool gp = false) {
        byte(gp ? 0x66 : 0xf3); byte(0x0f); byte(gp ? 0xd6 : 0x7f);
        byte(static_cast<std::uint8_t>(0x81U | (xmm << 3)));
        word(static_cast<std::uint32_t>(offset));
    }
    void literal(unsigned xmm, std::array<std::uint64_t, 2> value) {
        if (value[0] == 0 && value[1] == 0) { packed(0xef, xmm, xmm); return; }
        byte(0xf3); byte(0x0f); byte(0x6f);
        byte(static_cast<std::uint8_t>(0x05U | (xmm << 3))); word(5);
        byte(0xe9); word(16); // Skip inline data; no GPR or FP-state traffic.
        for (auto half : value)
            for (unsigned i = 0; i < 8; ++i) byte(static_cast<std::uint8_t>(half >> (i * 8)));
    }
    void shift_bytes(unsigned xmm, unsigned amount, bool left = false) {
        byte(0x66); byte(0x0f); byte(0x73);
        byte(static_cast<std::uint8_t>((left ? 0xf8U : 0xd8U) | xmm));
        byte(static_cast<std::uint8_t>(amount));
    }
    void shift_dword(unsigned xmm, unsigned amount, bool arithmetic = false) {
        byte(0x66); byte(0x0f); byte(0x72);
        byte(static_cast<std::uint8_t>((arithmetic ? 0xe0U : 0xd0U) | xmm));
        byte(static_cast<std::uint8_t>(amount));
    }
    void broadcast(unsigned xmm, unsigned width) {
        if (width == 1) packed(0x60, xmm, xmm);
        if (width <= 2) packed(0x61, xmm, xmm);
        if (width <= 4) {
            byte(0x66); byte(0x0f); byte(0x70);
            byte(static_cast<std::uint8_t>(0xc0U | (xmm << 3) | xmm)); byte(0);
        } else packed(0x6c, xmm, xmm);
    }
    void clear_upper(unsigned xmm) {
        byte(0xf3); byte(0x0f); byte(0x7e);
        byte(static_cast<std::uint8_t>(0xc0U | (xmm << 3) | xmm));
    }
    void condition_mask(unsigned condition) {
        load(2, offsetof(CpuState, nzcv), 4);
        const auto base = condition >> 1;
        bool invert = (condition & 1U) != 0;
        if (base < 4) {
            constexpr std::array shifts{30U, 29U, 31U, 28U};
            shift_dword(2, shifts[base]);
        } else if (base == 4) {
            packed(0x6f, 3, 2); shift_dword(2, 29); shift_dword(3, 30);
            // Only bit zero matters; ~Z & C is the HI condition.
            packed(0xdf, 3, 2); packed(0x6f, 2, 3);
        } else {
            packed(0x6f, 3, 2); shift_dword(2, 31); shift_dword(3, 28);
            packed(0xef, 2, 3); // N xor V in bit zero.
            if (base == 6) {
                load(3, offsetof(CpuState, nzcv), 4); shift_dword(3, 30);
                packed(0xeb, 2, 3); // (N xor V) or Z: inverse of GT.
            }
            invert = !invert;
        }
        // Broadcast the condition's low bit to an all-zero/all-one mask.
        byte(0x66); byte(0x0f); byte(0x72); byte(0xf2); byte(31); // pslld xmm2,31
        shift_dword(2, 31, true);
        byte(0x66); byte(0x0f); byte(0x70); byte(0xd2); byte(0);
        if (invert) { packed(0x76, 3, 3); packed(0xef, 2, 3); }
    }
};

constexpr std::size_t vector_offset(unsigned reg) { return offsetof(CpuState, v) + reg * 16; }
constexpr std::size_t gp_offset(unsigned reg) { return offsetof(CpuState, x) + reg * 8; }

} // namespace

bool accepts_bit_moves(std::uint32_t instruction) noexcept { return decode(instruction).has_value(); }

std::vector<std::uint8_t> emit_bit_moves(std::uint32_t instruction) {
    const auto decoded = decode(instruction);
    if (!decoded) throw std::invalid_argument("unsupported native bit-movement encoding");
    const auto& op = *decoded;
    Fragment out;
    switch (op.kind) {
    case Kind::constant: out.literal(0, op.constant); break;
    case Kind::immediate_or:
    case Kind::immediate_clear:
        out.load(0, vector_offset(op.rd)); out.literal(1, op.constant);
        if (op.kind == Kind::immediate_or) out.packed(0xeb, 0, 1);
        else { out.packed(0xdf, 1, 0); out.packed(0x6f, 0, 1); }
        if (!op.quad) out.clear_upper(0);
        break;
    case Kind::duplicate_gp:
    case Kind::gp_to_fp:
        if (op.rn == 31) out.packed(0xef, 0, 0);
        else out.load(0, gp_offset(op.rn), op.bytes == 8 ? 8 : 4);
        if (op.kind == Kind::duplicate_gp) {
            out.broadcast(0, op.bytes);
            if (!op.quad) out.clear_upper(0);
        }
        break;
    case Kind::duplicate_element:
    case Kind::scalar_element:
        out.load(0, vector_offset(op.rn));
        if (op.offset) out.shift_bytes(0, op.offset);
        if (op.kind == Kind::duplicate_element) {
            out.broadcast(0, op.bytes);
            if (!op.quad) out.clear_upper(0);
        } else if (op.bytes == 8) out.clear_upper(0);
        else { out.shift_bytes(0, 16 - op.bytes, true); out.shift_bytes(0, 16 - op.bytes); }
        break;
    case Kind::fp_to_gp:
        if (op.rd != 31) { out.load(0, vector_offset(op.rn), op.bytes); out.store(0, gp_offset(op.rd), true); }
        return out.bytes;
    case Kind::scalar_move:
    case Kind::scalar_abs:
    case Kind::scalar_neg:
        out.load(0, vector_offset(op.rn), op.bytes);
        if (op.kind != Kind::scalar_move) {
            const auto sign = 1ULL << (op.bytes * 8 - 1);
            out.literal(1, {op.kind == Kind::scalar_abs ? ~sign : sign,
                            op.kind == Kind::scalar_abs ? UINT64_MAX : 0});
            out.packed(op.kind == Kind::scalar_abs ? 0xdb : 0xef, 0, 1);
        }
        break;
    case Kind::select:
        out.load(0, vector_offset(op.rn), op.bytes);
        if (op.condition < 14) {
            out.load(1, vector_offset(op.rm), op.bytes);
            out.condition_mask(op.condition);
            out.packed(0xdb, 0, 2); out.packed(0xdf, 2, 1); out.packed(0xeb, 0, 2);
        }
        break;
    }
    out.store(0, vector_offset(op.rd));
    return out.bytes;
}

} // namespace transmute::jit_detail
