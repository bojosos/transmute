#pragma once

// Internal helpers shared by the NEON emitters (owner: neon).

#include "fx/common/cpu_features.hpp"
#include "fx/translator/emit_context.hpp"
#include "fx/translator/emit_neon.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace transmute::fx::neon {

using x64::Gp;
using x64::Mem;
using x64::SseOp;
using x64::VexOp;
using x64::Xmm;

// ---- legacy SSE opcodes (prefix, map, opcode); VEX forms derive from them ----
namespace op {
// map 1 (0F)
inline constexpr SseOp punpcklbw{0x66, 1, 0x60}, punpcklwd{0x66, 1, 0x61}, punpckldq{0x66, 1, 0x62};
inline constexpr SseOp packsswb{0x66, 1, 0x63}, pcmpgtb{0x66, 1, 0x64}, pcmpgtw{0x66, 1, 0x65};
inline constexpr SseOp pcmpgtd{0x66, 1, 0x66}, packuswb{0x66, 1, 0x67}, punpckhbw{0x66, 1, 0x68};
inline constexpr SseOp punpckhwd{0x66, 1, 0x69}, punpckhdq{0x66, 1, 0x6a}, packssdw{0x66, 1, 0x6b};
inline constexpr SseOp punpcklqdq{0x66, 1, 0x6c}, punpckhqdq{0x66, 1, 0x6d};
inline constexpr SseOp movdqa{0x66, 1, 0x6f}, movdqu{0xf3, 1, 0x6f}, movaps{0, 1, 0x28};
inline constexpr SseOp pshufd{0x66, 1, 0x70}, pshuflw{0xf2, 1, 0x70}, pshufhw{0xf3, 1, 0x70};
inline constexpr SseOp pcmpeqb{0x66, 1, 0x74}, pcmpeqw{0x66, 1, 0x75}, pcmpeqd{0x66, 1, 0x76};
inline constexpr SseOp psrlw{0x66, 1, 0xd1}, psrld{0x66, 1, 0xd2}, psrlq{0x66, 1, 0xd3};
inline constexpr SseOp paddq{0x66, 1, 0xd4}, pmullw{0x66, 1, 0xd5};
inline constexpr SseOp psubusb{0x66, 1, 0xd8}, psubusw{0x66, 1, 0xd9}, pminub{0x66, 1, 0xda};
inline constexpr SseOp pand{0x66, 1, 0xdb}, paddusb{0x66, 1, 0xdc}, paddusw{0x66, 1, 0xdd};
inline constexpr SseOp pmaxub{0x66, 1, 0xde}, pandn{0x66, 1, 0xdf};
inline constexpr SseOp pavgb{0x66, 1, 0xe0}, psraw{0x66, 1, 0xe1}, psrad{0x66, 1, 0xe2};
inline constexpr SseOp pavgw{0x66, 1, 0xe3}, pmulhuw{0x66, 1, 0xe4}, pmulhw{0x66, 1, 0xe5};
inline constexpr SseOp psubsb{0x66, 1, 0xe8}, psubsw{0x66, 1, 0xe9}, pminsw{0x66, 1, 0xea};
inline constexpr SseOp por{0x66, 1, 0xeb}, paddsb{0x66, 1, 0xec}, paddsw{0x66, 1, 0xed};
inline constexpr SseOp pmaxsw{0x66, 1, 0xee}, pxor{0x66, 1, 0xef};
inline constexpr SseOp psllw{0x66, 1, 0xf1}, pslld{0x66, 1, 0xf2}, psllq{0x66, 1, 0xf3};
inline constexpr SseOp pmuludq{0x66, 1, 0xf4}, pmaddwd{0x66, 1, 0xf5}, psadbw{0x66, 1, 0xf6};
inline constexpr SseOp psubb{0x66, 1, 0xf8}, psubw{0x66, 1, 0xf9}, psubd{0x66, 1, 0xfa};
inline constexpr SseOp psubq{0x66, 1, 0xfb}, paddb{0x66, 1, 0xfc}, paddw{0x66, 1, 0xfd};
inline constexpr SseOp paddd{0x66, 1, 0xfe};
inline constexpr SseOp shufps{0, 1, 0xc6}, shufpd{0x66, 1, 0xc6};
inline constexpr SseOp movq_load{0xf3, 1, 0x7e};            // movq xmm, xmm/m64 (zero upper)
inline constexpr SseOp movhlps{0, 1, 0x12}, movlhps{0, 1, 0x16};
inline constexpr SseOp movlps_load{0, 1, 0x12}, movhps_load{0, 1, 0x16};
inline constexpr SseOp movlps_store{0, 1, 0x13}, movhps_store{0, 1, 0x17};
inline constexpr SseOp movddup{0xf2, 1, 0x12};
inline constexpr SseOp andps{0, 1, 0x54}, andnps{0, 1, 0x55}, orps{0, 1, 0x56}, xorps{0, 1, 0x57};
inline constexpr SseOp cvtdq2ps{0, 1, 0x5b};
// map 2 (0F38)
inline constexpr SseOp pshufb{0x66, 2, 0x00}, phaddw{0x66, 2, 0x01}, phaddd{0x66, 2, 0x02};
inline constexpr SseOp pmaddubsw{0x66, 2, 0x04}, phsubw{0x66, 2, 0x05}, phsubd{0x66, 2, 0x06};
inline constexpr SseOp psignb{0x66, 2, 0x08}, psignw{0x66, 2, 0x09}, psignd{0x66, 2, 0x0a};
inline constexpr SseOp pmulhrsw{0x66, 2, 0x0b};
inline constexpr SseOp pabsb{0x66, 2, 0x1c}, pabsw{0x66, 2, 0x1d}, pabsd{0x66, 2, 0x1e};
inline constexpr SseOp pmovsxbw{0x66, 2, 0x20}, pmovsxbd{0x66, 2, 0x21}, pmovsxbq{0x66, 2, 0x22};
inline constexpr SseOp pmovsxwd{0x66, 2, 0x23}, pmovsxwq{0x66, 2, 0x24}, pmovsxdq{0x66, 2, 0x25};
inline constexpr SseOp pmuldq{0x66, 2, 0x28}, pcmpeqq{0x66, 2, 0x29}, packusdw{0x66, 2, 0x2b};
inline constexpr SseOp pmovzxbw{0x66, 2, 0x30}, pmovzxbd{0x66, 2, 0x31}, pmovzxbq{0x66, 2, 0x32};
inline constexpr SseOp pmovzxwd{0x66, 2, 0x33}, pmovzxwq{0x66, 2, 0x34}, pmovzxdq{0x66, 2, 0x35};
inline constexpr SseOp pcmpgtq{0x66, 2, 0x37}, pminsb{0x66, 2, 0x38}, pminsd{0x66, 2, 0x39};
inline constexpr SseOp pminuw{0x66, 2, 0x3a}, pminud{0x66, 2, 0x3b}, pmaxsb{0x66, 2, 0x3c};
inline constexpr SseOp pmaxsd{0x66, 2, 0x3d}, pmaxuw{0x66, 2, 0x3e}, pmaxud{0x66, 2, 0x3f};
inline constexpr SseOp pmulld{0x66, 2, 0x40}, phminposuw{0x66, 2, 0x41};
// map 3 (0F3A)
inline constexpr SseOp blendps{0x66, 3, 0x0c}, blendpd{0x66, 3, 0x0d}, pblendw{0x66, 3, 0x0e};
inline constexpr SseOp palignr{0x66, 3, 0x0f}, insertps{0x66, 3, 0x21}, pclmulqdq{0x66, 3, 0x44};
} // namespace op

