// Cross-check through the real translator core (owner: atomics/system):
// LL/SC, LSE and CAS programs translated by translate_region()/RegionEmitter
// and run on 1/2/4/8 threads by the core's test dispatcher, with exact final
// counts, no interpreter fallbacks, and ns/op against native x64 atomics.
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/emit_system.hpp"
#include "fx/translator/emit_system_test_util.hpp"
#include "fx/translator/translator.hpp"

#include "fx/testing/test.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::systest;
using testing::Buffer;

namespace {

constexpr unsigned kThreads[] = {1, 2, 4, 8};

// A guest program in host memory, translated by one shared TestMachine.
class CoreProgram {
public:
    explicit CoreProgram(const std::vector<std::uint32_t>& words, bool fused = true)
        : fused_(fused), code_(4096 + words.size() * 4 + 64), source_(code_.address(), code_.address() + code_.size()),
          machine_(TranslateOptions{}, std::size_t{4} << 20) {
        std::memcpy(code_.data(), words.data(), words.size() * 4);
        const std::uint32_t brk = 0xd4200000u | (0xdeadu << 5);   // ends the region, as DiffRunner does
        std::memcpy(code_.data() + words.size() * 4, &brk, 4);
        end_ = code_.address() + words.size() * 4;
        static const bool registered = [] { register_system_emitters(); return true; }();
        (void)registered;
        for (unsigned id = helper_system_first; id < helper_abi_first; ++id) machine_.set_helper(id, system_helper(id));
    }
    // Single-threaded run (translates and links on first use, with this
    // program's fusion setting).
    coretest::RunResult run(GuestContext& ctx) {
        ctx.pc = code_.address();
        system_set_fusion(fused_);
        const auto r = machine_.run(ctx, source_, end_);
        system_set_fusion(true);
        return r;
    }
    // Warm-up translation must have happened; runs are then lookup-only.
    double run_threads(unsigned threads, const std::function<void(unsigned, GuestContext&)>& init,
                       std::vector<coretest::RunResult>& results) {
        std::vector<GuestContext> contexts(threads);
        results.assign(threads, {});
        for (unsigned t = 0; t < threads; ++t) {
            contexts[t] = coretest::test_context(t + 1);
            init(t, contexts[t]);
            contexts[t].pc = code_.address();
        }
        return systest::run_threads(threads, [&](unsigned t) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            results[t] = machine_.run(contexts[t], source_, end_);
        });
    }
    [[nodiscard]] const coretest::TestMachine& machine() const { return machine_; }

private:
    bool fused_;
    Buffer code_;
    coretest::BufferCode source_;
    std::uint64_t end_{};
    coretest::TestMachine machine_;
};

