// Code arena reclaim: chunks whose regions are all dead are freed and reused,
// live chunks are evicted when the arena budget runs low, and a chunk that a
// thread still runs is kept until it has left; under concurrent execution,
// self-modifying code and reclaim passes, every call returns what its guest
// code says.

#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace rtt;
namespace rt = transmute::fx::rt;

namespace {

void init() {
    runtime_initialize(RuntimeOptions{});
    rt::reclaim_resume_for_tests();   // the process-exit tests stop the reclaimer
}

constexpr std::uint32_t kRet = 0xd65f03c0;
constexpr std::uint32_t movz(unsigned rd, std::uint32_t imm) { return 0xd2800000u | (imm << 5) | rd; }
constexpr std::size_t kPad = 256 * 1024;   // a mini region's int3 padding: 8 regions fill a chunk

std::uint64_t run(std::uint64_t pc) { return call_guest(pc, {}); }

// `n` functions `movz x0, #value(i); ret`, one per 16 bytes.
void write_functions(GuestCode& code, unsigned n, std::uint32_t (*value)(unsigned)) {
    for (unsigned i = 0; i < n; ++i) {
        code.words()[4 * i] = movz(0, value(i));
        code.words()[4 * i + 1] = kRet;
    }
}

// An arena address the test keeps without pinning its chunk: reclaim scans
// thread stacks conservatively, and a plain pointer in the test's frame is a
// reference like any other.
struct Hidden {
    static constexpr std::uint64_t kMask = 0xa5a5'5a5a'a5a5'5a5aull;
    std::uint64_t value;
    explicit Hidden(const void* p) : value(reinterpret_cast<std::uint64_t>(p) ^ kMask) {}
};

// Arena addresses only in frames that have returned (cleared by reclaim_run).
__declspec(noinline) bool has_entry(std::uint64_t pc) { return rt::find_entry(pc) != nullptr; }
__declspec(noinline) Hidden entry_of(std::uint64_t pc) { return Hidden(rt::find_entry(pc)); }

__declspec(noinline) std::uint32_t chunk_state_of(Hidden host) {
    const std::uint32_t chunk = rt::arena_chunk_of(host.value ^ Hidden::kMask);
    return chunk == ~0u ? ~0u : static_cast<std::uint32_t>(rt::arena_chunk(chunk).state);
}

struct MiniScope {
    explicit MiniScope(std::size_t padding) {
        set_mini_translator(true);
        set_mini_padding(padding);
    }
    ~MiniScope() {
        set_mini_padding(0);
        set_mini_translator(false);
    }
};

struct BudgetScope {
    explicit BudgetScope(std::size_t chunks) { rt::arena_set_budget_for_tests(chunks); }
    ~BudgetScope() { rt::arena_set_budget_for_tests(rt::kArenaChunks); }
};

} // namespace

FX_TEST(reclaim_frees_dead_chunks_and_reuses_them) {
    init();
    MiniScope mini(kPad);
    constexpr unsigned kFunctions = 48;   // 12 MiB of regions: 6 chunks
    GuestCode code({}, 0x4000);
    write_functions(code, kFunctions, [](unsigned i) { return i + 1; });
    for (unsigned i = 0; i < kFunctions; ++i) FX_CHECK_EQ(run(code.at(4 * i)), i + 1);
    FX_REQUIRE(has_entry(code.at(0)));
    const Hidden first = entry_of(code.at(0));
    FX_CHECK_EQ(chunk_state_of(first), static_cast<std::uint32_t>(rt::ChunkState::code));
    const auto s0 = runtime_stats();

    // Dead but not yet reclaimed: the code stays mapped.
    invalidate_guest_code(code.base(), 0x4000);
    FX_CHECK_EQ(chunk_state_of(first), static_cast<std::uint32_t>(rt::ChunkState::code));
    rt::ReclaimResult r;
    FX_REQUIRE(rt::reclaim_run(false, &r));
    FX_CHECK(r.scanned);
    FX_CHECK_EQ(r.evicted, 0u);
    // The stream's current chunk stays (allocation may continue there); the full ones go.
    FX_CHECK(r.freed >= 4u);
    FX_CHECK_EQ(chunk_state_of(first), static_cast<std::uint32_t>(rt::ChunkState::free));
    const auto s1 = runtime_stats();
    FX_CHECK(s1.reclaimed_bytes - s0.reclaimed_bytes >= 4 * rt::kArenaChunk);
    FX_CHECK(s1.arena_committed + 4 * kPad * 7 <= s0.arena_committed);
    FX_CHECK(s1.arena_metadata <= s0.arena_metadata);
    FX_CHECK_EQ(s1.reclaim_passes - s0.reclaim_passes, 1u);

    // The guest code runs again, translated into chunks taken afresh (some reused).
    write_functions(code, kFunctions, [](unsigned i) { return 1000 + i; });
    for (unsigned i = 0; i < kFunctions; ++i) FX_CHECK_EQ(run(code.at(4 * i)), 1000 + i);
    FX_CHECK(has_entry(code.at(0)));
    invalidate_guest_code(code.base(), 0x4000);
    FX_REQUIRE(rt::reclaim_run(false, nullptr));
}

