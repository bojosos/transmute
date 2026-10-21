// Atomicity of the one-instruction interpreter fallback (owner: integration).
//
// Every LSE family, CAS, CASP and the exclusive (pair) forms run on many host
// threads, each with its own GuestContext, through interpret_instruction on
// one shared word, and the results are checked for lost updates. The mixed
// tests run translated (atomics/system family) and interpreted code on the
// same lock word. A single-threaded differential checks that the native
// paths compute exactly what the reference engine computes.

#include "fx/common/interpreter.hpp"
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/emit_system.hpp"
#include "transmute/a64.hpp"

#include <windows.h>

#include <algorithm>
#include <barrier>
#include <cstring>
#include <functional>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace transmute;
using namespace transmute::fx;
using testing::Buffer;

namespace {

// ---- A64 encoders ---------------------------------------------------------------
std::uint32_t sz(unsigned bytes) { return bytes == 1 ? 0u : bytes == 2 ? 1u : bytes == 4 ? 2u : 3u; }
// opc: 0 ADD 1 CLR 2 EOR 3 SET 4 SMAX 5 SMIN 6 UMAX 7 UMIN, 8 = SWP
std::uint32_t lse(unsigned opc, unsigned bytes, bool acq, bool rel, unsigned rs, unsigned rt, unsigned rn) {
    const unsigned o3 = opc == 8 ? 1u : 0u, op = opc == 8 ? 0u : opc;
    return (sz(bytes) << 30) | 0x38200000u | (acq ? 1u << 23 : 0u) | (rel ? 1u << 22 : 0u) | (rs << 16) |
           (o3 << 15) | (op << 12) | (rn << 5) | rt;
}
std::uint32_t cas(unsigned bytes, bool acq, bool rel, unsigned rs, unsigned rt, unsigned rn) {
    return (sz(bytes) << 30) | 0x08a07c00u | (acq ? 1u << 22 : 0u) | (rs << 16) | (rel ? 0x8000u : 0u) |
           (rn << 5) | rt;
}
std::uint32_t casp(bool wide, bool acq, bool rel, unsigned rs, unsigned rt, unsigned rn) {
    return (wide ? 0x40000000u : 0u) | 0x08207c00u | (acq ? 1u << 22 : 0u) | (rs << 16) | (rel ? 0x8000u : 0u) |
           (rn << 5) | rt;
}
std::uint32_t ldxr(unsigned bytes, unsigned rt, unsigned rn, bool acquire) {
    return (sz(bytes) << 30) | 0x085f7c00u | (acquire ? 0x8000u : 0u) | (rn << 5) | rt;
}
std::uint32_t stxr(unsigned bytes, unsigned rs, unsigned rt, unsigned rn, bool release) {
    return (sz(bytes) << 30) | 0x08007c00u | (rs << 16) | (release ? 0x8000u : 0u) | (rn << 5) | rt;
}
std::uint32_t ldxp(bool wide, unsigned rt, unsigned rt2, unsigned rn, bool acquire) {
    return 0x887f0000u | (wide ? 0x40000000u : 0u) | (acquire ? 0x8000u : 0u) | (rt2 << 10) | (rn << 5) | rt;
}
std::uint32_t stxp(bool wide, unsigned rs, unsigned rt, unsigned rt2, unsigned rn, bool release) {
    return 0x88200000u | (wide ? 0x40000000u : 0u) | (rs << 16) | (release ? 0x8000u : 0u) | (rt2 << 10) |
           (rn << 5) | rt;
}
constexpr std::uint32_t kClrex = 0xd5033f5fu;

constexpr std::uint64_t mask_of(unsigned bytes) {
    return bytes == 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (bytes * 8)) - 1;
}

const char* kOpNames[] = {"ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin", "swp"};

unsigned thread_count() {
    return std::clamp(std::thread::hardware_concurrency() * 2u, 8u, 16u);
}

// Owns one GuestContext per thread (alignas(64), several KiB each).
struct Contexts {
    explicit Contexts(unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            ctx.push_back(std::make_unique<GuestContext>());
            ctx.back()->pc = 0x0000'0001'4000'1000ULL;
        }
    }
    GuestContext& operator[](unsigned i) { return *ctx[i]; }
    std::vector<std::unique_ptr<GuestContext>> ctx;
};

// Runs body(t) on `threads` host threads released together.
void run_threads(unsigned threads, const std::function<void(unsigned)>& body) {
    std::latch start(threads + 1);
    std::vector<std::thread> pool;
    std::vector<std::exception_ptr> errors(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            start.arrive_and_wait();
            try {
                body(t);
            } catch (...) {
                errors[t] = std::current_exception();
            }
        });
    }
    start.arrive_and_wait();
    for (auto& thread : pool) thread.join();
    for (auto& error : errors)
        if (error) std::rethrow_exception(error);
}