// VEX-only opcodes.
namespace vop {
inline constexpr VexOp vpbroadcastb{1, 2, 0x78, false}, vpbroadcastw{1, 2, 0x79, false};
inline constexpr VexOp vpbroadcastd{1, 2, 0x58, false}, vpbroadcastq{1, 2, 0x59, false};
inline constexpr VexOp vbroadcastss{1, 2, 0x18, false};
inline constexpr VexOp vpsllvd{1, 2, 0x47, false}, vpsllvq{1, 2, 0x47, true};
inline constexpr VexOp vpsrlvd{1, 2, 0x45, false}, vpsrlvq{1, 2, 0x45, true};
inline constexpr VexOp vpsravd{1, 2, 0x46, false};
inline constexpr VexOp vpblendvb{1, 3, 0x4c, false}, vblendvps{1, 3, 0x4a, false}, vblendvpd{1, 3, 0x4b, false};
inline constexpr VexOp vpblendd{1, 3, 0x02, false};
} // namespace vop

// Element-size indexed opcode tables (B, H, S, D). `none` marks gaps.
inline constexpr SseOp kNone{0, 0, 0};
inline constexpr SseOp kAdd[4] = {op::paddb, op::paddw, op::paddd, op::paddq};
inline constexpr SseOp kSub[4] = {op::psubb, op::psubw, op::psubd, op::psubq};
inline constexpr SseOp kCmpEq[4] = {op::pcmpeqb, op::pcmpeqw, op::pcmpeqd, op::pcmpeqq};
inline constexpr SseOp kCmpGt[4] = {op::pcmpgtb, op::pcmpgtw, op::pcmpgtd, op::pcmpgtq};
inline constexpr SseOp kMaxS[3] = {op::pmaxsb, op::pmaxsw, op::pmaxsd};
inline constexpr SseOp kMinS[3] = {op::pminsb, op::pminsw, op::pminsd};
inline constexpr SseOp kMaxU[3] = {op::pmaxub, op::pmaxuw, op::pmaxud};
inline constexpr SseOp kMinU[3] = {op::pminub, op::pminuw, op::pminud};
inline constexpr SseOp kAbs[3] = {op::pabsb, op::pabsw, op::pabsd};
inline constexpr SseOp kUnpackLo[4] = {op::punpcklbw, op::punpcklwd, op::punpckldq, op::punpcklqdq};
inline constexpr SseOp kUnpackHi[4] = {op::punpckhbw, op::punpckhwd, op::punpckhdq, op::punpckhqdq};
inline constexpr SseOp kSext[3] = {op::pmovsxbw, op::pmovsxwd, op::pmovsxdq};   // narrow size -> 2x
inline constexpr SseOp kZext[3] = {op::pmovzxbw, op::pmovzxwd, op::pmovzxdq};

