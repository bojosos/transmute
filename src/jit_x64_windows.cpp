#include "transmute/jit.hpp"
#include "jit_simd.hpp"
#include "jit_simd_fusion.hpp"
#include "jit_fp.hpp"
#include "jit_atomic.hpp"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace transmute {
namespace {

struct X64Emitter {
    std::vector<std::uint8_t> bytes;

    void byte(std::uint8_t value) { bytes.push_back(value); }
    void word(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    void wide_word(std::uint64_t value) {
        word(static_cast<std::uint32_t>(value));
        word(static_cast<std::uint32_t>(value >> 32));
    }
    void patch_relative(std::size_t displacement, std::size_t target) {
        const auto relative = static_cast<std::uint32_t>(target - (displacement + 4));
        for (unsigned i = 0; i < 4; ++i) {
            bytes[displacement + i] = static_cast<std::uint8_t>(relative >> (i * 8));
        }
    }
    void state_operand(std::uint8_t register_bits, std::uint32_t offset) {
        if (offset == 0) {
            byte(static_cast<std::uint8_t>(register_bits | 0x01U));
        } else if (offset <= 127) {
            byte(static_cast<std::uint8_t>(register_bits | 0x41U));
            byte(static_cast<std::uint8_t>(offset));
        } else {
            byte(static_cast<std::uint8_t>(register_bits | 0x81U));
            word(offset);
        }
    }
    void load_register(std::uint32_t offset, bool wide) {
        if (wide) byte(0x48);
        byte(0x8b); state_operand(0, offset); // mov rax/eax, [rcx + offset]
    }
    void load_second(std::uint32_t offset, bool wide) {
        if (wide) byte(0x48);
        byte(0x8b); state_operand(0x10, offset); // mov rdx/edx, [rcx + offset]
    }
    void store_register(std::uint32_t offset) {
        byte(0x48); byte(0x89); state_operand(0, offset); // mov [rcx + offset], rax
    }
    void immediate(std::uint64_t value, bool wide) {
        if (wide) byte(0x48);
        byte(0xb8); // mov rax/eax, imm64/imm32
        if (wide) wide_word(value);
        else word(static_cast<std::uint32_t>(value));
    }
    void store_arithmetic_flags(bool subtract) {
        byte(0x9c); byte(0x5a); // pushfq; pop rdx
        byte(0x41); byte(0x89); byte(0xd0); // mov r8d, edx
        byte(0x41); byte(0x81); byte(0xe0); word(0xc0); // N and Z
        byte(0x41); byte(0xc1); byte(0xe0); byte(24);
        byte(0x41); byte(0x89); byte(0xd1); // mov r9d, edx
        byte(0x41); byte(0x83); byte(0xe1); byte(1); // CF
        if (subtract) {
            byte(0x41); byte(0x83); byte(0xf1); byte(1); // ARM C = !x64 CF
        }
        byte(0x41); byte(0xc1); byte(0xe1); byte(29);
        byte(0x45); byte(0x09); byte(0xc8); // or r8d, r9d
        byte(0x41); byte(0x89); byte(0xd1);
        byte(0x41); byte(0x81); byte(0xe1); word(0x800); // OF
        byte(0x41); byte(0xc1); byte(0xe1); byte(17);
        byte(0x45); byte(0x09); byte(0xc8);
        byte(0x44); byte(0x89); byte(0x81);
        word(static_cast<std::uint32_t>(offsetof(CpuState, nzcv)));
    }
};

constexpr auto x_offset(unsigned reg) {
    return static_cast<std::uint32_t>(offsetof(CpuState, x) + reg * 8);
}
constexpr auto v_offset(unsigned reg, unsigned half = 0) {
    return static_cast<std::uint32_t>(offsetof(CpuState, v) + reg * 16 + half * 8);
}
constexpr auto sp_offset = static_cast<std::uint32_t>(offsetof(CpuState, sp));
constexpr auto pc_offset = static_cast<std::uint32_t>(offsetof(CpuState, pc));

constexpr bool branch_condition(unsigned condition, unsigned flags) {
    const bool n = (flags & 8U) != 0;
    const bool z = (flags & 4U) != 0;
    const bool c = (flags & 2U) != 0;
    const bool v = (flags & 1U) != 0;
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
    default: return true;
    }
}

constexpr std::uint32_t branch_truth_table(unsigned condition) {
    std::uint32_t table = 0;
    for (unsigned flags = 0; flags < 16; ++flags) {
        if (branch_condition(condition, flags)) table |= 1U << flags;
    }
    return table;
}

bool emit_instruction(X64Emitter& out, const DecodedInstruction& instruction,
                      std::uint32_t opcode, std::uint64_t next_pc) {
    bool returned = false;
        switch (instruction.op) {
        case BlockOp::nop:
            break;
        case BlockOp::guarded_fp:
            throw std::logic_error("guarded FP requires a counted native region");
        case BlockOp::atomic_lse:
            throw std::logic_error("LSE atomics require a counted native region");
        case BlockOp::load_vector:
        case BlockOp::store_vector:
        case BlockOp::load_vector_pair:
        case BlockOp::store_vector_pair:
            throw std::logic_error("vector memory requires a counted native region");
        case BlockOp::simd_integer: {
            const auto fragment = jit_detail::emit_integer_simd(opcode);
            out.bytes.insert(out.bytes.end(), fragment.begin(), fragment.end());
            break;
        }
        case BlockOp::move_zero:
        case BlockOp::move_inverse:
            if (instruction.destination != 31) {
                out.immediate(instruction.immediate, instruction.wide);
                out.store_register(x_offset(instruction.destination));
            }
            break;
        case BlockOp::address:
            if (instruction.destination != 31) {
                out.immediate(instruction.immediate, true);
                out.store_register(x_offset(instruction.destination));
            }
            break;
        case BlockOp::move_keep:
            if (instruction.destination != 31) {
                out.load_register(x_offset(instruction.destination), instruction.wide);
                if (instruction.wide) {
                    out.byte(0x48); out.byte(0xba);
                    out.wide_word(~(0xffffULL << instruction.shift)); // mov rdx, mask
                    out.byte(0x48); out.byte(0x21); out.byte(0xd0); // and rax, rdx
                    out.byte(0x48); out.byte(0xba);
                    out.wide_word(instruction.immediate); // mov rdx, immediate
                    out.byte(0x48); out.byte(0x09); out.byte(0xd0); // or rax, rdx
                } else {
                    out.byte(0x25);
                    out.word(~(0xffffU << instruction.shift)); // and eax, mask
                    out.byte(0x0d);
                    out.word(static_cast<std::uint32_t>(instruction.immediate));
                }
                out.store_register(x_offset(instruction.destination));
            }
            break;
        case BlockOp::float_immediate:
            out.immediate(instruction.immediate, instruction.wide);
            out.store_register(v_offset(instruction.destination));
            out.byte(0x31); out.byte(0xc0); // xor eax, eax
            out.store_register(v_offset(instruction.destination, 1));
            break;
        case BlockOp::float_register:
        case BlockOp::float_negate_bits:
        case BlockOp::float_absolute_bits:
            out.load_register(v_offset(instruction.source), instruction.wide);
            if (instruction.op == BlockOp::float_negate_bits) {
                if (instruction.wide) {
                    out.byte(0x48); out.byte(0xba);
                    out.wide_word(1ULL << 63); // mov rdx, sign bit
                    out.byte(0x48); out.byte(0x31); out.byte(0xd0);
                } else {
                    out.byte(0x35); out.word(1U << 31); // xor eax, sign bit
                }
            } else if (instruction.op == BlockOp::float_absolute_bits) {
                if (instruction.wide) {
                    out.byte(0x48); out.byte(0xba);
                    out.wide_word(~(1ULL << 63)); // mov rdx, magnitude mask
                    out.byte(0x48); out.byte(0x21); out.byte(0xd0);
                } else {
                    out.byte(0x25); out.word(~(1U << 31));
                }
            }
            out.store_register(v_offset(instruction.destination));
            out.byte(0x31); out.byte(0xc0); // clear high half of Vd
            out.store_register(v_offset(instruction.destination, 1));
            break;
        case BlockOp::float_to_integer_bits:
            if (instruction.destination != 31) {
                out.load_register(v_offset(instruction.source), instruction.wide);
                out.store_register(x_offset(instruction.destination));
            }
            break;
        case BlockOp::integer_to_float_bits:
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // WZR/XZR
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            out.store_register(v_offset(instruction.destination));
            out.byte(0x31); out.byte(0xc0);
            out.store_register(v_offset(instruction.destination, 1));
            break;
        case BlockOp::add_immediate:
        case BlockOp::subtract_immediate: {
            out.load_register(instruction.source == 31
                                  ? sp_offset : x_offset(instruction.source),
                              instruction.wide);
            if (instruction.wide) out.byte(0x48);
            out.byte(instruction.op == BlockOp::add_immediate ? 0x05 : 0x2d);
            out.word(static_cast<std::uint32_t>(instruction.immediate));
            if (instruction.destination != 31 || !instruction.set_flags) {
                out.store_register(instruction.destination == 31
                                       ? sp_offset : x_offset(instruction.destination));
            }
            if (instruction.set_flags) {
                out.store_arithmetic_flags(instruction.op == BlockOp::subtract_immediate);
            }
            break;
        }
        case BlockOp::add_register:
        case BlockOp::subtract_register: {
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            if (instruction.second == 31) {
                out.byte(0x31); out.byte(0xd2); // xor edx, edx
            } else {
                out.load_second(x_offset(instruction.second), instruction.wide);
            }
            if (instruction.shift != 0) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xc1);
                out.byte(instruction.shift_type == 0 ? 0xe2 :
                         instruction.shift_type == 1 ? 0xea : 0xfa);
                out.byte(instruction.shift); // shl/shr/sar rdx/edx, imm8
            }
            if (instruction.wide) out.byte(0x48);
            out.byte(instruction.op == BlockOp::add_register ? 0x01 : 0x29);
            out.byte(0xd0); // add/sub rax/eax, rdx/edx
            if (instruction.destination != 31) {
                out.store_register(x_offset(instruction.destination));
            }
            if (instruction.set_flags) {
                out.store_arithmetic_flags(instruction.op == BlockOp::subtract_register);
            }
            break;
        }
        case BlockOp::shift_register:
            if (instruction.destination != 31) {
                if (instruction.source == 31) {
                    out.byte(0x31); out.byte(0xc0); // WZR/XZR operand
                } else {
                    out.load_register(x_offset(instruction.source), instruction.wide);
                }
                if (instruction.second == 31) {
                    out.byte(0x31); out.byte(0xd2); // WZR/XZR count
                } else {
                    // Only the bottom five/six count bits affect ARM shifts.
                    out.load_second(x_offset(instruction.second), false);
                }
                out.byte(0x49); out.byte(0x89); out.byte(0xca); // mov r10, rcx
                out.byte(0x89); out.byte(0xd1); // mov ecx, edx
                if (instruction.wide) out.byte(0x48);
                out.byte(0xd3);
                out.byte(instruction.shift_type == 0 ? 0xe0
                    : instruction.shift_type == 1 ? 0xe8
                    : instruction.shift_type == 2 ? 0xf8 : 0xc8);
                // shl/shr/sar/ror rax/eax, cl; x64 masks the count to width.
                out.byte(0x4c); out.byte(0x89); out.byte(0xd1); // mov rcx, r10
                out.store_register(x_offset(instruction.destination));
            }
            break;
        case BlockOp::add_extended:
        case BlockOp::subtract_extended: {
            out.load_register(instruction.source == 31
                                  ? sp_offset : x_offset(instruction.source),
                              instruction.wide);
            if (instruction.second == 31) {
                out.byte(0x31); out.byte(0xd2); // xor edx, edx
            } else {
                out.load_second(x_offset(instruction.second), true);
            }
            switch (instruction.extend_option) {
            case 0: out.byte(0x0f); out.byte(0xb6); out.byte(0xd2); break;
            case 1: out.byte(0x0f); out.byte(0xb7); out.byte(0xd2); break;
            case 2: out.byte(0x89); out.byte(0xd2); break; // mov edx, edx
            case 3: break;
            case 4: out.byte(0x48); out.byte(0x0f); out.byte(0xbe);
                    out.byte(0xd2); break; // movsx rdx, dl
            case 5: out.byte(0x48); out.byte(0x0f); out.byte(0xbf);
                    out.byte(0xd2); break; // movsx rdx, dx
            case 6: out.byte(0x48); out.byte(0x63); out.byte(0xd2);
                    break; // movsxd rdx, edx
            case 7: break;
            }
            if (instruction.shift != 0) {
                out.byte(0x48); out.byte(0xc1); out.byte(0xe2);
                out.byte(instruction.shift); // shl rdx, imm8
            }
            if (instruction.wide) out.byte(0x48);
            out.byte(instruction.op == BlockOp::add_extended ? 0x01 : 0x29);
            out.byte(0xd0); // add/sub rax/eax, rdx/edx
            if (instruction.destination != 31 || !instruction.set_flags) {
                out.store_register(instruction.destination == 31
                                       ? sp_offset : x_offset(instruction.destination));
            }
            if (instruction.set_flags) {
                out.store_arithmetic_flags(
                    instruction.op == BlockOp::subtract_extended);
            }
            break;
        }
        case BlockOp::divide_unsigned:
        case BlockOp::divide_signed: {
            if (instruction.destination == 31) break;
            if (instruction.source == 31) { out.byte(0x31); out.byte(0xc0); }
            else out.load_register(x_offset(instruction.source), instruction.wide);
            if (instruction.second == 31) { out.byte(0x31); out.byte(0xd2); }
            else out.load_second(x_offset(instruction.second), instruction.wide);
            out.byte(instruction.wide ? 0x49 : 0x41);
            out.byte(0x89); out.byte(0xd0); // r8=divisor before RDX dividend high
            out.byte(instruction.wide ? 0x4d : 0x45);
            out.byte(0x85); out.byte(0xc0);
            out.byte(0x0f); out.byte(0x84);
            const auto zero = out.bytes.size(); out.word(0);
            std::optional<std::size_t> negated;
            if (instruction.op == BlockOp::divide_signed) {
                out.byte(instruction.wide ? 0x49 : 0x41);
                out.byte(0x83); out.byte(0xf8); out.byte(0xff);
                out.byte(0x0f); out.byte(0x85);
                const auto ordinary = out.bytes.size(); out.word(0);
                // Negation wraps INT_MIN, exactly Arm's nontrapping overflow.
                if (instruction.wide) out.byte(0x48);
                out.byte(0xf7); out.byte(0xd8);
                out.byte(0xe9); negated = out.bytes.size(); out.word(0);
                out.patch_relative(ordinary, out.bytes.size());
                if (instruction.wide) out.byte(0x48);
                out.byte(0x99); // cqo/cdq
            } else { out.byte(0x31); out.byte(0xd2); }
            out.byte(instruction.wide ? 0x49 : 0x41);
            out.byte(0xf7); out.byte(instruction.op == BlockOp::divide_signed ? 0xf8 : 0xf0);
            out.byte(0xe9); const auto divided = out.bytes.size(); out.word(0);
            out.patch_relative(zero, out.bytes.size());
            out.byte(0x31); out.byte(0xc0); // Arm division by zero returns zero.
            out.patch_relative(divided, out.bytes.size());
            if (negated) out.patch_relative(*negated, out.bytes.size());
            out.store_register(x_offset(instruction.destination));
            break;
        }
        case BlockOp::count_leading_zero: {
            if (instruction.destination == 31) break;
            if (instruction.source == 31) { out.byte(0x31); out.byte(0xc0); }
            else out.load_register(x_offset(instruction.source), instruction.wide);
            if (instruction.wide) out.byte(0x48);
            out.byte(0x0f); out.byte(0xbd); out.byte(0xc0); // bsr rax/eax,rax/eax
            out.byte(0x0f); out.byte(0x84);
            const auto zero = out.bytes.size(); out.word(0);
            out.byte(0x83); out.byte(0xf0); out.byte(instruction.wide ? 63 : 31);
            out.byte(0xe9); const auto done = out.bytes.size(); out.word(0);
            out.patch_relative(zero, out.bytes.size());
            out.immediate(instruction.wide ? 64 : 32, false);
            out.patch_relative(done, out.bytes.size());
            out.store_register(x_offset(instruction.destination));
            break;
        }
        case BlockOp::reverse_bytes: {
            if (instruction.destination == 31) break;
            if (instruction.source == 31) { out.byte(0x31); out.byte(0xc0); }
            else out.load_register(x_offset(instruction.source), instruction.wide);
            if (instruction.field_width == 2) {
                // Swap each byte pair with one mask, without crossing groups.
                if (instruction.wide) out.byte(0x48);
                out.byte(0x89); out.byte(0xc2); // mov rdx/edx, rax/eax
                out.byte(instruction.wide ? 0x49 : 0x41); out.byte(0xb8);
                if (instruction.wide) out.wide_word(0x00ff00ff00ff00ffULL);
                else out.word(0x00ff00ffU); // mov r8/r8d, byte mask
                out.byte(instruction.wide ? 0x4c : 0x44);
                out.byte(0x21); out.byte(0xc0); // and rax/eax, r8/r8d
                if (instruction.wide) out.byte(0x48);
                out.byte(0xc1); out.byte(0xe0); out.byte(8); // shl rax/eax, 8
                if (instruction.wide) out.byte(0x48);
                out.byte(0xc1); out.byte(0xea); out.byte(8); // shr rdx/edx, 8
                out.byte(instruction.wide ? 0x4c : 0x44);
                out.byte(0x21); out.byte(0xc2); // and rdx/edx, r8/r8d
                if (instruction.wide) out.byte(0x48);
                out.byte(0x09); out.byte(0xd0); // or rax/eax, rdx/edx
            } else {
                if (instruction.wide) out.byte(0x48);
                out.byte(0x0f); out.byte(0xc8); // bswap rax/eax
                if (instruction.wide && instruction.field_width == 4) {
                    out.byte(0x48); out.byte(0xc1); out.byte(0xc8); out.byte(32);
                    // ror rax,32 restores the order of the two reversed words.
                }
            }
            out.store_register(x_offset(instruction.destination));
            break;
        }
        case BlockOp::extract_registers: {
            if (instruction.destination == 31) break;
            if (instruction.second == 31) { out.byte(0x31); out.byte(0xc0); }
            else out.load_register(x_offset(instruction.second), instruction.wide);
            if (instruction.shift != 0) {
                if (instruction.source == 31) { out.byte(0x31); out.byte(0xd2); }
                else out.load_second(x_offset(instruction.source), instruction.wide);
                if (instruction.wide) out.byte(0x48);
                out.byte(0x0f); out.byte(0xac); out.byte(0xd0);
                out.byte(instruction.shift); // shrd rax/eax,rdx/edx,imm8
            }
            out.store_register(x_offset(instruction.destination));
            break;
        }
        case BlockOp::logical_register: {
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            if (instruction.second == 31) {
                out.byte(0x31); out.byte(0xd2); // xor edx, edx
            } else {
                out.load_second(x_offset(instruction.second), instruction.wide);
            }
            if (instruction.shift != 0) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xc1);
                out.byte(instruction.shift_type == 0 ? 0xe2
                    : instruction.shift_type == 1 ? 0xea
                    : instruction.shift_type == 2 ? 0xfa : 0xca);
                out.byte(instruction.shift);
            }
            if (instruction.invert_second) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xf7); out.byte(0xd2); // not rdx/edx
            }
            if (instruction.wide) out.byte(0x48);
            out.byte(instruction.logical_operation == 1 ? 0x09 :
                     instruction.logical_operation == 2 ? 0x31 : 0x21);
            out.byte(0xd0); // and/or/xor rax/eax, rdx/edx
            if (instruction.destination != 31) {
                out.store_register(x_offset(instruction.destination));
            }
            if (instruction.set_flags) out.store_arithmetic_flags(false);
            break;
        }
        case BlockOp::logical_immediate: {
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            if (instruction.wide) out.byte(0x48);
            out.byte(0xba); // mov rdx/edx, decoded bit mask
            if (instruction.wide) out.wide_word(instruction.immediate);
            else out.word(static_cast<std::uint32_t>(instruction.immediate));
            if (instruction.wide) out.byte(0x48);
            out.byte(instruction.logical_operation == 1 ? 0x09 :
                     instruction.logical_operation == 2 ? 0x31 : 0x21);
            out.byte(0xd0); // and/or/xor rax/eax, rdx/edx
            if (instruction.destination != 31 || !instruction.set_flags) {
                out.store_register(instruction.destination == 31
                                       ? sp_offset : x_offset(instruction.destination));
            }
            if (instruction.set_flags) out.store_arithmetic_flags(false);
            break;
        }
        case BlockOp::unsigned_bitfield:
        case BlockOp::signed_bitfield:
        case BlockOp::merge_bitfield: {
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            if (!instruction.bitfield_left && instruction.shift != 0) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xc1); out.byte(0xe8); out.byte(instruction.shift);
                // shr rax/eax, imm8
            }
            if (instruction.wide) out.byte(0x48);
            out.byte(0xba); // mov rdx/edx, mask
            if (instruction.wide) out.wide_word(instruction.immediate);
            else out.word(static_cast<std::uint32_t>(instruction.immediate));
            if (instruction.wide) out.byte(0x48);
            out.byte(0x21); out.byte(0xd0); // and rax/eax, rdx/edx
            if (instruction.bitfield_left) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xc1); out.byte(0xe0); out.byte(instruction.shift);
                // shl rax/eax, imm8
            }
            if (instruction.op == BlockOp::signed_bitfield) {
                const auto width = instruction.wide ? 64U : 32U;
                const auto sign_bit = instruction.field_width - 1U +
                    (instruction.bitfield_left ? instruction.shift : 0U);
                const auto extension_shift = width - sign_bit - 1U;
                if (extension_shift != 0) {
                    if (instruction.wide) out.byte(0x48);
                    out.byte(0xc1); out.byte(0xe0);
                    out.byte(static_cast<std::uint8_t>(extension_shift));
                    if (instruction.wide) out.byte(0x48);
                    out.byte(0xc1); out.byte(0xf8);
                    out.byte(static_cast<std::uint8_t>(extension_shift));
                    // shl then sar sign-extends the selected high field bit.
                }
            } else if (instruction.op == BlockOp::merge_bitfield &&
                       instruction.destination != 31) {
                out.load_second(x_offset(instruction.destination), instruction.wide);
                const auto mask = instruction.immediate <<
                    (instruction.bitfield_left ? instruction.shift : 0U);
                out.byte(0x49); out.byte(0xb8); out.wide_word(~mask); // mov r8, ~mask
                if (instruction.wide) out.byte(0x4c);
                else out.byte(0x44);
                out.byte(0x21); out.byte(0xc2); // and rdx/edx, r8/r8d
                if (instruction.wide) out.byte(0x48);
                out.byte(0x09); out.byte(0xd0); // or rax/eax, preserved destination
            }
            if (instruction.destination != 31) {
                out.store_register(x_offset(instruction.destination));
            }
            break;
        }
        case BlockOp::multiply_add:
        case BlockOp::multiply_subtract:
        case BlockOp::widening_multiply_add:
        case BlockOp::widening_multiply_subtract:
        case BlockOp::multiply_high_signed:
        case BlockOp::multiply_high_unsigned: {
            if (instruction.destination == 31) break;
            const bool widening = instruction.op == BlockOp::widening_multiply_add ||
                instruction.op == BlockOp::widening_multiply_subtract;
            const bool high = instruction.op == BlockOp::multiply_high_signed ||
                instruction.op == BlockOp::multiply_high_unsigned;
            const bool subtract = instruction.op == BlockOp::multiply_subtract ||
                instruction.op == BlockOp::widening_multiply_subtract;
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0);
            } else {
                out.load_register(x_offset(instruction.source), !widening && instruction.wide);
            }
            if (instruction.second == 31) {
                out.byte(0x31); out.byte(0xd2);
            } else {
                out.load_second(x_offset(instruction.second), !widening && instruction.wide);
            }
            if (widening && !instruction.unsigned_operands) {
                out.byte(0x48); out.byte(0x63); out.byte(0xc0); // movsxd rax, eax
                out.byte(0x48); out.byte(0x63); out.byte(0xd2); // movsxd rdx, edx
            }
            if (high) {
                out.byte(0x49); out.byte(0x89); out.byte(0xd0); // mov r8, rdx
                out.byte(0x49); out.byte(0xf7);
                out.byte(instruction.op == BlockOp::multiply_high_signed ? 0xe8 : 0xe0);
                // imul/mul r8 produces the complete RDX:RAX product.
                out.byte(0x48); out.byte(0x89); out.byte(0xd0); // mov rax, rdx
            } else {
                if (instruction.wide) out.byte(0x48);
                out.byte(0x0f); out.byte(0xaf); out.byte(0xc2); // imul rax/eax, rdx/edx
                if (instruction.accumulator == 31) {
                    out.byte(0x31); out.byte(0xd2);
                } else {
                    out.load_second(x_offset(instruction.accumulator), instruction.wide);
                }
                if (instruction.wide) out.byte(0x48);
                if (subtract) {
                    out.byte(0x29); out.byte(0xc2); // sub rdx/edx, product
                    if (instruction.wide) out.byte(0x48);
                    out.byte(0x89); out.byte(0xd0); // mov rax/eax, result
                } else {
                    out.byte(0x01); out.byte(0xd0);
                }
            }
            out.store_register(x_offset(instruction.destination));
            break;
        }
        case BlockOp::conditional_compare_add:
        case BlockOp::conditional_compare_subtract: {
            out.byte(0x44); out.byte(0x8b); out.byte(0x81);
            out.word(static_cast<std::uint32_t>(offsetof(CpuState, nzcv)));
            out.byte(0x41); out.byte(0xc1); out.byte(0xe8); out.byte(28);
            out.byte(0x41); out.byte(0xb9);
            out.word(branch_truth_table(instruction.condition));
            out.byte(0x45); out.byte(0x0f); out.byte(0xa3); out.byte(0xc1);
            out.byte(0x0f); out.byte(0x83); // jnc false_flags
            const auto false_jump = out.bytes.size();
            out.word(0);
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0);
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            if (instruction.compare_immediate) {
                out.byte(0xba); out.word(instruction.second); // mov edx, imm5
            } else if (instruction.second == 31) {
                out.byte(0x31); out.byte(0xd2);
            } else {
                out.load_second(x_offset(instruction.second), instruction.wide);
            }
            const bool subtract = instruction.op == BlockOp::conditional_compare_subtract;
            if (instruction.wide) out.byte(0x48);
            out.byte(subtract ? 0x29 : 0x01); out.byte(0xd0);
            out.store_arithmetic_flags(subtract);
            out.byte(0xe9); // jmp done
            const auto done_jump = out.bytes.size();
            out.word(0);
            out.patch_relative(false_jump, out.bytes.size());
            out.byte(0xc7); out.byte(0x81);
            out.word(static_cast<std::uint32_t>(offsetof(CpuState, nzcv)));
            out.word(static_cast<std::uint32_t>(instruction.immediate));
            out.patch_relative(done_jump, out.bytes.size());
            break;
        }
        case BlockOp::conditional_select: {
            if (instruction.second == 31) {
                out.byte(0x31); out.byte(0xd2); // xor edx, edx
            } else {
                out.load_second(x_offset(instruction.second), instruction.wide);
            }
            if (instruction.invert_second && instruction.increment_second) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xf7); out.byte(0xda); // neg rdx/edx
            } else if (instruction.invert_second) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xf7); out.byte(0xd2); // not rdx/edx
            } else if (instruction.increment_second) {
                if (instruction.wide) out.byte(0x48);
                out.byte(0xff); out.byte(0xc2); // inc rdx/edx
            }
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            out.byte(0x44); out.byte(0x8b); out.byte(0x81);
            out.word(static_cast<std::uint32_t>(offsetof(CpuState, nzcv)));
            out.byte(0x41); out.byte(0xc1); out.byte(0xe8); out.byte(28);
            out.byte(0x41); out.byte(0xb9);
            out.word(branch_truth_table(instruction.condition));
            out.byte(0x45); out.byte(0x0f); out.byte(0xa3); out.byte(0xc1);
            // bt r9d, r8d selects the condition result in CF.
            if (instruction.wide) out.byte(0x48);
            out.byte(0x0f); out.byte(0x43); out.byte(0xc2);
            // cmovnc rax/eax, rdx/edx selects transformed false operand.
            if (instruction.destination != 31) {
                out.store_register(x_offset(instruction.destination));
            }
            break;
        }
        case BlockOp::compare_branch_zero:
        case BlockOp::compare_branch_nonzero:
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            if (instruction.wide) out.byte(0x48);
            out.byte(0x85); out.byte(0xc0); // test rax/eax, rax/eax
            out.immediate(next_pc, true);
            out.byte(0x48); out.byte(0xba);
            out.wide_word(instruction.immediate); // mov rdx, target
            out.byte(0x48); out.byte(0x0f);
            out.byte(instruction.op == BlockOp::compare_branch_zero ? 0x44 : 0x45);
            out.byte(0xc2); // cmovz/cmovnz rax, rdx
            out.store_register(pc_offset);
            returned = true;
            break;
        case BlockOp::test_branch_zero:
        case BlockOp::test_branch_nonzero:
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), true);
            }
            out.byte(0x48); out.byte(0xba);
            out.wide_word(1ULL << instruction.shift); // mov rdx, tested bit
            out.byte(0x48); out.byte(0x85); out.byte(0xd0); // test rax, rdx
            out.immediate(next_pc, true);
            out.byte(0x48); out.byte(0xba);
            out.wide_word(instruction.immediate); // mov rdx, target
            out.byte(0x48); out.byte(0x0f);
            out.byte(instruction.op == BlockOp::test_branch_zero ? 0x44 : 0x45);
            out.byte(0xc2); // cmovz/cmovnz rax, rdx
            out.store_register(pc_offset);
            returned = true;
            break;
        case BlockOp::branch:
        case BlockOp::branch_link:
            if (instruction.op == BlockOp::branch_link) {
                out.immediate(next_pc, true);
                out.store_register(x_offset(30));
            }
            out.immediate(instruction.immediate, true);
            out.store_register(pc_offset);
            returned = true;
            break;
        case BlockOp::load_byte:
        case BlockOp::load_signed_byte:
        case BlockOp::store_byte:
        case BlockOp::load_halfword:
        case BlockOp::load_signed_halfword:
        case BlockOp::store_halfword:
        case BlockOp::load_word:
        case BlockOp::load_signed_word:
        case BlockOp::store_word:
        case BlockOp::load_doubleword:
        case BlockOp::store_doubleword:
            if (instruction.memory_writeback) {
                out.load_register(instruction.source == 31 ? sp_offset
                    : x_offset(instruction.source), true);
                out.byte(0x4c); out.byte(0x8d); out.byte(0x88);
                out.word(static_cast<std::uint32_t>(instruction.immediate));
                // lea r9, [rax + signed displacement]; retain the old base
                // when destination and base name the same guest register.
            }
            if (instruction.destination != 31 &&
                (instruction.op == BlockOp::load_byte ||
                 instruction.op == BlockOp::load_signed_byte ||
                 instruction.op == BlockOp::load_halfword ||
                 instruction.op == BlockOp::load_signed_halfword ||
                 instruction.op == BlockOp::load_word ||
                 instruction.op == BlockOp::load_signed_word ||
                 instruction.op == BlockOp::load_doubleword)) {
                if (instruction.op == BlockOp::load_byte) {
                    out.byte(0x0f); out.byte(0xb6); out.byte(0xc2);
                    // movzx eax, dl
                } else if (instruction.op == BlockOp::load_signed_byte) {
                    if (instruction.wide) out.byte(0x48);
                    out.byte(0x0f); out.byte(0xbe); out.byte(0xc2);
                    // movsx rax/eax, dl; the W form clears the upper half.
                } else if (instruction.op == BlockOp::load_halfword) {
                    out.byte(0x0f); out.byte(0xb7); out.byte(0xc2);
                    // movzx eax, dx
                } else if (instruction.op == BlockOp::load_signed_halfword) {
                    if (instruction.wide) out.byte(0x48);
                    out.byte(0x0f); out.byte(0xbf); out.byte(0xc2);
                    // movsx rax/eax, dx
                } else if (instruction.op == BlockOp::load_word) {
                    out.byte(0x89); out.byte(0xd0); // mov eax, edx
                } else if (instruction.op == BlockOp::load_signed_word) {
                    out.byte(0x48); out.byte(0x63); out.byte(0xc2);
                    // movsxd rax, edx
                } else {
                    out.byte(0x48); out.byte(0x89); out.byte(0xd0);
                    // mov rax, rdx
                }
                out.store_register(x_offset(instruction.destination));
            }
            if (instruction.memory_writeback) {
                out.byte(0x4c); out.byte(0x89); out.byte(0x89);
                out.word(instruction.source == 31 ? sp_offset
                    : x_offset(instruction.source)); // mov [rcx + base], r9
            }
            break;
        case BlockOp::load_pair:
        case BlockOp::store_pair:
            if (instruction.memory_writeback) {
                out.load_register(instruction.source == 31 ? sp_offset
                    : x_offset(instruction.source), true);
                out.byte(0x4c); out.byte(0x8d); out.byte(0x88);
                out.word(static_cast<std::uint32_t>(instruction.immediate));
                // lea r9, [old base + signed pair displacement]
            }
            if (instruction.op == BlockOp::load_pair) {
                if (instruction.destination != 31) {
                    if (instruction.wide) out.byte(0x48);
                    out.byte(0x89); out.byte(0xd0); // mov rax/eax, rdx/edx
                    out.store_register(x_offset(instruction.destination));
                }
                if (instruction.second != 31) {
                    out.byte(instruction.wide ? 0x4c : 0x44);
                    out.byte(0x89); out.byte(0xc0);
                    // mov rax/eax, r8/r8d
                    out.store_register(x_offset(instruction.second));
                }
            }
            if (instruction.memory_writeback) {
                out.byte(0x4c); out.byte(0x89); out.byte(0x89);
                out.word(instruction.source == 31 ? sp_offset
                    : x_offset(instruction.source));
            }
            break;
        case BlockOp::conditional_branch:
            out.byte(0x8b); out.byte(0x91);
            out.word(static_cast<std::uint32_t>(offsetof(CpuState, nzcv)));
            out.byte(0xc1); out.byte(0xea); out.byte(28); // shr edx, 28
            out.byte(0x41); out.byte(0xb8);
            out.word(branch_truth_table(instruction.condition)); // mov r8d, table
            out.byte(0x41); out.byte(0x0f); out.byte(0xa3); out.byte(0xd0);
            // bt r8d, edx selects the condition result in CF.
            out.immediate(next_pc, true);
            out.byte(0x48); out.byte(0xba); out.wide_word(instruction.immediate);
            out.byte(0x48); out.byte(0x0f); out.byte(0x42); out.byte(0xc2);
            // cmovc rax, rdx
            out.store_register(pc_offset);
            returned = true;
            break;
        case BlockOp::ret:
        case BlockOp::branch_register:
        case BlockOp::branch_link_register:
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0); // xor eax, eax
            } else {
                out.load_register(x_offset(instruction.source), true);
            }
            if (instruction.op == BlockOp::branch_link_register) {
                // Keep the target in RAX before writing LR; BLR X30 reads old X30.
                out.byte(0x48); out.byte(0xba);
                out.wide_word(next_pc);
                out.byte(0x48); out.byte(0x89); out.byte(0x91);
                out.word(static_cast<std::uint32_t>(x_offset(30)));
            }
            out.store_register(pc_offset);
            returned = true;
            break;
        }
    return returned;
}

