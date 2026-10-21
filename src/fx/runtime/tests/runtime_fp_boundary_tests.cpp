// FPSR/FPCR at host-code boundaries (docs/FX_ENGINE.md, FP; process.hpp,
// "FP state at host-code boundaries"). Hand-assembled regions raise FP
// flags with real x64 FP instructions (as translated FDIV / FCVTZS / FMUL
// do), reach host code through each boundary, and read FPSR/FPCR afterwards
// with the translator's MRS FPSR / MRS FPCR sequences. The host code must see
// the exact architectural FPSR in ctx.fpsr, and its changes to ctx.fpsr and
// ctx.fpcr must be what the guest sees next.

#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <immintrin.h>

#include <functional>

using namespace rtt;
using x64::Xmm;

namespace {

void init() { runtime_initialize(RuntimeOptions{}); }

constexpr std::uint32_t kIoc = 0x1, kDzc = 0x2, kOfc = 0x4, kIxc = 0x10, kIdc = 0x80;
constexpr std::uint32_t kCumulative = 0x9f;
constexpr std::uint32_t kFpcrRz = 3u << 22, kFpcrFz = 1u << 24;
constexpr std::int32_t d_fpsr = ctx_disp(offsetof(GuestContext, fpsr));
constexpr std::int32_t d_fpcr = ctx_disp(offsetof(GuestContext, fpcr));

// fdiv s0, 1.0, 0.0 (DZC); fcvtzs w0, NaN (IOC).
void emit_raise_dzc_ioc(x64::Assembler& a) {
    a.mov_imm(Gp::rax, 0x3f800000);
    a.movd(Xmm::xmm0, Gp::rax);
    a.sse(x64::sse::xorps, Xmm::xmm1, Xmm::xmm1);
    a.sse(x64::sse::divss, Xmm::xmm0, Xmm::xmm1);
    a.mov_imm(Gp::rax, 0x7fc00000);
    a.movd(Xmm::xmm0, Gp::rax);
    a.cvttss2si(Gp::rax, Xmm::xmm0, 32);
}

// mrs x<n>, fpsr exactly as emit_fp_arith.cpp lowers it (ctx.fpcr is the
// stmxcsr scratch slot): ctx.fpsr | flags(MXCSR).
void emit_mrs_fpsr(x64::Assembler& a, unsigned n) {
    a.mov(32, Gp::rcx, ptr(Gp::r15, d_fpcr));
    a.stmxcsr(ptr(Gp::r15, d_fpcr));
    a.mov(32, Gp::rax, ptr(Gp::r15, d_fpcr));
    a.mov(32, ptr(Gp::r15, d_fpcr), Gp::rcx);
    a.mov(32, Gp::rdx, Gp::rax);
    a.shr(32, Gp::rdx, 1);
    a.and_(32, Gp::rdx, 0x1e);
    a.and_(32, Gp::rax, 1);
    a.or_(32, Gp::rax, Gp::rdx);
    a.alu(x64::Alu::or_, 32, Gp::rax, ptr(Gp::r15, d_fpsr));
    a.mov(64, ptr(Gp::r15, x_disp(n)), Gp::rax);
}

void emit_mrs_fpcr(x64::Assembler& a, unsigned n) {
    a.mov(32, Gp::rax, ptr(Gp::r15, d_fpcr));
    a.mov(64, ptr(Gp::r15, x_disp(n)), Gp::rax);
}

// FP results that depend on FPCR: x[n] = 1.0f / 3.0f (RN 0x3eaaaaab, RZ
// 0x3eaaaaaa), x[n+1] = FLT_MIN * 0.5f (0x00400000, or 0 with FZ).
void emit_fp_probe(x64::Assembler& a, unsigned n) {
    a.mov_imm(Gp::rax, 0x3f800000);
    a.movd(Xmm::xmm0, Gp::rax);
    a.mov_imm(Gp::rax, 0x40400000);
    a.movd(Xmm::xmm1, Gp::rax);
    a.sse(x64::sse::divss, Xmm::xmm0, Xmm::xmm1);
    a.movd(Gp::rax, Xmm::xmm0);
    a.mov(64, ptr(Gp::r15, x_disp(n)), Gp::rax);
    a.mov_imm(Gp::rax, 0x00800000);
    a.movd(Xmm::xmm0, Gp::rax);
    a.mov_imm(Gp::rax, 0x3f000000);
    a.movd(Xmm::xmm1, Gp::rax);
    a.sse(x64::sse::mulss, Xmm::xmm0, Xmm::xmm1);
    a.movd(Gp::rax, Xmm::xmm0);
    a.mov(64, ptr(Gp::r15, x_disp(n + 1)), Gp::rax);
}

// What the host code at the boundary saw, and what it changes.
struct Boundary {
    std::uint32_t clear = 0, set = 0;
    bool change_fpcr = false;
    std::uint32_t fpcr = 0;
    int calls = 0;
    std::uint32_t seen_fpsr = 0, seen_fpcr = 0;
    void apply(GuestContext& ctx) {
        ++calls;
        seen_fpsr = ctx.fpsr;
        seen_fpcr = ctx.fpcr;
        ctx.fpsr = (ctx.fpsr & ~clear) | set;
        if (change_fpcr) ctx.fpcr = fpcr;
    }
};
Boundary* g_boundary = nullptr;

// One region: x30 -> x19; raise DZC | IOC; `transfer` reaches the host code
// and comes back at `cont` (it emits the entry for `cont`); then
// x1 = FPSR, x2 = FPCR, x3/x4 = the FPCR probe; return to x30.
using Transfer = std::function<void(RegionBuilder&, std::uint64_t cont)>;
TranslatedRegion boundary_region(std::uint64_t pc, std::uint64_t cont, const Transfer& transfer) {
    RegionBuilder b(pc);
    b.entry(pc);
    auto& a = b.as();
    a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
    a.mov(64, ptr(Gp::r15, x_disp(19)), Gp::rax);
    emit_raise_dzc_ioc(a);
    transfer(b, cont);
    emit_mrs_fpsr(a, 1);
    emit_mrs_fpcr(a, 2);
    emit_fp_probe(a, 3);
    a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(19)));
    a.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rax);
    b.return_via_x30();
    b.cover(pc, pc + 16);
    return b.finish();
}

