#pragma once

// Internal helpers shared by the fp emitters (owner: fp).

#include "fx/common/x64_assembler.hpp"
#include "fx/translator/emit_context.hpp"
#include "fx/translator/emit_fp.hpp"
#include "transmute/fx/context.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace transmute::fx::fpx {

using x64::Cond;
using x64::Gp;
using x64::Label;
using x64::Mem;
using x64::SseOp;
using x64::VexOp;
using x64::Xmm;

// ---- code-generation knobs ------------------------------------------------------
// TRANSMUTE_FX_FP=<knobs> (read once; folded into the family name and so into
// translator_family_fingerprint()) restore earlier lowerings:
//   noslot  - by-element operands in clean cached registers are not taken from
//             their context slot (EmitContext::vec_slot)
//   oldimm  - FMOV (scalar / 64-bit vector) immediates through a GPR instead
//             of one pooled-constant load
//   oldmask - FNMUL/FABD/FACGE/FACGT build their sign/magnitude masks in a
//             register instead of using pooled memory operands
//   noconstminmax - FMAX/FMIN/FMAXNM/FMINNM against a known finite normal
//             constant (EmitContext::vec_fp_const) use the general lowering
//             instead of one x64 min/max
//   oldminmax - FMAX/FMIN/FMAXNM/FMINNM NaN results as host policy (OR/AND of
//             both operand orders) instead of bit-exact FPProcessNaNs lanes
//   oldcvt  - UCVTF (vector, 32-bit lanes) with two converts and two adds
//             instead of converts + one FMA; SCVTF/UCVTF S from a SIMD
//             register through a GPR instead of in the vector domain
//   old     - all of the above
enum FpKnob : std::uint32_t {
    fp_no_slot = 1u,
    fp_old_imm = 2u,
    fp_old_mask = 4u,
    fp_old_cvt = 8u,
    fp_no_const_minmax = 16u,
    fp_old_minmax = 32u,
};
std::uint32_t fp_knob_bits();
inline bool fp_on(std::uint32_t knob) { return (fp_knob_bits() & knob) == 0; }

