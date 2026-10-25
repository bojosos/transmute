#pragma once

// fx runtime extensions beyond include/transmute/fx/process.hpp
// (owner: runtime). Other components may include and call these.

#include "transmute/fx/context.hpp"

#include <cstdint>
#include <functional>

namespace transmute::fx {

// ---- lookup table encoding --------------------------------------------------
// Translated code may inline stub_indirect using this encoding:
//   tag   = pc >> kLookupTagShift
//   index = ((pc >> 2) ^ tag) & (2^kLookupIndexBits - 1)  (also ctx->lookup_mask)
//   entry = ctx->lookup_table[index]                (one aligned 8-byte load)
//   hit   = (pc & 3) == 0 && (entry >> 32) == tag   (compare the full 64-bit tag)
//   host  = runtime_arena_base() + (std::uint32_t)entry
// An empty slot is 0, which decodes to the miss stub at arena offset 0, so a
// stray hit on an empty slot still lands in a correct (miss) path. That stub
// stores rcx as ctx->pc: inline lookups keep the target in rcx (and may
// store ctx->pc only on their own miss path).
inline constexpr unsigned kLookupIndexBits = 20;
inline constexpr unsigned kLookupTagShift = kLookupIndexBits + 2;
const void* runtime_arena_base() noexcept;

// ---- table shadows (GuestContext::table_shadow) ---------------------------------
// A shadow is kTableShadowSlots guest pcs, then as many host entries, then as
// many miss targets; the word before the pcs holds its miss stub. For a BR to
// rcx = table[rdx] (rdx < kTableShadowSlots) on this thread's table:
//   if (pc[rdx] == rcx) jmp host[rdx]; else jmp miss[rdx]
// with the cross-region convention registers live and rcx, rdx kept. An empty
// slot's miss target is the miss stub (fills the slot, ExitReason::table_shadow),
// a filled slot's is a stub that takes the inline lookup's path for rcx. A slot's
// pc never changes once set (its host entry may: stores host first, then pc),
// so a reader that sees the pc sees a host entry that was valid for it.
inline constexpr unsigned kTableShadowSlots = 1024;
inline constexpr std::uint32_t kTableShadowRetry = 4096;   // BRs on other tables before a switch

// ---- FP control mapping -------------------------------------------------------
// Delegates to emit_fp.hpp (fp_mxcsr_for_state / fp_read_fpsr), the single
// source of truth: RMode -> RC, FZ -> FTZ|DAZ, all exceptions masked, FPSR
// cumulative bits (IOC, DZC, OFC, UFC, IXC) <-> IE, ZE, OE, UE, PE.
std::uint32_t mxcsr_for_guest(std::uint32_t fpcr, std::uint32_t fpsr) noexcept;
// FPSR plus the cumulative flags accumulated in `mxcsr` (DE is ignored).
// Matches emit_fp.hpp: architectural FPSR = ctx.fpsr | flags(MXCSR).
std::uint32_t fpsr_with_mxcsr_flags(std::uint32_t fpsr, std::uint32_t mxcsr) noexcept;

// ---- hooks ----------------------------------------------------------------------
// SVC handler (macOS personality). ctx.pc is the instruction after the SVC.
// Without a handler an SVC raises STATUS_ILLEGAL_INSTRUCTION at the SVC.
// It runs after a dispatcher exit, on the host MXCSR: ctx.fpsr/ctx.fpcr are
// architectural and are loaded into MXCSR when the guest resumes
// (process.hpp, FP state at host-code boundaries).
using SvcHandler = void (*)(GuestContext& ctx, std::uint32_t immediate);
void set_svc_handler(SvcHandler handler);

// Queues `request` for the thread owning `ctx` and sets interrupt_pending.
// The request runs on that thread at its next safe point (dispatcher exit,
// ExitReason::interrupt, or helper_check_interrupt), with ctx.fpsr/ctx.fpcr
// architectural (process.hpp, FP state at host-code boundaries).
void request_interrupt(GuestContext& ctx, std::function<void(GuestContext&)> request);

// Leaves translated code from inside a helper called by translated code:
// sets exit_reason/exit_arg and unwinds the host stack to the dispatcher
// (helper frames are discarded without running destructors). Guest state
// must be flushed as the exit reason requires (e.g. ctx.pc for raise).
[[noreturn]] void exit_to_dispatcher(GuestContext& ctx, ExitReason reason, std::uint64_t arg);

// ---- guest stacks (threads and fibers) --------------------------------------------
// A guest stack: the reservation [allocation, allocation + size) holds a
// 64 KiB guard region at the bottom and the committed stack [guard_end, base).
// Faults in the guard region raise STATUS_STACK_OVERFLOW (the first one
// commits emergency space for the handlers); PAGE_GUARD pages inside the
// stack grow it like the kernel does for host stacks (_resetstkoflw).
struct GuestStack {
    std::uint64_t allocation{};   // DeallocationStack
    std::uint64_t size{};         // whole reservation
    std::uint64_t guard_end{};    // end of the guard region (initial StackLimit)
    std::uint64_t base{};         // StackBase (16-byte aligned initial SP)
    std::uint64_t limit{};        // current StackLimit
    bool overflowed{};
};
// reserve = usable bytes (0 = the image default). Throws std::bad_alloc.
GuestStack allocate_guest_stack(std::uint64_t reserve);
void free_guest_stack(const GuestStack& stack);
// Makes `stack` the calling thread's current guest stack (GuestContext
// bounds, shadow TEB StackBase/StackLimit/DeallocationStack, overflow
// tracking) and returns the previous one with its current state. Does not
// touch ctx.x[31]. For guest fibers: switch it together with the registers.
GuestStack switch_guest_stack(const GuestStack& stack);
// _resetstkoflw for the calling thread's current guest stack: after a handled
// overflow, decommits the emergency space and re-arms the 64 KiB guard region
// so the next overflow raises STATUS_STACK_OVERFLOW again (with fresh
// emergency space). Fails, like the native routine, while ctx.x[31] is still
// inside or just above the guard region. True if nothing needed re-arming.
bool rearm_guest_stack_guard(GuestContext& ctx);

// ---- statistics -------------------------------------------------------------------
struct RuntimeStats {
    std::uint64_t exits[16]{};          // by ExitReason
    std::uint64_t translations{};
    std::uint64_t cache_hits{};
    std::uint64_t translation_retries{};
    std::uint64_t links{};
    std::uint64_t unlinks{};
    std::uint64_t invalidated_regions{};
    std::uint64_t smc_write_faults{};
    std::uint64_t host_callbacks{};     // host->guest calls caught as execute faults
    std::uint64_t guest_exceptions{};
    std::uint64_t interpreted{};
    std::uint64_t threads_created{};
    std::uint64_t arena_used{};
    std::uint64_t arena_dead{};         // bytes of invalidated (unreachable) code
    std::uint64_t regions{};
    // Time in translate_and_install (all threads) and its parts.
    double translate_seconds{};
    double cache_seconds{};             // cache lookup + record validation
    double jit_seconds{};               // translate_region
    double install_seconds{};
    std::uint64_t preloaded{};          // cached regions installed ahead of use
    std::uint64_t jit_guest_instructions{};   // guest instructions translated by the JIT (jit_seconds' work)
    // (additive) Tiering: tier-0 (provisional, counted) regions installed;
    // tier-ups that installed a tier-1 region (of which from the cache);
    // guest instructions and time of tier-up JIT translations (included in
    // jit_guest_instructions / jit_seconds); entries moved to tier-1 regions.
    std::uint64_t tier0_regions{};
    std::uint64_t tier_ups{};
    std::uint64_t tier_up_cached{};
    std::uint64_t tier_up_instructions{};
    double tier_up_seconds{};
    std::uint64_t tier_replaced_entries{};
    // (additive) Startup diagnostics: contended region-map lock acquisitions
    // on guest threads and their wait time; preloader busy time (summed over
    // preloader threads); QPC time when the preload queue last drained (0 = never).
    std::uint64_t map_waits{};
    double map_wait_seconds{};
    double preload_seconds{};
    std::uint64_t preload_drained_qpc{};
    // (additive) Invalidation and SMC: time in the SMC write-fault handler
    // (lock wait plus invalidation, all threads); flush_guest_code calls,
    // their bytes and the calls that dropped a translation; invalidations
    // that killed live regions (each sets interrupt_pending on every thread);
    // pages left write-protected because translations outside the
    // invalidated bytes survived on them; JIT translations of code in
    // registered (non-image) guest code memory; installs on a page that lost
    // a translation before (re-translations); dead JIT translations of
    // registered code installed again because their guest words were
    // unchanged (no JIT run; included in re-translations, not in
    // translations); the registered code ranges now and how often the range
    // list was republished.
    double smc_fault_seconds{};
    std::uint64_t code_flushes{};
    std::uint64_t code_flush_bytes{};
    std::uint64_t code_flush_hits{};
    std::uint64_t invalidation_interrupts{};
    std::uint64_t invalidation_pages_kept{};
    std::uint64_t registered_code_translations{};
    std::uint64_t retranslations{};
    std::uint64_t jit_reuses{};
    std::uint64_t code_ranges{};
    std::uint64_t code_range_publishes{};
    // (additive) Code arena reclaim: bytes committed in the arena and in
    // region metadata blocks now; arena chunks in use and free now; reclaim
    // passes that ran; bytes of chunks they freed; victim chunks a pass found
    // still referenced by a thread (kept for a later pass); passes whose
    // thread scan failed (nothing freed); live regions evicted to make room
    // (their code was unchanged; they are translated or loaded again when
    // next run); lower-tier regions retired because a higher tier replaced
    // all their entries; time in passes and in their thread scans (threads
    // suspended).
    std::uint64_t arena_committed{};
    std::uint64_t arena_metadata{};
    std::uint64_t arena_chunks_used{};
    std::uint64_t arena_chunks_free{};
    std::uint64_t reclaim_passes{};
    std::uint64_t reclaimed_bytes{};
    std::uint64_t reclaim_pinned{};
    std::uint64_t reclaim_scan_failures{};
    std::uint64_t evicted_regions{};
    std::uint64_t retired_regions{};
    double reclaim_seconds{};
    double reclaim_scan_seconds{};
    // (additive) JIT write windows (guest_jit_write_window): SMC faults inside
    // a window that deferred their page instead of invalidating it; deferred
    // pages checked when a window closed; regions on them kept (guest bytes
    // unchanged) and killed (changed).
    std::uint64_t jit_window_deferred{};
    std::uint64_t jit_window_pages_checked{};
    std::uint64_t jit_window_regions_kept{};
    std::uint64_t jit_window_regions_killed{};
    // (additive) Copies of image code (register_guest_code_copy): JIT
    // translations made in a copy, and regions of a copy installed from the
    // source module's cache instead.
    std::uint64_t copy_translations{};
    std::uint64_t copy_cache_hits{};
};
RuntimeStats runtime_stats();

// Stops background work (the cached-region preloader) so that ExitProcess,
// which terminates other threads wherever they are, cannot leave a runtime
// or cache lock held by one of them. Called by loader_process_exit (every
// guest exit path: exit/_exit/quick_exit/abort, ExitProcess,
// RtlExitUserProcess, TerminateProcess/NtTerminateProcess of the current
// process, return from the guest entry point). Idempotent.
void runtime_prepare_exit();

// For the exiting thread inside host ExitProcess, after every other thread
// was terminated (the loader's process-detach hook): true if no runtime or
// cache lock was left held by a terminated thread, i.e. guest
// DLL_PROCESS_DETACH code may run (translate, link, look up the cache)
// without deadlocking. Never blocks. Also stops the preloaders when
// runtime_prepare_exit did not run (host code called ExitProcess).
bool runtime_exit_safe() noexcept;

// Writes the TRANSMUTE_FX_SAMPLE profile (no-op when it is not enabled). Called at process exit.
void runtime_profile_flush();

} // namespace transmute::fx
