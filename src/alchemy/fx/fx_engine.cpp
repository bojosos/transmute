#include "alchemy/fx/fx_engine.hpp"

#include "fx/cache/cache.hpp"
#include "fx/host/env.hpp"
#include "fx/host/fault.hpp"
#include "fx/host/guard.hpp"
#include "fx/host/process.hpp"
#include "fx/host/text.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/vm.hpp"
#include "fx/runtime/runtime.hpp"
#include "fx/translator/translator.hpp"
#include "transmute/fx/context.hpp"
#include "transmute/fx/process.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Alchemy's engine-neutral termination type, parsed exactly as the other
// alchemy-fx sources see it (ReferenceEngine names FxEngine there).
#include "alchemy/fx/alchemy_fx_prelude.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/perf.hpp"
#undef ReferenceEngine

namespace transmute {
namespace {

namespace fx = transmute::fx;

// Matches the reference engine's synthetic call layout so guest-visible
// function pointers keep the same values in both builds.
constexpr std::uint64_t kGuestCallBase = 0x0000710000000000ULL;
constexpr std::uint64_t kGuestCallStride = 16;
constexpr std::uint64_t kMaximumGuestCalls = 65'536;
constexpr std::uint64_t kGuestCallEnd = kGuestCallBase + kGuestCallStride * kMaximumGuestCalls;
constexpr std::uint32_t kGuestCallImmediate = 0x7a1c;
constexpr std::uint64_t kPage = 0x1000;
constexpr std::uint64_t kGranule = 0x10000;
constexpr std::uint64_t kLowLimit = 0x100000000ULL;   // __PAGEZERO is bookkeeping only
constexpr std::uint64_t kDefaultStack = 1024ULL * 1024;
// Demand commit: a Darwin page (16 KiB) is committed on its first touch.
constexpr std::uint64_t kDarwinPage = 0x4000;
// Darwin reservations made accessible in one call of at least this size
// (V8's 256 MiB code range made RWX in one mprotect) commit on first touch;
// smaller ones commit up front, as before (no faults on their first use).
constexpr std::uint64_t kLazyCommitMinimum = 8ULL << 20;

std::uint64_t align_down(std::uint64_t value, std::uint64_t alignment) { return value & ~(alignment - 1); }
std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

namespace vm = fx::host::vm;

// The main executable's load range, held from the start of the process.
// Host allocations made while Alchemy starts (thread stacks, heap segments)
// are placed bottom-up from a per-process random base. Once the engine's
// placeholder below is released, they could take the range the executable
// is about to load at, and mapping it then fails ("requested segment address
// is unavailable"): the intermittent rebased runs of alchemy_process_tree_fx
// under load. FxEngine::hold_load_range reserves the range first thing in
// main; the first guest placement inside it releases the hold, and the
// placement takes the range.
namespace load_range {
constexpr std::uint64_t kSize = 0x100000000ULL;   // 4 GiB past the load base
// The free parts of the range when main started (something may already sit
// in it then), each its own reservation.
constexpr std::size_t kMaxPieces = 64;
void* pieces[kMaxPieces]{};
std::uint64_t piece_sizes[kMaxPieces]{};
std::size_t piece_count{};
std::atomic<bool> held{false};
std::uint64_t begin{}, end{};   // written once, before the hold is published

void release_if_overlaps(std::uint64_t base, std::uint64_t size) noexcept {
    if (!held.load(std::memory_order_acquire) || size == 0) return;
    if (base >= end || base + size <= begin) return;
    if (!held.exchange(false, std::memory_order_acq_rel)) return;
    for (std::size_t index = 0; index < piece_count; ++index) vm::release(pieces[index]);
}
} // namespace load_range

[[noreturn]] void memory_fault(const char* operation, std::uint64_t address) {
    std::ostringstream message;
    message << "guest memory " << operation << " fault at 0x" << std::hex << address;
    throw std::runtime_error(message.str());
}

// Guest memory accesses that may fault (fx/host/guard.hpp: access
// violations, in-page errors and guard pages fail; guarded_length is the
// number of characters before the first zero, up to `maximum`, or -1).
using fx::host::guarded_fill;
using fx::host::guarded_length;
using fx::host::guarded_move;

void* host(std::uint64_t address) { return reinterpret_cast<void*>(address); }

// Private growable heap for guest malloc (low-fragmentation front end).
vm::Heap guest_heap() {
    static const vm::Heap heap = vm::create_heap();
    return heap;
}

// Commits, protection changes and decommits (VirtualAlloc(MEM_COMMIT),
// VirtualProtect, VirtualFree(MEM_DECOMMIT) on win32) must stay inside one
// reservation; guest ranges can span several of ours.
template <typename Function>
bool for_each_allocation_piece(std::uint64_t begin, std::uint64_t end, Function&& function) {
    auto address = begin;
    while (address < end) {
        vm::RegionInfo info;
        if (!vm::query(host(address), info)) return false;
        const auto piece_end = std::min(end, info.base + info.size);
        if (!function(address, piece_end - address)) return false;
        address = piece_end;
    }
    return true;
}

bool commit(std::uint64_t begin, std::uint64_t end) {
    return for_each_allocation_piece(begin, end, [](std::uint64_t address, std::uint64_t size) {
        // Shared memory (map_shared_memory, fx views) is committed already.
        vm::RegionInfo info;
        if (vm::query(host(address), info) && info.type == vm::Type::mapped && info.committed()) return true;
        return vm::commit(host(address), static_cast<std::size_t>(size), vm::kReadWrite) != nullptr;
    });
}

bool protect(std::uint64_t begin, std::uint64_t end, vm::Protection protection) {
    return for_each_allocation_piece(begin, end, [protection](std::uint64_t address, std::uint64_t size) {
        // Reserved pages of a reservation are inaccessible already (PROT_NONE
        // over a partly committed range); a later commit sets their protection.
        vm::RegionInfo info;
        if (protection == vm::kNoAccess && vm::query(host(address), info) && info.state == vm::State::reserved)
            return true;
        return vm::protect(host(address), static_cast<std::size_t>(size), protection);
    });
}

void decommit(std::uint64_t begin, std::uint64_t end) {
    for_each_allocation_piece(begin, end, [](std::uint64_t address, std::uint64_t size) {
        vm::decommit(host(address), static_cast<std::size_t>(size));
        return true;
    });
}

// Committed pieces of [begin, end) (vm::query runs). Reserved
// address space is skipped, so huge sparse reservations cost one query per run.
template <typename Function>
void for_each_committed_piece(std::uint64_t begin, std::uint64_t end, Function function) {
    auto address = begin;
    while (address < end) {
        vm::RegionInfo info;
        if (!vm::query(host(address), info)) return;
        const auto piece_end = std::min(end, info.base + info.size);
        if (info.committed()) function(address, piece_end - address);
        address = piece_end;
    }
}

// Drops translations (and fx's write protection of translated pages) in the
// committed parts of [begin, end); uncommitted pages hold no translated code.
void invalidate_committed(std::uint64_t begin, std::uint64_t end) {
    for_each_committed_piece(begin, end, [](std::uint64_t address, std::uint64_t size) {
        fx::invalidate_guest_code(address, size);
    });
}

// Ranges of guest-code registration dropped with the memory under them, as
// a whole: accessible pages may be uncommitted (committed on first touch)
// and still registered. fx's invalidation of a huge range visits only the
// pages that hold translations.
void unregister_code(std::uint64_t begin, std::uint64_t end) { fx::unregister_guest_code_memory(begin, end - begin); }

// Disjoint runs [begin, end) of 64 KiB granule indices (a terabyte
// reservation is one run, not sixteen million set entries).
struct GranuleSet {
    std::map<std::uint64_t, std::uint64_t> runs;   // begin -> end
    [[nodiscard]] bool contains(std::uint64_t granule) const {
        auto it = runs.upper_bound(granule);
        if (it == runs.begin()) return false;
        --it;
        return granule < it->second;
    }
    // End of the run that contains `granule`.
    [[nodiscard]] std::uint64_t run_end(std::uint64_t granule) const {
        auto it = runs.upper_bound(granule);
        --it;
        return it->second;
    }
    // First granule >= `granule` in the set, at most `limit` (for a granule outside the set).
    [[nodiscard]] std::uint64_t next(std::uint64_t granule, std::uint64_t limit) const {
        const auto it = runs.upper_bound(granule);
        return it == runs.end() ? limit : std::min(limit, it->first);
    }
    void insert(std::uint64_t begin, std::uint64_t end) {
        if (begin >= end) return;
        auto it = runs.upper_bound(begin);
        if (it != runs.begin() && std::prev(it)->second >= begin) {
            --it;
            begin = it->first;
            end = std::max(end, it->second);
            it = runs.erase(it);
        }
        while (it != runs.end() && it->first <= end) {
            end = std::max(end, it->second);
            it = runs.erase(it);
        }
        runs.emplace(begin, end);
    }
    void erase(std::uint64_t begin, std::uint64_t end) {
        if (begin >= end) return;
        auto it = runs.upper_bound(begin);
        if (it != runs.begin()) --it;
        while (it != runs.end() && it->first < end) {
            const auto run_begin = it->first, run_end = it->second;
            if (run_end <= begin) { ++it; continue; }
            it = runs.erase(it);
            if (run_begin < begin) runs.emplace(run_begin, begin);
            if (run_end > end) {
                runs.emplace(end, run_end);
                break;
            }
        }
    }
};

struct Region {
    std::uint64_t base{};
    std::uint64_t size{};
    bool readable{true}, writable{true}, executable{false};
    bool mapped{true};       // false: reservation without backing (__PAGEZERO)
    bool stack{false};       // fx-owned guest stack, registered for queries only
    bool lazy{false};        // reservation whose pages commit through protect_range
    bool shared{false};      // fx shared guest memory (map_shared_memory / alias_memory)
    bool copy_on_write{false};   // with shared: a copy-on-write file view (map_file_copy), never aliased
};

vm::Protection protection(bool readable, bool writable) {
    if (writable) return vm::kReadWrite;
    if (readable) return vm::kReadOnly;
    return vm::kNoAccess;
}

struct GuestExit {
    std::uint32_t code;
};

template <typename Function>
struct ScopeExit {
    Function function;
    ~ScopeExit() { function(); }
};
template <typename Function>
ScopeExit(Function) -> ScopeExit<Function>;

thread_local FxEngine* t_current = nullptr;
// An exception from a synthetic call entered as a native host callback: it is
// rethrown from the dispatcher (the rethrow slot's SVC), never unwound
// through translated frames.
thread_local std::exception_ptr t_callback_error;
thread_local std::uint64_t t_callback_pc = 0;
std::uint64_t g_rethrow_call = 0;
// ALCHEMY_FX_SVC_CALLS=1 keeps every synthetic call on the SVC exit path
// (diagnostic / A-B measurement); by default each becomes a native host
// callback at its first call (install_native_entry).
bool native_synthetic_calls() {
    static const bool enabled = [] {
        char value[8]{};
        const auto length = fx::host::get_env("ALCHEMY_FX_SVC_CALLS", value, sizeof(value));
        return !(length == 1 && value[0] == '1');
    }();
    return enabled;
}

struct ActiveEngine {
    FxEngine* previous;
    explicit ActiveEngine(FxEngine* engine) : previous(t_current) { t_current = engine; }
    ~ActiveEngine() { t_current = previous; }
};

// `vectors` false: integer-only synthetic calls (set_guest_call_integer_only)
// neither read nor write the SIMD/FP registers, so they are not copied.
void load_state(CpuState& state, const fx::GuestContext& ctx, bool vectors = true) {
    std::memcpy(state.x.data(), ctx.x, sizeof(std::uint64_t) * 31);
    state.sp = ctx.x[31];
    if (vectors)
        for (unsigned i = 0; i < 32; ++i) state.v[i] = {ctx.v[i][0], ctx.v[i][1]};
    state.nzcv = static_cast<std::uint32_t>(ctx.nzcv);
    state.fpcr = ctx.fpcr;
    state.fpsr = ctx.fpsr;
}

void store_state(const CpuState& state, fx::GuestContext& ctx, bool vectors = true) {
    std::memcpy(ctx.x, state.x.data(), sizeof(std::uint64_t) * 31);
    ctx.x[31] = state.sp;
    if (vectors)
        for (unsigned i = 0; i < 32; ++i) {
            ctx.v[i][0] = state.v[i][0];
            ctx.v[i][1] = state.v[i][1];
        }
    ctx.nzcv = state.nzcv;
    ctx.fpcr = state.fpcr;
    ctx.fpsr = state.fpsr;
}

void svc_hook(fx::GuestContext& ctx, std::uint32_t immediate) {
    auto* engine = t_current;
    if (!engine) {
        std::fprintf(stderr, "alchemy-fx: SVC #0x%x on a thread without an engine (pc 0x%llx)\n", immediate,
                     static_cast<unsigned long long>(ctx.pc - 4));
        std::abort();
    }
    engine->handle_svc(&ctx, immediate);
}

// ---- guest faults -------------------------------------------------------------
// fx dispatches guest faults to PE SEH (vectored handlers, .pdata frame
// handlers, the unhandled-exception filter). Mach-O code has none of these, so
// Alchemy installs the process unhandled-exception filter: a synthetic call
// that maps the NT status to the Darwin signal whose default action applies,
// then resumes the faulting thread at a second synthetic call that raises
// GuestSignalTermination on that thread's own dispatcher (never inside SEH
// dispatch frames). Unknown codes continue the search to fx's own report.
struct PendingFault {
    int signal{};
    std::string reason;
    std::uint64_t pc{};
};
thread_local std::optional<PendingFault> t_fault;


const char* access_name(std::uint64_t kind) { return kind == 1 ? "write" : kind == 8 ? "execute" : "read"; }

// Code rewritten under translations (JIT guests): SMC faults and explicit
// flushes. `when` (" (pid N, T s)") marks the periodic lines.
void print_invalidation_statistics(const fx::RuntimeStats& stats, const char* when) {
    std::fprintf(stderr,
                 "alchemy-fx invalidation%s: smc faults %llu (%.3f s in the fault path), icache flushes %llu "
                 "(%llu KiB, %llu dropped code), invalidations that interrupted threads %llu, pages kept protected %llu, "
                 "translations of registered code %llu, re-translations %llu (reused unchanged %llu), code ranges %llu "
                 "(published %llu); translations %llu (jit %.3f s, install %.3f s)\n",
                 when ? when : "", static_cast<unsigned long long>(stats.smc_write_faults), stats.smc_fault_seconds,
                 static_cast<unsigned long long>(stats.code_flushes),
                 static_cast<unsigned long long>(stats.code_flush_bytes / 1024),
                 static_cast<unsigned long long>(stats.code_flush_hits),
                 static_cast<unsigned long long>(stats.invalidation_interrupts),
                 static_cast<unsigned long long>(stats.invalidation_pages_kept),
                 static_cast<unsigned long long>(stats.registered_code_translations),
                 static_cast<unsigned long long>(stats.retranslations),
                 static_cast<unsigned long long>(stats.jit_reuses),
                 static_cast<unsigned long long>(stats.code_ranges),
                 static_cast<unsigned long long>(stats.code_range_publishes),
                 static_cast<unsigned long long>(stats.translations), stats.jit_seconds, stats.install_seconds);
    // The code arena's reclaim (fx runtime, "arena reclaim").
    std::fprintf(stderr,
                 "alchemy-fx arena%s: used %llu KiB in %llu chunks (%llu free), committed %llu KiB, region metadata "
                 "%llu KiB; reclaim passes %llu (%.3f s, threads stopped %.3f s), reclaimed %llu KiB, pinned %llu, "
                 "evicted regions %llu, retired lower tiers %llu, failed scans %llu\n",
                 when ? when : "", static_cast<unsigned long long>(stats.arena_used / 1024),
                 static_cast<unsigned long long>(stats.arena_chunks_used),
                 static_cast<unsigned long long>(stats.arena_chunks_free),
                 static_cast<unsigned long long>(stats.arena_committed / 1024),
                 static_cast<unsigned long long>(stats.arena_metadata / 1024),
                 static_cast<unsigned long long>(stats.reclaim_passes), stats.reclaim_seconds, stats.reclaim_scan_seconds,
                 static_cast<unsigned long long>(stats.reclaimed_bytes / 1024),
                 static_cast<unsigned long long>(stats.reclaim_pinned),
                 static_cast<unsigned long long>(stats.evicted_regions),
                 static_cast<unsigned long long>(stats.retired_regions),
                 static_cast<unsigned long long>(stats.reclaim_scan_failures));
}

std::shared_ptr<FxEngine::Shared> make_shared_space();
void print_memory_statistics(const FxEngine::Shared& shared, const char* when);

// ALCHEMY_FX_STATS_PERIOD=<seconds>: the invalidation and memory lines every period on
// stderr. Processes that are terminated (Chrome's renderers at browser exit)
// never print their statistics at exit.
void start_periodic_statistics() {
    static std::once_flag once;
    std::call_once(once, [] {
        char value[16]{};
        if (fx::host::get_env("ALCHEMY_FX_STATS_PERIOD", value, sizeof(value)) == 0) return;
        const auto seconds = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        if (seconds == 0 || seconds > 3600) return;
        std::thread([seconds] {
            for (;;) {
                fx::host::sleep_ms(seconds * 1000);
                char when[64];
                std::snprintf(when, sizeof(when), " (pid %u, %.0f s)", fx::host::process_id(),
                              fx::host::process_age_ms() / 1000.0);
                print_invalidation_statistics(fx::runtime_stats(), when);
                print_memory_statistics(*make_shared_space(), when);
            }
        }).detach();
    });
}

// ALCHEMY_FX_STATS=1: fx runtime counters at the end of a process (stderr).
void print_runtime_statistics() {
    alchemy::perf::report_exit();
    char value[8]{};
    if ((fx::host::get_env("ALCHEMY_FX_STATS", value, sizeof(value)) == 0 || value[0] == '0') &&
        !alchemy::perf::enabled())
        return;
    // Translation cache (Mach-O images are keyed by LC_UUID, register_image).
    const auto cache = fx::cache_stats();
    std::fprintf(stderr,
                 "alchemy-fx cache: modules %llu (with cache %llu, rejected files %llu), lookups %llu, hits %llu "
                 "(cold %llu), stores %llu (ignored %llu, unrelocatable %llu), rejected records %llu, guest mismatches %llu, "
                 "pinned mismatches %llu, journal %llu KiB, merges %llu\n",
                 static_cast<unsigned long long>(cache.modules_loaded), static_cast<unsigned long long>(cache.modules_with_cache),
                 static_cast<unsigned long long>(cache.files_rejected), static_cast<unsigned long long>(cache.lookups),
                 static_cast<unsigned long long>(cache.hits), static_cast<unsigned long long>(cache.cold_hits),
                 static_cast<unsigned long long>(cache.stores), static_cast<unsigned long long>(cache.stores_ignored),
                 static_cast<unsigned long long>(cache.stores_unrelocatable),
                 static_cast<unsigned long long>(cache.records_rejected), static_cast<unsigned long long>(cache.guest_mismatches),
                 static_cast<unsigned long long>(cache.pinned_mismatches),
                 static_cast<unsigned long long>(cache.journal_bytes / 1024), static_cast<unsigned long long>(cache.merges));
    const auto stats = fx::runtime_stats();
    static constexpr const char* reasons[] = {"none", "link", "lookup_miss", "interpret", "svc", "brk",
                                              "return_to_host", "interrupt", "raise", "redispatch"};
    std::fprintf(stderr,
                 "alchemy-fx stats: translations %llu (%llu guest instructions), translate %.3f s "
                 "(jit %.3f s, install %.3f s), interpreted %llu, threads %llu, arena %llu KiB (dead %llu KiB), "
                 "regions %llu, invalidated %llu, smc faults %llu, guest exceptions %llu\n  exits:",
                 static_cast<unsigned long long>(stats.translations),
                 static_cast<unsigned long long>(stats.jit_guest_instructions), stats.translate_seconds,
                 stats.jit_seconds, stats.install_seconds, static_cast<unsigned long long>(stats.interpreted),
                 static_cast<unsigned long long>(stats.threads_created),
                 static_cast<unsigned long long>(stats.arena_used / 1024),
                 static_cast<unsigned long long>(stats.arena_dead / 1024),
                 static_cast<unsigned long long>(stats.regions),
                 static_cast<unsigned long long>(stats.invalidated_regions),
                 static_cast<unsigned long long>(stats.smc_write_faults),
                 static_cast<unsigned long long>(stats.guest_exceptions));
    for (unsigned i = 1; i < std::size(reasons); ++i)
        if (stats.exits[i]) std::fprintf(stderr, " %s %llu", reasons[i], static_cast<unsigned long long>(stats.exits[i]));
    std::fprintf(stderr, "\n");
    print_invalidation_statistics(stats, nullptr);
    // Background translation of Mach-O images (fx::aot_external_module).
    const auto aot = fx::cache_external_aot_stats();
    if (aot.regions || aot.hits)
        std::fprintf(stderr,
                     "alchemy-fx aot: regions %llu (%llu guest instructions, %.3f s worker time, %llu flow jobs, "
                     "%llu static), lookups %llu (%.3f s), hits %llu, waits %llu (%.3f s), discarded %llu, cold files %llu\n",
                     static_cast<unsigned long long>(aot.regions), static_cast<unsigned long long>(aot.guest_instructions),
                     aot.translate_seconds, static_cast<unsigned long long>(aot.flow_jobs),
                     static_cast<unsigned long long>(aot.static_regions), static_cast<unsigned long long>(aot.lookups),
                     aot.lookup_seconds, static_cast<unsigned long long>(aot.hits),
                     static_cast<unsigned long long>(aot.waits), aot.wait_seconds,
                     static_cast<unsigned long long>(aot.discarded), static_cast<unsigned long long>(aot.cold_files));
}

std::optional<std::uint32_t> guarded_word(std::uint64_t address) {
    std::uint32_t word = 0;
    if ((address & 3) || !guarded_move(&word, host(address), 4)) return std::nullopt;
    return word;
}

} // namespace

struct FxEngine::Stack {
    fx::GuestStack stack{};
    fx::GuestStack previous{};
    bool switched{};
};

struct FxEngine::Shared {
    mutable std::shared_mutex memory_lock;
    std::map<std::uint64_t, Region> regions;     // by base
    // Reservations of the guest malloc heap (base -> size) that served a
    // block. Heap blocks are guest memory too (query_region reports them).
    mutable std::shared_mutex heap_lock;
    std::map<std::uint64_t, std::uint64_t> heap_segments;
    [[nodiscard]] const std::pair<const std::uint64_t, std::uint64_t>* heap_segment(std::uint64_t address) const {
        auto it = heap_segments.upper_bound(address);
        if (it == heap_segments.begin()) return nullptr;
        --it;
        return address - it->first < it->second ? &*it : nullptr;
    }
    // Our host reservations by base. A releasable one (made for a
    // Darwin mmap reservation) is freed once no guest region is left in it.
    struct Allocation { std::uint64_t extent{}; bool releasable{}; };
    std::map<std::uint64_t, Allocation> allocations;
    GranuleSet granules;                         // reserved 64 KiB granules (index runs)