std::vector<std::uint8_t> emit_block(const DecodedBlock& block) {
    X64Emitter out;
    bool returned = false;
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        if (index + 1 < block.instructions.size() &&
            block.instructions[index].op == BlockOp::simd_integer &&
            block.instructions[index + 1].op == BlockOp::simd_integer &&
            jit_detail::accepts_fused_integer_simd(block.words[index],
                                                   block.words[index + 1])) {
            const auto fragment = jit_detail::emit_fused_integer_simd(
                block.words[index], block.words[index + 1]);
            out.bytes.insert(out.bytes.end(), fragment.begin(), fragment.end());
            ++index;
            continue;
        }
        returned |= emit_instruction(out, block.instructions[index],
            block.words[index], block.guest_pc + (index + 1) * 4);
    }
    if (!returned) {
        out.immediate(block.guest_pc + block.words.size() * 4, true);
        out.store_register(pc_offset);
    }
    out.byte(0xc3); // x64 ret
    return out.bytes;
}

// Entries contain sealed code addresses, never unordered-map node pointers.
// A cache belongs to one execution thread. Its private native memory lease
// cannot reenter the cache, so entries and all owning chunks remain stable
// until the generated frame returns. Every invalidation clears this table.
struct NativeChainEntry {
    std::uint64_t guest_pc{};
    std::uint64_t generation{};
    const std::uint8_t* address{};
    std::uint32_t flags{}; // bit0: stores, bit1: shared-capable owner.
    std::uint32_t reserved{};
};
constexpr std::size_t native_chain_entries = 4096;
static_assert(sizeof(NativeChainEntry) == 32);