// ---- opcodes (legacy SSE form; VEX forms derive from them) -------------------
namespace op {
// map 1 (0F)
inline constexpr SseOp movups{0, 1, 0x10}, movss{0xf3, 1, 0x10}, movsd{0xf2, 1, 0x10};
inline constexpr SseOp movhlps{0, 1, 0x12}, movsldup{0xf3, 1, 0x12}, movddup{0xf2, 1, 0x12};
inline constexpr SseOp unpcklps{0, 1, 0x14}, unpcklpd{0x66, 1, 0x14};
inline constexpr SseOp unpckhps{0, 1, 0x15}, unpckhpd{0x66, 1, 0x15};
inline constexpr SseOp movlhps{0, 1, 0x16}, movshdup{0xf3, 1, 0x16};
inline constexpr SseOp movaps{0, 1, 0x28};
inline constexpr SseOp ucomiss{0, 1, 0x2e}, ucomisd{0x66, 1, 0x2e}, comiss{0, 1, 0x2f}, comisd{0x66, 1, 0x2f};
inline constexpr SseOp sqrtps{0, 1, 0x51}, sqrtpd{0x66, 1, 0x51}, sqrtss{0xf3, 1, 0x51}, sqrtsd{0xf2, 1, 0x51};
inline constexpr SseOp andps{0, 1, 0x54}, andpd{0x66, 1, 0x54}, andnps{0, 1, 0x55}, andnpd{0x66, 1, 0x55};
inline constexpr SseOp orps{0, 1, 0x56}, orpd{0x66, 1, 0x56}, xorps{0, 1, 0x57}, xorpd{0x66, 1, 0x57};
inline constexpr SseOp addps{0, 1, 0x58}, addpd{0x66, 1, 0x58}, addss{0xf3, 1, 0x58}, addsd{0xf2, 1, 0x58};
inline constexpr SseOp mulps{0, 1, 0x59}, mulpd{0x66, 1, 0x59}, mulss{0xf3, 1, 0x59}, mulsd{0xf2, 1, 0x59};
inline constexpr SseOp cvtps2pd{0, 1, 0x5a}, cvtpd2ps{0x66, 1, 0x5a}, cvtss2sd{0xf3, 1, 0x5a}, cvtsd2ss{0xf2, 1, 0x5a};
inline constexpr SseOp cvtdq2ps{0, 1, 0x5b}, cvtps2dq{0x66, 1, 0x5b}, cvttps2dq{0xf3, 1, 0x5b};
inline constexpr SseOp subps{0, 1, 0x5c}, subpd{0x66, 1, 0x5c}, subss{0xf3, 1, 0x5c}, subsd{0xf2, 1, 0x5c};
inline constexpr SseOp minps{0, 1, 0x5d}, minpd{0x66, 1, 0x5d}, minss{0xf3, 1, 0x5d}, minsd{0xf2, 1, 0x5d};
inline constexpr SseOp divps{0, 1, 0x5e}, divpd{0x66, 1, 0x5e}, divss{0xf3, 1, 0x5e}, divsd{0xf2, 1, 0x5e};
inline constexpr SseOp maxps{0, 1, 0x5f}, maxpd{0x66, 1, 0x5f}, maxss{0xf3, 1, 0x5f}, maxsd{0xf2, 1, 0x5f};
inline constexpr SseOp punpckldq{0x66, 1, 0x62}, packssdw{0x66, 1, 0x6b};
inline constexpr SseOp pcmpgtd{0x66, 1, 0x66};
inline constexpr SseOp punpcklqdq{0x66, 1, 0x6c}, punpckhqdq{0x66, 1, 0x6d};
inline constexpr SseOp pshufd{0x66, 1, 0x70};
inline constexpr SseOp pcmpeqd{0x66, 1, 0x76};
inline constexpr SseOp haddpd{0x66, 1, 0x7c}, haddps{0xf2, 1, 0x7c};
inline constexpr SseOp movq_xx{0xf3, 1, 0x7e};   // movq xmm, xmm (zero upper)
inline constexpr SseOp cmpps{0, 1, 0xc2}, cmppd{0x66, 1, 0xc2}, cmpss{0xf3, 1, 0xc2}, cmpsd{0xf2, 1, 0xc2};
inline constexpr SseOp pextrw{0x66, 1, 0xc5};
inline constexpr SseOp shufps{0, 1, 0xc6}, shufpd{0x66, 1, 0xc6};
inline constexpr SseOp paddq{0x66, 1, 0xd4};
inline constexpr SseOp pand{0x66, 1, 0xdb}, pandn{0x66, 1, 0xdf}, por{0x66, 1, 0xeb}, pxor{0x66, 1, 0xef};
inline constexpr SseOp paddd{0x66, 1, 0xfe}, psubd{0x66, 1, 0xfa}, psubq{0x66, 1, 0xfb};
inline constexpr SseOp cvtdq2pd{0xf3, 1, 0xe6}, cvttpd2dq{0x66, 1, 0xe6};
// map 2 (0F38)
inline constexpr SseOp ptest{0x66, 2, 0x17};
inline constexpr SseOp pmovsxwd{0x66, 2, 0x23}, pmovzxwd{0x66, 2, 0x33};
inline constexpr SseOp pcmpeqq{0x66, 2, 0x29}, packusdw{0x66, 2, 0x2b}, pcmpgtq{0x66, 2, 0x37};
inline constexpr SseOp pminsd{0x66, 2, 0x39}, pminud{0x66, 2, 0x3b}, pmaxsd{0x66, 2, 0x3d}, pmaxud{0x66, 2, 0x3f};
inline constexpr SseOp pmulld{0x66, 2, 0x40};
// map 3 (0F3A)
inline constexpr SseOp roundps{0x66, 3, 0x08}, roundpd{0x66, 3, 0x09}, roundss{0x66, 3, 0x0a}, roundsd{0x66, 3, 0x0b};
inline constexpr SseOp blendps{0x66, 3, 0x0c}, blendpd{0x66, 3, 0x0d}, pblendw{0x66, 3, 0x0e};
inline constexpr SseOp insertps{0x66, 3, 0x21};
} // namespace op

// VEX-only opcodes.
namespace vop {
inline constexpr VexOp vcvtph2ps{1, 2, 0x13, false};
inline constexpr VexOp vcvtps2ph{1, 3, 0x1d, false};   // ModRM.reg = source, ModRM.rm = destination
inline constexpr VexOp vpermilps{1, 3, 0x04, false}, vpermilpd{1, 3, 0x05, false};
inline constexpr VexOp vblendvps{1, 3, 0x4a, false}, vblendvpd{1, 3, 0x4b, false};
inline constexpr VexOp vpbroadcastd{1, 2, 0x58, false}, vpbroadcastq{1, 2, 0x59, false};
inline constexpr VexOp vbroadcastss{1, 2, 0x18, false};   // AVX: m32 source; AVX2: xmm source
} // namespace vop