// ---- code-generation knobs ----------------------------------------------------
// TRANSMUTE_FX_NEON=<knobs> (read once; folded into the family names and so
// into translator_family_fingerprint()) restore earlier lowerings:
//   nofold  - no ADD/SUB address folding into SIMD&FP loads/stores
//             (EmitContext::address; FamilyEmitter::address_base)
//   noslot  - by-element / DUP (element) sources in clean cached registers
//             are not broadcast from their context slot (EmitContext::vec_slot)
//   notbltemp - single-table TBL computes its adjusted indices in a temp
//             (not in the destination)
//   oldshl  - USHL/SSHL by register: S lanes negate the count with zero/sub
//             (not vpsignd), D lanes use the generic clamped path (not the
//             AVX2 vpsllvq/vpsrlvq sequence)
//   nozero64 - 64-bit vector results are always zeroed above bit 64 (not
//             skipped when every source is known zero there and the
//             operation maps zero lanes to zero)
//   old     - all of the above
//   nomaskfusion - keep AND/USHL/ADDV mask reductions as separate instructions
//   notblderived - TBL computes its adjusted indices itself (tier 2 may
//             otherwise precompute them after the index register's
//             definitions: EmitContext::vec_derived)
//   oldvarshift - 16-bit lanes of register shifts (SSHL, URSHL, SQSHL, ...)
//             through the shift-by-1/2/4/8/16 blend chain instead of the
//             AVX2 dword variable shifts
//   oldstruct - LD3 (Q)/LD4/ST4 (Q) with every chunk in its own temp (8-10
//             XMM registers) instead of 6-7
//   oldpmull8 - PMULL/PMULL2 .8B through the compare-and-mask chain instead of
//             PMULLW by single-bit multipliers
//   oldmullelem - SMULL/SMLAL/SMLSL/SQDMULL by element on 16-bit lanes through
//             PMULLW/PMULHW and an unpack instead of one PMADDWD
//   nomullpair - SMULL|SMLAL + SMLAL by element pairs into one accumulator stay
//             two instructions (not one PMADDWD of the interleaved sources)
//   oldwidenhi - unsigned widening of a high half (UMULL2, UADDL2, USHLL2, ...)
//             through PSHUFD + PMOVZX instead of one PUNPCKH with zero
//   oldrshl - SRSHL/URSHL on 16-/32-bit lanes through the clamped generic
//             register shift instead of the variable shifts' own saturation
//   oldushl8 - USHL on byte lanes through the shift-by-1/2/4/8 blend chain
//             instead of multiplying by a PSHUFB-selected power of two
enum NeonKnob : std::uint32_t {
    neon_no_fold = 1u,
    neon_no_slot = 2u,
    neon_no_tbl_dest = 4u,
    neon_old_shl = 8u,
    neon_no_zero64 = 16u,
    neon_no_mask_fusion = 32u,
    neon_mask_no_rcx = 64u, // Test override: exercise the flags-saving guard.
    neon_no_tbl_derived = 128u,
    neon_old_var_shift = 256u,
    neon_old_pmull8 = 512u,
    neon_old_struct = 1024u,
    neon_old_mull_elem = 2048u,
    neon_no_mull_pair = 4096u,
    neon_old_widen_hi = 8192u,
    neon_old_rshl = 16384u,
    neon_old_ushl8 = 32768u,
};
std::uint32_t neon_knob_bits();
// Test override of the knobs (-1 = TRANSMUTE_FX_NEON). Not thread-safe.
void neon_set_knobs_for_testing(int bits);
inline bool neon_on(std::uint32_t knob) { return (neon_knob_bits() & knob) == 0; }
// True if bits [127:64] of V`index` are known zero at the start of this
// instruction (EmitContext::vec_zero_above; query before write_vec of that
// register). Always false with TRANSMUTE_FX_NEON=nozero64.
inline bool zero_above_64(const EmitContext& ctx, unsigned index) {
    return neon_on(neon_no_zero64) && ((ctx.vec_zero_above(64) >> index) & 1u) != 0;
}
// "+nofold..." suffix for the family names.
const char* neon_knob_suffix();