// One interpreted instruction. The pc is irrelevant to these instructions.
void step(GuestContext& ctx, std::uint32_t word) {
    interpret_instruction(ctx, word);
}

std::uint64_t load(std::uint64_t address, unsigned bytes) {
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(address), bytes);
    return v;
}
void store(std::uint64_t address, unsigned bytes, std::uint64_t value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, bytes);
}

// Translated code for `words` (atomics/system family, MemoryEmitContext),
// assembled once and run many times.
class Translated {
public:
    explicit Translated(std::vector<std::uint32_t> words) : count_(words.size()) {
        x64::Assembler a;
        a.sub(64, x64::Gp::rsp, 40);
        testing::MemoryEmitContext emit(a, 0x0000'0001'4000'1000ULL);
        std::size_t index = 0;
        while (index < words.size()) {
            emit.begin_instruction(0x0000'0001'4000'1000ULL + index * 4);
            const auto consumed = emit_system(emit, std::span<const std::uint32_t>(words).subspan(index));
            if (consumed == 0) throw std::runtime_error("system family declined an instruction");
            emit.end_instruction();
            index += consumed;
        }
        a.add(64, x64::Gp::rsp, 40);
        a.ret();
        a.finalize();
        snippet_ = std::make_unique<testing::Snippet>(a.bytes());
    }
    void run(GuestContext& ctx) const {
        snippet_->run(ctx);
        ctx.pc += count_ * 4;
    }

private:
    std::size_t count_;
    std::unique_ptr<testing::Snippet> snippet_;
};

// Reference engine on direct memory, as the interpreter used to execute
// atomics (byte-wise, single-threaded semantics).
class ReferenceDirect final : public ReferenceEngine {
public:
    ReferenceDirect() : ReferenceEngine(ExecutionStart{0}) {}
    std::uint32_t read32(std::uint64_t a) const override { std::uint32_t v; std::memcpy(&v, p(a), 4); return v; }
    std::uint64_t read64(std::uint64_t a) const override { std::uint64_t v; std::memcpy(&v, p(a), 8); return v; }
    void write32(std::uint64_t a, std::uint32_t v) override { std::memcpy(p(a), &v, 4); }
    void write64(std::uint64_t a, std::uint64_t v) override { std::memcpy(p(a), &v, 8); }
    std::optional<RegionInfo> query_region(std::uint64_t a) const override {
        return RegionInfo{a & ~0xfffULL, 0x1000, true, true, false, false, true};
    }

protected:
    std::uint8_t read8(std::uint64_t a) const override { return *static_cast<const std::uint8_t*>(p(a)); }
    void write8(std::uint64_t a, std::uint8_t v) override { *static_cast<std::uint8_t*>(p(a)) = v; }

private:
    static void* p(std::uint64_t a) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)); }
};

GuestContext reference_step(const GuestContext& in, std::uint32_t word) {
    static ReferenceDirect engine;
    CpuState state{};
    context_to_cpu_state(in, state);
    engine.step_external(state, word);
    GuestContext out = in;
    cpu_state_to_context(state, out);
    return out;
}

// ---- LSE families --------------------------------------------------------------

// ADD: per-thread +1 counters; the multiset of returned old values must be
// exactly {0 .. total-1} (mod 2^bits).
void stress_add(unsigned bytes) {
    const unsigned threads = thread_count();
    const unsigned iterations = 20000;
    Buffer shared(4096);
    const std::uint64_t address = shared.address() + 64;
    Contexts contexts(threads);
    const std::uint64_t buckets = bytes == 1 ? 256 : 4096;
    std::vector<std::vector<std::uint32_t>> histograms(threads, std::vector<std::uint32_t>(buckets));
    run_threads(threads, [&](unsigned t) {
        GuestContext& ctx = contexts[t];
        ctx.x[9] = address;
        ctx.x[10] = 1;
        for (unsigned i = 0; i < iterations; ++i) {
            step(ctx, lse(0, bytes, i & 1, i & 2, 10, 11, 9));
            ++histograms[t][ctx.x[11] % buckets];
        }
    });
    const std::uint64_t total = std::uint64_t{threads} * iterations;
    FX_CHECK_EQ(load(address, bytes), total & mask_of(bytes));
    // Each residue r (mod buckets) appears total/buckets times, +1 for r < total % buckets.
    bool exact = true;
    for (std::uint64_t r = 0; r < buckets; ++r) {
        std::uint64_t seen = 0;
        for (const auto& h : histograms) seen += h[r];
        if (seen != total / buckets + (r < total % buckets ? 1 : 0)) exact = false;
    }
    FX_CHECK(exact);
}

