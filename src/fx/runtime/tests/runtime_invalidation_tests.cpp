// Explicit code flushes (flush_guest_code, Darwin sys_icache_invalidate),
// invalidation finer than a page, SMC coherence under concurrent execution,
// and the merging of registered guest code ranges.

#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace rtt;
namespace rt = transmute::fx::rt;

namespace {

void init() { runtime_initialize(RuntimeOptions{}); }

constexpr std::uint32_t kRet = 0xd65f03c0;
constexpr std::uint32_t movz(unsigned rd, std::uint32_t imm) { return 0xd2800000u | (imm << 5) | rd; }

std::uint64_t run(std::uint64_t pc) { return call_guest(pc, {}); }

// Changes the host protection behind fx's back: a later store cannot fault,
// as with a write through a mapping fx does not know.
void unprotect_unseen(std::uint64_t address) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(address & ~std::uint64_t{0xfff}), 0x1000, PAGE_READWRITE, &old);
}

// A guest store from host code; false if it faulted for good (no fx handler
// took the fault, so it reached this frame).
bool store_or_fault(volatile std::uint32_t* word, std::uint32_t value) {
    __try {
        *word = value;
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

} // namespace

FX_TEST(flush_guest_code_drops_a_rewrite_fx_did_not_see) {
    init();
    set_mini_translator(true);
    GuestCode code({movz(0, 1), kRet});
    FX_CHECK_EQ(run(code.at(0)), 1u);
    FX_CHECK(guest_page_protection(code.at(0)) != 0);   // write-protected for SMC
    const auto s0 = runtime_stats();
    unprotect_unseen(code.at(0));
    code.words()[0] = movz(0, 2);
    FX_CHECK_EQ(runtime_stats().smc_write_faults, s0.smc_write_faults);
    FX_CHECK_EQ(run(code.at(0)), 1u);   // stale: nothing told fx
    flush_guest_code(code.at(0), 8);
    FX_CHECK_EQ(run(code.at(0)), 2u);
    const auto s1 = runtime_stats();
    FX_CHECK_EQ(s1.code_flushes - s0.code_flushes, 1u);
    FX_CHECK_EQ(s1.code_flush_bytes - s0.code_flush_bytes, 8u);
    FX_CHECK_EQ(s1.code_flush_hits - s0.code_flush_hits, 1u);
    FX_CHECK_EQ(s1.translations - s0.translations, 1u);
    FX_CHECK_EQ(s1.retranslations - s0.retranslations, 1u);
    FX_CHECK(s1.registered_code_translations - s0.registered_code_translations >= 1u);
    FX_CHECK(s1.invalidation_interrupts - s0.invalidation_interrupts >= 1u);
    // A flush of bytes without translations drops nothing.
    flush_guest_code(code.at(40), 64);
    FX_CHECK_EQ(runtime_stats().code_flush_hits, s1.code_flush_hits);
    FX_CHECK_EQ(run(code.at(0)), 2u);
    set_mini_translator(false);
}

// Three functions on one page; dropping the middle one's bytes keeps the
// others, and their page stays write-protected, so a later store to them
// still faults and invalidates (on the old code the page got its guest
// protection back and the store went unseen: function a kept returning 1).
FX_TEST(partial_page_invalidation_keeps_neighbours_protected) {
    init();
    set_mini_translator(true);
    GuestCode code({});
    const auto a = code.at(0), b = code.at(16), c = code.at(32);
    code.words()[0] = movz(0, 1);
    code.words()[1] = kRet;
    code.words()[16] = movz(0, 2);
    code.words()[17] = kRet;
    code.words()[32] = movz(0, 3);
    code.words()[33] = kRet;
    FX_CHECK_EQ(run(a), 1u);
    FX_CHECK_EQ(run(b), 2u);
    FX_CHECK_EQ(run(c), 3u);
    FX_CHECK_EQ(rt::live_regions_on_page(a), 3u);
    const auto s0 = runtime_stats();
    invalidate_guest_code(b + 4, 4);   // inside b's bytes: b dies, a and c survive
    FX_CHECK_EQ(rt::live_regions_on_page(a), 2u);
    FX_CHECK(guest_page_protection(a) != 0);
    FX_CHECK_EQ(runtime_stats().invalidation_pages_kept - s0.invalidation_pages_kept, 1u);
    FX_CHECK_EQ(run(a), 1u);
    FX_CHECK_EQ(run(c), 3u);
    FX_CHECK_EQ(runtime_stats().translations - s0.translations, 0u);   // a and c survived
    code.words()[0] = movz(0, 7);   // still protected: faults and invalidates the page
    FX_CHECK_EQ(runtime_stats().smc_write_faults - s0.smc_write_faults, 1u);
    FX_CHECK_EQ(run(a), 7u);
    FX_CHECK_EQ(run(b), 2u);
    FX_CHECK_EQ(run(c), 3u);
    // The same through flush_guest_code: bytes before and after survive, a
    // range ending inside a region kills it.
    const auto s1 = runtime_stats();
    flush_guest_code(c - 4, 8);   // the word before c and c's first word
    FX_CHECK_EQ(rt::live_regions_on_page(a), 2u);
    FX_CHECK(guest_page_protection(a) != 0);
    FX_CHECK_EQ(run(a), 7u);
    FX_CHECK_EQ(run(b), 2u);
    FX_CHECK_EQ(runtime_stats().translations - s1.translations, 0u);
    FX_CHECK_EQ(run(c), 3u);   // c's words are unchanged: its dead translation is reused
    const auto s2c = runtime_stats();
    FX_CHECK_EQ(s2c.translations - s1.translations + s2c.jit_reuses - s1.jit_reuses, 1u);
    // Flushing every region on the page gives the page its protection back.
    flush_guest_code(a, 0x1000);
    FX_CHECK_EQ(rt::live_regions_on_page(a), 0u);
    FX_CHECK_EQ(guest_page_protection(a), 0u);
    const auto s2 = runtime_stats();
    code.words()[100] = 0;   // writable again: no fault
    FX_CHECK_EQ(runtime_stats().smc_write_faults, s2.smc_write_faults);
    set_mini_translator(false);
}

// A store next to translated code kills every region on its page (the SMC
// fault cannot tell how much the store writes); a killed JIT region whose
// words did not change is installed again without translating it, and one
// whose words changed is translated afresh.
FX_TEST(unchanged_dead_jit_regions_are_reused) {
    init();
    set_mini_translator(true);
    GuestCode code({movz(0, 3), kRet});
    FX_CHECK_EQ(run(code.at(0)), 3u);
    const auto s0 = runtime_stats();
    const int calls0 = g_translate_calls.load();
    code.words()[200] = 1;   // data on the same page: SMC fault, the region dies
    FX_CHECK_EQ(runtime_stats().smc_write_faults - s0.smc_write_faults, 1u);
    FX_CHECK_EQ(rt::live_regions_on_page(code.at(0)), 0u);
    FX_CHECK_EQ(run(code.at(0)), 3u);
    auto d = runtime_stats();
    FX_CHECK_EQ(d.jit_reuses - s0.jit_reuses, 1u);
    FX_CHECK_EQ(d.translations - s0.translations, 0u);
    FX_CHECK_EQ(g_translate_calls.load(), calls0);
    FX_CHECK(guest_page_protection(code.at(0)) != 0);   // protected again by the install
    code.words()[0] = movz(0, 4);   // the region's own word: no reuse
    FX_CHECK_EQ(run(code.at(0)), 4u);
    d = runtime_stats();
    FX_CHECK_EQ(d.jit_reuses - s0.jit_reuses, 1u);
    FX_CHECK_EQ(d.translations - s0.translations, 1u);
    // The new translation is the one kept now.
    code.words()[201] = 2;
    FX_CHECK_EQ(run(code.at(0)), 4u);
    FX_CHECK_EQ(runtime_stats().jit_reuses - s0.jit_reuses, 2u);
    set_mini_translator(false);
}

// The guest makes a translated (fx write-protected) page read-only: fx
// forgets the RW it saved, so invalidating the page later does not hand the
// page RW back over the guest's protection (it did: the restore only checks
// that the page still has fx's read-only protection, which it then had).
FX_TEST(lowered_guest_protection_is_not_restored) {
    init();
    set_mini_translator(true);
    GuestCode code({movz(0, 5), kRet});
    FX_CHECK_EQ(run(code.at(0)), 5u);
    FX_CHECK_EQ(guest_page_protection(code.at(0)), static_cast<std::uint32_t>(PAGE_READWRITE));
    DWORD old = 0;
    FX_REQUIRE(VirtualProtect(reinterpret_cast<void*>(code.base()), 0x1000, PAGE_READONLY, &old));
    guest_protection_lowered(code.base(), 0x1000);
    FX_CHECK_EQ(guest_page_protection(code.at(0)), 0u);
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(run(code.at(0)), 5u);   // the translation stays valid
    FX_CHECK_EQ(runtime_stats().translations, s0.translations);
    invalidate_guest_code(code.at(0), 8);
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<void*>(code.base()), &info, sizeof(info));
    FX_CHECK_EQ(info.Protect, static_cast<DWORD>(PAGE_READONLY));
    FX_CHECK_EQ(run(code.at(0)), 5u);
    FX_CHECK_EQ(guest_page_protection(code.at(0)), 0u);   // read-only: nothing to write-protect
    VirtualProtect(reinterpret_cast<void*>(code.base()), 0x1000, PAGE_READWRITE, &old);
    invalidate_guest_code(code.base(), 0x1000);
    set_mini_translator(false);
}

