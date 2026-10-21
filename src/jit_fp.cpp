#include "jit_fp.hpp"

#include "transmute/a64.hpp"

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#include <cpuid.h>
#endif

namespace transmute::jit_detail {
namespace {

struct Instruction {
    std::uint8_t arithmetic_opcode;
    unsigned destination, source, second, accumulator, lanes;
    bool wide, scalar, quad, fused, compare, compare_zero;
    bool indexed{};
    unsigned index{};
    bool integer_conversion{};
    bool integer_wide{};
    bool integer_unsigned{};
    unsigned fractional_bits{};
};

std::optional<Instruction> decode(std::uint32_t word) noexcept {
    const bool integer_conversion = (word & 0x7fbefc00U) == 0x1e220000U;
    const bool scaled_integer_conversion = (word & 0x7f3e0000U) == 0x1e020000U;
    if (integer_conversion || scaled_integer_conversion) {
        const auto type = (word >> 22) & 3U;
        const bool source_wide = (word & 0x80000000U) != 0;
        const auto scale = scaled_integer_conversion ? 64U - ((word >> 10) & 63U) : 0U;
        // Only S/D. W fixed-point forms encode scales1..32, X scales1..64.
        if (type > 1 || (!source_wide && scale > 32)) return std::nullopt;
        return Instruction{0x2a, word & 31U, (word >> 5) & 31U, 0, 0, 1,
                           type == 1, true, false, false, false, false,
                           false, 0, true, source_wide,
                           (word & 0x00010000U) != 0, scale};
    }
    const auto indexed_class = (word & 0xbfc0f400U) & ~0x10400000U;
    if (indexed_class == 0x0f801000U || indexed_class == 0x0f805000U ||
        indexed_class == 0x0f809000U) {
        const bool scalar = (word & 0x10000000U) != 0;
        const bool quad = (word & 0x40000000U) != 0;
        const bool wide = (word & 0x00400000U) != 0;
        // Scalar S/D requires Q=1. D has two source elements, so L is reserved.
        if ((scalar && !quad) || (wide && (!quad || (word & 0x00200000U) != 0))) {
            return std::nullopt;
        }
        const bool fused = indexed_class != 0x0f809000U;
        const bool subtract = indexed_class == 0x0f805000U;
        const auto opcode = fused
            ? (scalar ? (subtract ? 0xad : 0xa9) : (subtract ? 0xac : 0xa8))
            : 0x59;
        const auto index = wide ? (word >> 11) & 1U
            : (((word >> 11) & 1U) << 1) | ((word >> 21) & 1U);
        return Instruction{static_cast<std::uint8_t>(opcode), word & 31U,
                           (word >> 5) & 31U, (word >> 16) & 31U,
                           fused ? word & 31U : 0U,
                           scalar ? 1U : (quad ? 16U : 8U) / (wide ? 8U : 4U),
                           wide, scalar, quad, fused, false, false, true, index};
    }
    if ((word & 0xff20fc07U) == 0x1e202000U) {
        const auto type = (word >> 22) & 3U;
        const auto second = (word >> 16) & 31U;
        const bool zero = (word & 8U) != 0;
        // H/type=10 are outside this S/D module. The zero form requires Rm=0.
        if (type > 1 || (zero && second != 0)) return std::nullopt;
        return Instruction{static_cast<std::uint8_t>((word & 0x10U) != 0 ? 0x2f : 0x2e),
                           0, (word >> 5) & 31U, second, 0, 1,
                           type == 1, true, false, false, true, zero};
    }
    std::uint8_t arithmetic_opcode = 0;
    bool scalar = false;
    bool fused = false;
    unsigned accumulator = 0;
    if ((word & 0xff000000U) == 0x1f000000U) {
        scalar = true;
        fused = true;
        accumulator = (word >> 10) & 31U;
        const bool negate_addend = (word & 0x00200000U) != 0;
        const bool negate_product = negate_addend != ((word & 0x00008000U) != 0);
        arithmetic_opcode = negate_product
            ? (negate_addend ? 0xaf : 0xad) // -product-addend / addend-product
            : (negate_addend ? 0xab : 0xa9); // product-addend / product+addend
    }
    switch (word & 0xff20fc00U) {
    case 0x1e202800U: arithmetic_opcode = 0x58; scalar = true; break; // FADD
    case 0x1e203800U: arithmetic_opcode = 0x5c; scalar = true; break; // FSUB
    case 0x1e200800U: arithmetic_opcode = 0x59; scalar = true; break; // FMUL
    case 0x1e201800U: arithmetic_opcode = 0x5e; scalar = true; break; // FDIV
    default: break;
    }
    const bool wide = (word & 0x00400000U) != 0;
    const bool quad = (word & 0x40000000U) != 0;
    if (scalar) {
        // type=10 is reserved and type=11 is half precision, not S/D.
        if (((word >> 22) & 3U) > 1) return std::nullopt;
    } else {
        switch (word & 0xbfa0fc00U) {
        case 0x0e20d400U: arithmetic_opcode = 0x58; break; // FADD
        case 0x0ea0d400U: arithmetic_opcode = 0x5c; break; // FSUB
        case 0x2e20dc00U: arithmetic_opcode = 0x59; break; // FMUL
        case 0x0e20cc00U: arithmetic_opcode = 0xa8; fused = true; break; // FMLA
        case 0x0ea0cc00U: arithmetic_opcode = 0xac; fused = true; break; // FMLS
        default: return std::nullopt;
        }
        if (wide && !quad) return std::nullopt;
        if (fused) accumulator = word & 31U;
    }
    return Instruction{arithmetic_opcode, word & 31U, (word >> 5) & 31U,
                       (word >> 16) & 31U, accumulator,
                       scalar ? 1U : (quad ? 16U : 8U) / (wide ? 8U : 4U),
                       wide, scalar, quad, fused, false, false};
}

struct Emitter {
    GuardedFpFragment fragment;