// CLR/SET/EOR: thread t owns bit t and toggles it; every old value must show
// the bit in the state this thread left it in. The final word is 0 again
// (toggled an even number of times), except bits no thread owns.
void stress_bits(unsigned op, unsigned bytes) {
    const unsigned threads = std::min(thread_count(), bytes * 8);
    const unsigned iterations = 20000;
    Buffer shared(4096);
    const std::uint64_t address = shared.address() + 128;
    const std::uint64_t untouched = bytes * 8 > threads ? mask_of(bytes) & ~((std::uint64_t{1} << threads) - 1) : 0;
    store(address, bytes, untouched);   // bits above the threads' bits stay set
    Contexts contexts(threads);
    std::vector<unsigned> violations(threads);
    run_threads(threads, [&](unsigned t) {
        GuestContext& ctx = contexts[t];
        ctx.x[9] = address;
        ctx.x[10] = std::uint64_t{1} << t;
        bool set = false;
        for (unsigned i = 0; i < iterations; ++i) {
            // op 3 (SET) alternates with op 1 (CLR); op 2 (EOR) toggles.
            const unsigned this_op = op == 2 ? 2u : (set ? 1u : 3u);
            step(ctx, lse(this_op, bytes, i & 1, i & 2, 10, 11, 9));
            const bool old_bit = (ctx.x[11] >> t) & 1;
            if (old_bit != set) ++violations[t];
            if ((ctx.x[11] & untouched) != untouched) ++violations[t];
            set = !set;
        }
    });
    unsigned total = 0;
    for (auto v : violations) total += v;
    FX_CHECK_EQ(total, 0u);
    FX_CHECK_EQ(load(address, bytes), untouched);
}

// SWP: every value swapped in is returned exactly once (by the next swap) or
// is the final value. Checked through the sum and sum of squares.
void stress_swap(unsigned bytes) {
    const unsigned threads = thread_count();
    const unsigned iterations = 20000;
    Buffer shared(4096);
    const std::uint64_t address = shared.address() + 192;
    Contexts contexts(threads);
    std::vector<std::uint64_t> in_sum(threads), in_sq(threads), out_sum(threads), out_sq(threads);
    run_threads(threads, [&](unsigned t) {
        GuestContext& ctx = contexts[t];
        ctx.x[9] = address;
        std::uint64_t state = t * 0x9e3779b97f4a7c15ULL + 7;
        for (unsigned i = 0; i < iterations; ++i) {
            const std::uint64_t v = (testing::random_u64(state) & mask_of(bytes) & 0xffffff) | 1;
            ctx.x[10] = v | (~mask_of(bytes) & 0xabcd000000000000ULL);   // bits above the size are ignored
            step(ctx, lse(8, bytes, i & 1, i & 2, 10, 11, 9));
            in_sum[t] += v;
            in_sq[t] += v * v;
            out_sum[t] += ctx.x[11];
            out_sq[t] += ctx.x[11] * ctx.x[11];
        }
    });
    std::uint64_t is = 0, iq = 0, os = 0, oq = 0;
    for (unsigned t = 0; t < threads; ++t) { is += in_sum[t]; iq += in_sq[t]; os += out_sum[t]; oq += out_sq[t]; }
    const std::uint64_t final_value = load(address, bytes);
    FX_CHECK_EQ(os + final_value, is);   // initial value is 0
    FX_CHECK_EQ(oq + final_value * final_value, iq);
}

