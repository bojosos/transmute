// Atomics/system family dispatch, barriers, hints, MRS/MSR, PSTATE flag
// manipulation and cache maintenance (owner: atomics/system).
#include "fx/translator/emit_system.hpp"
#include "fx/translator/emit_system_internal.hpp"

#include "fx/common/cpu_features.hpp"

#include <intrin.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace transmute::fx {
namespace system_detail {
namespace {

// TRANSMUTE_FX_SYSTEM=<knobs> (read once; folded into the family name and so
// into translator_family_fingerprint()):
//   oldorder   STLR = xchg, LDAR plain, DMB never elided, DSB LD/ST no code
//              (the mapping before kTranslatorAbiVersion 11)
//   oldfusion  no flag-setting LL/SC bodies (ADDS/SUBS) and only MOV between
//              a fused loop and the barrier it consumes
bool env_knob(const char* name) {
    const char* v = std::getenv("TRANSMUTE_FX_SYSTEM");
    return v && std::strstr(v, name) != nullptr;
}

std::atomic<bool> g_fusion{true};
std::atomic<bool> g_extensions{true};
std::atomic<bool> g_inline_counter{true};
std::atomic<bool> g_legacy_ordering{env_knob("oldorder")};
std::atomic<bool> g_extended_fusion{!env_knob("oldfusion")};

HostFeatures detect_host() {
    const auto& f = cpu_features();
    HostFeatures h;
    h.aes = f.aes;
    h.pclmul = f.pclmul;
    h.sha = f.sha && f.sse41;
    h.bmi2 = f.bmi2;
    int r[4]{};
    __cpuid(r, 0);
    const int max_leaf = r[0];
    __cpuid(r, 1);
    h.rdrand = (static_cast<unsigned>(r[2]) & (1u << 30)) != 0;
    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        h.rdseed = (static_cast<unsigned>(r[1]) & (1u << 18)) != 0;
    }
    __cpuid(r, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(r[0]) >= 0x80000007u) {
        __cpuid(r, static_cast<int>(0x80000007u));
        h.invariant_tsc = (static_cast<unsigned>(r[3]) & (1u << 8)) != 0;
    }
    return h;
}

using x64::Alu;
using x64::Cond;

unsigned emit_hint(EmitContext& ctx, std::uint32_t w) {
    const unsigned imm = (w >> 5) & 0x7fu;
    // YIELD (1), WFE (2), WFI (3): spin-wait hint. Everything else in the hint
    // space (NOP, SEV/SEVL, BTI, PAC*/AUT*/XPACLRI with PAuth disabled, ESB,
    // CSDB, PSB, TSB, DGH, CLRBHB, CHKFEAT, GCSB...) executes as a NOP.
    if (imm >= 1 && imm <= 3) ctx.as().pause();
    return 1;
}

unsigned emit_barrier(EmitContext& ctx, std::span<const std::uint32_t> words) {
    const auto w = words[0];
    const unsigned op2 = (w >> 5) & 7u, crm = (w >> 8) & 15u;
    switch (op2) {
    case 2:   // CLREX
        ctx.as().mov(32, excl_size_mem(), 0);
        return 1;
    case 1:   // DSB <option>nXS (CRm<1:0> == 10)
        if ((crm & 3u) != 2u) return 0;
        full_barrier(ctx);
        return 1;
    case 4:   // DSB (CRm 0000 = SSBB, 0100 = PSSBB: speculation barriers)
        if (crm == 0 || crm == 4) return 1;
        // A DSB completes only when the earlier accesses of its type are
        // complete, and no later instruction executes before that: even DSB
        // ST/LD order earlier stores before later loads. Always a fence
        // (rare in user code; never elided).
        if (!legacy_ordering() || (crm & 3u) == 3u || (crm & 3u) == 0u) full_barrier(ctx);
        return 1;
    case 5:   // DMB
        // LD (xx01) and ST (xx10) variants only order what x64 TSO already
        // orders (R->R, R->W, W->W). SY/ISH/OSH/NSH (xx11) and the reserved
        // xx00 encodings (which behave as SY) need store->load ordering.
        if ((crm & 3u) != 3u && (crm & 3u) != 0u) return 1;
        // A full DMB followed in straight-line code by only stores and
        // register-only instructions up to another full barrier or locked
        // instruction is subsumed by it (NOTES_SYSTEM.md, "Ordering").
        if (!legacy_ordering() && reaches_full_barrier(words, 1)) return 1;
        full_barrier(ctx);
        return 1;
    case 6:   // ISB: the code cache keeps translations coherent.
    case 7:   // SB
        return 1;
    default:
        return 0;
    }
}

// CFINV / XAFLAG / AXFLAG (FEAT_FlagM, FEAT_FlagM2).
unsigned emit_pstate(EmitContext& ctx, std::uint32_t w) {
    const unsigned op1 = (w >> 16) & 7u, crm = (w >> 8) & 15u, op2 = (w >> 5) & 7u;
    if (op1 != 0 || crm != 0 || op2 > 2) return 0;
    auto& a = ctx.as();
    ctx.clobber_host_flags();
    const Gp n = ctx.get_nzcv();
    if (op2 == 0) {   // CFINV
        const Gp t = ctx.temp_gpr();
        a.mov(32, t, n);
        a.xor_(32, t, 0x20000000);
        ctx.set_nzcv(t);
        return 1;
    }
    const Gp t = ctx.temp_gpr(), u = ctx.temp_gpr(), r = ctx.temp_gpr();
    if (op2 == 1) {   // XAFLAG
        a.mov(32, t, n);
        a.shr(32, t, 1);                 // Z@29 C@28
        a.mov(32, u, n);
        a.or_(32, u, t);                 // bit29 = C|Z
        a.mov(32, r, u);
        a.and_(32, r, 0x20000000);       // C' = C|Z
        a.not_(32, u);
        a.and_(32, u, 0x20000000);
        a.shl(32, u, 2);                 // N' = !(C|Z)
        a.or_(32, r, u);
        a.mov(32, u, n);
        a.not_(32, u);
        a.and_(32, t, u);                // bit29 = Z & !C
        a.and_(32, t, 0x20000000);
        a.shr(32, t, 1);                 // V' = Z & !C
        a.or_(32, r, t);
        a.mov(32, u, n);
        a.shl(32, u, 1);                 // C@30
        a.and_(32, u, n);
        a.and_(32, u, 0x40000000);       // Z' = Z & C
        a.or_(32, r, u);
    } else {          // AXFLAG
        a.mov(32, t, n);
        a.shl(32, t, 2);                 // V@30
        a.or_(32, t, n);
        a.and_(32, t, 0x40000000);       // Z' = Z|V
        a.mov(32, u, n);
        a.shl(32, u, 1);                 // V@29
        a.not_(32, u);
        a.and_(32, u, n);
        a.and_(32, u, 0x20000000);       // C' = C & !V
        a.mov(32, r, t);
        a.or_(32, r, u);
    }
    ctx.set_nzcv(r);
    return 1;
}

unsigned emit_sys(EmitContext& ctx, std::uint32_t w) {
    auto& a = ctx.as();
    const unsigned rt = rd_of(w);
    switch (w & 0xffffffe0u) {
    case 0xd50b7420u: {   // DC ZVA, Xt
        const Gp base = ctx.copy_gpr(rt);
        const Xmm zero = ctx.temp_vec();
        ctx.clobber_host_flags();
        a.and_(64, base, -64);
        a.pxor(zero, zero);
        for (int i = 0; i < 4; ++i) {
            ctx.mark_may_fault();
            a.movdqu(x64::ptr(base, 16 * i), zero);
        }
        return 1;
    }
    case 0xd50b7a20u:     // DC CVAC
    case 0xd50b7b20u:     // DC CVAU
    case 0xd50b7c20u:     // DC CVAP
    case 0xd50b7d20u:     // DC CVADP
    case 0xd50b7e20u:     // DC CIVAC
        return 1;
    case 0xd50b7520u:     // IC IVAU, Xt
        ctx.call_helper(helper_system_ic_ivau, {HelperArg::guest(rt)}, HelperEffects::pure);
        return 1;
    default:
        break;
    }
    if (w == 0xd508711fu || w == 0xd508751fu) {   // IC IALLUIS / IC IALLU
        ctx.call_helper(helper_system_ic_iallu, {}, HelperEffects::pure);
        return 1;
    }
    return 0;
}

void emit_counter(EmitContext& ctx, unsigned rt) {
    auto& a = ctx.as();
    if (!host().invariant_tsc || timer_multiplier() == 0) {
        const Gp r = ctx.call_helper(helper_system_counter, {}, HelperEffects::pure);
        const Gp d = ctx.write_gpr(rt);
        a.mov(64, d, r);
        return;
    }
    FixedGprs fx(ctx, {Gp::rax, Gp::rdx});
    const Gp scale = fx.temp(), result = fx.temp();
    ctx.clobber_host_flags();
    fx.save({});
    a.rdtsc();
    a.shl(64, Gp::rdx, 32);
    a.or_(64, Gp::rax, Gp::rdx);
    a.mov(64, scale, x64::ptr(kCtx, helper_disp(helper_system_timer_data)));
    a.unary(4, 64, x64::ptr(scale, 0));          // mul qword [scale]: rdx:rax = tsc * mult
    a.shrd(64, Gp::rax, Gp::rdx, 32);
    a.mov(64, result, Gp::rax);
    fx.restore();
    const Gp d = ctx.write_gpr(rt);
    a.mov(64, d, result);
}

// Virtualize the user-readable performance cycle counter with the host TSC.
// A renderer uses this register for elapsed-cycle measurements, independently of
// the fixed-frequency architectural timer exposed by CNTVCT/CNTFRQ.
void emit_cycle_counter(EmitContext& ctx, unsigned rt) {
    auto& a = ctx.as();
    FixedGprs fx(ctx, {Gp::rax, Gp::rdx});
    const Gp result = fx.temp();
    ctx.clobber_host_flags();
    fx.save({});
    a.rdtsc();
    a.shl(64, Gp::rdx, 32);
    a.or_(64, Gp::rax, Gp::rdx);
    a.mov(64, result, Gp::rax);
    fx.restore();
    a.mov(64, ctx.write_gpr(rt), result);
}

void emit_random(EmitContext& ctx, unsigned rt, bool reseed) {
    auto& a = ctx.as();
    const Gp value = ctx.temp_gpr(), flags = ctx.temp_gpr();
    a.mov_imm(flags, 0);
    ctx.clobber_host_flags();
    // rdrand/rdseed r64: REX.W 0F C7 /6 or /7. CF=1 on success, value 0 otherwise.
    a.emit_rr(0, true, {0x0f, 0xc7}, (reseed && host().rdseed) ? 7u : 6u, x64::id(value));
    a.setcc(Cond::nc, flags);
    a.shl(32, flags, 30);                        // failure: NZCV = 0100
    ctx.set_nzcv(flags);
    const Gp d = ctx.write_gpr(rt);
    a.mov(64, d, value);
}

unsigned emit_mrs(EmitContext& ctx, std::uint32_t w) {
    auto& a = ctx.as();
    const unsigned rt = rd_of(w);
    switch (w & 0xffffffe0u) {
    case 0xd53bd040u: {   // TPIDR_EL0
        const Gp d = ctx.write_gpr(rt);
        a.mov(64, d, ctx_mem(offsetof(GuestContext, tpidr_el0)));
        return 1;
    }
    case 0xd53bd060u: {   // TPIDRRO_EL0
        const Gp d = ctx.write_gpr(rt);
        a.mov(64, d, ctx_mem(offsetof(GuestContext, tpidrro_el0)));
        return 1;
    }
    case 0xd53b4200u: {   // NZCV
        ctx.clobber_host_flags();
        const Gp n = ctx.get_nzcv();
        const Gp t = ctx.temp_gpr();
        a.mov(32, t, n);
        a.and_(32, t, static_cast<std::int32_t>(0xf0000000u));
        const Gp d = ctx.write_gpr(rt);
        a.mov(64, d, t);
        return 1;
    }
    case 0xd53b00e0u: {   // DCZID_EL0
        const Gp d = ctx.write_gpr(rt);
        a.mov_imm(d, kSystemDczidEl0);
        return 1;
    }
    case 0xd53b0020u: {   // CTR_EL0
        const Gp d = ctx.write_gpr(rt);
        a.mov_imm(d, kSystemCtrEl0);
        return 1;
    }
    case 0xd53be000u: {   // CNTFRQ_EL0
        const Gp d = ctx.write_gpr(rt);
        a.mov_imm(d, kSystemCounterFrequency);
        return 1;
    }
    case 0xd53b9d00u:     // PMCCNTR_EL0
        emit_cycle_counter(ctx, rt);
        return 1;
    case 0xd53be020u:     // CNTPCT_EL0
    case 0xd53be040u:     // CNTVCT_EL0
    case 0xd53be0a0u:     // CNTPCTSS_EL0
    case 0xd53be0c0u:     // CNTVCTSS_EL0
        emit_counter(ctx, rt);
        return 1;
    case 0xd53b2400u:     // RNDR
    case 0xd53b2420u:     // RNDRRS
        if (!host().rdrand) return 0;
        emit_random(ctx, rt, (w & 0xffffffe0u) == 0xd53b2420u);
        return 1;
    default:
        return 0;         // FPCR/FPSR (fp family), EL1+ registers
    }
}

unsigned emit_msr(EmitContext& ctx, std::uint32_t w) {
    auto& a = ctx.as();
    const unsigned rt = rd_of(w);
    switch (w & 0xffffffe0u) {
    case 0xd51bd040u: {   // TPIDR_EL0
        const Gp v = ctx.read_gpr(rt);
        a.mov(64, ctx_mem(offsetof(GuestContext, tpidr_el0)), v);
        return 1;
    }
    case 0xd51b4200u: {   // NZCV
        const Gp t = ctx.copy_gpr(rt);
        ctx.clobber_host_flags();
        a.and_(32, t, static_cast<std::int32_t>(0xf0000000u));
        ctx.set_nzcv(t);
        return 1;
    }
    default:
        return 0;
    }
}

} // namespace

