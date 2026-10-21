#include "jit_simd.hpp"
#include "jit_bit_moves.hpp"

#include "transmute/a64.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace transmute::jit_detail {
namespace {

enum class Operation : std::uint8_t {
    bit_and, bit_clear, bit_or, bit_or_not, bit_xor,
    bit_select, bit_insert, bit_insert_false, bit_not,
    add, subtract, negate, equal, equal_zero, bit_test,
};

struct Instruction {
    Operation operation;
    unsigned destination;
    unsigned source;
    unsigned second;
    unsigned size;
    bool quad;
};

std::optional<Instruction> decode(std::uint32_t word) noexcept {
    std::optional<Operation> operation;
    const auto logic = word & 0xbfe0fc00U;
    switch (logic) {
    case 0x0e201c00U: operation = Operation::bit_and; break;
    case 0x0e601c00U: operation = Operation::bit_clear; break;
    case 0x0ea01c00U: operation = Operation::bit_or; break;
    case 0x0ee01c00U: operation = Operation::bit_or_not; break;
    case 0x2e201c00U: operation = Operation::bit_xor; break;
    case 0x2e601c00U: operation = Operation::bit_select; break;
    case 0x2ea01c00U: operation = Operation::bit_insert; break;
    case 0x2ee01c00U: operation = Operation::bit_insert_false; break;
    default: break;
    }
    if ((word & 0xbffffc00U) == 0x2e205800U) {
        operation = Operation::bit_not;
    }
    const bool quad = (word & 0x40000000U) != 0;
    const auto size = (word >> 22) & 3U;
    if (!operation) {
        switch (word & 0xbf20fc00U) {
        case 0x0e208400U: operation = Operation::add; break;
        case 0x2e208400U: operation = Operation::subtract; break;
        case 0x2e208c00U: operation = Operation::equal; break;
        case 0x0e208c00U: operation = Operation::bit_test; break;
        default: break;
        }
        switch (word & 0xbf3ffc00U) {
        case 0x2e20b800U: operation = Operation::negate; break;
        case 0x0e209800U: operation = Operation::equal_zero; break;
        default: break;
        }
        // A 64-bit vector with a single D element is reserved for these
        // vector forms. Scalar D instructions have separate encodings.
        if (size == 3 && !quad) return std::nullopt;
    }
    if (!operation) return std::nullopt;
    return Instruction{*operation, word & 31U, (word >> 5) & 31U,
                       (word >> 16) & 31U, size, quad};
}

struct Fragment {
    std::vector<std::uint8_t> bytes;

