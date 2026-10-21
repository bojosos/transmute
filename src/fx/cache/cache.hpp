#pragma once

// Persistent translation cache and AOT hooks (owner: cache; signatures
// additive-only). The runtime and loader call these; when the cache is
// disabled they are cheap no-ops.

#include "fx/cache/abi_version.hpp"
#include "fx/translator/translator.hpp"
#include "transmute/fx/process.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace transmute::fx {

// Loader: a guest module was mapped (before its code runs). The cache opens
// or maps the module's cache file (keyed by image_hash, translator version,
// host CPU features) and indexes its regions.
void cache_module_loaded(const GuestModule& module);
void cache_module_unloaded(const GuestModule& module);
// (additive) Loader, right after mapping a module and before binding its
// imports: starts opening its cache file early. The module is published by
// a later cache_module_loaded with the same base (cache_module_unloaded if
// the load fails); until then the module-ready handler is not called, since
// the module's code is not yet registered as guest code.
void cache_module_mapped(const GuestModule& module);

// Runtime miss path, before translating: a cached region for `guest_pc`
// (any entry point), relocated to the module's current base. False if none.
bool cache_lookup(std::uint64_t guest_pc, TranslatedRegion& region);

// Runtime: a region was translated by the JIT. The cache may persist it
// (asynchronously). Regions outside guest modules are ignored.
void cache_store(const TranslatedRegion& region);

// (additive) Exact copies of a module's code at another address (see
// register_guest_code_copy). cache_lookup_copy: the cached region for
// `source_pc` (in the module) relocated by `delta` to the copy; pinned
// records (code embedding module addresses without fixups) are refused. The
// guest bytes are verified at the source; the caller checks the copy's.
// cache_store_copy: a region translated in the copy, stored for the module
// at its source addresses (region relocated by -delta); ignored if its code
// embeds an address in the copy without a fixup.
bool cache_lookup_copy(std::uint64_t source_pc, std::int64_t delta, TranslatedRegion& region);
void cache_store_copy(const TranslatedRegion& region, std::int64_t delta);

// Process exit / explicit flush: write pending regions.
void cache_flush();
// Process exit, after guest DLL_PROCESS_DETACH (other threads gone): appends
// regions stored since cache_flush (detach code) to this process's journals,
// without merging; the next process indexes them and merges them later.
// Bounded waits only.
void cache_flush_journals() noexcept;

// Process exit, on the exiting thread after every other thread was
// terminated: true if no cache lock or lookup epoch was left held by a
// terminated thread (try-acquire; never blocks). See runtime_exit_safe.
bool cache_exit_quiescent() noexcept;

// AOT: translates every discoverable function of the PE image at `path`
// (entry, exports, .pdata, relocated code pointers) and writes its cache
// file. Returns the number of regions written.
std::size_t aot_translate_image(const std::wstring& path, std::wstring* error);

// ============================================================================
// Additions (cache owner)
// ============================================================================

// Runtime (from runtime_initialize), before the first module load. Without a
// call the cache runs with these defaults. Environment overrides:
// TRANSMUTE_FX_CACHE=0 disables, TRANSMUTE_FX_CACHE_DIR sets the directory.
struct CacheConfig {
    bool enabled = true;
    // Empty: %TRANSMUTE_FX_CACHE_DIR%, else %LOCALAPPDATA%\transmute\fx-cache.
    std::wstring directory;
    // The options the JIT passes to translate_region. Part of the file key, so
    // AOT output is only used by a runtime translating with the same options.
    TranslateOptions translate_options{};
    // Hash the guest bytes a region depends on before reusing it (guards
    // against patched images, hash collisions and relocated literals).
    bool verify_guest_code = true;
    // Background writer period for journal appends.
    unsigned writer_interval_ms = 500;
    // cache_flush folds journals into the module's main file when there is no
    // main file yet, when journals reach max(merge_min_bytes, main size / 8),
    // or when 8+ journals exist. Otherwise they stay as indexed side files.
    std::uint64_t merge_min_bytes = 1u << 20;
    bool merge_always = false;
};
void cache_configure(const CacheConfig& config);

// Identity of a PE image for the cache key: hash of the headers through the
// section table with ImageBase and CheckSum masked, so the raw file bytes and
// the mapped image headers give the same value. The cache keys PE modules by
// this value over the headers mapped at GuestModule::base (so AOT output is
// found whatever the loader puts in GuestModule::image_hash, which is used
// only for modules without PE headers).
std::uint64_t compute_image_hash(const void* image_or_file, std::size_t size) noexcept;

// Key components (exposed for tooling and tests).
std::uint64_t cache_cpu_feature_mask() noexcept;
std::uint64_t cache_options_hash(const TranslateOptions& options) noexcept;
// Main cache file a module maps (it may not exist).
std::wstring cache_file_path(const GuestModule& module);

