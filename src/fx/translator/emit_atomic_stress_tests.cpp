// Multithreaded stress and scaling: translated guest snippets on 1/2/4/8
// host threads, each with its own GuestContext, against native x64 atomics
// in the same process (owner: atomics/system).
#include "fx/translator/emit_system_test_util.hpp"

#include "fx/testing/test.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::systest;
using testing::Buffer;

namespace {

constexpr unsigned kThreadCounts[] = {1, 2, 4, 8};

struct FusionScope {
    explicit FusionScope(bool on) { system_set_fusion(on); }
    ~FusionScope() { system_set_fusion(true); }
};

// x0 = counter, x27 = iterations. MSVC _InterlockedIncrement64 + dmb ish.
std::vector<std::uint32_t> llsc_increment() {
    return {a64::ldxr(8, 8, 0, true), a64::add_imm(true, 8, 8, 1), a64::stxr(8, 9, 8, 0, true),
            a64::cbnz(false, 9, -3), a64::mov_reg(true, 1, 8), a64::kDmbIsh,
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -7)};
}
// ldaddal x1, x8, [x0]   (x1 = 1)
std::vector<std::uint32_t> lse_increment() {
    return {a64::lse(0, 8, true, true, 1, 8, 0), a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -2)};
}
// MSVC _InterlockedCompareExchange64 retry loop around a plain load.
std::vector<std::uint32_t> llsc_cas_increment() {
    return {a64::ldr_x(1, 0),
            a64::add_imm(true, 2, 1, 1),                                 // 1: retry
            a64::ldxr(8, 8, 0, true), a64::cmp_reg(true, 8, 1), a64::bcond(a64::NE, 3),
            a64::stxr(8, 9, 2, 0, true), a64::cbnz(false, 9, -4),
            a64::kDmbIsh,                                                // 7
            a64::alu_reg(0x4b, true, 10, 8, 1), a64::mov_reg(true, 1, 8), a64::cbnz(true, 10, -9),
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -12)};
}
// casal retry loop
std::vector<std::uint32_t> lse_cas_increment() {
    return {a64::ldr_x(1, 0),
            a64::add_imm(true, 2, 1, 1),                                 // 1: retry
            a64::mov_reg(true, 3, 1), a64::cas(8, true, true, 3, 2, 0),
            a64::alu_reg(0x4b, true, 10, 3, 1), a64::mov_reg(true, 1, 3), a64::cbnz(true, 10, -5),
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -8)};
}
// Treiber push: x0 = &head, x2 = first node (16-byte nodes), x27 = count.
std::vector<std::uint32_t> push_lse() {
    return {a64::ldr_x(1, 0),
            a64::str_x(1, 2),                                            // 1: node->next = head
            a64::mov_reg(true, 3, 1), a64::cas(8, true, true, 3, 2, 0),
            a64::alu_reg(0x4b, true, 10, 3, 1), a64::mov_reg(true, 1, 3), a64::cbnz(true, 10, -5),
            a64::add_imm(true, 2, 2, 16), a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -9)};
}
std::vector<std::uint32_t> push_llsc() {
    return {a64::ldr_x(1, 0),
            a64::str_x(1, 2),                                            // 1
            a64::ldxr(8, 8, 0, true), a64::cmp_reg(true, 8, 1), a64::bcond(a64::NE, 3),
            a64::stxr(8, 9, 2, 0, true), a64::cbnz(false, 9, -4),
            a64::kDmbIsh,                                                // 7
            a64::alu_reg(0x4b, true, 10, 8, 1), a64::mov_reg(true, 1, 8), a64::cbnz(true, 10, -9),
            a64::add_imm(true, 2, 2, 16), a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -13)};
}
// Treiber pop until empty: x0 = &head, x5 = output cursor.
std::vector<std::uint32_t> pop_lse() {
    return {a64::ldr_x(1, 0),
            a64::cbz(true, 1, 10),                                       // 1: retry
            a64::ldr_x(2, 1), a64::mov_reg(true, 3, 1), a64::cas(8, true, true, 3, 2, 0),
            a64::alu_reg(0x4b, true, 10, 3, 1), a64::mov_reg(true, 1, 3), a64::cbnz(true, 10, -6),
            a64::str_x(1, 5), a64::add_imm(true, 5, 5, 8), a64::b(-10)};
}
std::vector<std::uint32_t> pop_llsc() {
    return {a64::ldr_x(1, 0),
            a64::cbz(true, 1, 14),                                       // 1
            a64::ldr_x(2, 1),
            a64::ldxr(8, 8, 0, true), a64::cmp_reg(true, 8, 1), a64::bcond(a64::NE, 3),   // 3
            a64::stxr(8, 9, 2, 0, true), a64::cbnz(false, 9, -4),
            a64::kDmbIsh,                                                // 8
            a64::alu_reg(0x4b, true, 10, 8, 1), a64::mov_reg(true, 1, 8), a64::cbnz(true, 10, -10),
            a64::str_x(1, 5), a64::add_imm(true, 5, 5, 8), a64::b(-14)};
}
// 128-bit {lo += 1, hi += 2} with LDAXP/STLXP (unfusable body: two adds).
std::vector<std::uint32_t> pair_increment() {
    return {a64::ldxp(true, 8, 9, 0, true), a64::add_imm(true, 8, 8, 1), a64::add_imm(true, 9, 9, 2),
            a64::stxp(true, 10, 8, 9, 0, true), a64::cbnz(false, 10, -4),
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -6)};
}
// 128-bit atomic snapshot (fused LDAXP/STLXP reload); x5 counts torn reads.
std::vector<std::uint32_t> pair_snapshot() {
    return {a64::ldxp(true, 8, 9, 0, true), a64::stxp(true, 10, 8, 9, 0, true), a64::cbnz(false, 10, -2),
            a64::add_imm(true, 11, 8, 0), a64::alu_reg(0x0b, true, 11, 11, 8),   // x11 = 2*lo
            a64::alu_reg(0x4b, true, 11, 11, 9), a64::cbz(true, 11, 2), a64::add_imm(true, 5, 5, 1),
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -9)};
}