// ---- features -----------------------------------------------------------------
struct Features {
    bool avx{};
    bool avx2{};
    bool bmi2{};
    bool pclmul{};
};
const Features& features() noexcept;

// ---- 16-byte constants ----------------------------------------------------------
struct Const128 {
    std::uint64_t lo{};
    std::uint64_t hi{};
    bool operator==(const Const128&) const = default;
};
constexpr std::uint64_t rep8(std::uint8_t v) { return 0x0101010101010101ULL * v; }
constexpr std::uint64_t rep16(std::uint16_t v) { return 0x0001000100010001ULL * v; }
constexpr std::uint64_t rep32(std::uint32_t v) { return 0x0000000100000001ULL * v; }
constexpr Const128 splat8(std::uint8_t v) { return {rep8(v), rep8(v)}; }
constexpr Const128 splat16(std::uint16_t v) { return {rep16(v), rep16(v)}; }
constexpr Const128 splat32(std::uint32_t v) { return {rep32(v), rep32(v)}; }
constexpr Const128 splat64(std::uint64_t v) { return {v, v}; }
// Replicates the low `esize` bits of `value` into every element of size `size`.
constexpr Const128 splat(unsigned size, std::uint64_t value) {
    switch (size) {
    case 0: return splat8(static_cast<std::uint8_t>(value));
    case 1: return splat16(static_cast<std::uint16_t>(value));
    case 2: return splat32(static_cast<std::uint32_t>(value));
    default: return splat64(value);
    }
}
Const128 bytes_const(const std::uint8_t (&bytes)[16]);

// Rip-relative, 16-byte aligned memory operand holding `value` (see the
// constant pool notes in emit_neon.hpp).
Mem constant(EmitContext& ctx, Const128 value);

// Per-instruction constant bookkeeping (inline mode): the family entry points
// call begin before and flush after every instruction.
void begin_instruction_constants(x64::Assembler& as);
void flush_instruction_constants(x64::Assembler& as);