// Min/max: in each round every thread applies a strictly monotonic sequence
// of distinct values; under atomicity the word is monotonic, so a thread's
// next old value is never "behind" the value it applied last. Signed ops use
// sequences that cross zero, so an unsigned comparison would be caught too.
void stress_minmax(unsigned op, unsigned bytes) {
    const unsigned threads = thread_count();
    const bool is_max = op == 4 || op == 6;
    const bool is_signed = op == 4 || op == 5;
    const std::uint64_t mask = mask_of(bytes);
    const std::uint64_t span = std::min<std::uint64_t>(mask, 1u << 16);   // distinct values per round
    const unsigned per_round = static_cast<unsigned>(span / threads);
    const std::uint64_t kmax = std::uint64_t{per_round} * threads;
    const unsigned rounds = std::max(4u, 40000u / per_round);
    Buffer shared(4096);
    const std::uint64_t address = shared.address() + 256;
    const std::uint64_t sign = std::uint64_t{1} << (bytes * 8 - 1);
    // Ordered key for comparisons in the op's domain (signed: flip the sign bit).
    const auto key = [&](std::uint64_t v) -> std::uint64_t { return is_signed ? (v ^ sign) & mask : v & mask; };
    // Step k (0 = start .. kmax) of the sequence, monotonic in the op's
    // direction. Signed keys are centred on zero, so values cross it.
    const std::uint64_t key_base = is_signed ? sign - kmax / 2 : (is_max ? 0 : mask - kmax);
    const auto value_of = [&](std::uint64_t k) -> std::uint64_t {
        const std::uint64_t k_key = is_max ? key_base + k : key_base + kmax - k;
        return (is_signed ? k_key ^ sign : k_key) & mask;
    };
    const auto behind = [&](std::uint64_t old_value, std::uint64_t applied) {
        return is_max ? key(old_value) < key(applied) : key(old_value) > key(applied);
    };
    Contexts contexts(threads);
    std::vector<unsigned> violations(threads);
    std::barrier sync(static_cast<std::ptrdiff_t>(threads));
    run_threads(threads, [&](unsigned t) {
        GuestContext& ctx = contexts[t];
        ctx.x[9] = address;
        for (unsigned round = 0; round < rounds; ++round) {
            if (t == 0) store(address, bytes, value_of(0));
            sync.arrive_and_wait();
            std::uint64_t applied = value_of(0);
            for (unsigned i = 0; i < per_round; ++i) {
                const std::uint64_t v = value_of(1 + std::uint64_t{i} * threads + t);
                ctx.x[10] = v;
                step(ctx, lse(op, bytes, i & 1, i & 2, 10, 11, 9));
                if (behind(ctx.x[11] & mask, applied)) ++violations[t];
                applied = v;
            }
            sync.arrive_and_wait();
            if (t == 0) {
                const std::uint64_t expected = value_of(std::uint64_t{per_round} * threads);
                if ((load(address, bytes) & mask) != expected) ++violations[t];
            }
        }
    });
    unsigned total = 0;
    for (auto v : violations) total += v;
    FX_CHECK_EQ(total, 0u);
}

} // namespace

// The reported repro: 16 threads x 100000 interpreted `ldaddal x10,x11,[x9]`.
FX_TEST(interpreter_ldaddal_16_threads_exact) {
    constexpr unsigned kThreads = 16, kIterations = 100000;
    Buffer shared(4096);
    Contexts contexts(kThreads);
    run_threads(kThreads, [&](unsigned t) {
        GuestContext& ctx = contexts[t];
        ctx.x[9] = shared.address();
        ctx.x[10] = 1;
        for (unsigned i = 0; i < kIterations; ++i) step(ctx, lse(0, 8, true, true, 10, 11, 9));
    });
    FX_CHECK_EQ(load(shared.address(), 8), std::uint64_t{kThreads} * kIterations);
}

FX_TEST(interpreter_lse_add_all_sizes) {
    for (unsigned bytes : {1u, 2u, 4u, 8u}) stress_add(bytes);
}

FX_TEST(interpreter_lse_clr_set_eor_all_sizes) {
    for (unsigned op : {1u, 2u}) {   // 1: SET/CLR alternating, 2: EOR
        for (unsigned bytes : {1u, 2u, 4u, 8u}) stress_bits(op, bytes);
    }
}

FX_TEST(interpreter_lse_swp_all_sizes) {
    for (unsigned bytes : {1u, 2u, 4u, 8u}) stress_swap(bytes);
}

FX_TEST(interpreter_lse_min_max_all_sizes) {
    for (unsigned op : {4u, 5u, 6u, 7u}) {
        for (unsigned bytes : {1u, 2u, 4u, 8u}) stress_minmax(op, bytes);
    }
}