    void byte(std::uint8_t value) { fragment.bytes.push_back(value); }
    void word(std::uint32_t value) {
        for (unsigned i = 0; i != 4; ++i) {
            byte(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }
    void wide_word(std::uint64_t value) {
        word(static_cast<std::uint32_t>(value));
        word(static_cast<std::uint32_t>(value >> 32));
    }
    std::size_t jump(std::uint8_t condition) {
        byte(0x0f); byte(condition);
        const auto displacement = fragment.bytes.size();
        word(0);
        return displacement;
    }
    void bailout(std::uint8_t condition) {
        fragment.bailout_displacements.push_back(jump(condition));
    }
    void patch(std::size_t displacement) {
        const auto distance = static_cast<std::uint32_t>(
            fragment.bytes.size() - displacement - 4);
        for (unsigned i = 0; i != 4; ++i) {
            fragment.bytes[displacement + i] =
                static_cast<std::uint8_t>(distance >> (i * 8));
        }
    }
    void load_bits(std::uint32_t offset, bool wide, bool scratch) {
        if (scratch) byte(wide ? 0x49 : 0x41); // R11 base
        else if (wide) byte(0x48);
        byte(0x8b); byte(scratch ? 0x83 : 0x81); word(offset); // RAX/EAX
    }
    void reject_subnormal(bool wide) {
        // Only used for FPCR.FZ. NaNs and infinities retain the direct native
        // result; a genuine subnormal result needs ARM's flush/status oracle.
        if (wide) byte(0x48);
        byte(0x89); byte(0xc2); // mov rdx/edx, rax/eax
        if (wide) byte(0x48);
        byte(0xc1); byte(0xea); byte(wide ? 52 : 23);
        byte(0x81); byte(0xe2); word(wide ? 0x7ff : 0xff);
        byte(0x85); byte(0xd2);
        const auto normal = jump(0x85); // jne: nonzero exponent
        if (wide) byte(0x48);
        byte(0xd1); byte(0xe0); // sign-free zero/subnormal classification
        if (wide) byte(0x48);
        byte(0x85); byte(0xc0);
        bailout(0x85);
        patch(normal);
    }
    void packed(std::uint8_t opcode, unsigned destination, unsigned source) {
        byte(0x66); byte(0x0f); byte(opcode);
        byte(static_cast<std::uint8_t>(0xc0U | (destination << 3) | source));
    }
    void packed_shift(bool wide, bool left, unsigned xmm, std::uint8_t count) {
        byte(0x66); byte(0x0f); byte(wide ? 0x73 : 0x72);
        byte(static_cast<std::uint8_t>((left ? 0xf0U : 0xd0U) | xmm));
        byte(count);
    }
    void reject_minimum_normal(bool wide) {
        // Compare every active result lane with minimum normal after stripping
        // its sign. Inactive lanes are zero and cannot match this constant.
        packed(0x6f, 1, 0);
        packed_shift(wide, true, 1, 1);
        packed_shift(wide, false, 1, 1);
        packed(0x76, 2, 2);
        packed_shift(wide, false, 2, wide ? 63 : 31);
        packed_shift(wide, true, 2, wide ? 52 : 23);
        packed(0x76, 1, 2);
        if (wide) {
            byte(0x66); byte(0x0f); byte(0x70); byte(0xd9); byte(0xb1);
            packed(0xdb, 1, 3);
        }
        byte(0x66); byte(0x0f); byte(0xd7); byte(0xd1); // pmovmskb edx,xmm1
        byte(0x85); byte(0xd2);
        bailout(0x85);
    }
    std::size_t skip_finite_inputs(bool wide) {
        // NaN priority can hide native denormal-input status. Only a result
        // with an all-ones exponent needs the explicit FZ operand scan.
        packed(0x6f, 1, 0);
        packed_shift(wide, true, 1, 1);
        packed_shift(wide, false, 1, wide ? 53 : 24);
        packed(0x76, 2, 2);
        packed_shift(wide, false, 2, wide ? 53 : 24);
        packed(0x76, 1, 2);
        if (wide) {
            byte(0x66); byte(0x0f); byte(0x70); byte(0xd9); byte(0xb1);
            packed(0xdb, 1, 3);
        }
        byte(0x66); byte(0x0f); byte(0xd7); byte(0xd1);
        byte(0x85); byte(0xd2);
        return jump(0x84);
    }
    void mxcsr(bool save, std::uint32_t offset) {
        byte(0x41); byte(0x0f); byte(0xae);
        byte(save ? 0x9b : 0x93); word(offset); // /3 ST, /2 LD [r11+disp32]
    }
    void store_scratch(unsigned gpr, std::uint32_t offset) {
        byte(0x41); byte(0x89);
        byte(static_cast<std::uint8_t>(0x83U | (gpr << 3))); word(offset);
    }
    void set_scratch(std::uint32_t offset, std::uint32_t value) {
        byte(0x41); byte(0xc7); byte(0x83); word(offset); word(value);
    }
    void merge_status(std::uint32_t offset) {
        // Ignore native denormal-input status. Guest FZ operations requiring
        // IDC take the oracle; IEEE exception flags otherwise map directly.
        load_bits(offset, false, true);
        byte(0x89); byte(0xc2);
        byte(0x83); byte(0xe2); byte(1); // IOC
        byte(0xd1); byte(0xe8);
        byte(0x83); byte(0xe0); byte(0x1e); // DZC/OFC/UFC/IXC
        byte(0x09); byte(0xd0);
        byte(0x09); byte(0x81);
        word(static_cast<std::uint32_t>(offsetof(CpuState, fpsr)));
    }
    void comparison_flags() {
        // UCOMI/COMI produce (ZF,PF,CF): greater=000, less=001,
        // equal=100, unordered=111. MOV/CMOV preserve those native flags.
        byte(0x41); byte(0xb8); word(0x20000000U); // ARM greater: C
        byte(0xba); word(0x80000000U); // ARM less: N
        byte(0x44); byte(0x0f); byte(0x42); byte(0xc2); // cmovb r8d,edx
        byte(0xba); word(0x60000000U); // ARM equal: Z,C
        byte(0x44); byte(0x0f); byte(0x44); byte(0xc2); // cmove r8d,edx
        byte(0xba); word(0x30000000U); // ARM unordered: C,V
        byte(0x44); byte(0x0f); byte(0x4a); byte(0xc2); // cmovp r8d,edx
    }
    void load_vector(unsigned xmm, unsigned guest, const Instruction& op) {
        const auto offset = static_cast<std::uint32_t>(offsetof(CpuState, v) + guest * 16);
        if (op.scalar && !op.wide) {
            byte(0x66); byte(0x0f); byte(0x6e); // movd clears upper 96
        } else {
            byte(0xf3); byte(0x0f);
            byte(op.scalar || !op.quad ? 0x7e : 0x6f); // movq / movdqu
        }
        byte(static_cast<std::uint8_t>(0x81U | (xmm << 3))); word(offset);
    }
    void load_indexed_second(const Instruction& op) {
        const auto offset = static_cast<std::uint32_t>(offsetof(CpuState, v) +
            op.second * 16 + op.index * (op.wide ? 8U : 4U));
        // Integer loads/shuffles preserve FP status and ignore every unselected
        // source element, including denormals and signaling NaNs.
        if (op.wide) { byte(0xf3); byte(0x0f); byte(0x7e); }
        else { byte(0x66); byte(0x0f); byte(0x6e); }
        byte(0x89); word(offset); // movq / movd xmm1,[rcx+selected element]
        if (!op.scalar) {
            if (op.wide) packed(0x6c, 1, 1); // punpcklqdq xmm1,xmm1
            else {
                byte(0x66); byte(0x0f); byte(0x70); byte(0xc9); byte(0);
            }
            if (!op.quad) {
                // An inactive zero lane multiplied by a broadcast infinity
                // would otherwise raise a spurious invalid-operation flag.
                byte(0xf3); byte(0x0f); byte(0x7e); byte(0xc9);
            }
        }
    }
    void convert_integer(const Instruction& op) {
        packed(0xef, 0, 0); // Scalar CVTSI preserves inactive bits; clear them first.
        if (op.source == 31) { byte(0x31); byte(0xc0); } // WZR/XZR, never SP.
        else load_bits(static_cast<std::uint32_t>(offsetof(CpuState, x) +
                           op.source * 8), op.integer_wide, false);
        const auto convert = [&] {
            byte(op.wide ? 0xf2 : 0xf3);
            // Zero-extended W unsigned inputs fit the signed64 host converter.
            if (op.integer_wide || op.integer_unsigned) byte(0x48);
            byte(0x0f); byte(0x2a); byte(0xc0); // cvtsi2sd/ss xmm0,rax/eax
        };
        if (op.integer_unsigned && op.integer_wide) {
            byte(0x48); byte(0x85); byte(0xc0);
            const auto signed_range = jump(0x89); // jns unsigned value<2^63
            // At this magnitude every S/D spacing exceeds1. Halving while
            // retaining the discarded low bit as a sticky odd bit preserves
            // ties and all directed rounding. A discarded odd input remains
            // inexact; an exactly representable even input stays exact.
            byte(0x48); byte(0x89); byte(0xc2);
            byte(0x83); byte(0xe2); byte(1);
            byte(0x48); byte(0xd1); byte(0xe8);
            byte(0x48); byte(0x09); byte(0xd0);
            convert();
            byte(op.wide ? 0xf2 : 0xf3);
            byte(0x0f); byte(0x58); byte(0xc0); // exact multiply by2 via addition
            byte(0xe9); const auto done = fragment.bytes.size(); word(0);
            patch(signed_range);
            convert();
            patch(done);
        } else convert();
        if (op.fractional_bits != 0) {
            const auto factor = static_cast<std::uint64_t>(
                (op.wide ? 1023U : 127U) - op.fractional_bits) <<
                (op.wide ? 52U : 23U);
            if (op.wide) { byte(0x48); byte(0xb8); wide_word(factor); }
            else { byte(0xb8); word(static_cast<std::uint32_t>(factor)); }
            byte(0x66); if (op.wide) byte(0x48);
            byte(0x0f); byte(0x6e); byte(0xc8); // movq/d xmm1,rax/eax
            byte(op.wide ? 0xf2 : 0xf3);
            byte(0x0f); byte(0x59); byte(0xc1);
            // Scale<=64: every nonzero result remains normal in S and D.
            // Exact binary scaling cannot add rounding, overflow or underflow.
        }
    }
};

static_assert(offsetof(NativeFpScratch, saved_mxcsr) == 0);
static_assert(offsetof(NativeFpScratch, work_mxcsr) == 4);
static_assert(offsetof(NativeFpScratch, result) == 8);
static_assert(offsetof(NativeFpScratch, committed_mxcsr) == 24);
static_assert(offsetof(NativeFpScratch, session_active) == 28);
static_assert(sizeof(NativeFpScratch) == 32);

} // namespace

bool native_fma_available() noexcept {
    static const bool available = []() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        int feature[4]{};
        __cpuid(feature, 1);
        constexpr auto required = (1U << 12) | (1U << 27) | (1U << 28);
        if ((static_cast<unsigned>(feature[2]) & required) != required) return false;
        return (_xgetbv(0) & 6) == 6;
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
        unsigned eax, ebx, ecx, edx;
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
        constexpr auto required = (1U << 12) | (1U << 27) | (1U << 28);
        if ((ecx & required) != required) return false;
        __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
        return (eax & 6U) == 6U;
#else
        return false;
#endif
    }();
    return available;
}

bool accepts_guarded_fp(std::uint32_t instruction, bool fma_available) noexcept {
    const auto op = decode(instruction);
    return op && (!op->fused || fma_available);
}

bool accepts_guarded_fp(std::uint32_t instruction) noexcept {
    const auto op = decode(instruction);
    return op && (!op->fused || native_fma_available());
}

GuardedFpFragment emit_guarded_fp(std::uint32_t instruction,
                                 std::uint32_t scratch_offset,
                                 NativeFpEmissionMode mode) {
    const auto decoded = decode(instruction);
    if (!decoded || (decoded->fused && !native_fma_available()) || scratch_offset >
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) -
                sizeof(NativeFpScratch)) {
        throw std::invalid_argument("unsupported guarded native FP encoding or scratch offset");
    }
    const auto& op = *decoded;
    Emitter out;
    out.fragment.bytes.reserve(650);
    const auto saved = scratch_offset;
    const auto work = scratch_offset + 4;
    const auto result = scratch_offset + 8;
    const auto committed = scratch_offset + 24;
    const auto active = scratch_offset + 28;
    const bool session = mode == NativeFpEmissionMode::region_session;