    // ---- demand commit --------------------------------------------------------
    // Ranges of Darwin reservations the guest made accessible without their
    // pages being committed: `protection` is the host protection a page there
    // gets when the guest first touches it (demand_commit_hook, from fx's fault
    // handler). Committed pages inside a range have that protection already;
    // their host state is authoritative. Changed with memory_lock held
    // exclusively and commit_lock held; read under memory_lock or commit_lock.
    struct Pending { std::uint64_t end{}; vm::Protection protection{}; };
    std::map<std::uint64_t, Pending> pending;    // begin -> range, disjoint
    // Serializes demand commits with changes of `pending`. Never held while
    // touching guest memory or calling into fx (the fault handler takes it).
    std::shared_mutex commit_lock;
    std::atomic<bool> any_pending{false};
    std::atomic<std::uint64_t> demand_commits{}, demand_committed_bytes{}, demand_commit_failures{};
    // ALCHEMY_FX_LAZY_COMMIT=0 commits every accessible range up front (A/B measurement).
    const bool lazy_commit = [] {
        char value[8]{};
        const auto length = fx::host::get_env("ALCHEMY_FX_LAZY_COMMIT", value, sizeof(value));
        return !(length == 1 && value[0] == '0');
    }();

    mutable std::shared_mutex call_lock;
    std::vector<std::shared_ptr<const GuestCallHandler>> calls;
    // Lock-free view of `calls` for the dispatch path (every synthetic call):
    // a handler stays alive in `calls` or `retired_calls` until the engine
    // ends, so a pointer read here never dangles.
    std::unique_ptr<std::atomic<const GuestCallHandler*>[]> call_table{
        new std::atomic<const GuestCallHandler*>[kMaximumGuestCalls]{}};
    std::vector<std::shared_ptr<const GuestCallHandler>> retired_calls;
    // Slots whose handlers use no SIMD/FP register (set_guest_call_integer_only).
    std::unique_ptr<std::atomic<std::uint8_t>[]> integer_only{new std::atomic<std::uint8_t>[kMaximumGuestCalls]{}};
    // Slots that have their native host-callback entry (set under call_lock).
    // A slot gets it at its first call, through the SVC slot: a Chrome process
    // registers about 14,000 provider calls and calls a small part of them,
    // and each entry costs arena code, map entries and an instruction-cache
    // flush.
    std::unique_ptr<std::atomic<std::uint8_t>[]> native_entry{new std::atomic<std::uint8_t>[kMaximumGuestCalls]{}};
    std::atomic<std::uint64_t> native_entries{};   // installed so far (ALCHEMY_FX_STATS)
    std::uint64_t next_call{};

    SupervisorCallHandler supervisor;
    std::uint64_t default_worker_stack{kDefaultStack};

    // ---- process stop (stop_lock) -------------------------------------------
    mutable std::mutex stop_lock;
    std::map<fx::GuestContext*, unsigned> running;   // contexts executing guest code (entry count)
    std::atomic<bool> stopping{false};
    std::atomic<std::uint32_t> stop_status{};
    std::optional<FxEngine::Termination> termination;
    std::function<void(std::uint32_t)> stop_observer;
    std::uint64_t stop_generation{};   // stale interrupts of an earlier run are ignored
    std::uint64_t fault_raise_call{};