// CAS retry loops: old values returned by successful CAS form {0 .. total-1}.
FX_TEST(interpreter_cas_all_sizes) {
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        const unsigned threads = thread_count();
        const unsigned iterations = 10000;
        Buffer shared(4096);
        const std::uint64_t address = shared.address() + 320;
        Contexts contexts(threads);
        const std::uint64_t buckets = bytes == 1 ? 256 : 4096;
        std::vector<std::vector<std::uint32_t>> histograms(threads, std::vector<std::uint32_t>(buckets));
        run_threads(threads, [&](unsigned t) {
            GuestContext& ctx = contexts[t];
            ctx.x[9] = address;
            for (unsigned i = 0; i < iterations; ++i) {
                std::uint64_t seen = load(address, bytes);
                for (unsigned attempt = 0;; ++attempt) {
                    ctx.x[4] = seen;                                   // Rs: expected, receives old
                    ctx.x[5] = (seen + 1) & mask_of(bytes);            // Rt: desired
                    step(ctx, cas(bytes, attempt & 1, i & 1, 4, 5, 9));
                    if (ctx.x[4] == seen) break;
                    seen = ctx.x[4];
                }
                ++histograms[t][seen % buckets];
            }
        });
        const std::uint64_t total = std::uint64_t{threads} * iterations;
        FX_CHECK_EQ(load(address, bytes), total & mask_of(bytes));
        bool exact = true;
        for (std::uint64_t r = 0; r < buckets; ++r) {
            std::uint64_t seen = 0;
            for (const auto& h : histograms) seen += h[r];
            if (seen != total / buckets + (r < total % buckets ? 1 : 0)) exact = false;
        }
        FX_CHECK(exact);
    }
}

// CASP W and X: {lo += 1, hi += 2} together; every observed pair has hi == 2*lo.
FX_TEST(interpreter_casp_pairs) {
    for (bool wide : {false, true}) {
        const unsigned threads = thread_count();
        const unsigned iterations = 10000;
        Buffer shared(4096);
        const std::uint64_t address = shared.address() + 512;   // 16-byte aligned
        const unsigned element = wide ? 8 : 4;
        const std::uint64_t mask = mask_of(element);
        Contexts contexts(threads);
        std::vector<unsigned> torn(threads);
        run_threads(threads, [&](unsigned t) {
            GuestContext& ctx = contexts[t];
            ctx.x[9] = address;
            for (unsigned i = 0; i < iterations; ++i) {
                std::uint64_t lo = 0, hi = 0;   // a guess; the first CAS returns the real value
                for (unsigned attempt = 0;; ++attempt) {
                    ctx.x[2] = lo; ctx.x[3] = hi;                              // Rs, Rs+1: expected
                    ctx.x[6] = (lo + 1) & mask; ctx.x[7] = (hi + 2) & mask;    // Rt, Rt+1: desired
                    step(ctx, casp(wide, attempt & 1, i & 1, 2, 6, 9));
                    const std::uint64_t old_lo = ctx.x[2], old_hi = ctx.x[3];
                    if (old_hi != ((old_lo * 2) & mask)) ++torn[t];
                    if (old_lo == lo && old_hi == hi) break;
                    lo = old_lo;
                    hi = old_hi;
                }
            }
        });
        unsigned total_torn = 0;
        for (auto v : torn) total_torn += v;
        FX_CHECK_EQ(total_torn, 0u);
        const std::uint64_t total = std::uint64_t{threads} * iterations;
        FX_CHECK_EQ(load(address, element), total & mask);
        FX_CHECK_EQ(load(address + element, element), (2 * total) & mask);
    }
}

// LDAXR/STLXR increment loops, all sizes and orderings.
FX_TEST(interpreter_exclusive_increment_all_sizes) {
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        const unsigned threads = thread_count();
        const unsigned iterations = 10000;
        Buffer shared(4096);
        const std::uint64_t address = shared.address() + 640;
        Contexts contexts(threads);
        run_threads(threads, [&](unsigned t) {
            GuestContext& ctx = contexts[t];
            ctx.x[9] = address;
            for (unsigned i = 0; i < iterations; ++i) {
                do {
                    step(ctx, ldxr(bytes, 8, 9, i & 1));
                    ctx.x[8] = (ctx.x[8] + 1) & mask_of(bytes);
                    step(ctx, stxr(bytes, 10, 8, 9, i & 2));
                } while (ctx.x[10] != 0);
            }
        });
        FX_CHECK_EQ(load(address, bytes), (std::uint64_t{threads} * iterations) & mask_of(bytes));
    }
}

