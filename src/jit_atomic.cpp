#include "jit_atomic.hpp"

#include "transmute/a64.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace transmute::jit_detail {
namespace {

struct Emitter {
    std::vector<std::uint8_t> bytes;

    void byte(std::uint8_t value) { bytes.push_back(value); }
    void word(std::uint32_t value) {
        for (unsigned i = 0; i != 4; ++i) byte(static_cast<std::uint8_t>(value >> (i * 8)));
    }
    void load_guest(unsigned gpr, unsigned guest, unsigned width) {
        if (guest == 31) {
            if (gpr >= 8) byte(0x45);
            byte(0x31); byte(static_cast<std::uint8_t>(0xc0U | ((gpr & 7U) * 9U)));
            return;
        }
        if (width == 8 || gpr >= 8) byte(static_cast<std::uint8_t>(
            (width == 8 ? 0x48U : 0x40U) | (gpr >= 8 ? 4U : 0U)));
        if (width < 4) {
            byte(0x0f); byte(width == 1 ? 0xb6 : 0xb7);
        } else byte(0x8b);
        byte(static_cast<std::uint8_t>(0x81U | ((gpr & 7U) << 3)));
        word(static_cast<std::uint32_t>(offsetof(CpuState, x) + guest * 8));
    }
    void store_guest(unsigned gpr, unsigned guest) {
        if (guest == 31) return;
        byte(static_cast<std::uint8_t>(0x48U | (gpr >= 8 ? 4U : 0U)));
        byte(0x89); byte(static_cast<std::uint8_t>(0x81U | ((gpr & 7U) << 3)));
        word(static_cast<std::uint32_t>(offsetof(CpuState, x) + guest * 8));
    }
    void load_memory(unsigned width) {
        if (width == 8) byte(0x48);
        if (width < 4) {
            byte(0x0f); byte(width == 1 ? 0xb6 : 0xb7);
        } else byte(0x8b);
        byte(0x02); // RAX/EAX = [RDX], with narrow zero extension.
    }
    void locked(std::uint8_t opcode, unsigned width) {
        if (width == 2) byte(0x66);
        byte(0xf0); byte(width == 8 ? 0x4c : 0x44);
        byte(0x0f); byte(opcode); byte(0x02); // [RDX],R8[b/w/d/q]
    }
    void move(unsigned destination, unsigned source, bool wide) {
        byte(static_cast<std::uint8_t>((wide ? 0x48U : 0x40U) |
            (source >= 8 ? 4U : 0U) | (destination >= 8 ? 1U : 0U)));
        byte(0x89); byte(static_cast<std::uint8_t>(
            0xc0U | ((source & 7U) << 3) | (destination & 7U)));
    }
    void compare_source(unsigned width) {
        if (width == 2) byte(0x66);
        byte(width == 8 ? 0x4c : 0x44);
        byte(width == 1 ? 0x38 : 0x39); byte(0xc8); // CMP AL/AX/EAX/RAX,R9
    }
    void retry(std::size_t target) {
        byte(0x0f); byte(0x85);
        const auto after = bytes.size() + 4;
        word(static_cast<std::uint32_t>(static_cast<std::int64_t>(target) -
                                       static_cast<std::int64_t>(after)));
    }
    void wrote() { byte(0x41); byte(0xb9); word(1); }
};

} // namespace

std::optional<NativeLseInstruction> decode_native_lse(std::uint32_t word) noexcept {
    const auto width = 1U << (word >> 30);
    const auto source = (word >> 16) & 31U;
    const auto target = word & 31U;
    const auto base = (word >> 5) & 31U;
    if ((word & 0x3fa07c00U) == 0x08a07c00U) {
        return NativeLseInstruction{NativeLseOperation::compare_swap, width,
            source, target, base, (word & 0x00400000U) != 0, (word & 0x00008000U) != 0};
    }
    const auto rmw = word & 0x3f208c00U;
    if (rmw == 0x38200000U) {
        return NativeLseInstruction{static_cast<NativeLseOperation>((word >> 12) & 7U),
            width, source, target, base, (word & 0x00800000U) != 0,
            (word & 0x00400000U) != 0};
    }
    if (rmw == 0x38208000U && (word & 0x00007000U) == 0) {
        return NativeLseInstruction{NativeLseOperation::swap, width, source, target,
            base, (word & 0x00800000U) != 0, (word & 0x00400000U) != 0};
    }
    return std::nullopt;
}