struct Contexts {
    explicit Contexts(unsigned n) : ctx(n) { for (unsigned i = 0; i < n; ++i) { ctx[i] = make_context(i + 1); } }
    std::vector<GuestContext> ctx;
};

// Runs `program` on `threads` threads; init(t, ctx) sets registers. Returns seconds.
double run_guest(const Program& program, unsigned threads, const std::function<void(unsigned, GuestContext&)>& init) {
    Contexts contexts(threads);
    for (unsigned t = 0; t < threads; ++t) init(t, contexts.ctx[t]);
    return run_threads(threads, [&](unsigned t) { program.run(contexts.ctx[t]); });
}

double ns_per_op(double seconds, std::uint64_t ops_per_thread) { return seconds * 1e9 / double(ops_per_thread); }

} // namespace

FX_TEST(stress_shared_counter_llsc_and_lse_exact) {
    Buffer shared(4096);
    auto* counter = reinterpret_cast<volatile std::uint64_t*>(shared.data());
    constexpr std::uint64_t kIters = 200000;
    for (int variant = 0; variant < 3; ++variant) {   // fused LL/SC, unfused LL/SC, LSE
        FusionScope scope(variant != 1);
        Program program(variant == 2 ? lse_increment() : llsc_increment());
        if (variant == 0) FX_CHECK_EQ(program.family_spans().front(), 6u);
        if (variant == 1) FX_CHECK_EQ(program.family_spans().front(), 1u);
        for (unsigned threads : kThreadCounts) {
            *counter = 0;
            run_guest(program, threads, [&](unsigned, GuestContext& c) {
                c.x[0] = shared.address(); c.x[27] = kIters; c.x[1] = 1;
            });
            FX_CHECK_EQ(*counter, kIters * threads);
        }
    }
}

FX_TEST(stress_cas_increment_exact) {
    Buffer shared(4096);
    auto* counter = reinterpret_cast<volatile std::uint64_t*>(shared.data());
    constexpr std::uint64_t kIters = 100000;
    for (int variant = 0; variant < 3; ++variant) {   // fused MSVC CAS, unfused, LSE CASAL
        FusionScope scope(variant != 1);
        Program program(variant == 2 ? lse_cas_increment() : llsc_cas_increment());
        for (unsigned threads : kThreadCounts) {
            *counter = 0;
            run_guest(program, threads, [&](unsigned, GuestContext& c) { c.x[0] = shared.address(); c.x[27] = kIters; });
            FX_CHECK_EQ(*counter, kIters * threads);
        }
    }
}