// cmpps predicates (0..7 legacy; 13/14 VEX only).
enum Pred : std::uint8_t {
    pred_eq = 0, pred_lt = 1, pred_le = 2, pred_unord = 3, pred_neq = 4, pred_nlt = 5, pred_nle = 6,
    pred_ord = 7, pred_ge_os = 13, pred_gt_os = 14,
};

// FMA3 forms (VEX.66.0F38, W1 for pd/sd).
enum class FmaKind : std::uint8_t { fmadd, fmsub, fnmadd, fnmsub };   // a*b+c, a*b-c, -(a*b)+c, -(a*b)-c
enum class FmaForm : std::uint8_t { f132 = 0x98, f213 = 0xa8, f231 = 0xb8 };

constexpr std::uint8_t vex_pp(std::uint8_t prefix) {
    return prefix == 0x66 ? 1 : prefix == 0xf3 ? 2 : prefix == 0xf2 ? 3 : 0;
}
constexpr VexOp vex_of(SseOp o, bool w = false) { return VexOp{vex_pp(o.prefix), o.map, o.opcode, w}; }

// ---- per-shape opcode selection --------------------------------------------------
struct FOps { SseOp ss, sd, ps, pd; };
namespace fops {
inline constexpr FOps add{op::addss, op::addsd, op::addps, op::addpd};
inline constexpr FOps sub{op::subss, op::subsd, op::subps, op::subpd};
inline constexpr FOps mul{op::mulss, op::mulsd, op::mulps, op::mulpd};
inline constexpr FOps div{op::divss, op::divsd, op::divps, op::divpd};
inline constexpr FOps max{op::maxss, op::maxsd, op::maxps, op::maxpd};
inline constexpr FOps min{op::minss, op::minsd, op::minps, op::minpd};
inline constexpr FOps sqrt{op::sqrtss, op::sqrtsd, op::sqrtps, op::sqrtpd};
inline constexpr FOps cmp{op::cmpss, op::cmpsd, op::cmpps, op::cmppd};
inline constexpr FOps round{op::roundss, op::roundsd, op::roundps, op::roundpd};
inline constexpr FOps and_{op::andps, op::andpd, op::andps, op::andpd};
inline constexpr FOps andn{op::andnps, op::andnpd, op::andnps, op::andnpd};
inline constexpr FOps or_{op::orps, op::orpd, op::orps, op::orpd};
inline constexpr FOps xor_{op::xorps, op::xorpd, op::xorps, op::xorpd};
} // namespace fops

// Element shape of an operation: element size in bits (16/32/64), scalar
// (only lane 0 is computed with ss/sd forms) or packed, and the number of
// valid result bits (16/32/64 for scalars, 64 or 128 for vectors).
struct Shape {
    unsigned es = 32;
    bool scalar = false;
    bool q = true;
    [[nodiscard]] bool dbl() const { return es == 64; }
    [[nodiscard]] unsigned width() const { return scalar ? es : (q ? 128u : 64u); }
    [[nodiscard]] SseOp pick(const FOps& f) const {
        if (scalar) return es == 64 ? f.sd : f.ss;
        return es == 64 ? f.pd : f.ps;
    }
    // Shape used to compute an H shape in single precision.
    [[nodiscard]] Shape as_single() const { Shape s = *this; s.es = 32; return s; }
};

// A read-only source operand: a host register, or a memory operand (a guest
// register's GuestContext slot, possibly offset to one element, or a
// constant). Memory operands are only produced where the consumer accepts
// them (VEX second sources; 16-byte aligned for legacy SSE).
struct Opnd {
    Xmm reg{};
    Mem mem{};
    bool is_mem = false;
    bool zero_upper = false;   // register with zeros above the scalar (element temps)
    static Opnd of(Xmm r) { Opnd o; o.reg = r; return o; }
    static Opnd at(const Mem& m) { Opnd o; o.mem = m; o.is_mem = true; return o; }
};

// ---- per-instruction emission state ---------------------------------------------
class FpEmit {
public:
    explicit FpEmit(EmitContext& context);