// LDAXP/STLXP {lo += 1, hi += 2}, W and X pairs; reads after a failed STXP
// may be torn, but every successful pair is consistent.
FX_TEST(interpreter_exclusive_pair_increment) {
    for (bool wide : {false, true}) {
        const unsigned threads = thread_count();
        const unsigned iterations = 10000;
        Buffer shared(4096);
        const std::uint64_t address = shared.address() + 768;
        const unsigned element = wide ? 8 : 4;
        const std::uint64_t mask = mask_of(element);
        Contexts contexts(threads);
        std::vector<unsigned> torn(threads);
        run_threads(threads, [&](unsigned t) {
            GuestContext& ctx = contexts[t];
            ctx.x[9] = address;
            for (unsigned i = 0; i < iterations; ++i) {
                std::uint64_t lo, hi;
                do {
                    step(ctx, ldxp(wide, 4, 5, 9, i & 1));
                    lo = ctx.x[4];
                    hi = ctx.x[5];
                    ctx.x[4] = (lo + 1) & mask;
                    ctx.x[5] = (hi + 2) & mask;
                    step(ctx, stxp(wide, 10, 4, 5, 9, i & 2));
                } while (ctx.x[10] != 0);
                if (hi != ((lo * 2) & mask)) ++torn[t];
            }
        });
        unsigned total_torn = 0;
        for (auto v : torn) total_torn += v;
        FX_CHECK_EQ(total_torn, 0u);
        const std::uint64_t total = std::uint64_t{threads} * iterations;
        FX_CHECK_EQ(load(address, element), total & mask);
        FX_CHECK_EQ(load(address + element, element), (2 * total) & mask);
    }
}

// Interpreted and translated code on the same lock word. Thread kinds:
// 0 fully translated, 1 fully interpreted, 2 translated LDAXR + interpreted
// STLXR, 3 interpreted LDAXR + translated STLXR (a reservation taken by one
// path is honoured by the other); LSE: half translated, half interpreted.
FX_TEST(interpreter_and_translated_share_lock_word) {
    const unsigned threads = std::max(8u, thread_count());
    const unsigned iterations = 10000;
    for (unsigned bytes : {4u, 8u}) {
        Buffer shared(4096);
        const std::uint64_t address = shared.address();
        const std::uint32_t load_word = ldxr(bytes, 8, 9, true), store_word = stxr(bytes, 10, 8, 9, true);
        const Translated t_load({load_word}), t_store({store_word});
        Contexts contexts(threads);
        run_threads(threads, [&](unsigned t) {
            GuestContext& ctx = contexts[t];
            ctx.x[9] = address;
            const unsigned kind = t % 4;
            for (unsigned i = 0; i < iterations; ++i) {
                do {
                    if (kind == 0 || kind == 2) t_load.run(ctx); else step(ctx, load_word);
                    ctx.x[8] = (ctx.x[8] + 1) & mask_of(bytes);
                    if (kind == 0 || kind == 3) t_store.run(ctx); else step(ctx, store_word);
                } while (ctx.x[10] != 0);
            }
        });
        FX_CHECK_EQ(load(address, bytes), (std::uint64_t{threads} * iterations) & mask_of(bytes));
    }
    {   // 128-bit pairs
        Buffer shared(4096);
        const std::uint64_t address = shared.address() + 256;
        const std::uint32_t load_word = ldxp(true, 4, 5, 9, true), store_word = stxp(true, 10, 4, 5, 9, true);
        const Translated t_load({load_word}), t_store({store_word});
        Contexts contexts(threads);
        run_threads(threads, [&](unsigned t) {
            GuestContext& ctx = contexts[t];
            ctx.x[9] = address;
            const unsigned kind = t % 4;
            for (unsigned i = 0; i < iterations; ++i) {
                do {
                    if (kind == 0 || kind == 2) t_load.run(ctx); else step(ctx, load_word);
                    ctx.x[4] += 1;
                    ctx.x[5] += 2;
                    if (kind == 0 || kind == 3) t_store.run(ctx); else step(ctx, store_word);
                } while (ctx.x[10] != 0);
            }
        });
        const std::uint64_t total = std::uint64_t{threads} * iterations;
        FX_CHECK_EQ(load(address, 8), total);
        FX_CHECK_EQ(load(address + 8, 8), 2 * total);
    }
    {   // LSE and CAS
        Buffer shared(4096);
        const std::uint64_t address = shared.address() + 512;
        const std::uint32_t add_word = lse(0, 8, true, true, 10, 11, 9);
        const std::uint32_t cas_word = cas(8, true, true, 4, 5, 9);
        const Translated t_add({add_word}), t_cas({cas_word});
        Contexts contexts(threads);
        run_threads(threads, [&](unsigned t) {
            GuestContext& ctx = contexts[t];
            ctx.x[9] = address;
            ctx.x[10] = 1;
            for (unsigned i = 0; i < iterations; ++i) {
                if (t % 4 == 0) t_add.run(ctx);
                else if (t % 4 == 1) step(ctx, add_word);
                else {
                    std::uint64_t seen = load(address, 8);
                    for (;;) {
                        ctx.x[4] = seen;
                        ctx.x[5] = seen + 1;
                        if (t % 4 == 2) t_cas.run(ctx); else step(ctx, cas_word);
                        if (ctx.x[4] == seen) break;
                        seen = ctx.x[4];
                    }
                }
            }
        });
        FX_CHECK_EQ(load(address, 8), std::uint64_t{threads} * iterations);
    }
}