FX_TEST(stress_treiber_stack_push_pop) {
    constexpr unsigned kNodes = 20000;
    for (int variant = 0; variant < 3; ++variant) {   // LSE, fused LL/SC, unfused LL/SC
        FusionScope scope(variant != 2);
        Program push(variant == 0 ? push_lse() : push_llsc());
        Program pop(variant == 0 ? pop_lse() : pop_llsc());
        for (unsigned threads : kThreadCounts) {
            Buffer head(4096);
            Buffer nodes(std::size_t{16} * kNodes * threads + 4096);
            run_guest(push, threads, [&](unsigned t, GuestContext& c) {
                c.x[0] = head.address();
                c.x[2] = nodes.address() + std::uint64_t{16} * kNodes * t;
                c.x[27] = kNodes;
            });
            // every node exactly once on the list
            std::set<std::uint64_t> seen;
            std::uint64_t p = *reinterpret_cast<std::uint64_t*>(head.data());
            while (p && seen.size() <= std::size_t{kNodes} * threads) {
                FX_REQUIRE(seen.insert(p).second);
                p = *reinterpret_cast<std::uint64_t*>(p);
            }
            FX_CHECK_EQ(seen.size(), std::size_t{kNodes} * threads);
            // pop everything concurrently
            std::vector<GuestContext> contexts(threads);
            Buffer outputs(std::size_t{8} * kNodes * threads * threads + 4096);
            for (unsigned t = 0; t < threads; ++t) {
                contexts[t] = make_context(t + 50);
                contexts[t].x[0] = head.address();
                contexts[t].x[5] = outputs.address() + std::uint64_t{8} * kNodes * threads * t;
            }
            run_threads(threads, [&](unsigned t) { pop.run(contexts[t]); });
            std::set<std::uint64_t> popped;
            for (unsigned t = 0; t < threads; ++t) {
                const auto begin = outputs.address() + std::uint64_t{8} * kNodes * threads * t;
                for (auto q = begin; q < contexts[t].x[5]; q += 8) {
                    FX_REQUIRE(popped.insert(*reinterpret_cast<std::uint64_t*>(q)).second);
                }
            }
            FX_CHECK(popped == seen);
            FX_CHECK_EQ(*reinterpret_cast<std::uint64_t*>(head.data()), std::uint64_t{0});
        }
    }
}

FX_TEST(stress_128bit_pairs_are_atomic) {
    Buffer shared(4096);
    auto* pair = reinterpret_cast<volatile std::uint64_t*>(shared.data());
    constexpr std::uint64_t kIters = 50000;
    for (bool fused : {true, false}) {
        FusionScope scope(fused);
        Program inc(pair_increment());
        Program snap(pair_snapshot());
        for (unsigned threads : {2u, 4u, 8u}) {
            pair[0] = pair[1] = 0;
            std::vector<GuestContext> contexts(threads);
            for (unsigned t = 0; t < threads; ++t) {
                contexts[t] = make_context(t + 9);
                contexts[t].x[0] = shared.address();
                contexts[t].x[27] = kIters;
                contexts[t].x[5] = 0;
            }
            run_threads(threads, [&](unsigned t) { (t == 0 ? snap : inc).run(contexts[t]); });
            FX_CHECK_EQ(pair[0], kIters * (threads - 1));
            FX_CHECK_EQ(pair[1], 2 * kIters * (threads - 1));
            FX_CHECK_EQ(contexts[0].x[5], std::uint64_t{0});   // no torn snapshot
        }
    }
}

FX_TEST(stress_private_counters) {
    Buffer shared(64 * 1024);
    constexpr std::uint64_t kIters = 200000;
    for (int variant = 0; variant < 3; ++variant) {
        FusionScope scope(variant != 1);
        Program program(variant == 2 ? lse_increment() : llsc_increment());
        for (unsigned threads : kThreadCounts) {
            std::memset(shared.data(), 0, shared.size());
            run_guest(program, threads, [&](unsigned t, GuestContext& c) {
                c.x[0] = shared.address() + 256 * t; c.x[27] = kIters; c.x[1] = 1;
            });
            for (unsigned t = 0; t < threads; ++t)
                FX_CHECK_EQ(*reinterpret_cast<std::uint64_t*>(shared.data() + 256 * t), kIters);
        }
    }
}

// ---- performance report ---------------------------------------------------------------
//
// The machine is shared with parallel builds, so every variant is measured in
// interleaved rounds (native and fx back to back under the same load) and the
// minimum per variant is reported, with worker threads at high priority.

namespace {

__declspec(noinline) void native_increment(volatile long long* p, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; ++i) _InterlockedIncrement64(p);
}
__declspec(noinline) void native_cas_increment(volatile long long* p, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; ++i) {
        long long old = *p;
        for (;;) {
            const long long seen = _InterlockedCompareExchange64(p, old + 1, old);
            if (seen == old) break;
            old = seen;
        }
    }
}

