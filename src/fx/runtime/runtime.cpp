// runtime_initialize, options and statistics.

#include "fx/cache/cache.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/diagnostics.hpp"
#include "fx/host/env.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace transmute::fx::rt {

Counters g_counters;

namespace {

RuntimeOptions g_options;
std::once_flag g_once;
std::atomic<bool> g_ready{false};

void initialize(const RuntimeOptions& options) {
    g_options = options;
    CacheConfig cache;
    cache.enabled = options.enable_translation_cache;
    cache.directory = options.cache_directory;
    cache.translate_options = jit_translate_options();
    cache_configure(cache);
    arena_initialize();
    lookup_initialize();
    generate_stubs();
    table_shadow_initialize();
    threads_initialize();
    g_helpers[fx::helper_interpret_one] = reinterpret_cast<const void*>(&interpret_one_helper);
    g_helpers[fx::helper_check_interrupt] = reinterpret_cast<const void*>(&check_interrupt_helper);
    g_helpers[fx::helper_rsb_push] = g_stubs.rsb_push;
    install_host_entry(kReturnToHostPc, g_stubs.return_to_host);
    veh_initialize();
    preload_initialize();
    g_ready.store(true, std::memory_order_release);
    profiler_start_if_requested();
    corestats_start_if_requested();
}

} // namespace

const RuntimeOptions& options() noexcept { return g_options; }

void set_background_thread_qos(host::ThreadHandle thread) noexcept {
    static const bool eco = [] {
        char value[16] = {};
        const auto n = host::get_env("TRANSMUTE_FX_BACKGROUND_QOS", value, sizeof(value));
        return n == 0 || n >= sizeof(value) || _stricmp(value, "eco") == 0;
    }();
    if (!eco) return;
    host::set_thread_eco_qos(thread);
}

// Core-type statistics are a host diagnostic (host/diagnostics.hpp); the
// runtime supplies the guest thread ids.
void corestats_start_if_requested() {
    host::corestats_start_if_requested([](std::vector<std::uint32_t>& ids) {
        std::vector<std::pair<std::uint32_t, GuestContext*>> threads;
        registered_threads(threads);
        for (const auto& [id, ctx] : threads) ids.push_back(id);
    });
}

void corestats_flush() { host::corestats_flush(); }

void ensure_initialized() {
    if (!g_ready.load(std::memory_order_acquire)) runtime_initialize(RuntimeOptions{});
}

[[noreturn]] void fatal(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
    std::abort();
}

} // namespace transmute::fx::rt