// ---- guest sources -----------------------------------------------------------------
// A read-only guest vector source (EmitContext::vsrc): its host register, or
// its GuestContext slot (16-byte aligned) as a memory operand. A memory
// source keeps the pre-instruction value until the instruction ends.
struct Src {
    Xmm reg{};
    Mem mem{};
    bool is_mem{};
    unsigned guest{};
    // Element `index` of size `size` (0..3) of an in-memory source.
    [[nodiscard]] Mem element(unsigned size, unsigned index) const {
        Mem m = mem;
        m.disp += static_cast<std::int32_t>(index << size);
        return m;
    }
};
Src source(EmitContext& ctx, unsigned index);
// A source whose element (S/D, `size` 2/3) is broadcast: its context slot
// when that is current (EmitContext::vec_slot), even if a clean copy sits in
// a register -- vbroadcastss / vmovddup from memory are one load, no shuffle;
// else source(). TRANSMUTE_FX_NEON=noslot: source().
Src element_source(EmitContext& ctx, unsigned index, unsigned size);
// The source in a register: an in-memory source is loaded with read_vec().
// Like any read, call it before write_vec() of the same instruction.
Xmm in_reg(EmitContext& ctx, const Src& s);
// Operands of d = n <op> m: the first in a register, the second a register or
// memory. m stays in memory if it is there; for commutative ops an in-memory
// n trades places with a register m. Call before write_vec().
struct BinOperands {
    Xmm first;
    Src second;
};
BinOperands bin_operands(EmitContext& ctx, const Src& n, const Src& m, bool commutative);

// ---- vector emission helper ------------------------------------------------------
// Chooses VEX (3-operand, AVX) or legacy SSE encodings. Destinations may alias
// sources: the helpers handle aliasing for the legacy forms.
class V {
public:
    explicit V(EmitContext& c);

    EmitContext& ctx;
    x64::Assembler& a;
    const bool avx;
    const bool avx2;

    Xmm temp() { return ctx.temp_vec(); }
    Mem k(Const128 c) { return constant(ctx, c); }

    // d = x <op> y
    void op(SseOp o, Xmm d, Xmm x, Xmm y, bool commutative = false);
    void op(SseOp o, Xmm d, Xmm x, const Mem& y);
    void op(SseOp o, Xmm d, Xmm x, const Src& y, bool commutative = false) {
        if (y.is_mem) op(o, d, x, y.mem);
        else op(o, d, x, y.reg, commutative);
    }

    void op(SseOp o, Xmm d, Xmm x, Const128 y) { op(o, d, x, k(y)); }
    void op_imm(SseOp o, Xmm d, Xmm x, Xmm y, std::uint8_t imm);
    void op_imm(SseOp o, Xmm d, Xmm x, const Mem& y, std::uint8_t imm);
    // d = <op> x (single source: pabs*, pmovsx*, phminposuw, ...)
    void un(SseOp o, Xmm d, Xmm x);
    void un(SseOp o, Xmm d, const Mem& x);
    void un(SseOp o, Xmm d, const Src& x) {
        if (x.is_mem) un(o, d, x.mem);
        else un(o, d, x.reg);
    }
    void un_imm(SseOp o, Xmm d, Xmm x, std::uint8_t imm);
    // VEX-only single-source/two-source forms (caller checks avx/avx2).
    void vex2(VexOp o, Xmm d, Xmm x) { a.vex_rr(o, false, x64::id(d), 0, x64::id(x)); }
    void vex2(VexOp o, Xmm d, const Mem& m) { a.vex_rm(o, false, x64::id(d), 0, m); }
    void vex3(VexOp o, Xmm d, Xmm x, Xmm y) { a.vex_rr(o, false, x64::id(d), x64::id(x), x64::id(y)); }
    void vex3_imm(VexOp o, Xmm d, Xmm x, Xmm y, std::uint8_t imm) {
        a.vex_rr(o, false, x64::id(d), x64::id(x), x64::id(y));
        a.db(imm);
    }

