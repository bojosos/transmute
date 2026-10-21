// Performance: hand-assembled ARM64 loops translated vs the same C compiled
// natively, and translation speed (owner: translator core). Timings are
// printed; the checks only guard against gross regressions.
#include "fx/testing/test.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_emitter.hpp"
#include "fx/translator/translator.hpp"

#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

#include <atomic>
#include <new>
static std::atomic<unsigned long long> g_allocations{0};
void* operator new(std::size_t n) {
    ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

double now_seconds() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

template <typename F>
double median_seconds(F&& body, unsigned runs = 5) {
    std::vector<double> t;
    for (unsigned i = 0; i < runs; ++i) {
        const auto start = now_seconds();
        body();
        t.push_back(now_seconds() - start);
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

struct PinThread {
    DWORD_PTR old;
    PinThread() { old = SetThreadAffinityMask(GetCurrentThread(), 1); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); }
    ~PinThread() { SetThreadAffinityMask(GetCurrentThread(), old); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL); }
};

// Same computation as the ARM64 LCG loop below.
__declspec(noinline) std::uint64_t native_lcg(std::uint64_t n, std::uint64_t a, std::uint64_t c, std::uint64_t* out11) {
    std::uint64_t x9 = 1, x11 = 0, x8 = 0;
    for (;;) {
        x9 = x9 * a + c;
        x11 ^= x9 >> 17;
        const auto x14 = std::rotr(x11, 59);
        x11 = x14 + x8;
        ++x8;
        if (x8 == n) break;
    }
    *out11 = x11;
    return x9;
}

// sum += a[i]; b[i] = sum; idx = a[idx & mask] (dependent load chain + stores)
__declspec(noinline) std::uint64_t native_memory(const std::uint64_t* a, std::uint64_t* b, std::uint64_t n,
                                                 std::uint64_t mask) {
    std::uint64_t idx = 0, sum = 0;
    for (std::uint64_t i = 0; i != n; ++i) {
        idx = a[idx & mask];
        sum += idx;
        b[i & mask] = sum;
    }
    return sum ^ idx;
}

struct Translated {
    DiffRunner& r;
    explicit Translated(DiffRunner& runner) : r(runner) {}
};

DiffRunner& perf_runner() {
    static DiffRunner r;
    return r;
}

// Runs the program at program_base() once with `ctx`; returns elapsed seconds.
double run_translated(DiffRunner& r, const std::vector<std::uint32_t>& prog, GuestContext& ctx) {
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    program[prog.size()] = a64::brk(0xdead);
    const BufferCode code(r.code_base(), r.program_base() + (prog.size() + 1) * 4);
    r.machine.reset();
    // Warm translation outside the timed region.
    r.machine.prepare(ctx);
    const auto* host = r.machine.entry_for(r.program_base(), code);
    if (const char* dump = std::getenv("TRANSMUTE_FX_DUMP")) {   // raw region bytes for disassembly
        if (FILE* f = std::fopen(dump, "ab")) {
            const auto& bytes = r.machine.regions.back().code;
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
    ctx.pc = r.program_base();
    const auto start = now_seconds();
    r.machine.enter(ctx, host);
    return now_seconds() - start;
}

} // namespace

FX_TEST(core_perf_lcg_loop) {
    using namespace a64;
    PinThread pin;
    constexpr std::uint64_t n = 200'000'000;
    const std::vector<std::uint32_t> prog = {
        dp3(true, 0, false, 10, 12, 9, 9),                // loop: madd x9, x9, x10, x12
        logic_shift(true, 2, 1, false, 9, 17, 11, 11),    // eor x11, x11, x9, lsr #17
        extr(true, 11, 59, 11, 14),                       // ror x14, x11, #59
        add_shift(true, false, false, 0, 8, 0, 14, 11),   // add x11, x14, x8
        add_imm(true, false, false, 8, 8, 1),             // add x8, x8, #1
        add_shift(true, true, true, 0, 13, 0, 8, 31),     // cmp x8, x13
        bcond(1, -6),                                     // b.ne loop
    };
    auto& r = perf_runner();
    const std::uint64_t a = 6364136223846793005ULL, c = 1442695040888963407ULL;
    GuestContext ctx{};
    double translated = 0;
    std::uint64_t t9 = 0, t11 = 0;
    translated = median_seconds([&] {
        ctx = r.context(1);
        ctx.x[9] = 1; ctx.x[10] = a; ctx.x[11] = 0; ctx.x[12] = c; ctx.x[8] = 0; ctx.x[13] = n;
        run_translated(r, prog, ctx);
        t9 = ctx.x[9];
        t11 = ctx.x[11];
    });
    std::uint64_t n9 = 0, n11 = 0;
    const double native = median_seconds([&] { n9 = native_lcg(n, a, c, &n11); });
    FX_CHECK_EQ(t9, n9);
    FX_CHECK_EQ(t11, n11);
    const auto ratio = translated / native;
    std::printf("    LCG loop: native %.3f ns/iter, translated %.3f ns/iter, ratio %.2fx\n", native * 1e9 / n,
                translated * 1e9 / n, ratio);
    FX_CHECK(ratio < 3.0);
}

FX_TEST(core_perf_memory_loop) {
    using namespace a64;
    PinThread pin;
    constexpr std::uint64_t n = 100'000'000;
    constexpr std::uint64_t entries = 1u << 12;   // 32 KiB: L1-resident
    std::vector<std::uint64_t> a(entries), b1(entries), b2(entries);
    for (std::uint64_t i = 0; i < entries; ++i) a[i] = (i * 2654435761u + 12345) % entries;
    // loop: and x3, x9, x4 ; ldr x9, [x0, x3, lsl #3] ; add x10, x10, x9 ; and x5, x8, x4
    //       str x10, [x1, x5, lsl #3] ; add x8, x8, #1 ; cmp x8, x2 ; b.ne loop
    const std::vector<std::uint32_t> prog = {
        logic_shift(true, 0, 0, false, 4, 0, 9, 3),       // and x3, x9, x4
        ldst_reg(3, 1, 3, 3, true, 0, 9),                 // ldr x9, [x0, x3, lsl #3]
        add_shift(true, false, false, 0, 9, 0, 10, 10),   // add x10, x10, x9
        logic_shift(true, 0, 0, false, 4, 0, 8, 5),       // and x5, x8, x4
        ldst_reg(3, 0, 5, 3, true, 1, 10),                // str x10, [x1, x5, lsl #3]
        add_imm(true, false, false, 8, 8, 1),             // add x8, x8, #1
        add_shift(true, true, true, 0, 2, 0, 8, 31),      // cmp x8, x2
        bcond(1, -7),                                     // b.ne loop
    };
    auto& r = perf_runner();
    GuestContext ctx{};
    std::uint64_t result = 0;
    const double translated = median_seconds([&] {
        ctx = r.context(2);
        ctx.x[0] = reinterpret_cast<std::uint64_t>(a.data());
        ctx.x[1] = reinterpret_cast<std::uint64_t>(b1.data());
        ctx.x[2] = n;
        ctx.x[4] = entries - 1;
        ctx.x[8] = 0; ctx.x[9] = 0; ctx.x[10] = 0;
        run_translated(r, prog, ctx);
        result = ctx.x[10] ^ ctx.x[9];
    });
    std::uint64_t expect = 0;
    const double native = median_seconds([&] { expect = native_memory(a.data(), b2.data(), n, entries - 1); });
    FX_CHECK_EQ(result, expect);
    FX_CHECK(b1 == b2);
    const auto ratio = translated / native;
    std::printf("    memory loop: native %.3f ns/iter, translated %.3f ns/iter, ratio %.2fx\n", native * 1e9 / n,
                translated * 1e9 / n, ratio);
    FX_CHECK(ratio < 4.0);
}

namespace {
__declspec(noinline) std::uint64_t native_leaf(std::uint64_t a, std::uint64_t b) { return a * 3 + b; }
std::uint64_t (*volatile g_leaf)(std::uint64_t, std::uint64_t) = &native_leaf;
__declspec(noinline) std::uint64_t native_calls(std::uint64_t n) {
    const auto leaf = g_leaf;   // a real call per iteration (no inlining or folding)
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i != n; ++i) acc = leaf(acc, i);
    return acc;
}
} // namespace

// BL + RET through the RSB: flush dirty pins, push, return hit, continuation entry.
FX_TEST(core_perf_call_loop) {
    using namespace a64;
    PinThread pin;
    constexpr std::uint64_t n = 50'000'000;
    // loop: mov x0, x21; mov x1, x19; bl leaf; mov x21, x0; add x19, x19, #1; cmp x19, x20; b.ne loop; b end
    // leaf: add x0, x0, x0, lsl #1; add x0, x0, x1; ret
    const std::vector<std::uint32_t> prog2 = {
        logic_shift(true, 1, 0, false, 21, 0, 31, 0),     // 0 mov x0, x21
        logic_shift(true, 1, 0, false, 19, 0, 31, 1),     // 1 mov x1, x19
        bl(7),                                            // 2 bl leaf (9)
        logic_shift(true, 1, 0, false, 0, 0, 31, 21),     // 3 mov x21, x0
        add_imm(true, false, false, 19, 19, 1),           // 4 add x19, x19, #1
        add_shift(true, true, true, 0, 20, 0, 19, 31),    // 5 cmp x19, x20
        bcond(1, -6),                                     // 6 b.ne loop
        b(5),                                             // 7 b end (12)
        nop(),                                            // 8
        add_shift(true, false, false, 0, 0, 1, 0, 0),     // 9 leaf: add x0, x0, x0, lsl #1
        add_shift(true, false, false, 0, 1, 0, 0, 0),     // 10 add x0, x0, x1
        ret(),                                            // 11 ret
    };
    auto& r = perf_runner();
    GuestContext ctx{};
    std::uint64_t result = 0;
    const double translated = median_seconds([&] {
        ctx = r.context(3);
        ctx.x[19] = 0; ctx.x[20] = n; ctx.x[21] = 0;
        ctx.x[30] = kReturnToHostPc;
        auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
        std::memcpy(program, prog2.data(), prog2.size() * 4);
        program[prog2.size()] = a64::brk(0xdead);
        const BufferCode code(r.code_base(), r.program_base() + (prog2.size() + 1) * 4);
        r.machine.reset();
        ctx.pc = r.program_base();
        const auto res = r.machine.run(ctx, code, kReturnToHostPc);   // links once, then stays in translated code
        FX_CHECK(res.ok && res.reason == ExitReason::brk && res.dispatches < 10);
        if (const char* dump = std::getenv("TRANSMUTE_FX_DUMP")) {
            if (FILE* f = std::fopen(dump, "wb")) {
                for (const auto& region : r.machine.regions) std::fwrite(region.code.data(), 1, region.code.size(), f);
                std::fclose(f);
            }
        }
        result = ctx.x[21];
    });
    std::uint64_t expect = 0;
    const double native = median_seconds([&] { expect = native_calls(n); });
    FX_CHECK_EQ(result, expect);
    std::printf("    call loop: native %.3f ns/call, translated %.3f ns/call, ratio %.2fx\n", native * 1e9 / n,
                translated * 1e9 / n, translated / native);
}

FX_TEST(core_perf_translation_speed) {
    auto& r = perf_runner();
    Rng rng(77);
    InstGen g(rng);
    // A "function" of 4000 instructions with forward branches every ~8 words.
    std::vector<std::uint32_t> prog;
    for (unsigned i = 0; i < 4000; ++i) {
        if (i % 8 == 7) prog.push_back(a64::bcond(rng.below(14), 1 + static_cast<std::int32_t>(rng.below(6))));
        else prog.push_back(g.data_or_memory());
    }
    prog.push_back(a64::ret());
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + prog.size() * 4);
    TranslateOptions options;
    std::size_t bytes = 0;
    unsigned blocks = 0;
    const double whole = median_seconds([&] {
        const auto region = translate_region(r.program_base(), code, options);
        bytes = region.code.size();
        blocks = region.stats.blocks;
    });
    // Small regions (typical basic blocks): translate at many start points.
    options.max_instructions = 12;
    const unsigned count = 2000;
    const double small = median_seconds([&] {
        for (unsigned i = 0; i < count; ++i) translate_region(r.program_base() + 8 * i, code, options);
    });
    std::printf("    translate 4001-instruction region: %.1f us (%.0f ns/instruction, %u blocks, %zu bytes, %.1f bytes/insn)\n",
                whole * 1e6, whole * 1e9 / 4001, blocks, bytes, static_cast<double>(bytes) / 4001);
    std::printf("    translate 12-instruction regions: %.2f us each\n", small * 1e6 / count);
    std::printf("    regions %llu, retries %llu\n", static_cast<unsigned long long>(core::core_counters().regions.load()),
                static_cast<unsigned long long>(core::core_counters().retries.load()));
    {
        PinThread pin;
        core::Plan plan;
        core::PlanOverrides ov;
        constexpr unsigned reps = 20000;
        const auto pc = r.program_base() + 8 * 17;
        const double plan_time = median_seconds([&] {
            for (unsigned i = 0; i < reps; ++i) core::build_plan(pc, code, options, ov, plan);
        });
        const double emit_time = median_seconds([&] {
            for (unsigned i = 0; i < reps; ++i) {
                TranslatedRegion out;
                core::RegionEmitter e(plan, options, out);
                e.run();
            }
        });
        const double full_time = median_seconds([&] {
            for (unsigned i = 0; i < reps; ++i) translate_region(pc, code, options);
        });
        std::printf("    one 12-instruction region (%zu insns): plan %.2f us, emit %.2f us, translate_region %.2f us\n",
                    plan.insts.size(), plan_time * 1e6 / reps, emit_time * 1e6 / reps, full_time * 1e6 / reps);
        // Thread CPU cycles are robust against preemption on a loaded machine.
        ULONG64 c0 = 0, c1 = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        for (unsigned i = 0; i < reps; ++i) translate_region(pc, code, options);
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        std::printf("    translate_region: %.0f thread cycles per 12-instruction region (%.0f per instruction)\n",
                    static_cast<double>(c1 - c0) / reps, static_cast<double>(c1 - c0) / reps / 12);
        if (std::getenv("TRANSMUTE_FX_PROFILE")) {
            // Poor man's sampling profiler: sample this thread's RIP from a helper thread.
            HANDLE self;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
            std::atomic<bool> stop{false};
            std::vector<DWORD64> samples;
            samples.reserve(200000);
            std::thread sampler([&] {
                SetThreadAffinityMask(GetCurrentThread(), 2);
                while (!stop) {
                    if (SuspendThread(self) != static_cast<DWORD>(-1)) {
                        CONTEXT c{};
                        c.ContextFlags = CONTEXT_CONTROL;
                        if (GetThreadContext(self, &c)) samples.push_back(c.Rip);
                        ResumeThread(self);
                    }
                    for (volatile int spin = 0; spin < 2000; ++spin) {}
                }
            });
            for (unsigned i = 0; i < 60000; ++i) translate_region(pc, code, options);
            stop = true;
            sampler.join();
            CloseHandle(self);
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            std::map<std::string, unsigned> counts;
            for (const auto rip : samples) {
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                counts[SymFromAddr(GetCurrentProcess(), rip, &disp, sym) ? sym->Name : "?"]++;
            }
            std::vector<std::pair<unsigned, std::string>> sorted;
            for (const auto& [k, v] : counts) sorted.emplace_back(v, k);
            std::sort(sorted.rbegin(), sorted.rend());
            std::printf("    profile (%zu samples):\n", samples.size());
            for (std::size_t i = 0; i < sorted.size() && i < 40; ++i)
                std::printf("      %5.1f%% %s\n", 100.0 * sorted[i].first / samples.size(), sorted[i].second.c_str());
        }
        const auto before = g_allocations.load();
        const auto region = translate_region(pc, code, options);
        std::printf("    allocations per translate_region: %llu (code %zu bytes, %zu exits, %zu fixups, %zu faults)\n",
                    g_allocations.load() - before, region.code.size(), region.exits.size(), region.fixups.size(),
                    region.faults.size());
    }
    FX_CHECK(small * 1e6 / count < 100);
}