namespace transmute::fx {

void runtime_initialize(const RuntimeOptions& options) {
    std::call_once(rt::g_once, [&] { rt::initialize(options); });
}

void runtime_profile_flush() {
    rt::profiler_flush();
    rt::corestats_flush();
}

void runtime_prepare_exit() {
    if (!rt::g_ready.load(std::memory_order_acquire)) return;
    cache_prepare_exit();   // no new background AOT work
    rt::preload_stop(1000);
    rt::reclaim_stop(1000);
}

bool runtime_exit_safe() noexcept {
    if (!rt::g_ready.load(std::memory_order_acquire)) return true;
    // Not prepared (host ExitProcess called directly): stop the preloaders
    // now; any that ExitProcess terminated mid-region make the process unsafe.
    const bool preloaders_idle = rt::preload_stop(0);
    const bool reclaimer_idle = rt::reclaim_stop(0);
    return preloaders_idle && reclaimer_idle && rt::region_locks_free() && rt::arena_lock_free() && rt::registry_lock_free() &&
           cache_exit_quiescent();
}

RuntimeStats runtime_stats() {
    rt::ensure_initialized();
    RuntimeStats stats;
    const auto& c = rt::g_counters;
    rt::sum_exit_counts(stats.exits);
    stats.translations = c.translations.load(std::memory_order_relaxed);
    stats.cache_hits = c.cache_hits.load(std::memory_order_relaxed);
    stats.translation_retries = c.translation_retries.load(std::memory_order_relaxed);
    stats.links = c.links.load(std::memory_order_relaxed);
    stats.unlinks = c.unlinks.load(std::memory_order_relaxed);
    stats.invalidated_regions = c.invalidated_regions.load(std::memory_order_relaxed);
    stats.smc_write_faults = c.smc_write_faults.load(std::memory_order_relaxed);
    stats.host_callbacks = c.host_callbacks.load(std::memory_order_relaxed);
    stats.guest_exceptions = c.guest_exceptions.load(std::memory_order_relaxed);
    stats.interpreted = c.interpreted.load(std::memory_order_relaxed);
    stats.threads_created = c.threads_created.load(std::memory_order_relaxed);
    const rt::ArenaUsage arena = rt::arena_usage();
    stats.arena_used = arena.permanent_bytes + arena.code_bytes;
    stats.arena_committed = arena.committed_bytes;
    stats.arena_metadata = arena.metadata_bytes;
    stats.arena_chunks_used = arena.used_chunks;
    stats.arena_chunks_free = arena.free_chunks;
    stats.arena_dead = c.arena_dead.load(std::memory_order_relaxed);
    stats.regions = c.regions.load(std::memory_order_relaxed);
    const double tick = 1.0 / static_cast<double>(host::tick_frequency());
    stats.translate_seconds = static_cast<double>(c.translate_ticks.load(std::memory_order_relaxed)) * tick;
    stats.cache_seconds = static_cast<double>(c.cache_ticks.load(std::memory_order_relaxed)) * tick;
    stats.jit_seconds = static_cast<double>(c.jit_ticks.load(std::memory_order_relaxed)) * tick;
    stats.install_seconds = static_cast<double>(c.install_ticks.load(std::memory_order_relaxed)) * tick;
    stats.preloaded = c.preloaded.load(std::memory_order_relaxed);
    stats.jit_guest_instructions = c.jit_guest_instructions.load(std::memory_order_relaxed);
    stats.tier0_regions = c.tier0_regions.load(std::memory_order_relaxed);
    stats.tier_ups = c.tier_ups.load(std::memory_order_relaxed);
    stats.tier_up_cached = c.tier_up_cached.load(std::memory_order_relaxed);
    stats.tier_up_instructions = c.tier_up_instructions.load(std::memory_order_relaxed);
    stats.tier_up_seconds = static_cast<double>(c.tier_up_ticks.load(std::memory_order_relaxed)) * tick;
    stats.tier_replaced_entries = c.tier_replaced_entries.load(std::memory_order_relaxed);
    stats.map_waits = c.map_waits.load(std::memory_order_relaxed);
    stats.map_wait_seconds = static_cast<double>(c.map_wait_ticks.load(std::memory_order_relaxed)) * tick;
    stats.preload_seconds = static_cast<double>(c.preload_ticks.load(std::memory_order_relaxed)) * tick;
    stats.preload_drained_qpc = c.preload_drained_ticks.load(std::memory_order_relaxed);
    stats.smc_fault_seconds = static_cast<double>(c.smc_fault_ticks.load(std::memory_order_relaxed)) * tick;
    stats.code_flushes = c.code_flushes.load(std::memory_order_relaxed);
    stats.code_flush_bytes = c.code_flush_bytes.load(std::memory_order_relaxed);
    stats.code_flush_hits = c.code_flush_hits.load(std::memory_order_relaxed);
    stats.invalidation_interrupts = c.invalidation_interrupts.load(std::memory_order_relaxed);
    stats.invalidation_pages_kept = c.invalidation_pages_kept.load(std::memory_order_relaxed);
    stats.registered_code_translations = c.registered_code_translations.load(std::memory_order_relaxed);
    stats.retranslations = c.retranslations.load(std::memory_order_relaxed);
    stats.jit_reuses = c.jit_reuses.load(std::memory_order_relaxed);
    stats.jit_window_deferred = c.jit_window_deferred.load(std::memory_order_relaxed);
    stats.jit_window_pages_checked = c.jit_window_pages_checked.load(std::memory_order_relaxed);
    stats.jit_window_regions_kept = c.jit_window_regions_kept.load(std::memory_order_relaxed);
    stats.jit_window_regions_killed = c.jit_window_regions_killed.load(std::memory_order_relaxed);
    stats.copy_translations = c.copy_translations.load(std::memory_order_relaxed);
    stats.copy_cache_hits = c.copy_cache_hits.load(std::memory_order_relaxed);
    stats.code_ranges = rt::registered_code_range_count();
    stats.code_range_publishes = c.code_range_publishes.load(std::memory_order_relaxed);
    stats.reclaim_passes = c.reclaim_passes.load(std::memory_order_relaxed);
    stats.reclaimed_bytes = c.reclaimed_chunks.load(std::memory_order_relaxed) * rt::kArenaChunk;
    stats.reclaim_pinned = c.reclaim_pinned.load(std::memory_order_relaxed);
    stats.reclaim_scan_failures = c.reclaim_scan_failures.load(std::memory_order_relaxed);
    stats.evicted_regions = c.evicted_regions.load(std::memory_order_relaxed);
    stats.retired_regions = c.retired_regions.load(std::memory_order_relaxed);
    stats.reclaim_seconds = static_cast<double>(c.reclaim_ticks.load(std::memory_order_relaxed)) * tick;
    stats.reclaim_scan_seconds = static_cast<double>(c.reclaim_scan_ticks.load(std::memory_order_relaxed)) * tick;
    return stats;
}

} // namespace transmute::fx