struct NativeRegionContext {
    const NativeReadTable* table{};
    std::uint64_t budget{};
    std::uint64_t completed{};
    std::uint64_t miss_address{};
    std::uint32_t miss_width{};
    jit_detail::NativeFpScratch fp;
    const NativeWriteTable* write_table{};
    NativeWriteJournal* write_journal{};
    std::uint32_t miss_kind{}; // 1=read, 2=write, 3=journal capacity, 4=serialized.
    std::uint32_t requires_atomic_access{};
    const std::uint8_t* atomic_read_host{};
    const NativeChainEntry* chain_table{};
    std::uint64_t code_generation{};
    std::uint64_t chain_edges{};
    const std::uint8_t* chain_exit{};
    const std::uint8_t* frame_exit{};
};
static_assert(offsetof(NativeRegionContext, fp) == 40);
static_assert(offsetof(NativeRegionContext, write_table) == 72);
static_assert(offsetof(NativeRegionContext, atomic_read_host) < 128);
static_assert(offsetof(NativeRegionContext, chain_edges) < 128);
static_assert(offsetof(NativeRegionContext, chain_exit) == 128);
static_assert(offsetof(NativeRegionContext, frame_exit) == 136);

struct NativeRegionCode {
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> entries;
    bool stores{};
    bool parallel_scalar_capable{true};
    std::size_t retained_spans{};
};

unsigned native_load_width(const DecodedInstruction& instruction) {
    switch (instruction.op) {
    case BlockOp::load_byte:
    case BlockOp::load_signed_byte: return 1;
    case BlockOp::load_halfword:
    case BlockOp::load_signed_halfword: return 2;
    case BlockOp::load_word:
    case BlockOp::load_signed_word: return 4;
    case BlockOp::load_doubleword: return 8;
    case BlockOp::load_pair: return instruction.wide ? 16 : 8;
    case BlockOp::load_vector: return instruction.memory_bytes;
    case BlockOp::load_vector_pair: return instruction.memory_bytes * 2;
    default: return 0;
    }
}

unsigned native_store_width(const DecodedInstruction& instruction) {
    switch (instruction.op) {
    case BlockOp::store_byte: return 1;
    case BlockOp::store_halfword: return 2;
    case BlockOp::store_word: return 4;
    case BlockOp::store_doubleword: return 8;
    case BlockOp::store_pair: return instruction.wide ? 16 : 8;
    case BlockOp::store_vector: return instruction.memory_bytes;
    case BlockOp::store_vector_pair: return instruction.memory_bytes * 2;
    default: return 0;
    }
}

bool native_vector_memory(BlockOp op) {
    return op == BlockOp::load_vector || op == BlockOp::store_vector ||
           op == BlockOp::load_vector_pair || op == BlockOp::store_vector_pair;
}

bool native_nonfaulting_register_op(BlockOp op) {
    // This explicit list is the accounting contract: no checked memory,
    // runtime bailout, control transfer, helper or observable callback may
    // occur after a span's budget has been committed and before its bodies end.
    switch (op) {
    case BlockOp::nop:
    case BlockOp::move_zero:
    case BlockOp::move_inverse:
    case BlockOp::move_keep:
    case BlockOp::float_immediate:
    case BlockOp::float_register:
    case BlockOp::float_to_integer_bits:
    case BlockOp::integer_to_float_bits:
    case BlockOp::float_negate_bits:
    case BlockOp::float_absolute_bits:
    case BlockOp::address:
    case BlockOp::add_immediate:
    case BlockOp::subtract_immediate:
    case BlockOp::add_register:
    case BlockOp::subtract_register:
    case BlockOp::shift_register:
    case BlockOp::divide_unsigned:
    case BlockOp::divide_signed:
    case BlockOp::count_leading_zero:
    case BlockOp::reverse_bytes:
    case BlockOp::extract_registers:
    case BlockOp::add_extended:
    case BlockOp::subtract_extended:
    case BlockOp::logical_register:
    case BlockOp::logical_immediate:
    case BlockOp::unsigned_bitfield:
    case BlockOp::signed_bitfield:
    case BlockOp::merge_bitfield:
    case BlockOp::conditional_select:
    case BlockOp::conditional_compare_add:
    case BlockOp::conditional_compare_subtract:
    case BlockOp::multiply_add:
    case BlockOp::multiply_subtract:
    case BlockOp::widening_multiply_add:
    case BlockOp::widening_multiply_subtract:
    case BlockOp::multiply_high_signed:
    case BlockOp::multiply_high_unsigned:
    case BlockOp::simd_integer:
        return true;
    default:
        return false;
    }
}

bool retained_integer_op(const DecodedInstruction& instruction) {
    if (instruction.set_flags) return false;
    switch (instruction.op) {
    case BlockOp::nop:
    case BlockOp::move_zero:
    case BlockOp::move_inverse:
    case BlockOp::move_keep:
    case BlockOp::address:
    case BlockOp::add_immediate:
    case BlockOp::subtract_immediate:
    case BlockOp::add_register:
    case BlockOp::subtract_register:
    case BlockOp::logical_register:
    case BlockOp::logical_immediate:
        return true;
    default: return false;
    }
}

struct RetainedIntegerSpan {
    // Guest SP has key32; key31 is ZR and never has a host assignment.
    std::array<unsigned, 3> registers{};
    std::array<bool, 3> dirty{};
    unsigned size{};
    unsigned slot(unsigned guest) const {
        for (unsigned index = 0; index < size; ++index)
            if (registers[index] == guest) return index;
        throw std::logic_error("retained integer span has an unassigned operand");
    }
};

std::optional<RetainedIntegerSpan> plan_retained_integer_span(
    const DecodedBlock& block, std::size_t begin, std::size_t end) {
    RetainedIntegerSpan span;
    const auto add = [&](unsigned reg, bool dirty = false) {
        if (reg == 31) return true;
        for (unsigned slot = 0; slot < span.size; ++slot) {
            if (span.registers[slot] == reg) {
                span.dirty[slot] = span.dirty[slot] || dirty;
                return true;
            }
        }
        if (span.size == span.registers.size()) return false;
        span.registers[span.size] = reg;
        span.dirty[span.size++] = dirty;
        return true;
    };
    for (auto index = begin; index < end; ++index) {
        const auto& instruction = block.instructions[index];
        if (!retained_integer_op(instruction)) return std::nullopt;
        if (instruction.op == BlockOp::nop) continue;
        const bool stack = instruction.op == BlockOp::add_immediate ||
                           instruction.op == BlockOp::subtract_immediate;
        const bool stack_destination = stack || instruction.op == BlockOp::logical_immediate;
        const auto destination = instruction.destination == 31 && stack_destination
            ? 32U : instruction.destination;
        if (!add(destination, true)) return std::nullopt;
        if (instruction.op == BlockOp::move_zero ||
            instruction.op == BlockOp::move_inverse ||
            instruction.op == BlockOp::move_keep || instruction.op == BlockOp::address)
            continue;
        if (!add(instruction.source == 31 && stack ? 32U : instruction.source))
            return std::nullopt;
        if ((instruction.op == BlockOp::add_register ||
             instruction.op == BlockOp::subtract_register ||
             instruction.op == BlockOp::logical_register) && !add(instruction.second))
            return std::nullopt;
    }
    if (span.size == 0) return std::nullopt;
    return span;
}

void load_retained_integer_span(X64Emitter& out, const RetainedIntegerSpan& span) {
    for (unsigned slot = 0; slot < span.size; ++slot) {
        out.byte(0x4c); out.byte(0x8b);
        out.state_operand(static_cast<std::uint8_t>(slot << 3),
            span.registers[slot] == 32 ? sp_offset : x_offset(span.registers[slot]));
        // mov r8/r9/r10,[rcx+guest register]
    }
}

void flush_retained_integer_span(X64Emitter& out, const RetainedIntegerSpan& span) {
    for (unsigned slot = 0; slot < span.size; ++slot) {
        if (!span.dirty[slot]) continue;
        out.byte(0x4c); out.byte(0x89);
        out.state_operand(static_cast<std::uint8_t>(slot << 3),
            span.registers[slot] == 32 ? sp_offset : x_offset(span.registers[slot]));
    }
}

void emit_retained_integer(X64Emitter& out, const DecodedInstruction& instruction,
                           const RetainedIntegerSpan& span) {
    if (instruction.op == BlockOp::nop) return;
    const bool stack = instruction.op == BlockOp::add_immediate ||
                       instruction.op == BlockOp::subtract_immediate;
    const bool stack_destination = stack || instruction.op == BlockOp::logical_immediate;
    const auto destination = instruction.destination == 31 && stack_destination
        ? 32U : instruction.destination;
    if (destination == 31) return;
    const auto dst = span.slot(destination);
    const auto immediate_to_destination = [&](std::uint64_t value) {
        const bool wide = instruction.wide || instruction.op == BlockOp::address;
        out.byte(wide ? 0x49 : 0x41);
        out.byte(static_cast<std::uint8_t>(0xb8U + dst));
        if (wide) out.wide_word(value);
        else out.word(static_cast<std::uint32_t>(value));
    };
    if (instruction.op == BlockOp::move_zero || instruction.op == BlockOp::move_inverse ||
        instruction.op == BlockOp::address) {
        immediate_to_destination(instruction.immediate);
        return;
    }
    if (instruction.op == BlockOp::move_keep) {
        out.immediate(~(0xffffULL << instruction.shift), instruction.wide);
        out.byte(instruction.wide ? 0x49 : 0x41); out.byte(0x21);
        out.byte(static_cast<std::uint8_t>(0xc0U | dst));
        out.immediate(instruction.immediate, instruction.wide);
        out.byte(instruction.wide ? 0x49 : 0x41); out.byte(0x09);
        out.byte(static_cast<std::uint8_t>(0xc0U | dst));
        return;
    }
    const bool register_operand = instruction.op == BlockOp::add_register ||
        instruction.op == BlockOp::subtract_register ||
        instruction.op == BlockOp::logical_register;
    if (register_operand) {
        // Snapshot Rm before writing Rd: Rd may alias Rm while Rn differs.
        if (instruction.second == 31) { out.byte(0x31); out.byte(0xc0); }
        else {
            out.byte(instruction.wide ? 0x4c : 0x44); out.byte(0x89);
            out.byte(static_cast<std::uint8_t>(0xc0U | (span.slot(instruction.second) << 3)));
        }
        if (instruction.shift != 0) {
            if (instruction.wide) out.byte(0x48);
            out.byte(0xc1);
            out.byte(instruction.shift_type == 0 ? 0xe0
                : instruction.shift_type == 1 ? 0xe8
                : instruction.shift_type == 2 ? 0xf8 : 0xc8);
            out.byte(instruction.shift);
        }
        if (instruction.invert_second) {
            if (instruction.wide) out.byte(0x48);
            out.byte(0xf7); out.byte(0xd0);
        }
    } else if (instruction.op == BlockOp::logical_immediate) {
        out.immediate(instruction.immediate, instruction.wide);
    }
    const auto source = instruction.source == 31 && stack ? 32U : instruction.source;
    if (source == 31) immediate_to_destination(0);
    else {
        out.byte(instruction.wide ? 0x4d : 0x45); out.byte(0x89);
        out.byte(static_cast<std::uint8_t>(0xc0U | (span.slot(source) << 3) | dst));
    }
    if (stack) {
        out.byte(instruction.wide ? 0x49 : 0x41); out.byte(0x81);
        out.byte(static_cast<std::uint8_t>((instruction.op == BlockOp::add_immediate
            ? 0xc0U : 0xe8U) | dst));
        out.word(static_cast<std::uint32_t>(instruction.immediate));
    } else {
        out.byte(instruction.wide ? 0x49 : 0x41);
        out.byte(instruction.op == BlockOp::add_register ? 0x01
            : instruction.op == BlockOp::subtract_register ? 0x29
            : instruction.logical_operation == 1 ? 0x09
            : instruction.logical_operation == 2 ? 0x31 : 0x21);
        out.byte(static_cast<std::uint8_t>(0xc0U | dst));
    }
}