// Monitor semantics match the translated path: a reservation lives in
// GuestContext::excl_*; STXR compares against the value LDXR read.
FX_TEST(interpreter_monitor_matches_translated_path) {
    Buffer buffer(4096);
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        auto ctx = std::make_unique<GuestContext>();
        ctx->x[1] = buffer.address() + 64;
        ctx->x[3] = 0x5566778899aabbccULL;
        std::memset(buffer.data(), 0x11, 128);
        step(*ctx, ldxr(bytes, 0, 1, true));
        FX_CHECK_EQ(ctx->excl_size, bytes);
        FX_CHECK_EQ(ctx->excl_address, ctx->x[1]);
        FX_CHECK_EQ(ctx->excl_value[0], 0x1111111111111111ULL & mask_of(bytes));
        // Another agent changes the value: the store fails, the reservation is gone.
        buffer.data()[64] ^= 0xff;
        step(*ctx, stxr(bytes, 2, 3, 1, true));
        FX_CHECK_EQ(ctx->x[2], 1u);
        FX_CHECK_EQ(ctx->excl_size, 0u);
        FX_CHECK_EQ(buffer.data()[64], std::uint8_t{0xee});
        // A reservation recorded by translated code is honoured by the interpreter.
        Translated translated_load({ldxr(bytes, 0, 1, false)});
        translated_load.run(*ctx);
        step(*ctx, stxr(bytes, 2, 3, 1, false));
        FX_CHECK_EQ(ctx->x[2], 0u);
        FX_CHECK_EQ(load(ctx->x[1], bytes), ctx->x[3] & mask_of(bytes));
        // And the other way round.
        step(*ctx, ldxr(bytes, 0, 1, false));
        Translated translated_store({stxr(bytes, 2, 0, 1, false)});
        translated_store.run(*ctx);
        FX_CHECK_EQ(ctx->x[2], 0u);
        // CLREX, mismatched size and address fail.
        step(*ctx, ldxr(bytes, 0, 1, false));
        step(*ctx, kClrex);
        FX_CHECK_EQ(ctx->excl_size, 0u);
        step(*ctx, stxr(bytes, 2, 3, 1, false));
        FX_CHECK_EQ(ctx->x[2], 1u);
        step(*ctx, ldxr(bytes, 0, 1, false));
        step(*ctx, stxr(bytes == 8 ? 4 : bytes * 2, 2, 3, 1, false));
        FX_CHECK_EQ(ctx->x[2], 1u);
        ctx->x[4] = ctx->x[1] + 8;
        step(*ctx, ldxr(bytes, 0, 1, false));
        step(*ctx, stxr(bytes, 2, 3, 4, false));
        FX_CHECK_EQ(ctx->x[2], 1u);
    }
}