// Runs the region with the standard scenarios against `boundary` (which the
// boundary's host code applies).
void run_scenarios(std::uint64_t pc, Boundary& boundary) {
    GuestContext& ctx = ensure_thread_context();
    g_boundary = &boundary;
    const auto run = [&](std::uint32_t initial_fpsr) {
        ctx.fpsr = initial_fpsr;
        ctx.fpcr = 0;
        boundary.calls = 0;
        boundary.seen_fpsr = boundary.seen_fpcr = 0xdead;
        for (unsigned i = 1; i <= 4; ++i) ctx.x[i] = 0;
        call_guest_prepared(ctx, pc);
        FX_CHECK_EQ(boundary.calls, 1);
    };

    // Nothing changed: the guest reads back what it raised, RN without FZ.
    boundary = {};
    run(0);
    FX_CHECK_EQ(boundary.seen_fpsr & kCumulative, kDzc | kIoc);
    FX_CHECK_EQ(ctx.x[1] & kCumulative, std::uint64_t{kDzc | kIoc});
    FX_CHECK_EQ(ctx.x[3], 0x3eaaaaabu);
    FX_CHECK_EQ(ctx.x[4], 0x00400000u);

    // Flags from before the region (IXC moves into MXCSR at entry, IDC
    // stays in ctx.fpsr) and from the region; the host clears them all.
    boundary = {};
    boundary.clear = kCumulative;
    run(kIdc | kIxc);
    FX_CHECK_EQ(boundary.seen_fpsr & kCumulative, kIdc | kIxc | kDzc | kIoc);
    FX_CHECK_EQ(ctx.x[1] & kCumulative, 0u);

    // The host raises a flag (feraiseexcept): the guest sees it next to its own.
    boundary = {};
    boundary.set = kOfc;
    run(0);
    FX_CHECK_EQ(boundary.seen_fpsr & kCumulative, kDzc | kIoc);
    FX_CHECK_EQ(ctx.x[1] & kCumulative, std::uint64_t{kDzc | kIoc | kOfc});

    // The host clears one flag and changes FPCR (fesetround, FZ): the next
    // guest FP operations round toward zero and flush.
    boundary = {};
    boundary.clear = kDzc;
    boundary.change_fpcr = true;
    boundary.fpcr = kFpcrRz | kFpcrFz;
    run(0);
    FX_CHECK_EQ(boundary.seen_fpcr, 0u);
    FX_CHECK_EQ(ctx.x[1] & kCumulative, std::uint64_t{kIoc});
    FX_CHECK_EQ(ctx.x[2], std::uint64_t{kFpcrRz | kFpcrFz});
    FX_CHECK_EQ(ctx.x[3], 0x3eaaaaaau);
    FX_CHECK_EQ(ctx.x[4], 0u);

    ctx.fpsr = 0;
    ctx.fpcr = 0;
    g_boundary = nullptr;
}