struct CacheStats {
    std::uint64_t modules_loaded;
    std::uint64_t modules_with_cache;   // main file or journals mapped
    std::uint64_t files_rejected;       // bad header/key/size
    std::uint64_t lookups;
    std::uint64_t hits;
    std::uint64_t records_rejected;     // checksum or structure failure
    std::uint64_t guest_mismatches;     // guest bytes changed
    std::uint64_t stores;
    std::uint64_t stores_ignored;       // outside modules, unencodable, duplicate
    std::uint64_t stores_unrelocatable; // stored pinned: code embeds a module address without a fixup
    std::uint64_t pinned_mismatches;    // pinned records skipped: module mapped at another base
    std::uint64_t records_queued;
    std::uint64_t journal_bytes;
    std::uint64_t merges;
    std::uint64_t cold_files;           // AOT files (<stem>.fxa) mapped
    std::uint64_t cold_hits;            // lookups answered from an AOT file
    std::uint64_t cold_promoted;        // of those, records copied to the journal (first use)
    std::uint64_t aot_requests;         // modules handed to background AOT
    std::uint64_t aot_helpers;          // background AOT helper processes started
};
CacheStats cache_stats() noexcept;

struct AotOptions {
    unsigned threads = 0;               // 0 = one per hardware thread
    bool write_cache = true;            // false = dry run (discover + translate only)
    std::wstring cache_directory;       // empty = the configured cache directory
    // Options for translate_region; nullptr = CacheConfig::translate_options.
    const TranslateOptions* translate_options = nullptr;
    // Also translate direct branch/call targets and BL/BLR continuations
    // reached from translated regions that no region covers yet.
    bool follow_flow = true;
    // Linear sweep of every .pdata function: BL targets and BL/BLR
    // continuations become roots even where no direct flow reaches them
    // (e.g. switch cases entered through a jump table).
    bool sweep_calls = true;
    // Write the module's cold AOT file (<stem>.fxa) instead of merging into
    // its main file: lookups fall back to it after the main file and the
    // journals, and the preloader never walks it (it holds every function;
    // records a run uses are copied to its journal and reach the main file
    // in first-use order). Skipped if a valid one exists. One process at a
    // time per module: <stem>.aot.lock is held while translating; another
    // process waits for it and then skips.
    bool cold_file = false;
};

struct AotStats {
    std::uint64_t functions{};          // distinct static roots (union of the below)
    std::uint64_t entry_point{};
    std::uint64_t exports{};
    std::uint64_t pdata{};
    std::uint64_t relocation_targets{};
    std::uint64_t call_sweep_roots{};   // extra regions for call-sweep roots no function region covered
    std::uint64_t flow_roots{};         // extra regions for branch/call targets found by following flow
    std::uint64_t regions{};
    std::uint64_t entries{};
    std::uint64_t code_bytes{};
    std::uint64_t guest_instructions{};
    std::uint64_t interpreted_instructions{};
    std::uint64_t failed{};             // translate_failed + unencodable
    std::uint64_t translate_failed{};   // translate_region threw
    std::uint64_t unencodable{};        // address outside the image, or guest bytes not in the file
    std::uint64_t unrelocatable{};      // stored pinned to the preferred base (address without a fixup)
    std::uint64_t file_bytes{};         // cache file size written
    unsigned threads{};
    double discover_seconds{};
    double translate_seconds{};
    double write_seconds{};
    double total_seconds{};
    std::wstring cache_path;
};

// Requires the translator families to be registered by the caller
// (register_fp_emitters() etc.), exactly as for the JIT.
std::size_t aot_translate_image(const std::wstring& path, const AotOptions& options, AotStats* stats,
                                std::wstring* error);
// One-line summary for `transmute-fx --aot <image>`.
std::wstring aot_format_stats(const AotStats& stats);

// ---- background AOT ----------------------------------------------------------
// transmute-fx enables this for guest processes (before the first module
// load). A module whose cache has neither a main file nor an AOT file is
// queued once the JIT stored 200 of its regions in this process (modules
// that are loaded but hardly run are left alone). Once the process has run
// for 20 s (TRANSMUTE_FX_BACKGROUND_AOT_DELAY=<seconds>; short runs never
// start one: their whole JIT cost is ~1 s, less than a helper competing for
// the CPU costs them) and shortly after the last request, a helper process
//   <program> --aot-background --aot-threads <n> --cache-dir <dir> --aot <image>...
// translates the queued images (aot_translate_image with cold_file) at
// idle CPU priority (IDLE_PRIORITY_CLASS)
// with at most `threads` workers. A separate process because the AOT of a
// large image (blender.exe: ~25M guest instructions, ~700 MiB of output)
// outlives short guest processes and needs its memory only while it runs.
// When the helper exits, this process maps the new AOT files, so later
// misses hit them; later runs use them from the start.
// `threads` == 0 or an empty `program` disables it; the environment variable
// TRANSMUTE_FX_BACKGROUND_AOT=0|<n> overrides `threads`.
void cache_enable_background_aot(const std::wstring& program, unsigned threads);
// Default worker count for background AOT: hardware threads / 4, 1..4.
unsigned cache_default_background_aot_threads() noexcept;
// Process exit (runtime_prepare_exit): no more background AOT requests, no
// AOT files mapped from now on.
void cache_prepare_exit() noexcept;
// Maps the AOT files that appeared for loaded modules (also done
// automatically when a background helper exits).
void cache_load_cold_files();

