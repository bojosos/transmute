// Shared NEON emission helpers: feature selection, the constant pool and the
// VEX/legacy SSE operand helper (owner: neon).
#include "fx/translator/emit_neon_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace transmute::fx {

namespace {
int g_feature_level = -1;
}

void neon_set_feature_level(int level) { g_feature_level = level; }

namespace neon {

using x64::id;

namespace {
int g_knob_override = -1;

std::uint32_t env_knob_bits() {
    static const std::uint32_t bits = [] {
        std::uint32_t b = 0;
        if (const char* v = std::getenv("TRANSMUTE_FX_NEON")) {
            // Comma-separated tokens ("old" would otherwise match "nofold").
            const std::string s = std::string(",") + v + ",";
            const auto has = [&](const char* k) { return s.find(std::string(",") + k + ",") != std::string::npos; };
            const bool all = has("old");
            if (all || has("nofold")) b |= neon_no_fold;
            if (all || has("noslot")) b |= neon_no_slot;
            if (all || has("notbltemp")) b |= neon_no_tbl_dest;
            if (all || has("oldshl")) b |= neon_old_shl;
            if (all || has("nozero64")) b |= neon_no_zero64;
            if (all || has("nomaskfusion")) b |= neon_no_mask_fusion;
            if (all || has("notblderived")) b |= neon_no_tbl_derived;
        }
        return b;
    }();
    return bits;
}
} // namespace

std::uint32_t neon_knob_bits() {
    return g_knob_override >= 0 ? static_cast<std::uint32_t>(g_knob_override) : env_knob_bits();
}

void neon_set_knobs_for_testing(int bits) { g_knob_override = bits; }

const char* neon_knob_suffix() {
    static const std::string suffix = [] {
        std::string n;
        const auto b = env_knob_bits();
        if (b & neon_no_fold) n += "+nofold";
        if (b & neon_no_slot) n += "+noslot";
        if (b & neon_no_tbl_dest) n += "+notbltemp";
        if (b & neon_old_shl) n += "+oldshl";
        if (b & neon_no_zero64) n += "+nozero64";
        // Include the enabled form in the cache identity as well: old cached
        // regions must not hide the new lowering during an A/B comparison.
        n += (b & neon_no_mask_fusion) ? "+nomaskfusion" : "+maskfusion2";
        if (b & neon_no_tbl_derived) n += "+notblderived";
        return n;
    }();
    return suffix.c_str();
}

namespace {

Features compute_features() {
    const auto& f = cpu_features();
    const bool want_avx = g_feature_level < 0 || g_feature_level >= 1;
    const bool want_avx2 = g_feature_level < 0 || g_feature_level >= 2;
    Features r;
    r.avx = f.avx && f.os_avx && want_avx;
    r.avx2 = r.avx && f.avx2 && want_avx2;
    r.bmi2 = f.bmi2 && want_avx2;
    r.pclmul = f.pclmul;
    return r;
}

struct PoolEntry {
    Const128 value;
    x64::Label label;
};

struct Pool {
    x64::Assembler* region{};
    std::vector<PoolEntry> region_entries;
    x64::Assembler* owner{};
    std::vector<PoolEntry> pending;
};

thread_local Pool g_pool;

x64::Label find_or_add(std::vector<PoolEntry>& entries, x64::Assembler& as, Const128 value) {
    for (const auto& e : entries)
        if (e.value == value) return e.label;
    entries.push_back({value, as.new_label()});
    return entries.back().label;
}

void emit_entries(x64::Assembler& as, const std::vector<PoolEntry>& entries) {
    as.align(16);
    for (const auto& e : entries) {
        as.bind(e.label);
        as.dq(e.value.lo);
        as.dq(e.value.hi);
    }
}

} // namespace

const Features& features() noexcept {
    static thread_local int cached_level = -2;
    static thread_local Features cached;
    if (cached_level != g_feature_level) {
        cached = compute_features();
        cached_level = g_feature_level;
    }
    return cached;
}

Const128 bytes_const(const std::uint8_t (&b)[16]) {
    Const128 c;
    for (int i = 0; i < 8; ++i) c.lo |= static_cast<std::uint64_t>(b[i]) << (8 * i);
    for (int i = 0; i < 8; ++i) c.hi |= static_cast<std::uint64_t>(b[8 + i]) << (8 * i);
    return c;
}

Mem constant(EmitContext& ctx, Const128 value) {
    auto& as = ctx.as();
    if (g_pool.region == &as) return x64::rip(find_or_add(g_pool.region_entries, as, value));
    if (g_pool.owner != &as) {
        g_pool.pending.clear();
        g_pool.owner = &as;
    }
    return x64::rip(find_or_add(g_pool.pending, as, value));
}

void begin_instruction_constants(x64::Assembler& as) {
    // Pending constants are per instruction: anything left over (e.g. after
    // an exception in another assembler) is stale.
    g_pool.pending.clear();
    g_pool.owner = &as;
}

void flush_instruction_constants(x64::Assembler& as) {
    if (g_pool.owner != &as || g_pool.pending.empty()) return;
    const auto skip = as.new_label();
    if (g_pool.pending.size() <= 6) as.jmp_short(skip);
    else as.jmp(skip);
    emit_entries(as, g_pool.pending);
    as.bind(skip);
    g_pool.pending.clear();
}

// ---- guest sources ------------------------------------------------------------------

Src source(EmitContext& ctx, unsigned index) {
    const auto s = ctx.vsrc(index);
    Src r;
    r.reg = s.reg;
    r.mem = s.mem;
    r.is_mem = s.is_mem;
    r.guest = index;
    return r;
}

Xmm in_reg(EmitContext& ctx, const Src& s) { return s.is_mem ? ctx.read_vec(s.guest) : s.reg; }

Src element_source(EmitContext& ctx, unsigned index, unsigned size) {
    Mem slot;
    if (size >= 2 && features().avx && neon_on(neon_no_slot) && ctx.vec_slot(index, slot)) {
        Src r;
        r.mem = slot;
        r.is_mem = true;
        r.guest = index;
        return r;
    }
    return source(ctx, index);
}

BinOperands bin_operands(EmitContext& ctx, const Src& n, const Src& m, bool commutative) {
    if (!n.is_mem) return {n.reg, m};
    if (commutative && !m.is_mem) return {m.reg, n};
    return {in_reg(ctx, n), m};
}

// ---- V ----------------------------------------------------------------------------

V::V(EmitContext& c) : ctx(c), a(c.as()), avx(features().avx), avx2(features().avx2) {}



Xmm V::scratch() {
    if (!have_scratch_) {
        scratch_ = ctx.temp_vec();
        have_scratch_ = true;
    }
    return scratch_;
}

Xmm V::alias_tmp() {
    if (!have_alias_) {
        alias_ = ctx.temp_vec();
        have_alias_ = true;
    }
    return alias_;
}

void V::op(SseOp o, Xmm d, Xmm x, Xmm y, bool commutative) {
    if (avx) {
        a.vex_rr(vex_of(o), false, id(d), id(x), id(y));
        return;
    }
    if (d == x) {
        a.sse(o, d, y);
    } else if (d == y) {
        if (commutative) {
            a.sse(o, d, x);
        } else {
            const Xmm s = alias_tmp();
            mov(s, x);
            a.sse(o, s, y);
            mov(d, s);
        }
    } else {
        mov(d, x);
        a.sse(o, d, y);
    }
}

void V::op(SseOp o, Xmm d, Xmm x, const Mem& y) {
    if (avx) {
        a.vex_rm(vex_of(o), false, id(d), id(x), y);
        return;
    }
    mov(d, x);
    a.sse(o, d, y);
}

void V::op_imm(SseOp o, Xmm d, Xmm x, Xmm y, std::uint8_t imm) {
    if (avx) {
        a.vex_rr(vex_of(o), false, id(d), id(x), id(y));
        a.db(imm);
        return;
    }
    if (d == x) {
        a.sse_imm(o, d, y, imm);
    } else if (d == y) {
        const Xmm s = alias_tmp();
        mov(s, x);
        a.sse_imm(o, s, y, imm);
        mov(d, s);
    } else {
        mov(d, x);
        a.sse_imm(o, d, y, imm);
    }
}

void V::op_imm(SseOp o, Xmm d, Xmm x, const Mem& y, std::uint8_t imm) {
    if (avx) {
        a.vex_rm(vex_of(o), false, id(d), id(x), y, 1);
        a.db(imm);
        return;
    }
    mov(d, x);
    a.sse_imm(o, d, y, imm);
}

void V::un(SseOp o, Xmm d, Xmm x) {
    if (avx) a.vex_rr(vex_of(o), false, id(d), 0, id(x));
    else a.sse(o, d, x);
}

void V::un(SseOp o, Xmm d, const Mem& x) {
    if (avx) a.vex_rm(vex_of(o), false, id(d), 0, x);
    else a.sse(o, d, x);
}

void V::un_imm(SseOp o, Xmm d, Xmm x, std::uint8_t imm) {
    if (avx) {
        a.vex_rr(vex_of(o), false, id(d), 0, id(x));
        a.db(imm);
    } else {
        a.sse_imm(o, d, x, imm);
    }
}

void V::shift_imm(std::uint8_t group, unsigned ext, Xmm d, Xmm x, unsigned n) {
    const auto count = static_cast<std::uint8_t>(std::min(n, 255u));
    if (avx) {
        a.vex_rr(VexOp{1, 1, group, false}, false, ext, id(d), id(x));
        a.db(count);
        return;
    }
    mov(d, x);
    a.sse_shift_imm(group, ext, d, count);
}

void V::mov(Xmm d, Xmm x) {
    if (d == x) return;
    if (avx) a.vex_rr(vex_of(op::movaps), false, id(d), 0, id(x));
    else a.sse(op::movaps, d, x);
}

void V::zero(Xmm d) { op(op::pxor, d, d, d, true); }
void V::ones(Xmm d) { op(op::pcmpeqd, d, d, d, true); }
void V::movq(Xmm d, Xmm x) { un(op::movq_load, d, x); }

void V::load_const(Xmm d, Const128 c) {
    if (c.lo == 0 && c.hi == 0) zero(d);
    else if (c.lo == ~0ULL && c.hi == ~0ULL) ones(d);
    else un(op::movdqa, d, k(c));
}

void V::blendv(Xmm d, Xmm x, Xmm y, Xmm mask) {
    if (avx) {
        a.vex_is4(vop::vpblendvb, d, x, y, mask);
        return;
    }
    // d = (y & mask) | (x & ~mask), using only the helper scratch.
    const Xmm s = scratch();
    if (d == mask) {
        op(op::pand, s, y, mask, true);
        op(op::pandn, d, mask, x);
        op(op::por, d, d, s, true);
    } else if (d == y) {
        op(op::pandn, s, mask, x);
        op(op::pand, d, d, mask, true);
        op(op::por, d, d, s, true);
    } else {
        op(op::pandn, d, mask, x);
        op(op::pand, s, y, mask, true);
        op(op::por, d, d, s, true);
    }
}

void V::blendv_lane(unsigned size, Xmm d, Xmm x, Xmm y, Xmm mask) {
    if (avx && (size == 2 || size == 3)) {
        a.vex_is4(size == 2 ? vop::vblendvps : vop::vblendvpd, d, x, y, mask);
        return;
    }
    blendv(d, x, y, mask);
}

void V::shl(unsigned size, Xmm d, Xmm x, unsigned n) {
    const unsigned esize = 8u << size;
    if (n >= esize) { zero(d); return; }
    if (n == 0) { mov(d, x); return; }
    switch (size) {
    case 0:
        psllw(d, x, n);
        op(op::pand, d, d, k(splat8(static_cast<std::uint8_t>(0xffu << n))));
        break;
    case 1: psllw(d, x, n); break;
    case 2: pslld(d, x, n); break;
    default: psllq(d, x, n); break;
    }
}

void V::srl(unsigned size, Xmm d, Xmm x, unsigned n) {
    const unsigned esize = 8u << size;
    if (n >= esize) { zero(d); return; }
    if (n == 0) { mov(d, x); return; }
    switch (size) {
    case 0:
        psrlw(d, x, n);
        op(op::pand, d, d, k(splat8(static_cast<std::uint8_t>(0xffu >> n))));
        break;
    case 1: psrlw(d, x, n); break;
    case 2: psrld(d, x, n); break;
    default: psrlq(d, x, n); break;
    }
}

void V::sra(unsigned size, Xmm d, Xmm x, unsigned n) {
    const unsigned esize = 8u << size;
    if (n == 0) { mov(d, x); return; }
    if (n >= esize) { sign_mask(size, d, x); return; }
    switch (size) {
    case 0:
        // ((x ^ 0x80) >>u n) - (0x80 >> n)
        op(op::pxor, d, x, k(splat8(0x80)));
        psrlw(d, d, n);
        op(op::pand, d, d, k(splat8(static_cast<std::uint8_t>(0xffu >> n))));
        op(op::psubb, d, d, k(splat8(static_cast<std::uint8_t>(0x80u >> n))));
        break;
    case 1: psraw(d, x, n); break;
    case 2: psrad(d, x, n); break;
    default:
        op(op::pxor, d, x, k(splat64(0x8000000000000000ULL)));
        psrlq(d, d, n);
        op(op::psubq, d, d, k(splat64(0x8000000000000000ULL >> n)));
        break;
    }
}

void V::sign_mask(unsigned size, Xmm d, Xmm x) {
    switch (size) {
    case 0: {
        if (d != x) {
            zero(d);
            op(op::pcmpgtb, d, d, x);
        } else {
            const Xmm s = scratch();
            zero(s);
            op(op::pcmpgtb, d, s, x);
        }
        break;
    }
    case 1: psraw(d, x, 15); break;
    case 2: psrad(d, x, 31); break;
    default:
        psrad(d, x, 31);
        pshufd(d, d, 0xf5);
        break;
    }
}

void V::cmp_gt_s(unsigned size, Xmm d, Xmm x, Xmm y) { op(kCmpGt[size], d, x, y); }

void V::cmp_gt_u(unsigned size, Xmm d, Xmm x, Xmm y) {
    const auto bias = splat(size, 1ULL << ((8u << size) - 1));
    const Xmm s = scratch();
    op(op::pxor, s, y, k(bias));
    op(op::pxor, d, x, k(bias));
    op(kCmpGt[size], d, d, s);
}

void V::cmp_ge_u(unsigned size, Xmm d, Xmm x, Xmm y) {
    if (size < 3) {
        const Xmm s = scratch();
        op(kMaxU[size], s, x, y, true);
        op(kCmpEq[size], d, s, x, true);
        return;
    }
    cmp_gt_u(size, d, y, x);
    not_(d, d);
}

void V::cmp_ge_s(unsigned size, Xmm d, Xmm x, Xmm y) {
    if (size < 3) {
        const Xmm s = scratch();
        op(kMaxS[size], s, x, y, true);
        op(kCmpEq[size], d, s, x, true);
        return;
    }
    op(op::pcmpgtq, d, y, x);
    not_(d, d);
}

void V::max_s(unsigned size, Xmm d, Xmm x, Xmm y) {
    if (size < 3) { op(kMaxS[size], d, x, y, true); return; }
    const Xmm m = temp();
    op(op::pcmpgtq, m, x, y);
    blendv(d, y, x, m);
}

void V::min_s(unsigned size, Xmm d, Xmm x, Xmm y) {
    if (size < 3) { op(kMinS[size], d, x, y, true); return; }
    const Xmm m = temp();
    op(op::pcmpgtq, m, x, y);
    blendv(d, x, y, m);
}

void V::max_u(unsigned size, Xmm d, Xmm x, Xmm y) {
    if (size < 3) { op(kMaxU[size], d, x, y, true); return; }
    const Xmm m = temp();
    cmp_gt_u(3, m, x, y);
    blendv(d, y, x, m);
}

void V::min_u(unsigned size, Xmm d, Xmm x, Xmm y) {
    if (size < 3) { op(kMinU[size], d, x, y, true); return; }
    const Xmm m = temp();
    cmp_gt_u(3, m, x, y);
    blendv(d, x, y, m);
}

void V::keep_element0(unsigned size, Xmm d, Xmm x) {
    switch (size) {
    case 0: op(op::pand, d, x, k(Const128{0xff, 0})); break;
    case 1: op(op::pand, d, x, k(Const128{0xffff, 0})); break;
    case 2: op_imm(op::insertps, d, x, x, 0x0e); break;
    default: movq(d, x); break;
    }
}

} // namespace neon

// ---- region constant pool ------------------------------------------------------------

void neon_begin_region(x64::Assembler& as) {
    neon::g_pool.region = &as;
    neon::g_pool.region_entries.clear();
}

void neon_end_region(x64::Assembler& as) {
    auto& pool = neon::g_pool;
    if (pool.region != &as) return;
    if (!pool.region_entries.empty()) neon::emit_entries(as, pool.region_entries);
    pool.region_entries.clear();
    pool.region = nullptr;
}

} // namespace transmute::fx