// A thread still executing a dead region pins its chunk: the pass keeps it,
// the thread finishes in the old code, and a later pass frees it.
FX_TEST(reclaim_keeps_a_chunk_a_thread_is_running) {
    init();
    // Words no other test uses: a kept translation of equal words would be reused.
    GuestCode code({movz(0, 4242), kRet}, 0x4000);
    const std::uint64_t pc = code.at(0);
    static std::atomic<std::uint32_t> entered{0}, release{0};
    entered = 0;
    release = 0;
    // Spins until `release`; padded so the next region needs another chunk.
    set_region_factory(pc, [](std::uint64_t start) {
        RegionBuilder b(start);
        b.entry(start);
        auto& a = b.as();
        a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&entered));
        a.mov(32, ptr(Gp::rax), 1);
        a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(&release));
        const Label loop = a.bound_label();
        a.pause();
        a.cmp(32, ptr(Gp::rax), 0);
        a.jcc(x64::Cond::e, loop);
        a.mov(64, ptr(Gp::r15, x_disp(0)), 77);
        b.return_via_x30();
        for (std::size_t i = 0; i < rt::kArenaChunk - 64 * 1024; ++i) a.db(0xcc);
        b.cover(start, start + 8);
        return b.finish();
    });
    std::uint64_t result = 0;
    std::thread spinner([&] { result = run(pc); });
    const auto deadline = GetTickCount64() + 30000;
    while (entered.load() == 0 && GetTickCount64() < deadline) std::this_thread::yield();
    if (entered.load() == 0) {   // never got there: do not leave the thread behind
        release = 1;
        spinner.join();
        FX_REQUIRE(entered.load() == 1u);
    }
    FX_REQUIRE(has_entry(pc));
    const Hidden host = entry_of(pc);
    set_region_factory(pc, nullptr);
    invalidate_guest_code(code.base(), 0x4000);   // dead; the thread keeps spinning in it
    {
        // Another volatile region that does not fit: the spinner's chunk is full now.
        MiniScope mini(256 * 1024);
        GuestCode other({movz(0, 5), kRet}, 0x4000);
        FX_CHECK_EQ(run(other.at(0)), 5u);
        rt::ReclaimResult r;
        FX_REQUIRE(rt::reclaim_run(false, &r));
        FX_CHECK(r.scanned);
        FX_CHECK(r.pinned >= 1u);
        FX_CHECK_EQ(chunk_state_of(host), static_cast<std::uint32_t>(rt::ChunkState::code));
        release = 1;
        spinner.join();
        FX_CHECK_EQ(result, 77u);   // finished in its (kept) dead code
        FX_REQUIRE(rt::reclaim_run(false, &r));
        FX_CHECK_EQ(chunk_state_of(host), static_cast<std::uint32_t>(rt::ChunkState::free));
        invalidate_guest_code(other.base(), 0x4000);
    }
    FX_CHECK_EQ(run(pc), 4242u);   // translated again (movz x0, #4242)
}

// With the arena budget nearly used, installing more live code evicts the
// chunks with the least live code; evicted functions run again correctly.
FX_TEST(reclaim_evicts_live_chunks_when_the_budget_runs_low) {
    init();
    MiniScope mini(kPad);
    FX_REQUIRE(rt::reclaim_run(false, nullptr));
    const auto s0 = runtime_stats();
    BudgetScope budget(s0.arena_chunks_used + 8);
    constexpr unsigned kFunctions = 96;   // 24 MiB of live regions in a 16 MiB budget
    GuestCode code({}, 0x4000);
    write_functions(code, kFunctions, [](unsigned i) { return 7 * i + 3; });
    for (unsigned round = 0; round < 2; ++round)
        for (unsigned i = 0; i < kFunctions; ++i) FX_CHECK_EQ(run(code.at(4 * i)), 7 * i + 3);
    const auto s1 = runtime_stats();
    FX_CHECK(s1.evicted_regions > s0.evicted_regions);
    FX_CHECK(s1.reclaimed_bytes > s0.reclaimed_bytes);
    FX_CHECK(s1.arena_chunks_used <= s0.arena_chunks_used + 8);
    invalidate_guest_code(code.base(), 0x4000);
}