const HostFeatures& host() {
    static const HostFeatures real = detect_host();
    thread_local HostFeatures view;
    view = real;
    if (!g_extensions.load(std::memory_order_relaxed)) {
        view.aes = view.pclmul = view.sha = view.rdrand = view.rdseed = false;
    }
    if (!g_inline_counter.load(std::memory_order_relaxed)) view.invariant_tsc = false;
    return view;
}

bool fusion_enabled() { return g_fusion.load(std::memory_order_relaxed); }
bool extended_fusion() { return g_extended_fusion.load(std::memory_order_relaxed); }
bool legacy_ordering() { return g_legacy_ordering.load(std::memory_order_relaxed); }

// ---- memory ordering ---------------------------------------------------------------

namespace {
Mem release_pending_mem() { return ctx_mem(offsetof(GuestContext, release_pending)); }
} // namespace

// Legacy: `lock or [rsp], 0`, measured faster than mfence on the reference
// hosts (see NOTES_SYSTEM.md); [rsp] is the callee-owned shadow area. Locked
// instructions order all ordinary (WB) loads and stores, which is all
// translated code produces. Default: `lock and byte [release_pending], 0`, the same locked
// read-modify-write on a thread-private L1-resident line, which also clears
// the flag without leaving a store in the store buffer (a plain clear after
// the fence made the next fence wait for it: +2 ns per DMB in loops).
void full_barrier(EmitContext& ctx) {
    ctx.clobber_host_flags();
    auto& a = ctx.as();
    a.lock();
    if (legacy_ordering()) a.alu(x64::Alu::or_, 32, x64::ptr(Gp::rsp, 0), 0);
    else a.alu(x64::Alu::and_, 8, release_pending_mem(), 0);
}