struct Variant {
    std::string name;
    void (*native)(volatile long long*, std::uint64_t);   // or guest program
    std::unique_ptr<Program> program;
    double best[4];
};

double run_variant(Variant& v, unsigned threads, bool shared_counter, std::uint64_t iters, Buffer& memory) {
    std::memset(memory.data(), 0, 4096);
    const auto counter_at = [&](unsigned t) { return memory.address() + (shared_counter ? 0 : 256 * t); };
    if (v.native) {
        return run_threads(threads, [&](unsigned t) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            v.native(reinterpret_cast<volatile long long*>(counter_at(t)), iters);
        });
    }
    Contexts contexts(threads);
    for (unsigned t = 0; t < threads; ++t) {
        contexts.ctx[t].x[0] = counter_at(t);
        contexts.ctx[t].x[27] = iters;
        contexts.ctx[t].x[1] = 1;
    }
    return run_threads(threads, [&](unsigned t) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        v.program->run(contexts.ctx[t]);
    });
}

std::vector<Variant> make_variants(std::initializer_list<std::tuple<const char*, void (*)(volatile long long*, std::uint64_t),
                                                                   std::vector<std::uint32_t>, bool>> list) {
    std::vector<Variant> out;
    for (const auto& [name, native, words, fused] : list) {
        Variant v{name, native, nullptr, {1e30, 1e30, 1e30, 1e30}};
        if (!native) {
            FusionScope scope(fused);
            v.program = std::make_unique<Program>(words);
        }
        out.push_back(std::move(v));
    }
    return out;
}

void measure(const char* title, std::vector<Variant>& variants, bool shared_counter, std::uint64_t iters, Buffer& memory) {
    for (unsigned i = 0; i < 4; ++i) {
        for (int round = 0; round < 7; ++round) {
            for (auto& v : variants) {
                const double s = run_variant(v, kThreadCounts[i], shared_counter, iters, memory);
                v.best[i] = std::min(v.best[i], ns_per_op(s, iters));
            }
        }
    }
    std::printf("    %s: ns per operation per thread (min of 7 interleaved rounds), [x native]\n", title);
    std::printf("    %-28s %15s %15s %15s %15s\n", "", "1T", "2T", "4T", "8T");
    for (const auto& v : variants) {
        std::printf("    %-28s", v.name.c_str());
        for (unsigned i = 0; i < 4; ++i) std::printf("  %7.2f [%4.2f]", v.best[i], v.best[i] / variants[0].best[i]);
        std::printf("\n");
    }
}

// Times a raw x64 loop body `n` times (rcx = counter, r8 = scratch memory).
double time_x64(const std::function<void(x64::Assembler&)>& body, void* memory, std::uint64_t n) {
    using x64::Gp;
    x64::Assembler a;
    a.push(Gp::rbx);
    a.sub(64, Gp::rsp, 32);
    a.mov(64, Gp::r8, Gp::rcx);
    a.mov(64, Gp::rcx, Gp::rdx);
    a.mov_imm(Gp::rax, 1);
    const auto top = a.bound_label();
    body(a);
    a.sub(64, Gp::rcx, 1);
    a.jcc(x64::Cond::ne, top);
    a.add(64, Gp::rsp, 32);
    a.pop(Gp::rbx);
    a.ret();
    a.finalize();
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    std::memcpy(code, a.data(), a.size());
    FlushInstructionCache(GetCurrentProcess(), code, a.size());
    auto fn = reinterpret_cast<void (*)(void*, std::uint64_t)>(code);
    double best = 1e30;
    for (int round = 0; round < 7; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        fn(memory, n);
        best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(n));
    }
    VirtualFree(code, 0, MEM_RELEASE);
    return best;
}

} // namespace