// Single-threaded results of the native paths equal the reference engine's
// (registers incl. XZR/SP operands, memory), for every op, size and order.
FX_TEST(interpreter_atomics_match_reference_engine) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 11;
    struct Regs { unsigned rs, rt, rn; };
    const Regs combos[] = {{10, 11, 9}, {31, 11, 9}, {10, 31, 9}, {12, 12, 9}, {3, 4, 31}, {30, 0, 1}};
    const auto run_both = [&](std::uint32_t word, unsigned rn, std::uint64_t address, const std::string& label) {
        std::uint64_t state = seed++;
        GuestContext ctx = testing::random_context(state);
        ctx.x[rn] = address;
        std::vector<std::uint8_t> before(buffer.data(), buffer.data() + buffer.size());
        GuestContext expected = reference_step(ctx, word);
        std::vector<std::uint8_t> expected_memory(buffer.data(), buffer.data() + buffer.size());
        std::memcpy(buffer.data(), before.data(), before.size());
        GuestContext actual = ctx;
        FX_REQUIRE(testing::oracle_step(actual, word));
        testing::compare_contexts(expected, actual, label);
        FX_CHECK(std::memcmp(expected_memory.data(), buffer.data(), buffer.size()) == 0);
    };
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (unsigned order = 0; order < 4; ++order) {
            for (const auto& r : combos) {
                for (unsigned op = 0; op <= 8; ++op) {
                    for (int sample = 0; sample < 4; ++sample) {
                        std::uint64_t state = seed * 31 + sample;
                        for (std::size_t i = 0; i < buffer.size(); i += 8) {
                            const auto v = testing::random_u64(state);
                            std::memcpy(buffer.data() + i, &v, 8);
                        }
                        const std::uint64_t address = buffer.address() + 1024 + bytes * (seed % 16);
                        run_both(lse(op, bytes, order & 1, order & 2, r.rs, r.rt, r.rn), r.rn, address,
                                 std::string(kOpNames[op]) + " bytes=" + std::to_string(bytes));
                    }
                }
            }
        }
    }
    // CAS/CASP differential with the memory restored between the runs.
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (int sample = 0; sample < 32; ++sample) {
            std::uint64_t state = 1000 + sample * 7 + bytes;
            GuestContext ctx = testing::random_context(state);
            const std::uint64_t address = buffer.address() + 4096 + bytes * (sample % 8);
            const unsigned rs = sample % 3 == 0 ? 31u : 4u, rt = sample % 5 == 0 ? 31u : 5u;
            const unsigned rn = sample % 4 == 0 ? 31u : 9u;
            ctx.x[rn] = address;
            if (sample & 1) ctx.x[rs] = load(address, bytes) | (ctx.x[rs] & ~mask_of(bytes));
            const std::uint32_t word = cas(bytes, sample & 2, sample & 4, rs, rt, rn);
            std::vector<std::uint8_t> before(buffer.data(), buffer.data() + buffer.size());
            GuestContext expected = reference_step(ctx, word);
            std::vector<std::uint8_t> expected_memory(buffer.data(), buffer.data() + buffer.size());
            std::memcpy(buffer.data(), before.data(), before.size());
            GuestContext actual = ctx;
            FX_REQUIRE(testing::oracle_step(actual, word));
            testing::compare_contexts(expected, actual, "cas bytes=" + std::to_string(bytes));
            FX_CHECK(std::memcmp(expected_memory.data(), buffer.data(), buffer.size()) == 0);
        }
    }
    for (bool wide : {false, true}) {
        for (int sample = 0; sample < 32; ++sample) {
            std::uint64_t state = 5000 + sample * 13 + wide;
            GuestContext ctx = testing::random_context(state);
            const std::uint64_t address = buffer.address() + 8192 + 16 * (sample % 8);
            const unsigned rs = sample % 3 == 0 ? 30u : 2u, rt = sample % 5 == 0 ? 30u : 6u;
            const unsigned rn = sample % 4 == 0 ? 31u : 9u;
            ctx.x[rn] = address;
            const unsigned element = wide ? 8 : 4;
            if (sample & 1) {
                ctx.x[rs] = load(address, element);
                if (rs + 1 != 31) ctx.x[rs + 1] = load(address + element, element);
                else store(address + element, element, 0);   // XZR compares as zero
            }
            const std::uint32_t word = casp(wide, sample & 2, sample & 4, rs, rt, rn);
            std::vector<std::uint8_t> before(buffer.data(), buffer.data() + buffer.size());
            GuestContext expected = reference_step(ctx, word);
            std::vector<std::uint8_t> expected_memory(buffer.data(), buffer.data() + buffer.size());
            std::memcpy(buffer.data(), before.data(), before.size());
            GuestContext actual = ctx;
            FX_REQUIRE(testing::oracle_step(actual, word));
            testing::compare_contexts(expected, actual, "casp wide=" + std::to_string(wide));
            FX_CHECK(std::memcmp(expected_memory.data(), buffer.data(), buffer.size()) == 0);
        }
    }
    // Rejected encodings are still reported (ctx unchanged): unaligned LSE,
    // CAS and exclusives, odd CASP registers.
    {
        GuestContext ctx{};
        ctx.x[9] = buffer.address() + 1;
        const GuestContext saved = ctx;
        FX_CHECK(!testing::oracle_step(ctx, lse(0, 8, true, true, 10, 11, 9)));
        FX_CHECK(!testing::oracle_step(ctx, cas(4, false, false, 4, 5, 9)));
        FX_CHECK(!testing::oracle_step(ctx, ldxr(2, 4, 9, false)));
        FX_CHECK(!testing::oracle_step(ctx, stxr(8, 2, 4, 9, false)));
        ctx.x[9] = buffer.address();
        FX_CHECK(!testing::oracle_step(ctx, casp(true, false, false, 3, 6, 9)));
        ctx.x[9] = saved.x[9];
        FX_CHECK(std::memcmp(&ctx, &saved, sizeof(ctx)) == 0);
    }
}