void boundary_callback(GuestContext& ctx, void*) { g_boundary->apply(ctx); }

} // namespace

FX_TEST(fp_boundary_host_callback) {
    init();
    GuestCode code({});
    const auto pc = code.at(0), cont = code.at(2), callee = code.at(32);
    register_host_callback(callee, &boundary_callback, nullptr);
    set_region_factory(pc, [&](std::uint64_t) {
        return boundary_region(pc, cont, [&](RegionBuilder& b, std::uint64_t at) {
            const Label continuation = b.as().new_label();
            b.call(at, continuation, callee);   // BL callee
            b.as().bind(continuation);
            b.entry(at);
        });
    });
    Boundary boundary;
    run_scenarios(pc, boundary);
    run_scenarios(pc, boundary);   // linked: BL jumps straight into the trampoline
    unregister_host_entry(callee);
    set_region_factory(pc, nullptr);
}

// A guest exception raised by a host callback (dispatch_guest_exception from
// the callback; the stand-in runs the handler): the handler sees the
// architectural FPSR and its changes reach the guest.
FX_TEST(fp_boundary_exception_from_host_callback) {
    init();
    GuestCode code({});
    const auto pc = code.at(0), cont = code.at(2), callee = code.at(32);
    register_host_callback(callee, [](GuestContext& ctx, void*) {
        GuestExceptionRecord record{};
        record.code = 0xe0000042u;
        record.address = ctx.pc;
        const auto resume = ctx.pc;
        dispatch_guest_exception(ctx, record);
        ctx.pc = resume;
    }, nullptr);
    set_exception_handler([](GuestContext& ctx, const GuestExceptionRecord&) { g_boundary->apply(ctx); });
    set_region_factory(pc, [&](std::uint64_t) {
        return boundary_region(pc, cont, [&](RegionBuilder& b, std::uint64_t at) {
            const Label continuation = b.as().new_label();
            b.call(at, continuation, callee);
            b.as().bind(continuation);
            b.entry(at);
        });
    });
    Boundary boundary;
    run_scenarios(pc, boundary);
    set_exception_handler(nullptr);
    unregister_host_entry(callee);
    set_region_factory(pc, nullptr);
}

// A callback that leaves by exit_to_dispatcher (as the macOS personality's
// error path does): the flags it cleared stay cleared.
FX_TEST(fp_boundary_host_callback_exit_to_dispatcher) {
    init();
    GuestCode code({});
    const auto pc = code.at(0), cont = code.at(2), callee = code.at(32);
    register_host_callback(callee, [](GuestContext& ctx, void*) {
        g_boundary->apply(ctx);
        exit_to_dispatcher(ctx, ExitReason::redispatch, 0);   // ctx.pc = the return address
    }, nullptr);
    set_region_factory(pc, [&](std::uint64_t) {
        return boundary_region(pc, cont, [&](RegionBuilder& b, std::uint64_t at) {
            const Label continuation = b.as().new_label();
            b.call(at, continuation, callee);
            b.as().bind(continuation);
            b.entry(at);
        });
    });
    Boundary boundary;
    run_scenarios(pc, boundary);
    unregister_host_entry(callee);
    set_region_factory(pc, nullptr);
}