FX_TEST(bench_barrier_and_release_store_sequences) {
    using x64::Gp;
    Buffer memory(4096);
    constexpr std::uint64_t n = 2'000'000;
    const auto store = [](x64::Assembler& a) { a.mov(64, x64::ptr(Gp::r8, 0), Gp::rax); };
    const auto load = [](x64::Assembler& a) { a.mov(64, Gp::rbx, x64::ptr(Gp::r8, 64)); };
    const double base = time_x64([&](x64::Assembler& a) { store(a); load(a); }, memory.data(), n);
    const double mfence = time_x64([&](x64::Assembler& a) { store(a); a.mfence(); load(a); }, memory.data(), n);
    const double lockor = time_x64([&](x64::Assembler& a) {
        store(a); a.lock(); a.alu(x64::Alu::or_, 32, x64::ptr(Gp::rsp, 0), 0); load(a); }, memory.data(), n);
    const double xchg = time_x64([&](x64::Assembler& a) {
        a.mov(64, Gp::rdx, Gp::rax); a.xchg(64, x64::ptr(Gp::r8, 0), Gp::rdx); load(a); }, memory.data(), n);
    const double lockadd = time_x64([&](x64::Assembler& a) {
        a.lock(); a.xadd(64, x64::ptr(Gp::r8, 0), Gp::rax); load(a); }, memory.data(), n);
    // ABI 11: STLR = store + flag byte; a later plain load is unaffected, an
    // LDAR tests the flag (r8+128 stands in for GuestContext::release_pending);
    // fences are `lock and byte [flag], 0` (fence + clear in one RMW).
    const auto flag = x64::ptr(Gp::r8, 128);
    const auto fence_clear = [&](x64::Assembler& a) { a.lock(); a.alu(x64::Alu::and_, 8, flag, 0); };
    const auto check = [&](x64::Assembler& a) {
        const auto skip = a.new_label();
        a.cmp(8, flag, 0); a.jcc_short(x64::Cond::e, skip); fence_clear(a); a.bind(skip);
    };
    const double lockand = time_x64([&](x64::Assembler& a) { store(a); fence_clear(a); load(a); }, memory.data(), n);
    const double lockor_clear = time_x64([&](x64::Assembler& a) {
        store(a); a.lock(); a.alu(x64::Alu::or_, 32, x64::ptr(Gp::rsp, 0), 0); a.mov(8, flag, 0); load(a); },
        memory.data(), n);
    const double stlr_flag = time_x64([&](x64::Assembler& a) { store(a); a.mov(8, flag, 1); load(a); }, memory.data(), n);
    std::memset(memory.data(), 0, memory.size());
    const double ldar_check = time_x64([&](x64::Assembler& a) { load(a); check(a); load(a); }, memory.data(), n);
    const double stlr_ldar = time_x64([&](x64::Assembler& a) {
        store(a); a.mov(8, flag, 1); check(a); load(a); }, memory.data(), n);
    std::printf("    store+load baseline        %6.2f ns\n", base);
    std::printf("    DMB: store;mfence;load     %6.2f ns\n", mfence);
    std::printf("    DMB: store;lock or;load    %6.2f ns (before ABI 11)\n", lockor);
    std::printf("    DMB: store;lock and flag;load %6.2f ns\n", lockand);
    std::printf("    DMB: lock or + plain clear %6.2f ns (rejected)\n", lockor_clear);
    std::printf("    STLR: xchg;load            %6.2f ns (before ABI 11)\n", xchg);
    std::printf("    STLR: store;flag;load      %6.2f ns\n", stlr_flag);
    std::printf("    LDAR: load;check;load      %6.2f ns (flag clear)\n", ldar_check);
    std::printf("    STLR;LDAR: store;flag;check+fence;load %6.2f ns\n", stlr_ldar);
    std::printf("    lock xadd;load             %6.2f ns\n", lockadd);
    FX_CHECK(base > 0);
}

FX_TEST(bench_atomics_scaling_vs_native) {
    Buffer memory(64 * 1024);
    constexpr std::uint64_t kShared = 200000, kPrivate = 500000;
    auto shared = make_variants({{"native InterlockedIncrement64", native_increment, {}, true},
                                 {"fx LL/SC fused", nullptr, llsc_increment(), true},
                                 {"fx LL/SC unfused", nullptr, llsc_increment(), false},
                                 {"fx LSE ldaddal", nullptr, lse_increment(), true}});
    measure("shared counter", shared, true, kShared, memory);
    auto priv = make_variants({{"native InterlockedIncrement64", native_increment, {}, true},
                               {"fx LL/SC fused", nullptr, llsc_increment(), true},
                               {"fx LL/SC unfused", nullptr, llsc_increment(), false},
                               {"fx LSE ldaddal", nullptr, lse_increment(), true}});
    measure("private counters", priv, false, kPrivate, memory);
    auto cas = make_variants({{"native CAS64 increment", native_cas_increment, {}, true},
                              {"fx LL/SC CAS fused", nullptr, llsc_cas_increment(), true},
                              {"fx LL/SC CAS unfused", nullptr, llsc_cas_increment(), false},
                              {"fx LSE casal", nullptr, lse_cas_increment(), true}});
    measure("CAS increment, shared counter", cas, true, kShared, memory);
    FX_CHECK(priv[1].best[0] > 0);
}