void note_release(EmitContext& ctx, std::span<const std::uint32_t> words) {
    if (legacy_ordering()) return;
    // A full barrier reached through stores/register-only code drains the
    // store and clears the flag itself.
    if (reaches_full_barrier(words, 1)) return;
    ctx.as().mov(8, release_pending_mem(), 1);
}

void acquire_fence(EmitContext& ctx) {
    if (legacy_ordering()) return;
    auto& a = ctx.as();
    ctx.clobber_host_flags();
    const x64::Label skip = a.new_label();
    a.cmp(8, release_pending_mem(), 0);
    a.jcc_short(x64::Cond::e, skip);
    a.lock();
    a.alu(x64::Alu::and_, 8, release_pending_mem(), 0);   // fence + clear
    a.bind(skip);
}

bool is_fence_word(std::uint32_t w) {
    if ((w & 0xfffff01fu) != 0xd503301fu) return false;
    const unsigned op2 = (w >> 5) & 7u, crm = (w >> 8) & 15u;
    if (op2 == 1) return (crm & 3u) == 2u;                                   // DSB nXS
    if (op2 == 4) return crm != 0 && crm != 4 && (!legacy_ordering() || (crm & 3u) == 3u || (crm & 3u) == 0u);
    if (op2 == 5) return (crm & 3u) == 3u || (crm & 3u) == 0u;              // DMB full
    return false;
}

