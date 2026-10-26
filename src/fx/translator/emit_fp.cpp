// fp family: public API, MXCSR/FPSR helpers, operand helpers, decoder and
// dispatch (owner: fp).
#include "fx/translator/emit_fp.hpp"

#include "fx/common/cpu_features.hpp"
#include "fx/translator/emit_fp_internal.hpp"
#include "fx/translator/emit_neon_internal.hpp"   // shared rip-relative constant pool

#include <immintrin.h>

#include <cstdlib>
#include <string>

namespace transmute::fx {

// ---- features -------------------------------------------------------------------

namespace {
bool g_override_active = false;
FpFeatures g_override{};
} // namespace

FpFeatures fp_host_features() {
    const auto& c = cpu_features();
    FpFeatures f;
    f.avx = c.avx && c.os_avx;
    f.fma = f.avx && c.fma;
    f.f16c = f.avx && c.f16c;
    f.avx2 = f.avx && c.avx2;
    return f;
}

FpFeatures fp_features() {
    if (g_override_active) return g_override;
    static const FpFeatures host = fp_host_features();
    return host;
}

void fp_set_features_for_testing(const FpFeatures* features) {
    if (features) {
        g_override = *features;
        // Never claim more than the host has.
        const auto host = fp_host_features();
        g_override.avx = g_override.avx && host.avx;
        g_override.fma = g_override.fma && host.fma && g_override.avx;
        g_override.f16c = g_override.f16c && host.f16c && g_override.avx;
        g_override.avx2 = g_override.avx2 && host.avx2 && g_override.avx;
        g_override_active = true;
    } else {
        g_override_active = false;
    }
}

// ---- FPCR / FPSR / MXCSR ------------------------------------------------------------

std::uint32_t fp_mxcsr_for_fpcr(std::uint32_t fpcr) {
    static constexpr std::uint32_t kRc[4] = {0, 2, 1, 3};   // RN, RP->up, RM->down, RZ
    std::uint32_t m = 0x1f80u;
    m |= kRc[(fpcr >> 22) & 3u] << 13;
    if (fpcr & (1u << 24)) m |= 0x8040u;   // FZ -> FTZ | DAZ
    return m;
}

std::uint32_t fp_mxcsr_flags_from_fpsr(std::uint32_t fpsr) {
    return (fpsr & 1u) | ((fpsr & 0x1eu) << 1);
}

std::uint32_t fp_fpsr_flags_from_mxcsr(std::uint32_t mxcsr) {
    return (mxcsr & 1u) | ((mxcsr >> 1) & 0x1eu);
}

std::uint32_t fp_mxcsr_for_state(std::uint32_t fpcr, std::uint32_t fpsr) {
    return fp_mxcsr_for_fpcr(fpcr) | fp_mxcsr_flags_from_fpsr(fpsr);
}

std::uint32_t fp_read_fpsr(std::uint32_t context_fpsr, std::uint32_t mxcsr) {
    return context_fpsr | fp_fpsr_flags_from_mxcsr(mxcsr);
}

void fp_write_fpsr(std::uint32_t value, std::uint32_t mxcsr, std::uint32_t& context_fpsr,
                   std::uint32_t& new_mxcsr) {
    context_fpsr = value;
    new_mxcsr = (mxcsr & ~kMxcsrStatusMask) | fp_mxcsr_flags_from_fpsr(value);
}

std::uint32_t fp_sync_fpsr(GuestContext& ctx) noexcept {
    ctx.fpsr = fp_read_fpsr(ctx.fpsr, _mm_getcsr());
    return ctx.fpsr;
}

void fp_load_mxcsr(const GuestContext& ctx) noexcept {
    _mm_setcsr(fp_mxcsr_for_state(ctx.fpcr, ctx.fpsr));
}

namespace fpx {

// ---- FpEmit ---------------------------------------------------------------------------

FpEmit::FpEmit(EmitContext& context)
    : ctx(context), as(context.as()), feat(fp_features()), zero32_(context.vec_zero_above(32)),
      zero64_(context.vec_zero_above(64)) {}

Xmm FpEmit::vec(unsigned index) {
    if (have_dest_ && dest_guest_ == index) throw std::logic_error("fp: guest vector read after write");
    for (unsigned i = 0; i < nvecs_; ++i)
        if (vecs_[i].guest == index && !vecs_[i].mem) return vecs_[i].host;
    const auto r = ctx.read_vec(index);
    for (unsigned i = 0; i < nvecs_; ++i)
        if (vecs_[i].guest == index) { vecs_[i] = {index, r, false, {}}; return r; }
    if (nvecs_ < 8) vecs_[nvecs_++] = {index, r, false, {}};
    return r;
}

Opnd FpEmit::src(unsigned index) {
    if (have_dest_ && dest_guest_ == index) throw std::logic_error("fp: guest vector read after write");
    for (unsigned i = 0; i < nvecs_; ++i) {
        if (vecs_[i].guest != index) continue;
        return vecs_[i].mem ? Opnd::at(vecs_[i].slot) : Opnd::of(vecs_[i].host);
    }
    const auto s = ctx.vsrc(index);
    if (nvecs_ < 8) vecs_[nvecs_++] = {index, s.reg, s.is_mem, s.mem};
    return s.is_mem ? Opnd::at(s.mem) : Opnd::of(s.reg);
}

bool FpEmit::in_memory(unsigned index) { return src(index).is_mem; }

bool FpEmit::zero_above(unsigned index, unsigned width) const {
    if (width >= 128) return true;
    if (index >= 32) return false;
    if (width == 64) return ((zero64_ >> index) & 1u) != 0;
    if (width == 32) return ((zero32_ >> index) & 1u) != 0;
    return false;
}

bool FpEmit::reg_zero_above(Xmm x, unsigned width) const {
    if (width >= 128) return true;
    for (unsigned i = 0; i < nvecs_; ++i)
        if (!vecs_[i].mem && vecs_[i].host == x) return zero_above(vecs_[i].guest, width);
    return false;
}

Gp FpEmit::gpr(unsigned index, bool sp) {
    for (unsigned i = 0; i < ngprs_; ++i)
        if (gprs_[i].guest == index && gprs_[i].sp == sp) return gprs_[i].host;
    const auto r = ctx.read_gpr(index, sp);
    if (ngprs_ < 8) gprs_[ngprs_++] = {index, sp, r};
    return r;
}

Xmm FpEmit::tmp() {
    if (++temps_ > (vec_budget_ ? vec_budget_ : kMaxVecTemps)) throw std::logic_error("fp: vector temp budget exceeded");
    return ctx.temp_vec();
}

Gp FpEmit::gtmp() {
    if (++gtemps_ > kMaxGprTemps) throw std::logic_error("fp: gpr temp budget exceeded");
    return ctx.temp_gpr();
}

Xmm FpEmit::dest(unsigned index) {
    if (have_dest_) {
        if (dest_guest_ != index) throw std::logic_error("fp: two vector destinations");
        return dest_host_;
    }
    dest_host_ = ctx.write_vec(index);
    dest_guest_ = index;
    have_dest_ = true;
    return dest_host_;
}

Gp FpEmit::gdest(unsigned index, bool sp) { return ctx.write_gpr(index, sp); }

Gp FpEmit::const_gpr() {
    if (!have_const_gpr_) {
        const_gpr_ = gtmp();
        have_const_gpr_ = true;
    }
    return const_gpr_;
}

void FpEmit::mov(Xmm d, Xmm s) {
    if (d == s) return;
    if (avx()) as.vex_rr(vex_of(op::movaps), false, x64::id(d), 0, x64::id(s));
    else as.movaps(d, s);
}

void FpEmit::op(SseOp o, Xmm d, Xmm a, Xmm b) {
    if (avx()) {
        as.vex_rr(vex_of(o), false, x64::id(d), x64::id(a), x64::id(b));
        return;
    }
    if (d != a) {
        if (d == b) throw std::logic_error("fp: SSE op destination aliases second source");
        mov(d, a);
    }
    as.sse_raw(o, false, x64::id(d), x64::id(b));
}

void FpEmit::op_i(SseOp o, Xmm d, Xmm a, Xmm b, std::uint8_t imm) {
    if (avx()) {
        as.vex_rr(vex_of(o), false, x64::id(d), x64::id(a), x64::id(b));
        as.db(imm);
        return;
    }
    if (d != a) {
        if (d == b) throw std::logic_error("fp: SSE op destination aliases second source");
        mov(d, a);
    }
    as.sse_raw(o, false, x64::id(d), x64::id(b));
    as.db(imm);
}

void FpEmit::op(SseOp o, Xmm d, Xmm a, const Opnd& b) {
    if (!b.is_mem) { op(o, d, a, b.reg); return; }
    if (avx()) {
        as.vex_rm(vex_of(o), false, x64::id(d), x64::id(a), b.mem);
        return;
    }
    mov(d, a);
    as.sse_raw(o, false, x64::id(d), b.mem);
}

void FpEmit::op_i(SseOp o, Xmm d, Xmm a, const Opnd& b, std::uint8_t imm) {
    if (!b.is_mem) { op_i(o, d, a, b.reg, imm); return; }
    if (avx()) {
        as.vex_rm(vex_of(o), false, x64::id(d), x64::id(a), b.mem, 1);
    } else {
        mov(d, a);
        as.sse_raw(o, false, x64::id(d), b.mem, 1);
    }
    as.db(imm);
}

void FpEmit::un(SseOp o, Xmm d, const Opnd& s) {
    if (!s.is_mem) { un(o, d, s.reg); return; }
    if (avx()) as.vex_rm(vex_of(o), false, x64::id(d), 0, s.mem);
    else as.sse_raw(o, false, x64::id(d), s.mem);
}

void FpEmit::un_i(SseOp o, Xmm d, const Opnd& s, std::uint8_t imm) {
    if (!s.is_mem) { un_i(o, d, s.reg, imm); return; }
    if (avx()) as.vex_rm(vex_of(o), false, x64::id(d), 0, s.mem, 1);
    else as.sse_raw(o, false, x64::id(d), s.mem, 1);
    as.db(imm);
}

void FpEmit::sc_u(SseOp o, Xmm d, Xmm upper, const Opnd& s) {
    if (!avx()) throw std::logic_error("fp: sc_u needs AVX");
    if (s.is_mem) as.vex_rm(vex_of(o), false, x64::id(d), x64::id(upper), s.mem);
    else as.vex_rr(vex_of(o), false, x64::id(d), x64::id(upper), x64::id(s.reg));
}

void FpEmit::sc_u_i(SseOp o, Xmm d, Xmm upper, const Opnd& s, std::uint8_t imm) {
    if (!avx()) throw std::logic_error("fp: sc_u needs AVX");
    if (s.is_mem) as.vex_rm(vex_of(o), false, x64::id(d), x64::id(upper), s.mem, 1);
    else as.vex_rr(vex_of(o), false, x64::id(d), x64::id(upper), x64::id(s.reg));
    as.db(imm);
}

void FpEmit::un(SseOp o, Xmm d, Xmm s) {
    if (avx()) as.vex_rr(vex_of(o), false, x64::id(d), 0, x64::id(s));
    else as.sse_raw(o, false, x64::id(d), x64::id(s));
}

void FpEmit::un_i(SseOp o, Xmm d, Xmm s, std::uint8_t imm) {
    un(o, d, s);
    as.db(imm);
}

void FpEmit::sc(SseOp o, Xmm d, Xmm s) {
    if (avx()) as.vex_rr(vex_of(o), false, x64::id(d), x64::id(s), x64::id(s));
    else as.sse_raw(o, false, x64::id(d), x64::id(s));
}

void FpEmit::sc_i(SseOp o, Xmm d, Xmm s, std::uint8_t imm) {
    sc(o, d, s);
    as.db(imm);
}

void FpEmit::shift(std::uint8_t opcode, unsigned ext, Xmm d, Xmm s, std::uint8_t imm) {
    if (avx()) {
        as.vex_rr(VexOp{1, 1, opcode, false}, false, ext, x64::id(d), x64::id(s));
        as.db(imm);
        return;
    }
    mov(d, s);
    as.sse_shift_imm(opcode, ext, d, imm);
}

void FpEmit::zero(Xmm d) { op(op::xorps, d, d, d); }
void FpEmit::ones(Xmm d) { op(op::pcmpeqd, d, d, d); }

void FpEmit::blend(bool dbl, Xmm d, Xmm a, Xmm b, Xmm mask, Xmm scratch) {
    if (avx()) {
        as.vex_is4(dbl ? vop::vblendvpd : vop::vblendvps, d, a, b, mask);
        return;
    }
    // d = a ^ ((a ^ b) & mask).
    const auto x = scratch;
    mov(x, b);
    op(op::xorps, x, x, a);
    op(op::andps, x, x, mask);
    mov(d, a);
    op(op::xorps, d, d, x);
}

void FpEmit::fma(FmaKind kind, FmaForm form, bool dbl, bool scalar, Xmm d, Xmm b, Xmm c) {
    if (!feat.fma) throw std::logic_error("fp: FMA used without host support");
    const auto opcode = static_cast<std::uint8_t>(static_cast<unsigned>(form) + 2u * static_cast<unsigned>(kind) +
                                                  (scalar ? 1u : 0u));
    as.vex_rr(VexOp{1, 2, opcode, dbl}, false, x64::id(d), x64::id(b), x64::id(c));
}

void FpEmit::fma(FmaKind kind, FmaForm form, bool dbl, bool scalar, Xmm d, Xmm b, const Opnd& c) {
    if (!c.is_mem) { fma(kind, form, dbl, scalar, d, b, c.reg); return; }
    if (!feat.fma) throw std::logic_error("fp: FMA used without host support");
    const auto opcode = static_cast<std::uint8_t>(static_cast<unsigned>(form) + 2u * static_cast<unsigned>(kind) +
                                                  (scalar ? 1u : 0u));
    as.vex_rm(VexOp{1, 2, opcode, dbl}, false, x64::id(d), x64::id(b), c.mem);
}

void FpEmit::movd(Xmm d, Gp s) {
    if (avx()) as.vex_rr(VexOp{1, 1, 0x6e, false}, false, x64::id(d), 0, x64::id(s));
    else as.movd(d, s);
}
void FpEmit::movq(Xmm d, Gp s) {
    if (avx()) as.vex_rr(VexOp{1, 1, 0x6e, true}, false, x64::id(d), 0, x64::id(s));
    else as.movq(d, s);
}
void FpEmit::movd(Gp d, Xmm s) {
    if (avx()) as.vex_rr(VexOp{1, 1, 0x7e, false}, false, x64::id(s), 0, x64::id(d));
    else as.movd(d, s);
}
void FpEmit::movq(Gp d, Xmm s) {
    if (avx()) as.vex_rr(VexOp{1, 1, 0x7e, true}, false, x64::id(s), 0, x64::id(d));
    else as.movq(d, s);
}
void FpEmit::movq(Xmm d, Xmm s) { un(op::movq_xx, d, s); }

void FpEmit::ins0(Xmm d, Xmm s) {
    if (avx()) {
        as.vex_rr(vex_of(op::insertps), false, x64::id(d), x64::id(s), x64::id(s));
        as.db(0x0e);
    } else {
        as.sse_raw(op::insertps, false, x64::id(d), x64::id(s));
        as.db(0x0e);
    }
}

void FpEmit::cvtsi2s(bool dbl, Xmm d, Gp s, unsigned bits) {
    if (avx()) as.vex_rr(VexOp{static_cast<std::uint8_t>(dbl ? 3 : 2), 1, 0x2a, bits == 64}, false,
                         x64::id(d), x64::id(d), x64::id(s));
    else if (dbl) as.cvtsi2sd(d, s, bits);
    else as.cvtsi2ss(d, s, bits);
}

void FpEmit::ucomi(bool dbl, Xmm a, Xmm b, bool signal) {
    const SseOp o = signal ? (dbl ? op::comisd : op::comiss) : (dbl ? op::ucomisd : op::ucomiss);
    as.sse_raw(o, false, x64::id(a), x64::id(b));
}

void FpEmit::ucomi(bool dbl, Xmm a, const Opnd& b, bool signal) {
    if (!b.is_mem) { ucomi(dbl, a, b.reg, signal); return; }
    const SseOp o = signal ? (dbl ? op::comisd : op::comiss) : (dbl ? op::ucomisd : op::ucomiss);
    as.sse_raw(o, false, x64::id(a), b.mem);   // reads the low 4/8 bytes only
}

void FpEmit::cvtph2ps(Xmm d, Xmm s) {
    if (!feat.f16c) throw std::logic_error("fp: F16C used without host support");
    as.vex_rr(vop::vcvtph2ps, false, x64::id(d), 0, x64::id(s));
}

void FpEmit::cvtph2ps(Xmm d, const Opnd& s) {
    if (!s.is_mem) { cvtph2ps(d, s.reg); return; }
    if (!feat.f16c) throw std::logic_error("fp: F16C used without host support");
    as.vex_rm(vop::vcvtph2ps, false, x64::id(d), 0, s.mem);
}

void FpEmit::cvtps2ph(Xmm d, Xmm s) {
    if (!feat.f16c) throw std::logic_error("fp: F16C used without host support");
    as.vex_rr(vop::vcvtps2ph, false, x64::id(s), 0, x64::id(d));
    as.db(4);   // MXCSR.RC
}

namespace {
// Returns true if v (within `bits`) is a contiguous run of ones; lo/hi are
// the run bounds [lo, hi).
bool contiguous(std::uint64_t v, unsigned bits, unsigned& lo, unsigned& hi) {
    if (v == 0) return false;
    lo = 0;
    while (((v >> lo) & 1u) == 0) ++lo;
    hi = lo;
    while (hi < bits && ((v >> hi) & 1u) != 0) ++hi;
    const std::uint64_t mask = bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
    const std::uint64_t run = (hi - lo == 64) ? ~0ULL : (((1ULL << (hi - lo)) - 1) << lo);
    return (v & mask) == run;
}
} // namespace

Mem FpEmit::kconst(std::uint64_t lo, std::uint64_t hi) { return neon::constant(ctx, neon::Const128{lo, hi}); }

Mem FpEmit::ksplat(unsigned es, std::uint64_t value) {
    switch (es) {
    case 16: value = (value & 0xffff) * 0x0001000100010001ULL; break;
    case 32: value = (value & 0xffffffff) * 0x0000000100000001ULL; break;
    default: break;
    }
    return kconst(value, value);
}

Mem FpEmit::kscalar(unsigned es, std::uint64_t value) {
    if (es < 64) value &= (1ULL << es) - 1;
    return kconst(value, 0);
}

void FpEmit::splat(unsigned es, Xmm d, std::uint64_t value) {
    const unsigned bits = es == 64 ? 64 : 32;
    if (bits == 32) value &= 0xffffffffULL;
    if (value == 0) { zero(d); return; }
    if (avx()) {   // one load from the pool
        un(op::movups, d, Opnd::at(ksplat(es, value)));
        return;
    }
    unsigned lo = 0, hi = 0;
    if (contiguous(value, bits, lo, hi)) {
        ones(d);
        const std::uint8_t sl = bits == 64 ? 0x73 : 0x72;
        const unsigned left = bits - hi;          // shift left to drop bits above the run
        if (left + lo != 0) {
            if (lo == 0) {
                shift(sl, 2, d, d, static_cast<std::uint8_t>(left));                  // srl
            } else if (hi == bits) {
                shift(sl, 6, d, d, static_cast<std::uint8_t>(lo));                    // sll
            } else {
                shift(sl, 6, d, d, static_cast<std::uint8_t>(left + lo));
                shift(sl, 2, d, d, static_cast<std::uint8_t>(left));
            }
        }
        return;
    }
    const auto g = const_gpr();
    as.mov_imm(g, value);
    if (bits == 32) {
        movd(d, g);
        un_i(op::pshufd, d, d, 0);
    } else {
        movq(d, g);
        op(op::punpcklqdq, d, d, d);
    }
}

void FpEmit::scalar_const(unsigned es, Xmm d, std::uint64_t value) {
    if (es != 64) value &= 0xffffffffULL;
    if (value == 0) { zero(d); return; }
    if (avx()) {   // one load from the pool
        un(op::movups, d, Opnd::at(kscalar(es == 64 ? 64 : 32, value)));
        return;
    }
    const auto g = const_gpr();
    as.mov_imm(g, value);
    if (es == 64) movq(d, g);
    else movd(d, g);
}

void FpEmit::zero_upper(Xmm d, unsigned width) {
    switch (width) {
    case 128: return;
    case 64: movq(d, d); return;
    case 32: ins0(d, d); return;
    case 16: pslldq(d, d, 14); psrldq(d, d, 14); return;
    default: throw std::logic_error("fp: bad width");
    }
}

void FpEmit::commit(unsigned rd, Xmm t, unsigned width) {
    const auto d = dest(rd);
    switch (width) {
    case 128: mov(d, t); return;
    case 64: movq(d, t); return;
    case 32: ins0(d, t); return;
    case 16: pslldq(d, t, 14); psrldq(d, d, 14); return;
    default: throw std::logic_error("fp: bad width");
    }
}

std::uint64_t expand_fp_imm(unsigned imm8, unsigned es) {
    const std::uint64_t sign = (imm8 >> 7) & 1u;
    const std::uint64_t b6 = (imm8 >> 6) & 1u;
    const std::uint64_t b54 = (imm8 >> 4) & 3u;
    const std::uint64_t frac4 = imm8 & 15u;
    const unsigned e = es == 16 ? 5 : es == 32 ? 8 : 11;
    const unsigned f = es - e - 1;
    // exp = NOT(b6) : Replicate(b6, e-3) : b5b4
    std::uint64_t exp = ((b6 ^ 1u) << (e - 1)) | b54;
    if (b6) exp |= ((1ULL << (e - 3)) - 1) << 2;
    return (sign << (es - 1)) | (exp << f) | (frac4 << (f - 4));
}

// ---- decoder ------------------------------------------------------------------------------

namespace {

unsigned ftype_es(unsigned ftype) {
    switch (ftype) {
    case 0: return 32;
    case 1: return 64;
    case 3: return 16;
    default: return 0;
    }
}

Shape scalar_shape(unsigned es) { Shape s; s.es = es; s.scalar = true; s.q = false; return s; }
Shape vector_shape(unsigned es, bool q) { Shape s; s.es = es; s.scalar = false; s.q = q; return s; }

void set_regs(Inst& in, std::uint32_t w) {
    in.rd = static_cast<std::uint8_t>(w & 31);
    in.rn = static_cast<std::uint8_t>((w >> 5) & 31);
    in.rm = static_cast<std::uint8_t>((w >> 16) & 31);
    in.ra = static_cast<std::uint8_t>((w >> 10) & 31);
}

// Scalar floating-point data processing (op0 = x0x1 1110 / 1111).
bool decode_scalar_fp(std::uint32_t w, Inst& in) {
    set_regs(in, w);
    // FP data-processing (3 source)
    if ((w & 0xff000000u) == 0x1f000000u) {
        const unsigned es = ftype_es((w >> 22) & 3);
        if (!es) return false;
        const bool o1 = (w >> 21) & 1, o0 = (w >> 15) & 1;
        in.k = !o1 ? (!o0 ? K::fmadd : K::fmsub) : (!o0 ? K::fnmadd : K::fnmsub);
        in.sh = scalar_shape(es);
        return true;
    }
    // FP <-> fixed point
    if ((w & 0x7f200000u) == 0x1e000000u) {
        const bool sf = (w >> 31) & 1;
        const unsigned es = ftype_es((w >> 22) & 3);
        const unsigned rmode = (w >> 19) & 3, opc = (w >> 16) & 7, scale = (w >> 10) & 63;
        if (!es || (!sf && scale < 32)) return false;
        in.sh = scalar_shape(es);
        in.fbits = static_cast<std::uint8_t>(64 - scale);
        in.ibits = sf ? 64 : 32;
        in.gpr = true;
        if (rmode == 0 && (opc == 2 || opc == 3)) {
            in.k = K::cvt_i2f;
            in.is_unsigned = opc == 3;
            return true;
        }
        if (rmode == 3 && (opc == 0 || opc == 1)) {
            in.k = K::cvt_f2i;
            in.rnd = Rnd::z;
            in.is_unsigned = opc == 1;
            return true;
        }
        return false;
    }
    if ((w & 0x5f200000u) != 0x1e200000u) return false;
    if ((w >> 29) & 1) return false;   // S bit
    const unsigned ftype = (w >> 22) & 3;
    const unsigned es = ftype_es(ftype);
    const bool m_bit = (w >> 31) & 1;
    switch ((w >> 10) & 3) {
    case 1:   // conditional compare
        if (m_bit || !es) return false;
        in.k = K::fccmp;
        in.sh = scalar_shape(es);
        in.cond = static_cast<std::uint8_t>((w >> 12) & 15);
        in.nzcv = static_cast<std::uint8_t>(w & 15);
        in.signal = (w >> 4) & 1;
        return true;
    case 2: {   // data-processing (2 source)
        if (m_bit || !es) return false;
        static constexpr K kOps[9] = {K::fmul, K::fdiv, K::fadd, K::fsub, K::fmax, K::fmin,
                                      K::fmaxnm, K::fminnm, K::fnmul};
        const unsigned opcode = (w >> 12) & 15;
        if (opcode > 8) return false;
        in.k = kOps[opcode];
        in.sh = scalar_shape(es);
        return true;
    }
    case 3:   // conditional select
        if (m_bit || !es) return false;
        in.k = K::fcsel;
        in.sh = scalar_shape(es);
        in.cond = static_cast<std::uint8_t>((w >> 12) & 15);
        return true;
    default: break;
    }
    // bits 11:10 == 00
    if ((w >> 12) & 1) {   // immediate: bits 12:10 = 100
        if (m_bit || !es || ((w >> 5) & 31) != 0) return false;
        in.k = K::fmov_imm;
        in.sh = scalar_shape(es);
        in.imm = expand_fp_imm((w >> 13) & 0xff, es);
        return true;
    }
    if (((w >> 12) & 3) == 2) {   // compare: bits 13:10 = 1000
        if (m_bit || !es || ((w >> 14) & 3) != 0 || (w & 7) != 0) return false;
        in.k = K::fcmp;
        in.sh = scalar_shape(es);
        in.zero = (w >> 3) & 1;
        in.signal = (w >> 4) & 1;
        return true;
    }
    if (((w >> 12) & 7) == 4) {   // 1 source: bits 14:10 = 10000
        if (m_bit || !es) return false;
        const unsigned opcode = (w >> 15) & 63;
        in.sh = scalar_shape(es);
        switch (opcode) {
        case 0: in.k = K::fmov; return true;
        case 1: in.k = K::fabs; return true;
        case 2: in.k = K::fneg; return true;
        case 3: in.k = K::fsqrt; return true;
        case 4: case 5: case 7: {
            const unsigned dst = ftype_es(opcode & 3);
            if (dst == es) return false;
            in.k = K::fcvt;
            in.dst_es = static_cast<std::uint8_t>(dst);
            return true;
        }
        case 8: in.k = K::frint; in.rnd = Rnd::n; return true;
        case 9: in.k = K::frint; in.rnd = Rnd::p; return true;
        case 10: in.k = K::frint; in.rnd = Rnd::m; return true;
        case 11: in.k = K::frint; in.rnd = Rnd::z; return true;
        case 12: in.k = K::frint; in.rnd = Rnd::a; return true;
        case 14: in.k = K::frint; in.rnd = Rnd::x; return true;
        case 15: in.k = K::frint; in.rnd = Rnd::i; return true;
        case 0x10: case 0x11: case 0x12: case 0x13:   // FRINT32Z/X, FRINT64Z/X (FEAT_FRINTTS)
            if (es == 16) return false;
            in.k = K::frint_n;
            in.rnd = (opcode & 1) ? Rnd::x : Rnd::z;
            in.ibits = (opcode & 2) ? 64 : 32;
            return true;
        default: return false;
        }
    }
    if (((w >> 10) & 63) == 0) {   // FP <-> integer: bits 15:10 = 000000
        const bool sf = m_bit;
        const unsigned rmode = (w >> 19) & 3, opc = (w >> 16) & 7;
        in.ibits = sf ? 64 : 32;
        in.gpr = true;
        const unsigned sel = (rmode << 3) | opc;
        if (sel == 0b01110 || sel == 0b01111) {   // FMOV Xd, Vn.D[1] / Vd.D[1], Xn
            if (!sf || ftype != 2) return false;
            in.k = sel == 0b01110 ? K::fmov_to_gpr_hi : K::fmov_from_gpr_hi;
            in.sh = scalar_shape(64);
            return true;
        }
        if (!es) return false;
        in.sh = scalar_shape(es);
        switch (sel) {
        case 0b00000: in.k = K::cvt_f2i; in.rnd = Rnd::n; return true;
        case 0b00001: in.k = K::cvt_f2i; in.rnd = Rnd::n; in.is_unsigned = true; return true;
        case 0b00100: in.k = K::cvt_f2i; in.rnd = Rnd::a; return true;
        case 0b00101: in.k = K::cvt_f2i; in.rnd = Rnd::a; in.is_unsigned = true; return true;
        case 0b01000: in.k = K::cvt_f2i; in.rnd = Rnd::p; return true;
        case 0b01001: in.k = K::cvt_f2i; in.rnd = Rnd::p; in.is_unsigned = true; return true;
        case 0b10000: in.k = K::cvt_f2i; in.rnd = Rnd::m; return true;
        case 0b10001: in.k = K::cvt_f2i; in.rnd = Rnd::m; in.is_unsigned = true; return true;
        case 0b11000: in.k = K::cvt_f2i; in.rnd = Rnd::z; return true;
        case 0b11001: in.k = K::cvt_f2i; in.rnd = Rnd::z; in.is_unsigned = true; return true;
        case 0b00010: in.k = K::cvt_i2f; return true;
        case 0b00011: in.k = K::cvt_i2f; in.is_unsigned = true; return true;
        case 0b00110: case 0b00111:
            if (es == 32 && sf) return false;
            if (es == 64 && !sf) return false;
            in.k = sel == 0b00110 ? K::fmov_to_gpr : K::fmov_from_gpr;
            return true;
        case 0b11110:
            if (sf || es != 64) return false;
            in.k = K::fjcvtzs;
            return true;
        default: return false;
        }
    }
    return false;
}

// Element size for "size" encodings of vector FP: sz bit -> S/D.
unsigned sz_es(std::uint32_t w) { return ((w >> 22) & 1) ? 64 : 32; }

// AdvSIMD scalar (bit 30 = 1, bit 28 = 1).
bool decode_simd_scalar(std::uint32_t w, Inst& in) {
    set_regs(in, w);
    const bool u = (w >> 29) & 1;
    const bool a = (w >> 23) & 1;
    // three same
    if ((w & 0xdf200400u) == 0x5e200400u) {
        const unsigned opcode = (w >> 11) & 31;
        const unsigned es = sz_es(w);
        in.sh = scalar_shape(es);
        if (!u) {
            if (!a && opcode == 0x1b) { in.k = K::fmulx; return true; }
            if (!a && opcode == 0x1c) { in.k = K::fcmeq; return true; }
            if (opcode == 0x1f) { in.k = a ? K::frsqrts : K::frecps; return true; }
            return false;
        }
        if (!a && opcode == 0x1c) { in.k = K::fcmge; return true; }
        if (!a && opcode == 0x1d) { in.k = K::facge; return true; }
        if (a && opcode == 0x1a) { in.k = K::fabd; return true; }
        if (a && opcode == 0x1c) { in.k = K::fcmgt; return true; }
        if (a && opcode == 0x1d) { in.k = K::facgt; return true; }
        return false;
    }
    // three same FP16
    if ((w & 0xdf60c400u) == 0x5e400400u) {
        const unsigned opcode = (w >> 11) & 7;
        in.sh = scalar_shape(16);
        if (!u) {
            if (!a && opcode == 3) { in.k = K::fmulx; return true; }
            if (!a && opcode == 4) { in.k = K::fcmeq; return true; }
            if (opcode == 7) { in.k = a ? K::frsqrts : K::frecps; return true; }
            return false;
        }
        if (!a && opcode == 4) { in.k = K::fcmge; return true; }
        if (!a && opcode == 5) { in.k = K::facge; return true; }
        if (a && opcode == 2) { in.k = K::fabd; return true; }
        if (a && opcode == 4) { in.k = K::fcmgt; return true; }
        if (a && opcode == 5) { in.k = K::facgt; return true; }
        return false;
    }
    // two-register misc (and FP16 variant)
    const bool misc = (w & 0xdf3e0c00u) == 0x5e200800u;
    const bool misc16 = (w & 0xdf7e0c00u) == 0x5e780800u;
    if (misc || misc16) {
        const unsigned opcode = (w >> 12) & 31;
        const unsigned es = misc16 ? 16 : sz_es(w);
        in.sh = scalar_shape(es);
        if (misc && u && !a && opcode == 0x16) {   // FCVTXN Sd, Dn
            if (es != 64) return false;
            in.k = K::fcvtxn;
            return true;
        }
        if (!a) {
            switch (opcode) {
            case 0x1a: in.k = K::cvt_f2i; in.rnd = Rnd::n; in.is_unsigned = u; return true;
            case 0x1b: in.k = K::cvt_f2i; in.rnd = Rnd::m; in.is_unsigned = u; return true;
            case 0x1c: in.k = K::cvt_f2i; in.rnd = Rnd::a; in.is_unsigned = u; return true;
            case 0x1d: in.k = K::cvt_i2f; in.is_unsigned = u; return true;
            default: return false;
            }
        }
        switch (opcode) {
        case 0x0c: in.k = u ? K::fcmge0 : K::fcmgt0; return true;
        case 0x0d: in.k = u ? K::fcmle0 : K::fcmeq0; return true;
        case 0x0e: if (u) return false; in.k = K::fcmlt0; return true;
        case 0x1a: in.k = K::cvt_f2i; in.rnd = Rnd::p; in.is_unsigned = u; return true;
        case 0x1b: in.k = K::cvt_f2i; in.rnd = Rnd::z; in.is_unsigned = u; return true;
        case 0x1d: in.k = u ? K::frsqrte : K::frecpe; return true;
        case 0x1f: if (u) return false; in.k = K::frecpx; return true;
        default: return false;
        }
    }
    // pairwise
    if ((w & 0xdf3e0c00u) == 0x5e300800u) {
        const unsigned opcode = (w >> 12) & 31;
        unsigned es;
        if (u) es = sz_es(w);
        else {
            if ((w >> 22) & 1) return false;
            es = 16;
        }
        in.sh = scalar_shape(es);
        switch (opcode) {
        case 0x0c: in.k = a ? K::fminnmp : K::fmaxnmp; return true;
        case 0x0d: if (a) return false; in.k = K::faddp; return true;
        case 0x0f: in.k = a ? K::fminp : K::fmaxp; return true;
        default: return false;
        }
    }
    // indexed element
    if ((w & 0xdf000400u) == 0x5f000000u) {
        const unsigned opcode = (w >> 12) & 15;
        const unsigned size = (w >> 22) & 3;
        const unsigned l = (w >> 21) & 1, m = (w >> 20) & 1, h = (w >> 11) & 1;
        if (!u && opcode == 1) in.k = K::fmla;
        else if (!u && opcode == 5) in.k = K::fmls;
        else if (!u && opcode == 9) in.k = K::fmul;
        else if (u && opcode == 9) in.k = K::fmulx;
        else return false;
        if (size == 0) {
            in.sh = scalar_shape(16);
            in.index = static_cast<std::int8_t>((h << 2) | (l << 1) | m);
            in.rm = static_cast<std::uint8_t>((w >> 16) & 15);
        } else if (size == 2) {
            in.sh = scalar_shape(32);
            in.index = static_cast<std::int8_t>((h << 1) | l);
        } else if (size == 3) {
            if (l) return false;
            in.sh = scalar_shape(64);
            in.index = static_cast<std::int8_t>(h);
        } else {
            return false;
        }
        return true;
    }
    // shift by immediate (fixed-point conversions)
    if ((w & 0xdf800400u) == 0x5f000400u) {
        const unsigned immh = (w >> 19) & 15, immhb = (w >> 16) & 127;
        const unsigned opcode = (w >> 11) & 31;
        if (immh == 0 || (opcode != 0x1c && opcode != 0x1f)) return false;
        unsigned es;
        if (immh & 8) es = 64;
        else if (immh & 4) es = 32;
        else if (immh & 2) es = 16;
        else return false;
        in.sh = scalar_shape(es);
        in.fbits = static_cast<std::uint8_t>(2 * es - immhb);
        in.is_unsigned = u;
        if (opcode == 0x1c) in.k = K::cvt_i2f;
        else { in.k = K::cvt_f2i; in.rnd = Rnd::z; }
        return true;
    }
    return false;
}

// AdvSIMD vector (bit 31 = 0, bit 28 = 0).
bool decode_simd_vector(std::uint32_t w, Inst& in) {
    set_regs(in, w);
    const bool q = (w >> 30) & 1;
    const bool u = (w >> 29) & 1;
    const bool a = (w >> 23) & 1;
    // three same / three same FP16
    const bool same = (w & 0x9f200400u) == 0x0e200400u && ((w >> 11) & 31) >= 0x18;
    const bool same16 = (w & 0x9f60c400u) == 0x0e400400u;
    if (same || same16) {
        unsigned opcode;
        unsigned es;
        if (same) {
            opcode = (w >> 11) & 7;   // low 3 bits of 11xxx
            es = sz_es(w);
            if (es == 64 && !q) return false;
        } else {
            opcode = (w >> 11) & 7;
            es = 16;
        }
        in.sh = vector_shape(es, q);
        switch (opcode) {
        case 0: in.k = u ? (a ? K::fminnmp : K::fmaxnmp) : (a ? K::fminnm : K::fmaxnm); return true;
        case 1:
            if (u) {   // FMLAL2/FMLSL2 (FEAT_FHM), sz = 0
                if (!same || ((w >> 22) & 1)) return false;
                in.k = a ? K::fmlsl : K::fmlal;
                in.upper = true;
                in.sh = vector_shape(32, q);
                return true;
            }
            in.k = a ? K::fmls : K::fmla;
            return true;
        case 2: in.k = u ? (a ? K::fabd : K::faddp) : (a ? K::fsub : K::fadd); return true;
        case 3:
            if (a) return false;
            in.k = u ? K::fmul : K::fmulx;
            return true;
        case 4:
            if (!u) { if (a) return false; in.k = K::fcmeq; return true; }
            in.k = a ? K::fcmgt : K::fcmge;
            return true;
        case 5:
            if (!u) {   // FMLAL/FMLSL (FEAT_FHM), sz = 0
                if (!same || ((w >> 22) & 1)) return false;
                in.k = a ? K::fmlsl : K::fmlal;
                in.sh = vector_shape(32, q);
                return true;
            }
            in.k = a ? K::facgt : K::facge;
            return true;
        case 6: in.k = u ? (a ? K::fminp : K::fmaxp) : (a ? K::fmin : K::fmax); return true;
        case 7:
            if (!u) { in.k = a ? K::frsqrts : K::frecps; return true; }
            if (a) return false;
            in.k = K::fdiv;
            return true;
        default: return false;
        }
    }
    // two-register misc / FP16 two-register misc
    const bool misc = (w & 0x9f3e0c00u) == 0x0e200800u;
    const bool misc16 = (w & 0x9f7e0c00u) == 0x0e780800u;
    if (misc || misc16) {
        const unsigned opcode = (w >> 12) & 31;
        const bool sz = (w >> 22) & 1;
        if (misc && !a && (opcode == 0x16 || opcode == 0x17)) {
            // FCVTN/FCVTN2/FCVTXN/FCVTXN2 (0x16), FCVTL/FCVTL2 (0x17)
            in.upper = q;
            if (opcode == 0x17) {
                if (u) return false;
                in.k = K::fcvtl;
                in.sh = vector_shape(sz ? 32 : 16, true);   // source elements
                in.dst_es = sz ? 64 : 32;
                return true;
            }
            if (u) {
                if (!sz) return false;
                in.k = K::fcvtxn;
            } else {
                in.k = K::fcvtn;
            }
            in.sh = vector_shape(sz ? 64 : 32, true);   // source elements
            in.dst_es = sz ? 32 : 16;
            return true;
        }
        const unsigned es = misc16 ? 16 : (sz ? 64 : 32);
        if (es == 64 && !q) return false;
        in.sh = vector_shape(es, q);
        if (!a) {
            switch (opcode) {
            case 0x18: in.k = K::frint; in.rnd = u ? Rnd::a : Rnd::n; return true;
            case 0x19: in.k = K::frint; in.rnd = u ? Rnd::x : Rnd::m; return true;
            case 0x1a: in.k = K::cvt_f2i; in.rnd = Rnd::n; in.is_unsigned = u; return true;
            case 0x1b: in.k = K::cvt_f2i; in.rnd = Rnd::m; in.is_unsigned = u; return true;
            case 0x1c: in.k = K::cvt_f2i; in.rnd = Rnd::a; in.is_unsigned = u; return true;
            case 0x1d: in.k = K::cvt_i2f; in.is_unsigned = u; return true;
            case 0x1e: case 0x1f:   // FRINT32Z/X, FRINT64Z/X (FEAT_FRINTTS; no FP16 form)
                if (misc16) return false;
                in.k = K::frint_n;
                in.rnd = u ? Rnd::x : Rnd::z;
                in.ibits = opcode == 0x1f ? 64 : 32;
                return true;
            default: return false;
            }
        }
        switch (opcode) {
        case 0x0c: in.k = u ? K::fcmge0 : K::fcmgt0; return true;
        case 0x0d: in.k = u ? K::fcmle0 : K::fcmeq0; return true;
        case 0x0e: if (u) return false; in.k = K::fcmlt0; return true;
        case 0x0f: in.k = u ? K::fneg : K::fabs; return true;
        case 0x18: if (u) return false; in.k = K::frint; in.rnd = Rnd::p; return true;
        case 0x19: in.k = K::frint; in.rnd = u ? Rnd::i : Rnd::z; return true;
        case 0x1a: in.k = K::cvt_f2i; in.rnd = Rnd::p; in.is_unsigned = u; return true;
        case 0x1b: in.k = K::cvt_f2i; in.rnd = Rnd::z; in.is_unsigned = u; return true;
        case 0x1d: in.k = u ? K::frsqrte : K::frecpe; return true;
        case 0x1f: if (!u) return false; in.k = K::fsqrt; return true;
        default: return false;
        }
    }
    // across lanes
    if ((w & 0x9f3e0c00u) == 0x0e300800u) {
        const unsigned opcode = (w >> 12) & 31;
        unsigned es;
        if (u) {
            if (((w >> 22) & 1) || !q) return false;
            es = 32;
        } else {
            if ((w >> 22) & 1) return false;
            es = 16;
        }
        in.sh = vector_shape(es, q);
        switch (opcode) {
        case 0x0c: in.k = a ? K::fminnmv : K::fmaxnmv; return true;
        case 0x0f: in.k = a ? K::fminv : K::fmaxv; return true;
        default: return false;
        }
    }
    // indexed element
    if ((w & 0x9f000400u) == 0x0f000000u) {
        const unsigned opcode = (w >> 12) & 15;
        const unsigned size = (w >> 22) & 3;
        const unsigned l = (w >> 21) & 1, m = (w >> 20) & 1, h = (w >> 11) & 1;
        if (size == 2 && (u ? (opcode == 8 || opcode == 12) : (opcode == 0 || opcode == 4))) {
            // FMLAL/FMLSL/FMLAL2/FMLSL2 (by element, FEAT_FHM): Vm.H[H:L:M], Rm 0..15
            in.k = (opcode & 4) ? K::fmlsl : K::fmlal;
            in.upper = u;
            in.sh = vector_shape(32, q);
            in.index = static_cast<std::int8_t>((h << 2) | (l << 1) | m);
            in.rm = static_cast<std::uint8_t>((w >> 16) & 15);
            return true;
        }
        if (!u && opcode == 1) in.k = K::fmla;
        else if (!u && opcode == 5) in.k = K::fmls;
        else if (!u && opcode == 9) in.k = K::fmul;
        else if (u && opcode == 9) in.k = K::fmulx;
        else return false;
        if (size == 0) {
            in.sh = vector_shape(16, q);
            in.index = static_cast<std::int8_t>((h << 2) | (l << 1) | m);
            in.rm = static_cast<std::uint8_t>((w >> 16) & 15);
        } else if (size == 2) {
            in.sh = vector_shape(32, q);
            in.index = static_cast<std::int8_t>((h << 1) | l);
        } else if (size == 3) {
            if (l || !q) return false;
            in.sh = vector_shape(64, q);
            in.index = static_cast<std::int8_t>(h);
        } else {
            return false;
        }
        return true;
    }
    // modified immediate: FMOV (vector, immediate), cmode == 1111
    if ((w & 0x9ff80400u) == 0x0f000400u) {
        const unsigned cmode = (w >> 12) & 15;
        if (cmode != 15) return false;
        const bool op = (w >> 29) & 1, o2 = (w >> 11) & 1;
        const unsigned imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
        unsigned es;
        if (!op && !o2) es = 32;
        else if (!op && o2) es = 16;
        else if (op && !o2 && q) es = 64;
        else return false;
        in.k = K::fmov_vimm;
        in.sh = vector_shape(es, q);
        in.imm = expand_fp_imm(imm8, es);
        return true;
    }
    // shift by immediate (fixed-point conversions)
    if ((w & 0x9f800400u) == 0x0f000400u) {
        const unsigned immh = (w >> 19) & 15, immhb = (w >> 16) & 127;
        const unsigned opcode = (w >> 11) & 31;
        if (immh == 0 || (opcode != 0x1c && opcode != 0x1f)) return false;
        unsigned es;
        if (immh & 8) { es = 64; if (!q) return false; }
        else if (immh & 4) es = 32;
        else if (immh & 2) es = 16;
        else return false;
        in.sh = vector_shape(es, q);
        in.fbits = static_cast<std::uint8_t>(2 * es - immhb);
        in.is_unsigned = u;
        if (opcode == 0x1c) in.k = K::cvt_i2f;
        else { in.k = K::cvt_f2i; in.rnd = Rnd::z; }
        return true;
    }
    return false;
}

} // namespace

bool decode(std::uint32_t w, Inst& in) {
    in = Inst{};
    // MRS/MSR FPCR/FPSR
    switch (w & 0xffffffe0u) {
    case 0xd53b4400u: in.k = K::mrs_fpcr; in.rd = w & 31; return true;
    case 0xd51b4400u: in.k = K::msr_fpcr; in.rd = w & 31; return true;
    case 0xd53b4420u: in.k = K::mrs_fpsr; in.rd = w & 31; return true;
    case 0xd51b4420u: in.k = K::msr_fpsr; in.rd = w & 31; return true;
    default: break;
    }
    // Data processing - scalar FP and Advanced SIMD: op0 bits 28:25 = x111.
    if (((w >> 25) & 7) != 7) return false;
    bool ok = false;
    if ((w & 0x5f000000u) == 0x1e000000u || (w & 0x5f000000u) == 0x1f000000u) {
        if (!((w >> 30) & 1)) ok = decode_scalar_fp(w, in);
    }
    if (!ok && (w & 0xdf000000u) >= 0x5e000000u && (w & 0xdf000000u) <= 0x5f000000u) ok = decode_simd_scalar(w, in);
    if (!ok && (w & 0x9f000000u) >= 0x0e000000u && (w & 0x9f000000u) <= 0x0f000000u) ok = decode_simd_vector(w, in);
    if (!ok) in = Inst{};
    return ok;
}

// ---- describe -----------------------------------------------------------------------------------

void describe(const Inst& in, InstructionUse& use) {
    use = InstructionUse{};
    const auto vr = [&](unsigned r) { use.vec_read |= 1u << r; };
    const auto vw = [&](unsigned r) { use.vec_write |= 1u << r; };
    const auto gr = [&](unsigned r) { if (r != 31) use.gpr_read |= 1u << r; };
    const auto gw = [&](unsigned r) { if (r != 31) use.gpr_write |= 1u << r; };
    switch (in.k) {
    case K::mrs_fpcr: case K::mrs_fpsr: gw(in.rd); return;
    case K::msr_fpcr: case K::msr_fpsr: gr(in.rd); return;
    case K::fcmp:
        vr(in.rn);
        if (!in.zero) vr(in.rm);
        use.writes_flags = true;
        return;
    case K::fccmp:
        vr(in.rn); vr(in.rm);
        use.reads_flags = use.writes_flags = true;
        return;
    case K::fcsel:
        vr(in.rn); vr(in.rm); vw(in.rd);
        use.reads_flags = true;
        return;
    case K::fmov_imm: case K::fmov_vimm: vw(in.rd); return;
    case K::fmov_to_gpr: case K::fmov_to_gpr_hi: vr(in.rn); gw(in.rd); return;
    case K::fmov_from_gpr: gr(in.rn); vw(in.rd); return;
    case K::fmov_from_gpr_hi: gr(in.rn); vr(in.rd); vw(in.rd); return;
    case K::cvt_f2i:
        vr(in.rn);
        if (in.gpr) gw(in.rd); else vw(in.rd);
        return;
    case K::cvt_i2f:
        if (in.gpr) gr(in.rn); else vr(in.rn);
        vw(in.rd);
        return;
    case K::fjcvtzs:
        vr(in.rn); gw(in.rd);
        use.writes_flags = true;
        return;
    case K::fmadd: case K::fmsub: case K::fnmadd: case K::fnmsub:
        vr(in.rn); vr(in.rm); vr(in.ra); vw(in.rd);
        return;
    case K::fmla: case K::fmls: case K::fmlal: case K::fmlsl:
        vr(in.rn); vr(in.rm); vr(in.rd); vw(in.rd);
        return;
    case K::fcvtn: case K::fcvtxn:
        vr(in.rn);
        if (in.upper) vr(in.rd);
        vw(in.rd);
        return;
    // one-source forms
    case K::fmov: case K::fabs: case K::fneg: case K::fsqrt: case K::frint: case K::frecpe:
    case K::frsqrte: case K::frecpx: case K::fcvt: case K::fcvtl: case K::frint_n:
    case K::fcmeq0: case K::fcmge0: case K::fcmgt0: case K::fcmle0: case K::fcmlt0:
    case K::fmaxv: case K::fminv: case K::fmaxnmv: case K::fminnmv:
        vr(in.rn); vw(in.rd);
        return;
    case K::faddp: case K::fmaxp: case K::fminp: case K::fmaxnmp: case K::fminnmp:
        vr(in.rn);
        if (!in.sh.scalar) vr(in.rm);
        vw(in.rd);
        return;
    case K::none: return;
    default:   // two-source arithmetic and compares
        vr(in.rn); vr(in.rm); vw(in.rd);
        return;
    }
}

namespace {

bool emit_inst(FpEmit& e, const Inst& in) {
    switch (in.k) {
    case K::mrs_fpcr: case K::msr_fpcr: case K::mrs_fpsr: case K::msr_fpsr:
        return emit_sysreg(e, in);
    case K::fmla: case K::fmls: case K::fmadd: case K::fmsub: case K::fnmadd: case K::fnmsub:
        return emit_fused(e, in);
    case K::fmov: case K::fabs: case K::fneg: case K::fsqrt: case K::frint:
        return emit_unary(e, in);
    case K::fcmp: case K::fccmp: case K::fcsel:
        return emit_select(e, in);
    case K::fmov_imm: case K::fmov_vimm: case K::fmov_to_gpr: case K::fmov_from_gpr:
    case K::fmov_to_gpr_hi: case K::fmov_from_gpr_hi:
        return emit_move(e, in);
    case K::cvt_f2i: case K::cvt_i2f: case K::fcvt: case K::fcvtl: case K::fcvtn: case K::fcvtxn:
    case K::fjcvtzs:
        return emit_convert(e, in);
    case K::frecpe: case K::frsqrte: case K::frecpx: case K::frecps: case K::frsqrts:
        return emit_estimate(e, in);
    case K::frint_n:
        return emit_frint_n(e, in);
    case K::fmlal: case K::fmlsl:
        return emit_fhm(e, in);
    case K::none:
        return false;
    default:
        return emit_arith(e, in);
    }
}

} // namespace

// Number of low bits of Vd an instruction defines; A64 zeroes the bits above
// (128: the whole register). Only meaningful when the instruction writes Vd.
unsigned result_width(const Inst& in) {
    switch (in.k) {
    case K::fmov_from_gpr_hi: return 128;   // writes D[1], keeps D[0]
    case K::fcvtl: return 128;
    case K::fcvtn: case K::fcvtxn:
        if (in.sh.scalar) return 32;          // FCVTXN Sd, Dn
        return in.upper ? 128 : 64;           // "2" forms keep the lower half
    case K::fcvt: return in.dst_es;
    case K::fmaxv: case K::fminv: case K::fmaxnmv: case K::fminnmv: return in.sh.es;
    default: return in.sh.scalar ? in.sh.es : (in.sh.q ? 128 : 64);
    }
}

namespace {

// FP16 support needs the half-precision extension on the guest side and F16C
// on the host for anything but plain moves.
bool supported(const Inst& in, const FpFeatures& f) {
    switch (in.k) {
    case K::fmla: case K::fmls: case K::fmadd: case K::fmsub: case K::fnmadd: case K::fnmsub:
    case K::frecps: case K::frsqrts:
        // Single-rounding fused operations: without FMA3 the instruction is
        // left to the interpreter (correct, slow; every Windows 11 x64 host
        // has FMA3).
        if (!f.fma) return false;
        break;
    case K::fmlal: case K::fmlsl:   // F16C widening + FMA3
        if (!f.fma || !f.f16c) return false;
        break;
    default:
        break;
    }
    if (in.sh.es == 16 || in.dst_es == 16) {
        switch (in.k) {
        case K::fmov_to_gpr: case K::fmov_from_gpr: case K::fmov_imm: case K::fmov_vimm:
        case K::fmov: case K::fabs: case K::fneg: case K::fcsel:
            break;
        default:
            if (!f.f16c) return false;
        }
    }
    return true;
}

} // namespace
} // namespace fpx

unsigned fp_emit(EmitContext& ctx, std::span<const std::uint32_t> words) {
    if (words.empty()) return 0;
    fpx::Inst in;
    if (!fpx::decode(words[0], in)) return 0;
    fpx::FpEmit e(ctx);
    if (!fpx::supported(in, e.feat)) return 0;
    auto& as = ctx.as();
    const auto start = as.size();
    neon::begin_instruction_constants(as);
    if (!fpx::emit_inst(e, in)) {
        if (as.size() != start) throw std::logic_error("fp: emitter declined after emitting code");
        neon::begin_instruction_constants(as);   // drop pending constants
        return 0;
    }
    neon::flush_instruction_constants(as);
    // A64 zeroes the bits above a scalar or 64-bit vector result: tell the
    // context so later scalar operations can skip their own zeroing.
    if (e.wrote_dest()) {
        const unsigned width = fpx::result_width(in);
        if (width < 128) ctx.note_vec_zero_above(e.dest_guest(), width < 32 ? 32 : width);
    }
    return 1;
}

bool fp_describe(std::uint32_t word, InstructionUse& use) {
    fpx::Inst in;
    if (!fpx::decode(word, in)) return false;
    fpx::describe(in, use);
    return true;
}

namespace fpx {
namespace {
int g_knob_override = -1;

std::uint32_t env_knob_bits() {
    static const std::uint32_t bits = [] {
        std::uint32_t b = 0;
        if (const char* v = std::getenv("TRANSMUTE_FX_FP")) {
            const std::string s = std::string(",") + v + ",";   // comma-separated tokens
            const auto has = [&](const char* k) { return s.find(std::string(",") + k + ",") != std::string::npos; };
            const bool all = has("old");
            if (all || has("noslot")) b |= fp_no_slot;
            if (all || has("oldimm")) b |= fp_old_imm;
            if (all || has("oldmask")) b |= fp_old_mask;
            if (all || has("oldcvt")) b |= fp_old_cvt;
            if (all || has("noconstminmax")) b |= fp_no_const_minmax;
            if (all || has("oldminmax")) b |= fp_old_minmax;
        }
        return b;
    }();
    return bits;
}
} // namespace

std::uint32_t fp_knob_bits() {
    return g_knob_override >= 0 ? static_cast<std::uint32_t>(g_knob_override) : env_knob_bits();
}
} // namespace fpx

void fp_set_knobs_for_testing(int bits) { fpx::g_knob_override = bits; }

const FamilyEmitter& fp_family() {
    // Code-generation knobs are part of the name, so translator_family_fingerprint()
    // (and the persistent cache key) changes with TRANSMUTE_FX_FP.
    static const std::string name = [] {
        std::string n = "fp";
        const auto b = fpx::env_knob_bits();
        if (b & fpx::fp_no_slot) n += "+noslot";
        if (b & fpx::fp_old_imm) n += "+oldimm";
        if (b & fpx::fp_old_mask) n += "+oldmask";
        n += (b & fpx::fp_old_cvt) ? "+oldcvt" : "+cvt2";   // +cvt2: the imm8 INT_MIN check
        n += (b & fpx::fp_no_const_minmax) ? "+noconstminmax" : "+constmm";
        n += (b & fpx::fp_old_minmax) ? "+oldminmax" : "+minmaxnan";
        // The core keeps V zero-above facts across labels (Plan::vzero_join; its
        // knob novzjoin is in the layout bits): cached fp code must follow it.
        n += "+vzj";
        return n;
    }();
    static const FamilyEmitter family{name.c_str(), &fp_emit, &fp_describe};
    return family;
}

} // namespace transmute::fx
