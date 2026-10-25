// Family dispatch through RegionEmitContext with toy families (owner:
// translator core). The toy families are only active inside these tests.
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/emit_context.hpp"
#include "fx/translator/region_emitter.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::coretest;
using x64::Gp;

namespace {

bool g_enabled = false;
bool g_greedy = false;   // add2d requests more scratch registers than exist
bool g_liar = false;     // mrs writes x19 although describe() does not say so
std::atomic<unsigned> g_helper_calls{0};

// ADD Vd.2D, Vn.2D, Vm.2D
bool is_add2d(std::uint32_t w) { return (w & 0xffe0fc00u) == 0x4ee08400u; }
// MRS Xt, NZCV / MSR NZCV, Xt
bool is_mrs(std::uint32_t w) { return (w & 0xffffffe0u) == 0xd53b4200u; }
bool is_msr(std::uint32_t w) { return (w & 0xffffffe0u) == 0xd51b4200u; }
// LDAXR Wt, [Xn] / STLXR Ws, Wt, [Xn]
bool is_ldaxr_w(std::uint32_t w) { return (w & 0xfffffc00u) == 0x885ffc00u; }
bool is_stlxr_w(std::uint32_t w) { return (w & 0xffe0fc00u) == 0x8800fc00u; }
// FCMP/FCMPE Sn|Dn, Sm|Dm|#0.0
bool is_fcmp(std::uint32_t w) { return (w & 0xff20fc07u) == 0x1e202000u && ((w >> 22) & 3) < 2; }
bool g_fcmp_memory = false;   // second (u)comis operand through vsrc() (memory when unpinned)

constexpr std::uint32_t kHintNop = 0xd503201fu;
constexpr std::uint32_t kHintYield = 0xd503203fu;   // used as "call a state helper"
constexpr unsigned kTestHelper = helper_misc_first + 7;
constexpr unsigned kStateHelper = helper_misc_first + 8;

void test_helper(GuestContext*, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    g_helper_calls += 1;
    (void)a; (void)b; (void)c;
}
// Reads and writes guest state: x0 += x1, NZCV = Z.
void state_helper(GuestContext* ctx) {
    ctx->x[0] += ctx->x[1];
    ctx->nzcv = 0x40000000u;
}

unsigned toy_emit(EmitContext& ctx, std::span<const std::uint32_t> words) {
    if (!g_enabled) return 0;
    auto& a = ctx.as();
    const auto w = words[0];
    if (is_add2d(w)) {
        if (g_greedy)
            for (int i = 0; i < 12; ++i) ctx.temp_gpr();
        const auto n = ctx.read_vec((w >> 5) & 31);
        const auto m = ctx.read_vec((w >> 16) & 31);
        const auto t = ctx.temp_vec();
        a.movaps(t, n);
        a.sse(x64::sse::paddq, t, m);
        const auto d = ctx.write_vec(w & 31);
        a.movaps(d, t);
        return 1;
    }
    if (is_fcmp(w)) {   // through the FCMP fusion protocol
        const bool dbl = ((w >> 22) & 1) != 0, zero = ((w >> 3) & 1) != 0, signal = ((w >> 4) & 1) != 0;
        const unsigned rn = (w >> 5) & 31, rm = (w >> 16) & 31;
        const bool swap = ctx.fcmp_swap();
        // a = first (u)comis operand, b = second (Rn, Rm or swapped; #0.0 in a zeroed temp).
        const unsigned ra = swap ? rm : rn, rb = swap ? rn : rm;
        const bool a_zero = swap && zero, b_zero = !swap && zero;
        x64::Xmm xa{}, xb{};
        x64::Mem mb{};
        bool b_mem = false;
        if (a_zero) { xa = ctx.temp_vec(); a.pxor(xa, xa); }
        else xa = ctx.read_vec(ra);
        if (b_zero) { xb = ctx.temp_vec(); a.pxor(xb, xb); }
        else if (g_fcmp_memory) {
            const auto src = ctx.vsrc(rb);
            b_mem = src.is_mem;
            xb = src.reg;
            mb = src.mem;
        } else {
            xb = ctx.read_vec(rb);
        }
        const x64::SseOp op = signal ? (dbl ? x64::sse::comisd : x64::sse::comiss) : (dbl ? x64::sse::ucomisd : x64::sse::ucomiss);
        if (b_mem) a.sse(op, xa, mb);
        else a.sse(op, xa, xb);
        if (ctx.set_fcmp_flags(swap)) return 1;
        // Fallback: GT (1,0) EQ (0,0) LT (0,1) UN (0x80000000,1) compared as "sub".
        const auto x = ctx.temp_gpr(), y = ctx.temp_gpr(), c = ctx.temp_gpr();
        a.mov_imm(x, 0);
        a.mov_imm(y, 0);
        a.setcc(swap ? x64::Cond::b : x64::Cond::a, x);
        a.setcc(swap ? x64::Cond::a : x64::Cond::b, y);
        a.mov_imm(c, 0x80000000u);
        a.cmov(x64::Cond::p, 32, x, c);
        a.mov_imm(c, 1);
        a.cmov(x64::Cond::p, 32, y, c);
        a.cmp(32, x, y);
        ctx.set_flags(FlagsProducer::sub);
        return 1;
    }
    if (is_mrs(w)) {
        const auto v = ctx.get_nzcv();
        const auto d = ctx.write_gpr(w & 31);
        a.mov(64, d, v);
        if (g_liar) {
            const auto x = ctx.write_gpr(19);
            a.mov_imm(x, 0x1919);
        }
        return 1;
    }
    if (is_msr(w)) {
        const auto v = ctx.copy_gpr(w & 31);
        ctx.clobber_host_flags();
        a.and_(32, v, static_cast<std::int32_t>(0xf0000000u));
        ctx.set_nzcv(v);
        return 1;
    }
    if (w == kHintNop) {
        ctx.call_helper(kTestHelper, {HelperArg::imm(1), HelperArg::guest(3), HelperArg::host(ctx.read_gpr(4))},
                        HelperEffects::pure);
        return 1;
    }
    if (w == kHintYield) {
        ctx.call_helper(kStateHelper, {}, HelperEffects::reads_writes_state);
        return 1;
    }
    // ldaxr wT, [xN]; add wT, wT, #imm; stlxr wS, wT, [xN]; cbnz wS, words[0]
    if (is_ldaxr_w(w) && words.size() >= 4) {
        const auto rt = w & 31, rn = (w >> 5) & 31;
        const auto add = words[1], st = words[2], cb = words[3];
        const bool add_ok = (add & 0xffc00000u) == 0x11000000u && (add & 31) == rt && ((add >> 5) & 31) == rt;
        const auto rs = (st >> 16) & 31;
        const bool st_ok = is_stlxr_w(st) && (st & 31) == rt && ((st >> 5) & 31) == rn && rs != rt && rs != rn;
        const bool cb_ok = (cb & 0xff00001fu) == (0x35000000u | rs) && ((cb >> 5) & 0x7ffff) == ((0u - 3u) & 0x7ffff);
        if (!add_ok || !st_ok || !cb_ok) return 0;
        const auto imm = static_cast<std::int32_t>((add >> 10) & 0xfff);
        const auto base = ctx.read_gpr(rn, true);
        const auto v = ctx.temp_gpr();
        a.mov_imm(v, static_cast<std::uint64_t>(imm));
        ctx.clobber_host_flags();
        ctx.mark_may_fault();
        a.lock();
        a.xadd(32, x64::ptr(base), v);
        const auto dt = ctx.write_gpr(rt);
        a.lea(32, dt, x64::ptr(v, imm));
        const auto ds = ctx.write_gpr(rs);
        a.mov_imm(ds, 0);
        return 4;
    }
    return 0;
}

bool toy_describe(std::uint32_t w, InstructionUse& use) {
    if (!g_enabled) return false;
    if (is_add2d(w)) {
        use.vec_read = (1u << ((w >> 5) & 31)) | (1u << ((w >> 16) & 31));
        use.vec_write = 1u << (w & 31);
        return true;
    }
    if (is_fcmp(w)) {
        use.vec_read = (1u << ((w >> 5) & 31)) | (((w >> 3) & 1) ? 0u : 1u << ((w >> 16) & 31));
        use.writes_flags = true;
        return true;
    }
    if (is_mrs(w)) { use.reads_flags = true; use.gpr_write = (w & 31) == 31 ? 0 : 1u << (w & 31); return true; }
    if (is_msr(w)) { use.writes_flags = true; use.gpr_read = (w & 31) == 31 ? 0 : 1u << (w & 31); return true; }
    if (w == kHintNop) { use.gpr_read = (1u << 3) | (1u << 4); return true; }
    if (w == kHintYield) { use.gpr_read = 3; use.gpr_write = 1; use.reads_flags = true; use.writes_flags = true; return true; }
    if (is_ldaxr_w(w)) { use.gpr_read = 1u << ((w >> 5) & 31); use.gpr_write = 1u << (w & 31); use.may_fault = true; return true; }
    if (is_stlxr_w(w)) {
        use.gpr_read = (1u << ((w >> 5) & 31)) | (1u << (w & 31));
        use.gpr_write = 1u << ((w >> 16) & 31);
        use.may_fault = true;
        return true;
    }
    return false;
}

// A family without describe(): planned as an interpret exit, then accepted.
unsigned nodescribe_emit(EmitContext& ctx, std::span<const std::uint32_t> words) {
    if (!g_enabled) return 0;
    // ADD Vd.2D handled above; this one handles "hint #0x31" (no-op write of x0 = 7).
    if (words[0] != 0xd503263fu) return 0;
    const auto d = ctx.write_gpr(0);
    ctx.as().mov_imm(d, 7);
    return 1;
}

void register_toys() {
    static bool done = false;
    if (done) return;
    done = true;
    register_family_emitter({"core-toy", &toy_emit, &toy_describe});
    register_family_emitter({"core-toy", &toy_emit, &toy_describe});   // idempotent
    register_family_emitter({"core-toy-nodescribe", &nodescribe_emit, nullptr});
}

struct Enable {
    Enable() { register_toys(); g_enabled = true; }
    ~Enable() { g_enabled = false; }
};

DiffRunner& runner() {
    static DiffRunner r;
    static bool init = false;
    if (!init) {
        init = true;
        r.machine.set_helper(kTestHelper, reinterpret_cast<const void*>(&test_helper));
        r.machine.set_helper(kStateHelper, reinterpret_cast<const void*>(&state_helper));
    }
    return r;
}

} // namespace