std::vector<std::uint32_t> llsc_increment() {
    return {a64::ldxr(8, 8, 0, true), a64::add_imm(true, 8, 8, 1), a64::stxr(8, 9, 8, 0, true),
            a64::cbnz(false, 9, -3), a64::mov_reg(true, 1, 8), a64::kDmbIsh,
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -7)};
}
std::vector<std::uint32_t> lse_increment() {
    return {a64::movz(true, 1, 1), a64::lse(0, 8, true, true, 1, 8, 0), a64::sub_imm(true, 27, 27, 1),
            a64::cbnz(true, 27, -2)};
}
std::vector<std::uint32_t> llsc_cas_increment() {
    return {a64::ldr_x(1, 0),
            a64::add_imm(true, 2, 1, 1),
            a64::ldxr(8, 8, 0, true), a64::cmp_reg(true, 8, 1), a64::bcond(a64::NE, 3),
            a64::stxr(8, 9, 2, 0, true), a64::cbnz(false, 9, -4),
            a64::kDmbIsh,
            a64::cmp_reg(true, 8, 1), a64::mov_reg(true, 1, 8), a64::bcond(a64::NE, -9),
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -12)};
}
std::vector<std::uint32_t> lse_cas_increment() {
    return {a64::ldr_x(1, 0),
            a64::add_imm(true, 2, 1, 1),
            a64::mov_reg(true, 3, 1), a64::cas(8, true, true, 3, 2, 0),
            a64::cmp_reg(true, 3, 1), a64::mov_reg(true, 1, 3), a64::bcond(a64::NE, -5),
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -8)};
}
std::vector<std::uint32_t> pair_increment() {
    return {a64::ldxp(true, 8, 9, 0, true), a64::add_imm(true, 8, 8, 1), a64::add_imm(true, 9, 9, 2),
            a64::stxp(true, 10, 8, 9, 0, true), a64::cbnz(false, 10, -4),
            a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -6)};
}
std::vector<std::uint32_t> push_llsc() {
    return {a64::ldr_x(1, 0),
            a64::str_x(1, 2),
            a64::ldxr(8, 8, 0, true), a64::cmp_reg(true, 8, 1), a64::bcond(a64::NE, 3),
            a64::stxr(8, 9, 2, 0, true), a64::cbnz(false, 9, -4),
            a64::kDmbIsh,
            a64::cmp_reg(true, 8, 1), a64::mov_reg(true, 1, 8), a64::bcond(a64::NE, -9),
            a64::add_imm(true, 2, 2, 16), a64::sub_imm(true, 27, 27, 1), a64::cbnz(true, 27, -13)};
}
std::vector<std::uint32_t> pop_lse() {
    return {a64::ldr_x(1, 0),
            a64::cbz(true, 1, 10),
            a64::ldr_x(2, 1), a64::mov_reg(true, 3, 1), a64::cas(8, true, true, 3, 2, 0),
            a64::cmp_reg(true, 3, 1), a64::mov_reg(true, 1, 3), a64::bcond(a64::NE, -6),
            a64::str_x(1, 5), a64::add_imm(true, 5, 5, 8), a64::b(-10)};
}

void check_results(const std::vector<coretest::RunResult>& results, const char* what) {
    for (const auto& r : results) {
        if (!r.ok) testing::report_failure(__FILE__, __LINE__, std::string(what) + ": run failed: " + r.error);
        if (r.interprets != 0)
            testing::report_failure(__FILE__, __LINE__, std::string(what) + ": " + std::to_string(r.interprets) +
                                    " interpreter fallbacks (non-atomic)");
    }
}

} // namespace

FX_TEST(real_core_counters_exact) {
    Buffer shared(64 * 1024);
    constexpr std::uint64_t kIters = 100000;
    struct Case { const char* name; std::vector<std::uint32_t> words; bool fused; bool private_counter; };
    const Case cases[] = {
        {"llsc fused shared", llsc_increment(), true, false},
        {"llsc unfused shared", llsc_increment(), false, false},
        {"lse shared", lse_increment(), true, false},
        {"llsc cas fused shared", llsc_cas_increment(), true, false},
        {"llsc cas unfused shared", llsc_cas_increment(), false, false},
        {"lse cas shared", lse_cas_increment(), true, false},
        {"llsc fused private", llsc_increment(), true, true},
    };
    for (const auto& c : cases) {
        CoreProgram program(c.words, c.fused);
        auto warm = coretest::test_context(99);
        warm.x[0] = shared.address() + 32768;
        warm.x[27] = 3;
        FX_CHECK(program.run(warm).ok);
        for (unsigned threads : kThreads) {
            std::memset(shared.data(), 0, 4096);
            std::vector<coretest::RunResult> results;
            program.run_threads(threads, [&](unsigned t, GuestContext& ctx) {
                ctx.x[0] = shared.address() + (c.private_counter ? 256 * t : 0);
                ctx.x[27] = kIters;
            }, results);
            check_results(results, c.name);
            if (c.private_counter) {
                for (unsigned t = 0; t < threads; ++t)
                    FX_CHECK_EQ(*reinterpret_cast<std::uint64_t*>(shared.data() + 256 * t), kIters);
            } else {
                FX_CHECK_EQ(*reinterpret_cast<std::uint64_t*>(shared.data()), kIters * threads);
            }
        }
    }
}

