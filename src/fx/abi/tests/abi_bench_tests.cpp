// Per-call cost of a guest->host thunk versus a direct native call.
//
// The guest side is emulated by an x64 loop that does what translated code
// does around a BL to an import: write the argument and x30 into the
// context, push the RSB entry, jump to the thunk; the thunk returns through
// the RSB hit path to the loop's continuation. The native loop calls the
// same function directly.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/tests/abi_test_support.hpp"
#include "fx/common/x64_assembler.hpp"
#include "fx/testing/test.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::abi_test;
using x64::Gp;
using x64::ptr;

namespace {

extern "C" __declspec(noinline) long long bench_add1(long long x) { return x + 1; }

constexpr std::uint64_t kLoopReturnPc = 0x0000'0bad'f00d'0000ULL;

struct Loop {
    std::uint8_t* code{};
};

// Guest-emulating loop. Iteration count in ctx.spill[0].
std::uint8_t* build_guest_loop(std::uint64_t thunk) {
    x64::Assembler a;
    const auto rsb = ctx_disp(offsetof(GuestContext, rsb));
    const auto top = a.bound_label();
    const auto cont = a.new_label();
    a.mov(64, ptr(Gp::r15, x_disp(0)), 41);
    a.mov_imm64(Gp::rax, kLoopReturnPc);
    a.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rax);
    a.mov(32, Gp::rax, ptr(Gp::r15, ctx_disp(offsetof(GuestContext, rsb_top))));
    a.mov(32, Gp::rdx, Gp::rax);
    a.shl(32, Gp::rdx, 4);
    a.mov_imm64(Gp::rcx, kLoopReturnPc);
    a.mov(64, ptr(Gp::r15, Gp::rdx, 1, rsb), Gp::rcx);
    a.lea(64, Gp::rcx, x64::rip(cont));
    a.mov(64, ptr(Gp::r15, Gp::rdx, 1, rsb + 8), Gp::rcx);
    a.add(32, Gp::rax, 1);
    a.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
    a.mov(32, ptr(Gp::r15, ctx_disp(offsetof(GuestContext, rsb_top))), Gp::rax);
    a.mov_imm64(Gp::rax, thunk);
    a.jmp(Gp::rax);
    a.align(16);
    a.bind(cont);
    a.alu(x64::Alu::sub, 64, ptr(Gp::r15, spill_disp(0)), 1);
    a.jcc(x64::Cond::nz, top);
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(return_stub()));
    a.jmp(Gp::rax);
    a.finalize();
    auto* memory = static_cast<std::uint8_t*>(allocate_code(a.size(), 16));
    std::memcpy(memory, a.data(), a.size());
    return memory;
}

// Native loop: the same call without a thunk.
std::uint8_t* build_native_loop(const void* native) {
    x64::Assembler a;
    const auto top = a.bound_label();
    a.mov(32, Gp::rcx, ptr(Gp::r15, x_disp(0)));
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(native));
    a.call(Gp::rax);
    a.mov(64, ptr(Gp::r15, x_disp(0)), Gp::rax);
    a.alu(x64::Alu::sub, 64, ptr(Gp::r15, spill_disp(0)), 1);
    a.jcc(x64::Cond::nz, top);
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(return_stub()));
    a.jmp(Gp::rax);
    a.finalize();
    auto* memory = static_cast<std::uint8_t*>(allocate_code(a.size(), 16));
    std::memcpy(memory, a.data(), a.size());
    return memory;
}

double seconds_per_iteration(GuestContext& ctx, const std::uint8_t* loop, std::uint64_t iterations) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    ctx.spill[0] = iterations;
    ctx.rsb_top = 0;
    QueryPerformanceCounter(&t0);
    run_guest_call(ctx, reinterpret_cast<std::uint64_t>(loop));
    QueryPerformanceCounter(&t1);
    return static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart) /
           static_cast<double>(iterations);
}

double median_ns(GuestContext& ctx, const std::uint8_t* loop, std::uint64_t iterations) {
    std::vector<double> runs;
    for (int i = 0; i < 7; ++i) runs.push_back(seconds_per_iteration(ctx, loop, iterations) * 1e9);
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}

} // namespace

FX_TEST(bench_thunk_overhead) {
    SetThreadAffinityMask(GetCurrentThread(), 1);
    auto thread = make_thread();
    auto& ctx = *thread->ctx;
    constexpr std::uint64_t kIterations = 5'000'000;

    struct Case { const char* name; const void* native; const char* sig; };
    const Case cases[] = {
        {"add1 l(l)", reinterpret_cast<const void*>(&bench_add1), "l(l)"},
        {"GetCurrentThreadId I()", reinterpret_cast<const void*>(&GetCurrentThreadId), "I()"},
        {"GetLastError I()", reinterpret_cast<const void*>(&GetLastError), "I()"},
    };
    double worst = 0;
    for (const auto& k : cases) {
        const auto thunk = abi::bind_host_pointer(k.native, abi::intern_signature(k.sig));
        auto* guest_loop = build_guest_loop(thunk);
        auto* native_loop = build_native_loop(k.native);
        // Warm up and check correctness of the loop itself.
        ctx.spill[0] = 1000;
        ctx.x[0] = 0;
        run_guest_call(ctx, reinterpret_cast<std::uint64_t>(guest_loop));
        FX_CHECK_EQ(ctx.spill[0], 0u);
        FX_CHECK(ctx.pc != kLoopReturnPc);         // every return took the RSB hit path
        const double via_thunk = median_ns(ctx, guest_loop, kIterations);
        const double direct = median_ns(ctx, native_loop, kIterations);
        const double overhead = via_thunk - direct;
        worst = std::max(worst, overhead);
        std::printf("  %-26s thunk %6.2f ns  native %6.2f ns  overhead %6.2f ns/call\n", k.name, via_thunk,
                    direct, overhead);
    }
    std::fflush(stdout);
    SetThreadAffinityMask(GetCurrentThread(), ~0ull);
    // Target: < 10 ns per call. Allow headroom for loaded CI machines.
    FX_CHECK(worst < 25.0);
}