namespace {
// Translated without any guest memory read: data processing (immediate,
// register, SIMD&FP) and hints.
bool is_register_only(std::uint32_t w) {
    if ((w & 0x1c000000u) == 0x10000000u) return true;    // DP immediate
    if ((w & 0x0e000000u) == 0x0a000000u) return true;    // DP register
    if ((w & 0x0e000000u) == 0x0e000000u) return true;    // DP SIMD & FP
    return (w & 0xfffff01fu) == 0xd503201fu;              // NOP, YIELD, BTI, PAC hints...
}

// A store that reads no guest memory (plain, pair, release, SIMD structure).
bool is_plain_store(std::uint32_t w) {
    if ((w & 0x3a000000u) == 0x38000000u) {                // load/store register
        const bool v = (w & (1u << 26)) != 0;
        const unsigned opc = (w >> 22) & 3u;
        if (!(w & (1u << 24)) && (w & (1u << 21)) && ((w >> 10) & 3u) != 2u) return false;   // atomics, PAC
        return v ? (opc & 1u) == 0 : opc == 0;
    }
    if ((w & 0x3a000000u) == 0x28000000u) return (w & (1u << 22)) == 0;   // STP/STNP (all forms)
    if ((w & 0xbe000000u) == 0x0c000000u) return (w & (1u << 22)) == 0;   // ST1..ST4
    if ((w & 0x3fe00000u) == 0x08800000u) return true;                   // STLR/STLLR
    if ((w & 0x3fe00c00u) == 0x19000000u) return true;                   // STLUR (opc 00)
    return false;
}
} // namespace