    EmitContext& ctx;
    x64::Assembler& as;
    FpFeatures feat;
    [[nodiscard]] bool avx() const { return feat.avx; }

    // Operands (memoized per instruction; results are read-only).
    Xmm vec(unsigned index);
    // Guest source as a register (if it already is one) or, with AVX, its
    // GuestContext slot (EmitContext::vsrc). Register results are memoized.
    Opnd src(unsigned index);
    // True if src(index) would load the register (it is not in a register).
    [[nodiscard]] bool in_memory(unsigned index);
    // Bits [127:width] of guest register `index` are known zero at the start
    // of this instruction (EmitContext::vec_zero_above); width 128 is trivial.
    [[nodiscard]] bool zero_above(unsigned index, unsigned width) const;
    // Same for a host register obtained from vec()/src() in this instruction
    // (false for temps).
    [[nodiscard]] bool reg_zero_above(Xmm x, unsigned width) const;
    // Destination written by this instruction (dest() was called).
    [[nodiscard]] bool wrote_dest() const { return have_dest_; }
    [[nodiscard]] unsigned dest_guest() const { return dest_guest_; }
    Gp gpr(unsigned index, bool sp = false);
    Xmm tmp();
    // One instruction may take `n` vector temps instead of kMaxVecTemps (the
    // NaN-exact pairwise / across-lanes min/max: rare, a vector retry at worst).
    void allow_vec_temps(unsigned n) { vec_budget_ = n; }
    Gp gtmp();
    Xmm dest(unsigned index);          // write_vec (memoized)
    // Scratch GPR reserved for materializing constants (allocated on first
    // use; emitters with internal labels call it before the first label).
    Gp const_gpr();
    Gp gdest(unsigned index, bool sp = false);

    // ---- generic instruction forms (AVX when available, else SSE) ----------
    void mov(Xmm d, Xmm s);                                   // movaps
    // d = a OP b. SSE: d == a, or d != b (a is copied into d first).
    void op(SseOp o, Xmm d, Xmm a, Xmm b);
    void op_i(SseOp o, Xmm d, Xmm a, Xmm b, std::uint8_t imm);
    // Same with a register-or-memory second source (SSE: memory operands must
    // be 16-byte aligned unless the op reads at most the scalar).
    void op(SseOp o, Xmm d, Xmm a, const Opnd& b);
    void op_i(SseOp o, Xmm d, Xmm a, const Opnd& b, std::uint8_t imm);
    // d = OP(s) / OP(s, imm) with a register-or-memory source.
    void un(SseOp o, Xmm d, const Opnd& s);
    void un_i(SseOp o, Xmm d, const Opnd& s, std::uint8_t imm);
    // d = OP(s) for instructions whose SSE form does not read d (packed
    // conversions, sqrtps, pshufd, movshdup, movddup, roundps, ...).
    void un(SseOp o, Xmm d, Xmm s);
    void un_i(SseOp o, Xmm d, Xmm s, std::uint8_t imm);
    // Scalar forms that merge d's upper lanes in SSE (sqrtss, cvtss2sd,
    // roundss): d[0] = OP(s[0]); d's upper lanes are unspecified.
    void sc(SseOp o, Xmm d, Xmm s);
    void sc_i(SseOp o, Xmm d, Xmm s, std::uint8_t imm);
    // AVX only: d[0] = OP(s[0]); d's upper lanes come from `upper`.
    void sc_u(SseOp o, Xmm d, Xmm upper, const Opnd& s);
    void sc_u_i(SseOp o, Xmm d, Xmm upper, const Opnd& s, std::uint8_t imm);
    // roundss/sd (scalar shapes) or roundps/pd with immediate.
    void round(Shape s, Xmm d, Xmm x, std::uint8_t imm) {
        if (s.scalar) sc_i(s.pick(fops::round), d, x, imm);
        else un_i(s.pick(fops::round), d, x, imm);
    }
    // Shift by immediate: opcode 0x71/0x72/0x73 with /ext.
    void shift(std::uint8_t opcode, unsigned ext, Xmm d, Xmm s, std::uint8_t imm);
    void psrld(Xmm d, Xmm s, std::uint8_t n) { shift(0x72, 2, d, s, n); }
    void psrad(Xmm d, Xmm s, std::uint8_t n) { shift(0x72, 4, d, s, n); }
    void pslld(Xmm d, Xmm s, std::uint8_t n) { shift(0x72, 6, d, s, n); }
    void psrlq(Xmm d, Xmm s, std::uint8_t n) { shift(0x73, 2, d, s, n); }
    void psllq(Xmm d, Xmm s, std::uint8_t n) { shift(0x73, 6, d, s, n); }
    void psrldq(Xmm d, Xmm s, std::uint8_t n) { shift(0x73, 3, d, s, n); }
    void pslldq(Xmm d, Xmm s, std::uint8_t n) { shift(0x73, 7, d, s, n); }
    void zero(Xmm d);                                          // xorps d, d
    void ones(Xmm d);                                          // pcmpeqd d, d
    // d = mask ? b : a, per lane (mask lanes all-ones or zero; AVX uses the
    // sign bits). `scratch` (distinct from all others) is used by the SSE
    // form only. d may alias a, b or mask.
    void blend(bool dbl, Xmm d, Xmm a, Xmm b, Xmm mask, Xmm scratch);
    // FMA3 (requires feat.fma).
    void fma(FmaKind kind, FmaForm form, bool dbl, bool scalar, Xmm d, Xmm b, Xmm c);
    void fma(FmaKind kind, FmaForm form, bool dbl, bool scalar, Xmm d, Xmm b, const Opnd& c);
    // xmm <-> gpr
    void movd(Xmm d, Gp s);
    void movq(Xmm d, Gp s);
    void movd(Gp d, Xmm s);
    void movq(Gp d, Xmm s);
    void movq(Xmm d, Xmm s);                                   // zero upper 64
    // Copy lane 0 (32 bits) of s into d and zero d's other lanes.
    void ins0(Xmm d, Xmm s);
    void cvtsi2s(bool dbl, Xmm d, Gp s, unsigned bits);        // d[0] = (float)s (upper from d)
    void ucomi(bool dbl, Xmm a, Xmm b, bool signal);           // ucomis/comis
    void ucomi(bool dbl, Xmm a, const Opnd& b, bool signal);
    void cvtph2ps(Xmm d, Xmm s);
    void cvtph2ps(Xmm d, const Opnd& s);                       // memory: 4 halves (m64)
    void cvtps2ph(Xmm d, Xmm s);                               // MXCSR rounding