// A terabyte invalidation (an unregistered code range or a reservation made
// writable) visits only the pages holding translations or fx protection.
FX_TEST(huge_range_invalidation_is_sparse) {
    init();
    set_mini_translator(true);
    GuestCode code({movz(0, 9), kRet});
    FX_CHECK_EQ(run(code.at(0)), 9u);
    FX_CHECK(guest_page_protection(code.at(0)) != 0);
    constexpr std::uint64_t tib = 1ULL << 40;
    const std::uint64_t begin = code.base() > tib / 2 ? code.base() - tib / 2 : 0;
    const auto start = GetTickCount64();
    invalidate_guest_code(begin, tib);
    FX_CHECK(GetTickCount64() - start < 10000);   // 2^28 pages one by one would take many minutes
    FX_CHECK_EQ(rt::live_regions_on_page(code.at(0)), 0u);
    FX_CHECK_EQ(guest_page_protection(code.at(0)), 0u);
    code.words()[0] = movz(0, 10);   // writable again
    FX_CHECK_EQ(run(code.at(0)), 10u);
    set_mini_translator(false);
}

// Thousands of registrations of the same and touching ranges (V8 registers
// its code range on every mprotect to executable) keep one range and publish
// only when the list changes; holes punched by unregistering split it, and
// lookups at their edges stay exact.
FX_TEST(guest_code_registrations_merge) {
    init();
    // Address space nobody uses here: registration needs no memory.
    constexpr std::uint64_t base = 0x6a00'0000'0000ULL, size = 256ULL << 20, page = 0x4000;
    const auto s0 = runtime_stats();
    register_guest_code_memory(base, size);
    FX_CHECK_EQ(runtime_stats().code_ranges, s0.code_ranges + 1);
    const auto published = runtime_stats().code_range_publishes;
    for (std::uint64_t i = 0; i < 5000; ++i) {
        const auto offset = (i * 7919 * page) % size;
        register_guest_code_memory(base + offset, std::min(size - offset, (i % 5 + 1) * page));
    }
    register_guest_code_memory(base, size);
    FX_CHECK_EQ(runtime_stats().code_range_publishes, published);   // nothing changed
    FX_CHECK_EQ(runtime_stats().code_ranges, s0.code_ranges + 1);
    // Touching and overlapping extensions merge into the one range.
    register_guest_code_memory(base + size, page);
    register_guest_code_memory(base - page, 2 * page);
    for (int i = 0; i < 1000; ++i) register_guest_code_memory(base + size - page, 2 * page);
    FX_CHECK_EQ(runtime_stats().code_ranges, s0.code_ranges + 1);
    FX_CHECK_EQ(runtime_stats().code_range_publishes, published + 2);
    FX_CHECK(is_guest_code(base - page));
    FX_CHECK(is_guest_code(base + size + page - 1));
    FX_CHECK(!is_guest_code(base + size + page));
    FX_CHECK(!is_guest_code(base - page - 1));
    // Holes.
    const std::uint64_t h1 = base + (1ULL << 20), h2 = base + (5ULL << 20);
    unregister_guest_code_memory(h1, page);
    unregister_guest_code_memory(h2, 2 * page);
    FX_CHECK_EQ(runtime_stats().code_ranges, s0.code_ranges + 3);
    FX_CHECK(is_guest_code(h1 - 1));
    FX_CHECK(!is_guest_code(h1));
    FX_CHECK(!is_guest_code(h1 + page - 1));
    FX_CHECK(is_guest_code(h1 + page));
    FX_CHECK(!is_guest_code(h2 + 2 * page - 4));
    FX_CHECK(is_guest_code(h2 + 2 * page));
    std::uint64_t lo = 0, hi = 0;
    FX_CHECK(rt::guest_code_range(h1 + page, lo, hi));
    FX_CHECK_EQ(lo, h1 + page);
    FX_CHECK_EQ(hi, h2);
    // Unregistering a hole again changes nothing; re-registering one fills it.
    const auto before = runtime_stats().code_range_publishes;
    unregister_guest_code_memory(h1, page);
    FX_CHECK_EQ(runtime_stats().code_range_publishes, before);
    register_guest_code_memory(h1, page);
    register_guest_code_memory(h2, 2 * page);
    FX_CHECK_EQ(runtime_stats().code_ranges, s0.code_ranges + 1);
    unregister_guest_code_memory(base - page, size + 2 * page);
    FX_CHECK_EQ(runtime_stats().code_ranges, s0.code_ranges);
    FX_CHECK(!is_guest_code(base));
}

