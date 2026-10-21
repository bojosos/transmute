#pragma once

// Private declarations shared by the runtime's translation units (owner: runtime).

#include "fx/runtime/runtime.hpp"
#include "fx/translator/translator.hpp"
#include "transmute/fx/process.hpp"
#include "fx/host/fault.hpp"
#include "fx/host/guard.hpp"
#include "fx/host/sync.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/unwind.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace transmute::fx::x64 { class Assembler; }

namespace transmute::fx::rt {

// ---- initialization -------------------------------------------------------------
const RuntimeOptions& options() noexcept;
void ensure_initialized();

// ---- code arena -------------------------------------------------------------------
// One RWX reservation (all code within rel32 reach of each other), cut into
// kArenaChunk chunks (arena.cpp): permanent chunks for stubs and thunks
// (arena_allocate, never freed), code chunks for translated regions
// (code_allocate, freed by reclaim in regions.cpp), free chunks (decommitted).
inline constexpr std::size_t kArenaReserve = std::size_t{1} << 30;
inline constexpr std::size_t kArenaChunk = std::size_t{2} << 20;
inline constexpr std::uint32_t kArenaChunks = static_cast<std::uint32_t>(kArenaReserve / kArenaChunk);
inline constexpr std::size_t kArenaCommitChunk = std::size_t{256} << 10;   // commit step, bottom-up
inline constexpr std::size_t kStubAreaSize = 64 * 1024;              // arena offset 0: stubs + unwind data
inline constexpr std::size_t kUnwindChunk = 64 * 1024;

void arena_initialize();
std::uint8_t* arena_base() noexcept;
inline bool in_arena(std::uint64_t address) noexcept {
    const auto base = reinterpret_cast<std::uint64_t>(arena_base());
    return address - base < kArenaReserve;
}
// Bump allocation from permanent chunks (never freed). Thread-safe, never
// reclaims (callers may hold the region map lock). Throws std::bad_alloc.
void* arena_allocate(std::size_t size, std::size_t alignment);
// Bytes allocated in chunks in use (permanent + code + counter pages).
std::size_t arena_used() noexcept;

// Translated regions go into one of two streams of code chunks, so code that
// dies together shares chunks: `volatile_code` is JIT code of registered guest
// code memory (a guest JIT rewrites and discards it), `stable` everything else
// (image code, cache and AOT records).
enum class CodeStream : std::uint8_t { stable = 0, volatile_code = 1 };
inline constexpr unsigned kCodeStreams = 2;
enum class ChunkState : std::uint8_t { free, permanent, code, tail };

struct Region;
// A chunk (or the head of a run of chunks for an allocation larger than one).
struct ArenaChunk {
    // ---- arena lock ----
    ChunkState state = ChunkState::free;
    CodeStream stream = CodeStream::stable;
    bool current = false;         // its stream's chunk for new code
    bool reused = false;          // held code before (install flushes the instruction cache)
    std::uint32_t span = 1;       // chunks in the run (head)
    std::uint32_t head = 0;       // index of the run's head (tail chunks)
    std::uint64_t sequence = 0;   // order in which chunks were taken (eviction age)
    std::size_t top = 0;          // bump offset of code (permanent: of anything)
    std::size_t floor = 0;        // offset of the lowest counter page (= span * kArenaChunk without one)
    std::size_t committed = 0;    // bytes committed from the bottom
    std::uint32_t counter_pages = 0;
    std::uint32_t* counter_next = nullptr;
    std::uint32_t* counter_end = nullptr;
    std::vector<void*> meta_blocks;   // region metadata (read-write host blocks)
    std::size_t meta_bytes = 0;
    std::uint8_t* meta_next = nullptr;
    std::uint8_t* meta_end = nullptr;
    // Allocations not yet installed or abandoned (code_allocate .. code_release):
    // reclaim leaves the chunk alone while any is in flight.
    std::atomic<std::uint32_t> pending{0};
    // ---- region map lock (regions.cpp) ----
    std::vector<Region*> regions;   // installed regions, sorted by code address
    std::uint32_t live = 0;         // regions not dead
    std::uint64_t live_bytes = 0;
};
ArenaChunk& arena_chunk(std::uint32_t index) noexcept;
// The head chunk holding arena address `address`, or ~0u outside the arena.
std::uint32_t arena_chunk_of(std::uint64_t address) noexcept;

// A translated region's memory: code (in its chunk), metadata (owned by the
// chunk, freed with it) and, if asked, a tier-0 entry counter (a counter page
// of the same chunk). Throws std::bad_alloc when no chunk can be found even
// after reclaim. Every block is released with code_release once installed
// (map lock held) or abandoned.
struct CodeBlock {
    std::uint8_t* code = nullptr;
    void* meta = nullptr;
    std::uint32_t* counter = nullptr;
    std::uint32_t chunk = 0;
    bool reused = false;
};
CodeBlock code_allocate(std::size_t code_bytes, std::size_t meta_bytes, bool counter, CodeStream stream);
void code_release(const CodeBlock& block) noexcept;

struct ArenaUsage {
    std::size_t used_chunks = 0, free_chunks = 0;
    std::size_t permanent_bytes = 0, code_bytes = 0;
    std::size_t committed_bytes = 0;   // arena pages committed
    std::size_t metadata_bytes = 0;    // region metadata blocks (outside the arena)
};
ArenaUsage arena_usage() noexcept;
// Reclaim's view of the code chunks (heads): busy = current or with allocations in flight.
struct ChunkSummary {
    std::uint32_t index, span;
    std::uint64_t sequence;
    bool busy;
    CodeStream stream;
    std::size_t used;   // code and counter pages allocated
};
void arena_code_chunks(std::vector<ChunkSummary>& out);
// Chunks that may still be taken: free ones, within the budget (all chunks,
// or TRANSMUTE_FX_ARENA_MB). Reclaim keeps an eighth to a quarter of the
// budget free.
std::size_t arena_free_chunks() noexcept;
std::size_t arena_budget() noexcept;
void arena_set_budget_for_tests(std::size_t chunks) noexcept;
// Decommits code chunk `index` (a head, not current), frees its metadata blocks
// and makes its run free. The caller (reclaim) made sure nothing can reach it.
void arena_free_chunk(std::uint32_t index);
// Reclaim (regions.cpp): a code chunk was just taken, `free_chunks` remain
// (cheap; wakes the reclaimer when a pass may be worth it), and the blocking
// form when none is left (false: no pass can run, e.g. at process exit).
void reclaim_notify(std::size_t free_chunks) noexcept;
bool reclaim_wait();
// One reclaim pass on the reclaimer thread, whatever the dead chunks add up
// to (with `evict`: also evicting when free chunks are low), waited for.
// False if none can run (stopping, or the reclaimer did not start).
struct ReclaimResult {
    std::size_t victims = 0;   // chunk runs examined
    std::size_t freed = 0;     // chunks freed
    std::size_t pinned = 0;    // victim runs still referenced (kept)
    std::size_t evicted = 0;   // live regions killed
    bool scanned = false;      // every guest thread was suspended and read
};
bool reclaim_run(bool evict, ReclaimResult* result);
// Process exit: no pass starts any more; waits up to `wait_ms` for one in
// progress (0: only checks). True if none is running.
bool reclaim_stop(unsigned wait_ms);
// Tests: undoes reclaim_stop (process-exit tests ran before).
void reclaim_resume_for_tests();
// Registers the unwind lookup that describes arena frames (host::register_unwind_lookup).
// `precise` are RUNTIME_FUNCTIONs (arena-relative) with their own unwind data;
// every other arena address unwinds with `generic_unwind_rva` (the frame of
// the entry stub, i.e. translated code running at rsp == ctx->host_rsp).
void arena_install_unwind(const std::vector<host::UnwindEntry>& precise, std::uint32_t generic_unwind_rva);

// ---- stubs ----------------------------------------------------------------------------
// fx_enter(ctx, code, guest mxcsr) -> guest mxcsr at exit
using EnterFn = std::uint32_t (*)(GuestContext* ctx, const void* code, std::uint32_t mxcsr);
using AbortFn = void (*)(GuestContext* ctx);

struct Stubs {
    EnterFn enter{};
    const void* dispatch_exit{};
    const void* indirect{};
    const void* link{};
    const void* miss{};              // arena offset 0
    const void* return_to_host{};
    const void* rsb_redispatch{};    // ctx->pc = x30; jmp indirect (scrubbed RSB entries)
    const void* interpret_pc{};      // exit(interpret) for ctx->pc (never installed)
    const void* host_call_bridge{};  // Win64 function: r10 = guest target (execute faults)
    AbortFn abort_to_dispatcher{};   // rcx = ctx; jumps to dispatch_exit
    // Cross-region register convention (stubs.cpp): GuestContext::
    // stub_indirect_live / stub_dispatch_exit / stub_host_return. Equal to
    // indirect / dispatch_exit / nullptr without convention registers.
    const void* indirect_live{};
    const void* dispatch_exit_live{};
    const void* host_return{};
};
// The convention registers to/from GuestContext (r15 = biased context).
void store_globals(x64::Assembler& a);
void load_globals(x64::Assembler& a);
extern Stubs g_stubs;
void generate_stubs();
// Arena-relative UNWIND_INFO for an abi code frame body (0 if unsupported).
std::uint32_t abi_frame_unwind_rva(std::uint32_t frame_bytes, bool entered_by_call) noexcept;
// x64 code in the arena that jumps to `target` (for host entries outside the arena).
const void* make_trampoline(const void* target);

// ---- lookup table ------------------------------------------------------------------------
inline constexpr std::uint64_t kLookupMask = (std::uint64_t{1} << kLookupIndexBits) - 1;
void lookup_initialize();
std::uint64_t* lookup_table() noexcept;
inline std::size_t lookup_index(std::uint64_t pc) noexcept {
    return static_cast<std::size_t>(((pc >> 2) ^ (pc >> kLookupTagShift)) & kLookupMask);
}
void lookup_install(std::uint64_t pc, const void* host) noexcept;
void lookup_remove(std::uint64_t pc, const void* host) noexcept;
const void* lookup_probe(std::uint64_t pc) noexcept;

// ---- regions / translation / linking -----------------------------------------------------
struct ExitInfo {
    std::uint32_t rel32_offset;
    std::int32_t original;       // rel32 to the unlinked tail
    std::uint64_t target_pc;
    bool linked;
    // The exit's node in the link or pending list of target_pc (regions.cpp
    // ExitRefs; an exit is in at most one), so removing it is O(1).
    std::uint32_t ref_node = UINT32_MAX;
};

// A fixed array inside a region's metadata block (a region and all its
// arrays are one allocation from its code chunk's metadata, see install;
// freed with the chunk).
template <typename T>
struct RegionArray {
    T* data_{};
    std::uint32_t size_{};
    T* begin() const noexcept { return data_; }
    T* end() const noexcept { return data_ + size_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    T& operator[](std::size_t i) const noexcept { return data_[i]; }
};

struct Region {
    std::uint64_t guest_start{};
    std::uint8_t* code{};
    std::uint32_t size{};
    std::uint32_t hot_size{};   // TranslationStats::hot_bytes: [0, hot_size) is the body, the rest cold
    RegionArray<RegionEntry> entries;
    RegionArray<ExitInfo> exits;                 // sorted by rel32_offset
    RegionArray<FaultSite> faults;               // sorted by host_begin
    RegionArray<PinnedRegister> pins;
    RegionArray<FaultMap> fault_maps;            // FaultSite::map (write-back register cache)
    RegionArray<PinnedRegister> fault_map_regs;
    RegionArray<std::pair<std::uint64_t, std::uint64_t>> guest_ranges;
    bool dead{};
    // TranslatedRegion::tier. A tier-0 region's entries are replaced by a
    // tier-1 translation once its entry counter runs out; it stays valid
    // (threads inside it, stale links) but is no longer reachable from the
    // map, the lookup table, links or RSBs.
    std::uint8_t tier{1};
    std::uint32_t* counter{};    // tier 0: the entry counter (outside code)
    std::uint32_t chunk{};       // arena_chunk of `code` (the run's head)
    // A higher tier replaced all its entries: unreachable but not dead (still
    // on its pages, for SMC); its chunk counts it as dead (reclaim evicts it).
    bool retired{};
    // JIT translations of registered code: a hash of the guest bytes (guest
    // ranges) at install, never 0; 0 = not hashed (cached and image regions).
    // A JIT write window's pages are checked against it (regions.cpp).
    std::uint64_t guest_hash{};
};

// The options every JIT translation uses (also the translation-cache key).
TranslateOptions jit_translate_options() noexcept;
// Host entry for `pc` from the lookup table or the authoritative map, or nullptr.
const void* find_entry(std::uint64_t pc);
// Translates (or loads from the cache) and installs the region for `pc`.
// Returns nullptr if `pc` is not readable guest code, or g_stubs.interpret_pc
// if the code keeps changing during translation (ctx->pc must equal pc).
const void* translate_and_install(std::uint64_t pc);
// ExitReason::tier_up at `pc`: if pc still enters a tier-0 region, translates
// (or loads from the cache) a tier-1 region for pc and installs it in place of
// the tier-0 entries it covers (map, lookup table, links, RSBs). The tier-0
// code stays valid for threads still running it.
void tier_up(std::uint64_t pc);
// True if `pc` has a registered host entry (thunk or override).
bool has_host_entry(std::uint64_t pc);
// Patches the exit whose rel32 field is at `field` to `host`, if it still is
// an unlinked exit of a live region that targets `target_pc`.
void link_exit(std::uint64_t field, std::uint64_t target_pc, const void* host);
// Diagnostics: live regions indexed on the guest page containing `address`.
std::size_t live_regions_on_page(std::uint64_t address);
// Registers `host` (in the arena) for `pc`, replacing any translation.
void install_host_entry(std::uint64_t pc, const void* host);
// Removes the host entry of `pc` (exits linked to it wait for its next entry).
// False if `pc` has no host entry.
bool remove_host_entry(std::uint64_t pc);
// Background installation of cached regions (RuntimeOptions::preload_cached_regions,
// TRANSMUTE_FX_PRELOAD=<threads>, 0 = off). preload_stop stops the preloaders
// for good and waits up to `wait_ms` until none is between its queue and the
// end of a region install (map, arena, cache locks); true if none is. With
// wait_ms == 0 (other threads already terminated) it only checks.
void preload_initialize();
// Preloader threads for `runtime` and TRANSMUTE_FX_PRELOAD's value (`setting`,
// nullptr when unset): none without the translation cache; else the setting
// (at most 8, 0 = off) when there is one; else 1-3 (hardware threads / 4)
// when RuntimeOptions::preload_cached_regions, 0 when not.
unsigned preload_threads(const RuntimeOptions& runtime, const char* setting, unsigned hardware);
bool preload_stop(unsigned wait_ms);
// Process exit, other threads terminated: true if neither the region map lock
// nor the translation lock was left held (try-acquire; never blocks).
bool region_locks_free() noexcept;
// Tests: pending (unlinked, live) exit references; *linked = linked ones.
std::size_t exit_refs_for_tests(std::size_t* linked);
// Tests: called with the region pc under the exclusive map lock during install.
void set_install_hook_for_tests(void (*hook)(std::uint64_t pc));
// A guest thread installed the cached region of `pc` itself: the preloader
// continues from there.
void preload_hint(std::uint64_t pc) noexcept;

// Fault-site lookup for a host pc in translated code. Copies what the
// exception handler needs. False if `rip` is not covered by a fault site.
struct FaultInfo {
    FaultSite site;
    std::vector<PinnedRegister> pins;
    std::vector<PinnedRegister> map;   // the site's fault map (overrides pins); site.map = map.empty() ? 0 : 1
};
bool find_fault_site(std::uint64_t rip, FaultInfo& info);
// Sampling profiler: the region containing host `rip` and the guest pc of the
// nearest fault site at or before it (the region start if none).
struct RegionSample {
    std::uint64_t guest_start;
    std::uint64_t guest_pc;
    std::uint64_t code;
    std::uint32_t size;
    std::uint32_t offset;
    std::uint32_t hot_size;     // Region::hot_size
};
bool sample_region(std::uint64_t rip, RegionSample& sample);
bool region_fault_sites(std::uint64_t code, std::vector<FaultSite>& faults);
// Guest address ranges [begin, end) the region at host `code` translates.
bool region_guest_ranges(std::uint64_t code, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges);
// TRANSMUTE_FX_SAMPLE=<file|1>[,<interval_us>]: starts the sampling profiler.
void profiler_start_if_requested();
// Final report (process exit); stops sampling.
void profiler_flush();
// A runtime background thread (preloader) announces itself: sampled with the `bg` flag.
void profiler_note_background_thread() noexcept;
// TRANSMUTE_FX_CORESTATS=<file|1>[,<interval_us>]: P-core/E-core share of
// guest threads (host/diagnostics.hpp; runtime.cpp passes the guest thread ids).
void corestats_start_if_requested();
void corestats_flush();
// Runtime background threads (preloaders): EcoQoS unless
// TRANSMUTE_FX_BACKGROUND_QOS=default, so they run on efficiency cores and
// leave performance cores to guest threads.
void set_background_thread_qos(host::ThreadHandle thread) noexcept;
// Thread ids and contexts of the registered guest threads (a context may be
// freed as soon as the registry lock is released).
void registered_threads(std::vector<std::pair<std::uint32_t, GuestContext*>>& threads);
// Write fault at `address`:
//   smc       the page was a write-protected translated page: its translations
//             are invalidated and its guest protection restored (retry);
//   writable  not fx's (any more), and writable now: another thread restored
//             it between the fault and this check (retry);
//   other     a protection fault fx does not own.
// Both checks are made under one map lock: fx write-protects a page again (a
// new translation from it) only under the exclusive lock, so a page restored
// by another thread cannot be protected again between them. `changes` is set
// to the count of fx's page protection changes so far (with `writable`).
enum class WriteFault { smc, writable, other };
WriteFault smc_handle_write_fault(std::uint64_t address, std::uint64_t& changes);

// JIT code source over live guest memory (tests use it directly).
class LiveCodeSource final : public CodeSource {
public:
    bool read(std::uint64_t address, std::uint32_t& word) const override;
    // Guest code, or committed guest memory that is read-only to the guest
    // (not writable, not a page fx write-protected over a writable guest
    // protection). Recorded like code reads (reads(), verify_reads).
    bool read_data(std::uint64_t address, std::uint32_t& word) const override;
    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override;
    // pc already has a translated (or host) entry: the new region stops there
    // and links to it instead of translating covered code again.
    bool has_entry(std::uint64_t pc) const override;
    // Tier-1 translation: only host entries stop region growth. Hot code is
    // translated as one region even where earlier (smaller) regions provide
    // entries: stopping there would keep a loop split across regions.
    bool host_entries_only = false;
    // Words returned by read(), for verification after write protection.
    const std::vector<std::pair<std::uint64_t, std::uint32_t>>& reads() const { return reads_; }
    // The words an earlier translation read (installing it again verifies them).
    void assign_reads(const std::vector<std::pair<std::uint64_t, std::uint32_t>>& reads) { reads_ = reads; }
    // Forgets everything for the next translation (keeps the buffer).
    void reset();

private:
    mutable std::uint64_t page_{~std::uint64_t{0}};
    mutable bool page_ok_{};
    mutable std::uint64_t code_lo_{}, code_hi_{};   // guest code range of the last read
    mutable std::uint64_t data_page_{~std::uint64_t{0}};   // read_data's last page and verdict
    mutable bool data_page_ok_{};
    mutable std::vector<std::pair<std::uint64_t, std::uint32_t>> reads_;
};
// Inside memory registered with register_guest_code_memory (not image
// text). Lock-free.
bool in_registered_code_memory(std::uint64_t address) noexcept;
// register_guest_code_copy: if `address` is in a copy of guest code, sets
// `delta` (copy - source) and returns true. Lock-free.
bool guest_code_copy_delta(std::uint64_t address, std::int64_t& delta) noexcept;
// Ends the copies overlapping [begin, end) (invalidate_guest_code).
void drop_guest_code_copies(std::uint64_t begin, std::uint64_t end);
// The disjoint ranges registered now (stats, tests). Lock-free.
std::size_t registered_code_range_count() noexcept;
// ARM64 .pdata lookup: bounds of the function containing `address`.
bool guest_code_range(std::uint64_t address, std::uint64_t& lo, std::uint64_t& hi) noexcept;
bool pdata_function_bounds(const GuestModule& module, std::uint64_t address,
                           std::uint64_t& begin, std::uint64_t& end);

// Bumped by every invalidation of translated code (and so by every guest
// protection change, which invalidates first).
std::uint64_t invalidation_epoch() noexcept;
// Bumped by explicit invalidations only (fx::invalidate_guest_code, which every
// guest protection change, unmap and decommit reaches, and
// guest_protection_lowered); SMC faults and code flushes leave it.
std::uint64_t memory_access_epoch() noexcept;

// The personality's demand-commit hook (set_demand_commit_hook): true when it
// made the page at `address` accessible for `access` (0 read, 1 write,
// 8 execute). False without a hook.
bool demand_commit(std::uint64_t address, std::uint32_t access) noexcept;

// Reads 4 bytes of guest memory; false (no exception) if unreadable.
inline bool safe_read32(std::uint64_t address, std::uint32_t& value) noexcept { return host::try_read32(address, value); }

// ---- threads ----------------------------------------------------------------------------------
struct ThreadState {
    GuestContext* ctx{};
    std::uint32_t thread_id{};
    GuestStack stack;            // current guest stack (a fiber's, after switch_guest_stack)
    GuestStack own_stack;        // allocated with the thread; freed at teardown
    void* teb{};
    bool attached{};             // loader_thread_attach ran
    std::uintptr_t activation_cookie{};   // guest_activation_context() activation (0 = none)
    // Exception captured by the vectored handler for the next ExitReason::raise.
    bool pending_valid{};
    GuestExceptionRecord pending{};
    // Exit counts by ExitReason (owner thread only; summed by runtime_stats).
    std::uint64_t exits[16]{};
    // Interrupt requests.
    host::RwLock interrupt_lock;
    std::vector<std::function<void(GuestContext&)>> interrupts;
    // A host thread fx did not create (thread pool, COM, ...) that entered guest
    // code. Its activation-context stack belongs to its owner: invoke() activates
    // the guest context only around each outermost entry and pops it on return.
    bool borrowed{};
    // 1 while host code reached from translated code without a dispatcher exit
    // runs between fp_state_to_context and fp_state_from_context (host
    // callbacks, interrupt requests, bracketed host entries; fp_boundary.hpp):
    // ctx.fpsr is authoritative and MXCSR's status flags are stale copies, so
    // exit_to_dispatcher and invoke's unwinding path must not fold them in.
    // invoke() saves it, clears it for the nested run and restores it.
    std::uint32_t fp_host{};
    // Reclaim's handle for suspending and reading the thread (host::sample;
    // opened by the reclaimer, closed with the state).
    void* scan_handle{};
};
inline constexpr std::int32_t kThreadFpHostOffset = static_cast<std::int32_t>(offsetof(ThreadState, fp_host));

ThreadState* current_thread_state() noexcept;
// Classifies a fault (code, data address) against the calling thread's guest
// stack: `grown` = a guard page moved down (retry the access, like the
// kernel's stack growth); `overflow` = raise STATUS_STACK_OVERFLOW (emergency
// space below the old limit is committed for the handlers).
enum class StackFault { none, grown, overflow };
StackFault guest_stack_fault(std::uint32_t code, std::uint64_t address) noexcept;
// Guest record for a host fault at guest `pc` (stack overflow conversion).
GuestExceptionRecord guest_fault_record(const host::FaultRecord& fault, std::uint64_t pc) noexcept;
// Reclaim (regions.cpp): suspends every registered guest thread, reports
// each one's register words, host-stack words and context, then resumes them
// all. Nothing is allocated or locked while threads are suspended, and the
// callbacks must do the same. False if a thread could not be suspended or
// read (or the caller is a guest thread itself): then any code may still be
// referenced.
struct ThreadScan {
    void (*thread)(void* user, std::uint32_t thread_id, std::uint64_t sp);   // before its words
    void (*word)(void* user, std::uint64_t value);    // its registers, then its stack from sp up
    void (*context)(void* user, GuestContext& ctx);
    void* user;
};
bool scan_guest_threads(const ThreadScan& scan);
// Zeroes the stack just below the caller's frame. Frames that returned there
// may hold code addresses (results of lookups, translations, installs), and a
// frame later allocated over them (a host call that blocks) would keep them
// in a live frame, where reclaim's conservative scan sees them and keeps
// their chunks while the thread blocks. Called after translations and
// before a reclaim wait.
void clear_stack_residue() noexcept;
// Calls `fn` for every live context while holding the registry lock.
void for_each_context(const std::function<void(GuestContext&)>& fn);
void threads_initialize();
// Registered (live) thread states, for diagnostics and leak tests.
std::size_t live_thread_states();
// Process exit: try-acquire checks for locks a terminated thread may have left held.
bool registry_lock_free() noexcept;
bool arena_lock_free() noexcept;
// Exit counts of live threads plus those of exited threads.
void sum_exit_counts(std::uint64_t (&exits)[16]);
// Copies the current helper table into every live context (registry lock).
void refresh_helpers(unsigned id, const void* function);
extern const void* g_helpers[kHelperSlots];

// ---- dispatcher -----------------------------------------------------------------------------
// Runs ctx until the guest returns to kReturnToHostPc.
void run_guest(GuestContext& ctx);
void process_interrupts(GuestContext& ctx);
// Called by the host-call bridge stub: target = guest function, args = Win64
// argument slots (rcx, rdx, r8, r9 homed, then caller stack args), xmm =
// xmm0..xmm3 (16 bytes each); xmm[0..1] receives v0 on return.
std::uint64_t host_call_bridge_entry(std::uint64_t target, const std::uint64_t* args,
                                     std::uint64_t* xmm);
void interpret_one_helper(GuestContext* ctx);
void check_interrupt_helper(GuestContext* ctx);

// ---- exception handler ----------------------------------------------------------------------
void veh_initialize();

// ---- counters -----------------------------------------------------------------------------------
struct Counters {
    std::atomic<std::uint64_t> exits[16]{};
    std::atomic<std::uint64_t> translations{};
    std::atomic<std::uint64_t> cache_hits{};
    std::atomic<std::uint64_t> translation_retries{};
    std::atomic<std::uint64_t> links{};
    std::atomic<std::uint64_t> unlinks{};
    std::atomic<std::uint64_t> invalidated_regions{};
    std::atomic<std::uint64_t> smc_write_faults{};
    std::atomic<std::uint64_t> host_callbacks{};
    std::atomic<std::uint64_t> guest_exceptions{};
    std::atomic<std::uint64_t> interpreted{};
    std::atomic<std::uint64_t> threads_created{};
    std::atomic<std::uint64_t> arena_dead{};
    std::atomic<std::uint64_t> regions{};
    // host::ticks() spent in translate_and_install (all
    // threads) and its parts: cache lookup, JIT translation, install.
    std::atomic<std::uint64_t> translate_ticks{};
    std::atomic<std::uint64_t> cache_ticks{};
    std::atomic<std::uint64_t> jit_ticks{};
    std::atomic<std::uint64_t> install_ticks{};
    std::atomic<std::uint64_t> preloaded{};       // cached regions installed by the preloader
    std::atomic<std::uint64_t> jit_guest_instructions{};
    // Tiering: tier-0 regions installed, tier-up requests that installed a
    // tier-1 region (JIT or cache), their JIT guest instructions and ticks.
    std::atomic<std::uint64_t> tier0_regions{};
    std::atomic<std::uint64_t> tier_ups{};
    std::atomic<std::uint64_t> tier_up_cached{};
    std::atomic<std::uint64_t> tier_up_instructions{};
    std::atomic<std::uint64_t> tier_up_ticks{};
    std::atomic<std::uint64_t> tier_replaced_entries{};
    // Startup diagnostics: guest-thread waits for the region map lock (only
    // contended acquisitions are timed), preloader busy time (all preloader
    // threads) and the process age when the preload queue last drained.
    std::atomic<std::uint64_t> map_waits{};
    std::atomic<std::uint64_t> map_wait_ticks{};
    std::atomic<std::uint64_t> preload_ticks{};
    std::atomic<std::uint64_t> preload_drained_ticks{};   // QPC value
    // Invalidation and SMC (RuntimeStats has the meanings).
    std::atomic<std::uint64_t> smc_fault_ticks{};
    std::atomic<std::uint64_t> code_flushes{};
    std::atomic<std::uint64_t> code_flush_bytes{};
    std::atomic<std::uint64_t> code_flush_hits{};
    std::atomic<std::uint64_t> invalidation_interrupts{};
    std::atomic<std::uint64_t> invalidation_pages_kept{};
    std::atomic<std::uint64_t> registered_code_translations{};
    std::atomic<std::uint64_t> retranslations{};
    std::atomic<std::uint64_t> jit_reuses{};
    std::atomic<std::uint64_t> code_range_publishes{};
    // Arena reclaim (RuntimeStats has the meanings).
    std::atomic<std::uint64_t> evicted_regions{};
    std::atomic<std::uint64_t> retired_regions{};
    std::atomic<std::uint64_t> reclaim_passes{};
    std::atomic<std::uint64_t> reclaimed_chunks{};
    std::atomic<std::uint64_t> reclaim_pinned{};
    std::atomic<std::uint64_t> reclaim_scan_failures{};
    std::atomic<std::uint64_t> reclaim_ticks{};
    std::atomic<std::uint64_t> reclaim_scan_ticks{};
    std::atomic<std::uint64_t> jit_window_deferred{};
    std::atomic<std::uint64_t> jit_window_pages_checked{};
    std::atomic<std::uint64_t> jit_window_regions_kept{};
    std::atomic<std::uint64_t> jit_window_regions_killed{};
    std::atomic<std::uint64_t> copy_translations{};
    std::atomic<std::uint64_t> copy_cache_hits{};
};
inline std::uint64_t ticks_now() noexcept { return host::ticks(); }
extern Counters g_counters;
inline void count(std::atomic<std::uint64_t>& counter, std::uint64_t n = 1) noexcept {
    counter.fetch_add(n, std::memory_order_relaxed);
}

// ---- small helpers ----------------------------------------------------------------------------
using host::ExclusiveLock;
using host::SharedLock;

[[noreturn]] void fatal(const char* message);

// NTSTATUS values used by the runtime.
inline constexpr std::uint32_t kStatusAccessViolation = 0xC0000005u;
inline constexpr std::uint32_t kStatusIllegalInstruction = 0xC000001Du;
inline constexpr std::uint32_t kStatusBreakpoint = 0x80000003u;
inline constexpr std::uint32_t kStatusAssertionFailure = 0xC0000420u;
inline constexpr std::uint32_t kStatusIntegerDivideByZero = 0xC0000094u;
inline constexpr std::uint32_t kStatusDatatypeMisalignment = 0x80000002u;
inline constexpr std::uint32_t kStatusStackOverflow = 0xC00000FDu;
inline constexpr std::uint32_t kStatusGuardPageViolation = 0x80000001u;

} // namespace transmute::fx::rt