    Shared() {
        // Keep the fx code arena and runtime allocations out of the range
        // Mach-O executables prefer (0x1'0000'0000 upward).
        void* placeholder = vm::reserve(host(kLowLimit), 0x100000000ULL, vm::kNoAccess);
        // Instruction families beyond the core: FP, NEON/vector memory and
        // atomics/system. Without them every such instruction takes the
        // one-instruction interpreter, which is far slower (fx's own loader
        // registers them at startup too).
        // ALCHEMY_FX_FAMILIES (diagnostic, default "fp,neon,system") selects
        // them for measurements; "system" is required for correct atomics.
        char families[64]{};
        const auto length = fx::host::get_env("ALCHEMY_FX_FAMILIES", families, sizeof(families));
        const std::string_view selected = length && length < sizeof(families) ? std::string_view(families)
                                                                                : "fp,neon,system";
        if (selected.find("fp") != std::string_view::npos) fx::register_fp_emitters();
        if (selected.find("neon") != std::string_view::npos) fx::register_neon_emitters();
        if (selected.find("system") != std::string_view::npos) fx::register_system_emitters();
        fx::RuntimeOptions options;
        // Mach-O images register as external modules keyed by LC_UUID, so
        // translations persist across runs. ALCHEMY_FX_CACHE=0 disables.
        char cache_setting[8]{};
        const auto cache_length = fx::host::get_env("ALCHEMY_FX_CACHE", cache_setting, sizeof(cache_setting));
        const bool cache = !(cache_length == 1 && cache_setting[0] == '0');
        options.enable_translation_cache = cache;
        // An Alchemy child process does not preload (TRANSMUTE_FX_PRELOAD=0 from
        // run_program, alchemy::child_preload_setting).
        options.preload_cached_regions = cache;
        alchemy::perf::phase("fx runtime initialize: start");
        fx::runtime_initialize(options);
        alchemy::perf::phase("fx runtime initialize: done");
        fx::set_svc_handler(&svc_hook);
        if (placeholder) vm::release(placeholder);

        // Synthetic call slots: `svc #kGuestCallImmediate; brk #0 ...`.
        const auto size = static_cast<std::size_t>(kGuestCallEnd - kGuestCallBase);
        auto* slots = static_cast<std::uint32_t*>(vm::allocate(host(kGuestCallBase), size, vm::kReadWrite));
        if (reinterpret_cast<std::uint64_t>(slots) != kGuestCallBase)
            throw std::runtime_error("alchemy-fx: cannot reserve the synthetic call range");
        for (std::size_t i = 0; i < size / 4; i += 4) {
            slots[i] = 0xd4000001u | (kGuestCallImmediate << 5);
            slots[i + 1] = slots[i + 2] = slots[i + 3] = 0xd4200000u;
        }
        vm::protect(slots, size, vm::kReadOnly);
        fx::register_guest_code_memory(kGuestCallBase, kGuestCallEnd - kGuestCallBase);
        calls.resize(kMaximumGuestCalls);
        alchemy::perf::set_slot_layout(kGuestCallBase, kGuestCallStride);

        // The two highest slots stay reserved (the second is the fault raise
        // continuation), so lower slots keep the reference engine's numbering.
        const auto raise_index = kMaximumGuestCalls - 1;
        g_rethrow_call = kGuestCallBase + (kMaximumGuestCalls - 2) * kGuestCallStride;
        fault_raise_call = kGuestCallBase + raise_index * kGuestCallStride;
        calls[raise_index] = std::make_shared<const GuestCallHandler>([](CpuState& state, GuestMemory&) {
            if (!t_fault) throw std::runtime_error("alchemy-fx: fault continuation without a pending fault");
            const auto fault = std::move(*t_fault);
            t_fault.reset();
            state.pc = fault.pc;   // diagnostics name the faulting instruction
            alchemy::terminate_by_signal(fault.signal, fault.reason);
        });
        call_table[raise_index].store(calls[raise_index].get(), std::memory_order_release);
        fx::set_guest_fault_hook(&Shared::fault_hook, this);
        fx::set_demand_commit_hook(&Shared::demand_commit_hook, this);
        take_load_range_for_test();
    }

    // Diagnostic (tests): ALCHEMY_FX_TEST_TAKE_LOAD_RANGE=<address> reserves
    // 64 KiB there right after the placeholder above is released, as a host
    // thread stack or heap segment would. With the executable's range held
    // (hold_load_range) the reservation is refused and the program loads.
    static void take_load_range_for_test() {
        char text[32]{};
        const auto length = fx::host::get_env("ALCHEMY_FX_TEST_TAKE_LOAD_RANGE", text, sizeof(text));
        if (!length || length >= sizeof(text)) return;
        const auto address = align_down(std::strtoull(text, nullptr, 0), kGranule);
        void* taken = vm::reserve(host(address), static_cast<std::size_t>(kGranule), vm::kNoAccess);
        const bool exact = reinterpret_cast<std::uint64_t>(taken) == address;
        if (taken && !exact) vm::release(taken);
        std::fprintf(stderr, "alchemy-fx: test: 0x%llx %s\n", static_cast<unsigned long long>(address),
                     exact ? "taken by a host reservation" : "refused (held)");
    }