    // ---- constants --------------------------------------------------------------
    // 16-byte constants live in the neon family's rip-relative pool (one per
    // region when the core brackets the region, else after the instruction
    // behind a jmp; 16-byte aligned).
    Mem kconst(std::uint64_t lo, std::uint64_t hi);
    // Every 16/32/64-bit lane = value / lane 0 = value and other bits zero.
    Mem ksplat(unsigned es, std::uint64_t value);
    Mem kscalar(unsigned es, std::uint64_t value);
    // Fills all lanes (32/64-bit elements) with `value`.
    void splat(unsigned es, Xmm d, std::uint64_t value);
    // Lane 0 = value, other bits zero.
    void scalar_const(unsigned es, Xmm d, std::uint64_t value);
    // Sign-bit mask / magnitude mask for the element size.
    void sign_mask(unsigned es, Xmm d) { ones(d); shift(es == 64 ? 0x73 : 0x72, 6, d, d, static_cast<std::uint8_t>(es - 1)); }
    void abs_mask(unsigned es, Xmm d) { ones(d); shift(es == 64 ? 0x73 : 0x72, 2, d, d, 1); }
    static std::uint64_t sign_bits(unsigned es) { return 1ULL << (es - 1); }
    static std::uint64_t abs_bits(unsigned es) { return es == 64 ? ~0ULL >> 1 : (1ULL << (es - 1)) - 1; }

    // ---- results ------------------------------------------------------------------
    // Writes the low `width` bits of t to Vd and zeroes the rest.
    void commit(unsigned rd, Xmm t, unsigned width);
    // Zeroes bits >= width of d in place.
    void zero_upper(Xmm d, unsigned width);

private:
    struct VecSlot { unsigned guest; Xmm host; bool mem; Mem slot; };
    struct GprSlot { unsigned guest; bool sp; Gp host; };
    VecSlot vecs_[8]{};
    unsigned nvecs_ = 0;
    GprSlot gprs_[8]{};
    unsigned ngprs_ = 0;
    bool have_dest_ = false;
    unsigned dest_guest_ = 0;
    Xmm dest_host_{};
    unsigned temps_ = 0;
    unsigned vec_budget_ = 0;   // 0: kMaxVecTemps
    unsigned gtemps_ = 0;
    bool have_const_gpr_ = false;
    Gp const_gpr_{};
    std::uint32_t zero32_ = 0;   // EmitContext::vec_zero_above at instruction start
    std::uint32_t zero64_ = 0;
};