FX_TEST(real_core_pairs_and_stack) {
    Buffer shared(4096);
    {
        CoreProgram program(pair_increment());
        auto warm = coretest::test_context(5);
        warm.x[0] = shared.address() + 1024;
        warm.x[27] = 2;
        FX_CHECK(program.run(warm).ok);
        for (unsigned threads : kThreads) {
            std::memset(shared.data(), 0, 64);
            std::vector<coretest::RunResult> results;
            program.run_threads(threads, [&](unsigned, GuestContext& ctx) {
                ctx.x[0] = shared.address();
                ctx.x[27] = 20000;
            }, results);
            check_results(results, "pair");
            FX_CHECK_EQ(*reinterpret_cast<std::uint64_t*>(shared.data()), std::uint64_t{20000} * threads);
            FX_CHECK_EQ(*reinterpret_cast<std::uint64_t*>(shared.data() + 8), std::uint64_t{40000} * threads);
        }
    }
    constexpr unsigned kNodes = 10000;
    CoreProgram push(push_llsc());
    CoreProgram pop(pop_lse());
    {   // warm-up: translate single-threaded (the test dispatcher is not thread-safe on misses)
        Buffer scratch(4096);
        auto ctx = coretest::test_context(3);
        ctx.x[0] = scratch.address();
        ctx.x[2] = scratch.address() + 64;
        ctx.x[27] = 1;
        FX_CHECK(push.run(ctx).ok);
        ctx = coretest::test_context(4);
        ctx.x[0] = scratch.address();
        ctx.x[5] = scratch.address() + 1024;
        FX_CHECK(pop.run(ctx).ok);
    }
    for (unsigned threads : kThreads) {
        Buffer head(4096);
        Buffer nodes(std::size_t{16} * kNodes * threads + 4096);
        Buffer out(std::size_t{8} * kNodes * threads * threads + 4096);
        std::vector<coretest::RunResult> results;
        push.run_threads(threads, [&](unsigned t, GuestContext& ctx) {
            ctx.x[0] = head.address();
            ctx.x[2] = nodes.address() + std::uint64_t{16} * kNodes * t;
            ctx.x[27] = kNodes;
        }, results);
        check_results(results, "push");
        std::set<std::uint64_t> seen;
        for (std::uint64_t p = *reinterpret_cast<std::uint64_t*>(head.data()); p && seen.size() <= kNodes * threads;
             p = *reinterpret_cast<std::uint64_t*>(p)) {
            FX_REQUIRE(seen.insert(p).second);
        }
        FX_CHECK_EQ(seen.size(), std::size_t{kNodes} * threads);
        pop.run_threads(threads, [&](unsigned t, GuestContext& ctx) {
            ctx.x[0] = head.address();
            ctx.x[5] = out.address() + std::uint64_t{8} * kNodes * threads * t;
        }, results);
        check_results(results, "pop");
        FX_CHECK_EQ(*reinterpret_cast<std::uint64_t*>(head.data()), std::uint64_t{0});
    }
}

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

} // namespace