// One thread rewrites a function (guest stores from host code: SMC faults on
// the write-protected page) while others call it. A value stored before a
// call starts must be what the call returns or a later one: no stale
// translation survives a completed store.
FX_TEST(concurrent_rewrite_and_execution_stay_coherent) {
    init();
    set_mini_translator(true);
    GuestCode code({movz(0, 0), kRet});
    std::atomic<std::uint32_t> published{0};
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0}, running{0};
    std::atomic<std::uint64_t> calls{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            ++running;
            while (!stop.load()) {
                const auto floor = published.load();
                const auto value = run(code.at(0));
                if (value < floor || value > 4000) ++errors;
                ++calls;
            }
        });
    }
    // Load-tolerant: up to 1000 rewrites within the deadline; coherence is
    // checked on every call, the count only needs to be meaningful.
    const auto deadline = GetTickCount64() + 30000;
    while (running.load() < 4 && GetTickCount64() < deadline) SwitchToThread();
    const auto s0 = runtime_stats();
    for (std::uint32_t i = 1; i <= 1000 && GetTickCount64() < deadline; ++i) {
        reinterpret_cast<volatile std::uint32_t*>(code.words())[0] = movz(0, i);
        if (i % 4 == 0) flush_guest_code(code.at(0), 4);   // an explicit flush on top changes nothing
        published.store(i);
        const auto at = calls.load();
        while (calls.load() < at + 2 && GetTickCount64() < deadline) SwitchToThread();
    }
    stop = true;
    for (auto& w : workers) w.join();
    FX_CHECK_EQ(errors.load(), 0);
    FX_CHECK_EQ(run(code.at(0)), published.load());
    std::printf("  concurrent rewrites: %u, calls %llu\n", published.load(),
                static_cast<unsigned long long>(calls.load()));
    FX_CHECK(published.load() >= 100u);
    FX_CHECK(runtime_stats().smc_write_faults - s0.smc_write_faults > 0);
    set_mini_translator(false);
}