void commit_native_vector(X64Emitter& out, const DecodedInstruction& instruction) {
    const bool load = instruction.op == BlockOp::load_vector ||
                      instruction.op == BlockOp::load_vector_pair;
    const bool pair = instruction.op == BlockOp::load_vector_pair ||
                      instruction.op == BlockOp::store_vector_pair;
    if (load) {
        if (instruction.memory_bytes == 16) {
            out.byte(0xf3); out.byte(0x0f); out.byte(0x7f); out.byte(0x81);
            out.word(v_offset(instruction.destination)); // movdqu [rcx+Vd],xmm0
            if (pair) {
                out.byte(0xf3); out.byte(0x0f); out.byte(0x7f); out.byte(0x89);
                out.word(v_offset(instruction.second));
            }
        } else {
            out.byte(instruction.memory_bytes == 8 ? 0x48 : 0x40);
            out.byte(0x89); out.byte(0xd0); // mov rax/eax,rdx/edx
            out.store_register(v_offset(instruction.destination));
            out.byte(0x31); out.byte(0xc0);
            out.store_register(v_offset(instruction.destination, 1));
            if (pair) {
                out.byte(instruction.memory_bytes == 8 ? 0x4c : 0x44);
                out.byte(0x89); out.byte(0xc0); // mov rax/eax,r8/r8d
                out.store_register(v_offset(instruction.second));
                out.byte(0x31); out.byte(0xc0);
                out.store_register(v_offset(instruction.second, 1));
            }
        }
    }
    if (instruction.memory_writeback) {
        out.load_register(instruction.source == 31 ? sp_offset
            : x_offset(instruction.source), true);
        out.byte(0x48); out.byte(0x8d); out.byte(0x80);
        out.word(static_cast<std::uint32_t>(instruction.immediate));
        out.store_register(instruction.source == 31 ? sp_offset
            : x_offset(instruction.source));
    }
}

struct NativeExitTrampolines {
    std::vector<std::uint8_t> bytes;
    std::size_t frame_exit{};
};

NativeExitTrampolines emit_native_exit_trampolines() {
    X64Emitter out;
    const auto jump = [&out](std::uint8_t condition) {
        out.byte(0x0f); out.byte(condition);
        const auto displacement = out.bytes.size();
        out.word(0);
        return displacement;
    };
    std::vector<std::size_t> returns;
    // Successful edges preserve RCX/R11, the live FP session and journal.
    // Budget/guard/fault exits jump directly to frame_exit, bypassing lookup.
    out.byte(0x49); out.byte(0x8b); out.byte(0x43);
    out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, completed)));
    out.byte(0x49); out.byte(0x3b); out.byte(0x43);
    out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, budget)));
    returns.push_back(jump(0x83));
    out.load_register(pc_offset, true);
    out.byte(0x49); out.byte(0x89); out.byte(0xc2);
    out.byte(0x49); out.byte(0xc1); out.byte(0xea); out.byte(2);
    out.byte(0x41); out.byte(0x81); out.byte(0xe2);
    out.word(static_cast<std::uint32_t>(native_chain_entries - 1));
    out.byte(0x49); out.byte(0xc1); out.byte(0xe2); out.byte(5);
    out.byte(0x4d); out.byte(0x8b); out.byte(0x4b);
    out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, chain_table)));
    out.byte(0x4d); out.byte(0x01); out.byte(0xd1);
    out.byte(0x49); out.byte(0x3b); out.byte(0x01);
    returns.push_back(jump(0x85));
    out.byte(0x4d); out.byte(0x8b); out.byte(0x43);
    out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, code_generation)));
    out.byte(0x4d); out.byte(0x3b); out.byte(0x41); out.byte(8);
    returns.push_back(jump(0x85));
    out.byte(0x49); out.byte(0x8b); out.byte(0x51); out.byte(16);
    out.byte(0x48); out.byte(0x85); out.byte(0xd2);
    returns.push_back(jump(0x84));
    out.byte(0x41); out.byte(0x83); out.byte(0x7b);
    out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, requires_atomic_access)));
    out.byte(0);
    const auto exclusive_chain = jump(0x84);
    out.byte(0x41); out.byte(0xf6); out.byte(0x41); out.byte(24); out.byte(2);
    returns.push_back(jump(0x84));
    out.patch_relative(exclusive_chain, out.bytes.size());
    out.byte(0x41); out.byte(0xf6); out.byte(0x41); out.byte(24); out.byte(1);
    const auto read_only_chain = jump(0x84);
    out.byte(0x49); out.byte(0x83); out.byte(0x7b);
    out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, write_journal)));
    out.byte(0);
    returns.push_back(jump(0x84));
    out.patch_relative(read_only_chain, out.bytes.size());
    out.byte(0x49); out.byte(0xff); out.byte(0x43);
    out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, chain_edges)));
    out.byte(0xff); out.byte(0xe2);
    const auto frame_exit = out.bytes.size();
    for (const auto displacement : returns) out.patch_relative(displacement, frame_exit);
    // Every true frame exit must handle a session inherited from any owner,
    // including owners without FP. Keep this immutable copy outside the arena
    // so invalidation/reclamation never drops an active frame's exit target.
    const auto finalizer = jit_detail::emit_fp_session_finish(
        static_cast<std::uint32_t>(offsetof(NativeRegionContext, fp)));
    out.bytes.insert(out.bytes.end(), finalizer.begin(), finalizer.end());
    out.byte(0xc3);
    return {std::move(out.bytes), frame_exit};
}