FX_TEST(bench_real_core_scaling_vs_native) {
    Buffer memory(64 * 1024);
    constexpr std::uint64_t kIters = 200000;
    struct Variant { const char* name; void (*native)(volatile long long*, std::uint64_t);
                     std::unique_ptr<CoreProgram> program; double best[4]; };
    const auto make = [](const char* name, void (*native)(volatile long long*, std::uint64_t),
                         std::vector<std::uint32_t> words, bool fused) {
        Variant v{name, native, nullptr, {1e30, 1e30, 1e30, 1e30}};
        if (!native) v.program = std::make_unique<CoreProgram>(words, fused);
        return v;
    };
    const auto measure = [&](const char* title, std::vector<Variant>& variants, bool shared_counter) {
        for (auto& v : variants) {
            if (!v.program) continue;
            auto warm = coretest::test_context(1);
            warm.x[0] = memory.address() + 32768;
            warm.x[27] = 2;
            v.program->run(warm);
        }
        for (unsigned i = 0; i < 4; ++i) {
            for (int round = 0; round < 7; ++round) {
                for (auto& v : variants) {
                    std::memset(memory.data(), 0, 4096);
                    double s;
                    if (v.native) {
                        s = systest::run_threads(kThreads[i], [&](unsigned t) {
                            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                            v.native(reinterpret_cast<volatile long long*>(memory.data() + (shared_counter ? 0 : 256 * t)), kIters);
                        });
                    } else {
                        std::vector<coretest::RunResult> results;
                        s = v.program->run_threads(kThreads[i], [&](unsigned t, GuestContext& ctx) {
                            ctx.x[0] = memory.address() + (shared_counter ? 0 : 256 * t);
                            ctx.x[27] = kIters;
                        }, results);
                        check_results(results, v.name);
                    }
                    v.best[i] = std::min(v.best[i], s * 1e9 / double(kIters));
                }
            }
        }
        std::printf("    real core, %s: ns per op per thread (min of 7 interleaved rounds) [x native]\n", title);
        std::printf("    %-24s %15s %15s %15s %15s\n", "", "1T", "2T", "4T", "8T");
        for (const auto& v : variants) {
            std::printf("    %-24s", v.name);
            for (unsigned i = 0; i < 4; ++i) std::printf("  %7.2f [%4.2f]", v.best[i], v.best[i] / variants[0].best[i]);
            std::printf("\n");
        }
    };
    std::vector<Variant> shared;
    shared.push_back(make("native Increment64", native_increment, {}, true));
    shared.push_back(make("LL/SC fused", nullptr, llsc_increment(), true));
    shared.push_back(make("LL/SC unfused", nullptr, llsc_increment(), false));
    shared.push_back(make("LSE ldaddal", nullptr, lse_increment(), true));
    measure("shared counter", shared, true);
    std::vector<Variant> priv;
    priv.push_back(make("native Increment64", native_increment, {}, true));
    priv.push_back(make("LL/SC fused", nullptr, llsc_increment(), true));
    priv.push_back(make("LSE ldaddal", nullptr, lse_increment(), true));
    measure("private counters", priv, false);
    std::vector<Variant> cas;
    cas.push_back(make("native CAS64 loop", native_cas_increment, {}, true));
    cas.push_back(make("LL/SC CAS fused", nullptr, llsc_cas_increment(), true));
    cas.push_back(make("LL/SC CAS unfused", nullptr, llsc_cas_increment(), false));
    cas.push_back(make("LSE casal", nullptr, lse_cas_increment(), true));
    measure("CAS increment, shared", cas, true);
    FX_CHECK(shared[0].best[0] > 0);
}

// ---- memory-model litmus tests -------------------------------------------------------
//
// Two guest threads run in lockstep (a fused LL/SC barrier, i.e. one locked
// instruction, at the start of every iteration) on fresh locations per
// iteration and record what they read. Store buffering (SB, Dekker) and
// message passing (MP) shapes whose relaxed outcome ARM forbids must never
// show it; the shapes ARM allows are reported to show the harness sees x64
// store->load reordering at all.