// A callback that calls guest code (call_guest_prepared) before changing
// FPSR: the nested guest code sees the caller's flags and its own flags reach
// the callback and the guest after it.
FX_TEST(fp_boundary_host_callback_calls_guest) {
    init();
    GuestCode code({});
    const auto pc = code.at(0), cont = code.at(2), callee = code.at(32), inner = code.at(48);
    static std::uint64_t s_inner = 0;
    s_inner = inner;
    // inner: x5 = FPSR on entry; fmul FLT_MAX, FLT_MAX (OFC IXC); ret.
    set_region_factory(inner, [&](std::uint64_t) {
        RegionBuilder b(inner);
        b.entry(inner);
        auto& a = b.as();
        emit_mrs_fpsr(a, 5);
        a.mov_imm(Gp::rax, 0x7f7fffff);
        a.movd(Xmm::xmm0, Gp::rax);
        a.sse(x64::sse::mulss, Xmm::xmm0, Xmm::xmm0);
        b.return_via_x30();
        return b.finish();
    });
    register_host_callback(callee, [](GuestContext& ctx, void*) {
        call_guest_prepared(ctx, s_inner);
        g_boundary->apply(ctx);
    }, nullptr);
    set_region_factory(pc, [&](std::uint64_t) {
        return boundary_region(pc, cont, [&](RegionBuilder& b, std::uint64_t at) {
            const Label continuation = b.as().new_label();
            b.call(at, continuation, callee);
            b.as().bind(continuation);
            b.entry(at);
        });
    });
    GuestContext& ctx = ensure_thread_context();
    Boundary boundary;
    g_boundary = &boundary;
    for (const std::uint32_t clear : {0u, kDzc, kCumulative}) {
        boundary = {};
        boundary.clear = clear;
        ctx.fpsr = 0;
        ctx.fpcr = 0;
        call_guest_prepared(ctx, pc);
        FX_CHECK_EQ(boundary.calls, 1);
        FX_CHECK_EQ(ctx.x[5] & kCumulative, std::uint64_t{kDzc | kIoc});
        FX_CHECK_EQ(boundary.seen_fpsr & kCumulative, kDzc | kIoc | kOfc | kIxc);
        FX_CHECK_EQ(ctx.x[1] & kCumulative, std::uint64_t{(kDzc | kIoc | kOfc | kIxc) & ~clear});
    }
    g_boundary = nullptr;
    ctx.fpsr = 0;
    unregister_host_entry(callee);
    set_region_factory(pc, nullptr);
    set_region_factory(inner, nullptr);
}

namespace {
void boundary_svc(GuestContext& ctx, std::uint32_t immediate) {
    FX_CHECK_EQ(immediate, 0x80u);
    g_boundary->apply(ctx);
}
} // namespace

FX_TEST(fp_boundary_svc) {
    init();
    GuestCode code({});
    const auto pc = code.at(0), cont = code.at(2);
    set_region_factory(pc, [&](std::uint64_t) {
        return boundary_region(pc, cont, [&](RegionBuilder& b, std::uint64_t at) {
            // svc #0x80: exit with ExitReason::svc, ctx.pc = the next instruction.
            b.set_pc(at);
            b.as().mov(32, ptr(Gp::r15, d_exit_reason), static_cast<std::int32_t>(ExitReason::svc));
            b.as().mov(64, ptr(Gp::r15, ctx_disp(offsetof(GuestContext, exit_arg))), 0x80);
            b.as().jmp(ptr(Gp::r15, d_stub_exit));
            b.entry(at);
        });
    });
    set_svc_handler(&boundary_svc);
    Boundary boundary;
    run_scenarios(pc, boundary);
    set_svc_handler(nullptr);
    set_region_factory(pc, nullptr);
}

namespace {
void queue_boundary_request(GuestContext* ctx) {
    request_interrupt(*ctx, [](GuestContext& c) { g_boundary->apply(c); });
}
} // namespace

// Interrupt requests run from translated code (helper_check_interrupt,
// without a dispatcher exit) and at an ExitReason::interrupt exit.
FX_TEST(fp_boundary_interrupt_request) {
    init();
    GuestCode code({});
    for (const bool via_exit : {false, true}) {
        const auto pc = code.at(via_exit ? 64 : 0), cont = pc + 8;
        set_region_factory(pc, [&, via_exit](std::uint64_t) {
            return boundary_region(pc, cont, [&, via_exit](RegionBuilder& b, std::uint64_t at) {
                auto& a = b.as();
                a.lea(64, Gp::rcx, ptr(Gp::r15, -kContextBias));
                a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&queue_boundary_request));
                a.call(Gp::rax);
                const Label none = a.new_label();
                a.cmp(32, ptr(Gp::r15, ctx_disp(offsetof(GuestContext, interrupt_pending))), 0);
                a.jcc(x64::Cond::e, none);
                if (via_exit) {
                    b.set_pc(at);
                    a.mov(32, ptr(Gp::r15, d_exit_reason), static_cast<std::int32_t>(ExitReason::interrupt));
                    a.jmp(ptr(Gp::r15, d_stub_exit));
                } else {
                    a.lea(64, Gp::rcx, ptr(Gp::r15, -kContextBias));
                    a.call(ptr(Gp::r15, helper_disp(helper_check_interrupt)));
                }
                a.bind(none);
                b.entry(at);
            });
        });
        Boundary boundary;
        run_scenarios(pc, boundary);
        set_region_factory(pc, nullptr);
    }
}