    void byte(std::uint8_t value) { bytes.push_back(value); }
    void word(std::uint32_t value) {
        for (unsigned i = 0; i != 4; ++i) {
            byte(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }
    void load(unsigned xmm, unsigned guest) {
        byte(0xf3); byte(0x0f); byte(0x6f); // movdqu xmm, [rcx+disp32]
        byte(static_cast<std::uint8_t>(0x81U | (xmm << 3)));
        word(static_cast<std::uint32_t>(offsetof(CpuState, v) + guest * 16));
    }
    void store(unsigned xmm, unsigned guest) {
        byte(0xf3); byte(0x0f); byte(0x7f); // movdqu [rcx+disp32], xmm
        byte(static_cast<std::uint8_t>(0x81U | (xmm << 3)));
        word(static_cast<std::uint32_t>(offsetof(CpuState, v) + guest * 16));
    }
    void packed(std::uint8_t opcode, unsigned destination, unsigned source) {
        byte(0x66); byte(0x0f); byte(opcode);
        byte(static_cast<std::uint8_t>(0xc0U | (destination << 3) | source));
    }
    void copy(unsigned destination, unsigned source) {
        packed(0x6f, destination, source); // movdqa register, register
    }
    void zero(unsigned xmm) { packed(0xef, xmm, xmm); }
    void invert(unsigned xmm, unsigned scratch) {
        packed(0x76, scratch, scratch); // pcmpeqd creates all ones
        packed(0xef, xmm, scratch);
    }
    void equal(unsigned destination, unsigned source, unsigned size) {
        constexpr std::array<std::uint8_t, 3> opcodes{0x74, 0x75, 0x76};
        packed(opcodes[size == 3 ? 2 : size], destination, source);
        if (size == 3) {
            // SSE2 has no PCMPEQQ. Pair adjacent dword masks and duplicate
            // the AND result into both halves of each D element.
            byte(0x66); byte(0x0f); byte(0x70); // pshufd xmm3, destination
            byte(static_cast<std::uint8_t>(0xd8U | destination));
            byte(0xb1); // dword order 1,0,3,2
            packed(0xdb, destination, 3);
        }
    }
    void clear_upper(unsigned xmm) {
        byte(0xf3); byte(0x0f); byte(0x7e); // movq xmm, xmm clears high qword
        byte(static_cast<std::uint8_t>(0xc0U | (xmm << 3) | xmm));
    }
};

static_assert(std::is_standard_layout_v<CpuState>);
static_assert(sizeof(decltype(CpuState::v)::value_type) == 16);

} // namespace

bool accepts_integer_simd(std::uint32_t instruction) noexcept {
    return decode(instruction).has_value() || accepts_bit_moves(instruction);
}

std::vector<std::uint8_t> emit_integer_simd(std::uint32_t instruction) {
    if (accepts_bit_moves(instruction)) return emit_bit_moves(instruction);
    const auto decoded = decode(instruction);
    if (!decoded) throw std::invalid_argument("unsupported native integer SIMD encoding");
    const auto& op = *decoded;
    Fragment out;
    out.bytes.reserve(80);
    out.load(0, op.source);
    // Even when registers alias, all inputs are loaded before the sole store.
    switch (op.operation) {
    case Operation::bit_not:
        out.invert(0, 3);
        break;
    case Operation::negate: {
        constexpr std::array<std::uint8_t, 4> subtract{0xf8, 0xf9, 0xfa, 0xfb};
        out.copy(1, 0);
        out.zero(0);
        out.packed(subtract[op.size], 0, 1);
        break;
    }
    case Operation::equal_zero:
        out.zero(1);
        out.equal(0, 1, op.size);
        break;
    default:
        out.load(1, op.second);
        switch (op.operation) {
        case Operation::bit_and: out.packed(0xdb, 0, 1); break;
        case Operation::bit_clear:
            out.packed(0xdf, 1, 0); // ~Rm & Rn
            out.copy(0, 1);
            break;
        case Operation::bit_or: out.packed(0xeb, 0, 1); break;
        case Operation::bit_or_not:
            out.invert(1, 3);
            out.packed(0xeb, 0, 1);
            break;
        case Operation::bit_xor: out.packed(0xef, 0, 1); break;
        case Operation::bit_select:
            out.load(2, op.destination);
            out.packed(0xdb, 0, 2); // Rn & old Rd
            out.packed(0xdf, 2, 1); // ~old Rd & Rm
            out.packed(0xeb, 0, 2);
            break;
        case Operation::bit_insert:
            out.load(2, op.destination);
            out.packed(0xdb, 0, 1); // Rn & Rm
            out.packed(0xdf, 1, 2); // ~Rm & old Rd
            out.packed(0xeb, 0, 1);
            break;
        case Operation::bit_insert_false:
            out.load(2, op.destination);
            out.packed(0xdb, 2, 1); // old Rd & Rm
            out.packed(0xdf, 1, 0); // ~Rm & Rn
            out.copy(0, 1);
            out.packed(0xeb, 0, 2);
            break;
        case Operation::add: {
            constexpr std::array<std::uint8_t, 4> add{0xfc, 0xfd, 0xfe, 0xd4};
            out.packed(add[op.size], 0, 1);
            break;
        }
        case Operation::subtract: {
            constexpr std::array<std::uint8_t, 4> subtract{0xf8, 0xf9, 0xfa, 0xfb};
            out.packed(subtract[op.size], 0, 1);
            break;
        }
        case Operation::equal:
            out.equal(0, 1, op.size);
            break;
        case Operation::bit_test:
            out.packed(0xdb, 0, 1);
            out.zero(1);
            out.equal(0, 1, op.size);
            out.invert(0, 3);
            break;
        default: throw std::logic_error("invalid native integer SIMD operation");
        }
        break;
    }
    if (!op.quad) out.clear_upper(0);
    out.store(0, op.destination);
    return std::move(out.bytes);
}

} // namespace transmute::jit_detail