    // fx's demand-commit hook: the first touch of a page in a pending range
    // commits its Darwin page (clipped to the range) with the range's
    // protection. Runs in the fault handler on any thread: no allocation, no
    // guest code, commit_lock only.
    static bool demand_commit_hook(std::uint64_t address, std::uint32_t access, void* user) {
        auto& self = *static_cast<Shared*>(user);
        if (!self.any_pending.load(std::memory_order_acquire)) return false;
        std::unique_lock commit_guard(self.commit_lock);
        const auto* entry = self.pending_at(address);
        if (!entry) return false;
        const auto protection = entry->second.protection;
        // A write to read-only memory faults as it would after the commit.
        if (access == fx::host::kFaultWrite && protection != vm::kReadWrite) return false;
        const auto page = align_down(address, kDarwinPage);
        const auto begin = std::max(entry->first, page), end = std::min(entry->second.end, page + kDarwinPage);
        bool committed = false;
        for (auto at = begin; at < end;) {
            vm::RegionInfo info;
            if (!vm::query(host(at), info) || info.state == vm::State::free) return false;
            const auto run_end = std::min(end, info.base + info.size);
            if (info.state == vm::State::reserved) {
                if (!vm::commit(host(at), static_cast<std::size_t>(run_end - at), protection)) {
                    self.demand_commit_failures.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                self.demand_committed_bytes.fetch_add(run_end - at, std::memory_order_relaxed);
                committed = true;
            }
            at = run_end;
        }
        if (committed) {
            self.demand_commits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Another thread committed the page meanwhile: retry if it allows the
        // access now. Execute faults are fx's own business on committed pages
        // (guest pages are never host executable).
        if (access == fx::host::kFaultExecute) return false;
        vm::RegionInfo info;
        if (!vm::query(host(address), info) || !info.committed() || (info.protect & vm::kGuard)) return false;
        const auto now = info.protect & 0xff;
        return access == fx::host::kFaultWrite ? now == vm::kReadWrite : (now == vm::kReadWrite || now == vm::kReadOnly);
    }

    // ---- pending ranges (memory_lock or commit_lock held to read; both to change)
    const std::pair<const std::uint64_t, Pending>* pending_at(std::uint64_t address) const {
        auto it = pending.upper_bound(address);
        if (it == pending.begin()) return nullptr;
        --it;
        return address < it->second.end ? &*it : nullptr;
    }
    bool pending_overlaps(std::uint64_t begin, std::uint64_t end) const {
        auto it = pending.upper_bound(begin);
        if (it != pending.begin() && std::prev(it)->second.end > begin) return true;
        return it != pending.end() && it->first < end;
    }
    // Lowest pending range start at or above `address` (or `limit`).
    std::uint64_t next_pending(std::uint64_t address, std::uint64_t limit) const {
        const auto it = pending.lower_bound(address);
        return it == pending.end() ? limit : std::min(limit, it->first);
    }
    void erase_pending(std::uint64_t begin, std::uint64_t end) {
        auto it = pending.upper_bound(begin);
        if (it != pending.begin()) --it;
        while (it != pending.end() && it->first < end) {
            const auto range_begin = it->first;
            const auto range = it->second;
            if (range.end <= begin) { ++it; continue; }
            it = pending.erase(it);
            if (range_begin < begin) pending.emplace(range_begin, Pending{begin, range.protection});
            if (range.end > end) {
                pending.emplace(end, Pending{range.end, range.protection});
                break;
            }
        }
        any_pending.store(!pending.empty(), std::memory_order_release);
    }
    void set_pending(std::uint64_t begin, std::uint64_t end, vm::Protection protection) {
        erase_pending(begin, end);
        // Merge with neighbours of the same protection.
        auto it = pending.lower_bound(begin);
        if (it != pending.begin()) {
            const auto before = std::prev(it);
            if (before->second.end == begin && before->second.protection == protection) {
                begin = before->first;
                pending.erase(before);
            }
        }
        it = pending.lower_bound(end);
        if (it != pending.end() && it->first == end && it->second.protection == protection) {
            end = it->second.end;
            pending.erase(it);
        }
        pending.emplace(begin, Pending{end, protection});
        any_pending.store(true, std::memory_order_release);
    }
    void forget_pending(std::uint64_t begin, std::uint64_t end) {   // memory_lock held exclusively
        if (!pending_overlaps(begin, end)) return;
        std::unique_lock commit_guard(commit_lock);
        erase_pending(begin, end);
    }
    // Code registration of the pending (uncommitted) parts of [begin, end),
    // which unregister_code's walk of committed pieces does not see. Call
    // before forget_pending when the memory goes away.
    void unregister_pending_code(std::uint64_t begin, std::uint64_t end) const {   // memory_lock held
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
        auto it = pending.upper_bound(begin);
        if (it != pending.begin()) --it;
        for (; it != pending.end() && it->first < end; ++it)
            if (it->second.end > begin) pieces.emplace_back(std::max(begin, it->first), std::min(end, it->second.end));
        for (const auto& [from, to] : pieces) fx::unregister_guest_code_memory(from, to - from);
    }
    // Makes [begin, end) (page aligned, inside our reservations) accessible
    // with `protection`: on first touch when `lazy`, else committed now
    // (std::bad_alloc when the host cannot commit it).
    void provide(std::uint64_t begin, std::uint64_t end, vm::Protection protection, bool lazy) {
        if (lazy) {
            std::unique_lock commit_guard(commit_lock);
            set_pending(begin, end, protection);
            bool protected_all = true;
            for_each_committed_piece(begin, end, [&](std::uint64_t address, std::uint64_t size) {
                protected_all = vm::protect(host(address), static_cast<std::size_t>(size), protection) && protected_all;
            });
            if (!protected_all) throw std::runtime_error("alchemy-fx: VirtualProtect failed");
            return;
        }
        forget_pending(begin, end);
        if (!commit(begin, end)) throw std::bad_alloc();
        if (!protect(begin, end, protection)) throw std::runtime_error("alchemy-fx: VirtualProtect failed");
    }

    // Last-chance guest fault hook: a fault no guest handler took becomes the
    // Darwin default action for its signal, raised from the dispatcher.
    static bool fault_hook(fx::GuestContext& ctx, const fx::GuestFaultInfo& info, void* user) {
        auto& self = *static_cast<Shared*>(user);
        auto fault = self.classify_fault(info.code, info.parameter_count, info.parameters, info.pc, ctx.x[31]);
        if (!fault) return false;   // fx reports the NT exception and terminates with its code
        t_fault = std::move(*fault);
        ctx.pc = self.fault_raise_call;
        return true;
    }

    // Maps an unhandled guest exception to its Darwin signal and diagnostic.
    std::optional<PendingFault> classify_fault(std::uint32_t code, std::uint32_t count,
                                               const std::uint64_t* parameters, std::uint64_t pc,
                                               std::uint64_t sp) const {
        const auto parameter = [&](unsigned index) -> std::uint64_t {
            return index < count ? parameters[index] : 0;
        };
        std::ostringstream reason;
        reason << std::hex;
        int signal = 0;
        switch (code) {
        case fx::host::kFaultAccessViolation:
        case fx::host::kFaultGuardPage: {
            // Darwin: an unmapped address is KERN_INVALID_ADDRESS (SIGSEGV);
            // a mapped page without the access is KERN_PROTECTION_FAILURE (SIGBUS).
            const auto address = parameter(1);
            vm::RegionInfo info;
            bool mapped = vm::query(host(address), info) && info.committed();
            if (!mapped && address >= kLowLimit) {
                std::shared_lock lock(memory_lock);
                const auto* region = find(address);
                mapped = region && region->mapped;
            }
            signal = mapped ? 10 : 11;
            reason << "EXC_BAD_ACCESS: guest " << access_name(parameter(0)) << " of 0x" << address
                   << (mapped ? " (protection failure)" : " (unmapped)") << " at pc 0x" << pc;
            if (mapped) {
                // What the host and fx know about the page (a protection failure
                // on memory the guest made writable is an Alchemy bug).
                reason << " [host page 0x" << info.base << " protect 0x" << static_cast<std::uint32_t>(info.protect)
                       << " allocation 0x" << info.allocation_base;
                if (const auto saved = fx::guest_page_protection(address)) reason << ", fx write-protected (guest 0x" << saved << ')';
                std::shared_lock lock(memory_lock);
                if (const auto* region = find(address))
                    reason << ", region 0x" << region->base << "+0x" << region->size << (region->readable ? " r" : " -")
                           << (region->writable ? "w" : "-") << (region->executable ? "x" : "-") << (region->lazy ? " lazy" : "")
                           << (region->shared ? " shared" : "");
                reason << ']';
            }
            break;
        }
        case fx::host::kFaultStackOverflow:
            signal = 11;
            reason << "EXC_BAD_ACCESS: guest stack overflow (sp 0x" << sp << ") at pc 0x" << pc;
            break;
        case fx::host::kFaultInPageError:
        case fx::host::kFaultDatatypeMisalignment:
            signal = 10;
            reason << "EXC_BAD_ACCESS: guest " << (code == fx::host::kFaultInPageError
                                                       ? "in-page error" : "misaligned access")
                   << " at 0x" << parameter(1) << ", pc 0x" << pc;
            break;
        case fx::host::kFaultIllegalInstruction:
        case fx::host::kFaultPrivilegedInstruction: {
            signal = 4;
            reason << "EXC_BAD_INSTRUCTION: guest ";
            if (const auto word = guarded_word(pc)) reason << "instruction 0x" << *word;
            else reason << "instruction";
            reason << (code == fx::host::kFaultIllegalInstruction ? " is illegal or unsupported"
                                                                                   : " is privileged")
                   << " at pc 0x" << pc;
            break;
        }
        case fx::host::kFaultBreakpoint: {
            signal = 5;
            reason << "EXC_BREAKPOINT: guest ";
            if (const auto word = guarded_word(pc); word && (*word & 0xffe0001fU) == 0xd4200000U)
                reason << "brk #0x" << ((*word >> 5) & 0xffff);
            else reason << "breakpoint";
            reason << " at pc 0x" << pc;
            break;
        }
        case fx::host::kFaultIntegerDivideByZero:
        case fx::host::kFaultIntegerOverflow:
            signal = 8;
            reason << "EXC_ARITHMETIC: guest integer exception 0x" << code << " at pc 0x" << pc;
            break;
        default:
            return std::nullopt;
        }
        return PendingFault{signal, reason.str(), pc};
    }

    // ---- region table (memory_lock held) -----------------------------------
    const Region* find(std::uint64_t address) const {
        auto it = regions.upper_bound(address);
        if (it == regions.begin()) return nullptr;
        --it;
        return address - it->second.base < std::max<std::uint64_t>(it->second.size, 1) ? &it->second : nullptr;
    }
    bool overlaps(std::uint64_t base, std::uint64_t size) const {
        const auto end = base + size;
        auto it = regions.lower_bound(base);
        if (it != regions.end() && it->second.base < end) return true;
        if (it == regions.begin()) return false;
        --it;
        return it->second.base + std::max<std::uint64_t>(it->second.size, 1) > base;
    }
    // Pages touched by any region other than `skip` in [begin, end).
    bool page_shared(std::uint64_t page, std::uint64_t skip) const {
        for (auto it = regions.lower_bound(page > kPage * 8 ? page - kPage * 8 : 0);
             it != regions.end() && it->second.base < page + kPage; ++it) {
            const auto& r = it->second;
            if (r.base == skip || !r.mapped || r.stack) continue;
            if (r.base < page + kPage && r.base + r.size > page) return true;
        }
        return false;
    }
    bool host_free(std::uint64_t base, std::uint64_t size) const {
        if (base + size <= kLowLimit) return true;
        load_range::release_if_overlaps(base, size);
        auto address = std::max(base, kLowLimit);
        const auto end = base + size;
        while (address < end) {
            vm::RegionInfo info;
            if (!vm::query(host(address), info)) return false;
            const auto region_end = info.base + info.size;
            if (info.state != vm::State::free && !allocations.count(info.allocation_base)) return false;
            address = region_end;
        }
        return true;
    }
    void reserve_granules(std::uint64_t begin, std::uint64_t end, bool releasable = false) {
        load_range::release_if_overlaps(begin, end - begin);
        auto granule = align_down(begin, kGranule) / kGranule;
        const auto last = align_up(end, kGranule) / kGranule;
        while (granule < last) {
            if (granules.contains(granule)) { granule = std::min(last, granules.run_end(granule)); continue; }
            const auto run_end = granules.next(granule, last);
            void* reserved = vm::reserve(host(granule * kGranule), static_cast<std::size_t>((run_end - granule) * kGranule),
                                         vm::kNoAccess);
            if (reinterpret_cast<std::uint64_t>(reserved) != granule * kGranule) {
                if (reserved) vm::release(reserved);
                std::ostringstream message;
                message << "alchemy-fx: host address range 0x" << std::hex << granule * kGranule << "-0x"
                        << run_end * kGranule << " is not available";
                throw std::runtime_error(message.str());
            }
            allocations[granule * kGranule] = Allocation{(run_end - granule) * kGranule, releasable};
            granules.insert(granule, run_end);
            granule = run_end;
        }
    }
    // Decommits the pages of [begin, end) inside our reservations only (a
    // munmap range may span host memory that is not the guest's).
    void decommit_ours(std::uint64_t begin, std::uint64_t end) {
        for_each_committed_piece(begin, end, [this](std::uint64_t address, std::uint64_t size) {
            vm::RegionInfo info;
            if (vm::query(host(address), info) && allocations.count(info.allocation_base))
                vm::decommit(host(address), static_cast<std::size_t>(size));
        });
    }
    // Frees the releasable reservations overlapping [begin, end) that no
    // guest region uses any more (after a Darwin munmap).
    void release_empty(std::uint64_t begin, std::uint64_t end) {
        auto it = allocations.upper_bound(begin);
        if (it != allocations.begin()) --it;
        while (it != allocations.end() && it->first < end) {
            const auto base = it->first;
            const auto allocation = it->second;
            if (!allocation.releasable || base + allocation.extent <= begin || overlaps(base, allocation.extent)) {
                ++it;
                continue;
            }
            invalidate_committed(base, base + allocation.extent);
            forget_pending(base, base + allocation.extent);
            vm::release(host(base));
            granules.erase(base / kGranule, (base + allocation.extent) / kGranule);
            it = allocations.erase(it);
        }
    }
    // Darwin munmap of [begin, end) (page aligned): every region it overlaps
    // loses that part, translations there are dropped, private pages are
    // decommitted and shared views cut (fx::unmap_shared_guest_range).
    // Stacks and bookkeeping-only ranges refuse.
    void unmap_range(std::uint64_t begin, std::uint64_t end) {
        std::vector<Region> cut;
        auto it = regions.upper_bound(begin);
        if (it != regions.begin()) --it;
        for (; it != regions.end() && it->first < end; ++it) {
            const auto& region = it->second;
            if (region.base + std::max<std::uint64_t>(region.size, 1) <= begin) continue;
            if (region.stack || !region.mapped) {
                std::ostringstream message;
                message << "alchemy-fx: partial unmap of a " << (region.stack ? "stack" : "reserved")
                        << " guest region at 0x" << std::hex << region.base;
                throw std::runtime_error(message.str());
            }
            cut.push_back(region);
        }
        // Shared views (file and section mappings, shared guest memory): fx
        // cuts them, keeping the parts outside the range mapped.
        for (const auto& region : cut)
            if (region.shared) {
                const auto from = std::max(begin, region.base), to = std::min(end, region.base + region.size);
                if (to > from) fx::unmap_shared_guest_range(from, to - from);
            }
        for (const auto& region : cut) {
            regions.erase(region.base);
            if (region.base < begin) {
                Region head = region;
                head.size = begin - region.base;
                regions[head.base] = head;
            }
            if (region.base + region.size > end) {
                Region tail = region;
                tail.base = end;
                tail.size = region.base + region.size - end;
                regions[tail.base] = tail;
            }
        }
        unregister_pending_code(begin, end);
        forget_pending(begin, end);
        unregister_code(begin, end);
        invalidate_committed(begin, end);
        auto first = align_down(begin, kPage), last = align_up(end, kPage);
        if (page_shared(first, 0)) first += kPage;
        if (last > first && page_shared(last - kPage, 0)) last -= kPage;
        if (last > first) decommit_ours(first, last);
        release_empty(begin, end);
    }
    // A fresh lazy region at exactly [base, base + size) in free or our own
    // unused address space; false when any of it is in use. Pages are zero
    // filled: leftovers of earlier regions there are dropped first.
    bool reserve_at(std::uint64_t base, std::uint64_t size, bool readable, bool writable) {
        if (!size || base % kPage || base < kLowLimit || base + size < base) return false;
        const auto end = align_up(base + size, kPage);
        if (overlaps(base, size) || !host_free(base, size)) return false;
        try {
            reserve_granules(base, end, true);
        } catch (const std::runtime_error&) {
            return false;   // another host thread took part of it
        }
        invalidate_committed(base, end);
        decommit_ours(base, end);
        forget_pending(base, end);
        if (readable || writable) {
            try {
                provide(base, end, protection(readable, writable), lazy_commit && end - base >= kLazyCommitMinimum);
            } catch (const std::bad_alloc&) {
                release_empty(base, end);
                throw;
            }
        }
        Region region;
        region.base = base;
        region.size = size;
        region.readable = readable;
        region.writable = writable;
        region.lazy = true;
        regions[base] = region;
        return true;
    }
};

namespace {
// The memory line of ALCHEMY_FX_STATS (at exit, and every ALCHEMY_FX_STATS_PERIOD):
// committed bytes of the guest's own memory (the Darwin reservations, mmap and
// Mach VM, and the guest malloc heap), the fx code arena, and the process's
// private bytes and host heap. Private memory beyond the guest's, the arena's and
// the host heap's is the host runtime's other allocations (fx tables, stacks).
void print_memory_statistics(const FxEngine::Shared& shared, const char* when) {
    const auto committed = [](std::uint64_t begin, std::uint64_t end) {
        std::uint64_t total = 0;
        for (auto address = begin; address < end;) {
            vm::RegionInfo info;
            if (!vm::query(host(address), info) || info.size == 0) break;
            const auto next = std::min(end, info.base + info.size);
            if (info.committed()) total += next - std::max(address, info.base);
            address = next;
        }
        return total;
    };
    std::uint64_t reservations = 0, heap = 0;
    {
        std::shared_lock lock(shared.memory_lock);
        for (const auto& [base, allocation] : shared.allocations) reservations += committed(base, base + allocation.extent);
    }
    {
        std::shared_lock lock(shared.heap_lock);
        for (const auto& [base, size] : shared.heap_segments) heap += committed(base, base + size);
    }
    const auto usage = alchemy::host::process::memory_usage();
    std::fprintf(stderr, "alchemy-fx memory: guest reservations %llu KiB, guest heap %llu KiB, fx arena %llu KiB, "
                 "process private %llu KiB, host heap %llu KiB%s\n",
                 static_cast<unsigned long long>(reservations / 1024), static_cast<unsigned long long>(heap / 1024),
                 static_cast<unsigned long long>(fx::runtime_stats().arena_used / 1024),
                 static_cast<unsigned long long>(usage ? usage->private_bytes / 1024 : 0),
                 static_cast<unsigned long long>(usage ? usage->heap_committed_bytes / 1024 : 0), when);
}

// ALCHEMY_FX_STATS=1: demand commits of Darwin reservations and memory at the end of a process (stderr).
void print_demand_commit_statistics(const FxEngine::Shared& shared) {
    char value[8]{};
    if ((fx::host::get_env("ALCHEMY_FX_STATS", value, sizeof(value)) == 0 || value[0] == '0') &&
        !alchemy::perf::enabled())
        return;
    std::fprintf(stderr, "alchemy-fx vm: demand commits %llu (%llu KiB), failed %llu, lazy commit %s\n",
                 static_cast<unsigned long long>(shared.demand_commits.load()),
                 static_cast<unsigned long long>(shared.demand_committed_bytes.load() / 1024),
                 static_cast<unsigned long long>(shared.demand_commit_failures.load()),
                 shared.lazy_commit ? "on" : "off");
    {
        std::shared_lock lock(shared.call_lock);
        std::fprintf(stderr, "alchemy-fx synthetic calls: registered %llu, native entries %llu\n",
                     static_cast<unsigned long long>(shared.next_call),
                     static_cast<unsigned long long>(shared.native_entries.load()));
    }
    char when[32];
    std::snprintf(when, sizeof(when), " (pid %u, exit)", fx::host::process_id());
    print_memory_statistics(shared, when);
}

// One host address space, one fx runtime and one synthetic call range per
// process, so every top-level engine (doctor or run) shares them.
std::shared_ptr<FxEngine::Shared> make_shared_space() {
    static const auto space = std::make_shared<FxEngine::Shared>();
    return space;
}
} // namespace

// ============================================================================
// construction and threads
// ============================================================================

FxEngine::FxEngine(ExecutionStart start)
    : shared_(make_shared_space()), stack_size_(start.stack_size ? start.stack_size : kDefaultStack),
      main_(true), stack_(std::make_unique<Stack>()) {
    state_.pc = start.entry_pc;
    start_periodic_statistics();
    // A new top-level process in this host process starts unstopped.
    std::lock_guard lock(shared_->stop_lock);
    if (shared_->running.empty()) {
        shared_->stopping = false;
        shared_->stop_status = 0;
        shared_->termination.reset();
        shared_->stop_observer = {};
        ++shared_->stop_generation;
    }
}

FxEngine::FxEngine(std::shared_ptr<Shared> shared, std::uint64_t entry, std::uint64_t argument,
                   std::uint64_t stack_size)
    : shared_(std::move(shared)), stack_size_(stack_size ? stack_size : shared_->default_worker_stack),
      stack_(std::make_unique<Stack>()) {
    state_.pc = entry;
    state_.x[0] = argument;
}

FxEngine::~FxEngine() {
    if (main_) {
        {
            std::lock_guard lock(shared_->stop_lock);
            shared_->stop_observer = {};   // it refers to the ending process's state
        }
        print_runtime_statistics();
        print_demand_commit_statistics(*shared_);
        fx::runtime_profile_flush();
    }
    if (stack_ && stack_->stack.allocation) {
        {
            std::unique_lock lock(shared_->memory_lock);
            shared_->regions.erase(stack_->stack.guard_end);
        }
        // A worker's stack is current on its thread only inside run() or an
        // outermost invoke_callback; afterwards it can be freed anywhere.
        if (!stack_->switched) fx::free_guest_stack(stack_->stack);
    }
}

void FxEngine::ensure_stack() {
    std::lock_guard guard(stack_lock_);
    if (stack_ready_) return;
    stack_->stack = fx::allocate_guest_stack(align_up(stack_size_, kGranule));
    {
        std::unique_lock lock(shared_->memory_lock);
        Region region;
        region.base = stack_->stack.guard_end;
        region.size = stack_->stack.base - stack_->stack.guard_end;
        region.stack = true;
        shared_->regions[region.base] = region;
    }
    if (!initial_sp_set_) state_.sp = stack_->stack.base;
    stack_ready_ = true;
}

// Makes this engine's stack the calling thread's guest stack.
void FxEngine::enter_thread() {
    ensure_stack();
    fx::ensure_thread_context();
    if (!stack_->switched) {
        stack_->previous = fx::switch_guest_stack(stack_->stack);
        stack_->switched = true;
    }
}

// Restores the calling thread's previous guest stack after the outermost
// guest entry of a worker (the main engine keeps its stack current).
void FxEngine::leave_thread() noexcept {
    if (!main_ && stack_->switched) {
        fx::switch_guest_stack(stack_->previous);
        stack_->switched = false;
    }
}

std::pair<std::uint64_t, std::uint64_t> FxEngine::stack_bounds() const {
    const_cast<FxEngine*>(this)->ensure_stack();
    return {stack_->stack.guard_end, stack_->stack.base};
}

// ---- process stop -------------------------------------------------------------

bool FxEngine::enter_guest(void* guest_context) {
    auto* ctx = static_cast<fx::GuestContext*>(guest_context);
    std::lock_guard lock(shared_->stop_lock);
    if (shared_->stopping.load()) return false;
    ++shared_->running[ctx];
    ++depth_;
    return true;
}

void FxEngine::leave_guest(void* guest_context) noexcept {
    auto* ctx = static_cast<fx::GuestContext*>(guest_context);
    std::lock_guard lock(shared_->stop_lock);
    if (const auto found = shared_->running.find(ctx); found != shared_->running.end() && --found->second == 0)
        shared_->running.erase(found);
    --depth_;
}

void FxEngine::request_stop(std::uint32_t status) {
    std::function<void(std::uint32_t)> observer;
    {
        std::lock_guard lock(shared_->stop_lock);
        if (shared_->stopping.exchange(true)) return;
        shared_->stop_status = status;
        observer = shared_->stop_observer;
    }
    // The personality records the exit status before any thread sees the stop.
    if (observer) observer(status);
    std::lock_guard lock(shared_->stop_lock);
    const auto generation = shared_->stop_generation;
    auto* shared = shared_.get();
    // Every thread inside guest code, the caller included: a caller that
    // returns into guest code (exit() below a host frame) stops there too.
    for (const auto& [ctx, count] : shared_->running) {
        static_cast<void>(count);
        fx::request_interrupt(*ctx, [shared, generation](fx::GuestContext&) {
            if (shared->stopping.load() && shared->stop_generation == generation) throw ThreadUnwind{};
        });
    }
}

bool FxEngine::stop_requested() const noexcept { return shared_->stopping.load(); }

std::optional<FxEngine::Termination> FxEngine::termination() const {
    std::lock_guard lock(shared_->stop_lock);
    return shared_->termination;
}

void FxEngine::set_stop_observer(std::function<void(std::uint32_t)> observer) {
    std::lock_guard lock(shared_->stop_lock);
    shared_->stop_observer = std::move(observer);
}

void FxEngine::record_termination(int signal, std::uint32_t status, const std::string& reason) {
    {
        std::lock_guard lock(shared_->stop_lock);
        if (!shared_->termination)
            shared_->termination = Termination{status, signal, reason, state_.pc, state_.x[30], state_.x[29], main_};
    }
    request_stop(status);
}

void FxEngine::unwind_current_thread(bool) {
    if (t_current && t_current->depth_ > 0) throw ThreadUnwind{};
}

void FxEngine::force_exit(std::uint32_t status, std::string_view diagnostic) {
    std::cout.flush();
    if (!diagnostic.empty()) {
        std::cerr.write(diagnostic.data(), static_cast<std::streamsize>(diagnostic.size()));
        std::cerr << std::endl;
    }
    std::cerr.flush();
    std::fflush(nullptr);
    print_runtime_statistics();
    fx::runtime_prepare_exit();   // stops the preloader before the cache flush, as fx's loader does
    fx::cache_flush();
    fx::runtime_profile_flush();
    // Darwin ends every thread at exit; threads still blocked in host calls
    // cannot be joined, so the host process ends without running its exit code.
    fx::host::terminate_process(status);
    std::abort();
}

void FxEngine::set_initial_register(unsigned index, std::uint64_t value) {
    if (index >= 31) throw std::out_of_range("initial register index");
    state_.x[index] = value;
}

void FxEngine::set_initial_vector(unsigned index, std::array<std::uint64_t, 2> value) {
    if (index >= 32) throw std::out_of_range("initial vector register index");
    state_.v[index] = value;
}

void FxEngine::set_initial_stack_pointer(std::uint64_t value) {
    state_.sp = value;
    initial_sp_set_ = true;
}

void FxEngine::set_supervisor_call_handler(SupervisorCallHandler handler) {
    shared_->supervisor = std::move(handler);
}

// fx counts per process, so only the main engine reports (workers add zero):
// native blocks = translated regions, fallback = interpreted instructions.
JitStats FxEngine::jit_stats(std::size_t) const {
    JitStats stats{};
    if (!main_) return stats;
    const auto runtime = fx::runtime_stats();
    stats.native_blocks = runtime.translations;
    stats.compiled_blocks = runtime.translations;
    stats.native_guest_instructions = runtime.jit_guest_instructions;
    stats.fallback_instructions = runtime.interpreted;
    return stats;
}

void FxEngine::resize_stack(std::uint64_t size) {
    if (stack_ready_) throw std::runtime_error("alchemy-fx: stack already allocated");
    stack_size_ = size;
}

void FxEngine::set_default_worker_stack_size(std::uint64_t size) { shared_->default_worker_stack = size; }
std::uint64_t FxEngine::default_worker_stack_size() const noexcept { return shared_->default_worker_stack; }
FxEngine* FxEngine::current() noexcept { return t_current; }

std::unique_ptr<FxEngine> FxEngine::fork_thread(std::uint64_t entry, std::uint64_t argument,
                                                std::uint64_t stack_size) {
    return std::unique_ptr<FxEngine>(new FxEngine(shared_, entry, argument, stack_size));
}

RunResult FxEngine::run(std::uint64_t) {
    ActiveEngine active(this);
    const bool outermost = depth_ == 0;
    enter_thread();
    auto& ctx = fx::ensure_thread_context();
    const ScopeExit restore_stack{[&]() noexcept { if (outermost) leave_thread(); }};
    if (!enter_guest(&ctx)) {
        // The process is stopping: this thread runs no guest code.
        if (!outermost) throw ThreadUnwind{};
        return {shared_->stop_status.load(), 0};
    }
    const ScopeExit leave{[&]() noexcept { leave_guest(&ctx); }};
    store_state(state_, ctx);
    std::uint32_t exit_code = 0;
    try {
        fx::call_guest_prepared(ctx, state_.pc);
        load_state(state_, ctx);
        state_.pc = fx::kReturnToHostPc;
        exit_code = static_cast<std::uint32_t>(state_.x[0]);
    } catch (const GuestExit& exit) {
        exit_code = exit.code;
    } catch (const ThreadUnwind&) {
        // Process stop at a safe point, or pthread_exit below a host frame
        // (the thread's exit value is already recorded by the personality).
        if (!outermost) throw;
        exit_code = shared_->stopping.load() ? shared_->stop_status.load() : static_cast<std::uint32_t>(state_.x[0]);
    }
    return {exit_code, 0};
}

void FxEngine::set_thread_pointer(std::uint64_t value) { fx::ensure_thread_context().tpidr_el0 = value; }

std::uint64_t FxEngine::invoke_callback(std::uint64_t address, std::uint64_t argument) {
    return invoke_callback(address, std::span(&argument, 1));
}

std::uint64_t FxEngine::invoke_callback(std::uint64_t address, std::span<const std::uint64_t> arguments) {
    if (arguments.size() > 8) throw std::runtime_error("guest callback has too many register arguments");
    ActiveEngine active(this);
    const bool outermost = depth_ == 0;
    enter_thread();
    auto& ctx = fx::ensure_thread_context();
    const ScopeExit restore_stack{[&]() noexcept { if (outermost) leave_thread(); }};
    if (!enter_guest(&ctx)) {
        // The process is stopping. Inside guest execution the thread unwinds;
        // host-initiated callbacks (initializers, TSD destructors of a
        // stopped thread) do not run, as on Darwin where exit ends them.
        if (!outermost) throw ThreadUnwind{};
        return 0;
    }
    const ScopeExit leave{[&]() noexcept { leave_guest(&ctx); }};
    const auto saved = state_;
    const auto outer_sp = ctx.x[31];
    const ScopeExit restore{[&]() noexcept {
        ctx.x[31] = outer_sp;
        state_ = saved;
    }};
    // Like the reference engine: below the current guest SP, past its red zone.
    ctx.x[31] = (state_.sp - 256) & ~15ULL;
    for (std::size_t i = 0; i < arguments.size(); ++i) ctx.x[i] = arguments[i];
    try {
        fx::call_guest_prepared(ctx, address);
        return ctx.x[0];
    } catch (const GuestExit&) {
        return state_.x[0];
    } catch (const ThreadUnwind&) {
        if (!outermost) throw;
        return 0;
    }
}

alchemy::CallResult FxEngine::invoke_callback_regs(std::uint64_t address, const alchemy::CallRegs& registers) {
    constexpr std::size_t kMaximumStackArguments = 64 * 1024;
    if (registers.stack.size() > kMaximumStackArguments)
        throw std::runtime_error("guest callback stack arguments exceed 64 KiB");
    ActiveEngine active(this);
    const bool outermost = depth_ == 0;
    enter_thread();
    auto& ctx = fx::ensure_thread_context();
    const ScopeExit restore_stack{[&]() noexcept { if (outermost) leave_thread(); }};
    alchemy::CallResult result{};
    result.complete = true;
    if (!enter_guest(&ctx)) {
        // As invoke_callback: unwind inside guest execution, else do not run.
        if (!outermost) throw ThreadUnwind{};
        return result;
    }
    const ScopeExit leave{[&]() noexcept { leave_guest(&ctx); }};
    const auto saved = state_;
    const auto outer_sp = ctx.x[31];
    const ScopeExit restore{[&]() noexcept {
        ctx.x[31] = outer_sp;
        state_ = saved;
    }};
    // Like invoke_callback, below the current guest SP past its red zone; the
    // stack arguments start at the new SP.
    const auto stack_bytes = align_up(registers.stack.size(), 16);
    const auto sp = ((state_.sp - 256) & ~15ULL) - stack_bytes;
    if (!registers.stack.empty()) write_bytes(sp, registers.stack);
    ctx.x[31] = sp;
    for (std::size_t i = 0; i < registers.x.size(); ++i) ctx.x[i] = registers.x[i];
    ctx.x[8] = registers.indirect_result;
    for (std::size_t i = 0; i < registers.v.size(); ++i) {
        ctx.v[i][0] = registers.v[i][0];
        ctx.v[i][1] = registers.v[i][1];
    }
    try {
        fx::call_guest_prepared(ctx, address);
        result.x0 = ctx.x[0];
        result.x1 = ctx.x[1];
        for (std::size_t i = 0; i < result.v.size(); ++i) result.v[i] = {ctx.v[i][0], ctx.v[i][1]};
        return result;
    } catch (const GuestExit&) {
        result.x0 = state_.x[0];
        return result;
    } catch (const ThreadUnwind&) {
        if (!outermost) throw;
        return result;
    }
}

std::uint64_t FxEngine::register_guest_call(GuestCallHandler handler) {
    if (!handler) throw std::runtime_error("synthetic guest call handler is empty");
    std::unique_lock lock(shared_->call_lock);
    if (shared_->next_call == kMaximumGuestCalls - 2)   // the top two slots belong to the fault filter
        throw std::runtime_error("synthetic guest call address space exhausted");
    const auto index = shared_->next_call++;
    shared_->calls[index] = std::make_shared<const GuestCallHandler>(std::move(handler));
    shared_->call_table[index].store(shared_->calls[index].get(), std::memory_order_release);
    // The native entry comes with the first call (install_native_entry).
    return kGuestCallBase + index * kGuestCallStride;
}

void FxEngine::install_native_entry(std::uint64_t pc) {
    if (!native_synthetic_calls() || pc < kGuestCallBase || pc >= kGuestCallEnd) return;
    const auto index = (pc - kGuestCallBase) / kGuestCallStride;
    if (shared_->native_entry[index].load(std::memory_order_acquire)) return;
    std::unique_lock lock(shared_->call_lock);
    // Only registered calls: the two reserved top slots stay on the SVC path
    // (the rethrow must leave through the dispatcher).
    if (index >= shared_->next_call || !shared_->calls[index] || shared_->native_entry[index].load(std::memory_order_relaxed))
        return;
    fx::register_host_callback(pc, &FxEngine::host_callback, reinterpret_cast<void*>(pc));
    shared_->native_entry[index].store(1, std::memory_order_release);
    shared_->native_entries.fetch_add(1, std::memory_order_relaxed);
}

// Native host-callback entry of a synthetic call: called from translated code
// on the guest thread, without a dispatcher exit.
void FxEngine::host_callback(fx::GuestContext& ctx, void* user) {
    const auto pc = reinterpret_cast<std::uint64_t>(user);
    bool failed = false;
    {
        try {
            auto* engine = t_current;
            if (!engine) throw std::runtime_error("alchemy-fx: synthetic call on a thread without an engine");
            engine->invoke_synthetic(&ctx, pc);
        } catch (...) {
            t_callback_error = std::current_exception();
            t_callback_pc = pc;
            failed = true;
        }
    }
    if (failed) {
        // Every local is destroyed; leave translated code and rethrow from the
        // dispatcher, where exceptions propagate to run()/invoke_callback.
        ctx.pc = g_rethrow_call;
        fx::exit_to_dispatcher(ctx, fx::ExitReason::redispatch, 0);
    }
}

void FxEngine::unregister_guest_call(std::uint64_t address) {
    std::unique_lock lock(shared_->call_lock);
    const auto index = (address - kGuestCallBase) / kGuestCallStride;
    if (address < kGuestCallBase || address >= kGuestCallEnd || !shared_->calls[index])
        throw std::runtime_error("unknown synthetic guest call address");
    shared_->call_table[index].store(nullptr, std::memory_order_release);
    // A call may still be running the handler on another thread.
    shared_->retired_calls.push_back(std::move(shared_->calls[index]));
    shared_->calls[index].reset();
    if (shared_->native_entry[index].exchange(0, std::memory_order_acq_rel)) fx::unregister_host_entry(address);
}

void FxEngine::invoke_synthetic(void* guest_context, std::uint64_t pc) {
    auto& ctx = *static_cast<fx::GuestContext*>(guest_context);
    const auto perf_frame = alchemy::perf::slot_enter(static_cast<std::uint32_t>((pc - kGuestCallBase) / kGuestCallStride));
    struct PerfLeave {
        const alchemy::perf::SlotFrame& frame;
        ~PerfLeave() { alchemy::perf::slot_leave(frame); }
    } perf_leave{perf_frame};
    const auto index = (pc - kGuestCallBase) / kGuestCallStride;
    const bool vectors = !shared_->integer_only[index].load(std::memory_order_relaxed);
    load_state(state_, ctx, vectors);
    state_.pc = pc;
    const auto* handler = shared_->call_table[index].load(std::memory_order_acquire);
    if (!handler) {
        std::ostringstream message;
        message << "unknown synthetic guest call address 0x" << std::hex << pc;
        throw std::runtime_error(message.str());
    }
    try {
        (*handler)(state_, *this);
    } catch (const alchemy::GuestSignalTermination& termination) {
        // Keeps its type; the first one on any thread stops the process.
        record_termination(termination.signal(), termination.status(), termination.what());
        throw;
    } catch (const std::exception& error) {
        std::ostringstream message;
        message << error.what() << " during synthetic guest call 0x" << std::hex << pc;
        if (const auto name = alchemy::perf::address_name(pc); !name.empty()) message << " (" << name << ')';
        throw std::runtime_error(message.str());
    }
    state_.pc = state_.x[30];
    store_state(state_, ctx, vectors);
    ctx.pc = state_.pc;
}

void FxEngine::set_guest_call_integer_only(std::uint64_t address) {
    if (address < kGuestCallBase || address >= kGuestCallEnd) return;
    shared_->integer_only[(address - kGuestCallBase) / kGuestCallStride].store(1, std::memory_order_relaxed);
}

void FxEngine::handle_svc(void* guest_context, std::uint32_t immediate) {
    auto& ctx = *static_cast<fx::GuestContext*>(guest_context);
    const auto pc = ctx.pc - 4;
    if (immediate == kGuestCallImmediate && pc == g_rethrow_call && t_callback_error) {
        auto error = std::exchange(t_callback_error, nullptr);
        state_.pc = std::exchange(t_callback_pc, 0);
        std::rethrow_exception(error);
    }
    if (immediate == kGuestCallImmediate && pc >= kGuestCallBase && pc < kGuestCallEnd) {
        install_native_entry(pc);   // later calls skip the dispatcher
        invoke_synthetic(guest_context, pc);
        return;
    }
    load_state(state_, ctx);
    state_.pc = pc;
    if (!shared_->supervisor) throw std::runtime_error("guest supervisor call has no handler");
    std::optional<std::uint32_t> exit;
    try {
        exit = shared_->supervisor(SupervisorCall{pc, static_cast<std::uint16_t>(immediate)}, state_, *this);
    } catch (const alchemy::GuestSignalTermination& termination) {
        record_termination(termination.signal(), termination.status(), termination.what());
        throw;
    } catch (const std::exception& error) {
        std::ostringstream message;
        message << error.what() << " during supervisor call #0x" << std::hex << immediate
                << " at guest address 0x" << pc;
        throw std::runtime_error(message.str());
    }
    state_.pc = pc + 4;
    store_state(state_, ctx);
    ctx.pc = state_.pc;
    if (exit) throw GuestExit{*exit};
}

// ============================================================================
// memory
// ============================================================================

void FxEngine::map_memory(std::uint64_t base, std::size_t size) {
    if (size == 0) throw std::runtime_error("cannot map an empty guest region");
    std::unique_lock lock(shared_->memory_lock);
    if (shared_->overlaps(base, size)) throw std::runtime_error("guest mapping overlaps an existing region");
    const auto begin = align_down(base, kPage);
    const auto end = align_up(base + size, kPage);
    shared_->reserve_granules(begin, end);
    if (!commit(begin, end)) {
        std::ostringstream message;
        message << "alchemy-fx: cannot commit guest range 0x" << std::hex << base << "+0x" << size
                << " (error " << std::dec << fx::host::last_error() << ')';
        throw std::runtime_error(message.str());
    }
    // A partially shared first/last page may hold stale bytes from an earlier region.
    std::memset(host(base), 0, size);
    Region region;
    region.base = base;
    region.size = size;
    shared_->regions[base] = region;
}

void FxEngine::reserve_memory(std::uint64_t base, std::uint64_t size) {
    std::unique_lock lock(shared_->memory_lock);
    if (shared_->overlaps(base, size)) throw std::runtime_error("guest reservation overlaps an existing region");
    if (base + size > kLowLimit) shared_->reserve_granules(std::max(base, kLowLimit), base + size);
    Region region;
    region.base = base;
    region.size = size;
    region.readable = region.writable = false;
    region.mapped = false;
    shared_->regions[base] = region;
}

void FxEngine::map_shared_memory(std::uint64_t base, std::uint64_t size, std::uint64_t granule) {
    if (size == 0 || base % kPage || size % kPage || (granule && (granule % kPage || size % granule)))
        throw std::invalid_argument("alchemy-fx: shared guest memory must be page aligned");
    const auto unit = granule ? granule : size;
    std::unique_lock lock(shared_->memory_lock);
    if (shared_->overlaps(base, size)) throw std::runtime_error("guest mapping overlaps an existing region");
    load_range::release_if_overlaps(base, size);
    fx::map_shared_guest_memory(base, size, unit, vm::kReadWrite);   // zero-filled
    for (auto at = base; at < base + size; at += unit) {
        Region region;
        region.base = at;
        region.size = unit;
        region.shared = true;
        shared_->regions[at] = region;
    }
}

bool FxEngine::is_shared_memory(std::uint64_t base, std::uint64_t size) const {
    std::shared_lock lock(shared_->memory_lock);
    for (auto at = base; at < base + size;) {
        const auto* region = shared_->find(at);
        if (!region || !region->shared || region->copy_on_write) return false;
        at = region->base + region->size;
    }
    return size != 0;
}

void FxEngine::alias_memory(std::uint64_t destination, std::uint64_t source, std::uint64_t size, bool readable,
                            bool writable, bool executable) {
    std::unique_lock lock(shared_->memory_lock);
    // Source: shared regions; the destination's regions follow their boundaries.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;   // destination base, size
    for (auto at = source; at < source + size;) {
        const auto* region = shared_->find(at);
        if (!region || !region->shared || region->copy_on_write)
            throw std::runtime_error("alchemy-fx: alias source is not shared guest memory");
        const auto end = std::min(source + size, region->base + region->size);
        pieces.emplace_back(destination + (at - source), end - at);
        at = end;
    }
    // Destination: free, or whole shared regions it replaces (VM_FLAGS_OVERWRITE).
    std::vector<std::uint64_t> replaced;
    for (auto it = shared_->regions.lower_bound(destination); it != shared_->regions.end() && it->first < destination + size; ++it) {
        if (!it->second.shared || it->second.copy_on_write || it->first + it->second.size > destination + size)
            throw std::runtime_error("alchemy-fx: alias destination overlaps private guest memory");
        replaced.push_back(it->first);
    }
    if (const auto* before = shared_->find(destination); before && before->base < destination)
        throw std::runtime_error("alchemy-fx: alias destination splits a guest region");
    fx::alias_guest_memory(destination, source, size, protection(readable, writable));
    for (const auto base : replaced) {
        if (shared_->regions[base].executable) fx::unregister_guest_code_memory(base, shared_->regions[base].size);
        shared_->regions.erase(base);
    }
    for (const auto& [base, length] : pieces) {
        Region region;
        region.base = base;
        region.size = length;
        region.readable = readable;
        region.writable = writable;
        region.executable = executable;
        region.shared = true;
        shared_->regions[base] = region;
    }
    if (executable) fx::register_guest_code_memory(destination, size);
}

std::uint64_t FxEngine::map_section(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                                    std::uint64_t view_size, bool writable_section, bool readable, bool writable) {
    if (size == 0 || base % kPage || size % kPage)
        throw std::invalid_argument("alchemy-fx: a section mapping must be page aligned");
    std::unique_lock lock(shared_->memory_lock);
    if (base && shared_->overlaps(base, size)) throw std::runtime_error("guest mapping overlaps an existing region");
    load_range::release_if_overlaps(base, size);
    const auto mapped = fx::map_section_guest_memory(base, size, section, offset, view_size, writable_section,
                                                     protection(readable, writable));
    if (!base && shared_->overlaps(mapped, size)) {
        // Host-chosen space inside a bookkeeping-only guest range (__PAGEZERO).
        fx::unmap_shared_guest_memory(mapped, size);
        throw std::runtime_error("alchemy-fx: the host placed a section view inside a reserved guest range");
    }
    Region region;
    region.base = mapped;
    region.size = size;
    region.readable = readable;
    region.writable = writable;
    region.shared = true;
    shared_->regions[mapped] = region;
    return mapped;
}

void FxEngine::map_file_copy(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                             std::uint64_t file_bytes) {
    if (size == 0 || base % kPage || size % kPage)
        throw std::invalid_argument("alchemy-fx: a file mapping must be page aligned");
    std::unique_lock lock(shared_->memory_lock);
    if (shared_->overlaps(base, size)) throw std::runtime_error("guest mapping overlaps an existing region");
    load_range::release_if_overlaps(base, size);
    fx::map_file_copy_guest_memory(base, size, section, offset, file_bytes);
    Region region;
    region.base = base;
    region.size = size;
    region.shared = true;
    region.copy_on_write = true;
    shared_->regions[base] = region;
}

void FxEngine::map_section_pages(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                                 std::uint64_t granule, bool writable_section, bool readable, bool writable) {
    if (!base || size == 0 || base % kPage || size % kPage || !granule || size % granule)
        throw std::invalid_argument("alchemy-fx: section pages must be page aligned");
    std::unique_lock lock(shared_->memory_lock);
    if (shared_->overlaps(base, size)) throw std::runtime_error("guest mapping overlaps an existing region");
    load_range::release_if_overlaps(base, size);
    fx::map_section_guest_pages(base, size, section, offset, granule, writable_section, protection(readable, writable));
    for (auto at = base; at < base + size; at += granule) {
        Region region;
        region.base = at;
        region.size = granule;
        region.readable = readable;
        region.writable = writable;
        region.shared = true;
        shared_->regions[at] = region;
    }
}

void* FxEngine::duplicate_section(std::uint64_t address, std::uint64_t& offset, std::uint64_t& size,
                                  bool& writable) const {
    return fx::duplicate_shared_guest_section(address, &offset, &size, &writable);
}

void FxEngine::map_external_memory(std::uint64_t, std::uint8_t*, std::size_t, bool) {
    throw std::runtime_error("alchemy-fx: external guest mappings are not supported");
}

void FxEngine::unmap_memory(std::uint64_t base) {
    std::unique_lock lock(shared_->memory_lock);
    const auto found = shared_->regions.find(base);
    if (found == shared_->regions.end()) throw std::runtime_error("unmap of an unknown guest region");
    const Region region = found->second;
    shared_->regions.erase(found);
    if (region.stack) return;
    if (region.executable) {
        fx::unregister_guest_code_memory(region.base, region.size);
        fx::invalidate_guest_code(region.base, region.size);
    }
    if (region.shared) {
        // One fx view (or several whole ones); other aliases keep the pages.
        fx::unmap_shared_guest_memory(region.base, region.size);
        return;
    }
    if (!region.mapped) return;
    auto begin = align_down(region.base, kPage);
    auto end = align_up(region.base + region.size, kPage);
    // A reservation's pages may have become code through protect_range,
    // which the region record does not show.
    if (region.lazy) {
        unregister_code(begin, end);
        shared_->unregister_pending_code(begin, end);
        shared_->forget_pending(begin, end);
    }
    if (shared_->page_shared(begin, base)) begin += kPage;
    if (end > begin && shared_->page_shared(end - kPage, base)) end -= kPage;
    if (end > begin) decommit(begin, end);
    if (region.lazy) shared_->release_empty(region.base, region.base + region.size);
}

void FxEngine::hold_load_range(std::optional<std::uint64_t> load_base) noexcept {
    if (load_range::held.load(std::memory_order_acquire)) return;
    const auto base = align_down(load_base.value_or(kLowLimit), kGranule);
    if (base < kLowLimit || base > (std::uint64_t{1} << 47) - load_range::kSize) return;
    // A base inside the engine's executable window [4 GiB, 8 GiB) holds the
    // window from its start too, as the engine's own placeholder does.
    const auto begin = base < kLowLimit + load_range::kSize ? kLowLimit : base;
    const auto end = base + load_range::kSize;
    // Diagnostic (tests): ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD=<address> takes
    // 64 KiB there first, as an allocation made before main would.
    char text[32]{};
    if (const auto length = fx::host::get_env("ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD", text, sizeof(text)); length && length < sizeof(text))
        static_cast<void>(vm::reserve(host(align_down(std::strtoull(text, nullptr, 0), kGranule)), static_cast<std::size_t>(kGranule),
                                      vm::kNoAccess));
    // Every free part of the range: allocations made before main (loader
    // threads, the first heap segments) may already sit in it.
    std::size_t count = 0;
    for (auto address = begin; address < end && count < load_range::kMaxPieces;) {
        vm::RegionInfo info;
        if (!vm::query(host(address), info)) break;
        const auto region_end = std::min(info.base + info.size, end);
        if (info.state == vm::State::free) {
            const auto piece_begin = align_up(address, kGranule), piece_end = align_down(region_end, kGranule);
            if (piece_end > piece_begin) {
                void* piece = vm::reserve(host(piece_begin), static_cast<std::size_t>(piece_end - piece_begin), vm::kNoAccess);
                if (reinterpret_cast<std::uint64_t>(piece) == piece_begin) {
                    load_range::pieces[count] = piece;
                    load_range::piece_sizes[count++] = piece_end - piece_begin;
                } else if (piece) {
                    vm::release(piece);
                }
            }
        }
        address = region_end;
    }
    if (!count) return;
    load_range::piece_count = count;
    load_range::begin = begin;
    load_range::end = end;
    load_range::held.store(true, std::memory_order_release);
}

std::optional<std::uint64_t> FxEngine::slide_main_image(std::uint64_t preferred, std::uint64_t size) noexcept {
    if (!load_range::held.load(std::memory_order_acquire) || size == 0) return std::nullopt;
    for (std::size_t index = 0; index < load_range::piece_count; ++index) {
        const auto piece = reinterpret_cast<std::uint64_t>(load_range::pieces[index]);
        if (preferred >= piece && preferred + size <= piece + load_range::piece_sizes[index]) return std::nullopt;
    }
    // Something placed before main sits in the image's range (hold_load_range
    // could only hold the rest): named on stderr, then the lowest held part
    // that fits the image.
    for (auto address = preferred; address < preferred + size;) {
        vm::RegionInfo info;
        if (!vm::query(host(address), info) || info.size == 0) break;
        if (info.state != vm::State::free) {
            bool held = false;
            for (std::size_t index = 0; index < load_range::piece_count; ++index)
                held = held || info.allocation_base == reinterpret_cast<std::uint64_t>(load_range::pieces[index]);
            if (!held) {
                std::fprintf(stderr, "alchemy-fx: host memory at 0x%llx (+0x%llx, allocation 0x%llx, %s, protect 0x%x) "
                             "sits in the executable's range\n", static_cast<unsigned long long>(info.base),
                             static_cast<unsigned long long>(info.size), static_cast<unsigned long long>(info.allocation_base),
                             info.committed() ? "committed" : "reserved", static_cast<unsigned>(info.protect));
                break;
            }
        }
        address = info.base + info.size;
    }
    const auto extent = align_up(size, kGranule);
    for (std::size_t index = 0; index < load_range::piece_count; ++index)
        if (load_range::piece_sizes[index] >= extent) return reinterpret_cast<std::uint64_t>(load_range::pieces[index]);
    return std::nullopt;
}

bool FxEngine::is_range_available(std::uint64_t base, std::uint64_t size) const {
    if (size == 0 || base + size < base) return false;
    std::shared_lock lock(shared_->memory_lock);
    return !shared_->overlaps(base, size) && shared_->host_free(base, size);
}

std::optional<GuestMemory::RegionInfo> FxEngine::query_region(std::uint64_t address) const {
    std::shared_lock lock(shared_->memory_lock);
    const auto* region = shared_->find(address);
    if (!region) {
        // A guest heap block (heap_allocate): the heap reservation is
        // read/write guest memory. Uncommitted heap pages inside it are never
        // handed to the guest, so the whole reservation is reported.
        std::shared_lock heap(shared_->heap_lock);
        if (const auto* segment = shared_->heap_segment(address))
            return RegionInfo{segment->first, segment->second, true, true, false, false, false};
        return std::nullopt;
    }
    if (region->lazy) {
        // Report the page's actual state: parts commit through protect_range.
        vm::RegionInfo info;
        bool readable = false, writable = false;
        if (vm::query(host(address), info) && info.committed()) {
            // A page fx write-protected for SMC is still writable to the guest.
            const auto saved = fx::guest_page_protection(address);
            const auto page = (saved ? saved : info.protect) & 0xff;
            writable = page == vm::kReadWrite;
            readable = writable || page == vm::kReadOnly;
        } else if (const auto* entry = shared_->pending_at(address)) {
            // Accessible, committed on first touch.
            writable = entry->second.protection == vm::kReadWrite;
            readable = true;
        }
        return RegionInfo{region->base, region->size, readable, writable, region->executable, false, true};
    }
    return RegionInfo{region->base, region->size, region->readable, region->writable,
                      region->executable, false, region->mapped};
}

void FxEngine::set_memory_access(std::uint64_t base, bool readable, bool writable, bool executable) {
    std::unique_lock lock(shared_->memory_lock);
    auto* found = const_cast<Region*>(shared_->find(base));
    if (!found || !found->mapped) throw std::runtime_error("access change for an unknown guest region");
    Region& region = *found;
    const auto begin = align_down(region.base, kPage);
    const auto end = align_up(region.base + region.size, kPage);
    if (region.shared) {
        // Section views only: the inaccessible tail of a file mapping stays a placeholder.
        const bool changed = for_each_allocation_piece(begin, end, [&](std::uint64_t address, std::uint64_t size) {
            vm::RegionInfo info;
            if (!vm::query(host(address), info)) return false;
            if (!info.committed()) return true;
            return vm::protect(host(address), static_cast<std::size_t>(size), protection(readable, writable));
        });
        if (!changed) throw std::runtime_error("alchemy-fx: VirtualProtect failed");
    } else if (!protect(begin, end, protection(readable, writable)))
        throw std::runtime_error("alchemy-fx: VirtualProtect failed");
    if (!writable) fx::guest_protection_lowered(region.base, region.size);   // as in protect_range
    if (executable && !region.executable) fx::register_guest_code_memory(region.base, region.size);
    if (!executable && region.executable) fx::unregister_guest_code_memory(region.base, region.size);
    // fx write-protects translated pages; a writable code region must drop them.
    if (region.executable && (writable || !executable)) fx::invalidate_guest_code(region.base, region.size);
    region.readable = readable;
    region.writable = writable;
    region.executable = executable;
}

std::uint64_t FxEngine::reserve_anywhere(std::uint64_t size, std::uint64_t alignment, bool readable,
                                         bool writable) {
    if (!size || alignment > kGranule) throw std::runtime_error("invalid guest reservation");
    const auto extent = align_up(size, kGranule);
    std::unique_lock lock(shared_->memory_lock);
    // Above __PAGEZERO: Darwin never hands out the low 4 GiB, and Swift uses
    // pointers below it as enum payloads.
    auto* reserved = vm::reserve_above(kLowLimit, static_cast<std::size_t>(extent));
    if (!reserved) throw std::bad_alloc();
    const auto base = reinterpret_cast<std::uint64_t>(reserved);
    shared_->allocations[base] = Shared::Allocation{extent, true};
    shared_->granules.insert(base / kGranule, (base + extent) / kGranule);
    if (readable || writable) {
        const auto end = base + align_up(size, kPage);
        try {
            shared_->provide(base, end, protection(readable, writable),
                             shared_->lazy_commit && end - base >= kLazyCommitMinimum);
        } catch (const std::bad_alloc&) {
            shared_->release_empty(base, base + extent);
            throw;
        }
    }
    Region region;
    region.base = base;
    region.size = size;
    region.readable = readable;
    region.writable = writable;
    region.lazy = true;
    shared_->regions[base] = region;
    return base;
}

void FxEngine::register_image(std::uint64_t header, std::uint64_t begin, std::uint64_t end, std::uint64_t text_begin,
                              std::uint64_t text_end, const std::string& path) {
    if (end <= begin) return;
    // Identity: LC_UUID (the linker's hash of the image contents). Without one
    // the module is named but not cached.
    std::uint64_t identity = 0;
    try {
        const auto commands = read32(header + 16);
        auto command = header + 32;
        for (std::uint32_t index = 0; index < commands && index < 4096; ++index) {
            const auto kind = read32(command), size = read32(command + 4);
            if (size < 8) break;
            if (kind == 0x1b && size >= 24) {
                std::uint64_t hash = 0xcbf29ce484222325ULL;
                for (unsigned i = 0; i < 16; ++i) {
                    hash ^= read8(command + 8 + i);
                    hash *= 0x100000001b3ULL;
                }
                identity = hash ? hash : 1;
                break;
            }
            command += size;
        }
    } catch (const std::exception&) {
        identity = 0;
    }
    fx::GuestCodeModule module;
    module.base = begin;
    module.size = end - begin;
    if (text_begin >= begin && text_end <= end && text_begin < text_end) {
        module.text_begin = text_begin;
        module.text_end = text_end;
    }
    module.image_hash = identity;
    module.path = fx::host::from_utf8(path);
    const auto slash = module.path.find_last_of(L"\\/");
    module.name = slash == std::wstring::npos ? module.path : module.path.substr(slash + 1);
    try {
        fx::register_external_module(module);
    } catch (const std::invalid_argument&) {
        // Overlap with an earlier registration (e.g. a failed load's leftover):
        // the image still runs, only unnamed and uncached.
        return;
    }
    // Background translation into the persistent cache (fx's external-module
    // AOT): function starts (LC_FUNCTION_STARTS) for region bounds and the
    // static phase, initializers and the entry point as the first roots.
    // fx does nothing for images that already have a main cache file.
    if (identity == 0) return;
    fx::ExternalAotRequest request;
    request.base = module.base;
    request.size = module.size;
    request.text_begin = module.text_begin;
    request.text_end = module.text_end;
    try {
        std::uint64_t slide = 0, linkedit_address = 0, linkedit_offset = 0, starts_offset = 0, starts_size = 0;
        bool have_text = false, have_linkedit = false;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pointer_arrays, offset_arrays;   // address, bytes
        const auto commands = read32(header + 16);
        auto command = header + 32;
        for (std::uint32_t index = 0; index < commands && index < 4096; ++index) {
            const auto kind = read32(command), size = read32(command + 4);
            if (size < 8) break;
            if (kind == 0x19 && size >= 72) {   // LC_SEGMENT_64
                char name[17]{};
                for (unsigned i = 0; i < 16; ++i) name[i] = static_cast<char>(read8(command + 8 + i));
                const auto vmaddr = read64(command + 24), fileoff = read64(command + 40);
                if (std::strcmp(name, "__TEXT") == 0 && fileoff == 0) {
                    slide = header - vmaddr;
                    have_text = true;
                } else if (std::strcmp(name, "__LINKEDIT") == 0) {
                    linkedit_address = vmaddr;
                    linkedit_offset = fileoff;
                    have_linkedit = true;
                }
                const auto sections = read32(command + 64);
                for (std::uint32_t s = 0; s < sections && 72 + (s + 1) * 80ULL <= size; ++s) {
                    const auto section = command + 72 + s * 80ULL;
                    const auto type = read32(section + 64) & 0xff;
                    if (type == 0x9) pointer_arrays.emplace_back(read64(section + 32), read64(section + 40));
                    if (type == 0x16) offset_arrays.emplace_back(read64(section + 32), read64(section + 40));
                }
            } else if (kind == 0x26 && size >= 16) {   // LC_FUNCTION_STARTS
                starts_offset = read32(command + 8);
                starts_size = read32(command + 12);
            } else if (kind == 0x80000028 && size >= 24) {   // LC_MAIN
                request.roots.push_back(header + read64(command + 8));
            }
            command += size;
        }
        if (have_text) {
            // Initializers run first (in this order), right after the image is published.
            std::vector<std::uint64_t> initializers;
            for (const auto& [address, bytes] : pointer_arrays)
                for (std::uint64_t at = 0; at + 8 <= bytes && at < 65536 * 8; at += 8)
                    initializers.push_back(read64(address + slide + at));
            for (const auto& [address, bytes] : offset_arrays)
                for (std::uint64_t at = 0; at + 4 <= bytes && at < 65536 * 4; at += 4)
                    initializers.push_back(header + read32(address + slide + at));
            request.roots.insert(request.roots.begin(), initializers.begin(), initializers.end());
        }
        if (have_text && have_linkedit && starts_size && starts_offset >= linkedit_offset) {
            std::vector<std::uint8_t> data(static_cast<std::size_t>(starts_size));
            read_bytes(linkedit_address + slide + (starts_offset - linkedit_offset), data);
            std::uint64_t address = header;   // deltas start at the __TEXT segment
            std::size_t at = 0;
            while (at < data.size()) {
                std::uint64_t delta = 0;
                unsigned shift = 0;
                std::uint8_t byte = 0;
                do {
                    byte = data[at++];
                    if (shift < 64) delta |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
                    shift += 7;
                } while ((byte & 0x80) && at < data.size());
                if (delta == 0) break;
                address += delta;
                request.function_starts.push_back(address);
            }
        }
    } catch (const std::exception&) {
        // Unreadable metadata: translate from the JIT's flow only.
    }
    fx::aot_external_module(request);
}

std::optional<std::uint64_t> FxEngine::heap_allocate(std::uint64_t size, std::uint64_t alignment) {
    // The Windows x64 heap returns 16-byte aligned blocks; larger alignments
    // and large requests keep their own mapped regions.
    if (alignment > 16 || size >= 256 * 1024) return std::nullopt;
    const vm::Heap heap = guest_heap();
    if (!heap) return std::nullopt;
    void* block = vm::heap_allocate(heap, static_cast<std::size_t>(size));
    if (!block) throw std::bad_alloc();
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        std::shared_lock lock(shared_->heap_lock);
        if (shared_->heap_segment(address) && shared_->heap_segment(address + std::max<std::uint64_t>(size, 1) - 1))
            return address;
    }
    // First block in a new heap reservation: record the reservation's extent.
    vm::RegionInfo info;
    if (!vm::query(block, info)) throw std::runtime_error("alchemy-fx: cannot query a guest heap block");
    const auto base = info.allocation_base;
    auto end = base;
    while (vm::query(host(end), info) && info.allocation_base == base) end = info.base + info.size;
    if (end <= address) throw std::runtime_error("alchemy-fx: guest heap block outside its reservation");
    std::unique_lock lock(shared_->heap_lock);
    shared_->heap_segments[base] = end - base;
    return address;
}

void FxEngine::heap_release(std::uint64_t address) {
    if (!vm::heap_free(guest_heap(), host(address))) throw std::runtime_error("alchemy-fx: invalid guest heap block");
}

void FxEngine::flush_translations() {
    fx::runtime_prepare_exit();
    fx::cache_flush();
}

void FxEngine::invalidate_code(std::uint64_t address, std::uint64_t size) { fx::flush_guest_code(address, size); }

void FxEngine::protect_range(std::uint64_t address, std::uint64_t size, bool readable, bool writable,
                             bool executable) {
    std::unique_lock lock(shared_->memory_lock);
    const auto begin = align_down(address, kPage);
    const auto end = align_up(address + size, kPage);
    bool reservations = true;   // only Darwin reservations (reserve_at / reserve_anywhere) commit on demand
    for (auto cursor = begin; cursor < end;) {
        const auto* region = shared_->find(cursor);
        if (!region || !region->mapped) {
            std::ostringstream message;
            message << "partial protection outside mapped guest memory at 0x" << std::hex << cursor << " (range 0x"
                    << address << "+0x" << size << ')';
            throw std::runtime_error(message.str());
        }
        reservations = reservations && region->lazy && !region->shared;
        cursor = std::max(cursor + kPage, align_up(region->base + region->size, kPage));
    }
    if (readable || writable) {
        // Darwin commits on first touch: a large range made accessible (V8's
        // code range) or any part of one that is pending already stays
        // uncommitted until touched.
        const bool lazy = shared_->lazy_commit && reservations &&
                          (end - begin >= kLazyCommitMinimum || shared_->pending_overlaps(begin, end));
        shared_->provide(begin, end, protection(readable, writable), lazy);
    } else {
        shared_->forget_pending(begin, end);
        if (!protect(begin, end, vm::kNoAccess)) throw std::runtime_error("alchemy-fx: VirtualProtect failed");
    }
    // fx saved the guest's writable protection of pages it write-protected
    // (SMC): forget it, so neither an SMC fault nor the invalidation below
    // puts it back over the guest's new non-writable protection.
    if (!writable) fx::guest_protection_lowered(begin, end - begin);
    if (executable) fx::register_guest_code_memory(begin, end - begin);
    else unregister_code(begin, end);   // no longer guest code; drops its translations
    if (writable && executable) fx::invalidate_guest_code(begin, end - begin);
}

bool FxEngine::reserve_at(std::uint64_t base, std::uint64_t size, bool readable, bool writable) {
    std::unique_lock lock(shared_->memory_lock);
    return shared_->reserve_at(base, size, readable, writable);
}

void FxEngine::unmap_range(std::uint64_t address, std::uint64_t size) {
    if (!size || address % kPage || address + size < address)
        throw std::invalid_argument("alchemy-fx: a partial unmap must be page aligned");
    std::unique_lock lock(shared_->memory_lock);
    shared_->unmap_range(address, align_up(address + size, kPage));
}

void FxEngine::replace_range(std::uint64_t address, std::uint64_t size, bool readable, bool writable,
                             bool executable) {
    if (!size || address % kPage || address + size < address)
        throw std::invalid_argument("alchemy-fx: a replaced range must be page aligned");
    std::unique_lock lock(shared_->memory_lock);
    const auto end = align_up(address + size, kPage);
    shared_->unmap_range(address, end);
    if (!shared_->reserve_at(address, end - address, readable, writable)) {
        std::ostringstream message;
        message << "alchemy-fx: cannot map the replaced range 0x" << std::hex << address << "+0x" << size;
        throw std::runtime_error(message.str());
    }
    if (executable) fx::register_guest_code_memory(address, end - address);
}

void FxEngine::discard_range(std::uint64_t address, std::uint64_t size) {
    std::unique_lock lock(shared_->memory_lock);
    const auto begin = align_down(address, kPage), end = align_up(address + size, kPage);
    for_each_committed_piece(begin, end, [this](std::uint64_t piece, std::uint64_t length) {
        vm::RegionInfo info;
        if (!vm::query(host(piece), info) || info.type != vm::Type::private_memory ||
            !shared_->allocations.count(info.allocation_base))
            return;
        // Pages of translated code would come back as zeros under their translations.
        fx::invalidate_guest_code(piece, length);
        // Pending ranges give the commit back: their pages read as zeros on
        // the next touch, which commits them again. Elsewhere the host may drop
        // the contents but keeps the commit.
        const auto piece_end = piece + length;
        for (auto at = piece; at < piece_end;) {
            if (const auto* entry = shared_->pending_at(at)) {
                const auto run_end = std::min(piece_end, entry->second.end);
                std::unique_lock commit_guard(shared_->commit_lock);
                vm::decommit(host(at), static_cast<std::size_t>(run_end - at));
                at = run_end;
            } else {
                const auto run_end = shared_->next_pending(at, piece_end);
                vm::discard(host(at), static_cast<std::size_t>(run_end - at));
                at = run_end;
            }
        }
    });
}

namespace {
// The guest's view of one host page run: what Darwin's map entry would say.
struct GuestView {
    bool readable{}, writable{}, executable{};
    bool operator==(const GuestView&) const = default;
};
// Host state of the vm::query run at `page`, as the guest sees it: fx's
// SMC write protection of translated pages (host read-only) is the guest's
// own protection; code registration supplies execute. `uniform` is false
// when pages of the run may differ (read-only runs may hold SMC-protected
// pages; code registration may start inside), so the caller steps per page.
// `pending`: the protection of an uncommitted page committed on first touch
// (FxEngine::Shared::pending), else 0.
GuestView guest_view(const vm::RegionInfo& info, std::uint64_t page, bool region_executable,
                     vm::Protection pending = 0) {
    auto protect = info.committed() ? (info.protect & 0xff) : pending ? pending : vm::kNoAccess;
    if (protect == vm::kReadOnly && info.committed())
        if (const auto saved = fx::guest_page_protection(page)) protect = saved & 0xff;
    GuestView view;
    view.writable = protect == vm::kReadWrite || protect == vm::kWriteCopy || protect == vm::kExecuteReadWrite;
    view.readable = view.writable || protect == vm::kReadOnly || protect == vm::kExecuteRead;
    view.executable = region_executable || ((info.committed() || pending) && fx::is_guest_code(page));
    return view;
}
struct ViewRun { GuestView view; std::uint64_t begin{}, end{}; };
// The run of pages around `page` (inside [low, high)) with one guest view:
// whole vm::query runs where they are uniform, single pages otherwise.
// Uncommitted runs split where pending ranges begin and end.
std::optional<ViewRun> view_run(const FxEngine::Shared& shared, std::uint64_t page, std::uint64_t low,
                                std::uint64_t high, bool region_executable) {
    vm::RegionInfo info;
    if (!vm::query(host(page), info)) return std::nullopt;
    auto run_begin = std::max(low, info.base);
    auto run_end = std::min(high, info.base + info.size);
    vm::Protection pending = 0;
    if (!info.committed()) {
        if (const auto* entry = shared.pending_at(page)) {
            pending = entry->second.protection;
            run_begin = std::max(run_begin, entry->first);
            run_end = std::min(run_end, entry->second.end);
        } else {
            // Between the pending ranges around `page` (they are disjoint and sorted).
            run_end = shared.next_pending(page, run_end);
            if (auto it = shared.pending.upper_bound(page); it != shared.pending.begin())
                run_begin = std::max(run_begin, std::prev(it)->second.end);
        }
    }
    const auto view = guest_view(info, page, region_executable, pending);
    const bool coded = info.committed() || pending;
    const bool uniform = (!info.committed() || (info.protect & 0xff) != vm::kReadOnly) &&
                         (region_executable || !coded ||
                          fx::is_guest_code(run_begin) == fx::is_guest_code(run_end - 1));
    if (uniform || run_end - run_begin > (64ULL << 20)) return ViewRun{view, run_begin, run_end};
    return ViewRun{view, page, std::min(high, page + kPage)};
}
} // namespace

std::optional<GuestMemory::RegionInfo> FxEngine::region_from(std::uint64_t address) const {
    std::shared_lock lock(shared_->memory_lock);
    // The guest region containing `address`, else the lowest one above it.
    const Region* found = nullptr;
    auto it = shared_->regions.upper_bound(address);
    if (it != shared_->regions.begin()) {
        const auto& before = std::prev(it)->second;
        if (address - before.base < std::max<std::uint64_t>(before.size, 1)) found = &before;
    }
    if (!found && it != shared_->regions.end()) found = &it->second;
    std::optional<RegionInfo> result;
    if (found) {
        result = RegionInfo{found->base, found->size, found->readable, found->writable, found->executable, false,
                            found->mapped};
        if (found->mapped && !found->stack && found->size >= kPage) {
            // Darwin splits map entries where protection changes (mprotect of
            // part of a mapping, of image data, fx-protected code pages): the
            // run of pages with the guest view of `address`.
            const auto low = align_down(found->base, kPage), high = align_up(found->base + found->size, kPage);
            const auto at = align_down(std::max(address, found->base), kPage);
            if (const auto run = view_run(*shared_, at, low, high, found->executable)) {
                auto begin = run->begin, end = run->end;
                while (begin > low) {
                    const auto previous = view_run(*shared_, begin - kPage, low, high, found->executable);
                    if (!previous || !(previous->view == run->view)) break;
                    begin = previous->begin;
                }
                while (end < high) {
                    const auto next = view_run(*shared_, end, low, high, found->executable);
                    if (!next || !(next->view == run->view)) break;
                    end = next->end;
                }
                begin = std::max(begin, found->base);
                end = std::min(end, found->base + found->size);
                result = RegionInfo{begin, end - begin, run->view.readable, run->view.writable, run->view.executable,
                                    false, true};
            }
        }
    }
    // Guest malloc heap reservations are guest memory too.
    std::shared_lock heap(shared_->heap_lock);
    auto segment = shared_->heap_segments.upper_bound(address);
    if (segment != shared_->heap_segments.begin() && address - std::prev(segment)->first < std::prev(segment)->second)
        --segment;
    if (segment != shared_->heap_segments.end() && (!result || segment->first < result->base) &&
        segment->first + segment->second > address)
        result = RegionInfo{segment->first, segment->second, true, true, false, false, false};
    return result;
}

std::uint64_t FxEngine::resident_bytes(std::uint64_t address, std::uint64_t size) const {
    std::shared_lock lock(shared_->memory_lock);
    std::uint64_t bytes = 0;
    for_each_committed_piece(align_down(address, kPage), align_up(address + size, kPage),
                             [&bytes](std::uint64_t, std::uint64_t length) { bytes += length; });
    return bytes;
}

std::uint8_t FxEngine::read8(std::uint64_t address) const {
    std::uint8_t value = 0;
    if (!guarded_move(&value, host(address), 1)) memory_fault("read", address);
    return value;
}

void FxEngine::write8(std::uint64_t address, std::uint8_t value) {
    if (!guarded_move(host(address), &value, 1)) memory_fault("write", address);
}

std::uint32_t FxEngine::read32(std::uint64_t address) const {
    std::uint32_t value = 0;
    if (!guarded_move(&value, host(address), 4)) memory_fault("read", address);
    return value;
}

std::uint64_t FxEngine::read64(std::uint64_t address) const {
    std::uint64_t value = 0;
    if (!guarded_move(&value, host(address), 8)) memory_fault("read", address);
    return value;
}

void FxEngine::write32(std::uint64_t address, std::uint32_t value) {
    if (!guarded_move(host(address), &value, 4)) memory_fault("write", address);
}

void FxEngine::write64(std::uint64_t address, std::uint64_t value) {
    if (!guarded_move(host(address), &value, 8)) memory_fault("write", address);
}

void FxEngine::read_bytes(std::uint64_t address, std::span<std::uint8_t> bytes) const {
    if (!bytes.empty() && !guarded_move(bytes.data(), host(address), bytes.size())) memory_fault("read", address);
}

void FxEngine::write_bytes(std::uint64_t address, std::span<const std::uint8_t> bytes) {
    if (!bytes.empty() && !guarded_move(host(address), bytes.data(), bytes.size())) memory_fault("write", address);
}

void FxEngine::fill_bytes(std::uint64_t address, std::uint8_t value, std::uint64_t count) {
    if (count && !guarded_fill(host(address), value, static_cast<std::size_t>(count))) memory_fault("write", address);
}

void FxEngine::copy_bytes(std::uint64_t destination, std::uint64_t source, std::uint64_t count, bool) {
    if (count && !guarded_move(host(destination), host(source), static_cast<std::size_t>(count)))
        memory_fault("copy", source);
}

std::size_t FxEngine::string_length(std::uint64_t address, unsigned character_bytes,
                                    std::size_t maximum_characters, bool* terminated) const {
    if (character_bytes != 1 && character_bytes != 2) throw std::runtime_error("invalid string character width");
    const auto length = guarded_length(static_cast<const std::uint8_t*>(host(address)), character_bytes,
                                       maximum_characters);
    if (length < 0) memory_fault("read", address);
    if (terminated) *terminated = static_cast<std::size_t>(length) < maximum_characters;
    return static_cast<std::size_t>(length);
}

std::vector<std::uint8_t> FxEngine::read_string(std::uint64_t address, unsigned character_bytes,
                                                std::size_t maximum_characters, bool* terminated) const {
    const auto length = string_length(address, character_bytes, maximum_characters, terminated);
    std::vector<std::uint8_t> bytes(length * character_bytes);
    read_bytes(address, bytes);
    return bytes;
}

} // namespace transmute