// Budget the core guarantees per instruction beyond operands.
inline constexpr unsigned kMaxVecTemps = 4;
inline constexpr unsigned kMaxGprTemps = 5;

// ---- decoded instructions ----------------------------------------------------------
enum class K : std::uint8_t {
    none,
    // two-source arithmetic (scalar FP, SIMD scalar, vector)
    fadd, fsub, fmul, fdiv, fnmul, fabd, fmulx, fmax, fmin, fmaxnm, fminnm, frecps, frsqrts,
    fcmeq, fcmge, fcmgt, facge, facgt,
    fcmeq0, fcmge0, fcmgt0, fcmle0, fcmlt0,
    // pairwise (vector: rn,rm; scalar: rn) and reductions
    faddp, fmaxp, fminp, fmaxnmp, fminnmp,
    fmaxv, fminv, fmaxnmv, fminnmv,
    // accumulate
    fmla, fmls, fmadd, fmsub, fnmadd, fnmsub,
    // one source
    fmov, fabs, fneg, fsqrt, frint, frecpe, frsqrte, frecpx,
    frint_n,              // FRINT32Z/X, FRINT64Z/X (FEAT_FRINTTS): rnd z or x, ibits 32/64
    fmlal, fmlsl,         // FEAT_FHM widening half -> single fused multiply-add (upper = "2")
    fcvt,                 // scalar precision conversion es -> dst_es
    fcvtl, fcvtn, fcvtxn,  // vector widen/narrow (upper = "2" variant)
    cvt_f2i,              // FCVT[NPMZA][SU] (rounding, unsigned, fbits; to_gpr or SIMD)
    cvt_i2f,              // SCVTF/UCVTF (from_gpr or SIMD)
    fjcvtzs,
    // compares and selects
    fcmp, fccmp, fcsel,
    // moves
    fmov_imm,             // scalar FMOV #imm (imm = bit pattern)
    fmov_vimm,            // vector FMOV #imm (imm = element bit pattern)
    fmov_to_gpr, fmov_from_gpr,
    fmov_to_gpr_hi, fmov_from_gpr_hi,
    // system registers
    mrs_fpcr, msr_fpcr, mrs_fpsr, msr_fpsr,
};

enum class Rnd : std::uint8_t { n, p, m, z, a, x, i };   // nearest-even, +inf, -inf, zero, away, exact(current), current

struct Inst {
    K k = K::none;
    std::uint8_t rd = 0, rn = 0, rm = 0, ra = 0;
    Shape sh;                  // element shape (source elements for conversions)
    std::int8_t index = -1;    // by-element lane, -1 = none
    std::uint8_t cond = 0;     // FCSEL/FCCMP
    std::uint8_t nzcv = 0;     // FCCMP immediate
    Rnd rnd = Rnd::z;
    bool is_unsigned = false;
    std::uint8_t fbits = 0;    // fixed-point fraction bits
    std::uint8_t ibits = 0;    // GPR width (32/64) for scalar int conversions
    std::uint8_t dst_es = 0;   // FCVT destination element size
    bool gpr = false;          // conversion to/from a general register
    bool upper = false;        // FCVTL2/FCVTN2/FCVTXN2
    bool signal = false;       // FCMPE/FCCMPE
    bool zero = false;         // FCMP #0.0
    std::uint64_t imm = 0;
};

bool decode(std::uint32_t word, Inst& inst);
void describe(const Inst& inst, InstructionUse& use);
// Low bits of Vd the instruction defines (A64 zeroes the rest; 128 = all).
unsigned result_width(const Inst& inst);