// All generated regions remain leaf functions: RCX is CpuState and R11 is a
// borrowed invocation context. Only volatile GPRs/XMMs are used, and no host
// pointer can be dereferenced until the complete lease entry has been checked.
NativeRegionCode emit_native_region(const DecodedBlock& region) {
    X64Emitter out;
    NativeRegionCode result;
    const auto count = region.instructions.size();
    std::vector<std::size_t> labels(count);
    std::vector<std::size_t> flow_labels(count);
    std::vector<std::size_t> span_ends(count);
    std::vector<std::optional<RetainedIntegerSpan>> retained(count);
    for (std::size_t begin = 0; begin < count;) {
        if (!native_nonfaulting_register_op(region.instructions[begin].op)) {
            ++begin;
            continue;
        }
        auto end = begin + 1;
        const bool retain = retained_integer_op(region.instructions[begin]);
        while (end < count && end - begin < 8 &&
               native_nonfaulting_register_op(region.instructions[end].op) &&
               retained_integer_op(region.instructions[end]) == retain) {
            if (retain && !plan_retained_integer_span(region, begin, end + 1)) break;
            ++end;
        }
        if (end - begin >= 2) {
            const auto plan = retain ? plan_retained_integer_span(region, begin, end)
                                     : std::nullopt;
            if (plan) ++result.retained_spans;
            for (auto index = begin; index < end; ++index) {
                span_ends[index] = end;
                retained[index] = plan;
            }
        }
        begin = end;
    }
    std::vector<std::size_t> entry_jumps;
    std::vector<std::pair<std::size_t, std::size_t>> internal_jumps;
    std::vector<std::pair<std::size_t, std::size_t>> flow_jumps;
    struct Exit {
        std::size_t jump;
        std::uint64_t pc;
        unsigned width;
        unsigned kind{1};
        bool chainable{};
    };
    std::vector<Exit> exits;
    const auto jump = [&out](std::uint8_t condition = 0) {
        if (condition == 0) out.byte(0xe9);
        else { out.byte(0x0f); out.byte(condition); }
        const auto displacement = out.bytes.size();
        out.word(0);
        return displacement;
    };
    std::vector<std::size_t> normal_exits;
    std::vector<std::size_t> frame_exits;
    const auto target_jump = [&](std::uint64_t pc, std::uint8_t condition = 0) {
        const auto displacement = jump(condition);
        if (pc >= region.guest_pc && pc - region.guest_pc < count * 4 &&
            ((pc - region.guest_pc) & 3U) == 0) {
            internal_jumps.emplace_back(displacement,
                static_cast<std::size_t>((pc - region.guest_pc) / 4));
        } else {
            exits.push_back({displacement, pc, 0, 1, true});
        }
    };
    const auto completed = [&out] {
        out.byte(0x49); out.byte(0xff); out.byte(0x43);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, completed)));
        // inc qword ptr [r11 + completed]
    };
    const auto alignment_guard = [&](std::uint64_t pc, unsigned width,
                                     bool host, bool shared_only) {
        if (width == 1) return;
        std::optional<std::size_t> exclusive;
        if (shared_only) {
            out.byte(0x41); out.byte(0x83); out.byte(0x7b);
            out.byte(static_cast<std::uint8_t>(offsetof(
                NativeRegionContext, requires_atomic_access)));
            out.byte(0);
            exclusive = jump(0x84);
        }
        if (host) {
            out.byte(0xf6); out.byte(0xc2); // test dl, width - 1
        } else {
            out.byte(0xa8); // test al, width - 1
        }
        out.byte(static_cast<std::uint8_t>(width - 1));
        exits.push_back({jump(0x85), pc, shared_only ? width : 0,
                         shared_only ? 4U : 1U});
        if (exclusive) out.patch_relative(*exclusive, out.bytes.size());
    };
    const auto atomic_alignment_guard = [&](std::uint64_t pc, unsigned width,
                                            bool host) {
        if (width == 1) return;
        if (host) {
            out.byte(0xf6); out.byte(0xc2); // test dl, width - 1
        } else {
            out.byte(0xa8); // test al, width - 1
        }
        out.byte(static_cast<std::uint8_t>(width - 1));
        const auto aligned = jump(0x84);
        out.byte(0x41); out.byte(0x83); out.byte(0x7b);
        out.byte(static_cast<std::uint8_t>(offsetof(
            NativeRegionContext, requires_atomic_access)));
        out.byte(0);
        exits.push_back({jump(0x85), pc, width, 4});
        // Atomic alignment is mandatory even under exclusive ownership.
        // Shared failure first promotes, preserving the completed prefix;
        // the exclusive attempt then uses the checked instruction semantics.
        exits.push_back({jump(), pc, 0});
        out.patch_relative(aligned, out.bytes.size());
    };
    const auto span_guard = [&](std::uint64_t pc, unsigned width, bool write) {
        const auto kind = write ? 2U : 1U;
        if (write) {
            out.byte(0x4d); out.byte(0x8b); out.byte(0x4b);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, write_table)));
        } else {
            out.byte(0x4d); out.byte(0x8b); out.byte(0x0b); // mov r9,[r11]
        }
        // Snapshot the table epoch while R9 still names the table. Keep the
        // indexing scratch in RDX rather than copying it through R10.
        out.byte(0x4d); out.byte(0x8b); out.byte(0x01); // mov r8,[r9]
        out.byte(0x48); out.byte(0x89); out.byte(0xc2); // mov rdx,rax
        out.byte(0x48); out.byte(0xc1); out.byte(0xea); out.byte(12);
        constexpr auto read_entries = NativeReadTable::entry_count;
        constexpr auto write_entries = NativeWriteTable::entry_count;
        static_assert((read_entries & (read_entries - 1)) == 0 &&
                      (write_entries & (write_entries - 1)) == 0);
        const auto mask = static_cast<std::uint32_t>(
            (write ? write_entries : read_entries) - 1);
        if (mask <= 127) {
            out.byte(0x83); out.byte(0xe2); out.byte(static_cast<std::uint8_t>(mask));
        } else {
            out.byte(0x81); out.byte(0xe2); out.word(mask);
        }
        static_assert(sizeof(NativeReadEntry) == 40);
        static_assert(sizeof(NativeWriteEntry) == 48);
        static_assert(offsetof(NativeReadTable, entries) == 8);
        static_assert(offsetof(NativeWriteTable, entries) == 8);
        static_assert(offsetof(NativeReadEntry, topology_epoch) ==
                      offsetof(NativeWriteEntry, topology_epoch));
        static_assert(offsetof(NativeReadEntry, guest_begin) ==
                      offsetof(NativeWriteEntry, guest_begin));
        static_assert(offsetof(NativeReadEntry, guest_size) ==
                      offsetof(NativeWriteEntry, guest_size));
        static_assert(offsetof(NativeReadEntry, host_begin) ==
                      offsetof(NativeWriteEntry, host_begin));
        out.byte(0x48); out.byte(0x6b); out.byte(0xd2); out.byte(write ? 48 : 40);
        out.byte(0x4d); out.byte(0x8d); out.byte(0x4c); out.byte(0x11); out.byte(8);
        // The fill-page tag is an indexing hint. Epoch and complete span
        // bounds prove ownership before the host pointer is resolved.
        out.byte(0x4d); out.byte(0x39); out.byte(0x41);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeReadEntry, topology_epoch)));
        exits.push_back({jump(0x85), pc, width, kind});
        out.byte(0x49); out.byte(0x8b); out.byte(0x51);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeReadEntry, guest_size)));
        out.byte(0x48); out.byte(0x83); out.byte(0xea); out.byte(static_cast<std::uint8_t>(width));
        exits.push_back({jump(0x82), pc, width, kind});
        out.byte(0x49); out.byte(0x89); out.byte(0xc0);
        out.byte(0x4d); out.byte(0x2b); out.byte(0x41);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeReadEntry, guest_begin)));
        // Every cached extent is nonwrapping. An address below guest_begin
        // wraps this unsigned offset above guest_size-width, so the same
        // comparison proves both bounds without a separate lower-bound test.
        out.byte(0x49); out.byte(0x39); out.byte(0xd0);
        exits.push_back({jump(0x87), pc, width, kind});
        out.byte(0x49); out.byte(0x8b); out.byte(0x51);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeReadEntry, host_begin)));
        out.byte(0x4c); out.byte(0x01); out.byte(0xc2);
        // RAX remains the guest address, RDX is the validated host pointer,
        // and R9 retains the selected entry for write journal staging.
    };
    const auto prepare_span = [&](std::size_t index) {
        const auto steps = static_cast<std::uint8_t>(span_ends[index] - index);
        out.byte(0x49); out.byte(0x8b); out.byte(0x43);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, budget)));
        out.byte(0x49); out.byte(0x2b); out.byte(0x43);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, completed)));
        out.byte(0x48); out.byte(0x83); out.byte(0xf8); out.byte(steps);
        exits.push_back({jump(0x82), region.guest_pc + index * 4, 0});
        out.byte(0x49); out.byte(0x83); out.byte(0x43);
        out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, completed)));
        out.byte(steps);
        if (retained[index]) load_retained_integer_span(out, *retained[index]);
        // The complete synchronous span is now budgeted. Its ordinary bodies
        // are emitted once and have no per-instruction counter loads/stores.
    };
    // Alias thunks check the complete pure suffix before entering its bodies.
    // Internal targets skip only MOV R11,RDX, retaining the live context.
    for (std::size_t index = 0; index < count; ++index) {
        result.entries.push_back(out.bytes.size());
        out.byte(0x49); out.byte(0x89); out.byte(0xd3); // mov r11, rdx
        if (span_ends[index] != 0) prepare_span(index);
        entry_jumps.push_back(jump());
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& instruction = region.instructions[index];
        const auto pc = region.guest_pc + index * 4;
        flow_labels[index] = out.bytes.size();
        const bool grouped = span_ends[index] != 0;
        if (grouped && (index == 0 || span_ends[index - 1] != span_ends[index]))
            prepare_span(index);
        labels[index] = out.bytes.size();
        if (!grouped) {
            out.byte(0x49); out.byte(0x8b); out.byte(0x43);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, completed)));
            out.byte(0x49); out.byte(0x3b); out.byte(0x43);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, budget)));
            exits.push_back({jump(0x83), pc, 0}); // jae precise budget exit
        }
        if (retained[index]) {
            emit_retained_integer(out, instruction, *retained[index]);
            if (index + 1 == span_ends[index])
                flush_retained_integer_span(out, *retained[index]);
            continue;
        }

        if (index + 1 < count && instruction.op == BlockOp::simd_integer &&
            region.instructions[index + 1].op == BlockOp::simd_integer &&
            ((!grouped && span_ends[index + 1] == 0) ||
             (grouped && span_ends[index] == span_ends[index + 1])) &&
            jit_detail::accepts_fused_integer_simd(region.words[index],
                                                   region.words[index + 1])) {
            // A grouped span already reserved both steps. Ungrouped pairs
            // retain their ordinary first body for a one-step budget. Every
            // second-instruction alias still has its independent suffix check.
            std::optional<std::size_t> ordinary;
            if (!grouped) {
                out.byte(0x49); out.byte(0x8b); out.byte(0x53);
                out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, budget)));
                out.byte(0x48); out.byte(0x29); out.byte(0xc2); // sub rdx,rax
                out.byte(0x48); out.byte(0x83); out.byte(0xfa); out.byte(2);
                ordinary = jump(0x82); // jb fewer than two steps
            }
            const auto fragment = jit_detail::emit_fused_integer_simd(
                region.words[index], region.words[index + 1]);
            out.bytes.insert(out.bytes.end(), fragment.begin(), fragment.end());
            if (grouped) {
                if (index + 2 < count) flow_jumps.emplace_back(jump(), index + 2);
                else target_jump(pc + 8);
                continue;
            }
            out.byte(0x49); out.byte(0x83); out.byte(0x43);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, completed)));
            out.byte(2);
            target_jump(pc + 8);
            out.patch_relative(*ordinary, out.bytes.size());
        }

        if (instruction.op == BlockOp::atomic_lse) {
            const auto atomic_width = instruction.memory_bytes;
            result.stores = true;
            out.load_register(instruction.source == 31 ? sp_offset
                : x_offset(instruction.source), true);
            atomic_alignment_guard(pc, atomic_width, false);
            out.byte(0x4d); out.byte(0x8b); out.byte(0x53);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, write_journal)));
            out.byte(0x4d); out.byte(0x8b); out.byte(0x02);
            out.byte(0x49); out.byte(0x83); out.byte(0xf8);
            out.byte(static_cast<std::uint8_t>(NativeWriteJournal::capacity - 1));
            exits.push_back({jump(0x87), pc, atomic_width, 3});
            span_guard(pc, atomic_width, false);
            out.byte(0x49); out.byte(0x89); out.byte(0x53);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, atomic_read_host)));
            span_guard(pc, atomic_width, true);
            atomic_alignment_guard(pc, atomic_width, true);
            out.byte(0x49); out.byte(0x3b); out.byte(0x53);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, atomic_read_host)));
            exits.push_back({jump(0x85), pc, 0});

            // Stage one unpublished record while RAX still holds the original
            // guest address. The fragment can overwrite Rs/Rn aliases and
            // every volatile scratch register except RCX/RDX/R11.
            out.byte(0x4d); out.byte(0x8b); out.byte(0x53);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, write_journal)));
            out.byte(0x4d); out.byte(0x8b); out.byte(0x02); // mov r8,[r10]
            out.byte(0x4d); out.byte(0x6b); out.byte(0xc0); out.byte(24);
            out.byte(0x4f); out.byte(0x8d); out.byte(0x44); out.byte(0x02); out.byte(8);
            out.byte(0x49); out.byte(0x89); out.byte(0x00); // record.guest_address
            out.byte(0x4d); out.byte(0x8b); out.byte(0x51);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeWriteEntry, region_token)));
            out.byte(0x4d); out.byte(0x89); out.byte(0x50); out.byte(8);
            out.byte(0x41); out.byte(0xc7); out.byte(0x40); out.byte(16);
            out.word(atomic_width);
            out.byte(0x41); out.byte(0xc7); out.byte(0x40); out.byte(20);
            out.word(0);
            const auto fragment = jit_detail::emit_native_lse(region.words[index]);
            out.bytes.insert(out.bytes.end(), fragment.begin(), fragment.end());
            out.byte(0x45); out.byte(0x85); out.byte(0xc9); // test r9d,r9d
            const auto no_write = jump(0x84);
            out.byte(0x4d); out.byte(0x8b); out.byte(0x53);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, write_journal)));
            out.byte(0x49); out.byte(0xff); out.byte(0x02); // publish only a real write
            out.patch_relative(no_write, out.bytes.size());
            completed();
            continue;
        }

        const auto store_width = native_store_width(instruction);
        const auto width = store_width != 0 ? store_width : native_load_width(instruction);
        const bool vector_memory = native_vector_memory(instruction.op);
        const bool pair_memory = instruction.op == BlockOp::load_pair ||
            instruction.op == BlockOp::store_pair ||
            instruction.op == BlockOp::load_vector_pair ||
            instruction.op == BlockOp::store_vector_pair;
        if (width != 0) {
            if (store_width != 0) result.stores = true;
            // Baseline A64 pairs are atomic per aligned element. Arm ARM L.b
            // known issue C23997 retains Q's two atomic 8-byte accesses when
            // aligned to 8 bytes. Use explicit host m64 operations below;
            // FEAT_LSE2 is not advertised.
            const auto element_width = pair_memory ? width / 2 : width;
            const auto atomic_width = std::min(element_width, 8U);
            // Q owners stay exclusive: shared Q admission regressed the
            // measured Blender render.
            // Retain the explicit m64 guest operations in either policy.
            const bool parallel_memory = element_width <= 8;
            if (!parallel_memory) result.parallel_scalar_capable = false;
            if (instruction.memory_literal) {
                out.immediate(instruction.immediate, true);
            } else {
                out.load_register(instruction.source == 31 ? sp_offset
                    : x_offset(instruction.source), true);
            }
            if (instruction.memory_register_offset) {
                if (instruction.second == 31) {
                    out.byte(0x31); out.byte(0xd2);
                } else {
                    out.load_second(x_offset(instruction.second),
                        instruction.extend_option == 3 ||
                        instruction.extend_option == 7);
                }
                if (instruction.extend_option == 6) {
                    out.byte(0x48); out.byte(0x63); out.byte(0xd2);
                    // movsxd rdx, edx
                }
                if (instruction.memory_scaled && instruction.shift != 0) {
                    out.byte(0x48); out.byte(0xc1); out.byte(0xe2);
                    out.byte(instruction.shift);
                }
                out.byte(0x48); out.byte(0x01); out.byte(0xd0);
            } else if (!instruction.memory_literal &&
                       (!instruction.memory_writeback || instruction.memory_preindex)) {
                out.byte(0x48); out.byte(0x05);
                out.word(static_cast<std::uint32_t>(instruction.immediate));
            }
            // RAX retains the exact fault address until every check passes.
            if (parallel_memory)
                alignment_guard(pc, atomic_width, false, true);
            if (store_width != 0) {
                out.byte(0x4d); out.byte(0x8b); out.byte(0x53);
                out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, write_journal)));
                out.byte(0x4d); out.byte(0x8b); out.byte(0x02); // mov r8,[r10]
                out.byte(0x49); out.byte(0x83); out.byte(0xf8);
                out.byte(static_cast<std::uint8_t>(NativeWriteJournal::capacity -
                    (pair_memory ? 2 : 1)));
                exits.push_back({jump(0x87), pc, width, 3});
            }
            span_guard(pc, width, store_width != 0);
            if (parallel_memory)
                alignment_guard(pc, atomic_width, true, true);
            if (store_width != 0) {
                // Retain the guest address for the commit record before
                // snapshotting operands. The pointer in R10 is already proven
                // to be one complete writable non-executable owned span.
                out.byte(0x49); out.byte(0x89); out.byte(0x43);
                out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, miss_address)));
                out.byte(0x49); out.byte(0x89); out.byte(0xd2); // mov r10,rdx
                if (!vector_memory && instruction.destination == 31) {
                    out.byte(0x31); out.byte(0xd2);
                } else {
                    out.load_second(vector_memory ? v_offset(instruction.destination)
                        : x_offset(instruction.destination), true);
                }
                const bool pair = pair_memory;
                if (pair) {
                    if (!vector_memory && instruction.second == 31) {
                        out.byte(0x45); out.byte(0x31); out.byte(0xc0);
                    } else {
                        out.byte(0x4c); out.byte(0x8b); out.byte(0x81);
                        out.word(vector_memory ? v_offset(instruction.second)
                            : x_offset(instruction.second));
                    }
                }
                if (element_width == 16) {
                    // CpuState is private to this engine. Snapshot all Q
                    // operands before any guest write, including aliases.
                    out.byte(0xf3); out.byte(0x0f); out.byte(0x6f); out.byte(0x81);
                    out.word(v_offset(instruction.destination));
                    if (pair) {
                        out.byte(0xf3); out.byte(0x0f); out.byte(0x6f); out.byte(0x89);
                        out.word(v_offset(instruction.second));
                    }
                    out.byte(0x66); out.byte(0x41); out.byte(0x0f); out.byte(0xd6);
                    out.byte(0x02); // movq [r10],xmm0
                    out.byte(0x41); out.byte(0x0f); out.byte(0x17);
                    out.byte(0x42); out.byte(8); // movhps [r10+8],xmm0
                    if (pair) {
                        out.byte(0x66); out.byte(0x41); out.byte(0x0f); out.byte(0xd6);
                        out.byte(0x4a); out.byte(16);
                        out.byte(0x41); out.byte(0x0f); out.byte(0x17);
                        out.byte(0x4a); out.byte(24); // movhps [r10+24],xmm1
                    }
                } else {
                    if (element_width == 2) out.byte(0x66);
                    out.byte(element_width == 8 ? 0x49 : 0x41);
                    out.byte(element_width == 1 ? 0x88 : 0x89);
                    out.byte(0x12); // mov [r10],dl/dx/edx/rdx
                    if (pair) {
                        out.byte(instruction.wide ? 0x4d : 0x45);
                        out.byte(0x89); out.byte(0x42);
                        out.byte(static_cast<std::uint8_t>(element_width));
                    }
                }
                static_assert(sizeof(NativeWriteRecord) == 24);
                static_assert(offsetof(NativeWriteJournal, entries) == 8);
                out.byte(0x4d); out.byte(0x8b); out.byte(0x53);
                out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, write_journal)));
                out.byte(0x49); out.byte(0x8b); out.byte(0x02); // mov rax,[r10]
                out.byte(0x48); out.byte(0x6b); out.byte(0xd0); out.byte(24);
                out.byte(0x49); out.byte(0x8d); out.byte(0x54); out.byte(0x12);
                out.byte(8); // lea rdx,[r10 + rdx + entries]
                out.byte(0x4d); out.byte(0x8b); out.byte(0x43);
                out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, miss_address)));
                out.byte(0x4d); out.byte(0x8b); out.byte(0x49);
                out.byte(static_cast<std::uint8_t>(offsetof(NativeWriteEntry, region_token)));
                for (unsigned record = 0; record != (pair ? 2U : 1U); ++record) {
                    const auto offset = static_cast<std::uint8_t>(record * 24);
                    out.byte(0x4c); out.byte(0x89); out.byte(0x42); out.byte(offset);
                    out.byte(0x4c); out.byte(0x89); out.byte(0x4a);
                    out.byte(static_cast<std::uint8_t>(offset + 8));
                    out.byte(0xc7); out.byte(0x42);
                    out.byte(static_cast<std::uint8_t>(offset + 16));
                    out.word(element_width);
                    out.byte(0xc7); out.byte(0x42);
                    out.byte(static_cast<std::uint8_t>(offset + 20));
                    out.word(0);
                    if (pair && record == 0) {
                        out.byte(0x49); out.byte(0x83); out.byte(0xc0);
                        out.byte(static_cast<std::uint8_t>(element_width));
                    }
                }
                out.byte(0x48); out.byte(0x83); out.byte(0xc0); out.byte(pair ? 2 : 1);
                out.byte(0x49); out.byte(0x89); out.byte(0x02); // publish journal count
            } else if (vector_memory && instruction.memory_bytes == 16) {
                // The complete 16/32-byte span is proven before these m64
                // accesses. Snapshot both Qs before register/base writeback.
                out.byte(0xf3); out.byte(0x0f); out.byte(0x7e); out.byte(0x02);
                out.byte(0x0f); out.byte(0x16); out.byte(0x42); out.byte(8);
                if (pair_memory) {
                    out.byte(0xf3); out.byte(0x0f); out.byte(0x7e); out.byte(0x4a);
                    out.byte(16);
                    out.byte(0x0f); out.byte(0x16); out.byte(0x4a); out.byte(24);
                }
            } else if (pair_memory) {
                out.byte(instruction.wide ? 0x4c : 0x44);
                out.byte(0x8b); out.byte(0x42);
                out.byte(instruction.wide ? 8 : 4); // snapshot second load
                if (instruction.wide) out.byte(0x48);
                out.byte(0x8b); out.byte(0x12); // snapshot first load
            } else if (width == 1 || width == 2) {
                out.byte(0x0f); out.byte(width == 1 ? 0xb6 : 0xb7);
                out.byte(0x12); // movzx edx, byte/word ptr [rdx]
            } else {
                if (width == 8) out.byte(0x48);
                out.byte(0x8b); out.byte(0x12);
            }
            if (vector_memory) commit_native_vector(out, instruction);
            else (void)emit_instruction(out, instruction, region.words[index], pc + 4);
            completed();
            continue;
        }
        if (instruction.op == BlockOp::guarded_fp) {
            const auto fragment = jit_detail::emit_guarded_fp(region.words[index],
                static_cast<std::uint32_t>(offsetof(NativeRegionContext, fp)),
                jit_detail::NativeFpEmissionMode::region_session);
            const auto begin = out.bytes.size();
            out.bytes.insert(out.bytes.end(), fragment.bytes.begin(), fragment.bytes.end());
            for (const auto displacement : fragment.bailout_displacements)
                exits.push_back({begin + displacement, pc, 0});
            completed();
            continue;
        }
        switch (instruction.op) {
        case BlockOp::branch:
        case BlockOp::branch_link:
            if (instruction.op == BlockOp::branch_link) {
                out.immediate(pc + 4, true);
                out.store_register(x_offset(30));
            }
            completed();
            target_jump(instruction.immediate);
            break;
        case BlockOp::compare_branch_zero:
        case BlockOp::compare_branch_nonzero:
        case BlockOp::test_branch_zero:
        case BlockOp::test_branch_nonzero: {
            completed();
            if (instruction.source == 31) {
                out.byte(0x31); out.byte(0xc0);
            } else {
                out.load_register(x_offset(instruction.source), instruction.wide);
            }
            const bool bit_test = instruction.op == BlockOp::test_branch_zero ||
                                  instruction.op == BlockOp::test_branch_nonzero;
            if (bit_test) {
                out.byte(0x48); out.byte(0xba);
                out.wide_word(1ULL << instruction.shift);
                out.byte(0x48); out.byte(0x85); out.byte(0xd0);
            } else {
                if (instruction.wide) out.byte(0x48);
                out.byte(0x85); out.byte(0xc0);
            }
            const bool zero = instruction.op == BlockOp::compare_branch_zero ||
                              instruction.op == BlockOp::test_branch_zero;
            target_jump(instruction.immediate, zero ? 0x84 : 0x85);
            break;
        }
        case BlockOp::conditional_branch:
            completed();
            out.byte(0x8b); out.byte(0x91);
            out.word(static_cast<std::uint32_t>(offsetof(CpuState, nzcv)));
            out.byte(0xc1); out.byte(0xea); out.byte(28);
            out.byte(0x41); out.byte(0xb8);
            out.word(branch_truth_table(instruction.condition));
            out.byte(0x41); out.byte(0x0f); out.byte(0xa3); out.byte(0xd0);
            target_jump(instruction.immediate, 0x82); // jc condition true
            break;
        default:
            if (emit_instruction(out, instruction, region.words[index], pc + 4)) {
                completed();
                normal_exits.push_back(jump());
            } else {
                if (!grouped) completed();
            }
            break;
        }
    }
    const auto final_op = region.instructions.back().op;
    const bool terminal = final_op == BlockOp::ret ||
        final_op == BlockOp::branch_register || final_op == BlockOp::branch_link_register ||
        final_op == BlockOp::branch || final_op == BlockOp::branch_link;
    if (!terminal) {
        out.immediate(region.guest_pc + count * 4, true);
        out.store_register(pc_offset);
        normal_exits.push_back(jump());
    }
    std::map<std::tuple<std::uint64_t, unsigned, unsigned, bool>, std::size_t> exit_labels;
    for (const auto& exit : exits) {
        const auto identity = std::tuple{exit.pc, exit.width, exit.kind, exit.chainable};
        if (const auto found = exit_labels.find(identity); found != exit_labels.end()) {
            out.patch_relative(exit.jump, found->second);
            continue;
        }
        exit_labels.emplace(identity, out.bytes.size());
        out.patch_relative(exit.jump, out.bytes.size());
        if (exit.width != 0) {
            out.byte(0x49); out.byte(0x89); out.byte(0x43);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, miss_address)));
            out.byte(0x41); out.byte(0xc7); out.byte(0x43);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, miss_width)));
            out.word(exit.width);
            out.byte(0x41); out.byte(0xc7); out.byte(0x43);
            out.byte(static_cast<std::uint8_t>(offsetof(NativeRegionContext, miss_kind)));
            out.word(exit.kind);
        }
        out.immediate(exit.pc, true);
        out.store_register(pc_offset);
        if (exit.chainable) normal_exits.push_back(jump());
        else frame_exits.push_back(jump());
    }
    // Compact leaves jump to one immutable exit pair per cache. No owner
    // embeds a copy of the lookup/finalizer, and precise zero-progress exits
    // retain their separate route to prevent retry loops.
    const auto chain_exit = out.bytes.size();
    for (const auto displacement : normal_exits)
        out.patch_relative(displacement, chain_exit);
    out.byte(0x41); out.byte(0xff); out.byte(0xa3);
    out.word(static_cast<std::uint32_t>(offsetof(NativeRegionContext, chain_exit)));
    const auto returned = out.bytes.size();
    for (const auto displacement : frame_exits) out.patch_relative(displacement, returned);
    out.byte(0x41); out.byte(0xff); out.byte(0xa3);
    out.word(static_cast<std::uint32_t>(offsetof(NativeRegionContext, frame_exit)));
    for (std::size_t index = 0; index < count; ++index)
        out.patch_relative(entry_jumps[index], labels[index]);
    for (const auto& [displacement, index] : internal_jumps)
        out.patch_relative(displacement, span_ends[index] != 0
            ? result.entries[index] + 3 : labels[index]);
    for (const auto& [displacement, index] : flow_jumps)
        out.patch_relative(displacement, flow_labels[index]);
    result.bytes = std::move(out.bytes);
    return result;
}