namespace {

constexpr std::uint32_t kAdrX9Plus8 = 0x10000049u;   // adr x9, .+8
constexpr std::uint32_t kBrX9 = 0xd61f0120u;         // br x9 (ends the region)
constexpr unsigned kLo = 3;                          // b.lo

// x0/x1 = store/load cursors (8-byte slots), x2 = result cursor (16-byte
// slots), x3 = &arrival counter, x4 = role (0 = A, 1 = B), x5 = 1, x12 = 0,
// x27 = iterations.
std::vector<std::uint32_t> litmus_program(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> w = {
        a64::ldxr(8, 9, 3), a64::add_imm(true, 9, 9, 1), a64::stxr(8, 10, 9, 3), a64::cbnz(false, 10, -3),
        a64::add_imm(true, 12, 12, 2),
        a64::ldr_x(9, 3), a64::cmp_reg(true, 9, 12), a64::bcond(kLo, -2),   // wait for both threads
    };
    w.push_back(a64::cbnz(true, 4, static_cast<int>(a.size()) + 2));
    w.insert(w.end(), a.begin(), a.end());
    w.push_back(a64::b(static_cast<int>(b.size()) + 1));
    w.insert(w.end(), b.begin(), b.end());
    w.insert(w.end(), {a64::add_imm(true, 0, 0, 8), a64::add_imm(true, 1, 1, 8), a64::add_imm(true, 2, 2, 16),
                       a64::sub_imm(true, 27, 27, 1)});
    w.push_back(a64::cbnz(true, 27, -static_cast<int>(w.size())));
    return w;
}

struct LitmusArrays {
    explicit LitmusArrays(std::uint64_t n)
        : n(n), x(n * 8 + 64), y(n * 8 + 64), r0(n * 16 + 64), r1(n * 16 + 64), counter(4096) {}
    void clear() {
        for (auto* b : {&x, &y, &r0, &r1, &counter}) std::memset(b->data(), 0, b->size());
    }
    std::uint64_t n;
    Buffer x, y, r0, r1, counter;
};

enum class Shape : std::uint8_t { sb, mp };

// Runs the program; returns the number of forbidden-shape outcomes: SB: both
// threads read 0; MP: B saw the flag (slot 0) but not the data (slot 1).
std::uint64_t run_litmus(const char* name, Shape shape, const std::vector<std::uint32_t>& a,
                         const std::vector<std::uint32_t>& b, LitmusArrays& arrays, std::uint64_t n, bool legacy) {
    const auto words = litmus_program(a, b);
    CoreProgram program(words);
    {   // translate single-threaded, with this variant's ordering mode
        LegacyOrderingScope scope(legacy);
        LitmusArrays warm_arrays(4);
        warm_arrays.clear();
        for (unsigned role = 0; role < 2; ++role) {
            auto warm = coretest::test_context(40 + role);
            warm.x[0] = warm_arrays.x.address(); warm.x[1] = warm_arrays.y.address();
            warm.x[2] = warm_arrays.r0.address(); warm.x[3] = warm_arrays.counter.address();
            warm.x[4] = role; warm.x[5] = 1; warm.x[12] = 0; warm.x[27] = 1;
            std::memset(warm_arrays.counter.data(), 0, 8);
            *reinterpret_cast<std::uint64_t*>(warm_arrays.counter.data()) = 1;   // the other thread "arrived"
            const auto r = program.run(warm);
            if (!r.ok) testing::report_failure(__FILE__, __LINE__, std::string(name) + ": warm-up failed: " + r.error);
        }
    }
    arrays.clear();
    std::vector<coretest::RunResult> results;
    program.run_threads(2, [&](unsigned t, GuestContext& ctx) {
        const bool sb = shape == Shape::sb;
        // SB: A stores X, loads Y; B stores Y, loads X. MP: both use X = data, Y = flag.
        ctx.x[0] = sb && t == 1 ? arrays.y.address() : arrays.x.address();
        ctx.x[1] = sb && t == 1 ? arrays.x.address() : arrays.y.address();
        ctx.x[2] = t == 0 ? arrays.r0.address() : arrays.r1.address();
        ctx.x[3] = arrays.counter.address();
        ctx.x[4] = t;
        ctx.x[5] = 1;
        ctx.x[12] = 0;
        ctx.x[27] = n;
        ctx.release_pending = 0;
    }, results);
    check_results(results, name);
    const auto* r0 = reinterpret_cast<const std::uint64_t*>(arrays.r0.data());
    const auto* r1 = reinterpret_cast<const std::uint64_t*>(arrays.r1.data());
    std::uint64_t forbidden = 0, seen_one = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        if (shape == Shape::sb) {
            forbidden += r0[2 * i] == 0 && r1[2 * i] == 0;
            seen_one += r0[2 * i] == 1 && r1[2 * i] == 1;
        } else {
            forbidden += r1[2 * i] == 1 && r1[2 * i + 1] == 0;
            seen_one += r1[2 * i] == 1;
        }
    }
    std::printf("    %-44s %8llu iterations: relaxed outcome %7llu (%5.2f%%)%s, %s %llu\n", name,
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(forbidden),
                100.0 * double(forbidden) / double(n), legacy ? " [legacy]" : "",
                shape == Shape::sb ? "both saw 1:" : "flag seen:", static_cast<unsigned long long>(seen_one));
    return forbidden;
}

} // namespace