    std::optional<std::size_t> already_active;
    if (session) {
        // FPCR/FPSR writes and host callbacks terminate native regions, so the
        // guest rounding policy stays constant throughout an active session.
        out.byte(0x41); out.byte(0x83); out.byte(0xbb); out.word(active); out.byte(0);
        already_active = out.jump(0x85);
    }

    // Only rounding, FZ, DN and AHP may be set. Trap enables and unsupported
    // controls are rejected before host FP state or architectural state changes.
    out.load_bits(static_cast<std::uint32_t>(offsetof(CpuState, fpcr)), false, false);
    out.byte(0xa9); out.word(~0x07c00000U);
    out.bailout(0x85);
    out.byte(0x89); out.byte(0xc2); // rounding bits into EDX
    out.byte(0xc1); out.byte(0xea); out.byte(22);
    out.byte(0x83); out.byte(0xe2); out.byte(3);
    // ARM +infinity/-infinity encodings are reversed in MXCSR.
    out.byte(0x41); out.byte(0x89); out.byte(0xd0); // mov r8d,edx
    out.byte(0x41); out.byte(0x83); out.byte(0xe0); out.byte(1);
    out.byte(0x41); out.byte(0xd1); out.byte(0xe0);
    out.byte(0xd1); out.byte(0xea);
    out.byte(0x44); out.byte(0x09); out.byte(0xc2);
    out.byte(0xc1); out.byte(0xe2); out.byte(13);
    out.byte(0x81); out.byte(0xca); out.word(0x1f80); // mask traps, DAZ/FZ off
    out.store_scratch(2, work);
    if (session) out.store_scratch(2, committed);