bool reaches_rcsc_acquire(std::span<const std::uint32_t> words, std::size_t from) {
    constexpr std::size_t kScan = 8;
    for (std::size_t i = from; i < words.size() && i < from + kScan; ++i) {
        const auto w = words[i];
        if (is_fence_word(w)) return false;
        // LDAR/LDLAR (o2 = 1, L = 1, o1 = 0) and LDAXR/LDAXP (o2 = 0, L = 1, o0 = 1).
        if ((w & 0x3fe00000u) == 0x08c00000u || (w & 0x3fc08000u) == 0x08408000u) return true;
        // Anything else that cannot branch: data processing, hints, loads/stores.
        if (!is_register_only(w) && (w & 0x0a000000u) != 0x08000000u) return false;
    }
    return false;
}

bool reaches_full_barrier(std::span<const std::uint32_t> words, std::size_t from) {
    constexpr std::size_t kScan = 16;
    for (std::size_t i = from; i < words.size() && i < from + kScan; ++i) {
        const auto w = words[i];
        if (is_fence_word(w)) return true;
        if (!is_register_only(w) && !is_plain_store(w)) return false;
    }
    return false;
}

// ---- FixedGprs / FixedXmm0 ----------------------------------------------------

FixedGprs::FixedGprs(EmitContext& ctx, std::initializer_list<Gp> fixed) : ctx_(ctx) {
    for (auto f : fixed) {
        fixed_[count_] = f;
        claimed_[count_] = ctx.temp_gpr_prefer(f) == f;
        hooks_ = hooks_ || claimed_[count_];
        ++count_;
    }
    if (!hooks_) {
        // No hook: pool-ordered contexts may still hand the implicit registers
        // out first. Stop at the first other register (kept as a spare).
        for (;;) {
            bool pending = false;
            for (unsigned i = 0; i < count_; ++i) pending = pending || !claimed_[i];
            if (!pending) break;
            const Gp t = ctx.temp_gpr();
            bool matched = false;
            for (unsigned i = 0; i < count_; ++i) {
                if (!claimed_[i] && fixed_[i] == t) { claimed_[i] = true; matched = true; }
            }
            if (!matched) { spare_[spares_++] = t; break; }
        }
    }
    for (unsigned i = 0; i < count_; ++i) {
        if (!claimed_[i]) copy_[i] = ctx.temp_vec();
    }
}