// A hardware fault in translated code (the stand-in dispatcher runs the
// handler, as the seh dispatcher runs guest handlers and the fault hook).
FX_TEST(fp_boundary_fault) {
    init();
    GuestCode code({});
    const auto pc = code.at(0), cont = code.at(2);
    set_region_factory(pc, [&](std::uint64_t) {
        return boundary_region(pc, cont, [&](RegionBuilder& b, std::uint64_t at) {
            auto& a = b.as();
            a.mov_imm(Gp::rax, 0);
            const auto begin = a.size();
            a.mov(64, Gp::rax, ptr(Gp::rax, 0x40));   // faults
            const auto end = a.size();
            b.fault_site(begin, end, pc + 4);
            b.entry(at);
        });
    });
    set_exception_handler([&](GuestContext& ctx, const GuestExceptionRecord& record) {
        FX_CHECK_EQ(record.code, 0xC0000005u);
        g_boundary->apply(ctx);
        ctx.pc = cont;
    });
    Boundary boundary;
    run_scenarios(pc, boundary);
    set_exception_handler(nullptr);
    set_region_factory(pc, nullptr);
}

namespace {
void boundary_host_entry_body(GuestContext* ctx) { g_boundary->apply(*ctx); }
} // namespace

// Hand-written register_host_entry code handing ctx to C++ through
// fp_state_to_context / fp_state_from_context.
FX_TEST(fp_boundary_host_entry) {
    init();
    GuestCode code({});
    const auto pc = code.at(0), cont = code.at(2), callee = code.at(32);
    x64::Assembler a;
    a.mov(64, Gp::rbx, ptr(Gp::r15, x_disp(30)));
    for (const void* fn : {reinterpret_cast<const void*>(&fp_state_to_context),
                           reinterpret_cast<const void*>(&boundary_host_entry_body),
                           reinterpret_cast<const void*>(&fp_state_from_context)}) {
        a.lea(64, Gp::rcx, ptr(Gp::r15, -kContextBias));
        a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(fn));
        a.call(Gp::rax);
    }
    a.mov(64, ptr(Gp::r15, d_pc), Gp::rbx);
    a.jmp(ptr(Gp::r15, d_stub_indirect));
    a.finalize();
    void* entry = allocate_code(a.size());
    std::memcpy(entry, a.data(), a.size());
    register_host_entry(callee, entry);
    set_region_factory(pc, [&](std::uint64_t) {
        return boundary_region(pc, cont, [&](RegionBuilder& b, std::uint64_t at) {
            const Label continuation = b.as().new_label();
            b.call(at, continuation, callee);
            b.as().bind(continuation);
            b.entry(at);
        });
    });
    Boundary boundary;
    run_scenarios(pc, boundary);
    unregister_host_entry(callee);
    set_region_factory(pc, nullptr);
}

// The C++ helpers themselves.
FX_TEST(fp_state_to_and_from_context) {
    init();
    GuestContext& ctx = ensure_thread_context();
    const unsigned saved = _mm_getcsr();
    ctx.fpsr = kIdc | 0x08000000u;   // IDC, QC
    ctx.fpcr = 0;
    _mm_setcsr(0x1f80u | 0x01u | 0x02u | 0x20u);   // IE, DE, PE
    fp_state_to_context(ctx);
    FX_CHECK_EQ(ctx.fpsr, kIdc | 0x08000000u | kIoc | kIxc);
    // The host cleared IOC and IXC and raised OFC in ctx.fpsr and set RP | FZ;
    // MXCSR still holds the stale IE | PE (and DE).
    ctx.fpsr = kIdc | kOfc;
    ctx.fpcr = (1u << 22) | kFpcrFz;
    fp_state_from_context(ctx);
    FX_CHECK_EQ(_mm_getcsr(), 0x1f80u | (2u << 13) | 0x8040u | 0x08u | 0x02u);   // RC up, FTZ|DAZ, OE, DE
    FX_CHECK_EQ(ctx.fpsr, kIdc);
    _mm_setcsr(saved);
    ctx.fpsr = 0;
    ctx.fpcr = 0;
}