    // Immediate shifts. Counts >= the lane width give zero (sign fill for psra).
    void psllw(Xmm d, Xmm x, unsigned n) { shift_imm(0x71, 6, d, x, n); }
    void psrlw(Xmm d, Xmm x, unsigned n) { shift_imm(0x71, 2, d, x, n); }
    void psraw(Xmm d, Xmm x, unsigned n) { shift_imm(0x71, 4, d, x, n); }
    void pslld(Xmm d, Xmm x, unsigned n) { shift_imm(0x72, 6, d, x, n); }
    void psrld(Xmm d, Xmm x, unsigned n) { shift_imm(0x72, 2, d, x, n); }
    void psrad(Xmm d, Xmm x, unsigned n) { shift_imm(0x72, 4, d, x, n); }
    void psllq(Xmm d, Xmm x, unsigned n) { shift_imm(0x73, 6, d, x, n); }
    void psrlq(Xmm d, Xmm x, unsigned n) { shift_imm(0x73, 2, d, x, n); }
    void pslldq(Xmm d, Xmm x, unsigned n) { shift_imm(0x73, 7, d, x, n); }
    void psrldq(Xmm d, Xmm x, unsigned n) { shift_imm(0x73, 3, d, x, n); }
    void shift_imm(std::uint8_t group, unsigned ext, Xmm d, Xmm x, unsigned n);

    void mov(Xmm d, Xmm x);
    void zero(Xmm d);
    void ones(Xmm d);
    void movq(Xmm d, Xmm x);                 // d = {x.lo, 0}
    void load_const(Xmm d, Const128 c);      // d = c (pxor/pcmpeqd for 0/~0)
    void pand(Xmm d, Xmm x, Xmm y) { op(op::pand, d, x, y, true); }
    void por(Xmm d, Xmm x, Xmm y) { op(op::por, d, x, y, true); }
    void pxor(Xmm d, Xmm x, Xmm y) { op(op::pxor, d, x, y, true); }
    void pandn(Xmm d, Xmm x, Xmm y) { op(op::pandn, d, x, y); }      // ~x & y
    void pshufb(Xmm d, Xmm table, Xmm index) { op(op::pshufb, d, table, index); }
    void pshufb(Xmm d, Xmm table, Const128 index) { op(op::pshufb, d, table, k(index)); }
    void pshufd(Xmm d, Xmm x, std::uint8_t imm) { un_imm(op::pshufd, d, x, imm); }
    void not_(Xmm d, Xmm x) { op(op::pxor, d, x, k(splat8(0xff))); }
    // d = mask ? y : x, per byte using bit 7 of each mask byte (AVX) or the
    // whole mask (baseline: masks must be all-ones/all-zeros per element).
    void blendv(Xmm d, Xmm x, Xmm y, Xmm mask);
    // d = mask.sign ? y : x per 32-bit (ps) or 64-bit (pd) lane; the
    // baseline path requires full-lane masks.
    void blendv_lane(unsigned size, Xmm d, Xmm x, Xmm y, Xmm mask);
    // Upper 64 bits cleared when !q.
    void finish(Xmm d, bool q) { if (!q) movq(d, d); }

    // ---- per-element-size operations (size 0..3 = B/H/S/D) ----
    void shl(unsigned size, Xmm d, Xmm x, unsigned n);   // n >= esize -> 0
    void srl(unsigned size, Xmm d, Xmm x, unsigned n);   // n >= esize -> 0
    void sra(unsigned size, Xmm d, Xmm x, unsigned n);   // n >= esize -> sign fill
    void sign_mask(unsigned size, Xmm d, Xmm x);         // d = x < 0 ? ~0 : 0
    // Unsigned/signed comparisons producing full masks.
    void cmp_gt_s(unsigned size, Xmm d, Xmm x, Xmm y);   // x > y signed
    void cmp_gt_u(unsigned size, Xmm d, Xmm x, Xmm y);   // x > y unsigned
    void cmp_ge_u(unsigned size, Xmm d, Xmm x, Xmm y);   // x >= y unsigned
    void cmp_ge_s(unsigned size, Xmm d, Xmm x, Xmm y);   // x >= y signed
    void max_s(unsigned size, Xmm d, Xmm x, Xmm y);
    void min_s(unsigned size, Xmm d, Xmm x, Xmm y);
    void max_u(unsigned size, Xmm d, Xmm x, Xmm y);
    void min_u(unsigned size, Xmm d, Xmm x, Xmm y);
    // Keeps only element 0 of `size` (scalar results), zeroing the rest.
    void keep_element0(unsigned size, Xmm d, Xmm x);