// ---- background translation of external modules (additive) -------------------
// aot_translate_image takes PE files. Modules a personality maps itself
// (register_external_module, e.g. Mach-O images) are translated in-process
// from their mapped executable memory instead, by background worker threads
// (src/fx/cache/external_aot.cpp):
//  * flow-ahead, from the start: the direct branch/call targets and the next
//    functions of every region the guest starts using in the module, and of
//    those up to TRANSMUTE_FX_EXTERNAL_AOT_DEPTH (1) levels, newest first, so
//    the guest's next misses find them ready;
//  * static, once the process has run TRANSMUTE_FX_BACKGROUND_AOT_DELAY (20 s)
//    and the workers are otherwise idle: every function start of modules whose
//    text is at most TRANSMUTE_FX_EXTERNAL_AOT_STATIC MiB (2; 0 = none),
//    written as the module's cold AOT file (<stem>.fxa) once complete.
// cache_lookup consults these regions after the main file and journals; the
// ones the guest uses go to the journal, so warm starts preload them. Nothing
// happens for a module that already has a main file, or when the cache is
// off. TRANSMUTE_FX_EXTERNAL_AOT=0|<threads> (default hardware threads / 4,
// 1..3). Call after register_external_module, once the code is mapped.
struct ExternalAotRequest {
    std::uint64_t base{};                 // the module as registered
    std::uint64_t size{};
    std::uint64_t text_begin{};           // executable range; 0, 0 = [base, base + size)
    std::uint64_t text_end{};
    // Function entry points (absolute, any order), e.g. LC_FUNCTION_STARTS:
    // region bounds, "next function" flow-ahead and the static phase.
    std::vector<std::uint64_t> function_starts;
    // Translated first, in this order (initializers, the entry point).
    std::vector<std::uint64_t> roots;
};
void aot_external_module(const ExternalAotRequest& request);

struct ExternalAotStats {
    std::uint64_t regions{};              // translated by the workers
    std::uint64_t guest_instructions{};
    double translate_seconds{};           // worker time (summed over workers)
    std::uint64_t discarded{};            // guest bytes changed while translating, or unencodable
    std::uint64_t hits{};                 // lookups answered by a worker's region
    std::uint64_t waits{};                // lookups that waited for a running translation
    double wait_seconds{};
    std::uint64_t flow_jobs{};            // flow-ahead translations attempted
    std::uint64_t static_regions{};       // regions of the static phase
    std::uint64_t cold_files{};           // .fxa files written
    std::uint64_t lookups{};              // cache lookups that searched a pool
    double lookup_seconds{};              // their time, waits and record checks included
};
ExternalAotStats cache_external_aot_stats() noexcept;

// ---- preloading (runtime) ----------------------------------------------------
// Called once per loaded module whose main cache file is open, from the
// thread that opened it (a cache worker, or a lookup that ran the open
// inline). `sequence` increases with cache_module_loaded order, so a
// preloader can follow the order the loader initializes modules in. The
// handler must not block.
using CacheModuleReadyHandler = void (*)(std::uint64_t base, std::uint64_t sequence);
void cache_set_module_ready_handler(CacheModuleReadyHandler handler);

// Start pcs of the regions stored in the main cache file of the module
// mapped at `base`, in file order (first executed first). A cursor is a
// position in that order: records from `cursor` (0 = first; advanced past the
// returned records) up to, not including, `end` are returned. Only records
// usable at the module's current base are returned. Returns the number
// written (0: nothing left before `end`, or the module is no longer loaded).
inline constexpr std::uint64_t kCacheCursorEnd = ~std::uint64_t{0};
std::size_t cache_region_starts(std::uint64_t base, std::uint64_t& cursor, std::uint64_t end, std::uint64_t* out,
                                std::size_t max);
// Cursor of the main-file record a lookup of `pc` would use (its region
// usually starts at pc), or kCacheCursorEnd if none.
std::uint64_t cache_region_cursor(std::uint64_t pc);

} // namespace transmute::fx