FX_TEST(core_family_vector_and_nzcv) {
    Enable on;
    auto& r = runner();
    using namespace a64;
    Rng rng(5);
    InstGen g(rng);
    for (unsigned k = 0; k < 300; ++k) {
        std::vector<std::uint32_t> prog;
        for (unsigned i = 0; i < 12; ++i) {
            switch (rng.below(6)) {
            case 0: prog.push_back(neon_add2d(rng.below(32), rng.below(32), rng.below(32))); break;
            case 1: prog.push_back(mrs_nzcv(rng.pick(kDest))); break;
            case 2: prog.push_back(msr_nzcv(rng.pick(kSrc))); break;
            case 3: prog.push_back(add_shift(true, true, true, 0, rng.pick(kSrc), 0, rng.pick(kSrc), 31)); break;
            default: prog.push_back(g.data_or_memory()); break;
            }
        }
        prog.push_back(csel(true, false, 2, rng.below(14), 0, 1, 3));
        r.run(prog, r.context(k), "family " + std::to_string(k));
        FX_CHECK_EQ(r.last.interprets, 0u);
    }
}

FX_TEST(core_family_cached_destinations_preserve_state) {
    Enable on;
    auto& r = runner();
    std::vector<std::uint32_t> words;
    // More live vectors than cache registers: each in-place ADD should reuse
    // its input register rather than evict another live value for its result.
    for (unsigned pass = 0; pass < 4; ++pass)
        for (unsigned v = 0; v < 16; ++v)
            words.push_back(a64::neon_add2d(v, v, 31));
    words.push_back(a64::brk(0xdead));
    auto* program = reinterpret_cast<std::uint32_t*>(r.program_base());
    std::memcpy(program, words.data(), words.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + words.size() * 4);
    const auto initial = r.context(19834);
    GuestContext results[2];
    std::size_t sizes[2]{};
    for (unsigned mode = 0; mode < 2; ++mode) {
        core::Plan plan;
        core::PlanOverrides overrides;
        overrides.vector_pin_limit = 0;
        overrides.no_reuse_dest = mode == 0;
        core::build_plan(r.program_base(), code, {}, overrides, plan);
        TranslatedRegion region;
        core::RegionEmitter(plan, {}, region).run();
        sizes[mode] = region.code.size();
        void* host = VirtualAlloc(nullptr, region.code.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        FX_REQUIRE(host);
        std::memcpy(host, region.code.data(), region.code.size());
        FlushInstructionCache(GetCurrentProcess(), host, region.code.size());
        results[mode] = initial;
        r.machine.prepare(results[mode]);
        r.machine.enter(results[mode], static_cast<std::uint8_t*>(host) + region.entries[0].offset);
        VirtualFree(host, 0, MEM_RELEASE);
        for (unsigned v = 0; v < 16; ++v)
            for (unsigned lane = 0; lane < 2; ++lane)
                FX_CHECK_EQ(results[mode].v[v][lane], initial.v[v][lane] + 4 * initial.v[31][lane]);
    }
    std::printf("    Cached vector destinations: %zu -> %zu host bytes\n", sizes[0], sizes[1]);
    testing::compare_contexts(results[0], results[1], "cached destinations");
}

FX_TEST(core_family_helpers) {
    Enable on;
    auto& r = runner();
    using namespace a64;
    // The pure helper is invisible to the guest; the state helper does x0 += x1, NZCV = Z.
    for (unsigned k = 0; k < 50; ++k) {
        const std::vector<std::uint32_t> prog = {
            add_imm(true, false, false, 0, 0, 3),            // add x0, x0, #3   (x0 pinned + dirty)
            add_imm(true, false, false, 1, 1, 1),            // add x1, x1, #1
            add_shift(true, true, true, 0, 2, 0, 1, 31),     // cmp x1, x2 (flags live across the pure helper)
            kHintNop,                                        // pure helper
            csel(true, false, 6, 1, 0, 5, 7),                // csel x7, x5, x6, ne
            kHintYield,                                      // state helper: x0 += x1, nzcv = Z
            csel(true, false, 6, 0, 0, 5, 8),                // csel x8, x5, x6, eq  (must see Z)
            add_imm(true, false, false, 0, 0, 1),            // add x0, x0, #1 (pin reloaded)
        };
        auto ctx = r.context(500 + k);
        const unsigned before = g_helper_calls;
        // Expected state computed by hand (the oracle treats YIELD as a hint).
        auto expected = ctx;
        expected.x[0] = ctx.x[0] + 3;
        expected.x[1] = ctx.x[1] + 1;
        expected.x[7] = expected.x[1] != ctx.x[2] ? ctx.x[5] : ctx.x[6];
        expected.x[0] += expected.x[1];
        expected.x[8] = ctx.x[5];
        expected.x[0] += 1;
        FX_CHECK(r.run_translated(prog, ctx));
        FX_CHECK_EQ(g_helper_calls - before, 1u);
        FX_CHECK_EQ(r.actual.x[0], expected.x[0]);
        FX_CHECK_EQ(r.actual.x[1], expected.x[1]);
        FX_CHECK_EQ(r.actual.x[7], expected.x[7]);
        FX_CHECK_EQ(r.actual.x[8], expected.x[8]);
        FX_CHECK_EQ(r.actual.nzcv & 0xf0000000u, 0x40000000u);
    }
}

FX_TEST(core_family_multiword_fusion) {
    Enable on;
    auto& r = runner();
    using namespace a64;
    for (unsigned k = 0; k < 60; ++k) {
        // x16 points at data; increment *(u32*)x16 by #5 via an LL/SC loop, then use the results.
        const std::vector<std::uint32_t> prog = {
            add_imm(true, false, false, 3, 3, 1),            // add x3, x3, #1
            0x885ffc00u | (16u << 5) | 8u,                   // L: ldaxr w8, [x16]
            add_imm(false, false, false, 8, 8, 5),           // add w8, w8, #5
            0x8800fc00u | (9u << 16) | (16u << 5) | 8u,      // stlxr w9, w8, [x16]
            cbz(false, true, -3, 9),                         // cbnz w9, L
            add_shift(true, false, false, 0, 8, 0, 3, 4),    // add x4, x3, x8
        };
        auto ctx = r.context(700 + k);
        r.run(prog, ctx, "llsc " + std::to_string(k));
        FX_CHECK_EQ(r.last.interprets, 0u);
    }
}

FX_TEST(core_family_without_describe) {
    Enable on;
    auto& r = runner();
    using namespace a64;
    for (unsigned k = 0; k < 40; ++k) {
        const std::vector<std::uint32_t> prog = {
            add_imm(true, false, false, 19, 19, 1),          // add x19, x19, #1 (pinned, dirty)
            add_imm(true, false, false, 19, 19, 1),
            add_shift(true, true, true, 0, 2, 0, 1, 31),     // cmp x1, x2
            0xd503263fu,                                     // hint #0x31 -> x0 = 7 (no describe)
            csel(true, false, 19, 0, 0, 0, 5),               // csel x5, x0, x19, eq
            add_imm(true, false, false, 19, 19, 1),
        };
        auto ctx = r.context(800 + k);
        FX_CHECK(r.run_translated(prog, ctx));
        const bool eq = ctx.x[1] == ctx.x[2];
        FX_CHECK_EQ(r.actual.x[0], 7u);
        FX_CHECK_EQ(r.actual.x[19], ctx.x[19] + 3);
        FX_CHECK_EQ(r.actual.x[5], eq ? 7u : ctx.x[19] + 2);
    }
}

FX_TEST(core_family_retry_paths) {
    Enable on;
    auto& r = runner();
    using namespace a64;
    // Scratch exhaustion: the instruction is retranslated as an interpret exit.
    g_greedy = true;
    for (unsigned k = 0; k < 20; ++k) {
        const std::vector<std::uint32_t> prog = {
            add_imm(true, false, false, 19, 19, 1),
            neon_add2d(1, 2, 3),
            add_imm(true, false, false, 19, 19, 1),
            add_shift(true, true, true, 0, 2, 0, 1, 31),
            csel(true, false, 19, 0, 0, 4, 5),
        };
        r.run(prog, r.context(900 + k), "greedy " + std::to_string(k));
        FX_CHECK(r.last.interprets >= 1);
    }
    g_greedy = false;
    // A family writing a pinned register its describe() omits: retranslated
    // with conservative usage, so the write is still flushed.
    g_liar = true;
    for (unsigned k = 0; k < 20; ++k) {
        const std::vector<std::uint32_t> prog = {
            add_imm(true, false, false, 19, 19, 1),          // x19 hot -> pinned
            add_imm(true, false, false, 19, 19, 1),
            mrs_nzcv(3),                                     // also writes x19 = 0x1919
            add_imm(true, false, false, 20, 19, 0),          // 3 mov x20, x19
            bl(3),                                           // 4 bl 7 (exit flushes dirty pins)
            add_imm(true, false, false, 19, 19, 1),          // 5
            b(3),                                            // 6 b end (9)
            add_imm(true, false, false, 21, 19, 0),          // 7 callee: mov x21, x19
            ret(),                                           // 8
        };
        const auto ctx = r.context(950 + k);
        FX_CHECK(r.run_translated(prog, ctx));
        FX_CHECK_EQ(r.actual.x[20], 0x1919u);
        FX_CHECK_EQ(r.actual.x[21], 0x1919u);
        FX_CHECK_EQ(r.actual.x[19], 0x191au);
    }
    g_liar = false;
}

// FCMP through the fusion protocol: every consumer form and condition,
// unordered/equal/signed-zero operands, flags live beyond the first consumer,
// memory accesses (fault sites) between compare and consumer, and the NZCV
// value at the exit, against the oracle.
FX_TEST(core_family_fcmp_fusion) {
    Enable on;
    auto& r = runner();
    using namespace a64;
    Rng rng(77);
    InstGen g(rng);
    static constexpr std::uint32_t kF[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x40000000u,
                                           0x7fc00000u, 0x7f800000u, 0xff800000u, 0x3fc00000u, 0xffc00001u};
    static constexpr std::uint64_t kD[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
                                           0x4000000000000000ull, 0x7ff8000000000000ull, 0x7ff0000000000000ull,
                                           0xfff0000000000000ull, 0x3ff8000000000000ull, 0xfff8000000000001ull};
    const auto consumer = [&](std::vector<std::uint32_t>& prog) {
        const unsigned cond = rng.below(14);
        switch (rng.below(6)) {
        case 0:   // b.cond over one instruction
            prog.push_back(bcond(cond, 2));
            prog.push_back(add_imm(true, false, false, rng.pick(kDest), rng.pick(kSrc), 1 + rng.below(9)));
            break;
        case 1: prog.push_back(csel(rng.chance(50), rng.chance(30), rng.pick(kSrc), cond, rng.below(2), rng.pick(kSrc), rng.pick(kDest))); break;
        case 2: prog.push_back(csel(true, false, 31, cond, 1, 31, rng.pick(kDest))); break;                       // cset
        case 3: prog.push_back(csel(rng.chance(50), true, 31, cond, 0, 31, rng.pick(kDest))); break;              // csetm
        case 4: {   // csel xd, xn, xd (dd aliases the else-value) / xd, xd, xm
            const unsigned d = rng.pick(kDest);
            prog.push_back(rng.chance(50) ? csel(true, false, d, cond, 0, rng.pick(kSrc), d)
                                          : csel(true, false, rng.pick(kSrc), cond, rng.below(2), d, d));
            break;
        }
        default: prog.push_back(ccmp(true, true, true, rng.below(32), cond, rng.pick(kSrc), rng.below(16))); break;
        }
    };
    int failures = 0;
    for (unsigned k = 0; k < 3000 && failures < 6; ++k) {
        g_fcmp_memory = (k & 1) != 0;
        std::vector<std::uint32_t> prog;
        const unsigned parts = 1 + rng.below(3);
        for (unsigned p = 0; p < parts; ++p) {
            const bool dbl = rng.chance(40), zero = rng.chance(20), signal = rng.chance(20);
            const unsigned rn = rng.below(8), rm = zero ? 0 : rng.below(8);
            prog.push_back(0x1e202000u | (dbl ? 1u << 22 : 0u) | (rm << 16) | (rn << 5) | (zero ? 8u : 0u) | (signal ? 16u : 0u));
            if (rng.chance(25)) prog.push_back(g.load_store());   // fault site while the compare flags are live
            consumer(prog);
            if (rng.chance(40)) consumer(prog);                   // flags live beyond the first consumer
            if (rng.chance(30)) prog.push_back(g.data_or_memory());
        }
        auto ctx = r.context(3000 + k);
        ctx.fpcr = 0;
        ctx.fpsr = 0;
        for (unsigned v = 0; v < 8; ++v) {
            if (rng.chance(50)) { ctx.v[v][0] = kF[rng.below(10)]; ctx.v[v][1] = rng.next(); }
            else { ctx.v[v][0] = kD[rng.below(10)]; ctx.v[v][1] = rng.next(); }
        }
        if (!r.run(prog, ctx, "fcmp " + std::to_string(k))) ++failures;
        FX_CHECK_EQ(r.last.interprets, 0u);
    }
    g_fcmp_memory = false;
}

// RBIT feeding CLZ (count trailing zeros) is emitted as TZCNT when the RBIT
// value is dead afterwards; reads of it, writes of the source, memory
// accesses and branches in between must keep the plain lowering.
FX_TEST(core_rbit_clz_fusion) {
    auto& r = runner();
    using namespace a64;
    Rng rng(91);
    InstGen g(rng);
    int failures = 0;
    for (unsigned k = 0; k < 2000 && failures < 6; ++k) {
        std::vector<std::uint32_t> prog;
        const bool sf = rng.chance(70);
        const unsigned xd = rng.pick(kDest), xn = rng.chance(10) ? 31u : rng.pick(kSrc);
        if (rng.chance(30)) prog.push_back(g.data_or_memory());
        prog.push_back(dp1(sf, 0, xn, xd));                                  // rbit xd, xn
        for (unsigned i = 0, m = rng.below(4); i < m; ++i) {
            switch (rng.below(5)) {
            case 0: prog.push_back(g.load_store()); break;                   // blocks the fusion
            case 1: prog.push_back(add_imm(true, false, false, rng.pick(kDest), xd, 1)); break;   // reads xd
            default: prog.push_back(g.add_sub_imm()); break;
            }
        }
        const unsigned xm = rng.chance(40) ? xd : rng.pick(kDest);
        prog.push_back(dp1(sf, 4, xd, xm));                                  // clz xm, xd
        if (rng.chance(50)) prog.push_back(g.add_sub_imm());
        if (rng.chance(60)) prog.push_back(movw(true, 2, 0, rng.below(65536), xd));   // xd dead
        if (rng.chance(30)) prog.push_back(add_shift(true, false, false, 0, xd, 0, xd, rng.pick(kDest)));
        if (!r.run(prog, r.context(7000 + k), "rbit/clz " + std::to_string(k))) ++failures;
    }
}

// Two RBIT/CLZ pairs interleaved, as dav1d's ctz(width) + ctz(height) in its
// CfL AC functions (rbit w9, w5; rbit w10, w6; clz w9, w9; clz w10, w10): the
// second RBIT must not take over the first one's pending fusion (the first
// CLZ then counted the leading zeros of a register its RBIT never wrote).
FX_TEST(core_rbit_clz_interleaved) {
    auto& r = runner();
    using namespace a64;
    Rng rng(92);
    int failures = 0;
    for (unsigned k = 0; k < 1000 && failures < 6; ++k) {
        const bool sf = rng.chance(50);
        const unsigned xa = rng.pick(kDest), xb = rng.pick(kDest), na = rng.pick(kSrc), nb = rng.pick(kSrc);
        std::vector<std::uint32_t> prog{dp1(sf, 0, na, xa), dp1(sf, 0, nb, xb), dp1(sf, 4, xa, xa), dp1(sf, 4, xb, xb)};
        if (rng.chance(50)) std::swap(prog[2], prog[3]);
        prog.push_back(add_shift(true, false, false, 0, xb, 0, xa, rng.pick(kDest)));
        if (!r.run(prog, r.context(7500 + k), "rbit/clz interleaved " + std::to_string(k))) ++failures;
    }
}

// Indirect branches look the target up inline (ctx->arena_base set by the
// test machine): after the first miss per target, BLR/BR/RET-misses reach the
// target region without a dispatcher round trip.
FX_TEST(core_inline_indirect_lookup) {
    auto& r = runner();
    using namespace a64;
    // 0: adr x9, f        1: adr x10, g        2: movz x12, #200
    // 3: L: blr x9        4: br x10 (g: skips 5) 5: brk #1   6: g2: subs x12, x12, #1
    // 7: b.ne L           8: b end (11)        9: f: add x0, x0, #1   10: ret   11: end
    const std::vector<std::uint32_t> prog = {
        adr(false, 9 * 4, 9),                         // 0
        adr(false, 6 * 4 - 4, 10),                    // 1: x10 = &insn 6
        movw(true, 2, 0, 200, 12),                    // 2
        blr(9),                                       // 3
        br(10),                                       // 4
        brk(1),                                       // 5
        add_imm(true, true, true, 12, 12, 1),         // 6
        bcond(1, -4),                                 // 7: b.ne 3
        b(3),                                         // 8: b 11
        add_imm(true, false, false, 0, 0, 1),         // 9
        ret(),                                        // 10
        nop(),                                        // 11
    };
    for (const bool inline_lookup : {true, false}) {
        r.machine.inline_lookup = inline_lookup;
        auto ctx = r.context(4242);
        FX_CHECK(r.run(prog, ctx, std::string("inline lookup ") + (inline_lookup ? "on" : "off")));
        if (inline_lookup) FX_CHECK(r.last.lookups <= 4u);
        else FX_CHECK(r.last.lookups >= 200u);
    }
    r.machine.inline_lookup = true;
}

// Inline lookups store ctx->pc only on their miss path. A target below 4 MiB
// has tag 0, so an empty slot (0) "hits" and enters the miss stub at arena
// offset 0, which stores the target itself; misaligned targets take the miss
// path. Either way the exit names the target.
FX_TEST(core_inline_lookup_miss_pc) {
    auto& r = runner();
    using namespace a64;
    for (const std::uint64_t target : {0x1000ull, 0x3ffffcull, 0x1002ull, 0x7fff00001234ull, 0x7fff00001233ull}) {
        for (const bool inline_lookup : {true, false}) {
            r.machine.inline_lookup = inline_lookup;
            r.machine.reset();
            // movz x9, #lo; movk x9, #hi, lsl #16; movk x9, #top, lsl #32; br x9
            auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
            program[0] = movw(true, 2, 0, static_cast<unsigned>(target & 0xffff), 9);
            program[1] = movw(true, 3, 1, static_cast<unsigned>((target >> 16) & 0xffff), 9);
            program[2] = movw(true, 3, 2, static_cast<unsigned>((target >> 32) & 0xffff), 9);
            program[3] = br(9);
            const BufferCode code(r.code_base(), r.program_base() + 16);
            auto ctx = r.context(77);
            ctx.pc = r.program_base();
            const auto res = r.machine.run(ctx, code, kReturnToHostPc);
            FX_CHECK(!res.ok && res.error == "unreadable guest pc");
            FX_CHECK_EQ(res.lookups, 1u);
            FX_CHECK_EQ(ctx.pc, target);
        }
    }
    r.machine.inline_lookup = true;
}