    out.mxcsr(true, saved);
    out.mxcsr(false, work);
    if (session) {
        out.set_scratch(active, 1);
        out.patch(*already_active);
    }
    if (op.integer_conversion) {
        out.convert_integer(op);
    } else {
        out.load_vector(0, op.source, op);
        if (op.compare_zero) out.packed(0xef, 1, 1);
        else if (op.indexed) out.load_indexed_second(op);
        else out.load_vector(1, op.second, op);
    }
    if (op.integer_conversion) {
        // No FP source operand exists, and every result is finite and zero or
        // normal. Existing arithmetic FZ/tininess guards therefore do not apply.
    } else if (op.compare) {
        // FCMP only signals sNaN (UCOMI); FCMPE signals every NaN (COMI).
        if (op.wide) out.byte(0x66);
        out.byte(0x0f); out.byte(op.arithmetic_opcode); out.byte(0xc1);
        out.comparison_flags(); // staged in R8D until all guards accept
    } else if (op.fused) {
        out.load_vector(2, op.accumulator, op);
        // VFMADD/VFNMADD/VFMSUB/VFNMSUB213 S/D: Rn*Rm with Ra/prior Rd.
        // FMA3 performs exactly one rounding, including intermediate products
        // outside the representable range whose fused final result is finite.
        out.byte(0xc4); out.byte(0xe2); out.byte(op.wide ? 0xf1 : 0x71);
        out.byte(op.arithmetic_opcode); out.byte(0xc2);
        out.byte(0xc5); out.byte(0xf8); out.byte(0x77); // vzeroupper
    } else {
        if (op.scalar) out.byte(op.wide ? 0xf2 : 0xf3);
        else if (op.wide) out.byte(0x66);
        out.byte(0x0f); out.byte(op.arithmetic_opcode); out.byte(0xc1);
    }
    if (!op.scalar && !op.quad) {
        // In round-down mode SUBPS on the inactive +0 lanes can produce -0.
        // ARM Q=0 still clears the complete upper half of the destination.
        out.byte(0xf3); out.byte(0x0f); out.byte(0x7e); out.byte(0xc0);
    }
    out.mxcsr(true, work);
    if (!session) out.mxcsr(false, saved);
    if (!op.integer_conversion) {
        // FZ remains exact: native denormal input or tiny output takes the
        // oracle, which records IDC and flushes the signed-zero result.
        out.byte(0xf7); out.byte(0x81);
        out.word(static_cast<std::uint32_t>(offsetof(CpuState, fpcr)));
        out.word(1U << 24);
        const auto without_flush = out.jump(0x84);
        out.load_bits(work, false, true);
        out.byte(0xa9); out.word(0x12); // native denormal-input/underflow flags
        out.bailout(0x85);
        const auto check_input = [&](unsigned guest, bool indexed = false) {
            const auto lanes = indexed ? 1U : op.lanes;
            for (unsigned lane = 0; lane != lanes; ++lane) {
                out.load_bits(static_cast<std::uint32_t>(offsetof(CpuState, v) + guest * 16 +
                                  (indexed ? op.index : lane) * (op.wide ? 8U : 4U)),
                              op.wide, false);
                out.reject_subnormal(op.wide);
            }
        };
        if (op.compare) {
            // NaN priority may suppress native denormal status for the other
            // input. ARM FZ still records IDC, so inspect actual input bits.
            check_input(op.source);
            if (!op.compare_zero) check_input(op.second);
        } else {
            out.byte(0xf3); out.byte(0x41); out.byte(0x0f); out.byte(0x7f);
            out.byte(0x83); out.word(result); // private result snapshot
            for (unsigned lane = 0; lane != op.lanes; ++lane) {
                out.load_bits(result + lane * (op.wide ? 8U : 4U), op.wide, true);
                out.reject_subnormal(op.wide);
            }
            const auto finite = out.skip_finite_inputs(op.wide);
            check_input(op.source);
            check_input(op.second, op.indexed);
            if (op.fused) check_input(op.accumulator);
            out.patch(finite);
        }
        out.patch(without_flush);
        // Pre-round tininess can differ only when an inexact arithmetic
        // result rounds up to minimum normal. GP conversion cannot reach it.
        if (!op.compare) {
            out.load_bits(work, false, true);
            out.byte(0xa9); out.word(0x20);
            const auto exact = out.jump(0x84);
            out.reject_minimum_normal(op.wide);
            out.patch(exact);
        }
    }
    if (session) {
        // Failed operations may already have raised native flags. Preserve a
        // separate snapshot only after every guard accepted the instruction.
        out.load_bits(work, false, true);
        out.store_scratch(0, committed);
    } else {
        out.merge_status(work);
    }
    if (op.compare) {
        out.byte(0x44); out.byte(0x89); out.byte(0x81);
        out.word(static_cast<std::uint32_t>(offsetof(CpuState, nzcv)));
    } else {
        out.byte(0xf3); out.byte(0x0f); out.byte(0x7f); out.byte(0x81);
        out.word(static_cast<std::uint32_t>(offsetof(CpuState, v) + op.destination * 16));
    }
    return std::move(out.fragment);
}

std::vector<std::uint8_t> emit_fp_session_finish(std::uint32_t scratch_offset) {
    if (scratch_offset >
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) -
                sizeof(NativeFpScratch)) {
        throw std::invalid_argument("unsupported native FP session scratch offset");
    }
    Emitter out;
    out.byte(0x41); out.byte(0x83); out.byte(0xbb);
    out.word(scratch_offset + 28); out.byte(0);
    const auto inactive = out.jump(0x84);
    out.merge_status(scratch_offset + 24);
    out.mxcsr(false, scratch_offset);
    out.set_scratch(scratch_offset + 28, 0);
    out.patch(inactive);
    return std::move(out.fragment.bytes);
}

} // namespace transmute::jit_detail