    // Helper-level scratch: valid only inside one helper call. Instruction
    // emitters use temp() for their own values.
    Xmm scratch();

private:
    Xmm alias_tmp();
    Xmm scratch_{};
    Xmm alias_{};
    bool have_scratch_{false};
    bool have_alias_{false};
};

constexpr VexOp vex_of(SseOp o) {
    const std::uint8_t pp = o.prefix == 0x66 ? 1 : o.prefix == 0xf3 ? 2 : o.prefix == 0xf2 ? 3 : 0;
    return VexOp{pp, o.map, o.opcode, false};
}

// ---- decode helpers ----------------------------------------------------------------
constexpr unsigned bits(std::uint32_t w, unsigned hi, unsigned lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}
constexpr bool bit(std::uint32_t w, unsigned n) { return ((w >> n) & 1u) != 0; }

// Per-group emitters (return instructions consumed, 0 = not handled). Each
// validates the encoding before emitting anything.
unsigned emit_three_same(EmitContext& ctx, std::uint32_t w);
unsigned emit_three_different(EmitContext& ctx, std::uint32_t w);
unsigned emit_two_reg_misc(EmitContext& ctx, std::uint32_t w);
unsigned emit_across_lanes(EmitContext& ctx, std::uint32_t w);
unsigned emit_indexed(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_three_same(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_three_different(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_two_reg_misc(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_pairwise(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_indexed(EmitContext& ctx, std::uint32_t w);
unsigned emit_dot_product(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_three_same_extra(EmitContext& ctx, std::uint32_t w);
unsigned emit_shift_imm(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_shift_imm(EmitContext& ctx, std::uint32_t w);
unsigned emit_copy(EmitContext& ctx, std::uint32_t w);
unsigned emit_scalar_copy(EmitContext& ctx, std::uint32_t w);
unsigned emit_permute(EmitContext& ctx, std::uint32_t w);
unsigned emit_extract(EmitContext& ctx, std::uint32_t w);
unsigned emit_table(EmitContext& ctx, std::uint32_t w);
unsigned emit_modified_immediate(EmitContext& ctx, std::uint32_t w);
unsigned emit_mask_reduce(EmitContext& ctx, std::span<const std::uint32_t> words);
// SMULL|SMLAL + SMLAL by element (16-bit) into one accumulator: 2, else 0.
unsigned emit_mull_pair(EmitContext& ctx, std::span<const std::uint32_t> words);

// ---- shared building blocks (emit_neon_arith.cpp) ----
enum class Narrow { truncate, sat_signed, sat_unsigned, sat_signed_to_unsigned };
// Narrows 2x-wide elements of x to elements of `size` in the low 64 bits of
// d (upper 64 bits zero).
void narrow(V& v, unsigned size, Narrow kind, Xmm d, Xmm x);
// Saturating add/sub of any element size (u = unsigned).
void sat_add_sub(V& v, unsigned size, bool u, bool sub, Xmm d, Xmm n, Xmm m);
// Broadcasts element `index` (of `size`) of x to every lane of d.
void dup_element(V& v, unsigned size, Xmm d, Xmm x, unsigned index);
// Same for a guest source: an in-memory one is broadcast straight from its
// slot (vbroadcastss / vmovddup / vpbroadcastb/w). Pass x through
// dup_source() before write_vec(): it loads x if the host cannot do that.
void dup_element(V& v, unsigned size, Xmm d, const Src& x, unsigned index);
Src dup_source(V& v, unsigned size, const Src& x);
// Sign/zero extends the low (or high) half of x to 2x-wide elements.
void widen(V& v, unsigned size, bool is_signed, bool high, Xmm d, Xmm x);

// Register-controlled shifts (SSHL/USHL/SRSHL/URSHL/SQSHL/UQSHL/SQRSHL/UQRSHL),
// vector (lanes = 16/8 bytes worth) or scalar (one element).
struct RegShift {
    bool is_unsigned;
    bool rounding;
    bool saturating;
};
void emit_register_shift(EmitContext& ctx, unsigned size, unsigned lanes, RegShift kind,
                         unsigned rd, unsigned rn, unsigned rm, bool scalar);

} // namespace transmute::fx::neon