Gp FixedGprs::temp() {
    if (saved_) throw std::logic_error("FixedGprs::temp after save");
    if (spares_ != 0) return spare_[--spares_];
    for (;;) {
        const Gp t = ctx_.temp_gpr();
        bool fixed = false;
        for (unsigned i = 0; i < count_; ++i) {
            if (!claimed_[i] && fixed_[i] == t) { claimed_[i] = true; fixed = true; }   // free after all
        }
        if (!fixed) return t;
    }
}

bool FixedGprs::clobbers_live() const {
    for (unsigned i = 0; i < count_; ++i) if (!claimed_[i]) return true;
    return false;
}

bool FixedGprs::claimed(Gp r) const {
    for (unsigned i = 0; i < count_; ++i) if (fixed_[i] == r) return claimed_[i];
    return false;
}

void FixedGprs::save(std::initializer_list<Gp*> operands) {
    auto& a = ctx_.as();
    for (auto* op : operands) {
        for (unsigned i = 0; i < count_; ++i) {
            if (!claimed_[i] && *op == fixed_[i]) {
                const Gp t = temp();
                a.mov(64, t, *op);
                *op = t;
            }
        }
    }
    saved_ = true;
    for (unsigned i = 0; i < count_; ++i) {
        if (!claimed_[i]) a.movq(copy_[i], fixed_[i]);
    }
}

void FixedGprs::restore() {
    auto& a = ctx_.as();
    for (unsigned i = 0; i < count_; ++i) {
        if (!claimed_[i]) a.movq(fixed_[i], copy_[i]);
    }
}

FixedXmm0::FixedXmm0(EmitContext& ctx, unsigned extra_temps) : ctx_(ctx) {
    bool have0 = false;
    for (unsigned i = 0; i < extra_temps + 1; ++i) {
        const Xmm t = ctx.temp_vec();
        if (t == Xmm::xmm0) have0 = true;
        else spare_[spare_count_++] = t;
    }
    if (!have0) {
        while (spare_count_ < extra_temps + 1) spare_[spare_count_++] = ctx.temp_vec();
        need_save_ = true;
        copy_ = spare_[--spare_count_];
    }
}

void FixedXmm0::save(std::initializer_list<Xmm*> operands) {
    if (!need_save_) return;
    ctx_.as().movaps(copy_, Xmm::xmm0);
    for (auto* op : operands) {
        if (*op == Xmm::xmm0) *op = copy_;
    }
}

void FixedXmm0::restore() {
    if (need_save_) ctx_.as().movaps(Xmm::xmm0, copy_);
}

bool decode_bit_mask(unsigned n, unsigned imms, unsigned immr, bool is64, std::uint64_t& value) {
    if (!is64 && n) return false;
    const unsigned combined = (n << 6) | (~imms & 0x3fu);
    if (combined == 0) return false;
    unsigned len = 31 - static_cast<unsigned>(__lzcnt(combined));
    if (len < 1) return false;
    const unsigned esize = 1u << len;
    const unsigned levels = esize - 1;
    const unsigned s = imms & levels, r = immr & levels;
    if (s == levels) return false;
    const std::uint64_t emask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
    const std::uint64_t welem = (s + 1 == 64) ? ~0ULL : ((1ULL << (s + 1)) - 1);
    std::uint64_t elem = r == 0 ? welem : (((welem >> r) | (welem << (esize - r))) & emask);
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 64; i += esize) result |= elem << i;
    value = is64 ? result : (result & 0xffffffffULL);
    return true;
}

unsigned emit_sysop(EmitContext& ctx, std::span<const std::uint32_t> words) {
    const auto w = words[0];
    if ((w & 0xfffff01fu) == 0xd503201fu) return emit_hint(ctx, w);
    if ((w & 0xfffff01fu) == 0xd503301fu) return emit_barrier(ctx, words);
    if ((w & 0xfff8f01fu) == 0xd500401fu) return emit_pstate(ctx, w);
    if ((w & 0xfff80000u) == 0xd5080000u) return emit_sys(ctx, w);
    if ((w & 0xfff00000u) == 0xd5300000u) return emit_mrs(ctx, w);
    if ((w & 0xfff00000u) == 0xd5100000u) return emit_msr(ctx, w);
    return 0;
}