// Emitters. Return false to decline (e.g. a required host feature is
// missing); nothing may have been emitted in that case.
bool emit_arith(FpEmit& e, const Inst& in);       // arithmetic, min/max, compares, pairwise, reductions
bool emit_fused(FpEmit& e, const Inst& in);       // fmla/fmls/fmadd/...
bool emit_unary(FpEmit& e, const Inst& in);       // fmov/fabs/fneg/fsqrt/frint
bool emit_select(FpEmit& e, const Inst& in);      // fcmp/fccmp/fcsel
bool emit_move(FpEmit& e, const Inst& in);        // fmov forms, immediates
bool emit_convert(FpEmit& e, const Inst& in);     // cvt_f2i/cvt_i2f/fcvt/fcvtl/fcvtn/fcvtxn/fjcvtzs
bool emit_estimate(FpEmit& e, const Inst& in);    // frecpe/frsqrte/frecpx/frecps/frsqrts
bool emit_sysreg(FpEmit& e, const Inst& in);      // MRS/MSR FPCR/FPSR
bool emit_frint_n(FpEmit& e, const Inst& in);     // FRINT32*/FRINT64*
bool emit_fhm(FpEmit& e, const Inst& in);         // FMLAL/FMLSL(2)

// Shared building blocks.
// out = exact A64 FMAX/FMIN/FMAXNM/FMINNM (kind) of (a, b) for shape s,
// including FZ flushing. u and v are distinct scratch registers (v may equal
// u for FMIN). out may alias a or b if minmax_may_alias() (AVX, not
// FMINNM); it never aliases u or v.
void emit_minmax(FpEmit& e, K kind, Shape s, Xmm out, Xmm a, Xmm b, Xmm u, Xmm v);
bool minmax_may_alias(const FpEmit& e, K kind);
// Same, but a and b are temps the sequence may clobber: needs only out and u.
void emit_minmax_scratch(FpEmit& e, K kind, Shape s, Xmm out, Xmm a, Xmm b, Xmm u);
// Allocates the scratch registers emit_minmax needs.
struct MinMaxTemps { Xmm u, v; };
MinMaxTemps minmax_temps(FpEmit& e, K kind);
// Lane `index` of Vm broadcast to all lanes (vector shapes) or moved to lane
// 0 (scalar shapes). May return the guest register itself.
Xmm element(FpEmit& e, Shape s, unsigned rm, int index);
// The same as an operand (S/D shapes): scalar shapes give a memory operand at
// the element when Vm is in memory; vector shapes broadcast from memory
// (vbroadcastss / movddup) into a temp.
Opnd element_op(FpEmit& e, Shape s, unsigned rm, int index);
// `o` in a register: memory scalars are loaded with movss/movsd (upper lanes
// zero), vectors with movups.
Xmm load_opnd(FpEmit& e, const Opnd& o, Shape s);
// Vd = Vn OP Vm (or Vm[index]) for guest sources (S/D shapes): memory second
// sources, commutative operand swap, and no zeroing of a scalar result when
// its first source has zeros above the scalar. imm >= 0: with immediate.
// zero_safe: 0 OP 0 = +0 exactly (a 64-bit vector result needs no zeroing
// when both sources are zero above bit 64).
void guest_binary(FpEmit& e, const Inst& in, SseOp o, bool commutative, int imm = -1, bool zero_safe = false);
// Vd = a OP b (imm >= 0: with immediate) with A64 zeroing above `width`.
void final_op(FpEmit& e, unsigned rd, unsigned width, SseOp o, Xmm a, Xmm b, bool commutative,
              int imm = -1);
// Round-half-away-from-zero to integral (FRINTA / FCVTA*) using the four
// temps T. x must not be T[0..2]; out must not be T[0], T[1] or T[3] (it may
// be T[2] or any non-temp, including x). `exact` keeps the inexact flag.
void emit_round_away(FpEmit& e, Shape s, Xmm out, Xmm x, bool exact, const Xmm (&T)[4]);
// Round-to-odd narrowing double -> single of lane 0 (packed: lanes 0..1) of
// x into out (low lanes); b and m are scratch. All four must be distinct.
void emit_narrow_round_odd(FpEmit& e, bool packed, Xmm out, Xmm x, Xmm b, Xmm m);
// VFPExpandImm.
std::uint64_t expand_fp_imm(unsigned imm8, unsigned es);

// Displacements of FP context fields.
inline constexpr std::int32_t kFpcrDisp = ctx_disp(offsetof(GuestContext, fpcr));
inline constexpr std::int32_t kFpsrDisp = ctx_disp(offsetof(GuestContext, fpsr));
inline Mem ctx_mem(std::int32_t disp) { return x64::ptr(Gp::r15, disp); }

} // namespace transmute::fx::fpx