// Several threads store to one code page while others run code from it.
// A store's fault can find the page restored by another writer's SMC fault
// and then translated from (protected) again: the store must be retried, not
// raised as a protection failure (Chrome's V8 code pages: an intermittent
// SIGBUS on a page fx had write-protected).
FX_TEST(concurrent_writers_on_a_code_page_never_fault) {
    init();
    set_mini_translator(true);
    constexpr int kFunctions = 4;
    GuestCode code({movz(0, 0), kRet, movz(0, 0), kRet, movz(0, 0), kRet, movz(0, 0), kRet});
    std::atomic<bool> stop{false};
    std::atomic<int> failed{0}, running{0};
    std::atomic<std::uint64_t> calls{0}, stores{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kFunctions; ++t) {
        threads.emplace_back([&, t] {
            ++running;
            while (!stop.load()) {
                static_cast<void>(run(code.at(2 * ((t + calls.load()) % kFunctions))));
                ++calls;
            }
        });
    }
    const auto deadline = GetTickCount64() + 20000;
    while (running.load() < kFunctions && GetTickCount64() < deadline) SwitchToThread();
    const auto s0 = runtime_stats();
    for (int t = 0; t < kFunctions; ++t) {
        threads.emplace_back([&, t] {
            auto* word = reinterpret_cast<volatile std::uint32_t*>(code.words()) + 2 * t;
            for (std::uint32_t i = 1; i <= 3000 && !stop.load() && GetTickCount64() < deadline; ++i) {
                if (!store_or_fault(word, movz(0, i))) {
                    ++failed;
                    return;
                }
                ++stores;
            }
        });
    }
    for (std::size_t i = kFunctions; i < threads.size(); ++i) threads[i].join();
    stop = true;
    for (int i = 0; i < kFunctions; ++i) threads[i].join();
    std::printf("  concurrent writers: stores %llu, calls %llu, smc faults %llu\n",
                static_cast<unsigned long long>(stores.load()), static_cast<unsigned long long>(calls.load()),
                static_cast<unsigned long long>(runtime_stats().smc_write_faults - s0.smc_write_faults));
    FX_CHECK_EQ(failed.load(), 0);
    FX_CHECK(stores.load() >= 400u);
    set_mini_translator(false);
}