struct CacheKey {
    std::uint64_t guest_pc{};
    std::uint64_t generation{};
    std::vector<std::uint32_t> words;
    bool native_region{};
    bool operator==(const CacheKey&) const = default;
};

struct CacheHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        auto hash = static_cast<std::size_t>(key.guest_pc ^ (key.guest_pc >> 32) ^
                                             key.generation ^
                                             static_cast<std::uint64_t>(key.native_region));
        for (const auto word : key.words) {
            hash ^= static_cast<std::size_t>(word) + 0x9e3779b9U +
                    (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

constexpr std::size_t code_chunk_bytes = 64U * 1024U;
constexpr std::size_t maximum_live_code_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_negative_entries = 131072;

struct ExecutableChunk {
    std::uint8_t* address{};
    std::size_t capacity{};
    std::size_t used{};
    bool executable{};
    bool sealed{};

    explicit ExecutableChunk(std::size_t size) : capacity(size) {
        address = static_cast<std::uint8_t*>(VirtualAlloc(
            nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (address == nullptr) throw std::runtime_error("cannot allocate x64 JIT code");
    }
    ~ExecutableChunk() {
        if (address != nullptr) VirtualFree(address, 0, MEM_RELEASE);
    }
    ExecutableChunk(const ExecutableChunk&) = delete;
    ExecutableChunk& operator=(const ExecutableChunk&) = delete;

    [[nodiscard]] bool fits(std::size_t bytes) const noexcept {
        if (sealed) return false;
        const auto aligned = (used + 15U) & ~std::size_t{15U};
        return aligned <= capacity && bytes <= capacity - aligned;
    }
    void seal() noexcept { sealed = true; }
    [[nodiscard]] void* append(const std::vector<std::uint8_t>& bytes) {
        if (!fits(bytes.size())) throw std::runtime_error("x64 JIT code arena is full");
        const auto aligned = (used + 15U) & ~std::size_t{15U};
        if (executable) {
            DWORD prior = 0;
            if (!VirtualProtect(address, capacity, PAGE_READWRITE, &prior)) {
                throw std::runtime_error("cannot make x64 JIT arena writable");
            }
            executable = false;
        }
        std::memcpy(address + aligned, bytes.data(), bytes.size());
        DWORD prior = 0;
        if (!VirtualProtect(address, capacity, PAGE_EXECUTE_READ, &prior)) {
            throw std::runtime_error("cannot protect x64 JIT code");
        }
        executable = true;
        if (!FlushInstructionCache(GetCurrentProcess(), address + aligned,
                                   bytes.size())) {
            throw std::runtime_error("cannot flush x64 JIT code");
        }
        used = aligned + bytes.size();
        return address + aligned;
    }
};

struct ExecutableCode {
    std::shared_ptr<ExecutableChunk> chunk;
    void* address{};
    std::optional<DecodedInstruction> memory;
    std::uint64_t region_pc{};
    std::vector<std::size_t> region_entries;
    bool region_stores{};
    bool parallel_scalar_capable{};
    std::size_t code_bytes{};
    ExecutableCode(std::shared_ptr<ExecutableChunk> owner,
                   const std::vector<std::uint8_t>& bytes,
                   const DecodedBlock& block)
        : chunk(std::move(owner)), address(chunk->append(bytes)), code_bytes(bytes.size()) {
        if (!block.instructions.empty()) {
            const auto op = block.instructions.front().op;
            if (op == BlockOp::load_byte || op == BlockOp::store_byte ||
                op == BlockOp::load_signed_byte ||
                op == BlockOp::load_halfword ||
                op == BlockOp::load_signed_halfword ||
                op == BlockOp::store_halfword ||
                op == BlockOp::load_word ||
                op == BlockOp::load_signed_word ||
                op == BlockOp::store_word ||
                op == BlockOp::load_doubleword ||
                op == BlockOp::store_doubleword ||
                op == BlockOp::load_pair || op == BlockOp::store_pair) {
                memory = block.instructions.front();
            }
        }
    }
    ExecutableCode(std::shared_ptr<ExecutableChunk> owner,
                   const NativeRegionCode& code, std::uint64_t guest_pc)
        : chunk(std::move(owner)), address(chunk->append(code.bytes)),
          region_pc(guest_pc), region_entries(code.entries), region_stores(code.stores),
          parallel_scalar_capable(code.parallel_scalar_capable), code_bytes(code.bytes.size()) {}
};

struct NativeRegionResult {
    std::size_t completed{};
    std::size_t misses{};
    std::size_t store_misses{};
    std::size_t journal_exits{};
    std::size_t serialized_exits{};
    std::size_t chain_edges{};
};

using NativeEntryResolver = const std::uint8_t* (*)(void*, std::uint64_t,
                                                  std::uint64_t, bool);

void record_native_refill(JitSpanRefillStats& stats,
                          const NativeSpanRefillDiagnostic& diagnostic,
                          bool successful) {
    ++stats.causes[static_cast<std::size_t>(diagnostic.cause)];
    if (successful) {
        ++stats.successful;
    } else {
        ++stats.rejected;
        ++stats.rejections[static_cast<std::size_t>(diagnostic.rejection)];
    }
}

NativeRegionResult run_native_region(ExecutableCode& code, CpuState& state,
                                     NativeReadLease& lease,
                                     NativeWriteLease* writes,
                                     std::size_t instruction_budget,
                                     const NativeChainEntry* chain_table,
                                     std::uint64_t generation,
                                     NativeEntryResolver resolve, void* cache,
                                     const std::uint8_t* chain_exit,
                                     const std::uint8_t* frame_exit,
                                     JitSpanRefillStats* read_refills,
                                     JitSpanRefillStats* store_refills) {
    if (lease.requires_atomic_access() && !code.parallel_scalar_capable)
        return {0, 0, 0, 0, 1};
    if (code.region_stores && writes == nullptr) return {};
    NativeRegionContext context{&lease.table(),
        std::min<std::size_t>(instruction_budget, 256)};
    context.chain_table = chain_table;
    context.code_generation = generation;
    context.chain_exit = chain_exit;
    context.frame_exit = frame_exit;
    context.requires_atomic_access = lease.requires_atomic_access() ? 1U : 0U;
    if (writes != nullptr) {
        context.write_table = &writes->table();
        context.write_journal = &writes->journal();
    }
    if (state.pc < code.region_pc ||
        state.pc - code.region_pc >= code.region_entries.size() * 4 ||
        ((state.pc - code.region_pc) & 3U) != 0) return {};
    auto* entry_address = static_cast<std::uint8_t*>(code.address) +
        code.region_entries[static_cast<std::size_t>((state.pc - code.region_pc) / 4)];
    std::size_t misses = 0;
    std::size_t store_misses = 0;
    std::size_t journal_exits = 0;
    std::size_t serialized_exits = 0;
    std::size_t exits = 0;
    using Function = void (*)(CpuState*, NativeRegionContext*);
    for (;;) {
        context.miss_width = 0;
        reinterpret_cast<Function>(entry_address)(&state, &context);
        if (writes != nullptr) writes->commit();
        if (context.miss_width == 0) break;
        // Generated code has returned before metadata refill. No host pointer
        // or guest callback crosses a native invocation, and already completed
        // instructions are never restarted after a miss.
        if (++exits > 256) break;
        if (context.miss_kind == 1) {
            ++misses;
            if (read_refills != nullptr) {
                NativeSpanRefillDiagnostic diagnostic;
                const auto successful = lease.refill_read(
                    context.miss_address, context.miss_width, &diagnostic);
                record_native_refill(*read_refills, diagnostic, successful);
                if (!successful) break;
            } else if (!lease.refill_read(context.miss_address, context.miss_width)) {
                break;
            }
        } else if (context.miss_kind == 2) {
            ++store_misses;
            if (writes == nullptr) break;
            if (store_refills != nullptr) {
                NativeSpanRefillDiagnostic diagnostic;
                const auto successful = writes->refill_write(
                    context.miss_address, context.miss_width, &diagnostic);
                record_native_refill(*store_refills, diagnostic, successful);
                if (!successful) break;
            } else if (!writes->refill_write(context.miss_address, context.miss_width)) {
                break;
            }
        } else if (context.miss_kind == 3) {
            ++journal_exits;
            // commit() emptied the journal before this exact store resumes.
        } else if (context.miss_kind == 4) {
            ++serialized_exits;
            // The already committed prefix is retained. The dispatcher
            // promotes the next invocation and retries only this instruction.
            break;
        } else {
            throw std::logic_error("native memory exit has an invalid kind");
        }
        // A chained owner can miss at any of its aliases. Resolve only this
        // precise instruction after committing; never restart the initial
        // region or repeat its completed prefix. The private lease cannot
        // invoke guest/cache callbacks, so all sealed owners stay pinned here.
        const auto* resumed = resolve(cache, state.pc, generation,
                                      context.requires_atomic_access != 0);
        if (resumed == nullptr) break;
        entry_address = const_cast<std::uint8_t*>(resumed);
        context.table = &lease.table();
        if (writes != nullptr) context.write_table = &writes->table();
    }
    return {static_cast<std::size_t>(context.completed), misses,
            store_misses, journal_exits, serialized_exits,
            static_cast<std::size_t>(context.chain_edges)};
}

struct MemoryValues {
    std::uint64_t first{};
    std::uint64_t second{};
};

std::optional<MemoryValues> access_memory(
    GuestMemory& memory, const DecodedInstruction& access,
    const CpuState& state) {
    const auto base = access.source == 31 ? state.sp : state.x[access.source];
    std::uint64_t offset = access.immediate;
    if (access.memory_register_offset) {
        const auto register_value = access.second == 31 ? 0
            : state.x[access.second];
        if (access.extend_option == 2) {
            offset = static_cast<std::uint32_t>(register_value);
        } else if (access.extend_option == 3 || access.extend_option == 7) {
            offset = register_value;
        } else if (access.extend_option == 6) {
            offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(
                static_cast<std::int32_t>(register_value)));
        } else {
            return std::nullopt;
        }
        if (access.memory_scaled) offset <<= access.shift;
    }
    const auto address = base + (access.memory_writeback &&
        !access.memory_preindex ? 0 : offset);
    if (access.op == BlockOp::load_byte ||
        access.op == BlockOp::load_signed_byte) {
        const auto value = memory.try_read_contiguous(address, 1);
        return MemoryValues{value ? *value : memory.read8(address), 0};
    }
    if (access.op == BlockOp::load_halfword ||
        access.op == BlockOp::load_signed_halfword) {
        const auto value = memory.try_read_contiguous(address, 2);
        if (value) return MemoryValues{*value, 0};
        const auto low = memory.read8(address);
        const auto high = memory.read8(address + 1);
        return MemoryValues{static_cast<std::uint64_t>(low) |
                            (static_cast<std::uint64_t>(high) << 8), 0};
    }
    if (access.op == BlockOp::load_word ||
        access.op == BlockOp::load_signed_word) {
        const auto value = memory.try_read_contiguous(address, 4);
        return MemoryValues{value ? *value : memory.read32(address), 0};
    }
    if (access.op == BlockOp::load_doubleword) {
        const auto value = memory.try_read_contiguous(address, 8);
        return MemoryValues{value ? *value : memory.read64(address), 0};
    }
    if (access.op == BlockOp::load_pair) {
        const auto width = access.wide ? 8U : 4U;
        const auto first_fast = memory.try_read_contiguous(address, width);
        const auto first = first_fast ? *first_fast
            : (access.wide ? memory.read64(address) : memory.read32(address));
        const auto second_fast = memory.try_read_contiguous(address + width, width);
        const auto second = second_fast ? *second_fast
            : (access.wide ? memory.read64(address + width)
                           : memory.read32(address + width));
        return MemoryValues{first, second};
    }
    const auto value = access.destination == 31 ? 0 : state.x[access.destination];
    if (access.op == BlockOp::store_byte) {
        if (!memory.try_write_contiguous(address, 1, value)) {
            memory.write8(address, static_cast<std::uint8_t>(value));
        }
        return MemoryValues{};
    }
    // The mapped API checks the entire access before writing any bytes. A
    // failed fast access leaves partial boundary/image faults to the reference
    // engine, and avoids a separate region lookup for ordinary single stores.
    if (access.op == BlockOp::store_halfword ||
        access.op == BlockOp::store_word ||
        access.op == BlockOp::store_doubleword) {
        const auto bytes = access.op == BlockOp::store_halfword ? 2U
            : access.op == BlockOp::store_word ? 4U : 8U;
        if (!memory.try_write_contiguous(address, bytes, value)) return std::nullopt;
        return MemoryValues{};
    }
    // Pairs use two writes, so check the combined extent before either one.
    const auto bytes = access.wide ? 16U : 8U;
    const auto region = memory.query_region(address);
    if (!region || region->image || !region->writable ||
        address < region->base || address - region->base >= region->size ||
        bytes > region->size - (address - region->base)) {
        return std::nullopt;
    }
    if (access.op == BlockOp::store_pair) {
        const auto second_value = access.second == 31 ? 0
            : state.x[access.second];
        if (access.wide) {
            memory.write64(address, value);
            memory.write64(address + 8, second_value);
        } else {
            memory.write32(address, static_cast<std::uint32_t>(value));
            memory.write32(address + 4,
                           static_cast<std::uint32_t>(second_value));
        }
    }
    return MemoryValues{};
}

} // namespace

struct X64BlockCache::Impl {
    struct HotBlock {
        std::uint64_t generation;
        std::size_t instructions;
        ExecutableCode* code;
        std::size_t entry{};
    };
    struct FrontEntry {
        std::uint64_t guest_pc{};
        HotBlock block{};
        bool valid{};
    };
    std::array<FrontEntry, 1024> front{};
    std::array<NativeChainEntry, native_chain_entries> chains{};
    void publish_chain(std::uint64_t pc, const HotBlock& hot) {
        auto& target = chains[(pc >> 2) & (chains.size() - 1)];
        if (hot.code == nullptr || hot.code->region_entries.empty()) {
            if (target.guest_pc == pc) target = {};
            return;
        }
        target = {pc, hot.generation,
            static_cast<std::uint8_t*>(hot.code->address) +
                hot.code->region_entries[hot.entry] + 3,
            (hot.code->region_stores ? 1U : 0U) |
                (hot.code->parallel_scalar_capable ? 2U : 0U), 0};
    }
    FrontEntry& front_entry(std::uint64_t pc) {
        return front[(pc >> 2) & (front.size() - 1)];
    }
    const HotBlock* find_hot(std::uint64_t pc, std::uint64_t generation) {
        auto& entry = front_entry(pc);
        if (entry.valid && entry.guest_pc == pc &&
            entry.block.generation == generation) {
            if (profiling && entry.block.code != nullptr) ++front_cache_hits;
            return &entry.block;
        }
        const auto found = hot_blocks.find(pc);
        if (found == hot_blocks.end() || found->second.generation != generation)
            return nullptr;
        if (profiling && found->second.code != nullptr) ++map_cache_hits;
        entry = {pc, found->second, true};
        publish_chain(pc, found->second);
        return &entry.block;
    }
    const HotBlock* peek_hot(std::uint64_t pc, std::uint64_t generation) const noexcept {
        const auto& entry = front[(pc >> 2) & (front.size() - 1)];
        if (entry.valid && entry.guest_pc == pc &&
            entry.block.generation == generation) return &entry.block;
        const auto found = hot_blocks.find(pc);
        return found != hot_blocks.end() && found->second.generation == generation
            ? &found->second : nullptr;
    }
    static const std::uint8_t* resolve_native_entry(
        void* opaque, std::uint64_t pc, std::uint64_t generation, bool shared) {
        auto& owner = *static_cast<Impl*>(opaque);
        const auto* hot = owner.find_hot(pc, generation);
        if (hot == nullptr || hot->code == nullptr ||
            hot->code->region_entries.empty() ||
            (shared && !hot->code->parallel_scalar_capable)) return nullptr;
        return static_cast<std::uint8_t*>(hot->code->address) +
            hot->code->region_entries[hot->entry];
    }
    std::unordered_map<CacheKey, std::unique_ptr<ExecutableCode>, CacheHash> blocks;
    std::unordered_map<std::uint64_t, HotBlock> hot_blocks;
    std::unordered_map<std::uint64_t, JitBlockStat> block_counts;
    std::unordered_map<std::uint64_t, JitFallbackStat> fallback_samples;
    std::shared_ptr<ExecutableChunk> append_chunk;
    // Sealed once and retained until cache destruction, independently of
    // ordinary block retirement and arena reclamation. Native frames never
    // reenter compilation; shared workers own different cache objects.
    std::shared_ptr<ExecutableChunk> exit_chunk;
    const std::uint8_t* native_chain_exit{};
    const std::uint8_t* native_frame_exit{};
    void ensure_native_exits() {
        if (exit_chunk) return;
        const auto start = std::chrono::steady_clock::now();
        const auto exits = emit_native_exit_trampolines();
        auto chunk = std::make_shared<ExecutableChunk>(4096);
        const auto* address = static_cast<std::uint8_t*>(chunk->append(exits.bytes));
        native_chain_exit = address;
        native_frame_exit = address + exits.frame_exit;
        exit_chunk = std::move(chunk);
        emitted_code_bytes += exits.bytes.size();
        allocated_code_bytes += exit_chunk->capacity;
        compilation_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    }
    std::size_t arena_bytes{};
    std::uint64_t compiled_blocks{};
    std::uint64_t native_blocks{};
    std::uint64_t fast_hits{};
    std::uint64_t native_guest_instructions{};
    std::uint64_t compiled_native_regions{};
    std::uint64_t inherited_native_regions{};
    std::uint64_t inherited_code_bytes{};
    std::uint64_t inherited_chunk_bytes{};
    std::uint64_t native_region_instructions{};
    std::uint64_t native_region_alias_hits{};
    std::uint64_t native_chain_edges{};
    std::uint64_t compiled_register_spans{};
    std::uint64_t native_read_misses{};
    std::uint64_t native_store_misses{};
    JitSpanRefillStats native_read_refills;
    JitSpanRefillStats native_store_refills;
    std::uint64_t native_journal_exits{};
    std::uint64_t native_serialized_access_exits{};
    std::uint64_t front_cache_hits{};
    std::uint64_t map_cache_hits{};
    std::uint64_t arena_reclamations{};
    std::uint64_t negative_hits{};
    std::size_t negative_entries{};
    std::uint64_t fallback_profile_observations{};
    std::uint32_t sample_rng{0x9e3779b9U};
    std::uint64_t emitted_code_bytes{};
    std::uint64_t allocated_code_bytes{};
    double compilation_seconds{};
    double decoding_seconds{};
    bool profiling{};
    const void* last_code{};
    std::uint64_t cache_epoch{};
    struct Invocation;
    Invocation* active_invocation{};
    std::vector<std::unique_ptr<ExecutableCode>> retired_code;

    // Virtual memory callbacks can invalidate or compile recursively. Keep
    // only the code referenced by live invocations; ordinary hits allocate
    // nothing and perform no atomic/shared-pointer reference counting.
    struct Invocation {
        Impl& owner;
        ExecutableCode* code;
        Invocation* previous;
        std::uint64_t epoch;
        Invocation(Impl& cache_owner, ExecutableCode* active_code)
            : owner(cache_owner), code(active_code),
              previous(cache_owner.active_invocation), epoch(cache_owner.cache_epoch) {
            owner.active_invocation = this;
        }
        Invocation(const Invocation&) = delete;
        Invocation& operator=(const Invocation&) = delete;
        ~Invocation() {
            owner.active_invocation = previous;
            if (!owner.retired_code.empty()) owner.collect_retired();
        }
        void publish_address() const {
            // A nested invocation may have published newer live code after
            // invalidation. Never replace it with this retired invocation.
            if (epoch == owner.cache_epoch) owner.last_code = code->address;
        }
    };
    bool is_active(const ExecutableCode* code) const {
        for (const auto* invocation = active_invocation; invocation != nullptr;
             invocation = invocation->previous) {
            if (invocation->code == code) return true;
        }
        return false;
    }
    void prepare_retirement() {
        std::size_t depth = 0;
        for (const auto* invocation = active_invocation; invocation != nullptr;
             invocation = invocation->previous) ++depth;
        // Reserve before removing any map entries so allocation failure cannot
        // leave an active block unowned or an empty entry in the code map.
        if (depth != 0) retired_code.reserve(retired_code.size() + depth);
    }
    void retain_active(std::unique_ptr<ExecutableCode>& code) {
        if (is_active(code.get())) retired_code.push_back(std::move(code));
    }
    void collect_retired() {
        std::erase_if(retired_code, [this](const auto& code) {
            return !is_active(code.get());
        });
    }
};

X64BlockCache::X64BlockCache() : impl_(std::make_unique<Impl>()) {}
X64BlockCache::~X64BlockCache() = default;

std::size_t X64BlockCache::inherit_immutable(
    X64BlockCache& parent, std::uint64_t code_generation,
    std::span<PeImage* const> published_modules) {
    if (&parent == this || impl_->active_invocation != nullptr ||
        parent.impl_->active_invocation != nullptr || !impl_->blocks.empty() ||
        !impl_->hot_blocks.empty()) return 0;
    std::vector<const PeImage*> modules;
    modules.reserve(published_modules.size());
    for (const auto* module : published_modules)
        if (module != nullptr) modules.push_back(module);
    std::sort(modules.begin(), modules.end(), [](const auto* left, const auto* right) {
        return left->base() < right->base();
    });
    if (modules.empty()) return 0;
    std::unordered_set<const ExecutableCode*> positive_owners;
    for (const auto& [pc, hot] : parent.impl_->hot_blocks) {
        static_cast<void>(pc);
        if (hot.generation == code_generation && hot.code != nullptr &&
            !hot.code->region_entries.empty()) positive_owners.insert(hot.code);
    }
    if (positive_owners.empty()) return 0;
    const auto admissible = [&](const CacheKey& key) {
        if (!key.native_region || key.generation != code_generation ||
            key.words.empty() || key.words.size() > 64) return false;
        const auto position = std::upper_bound(modules.begin(), modules.end(), key.guest_pc,
            [](std::uint64_t pc, const auto* module) { return pc < module->base(); });
        if (position == modules.begin()) return false;
        const auto* module = *std::prev(position);
        if (key.guest_pc < module->base() || key.guest_pc - module->base() >= module->size())
            return false;
        const auto rva = key.guest_pc - module->base();
        const auto bytes = key.words.size() * 4;
        if (bytes > module->size() - rva) return false;
        bool immutable = false;
        for (const auto& section : module->sections()) {
            if (section.executable && !section.writable && rva >= section.rva &&
                rva - section.rva < section.size &&
                bytes <= section.size - (rva - section.rva)) {
                immutable = true;
                break;
            }
        }
        if (!immutable) return false;
        for (std::size_t index = 0; index < key.words.size(); ++index)
            if (module->instruction(key.guest_pc + index * 4) != key.words[index]) return false;
        return true;
    };
    // Prepare every allocation before changing donor append ownership. Owner
    // wrappers and authoritative aliases are copied, never map-node pointers.
    decltype(impl_->blocks) inherited_blocks;
    inherited_blocks.reserve(positive_owners.size());
    std::unordered_map<const ExecutableCode*, ExecutableCode*> replacements;
    replacements.reserve(positive_owners.size());
    std::unordered_set<ExecutableChunk*> chunks;
    std::size_t chunk_bytes = 0;
    std::uint64_t code_bytes = 0;
    for (const auto& [key, code] : parent.impl_->blocks) {
        if (!positive_owners.contains(code.get()) || !admissible(key)) continue;
        const bool new_chunk = !chunks.contains(code->chunk.get());
        if (new_chunk && code->chunk->capacity > maximum_live_code_bytes - chunk_bytes) continue;
        auto copied = std::make_unique<ExecutableCode>(*code);
        auto* copied_owner = copied.get();
        inherited_blocks.emplace(key, std::move(copied));
        replacements.emplace(code.get(), copied_owner);
        code_bytes += code->code_bytes;
        if (new_chunk) {
            chunks.insert(code->chunk.get());
            chunk_bytes += code->chunk->capacity;
        }
    }
    if (inherited_blocks.empty()) return 0;
    decltype(impl_->hot_blocks) inherited_hot;
    inherited_hot.reserve(parent.impl_->hot_blocks.size());
    for (const auto& [pc, hot] : parent.impl_->hot_blocks) {
        if (hot.generation != code_generation) continue;
        const auto replacement = replacements.find(hot.code);
        if (replacement == replacements.end()) continue;
        inherited_hot.emplace(pc, Impl::HotBlock{hot.generation, hot.instructions,
                                                replacement->second, hot.entry});
    }
    impl_->ensure_native_exits();
    // An RX chunk shared by another execution thread may never be made RW
    // again. Retire the donor's append pointer before publishing the copies.
    for (auto* chunk : chunks) chunk->seal();
    if (chunks.contains(parent.impl_->append_chunk.get())) parent.impl_->append_chunk.reset();
    impl_->blocks = std::move(inherited_blocks);
    impl_->hot_blocks = std::move(inherited_hot);
    impl_->arena_bytes = chunk_bytes;
    impl_->inherited_native_regions += impl_->blocks.size();
    impl_->inherited_code_bytes += code_bytes;
    impl_->inherited_chunk_bytes += chunk_bytes;
    // Preserve recent donor hints only when they refer to admitted owners.
    // Every mutable hint/table and the exit blob still belongs to this cache.
    for (const auto& entry : parent.impl_->front) {
        if (!entry.valid || entry.block.generation != code_generation) continue;
        const auto found = impl_->hot_blocks.find(entry.guest_pc);
        if (found != impl_->hot_blocks.end())
            impl_->front_entry(entry.guest_pc) = {entry.guest_pc, found->second, true};
    }
    for (const auto& entry : parent.impl_->chains) {
        if (entry.address == nullptr || entry.generation != code_generation) continue;
        const auto found = impl_->hot_blocks.find(entry.guest_pc);
        if (found != impl_->hot_blocks.end()) impl_->publish_chain(entry.guest_pc, found->second);
    }
    return impl_->blocks.size();
}

bool X64BlockCache::execute(CpuState& state, std::uint64_t guest_pc,
                            std::span<const std::uint32_t> words,
                            std::uint64_t code_generation,
                            GuestMemory* memory) {
    if (state.pc != guest_pc) return false;
    const auto decode_start = std::chrono::steady_clock::now();
    const auto decoded = decode_integer_block(guest_pc, words);
    impl_->decoding_seconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - decode_start).count();
    if (!decoded) return false;
    CacheKey key{guest_pc, code_generation, decoded->words};
    auto found = impl_->blocks.find(key);
    if (found == impl_->blocks.end()) {
        const auto start = std::chrono::steady_clock::now();
        const auto bytes = emit_block(*decoded);
        if (!impl_->append_chunk || !impl_->append_chunk->fits(bytes.size())) {
            const auto chunk_size = std::max(code_chunk_bytes,
                ((bytes.size() + code_chunk_bytes - 1) / code_chunk_bytes) *
                    code_chunk_bytes);
            if (chunk_size > maximum_live_code_bytes) {
                throw std::runtime_error("x64 JIT block exceeds code arena limit");
            }
            if (impl_->arena_bytes > maximum_live_code_bytes - chunk_size) {
                ++impl_->arena_reclamations;
                invalidate_all();
            }
            impl_->append_chunk = std::make_shared<ExecutableChunk>(chunk_size);
            impl_->arena_bytes += chunk_size;
            impl_->allocated_code_bytes += chunk_size;
        }
        auto code = std::make_unique<ExecutableCode>(impl_->append_chunk, bytes,
                                                     *decoded);
        impl_->emitted_code_bytes += bytes.size();
        impl_->compilation_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        found = impl_->blocks.emplace(std::move(key), std::move(code)).first;
        ++impl_->compiled_blocks;
    }
    const auto prior_hot = impl_->hot_blocks.find(guest_pc);
    const bool preserve_region = prior_hot != impl_->hot_blocks.end() &&
        prior_hot->second.generation == code_generation &&
        prior_hot->second.code != nullptr &&
        !prior_hot->second.code->region_entries.empty();
    if (!preserve_region) {
        if (prior_hot != impl_->hot_blocks.end() && prior_hot->second.code == nullptr)
            --impl_->negative_entries;
        impl_->hot_blocks[guest_pc] = Impl::HotBlock{
            code_generation, decoded->words.size(), found->second.get()};
        impl_->front_entry(guest_pc) = {guest_pc, impl_->hot_blocks[guest_pc], true};
        impl_->publish_chain(guest_pc, impl_->hot_blocks[guest_pc]);
    }
    auto* executable = found->second.get();
    if (executable->memory && memory == nullptr) return false;
    Impl::Invocation invocation(*impl_, executable);
    MemoryValues memory_values{};
    if (executable->memory) {
        try {
            const auto value = access_memory(*memory, *executable->memory,
                                             state);
            if (!value) return false;
            memory_values = *value;
        } catch (...) {
            // The interpreter must report a fault at the precise guest step.
            return false;
        }
    }
    invocation.publish_address();
    using Function = void (*)(CpuState*, std::uint64_t, std::uint64_t);
    reinterpret_cast<Function>(executable->address)(
        &state, memory_values.first, memory_values.second);
    ++impl_->native_blocks;
    impl_->native_guest_instructions += decoded->words.size();
    if (impl_->profiling) {
        auto& block = impl_->block_counts[guest_pc];
        block = {guest_pc, decoded->words.size(), block.executions + 1};
    }
    return true;
}

std::size_t X64BlockCache::execute_native_region(
    CpuState& state, std::uint64_t guest_pc,
    std::span<const std::uint32_t> words, std::uint64_t code_generation,
    std::size_t instruction_budget, GuestMemory* memory,
    bool* requires_exclusive) {
    if (requires_exclusive != nullptr) *requires_exclusive = false;
    if (state.pc != guest_pc || instruction_budget == 0 || memory == nullptr)
        return 0;
    auto* lease = memory->native_read_lease();
    if (lease == nullptr) return 0;
    if (const auto* prior = impl_->find_hot(guest_pc, code_generation);
        prior != nullptr && prior->code != nullptr &&
        !prior->code->region_entries.empty()) {
        // Yield and branch targets already covered by a region must not
        // compile overlapping suffixes, even when a short budget or runtime
        // guard made the first cached invocation return zero.
        return execute_cached(state, guest_pc, code_generation,
                              instruction_budget, nullptr, memory, requires_exclusive);
    }
    const auto decode_start = std::chrono::steady_clock::now();
    const auto decoded = decode_native_read_region(guest_pc, words);
    impl_->decoding_seconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - decode_start).count();
    if (!decoded) return 0;
    impl_->ensure_native_exits();
    CacheKey key{guest_pc, code_generation, decoded->words, true};
    auto found = impl_->blocks.find(key);
    if (found == impl_->blocks.end()) {
        const auto start = std::chrono::steady_clock::now();
        const auto native = emit_native_region(*decoded);
        impl_->compiled_register_spans += native.retained_spans;
        if (!impl_->append_chunk || !impl_->append_chunk->fits(native.bytes.size())) {
            const auto chunk_size = std::max(code_chunk_bytes,
                ((native.bytes.size() + code_chunk_bytes - 1) / code_chunk_bytes) *
                    code_chunk_bytes);
            if (chunk_size > maximum_live_code_bytes)
                throw std::runtime_error("x64 JIT region exceeds code arena limit");
            if (impl_->arena_bytes > maximum_live_code_bytes - chunk_size) {
                ++impl_->arena_reclamations;
                invalidate_all();
            }
            impl_->append_chunk = std::make_shared<ExecutableChunk>(chunk_size);
            impl_->arena_bytes += chunk_size;
            impl_->allocated_code_bytes += chunk_size;
        }
        auto code = std::make_unique<ExecutableCode>(impl_->append_chunk, native,
                                                     guest_pc);
        impl_->emitted_code_bytes += native.bytes.size();
        impl_->compilation_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        found = impl_->blocks.emplace(std::move(key), std::move(code)).first;
        ++impl_->compiled_blocks;
        ++impl_->compiled_native_regions;
    }
    auto* code = found->second.get();
    for (std::size_t index = 0; index < code->region_entries.size(); ++index) {
        const auto alias_pc = guest_pc + index * 4;
        const Impl::HotBlock alias{code_generation,
            code->region_entries.size() - index, code, index};
        auto [position, inserted] = impl_->hot_blocks.try_emplace(alias_pc, alias);
        if (!inserted) {
            const auto& prior = position->second;
            if (prior.generation != code_generation || prior.code == nullptr ||
                prior.code->region_entries.empty()) {
                if (prior.code == nullptr) --impl_->negative_entries;
                position->second = alias;
            }
        }
        impl_->front_entry(alias_pc) = {alias_pc, position->second, true};
        impl_->publish_chain(alias_pc, position->second);
    }
    Impl::Invocation invocation(*impl_, code);
    auto* writes = memory->native_write_lease();
    const auto result = run_native_region(*code, state, *lease, writes,
        instruction_budget, impl_->chains.data(), code_generation,
        Impl::resolve_native_entry, impl_.get(),
        impl_->native_chain_exit, impl_->native_frame_exit,
        impl_->profiling ? &impl_->native_read_refills : nullptr,
        impl_->profiling ? &impl_->native_store_refills : nullptr);
    impl_->native_chain_edges += result.chain_edges;
    impl_->native_read_misses += result.misses;
    impl_->native_store_misses += result.store_misses;
    impl_->native_journal_exits += result.journal_exits;
    impl_->native_serialized_access_exits += result.serialized_exits;
    if (requires_exclusive != nullptr)
        *requires_exclusive = result.serialized_exits != 0;
    if (result.completed == 0) return 0;
    invocation.publish_address();
    ++impl_->native_blocks;
    impl_->native_guest_instructions += result.completed;
    impl_->native_region_instructions += result.completed;
    if (impl_->profiling) {
        auto& block = impl_->block_counts[guest_pc];
        block = {guest_pc, decoded->words.size(), block.executions + 1};
    }
    return result.completed;
}