bool describe_sysop(std::uint32_t w, InstructionUse& use) {
    use = {};
    const unsigned rt = rd_of(w);
    const std::uint32_t rbit = rt == 31 ? 0u : 1u << rt;
    if ((w & 0xfffff01fu) == 0xd503201fu) return true;
    if ((w & 0xfffff01fu) == 0xd503301fu) {
        const unsigned op2 = (w >> 5) & 7u;
        return op2 == 2 || op2 >= 4 || (op2 == 1 && ((w >> 8) & 3u) == 2u);
    }
    if ((w & 0xfff8f01fu) == 0xd500401fu) {
        if (((w >> 16) & 7u) != 0 || ((w >> 8) & 15u) != 0 || ((w >> 5) & 7u) > 2) return false;
        use.reads_flags = use.writes_flags = true;
        return true;
    }
    switch (w & 0xffffffe0u) {
    case 0xd50b7420u: use.gpr_read = rbit; use.may_fault = true; return true;
    case 0xd50b7a20u: case 0xd50b7b20u: case 0xd50b7c20u: case 0xd50b7d20u: case 0xd50b7e20u: return true;
    case 0xd50b7520u: use.gpr_read = rbit; return true;
    case 0xd53bd040u: case 0xd53bd060u: case 0xd53b00e0u: case 0xd53b0020u: case 0xd53be000u:
    case 0xd53be020u: case 0xd53be040u: case 0xd53be0a0u: case 0xd53be0c0u: case 0xd53b9d00u:
        use.gpr_write = rbit;
        return true;
    case 0xd53b4200u: use.gpr_write = rbit; use.reads_flags = true; return true;
    case 0xd53b2400u: case 0xd53b2420u:
        if (!host().rdrand) return false;
        use.gpr_write = rbit;
        use.writes_flags = true;
        return true;
    case 0xd51bd040u: use.gpr_read = rbit; return true;
    case 0xd51b4200u: use.gpr_read = rbit; use.writes_flags = true; return true;
    default: break;
    }
    return w == 0xd508711fu || w == 0xd508751fu;
}

} // namespace system_detail

// ---- family entry points ---------------------------------------------------------

unsigned emit_system(EmitContext& ctx, std::span<const std::uint32_t> words) {
    if (words.empty()) return 0;
    const auto w = words[0];
    if ((w & 0x3f000000u) == 0x08000000u || (w & 0x3f200c00u) == 0x38200000u ||
        (w & 0x3f200c00u) == 0x19000000u) {
        return system_detail::emit_atomic(ctx, words);
    }
    if ((w & 0xffc00000u) == 0xd5000000u) return system_detail::emit_sysop(ctx, words);
    return system_detail::emit_crypto(ctx, words);
}

bool describe_system(std::uint32_t w, InstructionUse& use) {
    if ((w & 0x3f000000u) == 0x08000000u || (w & 0x3f200c00u) == 0x38200000u ||
        (w & 0x3f200c00u) == 0x19000000u) {
        return system_detail::describe_atomic(w, use);
    }
    if ((w & 0xffc00000u) == 0xd5000000u) return system_detail::describe_sysop(w, use);
    return system_detail::describe_crypto(w, use);
}

const FamilyEmitter& system_family() {
    // Code-generation knobs are part of the name, so translator_family_fingerprint()
    // (and the persistent cache key) changes with them.
    static const std::string name = [] {
        std::string n = "system";
        if (system_detail::legacy_ordering()) n += "+oldorder";
        if (!system_detail::extended_fusion()) n += "+oldfusion";
        return n;
    }();
    static const FamilyEmitter family{name.c_str(), &emit_system, &describe_system};
    return family;
}

void system_set_fusion(bool enabled) { system_detail::g_fusion.store(enabled); }
void system_set_legacy_ordering(bool enabled) { system_detail::g_legacy_ordering.store(enabled); }
void system_set_extended_fusion(bool enabled) { system_detail::g_extended_fusion.store(enabled); }
void system_set_host_extensions(bool enabled) { system_detail::g_extensions.store(enabled); }
void system_set_inline_counter(bool enabled) { system_detail::g_inline_counter.store(enabled); }

} // namespace transmute::fx