bool accepts_native_lse(std::uint32_t word) noexcept { return decode_native_lse(word).has_value(); }

std::vector<std::uint8_t> emit_native_lse(std::uint32_t word) {
    const auto decoded = decode_native_lse(word);
    if (!decoded) throw std::invalid_argument("unsupported native single-element LSE encoding");
    const auto& op = *decoded;
    Emitter out;
    out.bytes.reserve(96);
    if (op.operation == NativeLseOperation::compare_swap) {
        // Snapshot both guest inputs before Rs is overwritten, including aliases.
        out.load_guest(0, op.source, op.bytes);
        out.load_guest(8, op.target, op.bytes);
        out.locked(op.bytes == 1 ? 0xb0 : 0xb1, op.bytes);
        out.byte(0x41); out.byte(0x0f); out.byte(0x94); out.byte(0xc1); // SETE R9B
        out.byte(0x45); out.byte(0x0f); out.byte(0xb6); out.byte(0xc9); // MOVZX R9D,R9B
        out.store_guest(0, op.source);
    } else if (op.operation == NativeLseOperation::add ||
               op.operation == NativeLseOperation::swap) {
        out.load_guest(8, op.source, op.bytes);
        if (op.operation == NativeLseOperation::add) {
            out.locked(op.bytes == 1 ? 0xc0 : 0xc1, op.bytes); // LOCK XADD
        } else {
            if (op.bytes == 2) out.byte(0x66);
            out.byte(op.bytes == 8 ? 0x4c : 0x44);
            out.byte(op.bytes == 1 ? 0x86 : 0x87); out.byte(0x02); // XCHG implicitly locks
        }
        out.store_guest(8, op.target);
        out.wrote();
    } else {
        // CMPXCHG retries return the newest memory value in RAX. Rs stays in
        // R9 for every retry; Rt commits only after the single atomic effect.
        out.load_guest(9, op.source, op.bytes);
        out.load_memory(op.bytes);
        const auto retry = out.bytes.size();
        const bool wide = op.bytes == 8;
        if (op.operation == NativeLseOperation::clear) {
            out.move(8, 9, wide);
            out.byte(wide ? 0x49 : 0x41); out.byte(0xf7); out.byte(0xd0); // NOT R8
            out.byte(wide ? 0x49 : 0x41); out.byte(0x21); out.byte(0xc0); // AND R8,RAX
        } else if (op.operation == NativeLseOperation::eor ||
                   op.operation == NativeLseOperation::set) {
            out.move(8, 0, wide);
            out.byte(wide ? 0x4d : 0x45);
            out.byte(op.operation == NativeLseOperation::eor ? 0x31 : 0x09);
            out.byte(0xc8); // XOR/OR R8,R9
        } else {
            out.move(8, 9, wide);
            out.compare_source(op.bytes);
            const auto condition = op.operation == NativeLseOperation::smax ? 0x4d
                : op.operation == NativeLseOperation::smin ? 0x4e
                : op.operation == NativeLseOperation::umax ? 0x43 : 0x46;
            out.byte(wide ? 0x4c : 0x44); out.byte(0x0f);
            out.byte(static_cast<std::uint8_t>(condition)); out.byte(0xc0); // CMOV R8,RAX
        }
        out.locked(op.bytes == 1 ? 0xb0 : 0xb1, op.bytes);
        out.retry(retry);
        out.store_guest(0, op.target);
        out.wrote();
    }
    return std::move(out.bytes);
}

} // namespace transmute::jit_detail