std::size_t X64BlockCache::execute_cached(CpuState& state,
                                           std::uint64_t guest_pc,
                                           std::uint64_t code_generation,
                                           std::size_t instruction_budget,
                                           bool* known_fallback,
                                           GuestMemory* memory,
                                           bool* requires_exclusive) {
    if (known_fallback != nullptr) *known_fallback = false;
    if (requires_exclusive != nullptr) *requires_exclusive = false;
    if (state.pc != guest_pc) return 0;
    const auto* found = impl_->find_hot(guest_pc, code_generation);
    if (found == nullptr) return 0;
    // GuestMemory is virtual and may reenter this cache. Snapshot metadata so
    // a colliding lookup during an access cannot replace this invocation.
    const auto hot = *found;
    if (hot.code == nullptr) {
        ++impl_->negative_hits;
        if (known_fallback != nullptr) *known_fallback = true;
        return 0;
    }
    if (!hot.code->region_entries.empty()) {
        if (instruction_budget == 0 || memory == nullptr) return 0;
        Impl::Invocation invocation(*impl_, hot.code);
        auto* lease = memory->native_read_lease();
        if (lease == nullptr) return 0;
        auto* writes = memory->native_write_lease();
        const auto result = run_native_region(*hot.code, state, *lease, writes,
            instruction_budget, impl_->chains.data(), code_generation,
            Impl::resolve_native_entry, impl_.get(),
            impl_->native_chain_exit, impl_->native_frame_exit,
            impl_->profiling ? &impl_->native_read_refills : nullptr,
            impl_->profiling ? &impl_->native_store_refills : nullptr);
        impl_->native_chain_edges += result.chain_edges;
        impl_->native_read_misses += result.misses;
        impl_->native_store_misses += result.store_misses;
        impl_->native_journal_exits += result.journal_exits;
        impl_->native_serialized_access_exits += result.serialized_exits;
        if (requires_exclusive != nullptr)
            *requires_exclusive = result.serialized_exits != 0;
        if (result.completed == 0) return 0;
        invocation.publish_address();
        ++impl_->native_blocks;
        ++impl_->fast_hits;
        if (hot.entry != 0) ++impl_->native_region_alias_hits;
        impl_->native_guest_instructions += result.completed;
        impl_->native_region_instructions += result.completed;
        if (impl_->profiling) {
            auto& block = impl_->block_counts[guest_pc];
            block = {guest_pc, hot.instructions, block.executions + 1};
        }
        return result.completed;
    }
    if (hot.instructions > instruction_budget) return 0;
    if (hot.code->memory && memory == nullptr) return 0;
    Impl::Invocation invocation(*impl_, hot.code);
    MemoryValues memory_values{};
    if (hot.code->memory) {
        try {
            const auto value = access_memory(*memory,
                *hot.code->memory, state);
            if (!value) return 0;
            memory_values = *value;
        } catch (...) {
            // Let the interpreter reproduce the access and report its fault
            // with the guest instruction and module attached.
            return 0;
        }
    }
    invocation.publish_address();
    using Function = void (*)(CpuState*, std::uint64_t, std::uint64_t);
    reinterpret_cast<Function>(hot.code->address)(&state,
        memory_values.first, memory_values.second);
    ++impl_->native_blocks;
    ++impl_->fast_hits;
    impl_->native_guest_instructions += hot.instructions;
    if (impl_->profiling) {
        auto& block = impl_->block_counts[guest_pc];
        block = {guest_pc, hot.instructions, block.executions + 1};
    }
    return hot.instructions;
}