// Callers run functions on several threads while one rewrites their code
// (SMC kills whole pages) and the budget keeps reclaim (with eviction)
// running. Each call must return the function's old or new value, never
// anything else, and the arena stays within its budget.
FX_TEST(reclaim_under_concurrent_execution_smc_and_eviction) {
    init();
    MiniScope mini(64 * 1024);
    FX_REQUIRE(rt::reclaim_run(false, nullptr));
    const auto s0 = runtime_stats();
    BudgetScope budget(s0.arena_chunks_used + 12);
    constexpr unsigned kFunctions = 256;   // 4 pages, 16 MiB of regions if all were live
    GuestCode code({}, 0x4000);
    write_functions(code, kFunctions, [](unsigned i) { return i; });
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> calls{0}, wrong{0}, writes{0}, passes{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::uint32_t x = 0x9e3779b9u * (t + 1);
            while (!stop.load(std::memory_order_relaxed)) {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                const unsigned i = x % kFunctions;
                const std::uint64_t value = run(code.at(4 * i));
                if (value != i && value != i + 4096) ++wrong;
                ++calls;
            }
        });
    }
    threads.emplace_back([&] {   // the writer: function i returns i or i + 4096
        std::uint32_t x = 12345;
        while (!stop.load(std::memory_order_relaxed)) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const unsigned i = x % kFunctions;
            const std::uint32_t now = (code.words()[4 * i] >> 5) & 0xffff;
            code.words()[4 * i] = movz(0, now == i ? i + 4096 : i);
            ++writes;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    threads.emplace_back([&] {   // explicit passes on top of the ones the budget asks for
        while (!stop.load(std::memory_order_relaxed)) {
            if (rt::reclaim_run(false, nullptr)) ++passes;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    // Load-tolerant: at least 4 s, then until there was enough of everything
    // (or 60 s passed); coherence is checked on every call.
    const auto start = GetTickCount64();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto elapsed = GetTickCount64() - start;
        if (elapsed >= 60000) break;
        if (elapsed >= 4000 && calls.load() > 1000u && writes.load() > 100u && passes.load() > 20u) break;
    }
    stop = true;
    for (auto& t : threads) t.join();
    const auto s1 = runtime_stats();
    std::printf("  reclaim stress: %llu calls, %llu writes, %llu explicit passes, %llu passes in all, %llu MiB "
                "reclaimed, %llu evicted, %llu pinned\n",
                static_cast<unsigned long long>(calls.load()), static_cast<unsigned long long>(writes.load()),
                static_cast<unsigned long long>(passes.load()),
                static_cast<unsigned long long>(s1.reclaim_passes - s0.reclaim_passes),
                static_cast<unsigned long long>((s1.reclaimed_bytes - s0.reclaimed_bytes) >> 20),
                static_cast<unsigned long long>(s1.evicted_regions - s0.evicted_regions),
                static_cast<unsigned long long>(s1.reclaim_pinned - s0.reclaim_pinned));
    FX_CHECK_EQ(wrong.load(), 0u);
    FX_CHECK(calls.load() > 1000u);
    FX_CHECK(writes.load() > 20u);
    FX_CHECK(s1.reclaim_passes - s0.reclaim_passes >= passes.load());
    FX_CHECK(s1.reclaimed_bytes > s0.reclaimed_bytes);
    FX_CHECK_EQ(s1.reclaim_scan_failures, s0.reclaim_scan_failures);
    FX_CHECK(s1.arena_chunks_used <= s0.arena_chunks_used + 12);
    // Every function still returns its current value.
    for (unsigned i = 0; i < kFunctions; ++i) {
        const std::uint32_t now = (code.words()[4 * i] >> 5) & 0xffff;
        FX_CHECK_EQ(run(code.at(4 * i)), now);
    }
    invalidate_guest_code(code.base(), 0x4000);
}

// The process-exit checks: no pass runs once stopped is covered by
// process_exit_* in runtime_thread_tests; here only that an idle reclaimer
// reports idle without waiting.
FX_TEST(reclaim_lock_free_when_idle) {
    init();
    FX_REQUIRE(rt::reclaim_run(false, nullptr));
    FX_CHECK(rt::arena_lock_free());
    FX_CHECK(rt::region_locks_free());
}