FX_TEST(litmus_store_buffering_and_message_passing) {
    const char* env = std::getenv("FX_LITMUS_ITERATIONS");
    const std::uint64_t n = env ? std::strtoull(env, nullptr, 10) : 1'000'000;
    LitmusArrays arrays(n);
    const std::uint32_t store_x5 = a64::str_x(5, 0), load_x6 = a64::ldr_x(6, 1), save_x6 = a64::str_x(6, 2);
    const std::uint32_t stlr = a64::stlr(8, 5, 0), ldar = a64::ldar(8, 6, 1), ldapr = a64::ldapr(8, 6, 1);
    const std::uint32_t dummy = a64::str_x(5, 2, 8);
    struct Sb { const char* name; std::vector<std::uint32_t> body; bool allowed; bool legacy; };
    const Sb sb[] = {
        {"SB stlr; ldar (same span: xchg)", {stlr, ldar, save_x6}, false, false},
        {"SB stlr; b .+4; ldar (flag)", {stlr, a64::b(1), ldar, save_x6}, false, false},
        {"SB stlr; ldar (legacy xchg)", {stlr, ldar, save_x6}, false, true},
        {"SB stlr; br; ldar (next region)", {stlr, kAdrX9Plus8, kBrX9, ldar, save_x6}, false, false},
        {"SB stlr; ldaxr; clrex", {stlr, a64::ldxr(8, 6, 1, true), a64::kClrex, save_x6}, false, false},
        {"SB stlr; dmb ish; ldr", {stlr, a64::kDmbIsh, load_x6, save_x6}, false, false},
        {"SB str; dmb ish; ldr", {store_x5, a64::kDmbIsh, load_x6, save_x6}, false, false},
        {"SB str; dmb sy; str; dmb ish; ldr (1st elided)", {store_x5, a64::kDmbSy, dummy, a64::kDmbIsh, load_x6, save_x6},
         false, false},
        {"SB str; dsb ishst; ldr", {store_x5, 0xd5033a9fu, load_x6, save_x6}, false, false},
        {"SB stlr; ldapr (RCpc: allowed)", {stlr, ldapr, save_x6}, true, false},
        {"SB str; ldr (allowed)", {store_x5, load_x6, save_x6}, true, false},
        {"SB stlr; dmb ishld; ldr (allowed)", {stlr, a64::kDmbIshld, load_x6, save_x6}, true, false},
    };
    std::uint64_t allowed_seen = 0;
    for (const auto& c : sb) {
        const auto count = run_litmus(c.name, Shape::sb, c.body, c.body, arrays, n, c.legacy);
        if (c.allowed) allowed_seen += count;
        else if (count != 0)
            testing::report_failure(__FILE__, __LINE__, std::string(c.name) + ": forbidden outcome observed " +
                                    std::to_string(count) + " times");
    }
    if (allowed_seen == 0)
        std::printf("    WARNING: no store->load reordering observed in the allowed SB shapes (harness insensitive?)\n");
    // MP: A = writer (data X, flag Y), B = reader (flag into slot 0, data into slot 1).
    const std::uint32_t store_flag = a64::str_x(5, 1), load_flag = a64::ldr_x(6, 1), load_data = a64::ldr_x(7, 0);
    const std::vector<std::uint32_t> save = {a64::str_x(6, 2), a64::str_x(7, 2, 8)};
    struct Mp { const char* name; std::vector<std::uint32_t> writer, reader; };
    const auto reader = [&](std::initializer_list<std::uint32_t> head) {
        std::vector<std::uint32_t> r(head);
        r.insert(r.end(), save.begin(), save.end());
        return r;
    };
    const Mp mp[] = {
        {"MP str; dmb ishst; str / ldr; dmb ishld; ldr", {store_x5, a64::kDmbIshst, store_flag},
         reader({load_flag, a64::kDmbIshld, load_data})},
        {"MP str; stlr / ldar; ldr", {store_x5, a64::stlr(8, 5, 1)}, reader({a64::ldar(8, 6, 1), load_data})},
        {"MP str; stlr / ldapr; ldr", {store_x5, a64::stlr(8, 5, 1)}, reader({a64::ldapr(8, 6, 1), load_data})},
        {"MP str; dmb ish; str / ldr; dmb ish; ldr", {store_x5, a64::kDmbIsh, store_flag},
         reader({load_flag, a64::kDmbIsh, load_data})},
    };
    for (const auto& c : mp) {
        const auto count = run_litmus(c.name, Shape::mp, c.writer, c.reader, arrays, n, false);
        if (count != 0)
            testing::report_failure(__FILE__, __LINE__, std::string(c.name) + ": forbidden outcome observed " +
                                    std::to_string(count) + " times");
    }
}