bool X64BlockCache::has_read_only_native_region(
    std::uint64_t guest_pc, std::uint64_t code_generation) const noexcept {
    const auto* hot = impl_->peek_hot(guest_pc, code_generation);
    return hot != nullptr && hot->code != nullptr &&
        !hot->code->region_entries.empty() && !hot->code->region_stores;
}

bool X64BlockCache::has_read_only_cached_block(
    std::uint64_t guest_pc, std::uint64_t code_generation) const noexcept {
    const auto* hot = impl_->peek_hot(guest_pc, code_generation);
    return hot != nullptr && hot->code != nullptr &&
        (hot->code->region_entries.empty() ? !hot->code->memory
                                          : !hot->code->region_stores);
}

bool X64BlockCache::has_parallel_cached_block(
    std::uint64_t guest_pc, std::uint64_t code_generation) const noexcept {
    const auto* hot = impl_->peek_hot(guest_pc, code_generation);
    return hot != nullptr && hot->code != nullptr &&
        (hot->code->region_entries.empty() ? !hot->code->memory
                                          : hot->code->parallel_scalar_capable);
}

void X64BlockCache::remember_fallback(std::uint64_t guest_pc,
                                      std::uint64_t code_generation) {
    // The authoritative map below may replace a positive or older-generation
    // entry. A stale front entry must never mask that transition.
    impl_->front_entry(guest_pc).valid = false;
    const auto found = impl_->hot_blocks.find(guest_pc);
    if (found != impl_->hot_blocks.end()) {
        if (found->second.code != nullptr &&
            found->second.generation == code_generation) return;
        if (found->second.code == nullptr) {
            found->second.generation = code_generation;
            return;
        }
        if (impl_->negative_entries == maximum_negative_entries) return;
        found->second = {code_generation, 0, nullptr};
        impl_->publish_chain(guest_pc, found->second);
        ++impl_->negative_entries;
        return;
    }
    if (impl_->negative_entries == maximum_negative_entries) return;
    impl_->hot_blocks.emplace(guest_pc,
                              Impl::HotBlock{code_generation, 0, nullptr});
    impl_->publish_chain(guest_pc, impl_->hot_blocks.at(guest_pc));
    ++impl_->negative_entries;
}

void X64BlockCache::record_fallback(std::uint64_t guest_pc,
                                    std::uint32_t opcode) {
    if (!impl_->profiling) return;
    ++impl_->fallback_profile_observations;
    auto value = impl_->sample_rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    impl_->sample_rng = value;
    if ((value & 1023U) != 0) return;
    auto& sample = impl_->fallback_samples[guest_pc];
    sample = {guest_pc, opcode, sample.samples + 1};
}

void X64BlockCache::invalidate_range(std::uint64_t guest_pc, std::uint64_t bytes) {
    if (bytes == 0) return;
    if (bytes > std::numeric_limits<std::uint64_t>::max() - guest_pc) {
        invalidate_all();
        return;
    }
    const auto end = guest_pc + bytes;
    impl_->prepare_retirement();
    ++impl_->cache_epoch;
    impl_->chains = {};
    bool removed = false;
    for (auto it = impl_->blocks.begin(); it != impl_->blocks.end();) {
        const auto block_bytes = it->first.words.size() * 4;
        if (it->first.guest_pc < end &&
            (it->first.guest_pc >= guest_pc ||
             guest_pc - it->first.guest_pc < block_bytes)) {
            if (impl_->last_code == it->second->address) impl_->last_code = nullptr;
            impl_->retain_active(it->second);
            it = impl_->blocks.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed || !impl_->hot_blocks.empty()) {
        impl_->front = {};
        impl_->hot_blocks.clear();
        impl_->negative_entries = 0;
    }
}

void X64BlockCache::invalidate_all() {
    impl_->prepare_retirement();
    ++impl_->cache_epoch;
    impl_->chains = {};
    impl_->last_code = nullptr;
    impl_->front = {};
    impl_->hot_blocks.clear();
    impl_->negative_entries = 0;
    if (impl_->active_invocation != nullptr) {
        for (auto& [key, code] : impl_->blocks) {
            static_cast<void>(key);
            impl_->retain_active(code);
        }
    }
    impl_->blocks.clear();
    impl_->append_chunk.reset();
    impl_->arena_bytes = 0;
}

std::size_t X64BlockCache::cache_size() const { return impl_->blocks.size(); }
const void* X64BlockCache::last_code_address() const noexcept {
    return impl_->last_code;
}

void X64BlockCache::set_profiling(bool enabled) {
    impl_->profiling = enabled;
    if (!enabled) {
        impl_->block_counts.clear();
        impl_->fallback_samples.clear();
    }
}

JitStats X64BlockCache::stats(std::size_t hottest) const {
    JitStats result;
    result.live_blocks = impl_->blocks.size();
    result.compiled_blocks = impl_->compiled_blocks;
    result.native_blocks = impl_->native_blocks;
    result.fast_hits = impl_->fast_hits;
    result.native_guest_instructions = impl_->native_guest_instructions;
    result.compiled_native_regions = impl_->compiled_native_regions;
    result.inherited_native_regions = impl_->inherited_native_regions;
    result.inherited_code_bytes = impl_->inherited_code_bytes;
    result.inherited_chunk_bytes = impl_->inherited_chunk_bytes;
    result.native_region_instructions = impl_->native_region_instructions;
    result.native_region_alias_hits = impl_->native_region_alias_hits;
    result.native_read_misses = impl_->native_read_misses;
    result.native_store_misses = impl_->native_store_misses;
    result.native_read_refills = impl_->native_read_refills;
    result.native_store_refills = impl_->native_store_refills;
    result.native_journal_exits = impl_->native_journal_exits;
    result.native_serialized_access_exits = impl_->native_serialized_access_exits;
    result.front_cache_hits = impl_->front_cache_hits;
    result.map_cache_hits = impl_->map_cache_hits;
    result.arena_reclamations = impl_->arena_reclamations;
    result.negative_cache_hits = impl_->negative_hits;
    result.negative_cache_entries = impl_->negative_entries;
    result.fallback_profile_observations = impl_->fallback_profile_observations;
    result.emitted_code_bytes = impl_->emitted_code_bytes;
    result.allocated_code_bytes = impl_->allocated_code_bytes;
    result.compilation_seconds = impl_->compilation_seconds;
    result.decoding_seconds = impl_->decoding_seconds;
    result.native_chain_edges = impl_->native_chain_edges;
    result.compiled_register_spans = impl_->compiled_register_spans;
    std::unordered_set<const ExecutableChunk*> chunks;
    if (impl_->exit_chunk) chunks.insert(impl_->exit_chunk.get());
    if (impl_->append_chunk) chunks.insert(impl_->append_chunk.get());
    for (const auto& [key, code] : impl_->blocks) {
        static_cast<void>(key);
        chunks.insert(code->chunk.get());
    }
    for (const auto* chunk : chunks) result.live_code_bytes += chunk->capacity;
    result.hottest_blocks.reserve(impl_->block_counts.size());
    for (const auto& [pc, block] : impl_->block_counts) {
        static_cast<void>(pc);
        result.hottest_blocks.push_back(block);
    }
    std::sort(result.hottest_blocks.begin(), result.hottest_blocks.end(),
              [](const auto& left, const auto& right) {
                  return left.executions > right.executions;
              });
    if (result.hottest_blocks.size() > hottest) {
        result.hottest_blocks.resize(hottest);
    }
    result.hottest_fallbacks.reserve(impl_->fallback_samples.size());
    for (const auto& [pc, sample] : impl_->fallback_samples) {
        static_cast<void>(pc);
        result.hottest_fallbacks.push_back(sample);
    }
    std::sort(result.hottest_fallbacks.begin(), result.hottest_fallbacks.end(),
              [](const auto& left, const auto& right) {
                  return left.samples > right.samples;
              });
    if (result.hottest_fallbacks.size() > hottest) {
        result.hottest_fallbacks.resize(hottest);
    }
    return result;
}

} // namespace transmute
