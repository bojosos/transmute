#pragma once

// Internals shared by the cache runtime, the AOT driver and the cache tests
// (owner: cache). Not a public interface.

#include "fx/cache/cache.hpp"
#include "fx/cache/format.hpp"

#include <span>
#include <string>

namespace transmute::fx::cache_detail {

cachefmt::Key make_key(std::uint64_t image_hash, std::uint64_t file_size, const TranslateOptions& options);
// Hash of the translator, emitter, cache-format and translated-code-ABI
// sources of this build (generated: cmake/FxTranslatorFingerprint.cmake).
// make_key folds it into every key, so another build's code generation never
// reuses this one's files. Tests may replace it (0 restores the build's).
std::uint64_t translator_source_fingerprint() noexcept;
void set_source_fingerprint_for_tests(std::uint64_t value) noexcept;
// Effective cache directory (no trailing separator) and JIT options.
std::wstring effective_directory();
TranslateOptions configured_options();
// <directory>\<sanitized module file name>.<16 hex digits of key digest>
std::wstring stem_path(const std::wstring& directory, const std::wstring& module_name, const cachefmt::Key& key);
std::wstring file_name_of(const std::wstring& path);
bool ensure_directory(const std::wstring& path);
std::uint64_t merge_min_bytes();

// Rewrites <stem>.fxc (temp file + rename, under <stem>.lock) from the
// records of the existing main file when `include_existing`, followed by
// `records` (later wins for the same start RVA).
// `extension` selects the file: ".fxc" (main) or ".fxa" (cold AOT file).
bool write_main(const std::wstring& stem, const cachefmt::Key& key, std::span<const cachefmt::RecordRef> records,
                bool include_existing, cachefmt::BuildResult* result, std::wstring* error,
                const wchar_t* extension = L".fxc");

// Background AOT (background.cpp): queues `image_path` once per process.
void background_aot_request(const std::wstring& image_path);
// Process exit: false if the scheduler's lock was left held.
bool background_aot_quiescent() noexcept;
bool background_aot_stopping() noexcept;
std::uint64_t background_aot_requests() noexcept;
std::uint64_t background_aot_helpers() noexcept;
// Cache background threads (journal writer, AOT scheduler): EcoQoS unless
// TRANSMUTE_FX_BACKGROUND_QOS=default, so they leave performance cores to
// guest threads. `thread` is a HANDLE.
void set_background_thread_qos(void* thread) noexcept;

// Folds the closed journals of `stem` into its main file. Without `force`
// the CacheConfig merge policy decides; `settled` (journals left by earlier
// processes) also merges below the size threshold when the main file is at
// most kSettledRewriteBytes (a cheap one-time rewrite, after which the
// preloader sees those records). True if a main file was written.
inline constexpr std::uint64_t kSettledRewriteBytes = 8u << 20;
bool merge_stem(const std::wstring& stem, bool force, bool settled = false);

// Hash of guest bytes [base + begin, base + end) for each range, SEH-guarded.
bool guest_memory_hash(std::uint64_t base, const cachefmt::DiskRange* ranges, std::uint32_t count,
                       std::uint64_t& out) noexcept;

// Testing: ends the simulated process (journals written and closed when
// `flush`; pending regions dropped as in a crash otherwise; no merge), drops
// every module and starts a new simulated process (new journal names).
void testing_reset(bool flush);

// Cache state of the module mapped at `base` (waits for its files to open):
// -1 not a cached module (unknown, no identity, or the cache is off), 0 no
// main file yet (a cold start of that module), 1 a main file exists. Fills
// the module's file stem and key, whether an AOT file (.fxa) is mapped and
// where the main file's bytes are (tests).
int module_cache_state(std::uint64_t base, std::wstring* stem, cachefmt::Key* key, bool* has_cold,
                       const std::uint8_t** main_data = nullptr);

// ---- background translation of external modules (external_aot.cpp) ----------
using ExternalAotTranslateFn = TranslatedRegion (*)(std::uint64_t guest_pc, const CodeSource& code,
                                                   const TranslateOptions& options);
// aot_external_module with an injectable translator (tests use a stub).
void external_aot_start(const ExternalAotRequest& request, ExternalAotTranslateFn translate);
bool external_aot_active() noexcept;
// Lookup hook (lookup_files, after the main file and journals): calls
// `try_record(context, record, size)` for the pool record with an entry at
// `rva` of the module at `base` (waiting up to ~20 ms while a worker
// translates that very pc). True if try_record accepted it.
using ExternalAotTryFn = bool (*)(void* context, const std::uint8_t* data, std::size_t size);
bool external_aot_lookup(std::uint64_t base, std::uint32_t rva, ExternalAotTryFn try_record, void* context);
// The guest uses a region (a JIT translation stored by cache_store, or a pool
// record): its entries are never translated in the background, and its
// successors are queued (flow-ahead).
void external_aot_note_region(const TranslatedRegion& region);
void external_aot_note_record(std::uint64_t base, const cachefmt::RecordView& record);
void external_aot_module_unloaded(std::uint64_t base);
// Process exit: no more work; waits up to `wait_ms` for running translations.
// True if no worker is inside a translation.
bool external_aot_stop(unsigned wait_ms) noexcept;
// Process exit (cache_exit_quiescent): no worker busy, no pool lock held.
bool external_aot_quiescent() noexcept;
// Testing: stops the workers and forgets every module and counter.
void external_aot_testing_reset();
// Testing: waits (bounded) until the queue is empty and no worker is busy.
void external_aot_testing_wait_idle(unsigned timeout_ms);

} // namespace transmute::fx::cache_detail
