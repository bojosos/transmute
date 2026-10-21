// Persistent translation cache: runtime side (owner: cache).
//
// Files (see format.hpp): one main file per module key, mapped read-only and
// searched through its bucketed index; per-process journals that the
// background writer appends to (records are self-checksummed, so a torn tail
// is detected and ignored). Journals left by earlier processes are mapped and
// indexed too, and cache_flush folds closed journals into the main file
// (temp file + rename under a per-module lock file) when the merge policy
// says so. Concurrent processes never write the same file: a journal is owned
// by its process until closed, and main files are replaced only atomically.
//
// Module load is asynchronous: cache_module_loaded publishes the module and
// queues an OpenTask (identity hash, file size, directory listing, mapping)
// on the system thread pool, so the loader never waits for file-system
// latency. The first lookup in a module runs the task inline or waits for it.
//
// Lookup path: lock-free. Readers enter a two-slot epoch, read an immutable
// module table, and touch only immutable file data. Module load/unload
// publish a new table and wait for the readers of the previous epoch.
//
// Main files are mapped read-only, so every process using a module shares the
// file's pages (a private copy of Chrome Framework's 470 MB main file cost
// each of Chrome's processes 452 MB; docs/ALCHEMY_MEMORY.md). A warm process
// uses nearly every record, so the open task prefetches the whole view: the
// pages arrive in large reads instead of one fault at a time. The
// runtime's preloader (cache_set_module_ready_handler, cache_region_starts)
// walks the records in file order -- first-execution order -- and installs
// them before the guest asks.

#include "fx/cache/cache.hpp"

#include "fx/cache/cache_internal.hpp"
#include "fx/cache/format.hpp"
#include "fx/cache/pe_view.hpp"
#include "fx/common/cpu_features.hpp"
#include "fx/host/env.hpp"
#include "fx/host/file.hpp"
#include "fx/host/guard.hpp"
#include "fx/host/process.hpp"
#include "fx/host/sync.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/vm.hpp"

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transmute::fx {
std::uint64_t cache_family_fingerprint_absent() { return 0; }
}
#pragma comment(linker, "/alternatename:?translator_family_fingerprint@fx@transmute@@YA_KXZ=?cache_family_fingerprint_absent@fx@transmute@@YA_KXZ")

namespace transmute::fx {
namespace {

using cachefmt::DiskRange;
using cachefmt::Key;
using cachefmt::MainView;
using cachefmt::RecordView;

constexpr std::uint64_t kWakeBytes = 1u << 20;  // wake the writer early past this
constexpr unsigned kMaxCandidates = 4;
// JIT regions stored for a module (this process) before it is queued for
// background AOT: modules that are loaded but hardly run are not worth
// translating whole (Blender loads ~50 ARM64 DLLs, ~1.7 GB of AOT output).
constexpr std::size_t kBackgroundAotAfterStores = 200;

// ---------------------------------------------------------------------------
// small helpers

namespace file = host::file;
using Exclusive = host::ExclusiveLock;
using Shared = host::SharedLock;

std::wstring hex(std::uint64_t value, int digits) {
    static constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring s(static_cast<std::size_t>(digits), L'0');
    for (int i = digits - 1; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = kDigits[value & 15];
        value >>= 4;
    }
    return s;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool ends_with(const std::wstring& s, const wchar_t* suffix) {
    const std::size_t n = std::wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::wstring env(const wchar_t* name) {
    wchar_t buffer[1024];
    const auto n = host::get_env(name, buffer, 1024);
    return n > 0 && n < 1024 ? std::wstring(buffer, n) : std::wstring();
}

std::wstring strip_separators(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

std::wstring default_directory() {
    std::wstring dir = env(L"TRANSMUTE_FX_CACHE_DIR");
    if (!dir.empty()) return strip_separators(dir);
    dir = env(L"LOCALAPPDATA");
    if (!dir.empty()) return strip_separators(dir) + L"\\transmute\\fx-cache";
    return strip_separators(file::temp_directory()) + L"\\transmute\\fx-cache";
}

std::uint64_t now_filetime() { return host::wall_time(); }

bool guest_hash_unguarded(std::uint64_t base, const DiskRange* ranges, std::uint32_t count, std::uint64_t& out) {
    std::uint64_t h = cachefmt::guest_hash_init(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t size = ranges[i].end_rva - ranges[i].begin_rva;
        h = cachefmt::guest_hash_step(h, ranges[i].begin_rva,
                                      reinterpret_cast<const void*>(base + ranges[i].begin_rva), size);
    }
    out = h;
    return true;
}

// Access violations and in-page errors fail (host::call_guarded_access).
bool header_identity_guarded(const std::uint8_t* data, std::size_t size, std::uint64_t& out) noexcept {
    bool ok = false;
    if (!host::call_guarded_access([&] { ok = pe::header_identity(data, size, out); })) return false;
    return ok;
}

// ---------------------------------------------------------------------------
// module state

struct JournalRef {
    std::uint32_t rva;
    std::uint32_t rank;  // region starts first, then newer records first
    const std::uint8_t* record;
    std::size_t avail;
};

// Per-process write state of one module key. Outlives module unloads (a DLL
// loaded again reuses its journal and dedupe set); freed only by testing_reset.
struct StemWriter {
    std::wstring stem;
    Key key{};
    // Guarded by `lock`.
    host::RwLock lock;
    std::vector<std::uint8_t> pending;
    std::unordered_set<std::uint64_t> stored;   // stored_key(start rva, tier 0)
    // Guarded by Global::write_lock.
    file::File journal = nullptr;
    bool journal_failed{};
};

inline std::uint64_t stored_key(std::uint32_t start_rva, bool tier0) noexcept {
    return std::uint64_t{start_rva} | (tier0 ? std::uint64_t{1} << 32 : 0);
}

// AOT output of a module (<stem>.fxa): looked up last, never preloaded.
// Mapped (demand-paged): it covers every function, and a run touches few.
struct ColdFile {
    pe::MappedFile file;
    MainView view{};
};

// What a module's key resolved to. Immutable once built, except `cold`,
// which is set at most once (background AOT finishing while the module is
// loaded) and freed with the files.
struct ModuleFiles {
    bool identity{};  // false: no image hash, the module is never cached
    Key key{};
    std::wstring stem;
    StemWriter* writer{};
    pe::MappedFile main_file;
    MainView main{};
    bool has_main{};
    std::vector<pe::MappedFile> journal_files;
    std::vector<JournalRef> jindex;  // sorted by (rva, rank)
    mutable std::atomic<const ColdFile*> cold{nullptr};
    ModuleFiles() = default;
    ModuleFiles(const ModuleFiles&) = delete;
    ModuleFiles& operator=(const ModuleFiles&) = delete;
    ~ModuleFiles() { delete cold.load(); }
};

struct OpenTask {
    GuestModule module;
    std::wstring directory;
    TranslateOptions options;
    std::uint64_t sequence{};  // cache_module_loaded order
    std::atomic<int> state{0};  // 0 queued, 1 running, 2 done
    // The loader published the module (cache_module_loaded); before that the
    // ready handler must not run (cache_module_mapped).
    std::atomic<bool> published{true};
    std::atomic<bool> has_main{false};   // set before state = 2
    std::atomic<bool> ready_fired{false};
    ModuleFiles* result{};
    std::atomic<bool> adopted{false};
    host::RwLock lock;
    host::CondVar done;
    ~OpenTask() {
        if (!adopted.load()) delete result;
    }
};

struct CacheModule {
    std::uint64_t base{};
    std::uint64_t size{};
    std::shared_ptr<OpenTask> task;
    std::atomic<ModuleFiles*> files{nullptr};  // adopted from task->result
    ~CacheModule() { delete files.load(); }
};

struct ModuleTable {
    std::vector<CacheModule*> modules;  // sorted by base
};

struct JournalFile {
    std::wstring path;
    std::uint64_t time;
    std::uint64_t size;
};

struct Global {
    // Configuration (config_lock).
    host::RwLock config_lock;
    bool configured{};
    CacheConfig config;
    std::wstring directory;
    std::atomic<unsigned> writer_interval_ms{500};

    // Module registry (registry_lock) and the lock-free published table.
    host::RwLock registry_lock;
    std::vector<CacheModule*> modules;
    std::atomic<const ModuleTable*> table{nullptr};
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::int64_t> readers[2]{};
    std::atomic<std::int64_t> tasks_in_flight{0};
    std::atomic<std::int64_t> tasks_running{0};  // build_files in progress (process-exit check)

    // Writers by stem (writers_lock); journal writing (write_lock).
    host::RwLock writers_lock;
    std::unordered_map<std::wstring, StemWriter*> writers;
    host::RwLock write_lock;
    std::uint32_t session{};
    std::uint32_t journal_sequence{};
    std::atomic<std::uint64_t> pending_bytes{0};

    // Background writer.
    host::RwLock wake_lock;
    host::CondVar wake_cv;
    bool wake{};
    std::atomic<bool> writer_started{false};

    // Directory listing taken once per process (listing_lock).
    host::RwLock listing_lock;
    bool listing_valid{};
    std::wstring listing_directory;
    std::unordered_set<std::wstring> listing_mains;
    std::unordered_set<std::wstring> listing_colds;
    std::unordered_map<std::wstring, std::vector<JournalFile>> listing_journals;

    // Statistics.
    std::atomic<std::uint64_t> modules_loaded{}, modules_with_cache{}, files_rejected{}, lookups{}, hits{},
        records_rejected{}, guest_mismatches{}, stores{}, stores_ignored{}, stores_unrelocatable{}, pinned_mismatches{}, records_queued{},
        journal_bytes{}, merges{}, cold_files{}, cold_hits{}, cold_promoted{};
};

Global& G() {
    static Global* const global = new Global();  // never destroyed: usable during process exit
    return *global;
}

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_verify{true};
std::atomic<CacheModuleReadyHandler> g_ready_handler{nullptr};
std::atomic<std::uint64_t> g_load_sequence{0};

void apply_config(Global& g, const CacheConfig& config) {
    g.config = config;
    g.directory = config.directory.empty() ? default_directory() : strip_separators(config.directory);
    const std::wstring switch_value = lower(env(L"TRANSMUTE_FX_CACHE"));
    const bool env_off = switch_value == L"0" || switch_value == L"off" || switch_value == L"false";
    g_enabled.store(config.enabled && !env_off);
    g_verify.store(config.verify_guest_code);
    g.writer_interval_ms.store(config.writer_interval_ms ? config.writer_interval_ms : 500);
    g.configured = true;
    Exclusive listing(g.listing_lock);
    g.listing_valid = false;
}

Global& configured() {
    Global& g = G();
    {
        Shared lock(g.config_lock);
        if (g.configured) return g;
    }
    Exclusive lock(g.config_lock);
    if (!g.configured) apply_config(g, CacheConfig{});
    return g;
}

// ---------------------------------------------------------------------------
// epoch-protected readers

class ReadGuard {
public:
    explicit ReadGuard(Global& g) : g_(g) {
        for (;;) {
            const std::uint64_t e = g.epoch.load();
            slot_ = static_cast<unsigned>(e & 1);
            g.readers[slot_].fetch_add(1);
            if (g.epoch.load() == e) break;
            g.readers[slot_].fetch_sub(1);
        }
    }
    ~ReadGuard() { g_.readers[slot_].fetch_sub(1, std::memory_order_release); }
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;

private:
    Global& g_;
    unsigned slot_{};
};

// Waits until no reader can still hold anything unpublished before the call.
void synchronize(Global& g) {
    const std::uint64_t old = g.epoch.fetch_add(1);
    while (g.readers[old & 1].load() != 0) _mm_pause();
}

// registry_lock held exclusively.
void publish_locked(Global& g) {
    auto* table = new ModuleTable{g.modules};
    std::sort(table->modules.begin(), table->modules.end(),
              [](const CacheModule* a, const CacheModule* b) { return a->base < b->base; });
    const ModuleTable* old = g.table.exchange(table);
    synchronize(g);
    delete old;
}

CacheModule* find_module(const ModuleTable* table, std::uint64_t address) {
    if (!table || table->modules.empty()) return nullptr;
    const auto& v = table->modules;
    auto it = std::upper_bound(v.begin(), v.end(), address,
                               [](std::uint64_t a, const CacheModule* m) { return a < m->base; });
    if (it == v.begin()) return nullptr;
    --it;
    return address - (*it)->base < (*it)->size ? *it : nullptr;
}

// ---------------------------------------------------------------------------
// module identity and files

std::wstring module_name(const GuestModule& module) {
    if (!module.name.empty()) return module.name;
    if (!module.path.empty()) return cache_detail::file_name_of(module.path);
    return L"module";
}

// Image identity for the key. PE modules use compute_image_hash over the
// mapped headers (ImageBase/CheckSum masked), exactly what AOT computes from
// the file, so AOT output is found whatever GuestModule::image_hash the loader
// derives (the loader's also covers the file's last-write time, which changes
// on copy/install). GuestModule::image_hash identifies non-PE guest modules.
// Correctness never rests on this: every region is verified against the guest
// bytes it was translated from.
bool module_identity(const GuestModule& module, std::uint64_t& image_hash, std::uint64_t& file_size) {
    image_hash = 0;
    file_size = 0;
    if (!module.path.empty()) {
        file::attributes(module.path, &file_size, nullptr);
    }
    if (module.base != 0 && module.size != 0) {
        header_identity_guarded(reinterpret_cast<const std::uint8_t*>(module.base),
                                static_cast<std::size_t>(std::min<std::uint64_t>(module.size, 64 * 1024)), image_hash);
    }
    if (image_hash == 0) image_hash = module.image_hash;
    if (image_hash == 0 && !module.path.empty()) {
        pe::MappedFile file;
        if (file.open(module.path, true)) image_hash = pe::image_identity(file.data(), file.size());
    }
    return image_hash != 0;
}

void ensure_listing(Global& g, const std::wstring& directory) {
    Exclusive lock(g.listing_lock);
    if (g.listing_valid && g.listing_directory == directory) return;
    g.listing_valid = true;
    g.listing_directory = directory;
    g.listing_mains.clear();
    g.listing_colds.clear();
    g.listing_journals.clear();
    for (const file::Entry& entry : file::list(directory + L"\\*", /*large=*/true)) {
        if (entry.directory) continue;
        const std::wstring name = lower(entry.name);
        if (ends_with(name, L".fxc")) {
            g.listing_mains.insert(directory + L"\\" + name.substr(0, name.size() - 4));
        } else if (ends_with(name, L".fxa")) {
            g.listing_colds.insert(directory + L"\\" + name.substr(0, name.size() - 4));
        } else if (ends_with(name, L".fxj")) {
            const std::wstring base = name.substr(0, name.size() - 4);
            const std::size_t dot = base.rfind(L'.');
            if (dot == std::wstring::npos) continue;
            g.listing_journals[directory + L"\\" + base.substr(0, dot)].push_back(
                {directory + L"\\" + name, entry.write_time, entry.size});
        }
    }
}

// Maps <stem>.fxa if it holds a valid main-format file for `key`.
ColdFile* open_cold(Global& g, const std::wstring& stem, const Key& key) {
    auto cold = std::make_unique<ColdFile>();
    if (!cold->file.open(stem + L".fxa", false)) return nullptr;
    if (!cachefmt::open_main(cold->file.data(), cold->file.size(), key, cold->view)) {
        g.files_rejected.fetch_add(1);
        return nullptr;
    }
    g.cold_files.fetch_add(1);
    return cold.release();
}

StemWriter* writer_for(Global& g, const std::wstring& stem, const Key& key) {
    {
        Shared lock(g.writers_lock);
        auto it = g.writers.find(stem);
        if (it != g.writers.end()) return it->second;
    }
    Exclusive lock(g.writers_lock);
    StemWriter*& w = g.writers[stem];
    if (!w) {
        w = new StemWriter();
        w->stem = stem;
        w->key = key;
    }
    return w;
}

ModuleFiles* build_files(Global& g, const OpenTask& task) {
    auto f = std::make_unique<ModuleFiles>();
    std::uint64_t image_hash = 0, file_size = 0;
    if (!module_identity(task.module, image_hash, file_size)) return f.release();
    f->identity = true;
    f->key = cache_detail::make_key(image_hash, file_size, task.options);
    f->stem = cache_detail::stem_path(task.directory, module_name(task.module), f->key);
    f->writer = writer_for(g, f->stem, f->key);

    ensure_listing(g, task.directory);
    bool main_listed = false, cold_listed = false;
    std::vector<JournalFile> journals;
    {
        Shared lock(g.listing_lock);
        main_listed = g.listing_mains.count(f->stem) != 0;
        cold_listed = g.listing_colds.count(f->stem) != 0;
        auto it = g.listing_journals.find(f->stem);
        if (it != g.listing_journals.end()) journals = it->second;
    }
    // Main files are shared read-only views, prefetched on this pool thread:
    // nearly every record of a warm cache is used, and faulting the view in
    // page by page on the lookup path would cost far more than large reads.
    // The checks below and the per-record checksums read the view as they
    // read a private copy.
    const std::wstring main_path = f->stem + L".fxc";
    if (main_listed && f->main_file.open(main_path, false)) {
        host::vm::prefetch(f->main_file.data(), f->main_file.size());
        if (cachefmt::open_main(f->main_file.data(), f->main_file.size(), f->key, f->main)) {
            f->has_main = true;
        } else {
            g.files_rejected.fetch_add(1);
            f->main_file.close();
        }
    }
    // Not listed but no main file either: a background helper of this
    // process may have written it after the listing was taken.
    if (cold_listed || !f->has_main) {
        if (ColdFile* cold = open_cold(g, f->stem, f->key)) f->cold.store(cold);
    }
    std::sort(journals.begin(), journals.end(),
              [](const JournalFile& a, const JournalFile& b) { return a.time < b.time; });
    std::uint32_t sequence = 0;
    for (const JournalFile& j : journals) {
        pe::MappedFile file;
        if (!file.open(j.path, true)) continue;
        if (!cachefmt::validate_header(file.data(), file.size(), cachefmt::kKindJournal, f->key)) {
            g.files_rejected.fetch_add(1);
            continue;
        }
        const std::size_t before = f->jindex.size();
        const std::uint8_t* data = file.data();
        const std::size_t size = file.size();
        // Structure only; checksums are verified lazily per lookup.
        cachefmt::walk_records(data, sizeof(cachefmt::FileHeader), size, false,
                               [&](std::size_t offset, const RecordView& view) {
                                   const std::uint32_t age = 0x7FFF'FFFFu - std::min(sequence++, 0x7FFF'FFFFu);
                                   for (std::uint32_t e = 0; e < view.header->n_entries; ++e) {
                                       const std::uint32_t rva = view.entries[e].rva;
                                       // Tier-1 records first, then the region starting at rva, then newer.
                                       const std::uint32_t rank =
                                           (cachefmt::record_tier0(*view.header) ? 0x8000'0000u : 0u) |
                                           (rva == view.header->start_rva ? 0u : 0x4000'0000u) | (age & 0x3FFF'FFFFu);
                                       f->jindex.push_back({rva, rank, data + offset, size - offset});
                                   }
                               });
        if (f->jindex.size() != before) f->journal_files.push_back(std::move(file));
    }
    std::sort(f->jindex.begin(), f->jindex.end(), [](const JournalRef& a, const JournalRef& b) {
        return a.rva != b.rva ? a.rva < b.rva : a.rank < b.rank;
    });
    if (f->has_main || !f->jindex.empty() || f->cold.load()) g.modules_with_cache.fetch_add(1);
    return f.release();
}

// The module's main file is open and the loader published the module: the
// runtime may start installing its regions (once per task).
void fire_ready(OpenTask& task) {
    if (task.ready_fired.exchange(true)) return;
    if (const CacheModuleReadyHandler handler = g_ready_handler.load(std::memory_order_acquire))
        handler(task.module.base, task.sequence);
}

// Runs `task` if nobody has started it, otherwise waits for it.
void run_task(Global& g, OpenTask& task) {
    int expected = 0;
    if (task.state.compare_exchange_strong(expected, 1)) {
        g.tasks_running.fetch_add(1);
        ModuleFiles* files = build_files(g, task);
        g.tasks_running.fetch_sub(1);
        const bool has_main = files->has_main;  // `files` may be freed once published
        task.has_main.store(has_main);
        {
            Exclusive lock(task.lock);
            task.result = files;
            task.state.store(2);
        }
        task.done.notify_all();
        // seq_cst against cache_module_loaded: one of the two sees the other.
        if (has_main && task.published.load()) fire_ready(task);
        return;
    }
    Exclusive lock(task.lock);
    while (task.state.load() != 2) task.done.wait_for(task.lock, host::kWaitForever);
}

void open_callback(void* context) {
    auto* holder = static_cast<std::shared_ptr<OpenTask>*>(context);
    Global& g = G();
    run_task(g, **holder);
    delete holder;
    g.tasks_in_flight.fetch_sub(1);
}

void submit(Global& g, const std::shared_ptr<OpenTask>& task) {
    auto* holder = new std::shared_ptr<OpenTask>(task);
    g.tasks_in_flight.fetch_add(1);
    if (!host::submit_to_thread_pool(open_callback, holder)) {
        delete holder;  // the first lookup runs it inline
        g.tasks_in_flight.fetch_sub(1);
    }
}

// Files of `m` if its task has finished (adopting them), else nullptr.
// Caller holds a ReadGuard.
const ModuleFiles* ready_files(CacheModule& m) {
    ModuleFiles* f = m.files.load(std::memory_order_acquire);
    if (f || !m.task || m.task->state.load(std::memory_order_acquire) != 2) return f;
    ModuleFiles* expected = nullptr;
    if (m.files.compare_exchange_strong(expected, m.task->result)) {
        m.task->adopted.store(true);
        return m.task->result;
    }
    return expected;
}

// Runs `fn(module, files)` for the module containing `address` once its files
// are ready, without holding the epoch while waiting for file I/O. Returns
// false (without calling fn) if there is no such module.
template <typename Fn>
bool with_module(Global& g, std::uint64_t address, Fn&& fn) {
    std::shared_ptr<OpenTask> pending;
    {
        ReadGuard guard(g);
        CacheModule* m = find_module(g.table.load(std::memory_order_acquire), address);
        if (!m) return false;
        if (const ModuleFiles* f = ready_files(*m)) return fn(*m, *f);
        if (!m->task) return false;
        pending = m->task;
    }
    run_task(g, *pending);
    ReadGuard guard(g);
    CacheModule* m = find_module(g.table.load(std::memory_order_acquire), address);
    if (!m || m->task != pending) return false;
    const ModuleFiles* f = ready_files(*m);
    return f && fn(*m, *f);
}

// ---------------------------------------------------------------------------
// lookup

bool try_record(Global& g, const CacheModule& m, const std::uint8_t* data, std::size_t avail, std::uint32_t rva,
                TranslatedRegion& out, RecordView* used = nullptr, bool verify_checksum = true) {
    RecordView v;
    if (!cachefmt::decode_record(data, avail, verify_checksum, v)) {
        g.records_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const cachefmt::RecordHeader& h = *v.header;
    bool is_entry = false;
    for (std::uint32_t i = 0; i < h.n_entries && !is_entry; ++i) is_entry = v.entries[i].rva == rva;
    if (!is_entry) return false;
    // Pinned code embeds addresses of the base it was translated at.
    if (!cachefmt::usable_at(h, m.base)) {
        g.pinned_mismatches.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Everything the region refers to must lie inside the module as mapped now.
    const std::uint64_t size = m.size;
    for (std::uint32_t i = 0; i < h.n_ranges; ++i)
        if (v.ranges[i].end_rva > size) return false;
    for (std::uint32_t i = 0; i < h.n_exits; ++i)
        if (v.exits[i].target_rva > size) return false;
    for (std::uint32_t i = 0; i < h.n_fixups; ++i)
        if (v.fixups[i].kind == static_cast<std::uint32_t>(FixupKind::guest_address_imm64) && v.fixups[i].rva > size)
            return false;
    if (g_verify.load(std::memory_order_relaxed)) {
        std::uint64_t hash = 0;
        if (!cache_detail::guest_memory_hash(m.base, v.ranges, h.n_ranges, hash) || hash != h.guest_hash) {
            g.guest_mismatches.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    cachefmt::rebuild_region(v, m.base, out);
    g.hits.fetch_add(1, std::memory_order_relaxed);
    if (used) *used = v;
    return true;
}

void start_writer(Global& g);
void wake_writer(Global& g);

// A cold (AOT) record the guest used: appended as-is to this process's
// journal, so the next merge puts it in the main file in first-use order
// (where the preloader finds it) instead of leaving it among every AOT region.
void promote_cold(Global& g, const ModuleFiles& f, const RecordView& v) {
    if (!f.writer) return;
    const auto bytes = v.bytes();
    StemWriter& w = *f.writer;
    {
        Exclusive lock(w.lock);
        if (!w.stored.insert(stored_key(v.header->start_rva, cachefmt::record_tier0(*v.header))).second) return;
        w.pending.insert(w.pending.end(), bytes.begin(), bytes.end());
    }
    g.cold_promoted.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t pending = g.pending_bytes.fetch_add(bytes.size()) + bytes.size();
    start_writer(g);
    if (pending >= kWakeBytes) wake_writer(g);
}

bool lookup_files(Global& g, const CacheModule& m, const ModuleFiles& f, std::uint64_t pc, TranslatedRegion& out) {
    const std::uint64_t rel = pc - m.base;
    if (rel > 0xFFFF'FFFFull) return false;
    const auto rva = static_cast<std::uint32_t>(rel);
    if (!f.jindex.empty()) {
        auto it = std::lower_bound(f.jindex.begin(), f.jindex.end(), rva,
                                   [](const JournalRef& r, std::uint32_t value) { return r.rva < value; });
        for (unsigned n = 0; it != f.jindex.end() && it->rva == rva && n < kMaxCandidates; ++it, ++n)
            if (try_record(g, m, it->record, it->avail, rva, out)) return true;
    }
    if (f.has_main) {
        std::uint64_t offsets[kMaxCandidates];
        const unsigned n = cachefmt::main_find(f.main, rva, offsets, kMaxCandidates);
        for (unsigned i = 0; i < n; ++i)
            if (try_record(g, m, f.main.data + offsets[i], f.main.size - offsets[i], rva, out)) return true;
    }
    // Background translations of an external module (external_aot.cpp): used
    // records go to the journal like cold hits, and the guest's path seeds
    // further flow-ahead.
    if (cache_detail::external_aot_active()) {
        struct Context {
            Global& g;
            const CacheModule& m;
            const ModuleFiles& f;
            std::uint32_t rva;
            TranslatedRegion& out;
        } context{g, m, f, rva, out};
        const auto try_pool = [](void* p, const std::uint8_t* data, std::size_t size) {
            auto& c = *static_cast<Context*>(p);
            RecordView used;
            // Built in this process (never read from disk): no record checksum to verify.
            if (!try_record(c.g, c.m, data, size, c.rva, c.out, &used, false)) return false;
            promote_cold(c.g, c.f, used);
            cache_detail::external_aot_note_record(c.m.base, used);
            return true;
        };
        if (cache_detail::external_aot_lookup(m.base, rva, try_pool, &context)) return true;
    }
    if (const ColdFile* cold = f.cold.load(std::memory_order_acquire)) {
        std::uint64_t offsets[kMaxCandidates];
        const unsigned n = cachefmt::main_find(cold->view, rva, offsets, kMaxCandidates);
        for (unsigned i = 0; i < n; ++i) {
            RecordView used;
            if (try_record(g, m, cold->view.data + offsets[i], cold->view.size - offsets[i], rva, out, &used)) {
                g.cold_hits.fetch_add(1, std::memory_order_relaxed);
                promote_cold(g, f, used);
                return true;
            }
        }
    }
    return false;
}

// True if a record starting at `rva` with guest hash `hash`, usable at the
// module's current base, is already on disk (for a tier-1 region: a tier-1 record).
bool already_cached(const CacheModule& m, const ModuleFiles& f, std::uint32_t rva, std::uint64_t hash, bool tier0) {
    auto matches = [&](const std::uint8_t* data, std::size_t avail) {
        RecordView v;
        return cachefmt::decode_record(data, avail, false, v) && v.header->start_rva == rva &&
               v.header->guest_hash == hash && cachefmt::usable_at(*v.header, m.base) &&
               (tier0 || !cachefmt::record_tier0(*v.header));
    };
    auto it = std::lower_bound(f.jindex.begin(), f.jindex.end(), rva,
                               [](const JournalRef& r, std::uint32_t value) { return r.rva < value; });
    for (; it != f.jindex.end() && it->rva == rva; ++it)
        if (matches(it->record, it->avail)) return true;
    if (f.has_main) {
        std::uint64_t offsets[kMaxCandidates];
        const unsigned n = cachefmt::main_find(f.main, rva, offsets, kMaxCandidates);
        for (unsigned i = 0; i < n; ++i)
            if (matches(f.main.data + offsets[i], f.main.size - offsets[i])) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// journal writing

constexpr unsigned kForever = 0xffffffffu;   // acquire_write_lock / drain_all without a timeout

bool acquire_write_lock(Global& g, unsigned timeout_ms) {
    if (timeout_ms == kForever) {
        g.write_lock.lock();
        return true;
    }
    const std::uint64_t deadline = host::monotonic_ms() + timeout_ms;
    while (!g.write_lock.try_lock()) {
        if (host::monotonic_ms() >= deadline) return false;
        host::sleep_ms(1);
    }
    return true;
}

bool write_all(file::File h, const std::uint8_t* data, std::size_t size) { return file::write_all(h, data, size); }

// write_lock held.
bool open_journal(Global& g, StemWriter& w) {
    const std::size_t slash = w.stem.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !cache_detail::ensure_directory(w.stem.substr(0, slash))) return false;
    const std::wstring path = w.stem + L".j" + hex(host::process_id(), 8) + L"-" + hex(g.session, 4) + L"-" +
                              hex(g.journal_sequence++, 6) + L".fxj";
    // Write access without write sharing marks the journal as live: mergers skip it.
    file::File h = file::open(path, file::kWrite, file::kRead | file::kDelete, file::Create::create_new,
                              file::Flags::normal);
    if (!h) return false;
    cachefmt::FileHeader header{};
    header.magic = cachefmt::kFileMagic;
    header.header_size = sizeof(header);
    header.kind = cachefmt::kKindJournal;
    header.key = w.key;
    header.created_filetime = now_filetime();
    header.creator_pid = host::process_id();
    header.header_checksum = cachefmt::header_checksum(header);
    if (!write_all(h, reinterpret_cast<const std::uint8_t*>(&header), sizeof(header))) {
        file::close(h);
        file::remove(path);
        return false;
    }
    w.journal = h;
    return true;
}

// write_lock held.
void drain_writer(Global& g, StemWriter& w, bool close_journal) {
    std::vector<std::uint8_t> bytes;
    {
        Exclusive lock(w.lock);
        bytes.swap(w.pending);
    }
    if (!bytes.empty()) {
        g.pending_bytes.fetch_sub(bytes.size());
        if (!w.journal_failed && !w.journal && !open_journal(g, w)) w.journal_failed = true;
        if (w.journal) {
            if (write_all(w.journal, bytes.data(), bytes.size())) g.journal_bytes.fetch_add(bytes.size());
            else w.journal_failed = true;
        }
    }
    if (close_journal && w.journal) {
        file::close(w.journal);
        w.journal = nullptr;
    }
}

bool drain_all(Global& g, bool close_journals, unsigned timeout_ms) {
    if (!acquire_write_lock(g, timeout_ms)) return false;
    std::vector<StemWriter*> snapshot;
    {
        Shared lock(g.writers_lock);
        snapshot.reserve(g.writers.size());
        for (const auto& [stem, w] : g.writers) snapshot.push_back(w);
    }
    for (StemWriter* w : snapshot) drain_writer(g, *w, close_journals);
    g.write_lock.unlock();
    return true;
}

std::uint32_t writer_main(void*) {
    Global& g = G();
    for (;;) {
        g.wake_lock.lock();
        if (!g.wake) g.wake_cv.wait_for(g.wake_lock, g.writer_interval_ms.load());
        g.wake = false;
        g.wake_lock.unlock();
        if (g.pending_bytes.load() != 0) drain_all(g, false, kForever);
    }
}

void start_writer(Global& g) {
    if (g.writer_started.load(std::memory_order_relaxed) || g.writer_started.exchange(true)) return;
    host::ThreadHandle thread = host::create_thread(writer_main, nullptr, 256 * 1024, host::kThreadStackReservation, nullptr);
    if (thread) {
        host::set_thread_priority(thread, host::ThreadPriority::below_normal);
        cache_detail::set_background_thread_qos(thread);
        host::close_thread(thread);
    }
}

void wake_writer(Global& g) {
    {
        Exclusive lock(g.wake_lock);
        g.wake = true;
    }
    g.wake_cv.notify_one();
}

bool store_in(Global& g, const CacheModule& m, const ModuleFiles& f, const TranslatedRegion& region) {
    if (!f.identity || !f.writer) return false;
    thread_local std::vector<DiskRange> ranges;
    thread_local std::vector<std::uint8_t> record;
    ranges.clear();
    for (const auto& [begin, end] : region.guest_ranges) {
        if (begin < m.base || end < begin || end - m.base > m.size) return false;
        ranges.push_back({static_cast<std::uint32_t>(begin - m.base), static_cast<std::uint32_t>(end - m.base)});
    }
    std::uint64_t hash = 0;
    const auto start_rva = static_cast<std::uint32_t>(region.guest_start - m.base);
    if (!cache_detail::guest_memory_hash(m.base, ranges.data(), static_cast<std::uint32_t>(ranges.size()), hash) ||
        already_cached(m, f, start_rva, hash, region.tier == 0))
        return false;
    // Code that embeds module addresses without fixups is kept, pinned to this
    // base: reused only while the module maps here (the loader is deterministic
    // in most runs), a miss elsewhere.
    const bool pinned = cachefmt::unrecorded_address(region, m.base, m.size) >= 0;
    record.clear();
    if (!cachefmt::encode_record(region, m.base, m.size, hash, record, pinned)) return false;
    if (pinned) g.stores_unrelocatable.fetch_add(1, std::memory_order_relaxed);
    StemWriter& w = *f.writer;
    std::size_t stored = 0;
    {
        Exclusive lock(w.lock);
        // Keyed by start and tier: a tier-up stores its tier-1 region after
        // the tier-0 region it replaces.
        if (!w.stored.insert(stored_key(start_rva, region.tier == 0)).second) return false;
        stored = w.stored.size();
        w.pending.insert(w.pending.end(), record.begin(), record.end());
    }
    // A module with nothing cached yet whose code the guest keeps running
    // (not merely loaded): translate all of it ahead of time.
    // (PE images only: pdata_rva is 0 for external, e.g. Mach-O, modules.)
    if (stored == kBackgroundAotAfterStores && !f.has_main && !f.cold.load(std::memory_order_relaxed) && m.task &&
        !m.task->module.path.empty() && m.task->module.pdata_rva != 0)
        cache_detail::background_aot_request(m.task->module.path);
    g.records_queued.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t pending = g.pending_bytes.fetch_add(record.size()) + record.size();
    start_writer(g);
    if (pending >= kWakeBytes) wake_writer(g);
    return true;
}

} // namespace

// ===========================================================================
// internal API

namespace cache_detail {

std::wstring file_name_of(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// translator_family_fingerprint() lives in fx_translator, which the stub-
// translator cache tests do not link; fall back to 0 there.
std::uint64_t family_fingerprint() { return translator_family_fingerprint(); }

Key make_key(std::uint64_t image_hash, std::uint64_t file_size, const TranslateOptions& options) {
    Key key{};
    key.format_version = cachefmt::kFormatVersion;
    key.abi_version = kTranslatorAbiVersion;
    key.image_hash = image_hash;
    key.file_size = file_size;
    key.cpu_features = cache_cpu_feature_mask();
    // Regions also depend on which instruction families were registered.
    key.options_hash = cachefmt::mix64(cache_options_hash(options), family_fingerprint());
    return key;
}

std::wstring effective_directory() {
    Global& g = configured();
    Shared lock(g.config_lock);
    return g.directory;
}

TranslateOptions configured_options() {
    Global& g = configured();
    Shared lock(g.config_lock);
    return g.config.translate_options;
}

std::uint64_t merge_min_bytes() {
    Global& g = configured();
    Shared lock(g.config_lock);
    return g.config.merge_min_bytes;
}

std::wstring stem_path(const std::wstring& directory, const std::wstring& module_name, const Key& key) {
    std::wstring name;
    for (wchar_t c : lower(module_name)) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_';
        name.push_back(ok ? c : L'_');
        if (name.size() >= 96) break;
    }
    if (name.empty()) name = L"module";
    return strip_separators(directory) + L"\\" + name + L"." + hex(key.digest(), 16);
}

bool guest_memory_hash(std::uint64_t base, const DiskRange* ranges, std::uint32_t count,
                       std::uint64_t& out) noexcept {
    bool ok = false;
    if (!host::call_guarded_access([&] { ok = guest_hash_unguarded(base, ranges, count, out); })) return false;
    return ok;
}

int module_cache_state(std::uint64_t base, std::wstring* stem, cachefmt::Key* key, bool* has_cold,
                       const std::uint8_t** main_data) {
    if (!g_enabled.load(std::memory_order_relaxed)) return -1;
    Global& g = G();
    int state = -1;
    with_module(g, base, [&](const CacheModule& m, const ModuleFiles& f) {
        if (m.base != base || !f.identity) return false;
        state = f.has_main ? 1 : 0;
        if (stem) *stem = f.stem;
        if (key) *key = f.key;
        if (has_cold) *has_cold = f.cold.load(std::memory_order_acquire) != nullptr;
        if (main_data) *main_data = f.has_main ? f.main_file.data() : nullptr;
        return true;
    });
    return state;
}

void testing_reset(bool flush) {
    external_aot_testing_reset();
    Global& g = configured();
    while (g.tasks_in_flight.load() != 0) host::sleep_ms(1);
    if (flush) drain_all(g, true, kForever);
    std::vector<CacheModule*> removed;
    {
        Exclusive lock(g.registry_lock);
        removed.swap(g.modules);
        publish_locked(g);
    }
    for (CacheModule* m : removed) delete m;
    {
        // A crash: the OS closes journal handles; nothing pending survives.
        Exclusive write(g.write_lock);
        Exclusive lock(g.writers_lock);
        for (auto& [stem, w] : g.writers) {
            if (w->journal) file::close(w->journal);
            delete w;
        }
        g.writers.clear();
        g.pending_bytes.store(0);
        ++g.session;
    }
    {
        Exclusive lock(g.listing_lock);
        g.listing_valid = false;
    }
    for (auto* counter : {&g.modules_loaded, &g.modules_with_cache, &g.files_rejected, &g.lookups, &g.hits,
                          &g.records_rejected, &g.guest_mismatches, &g.stores, &g.stores_ignored, &g.stores_unrelocatable, &g.pinned_mismatches, &g.records_queued,
                          &g.journal_bytes, &g.merges, &g.cold_files, &g.cold_hits, &g.cold_promoted})
        counter->store(0);
}

} // namespace cache_detail

// ===========================================================================
// public API

void cache_configure(const CacheConfig& config) {
    Global& g = G();
    Exclusive lock(g.config_lock);
    apply_config(g, config);
}

std::uint64_t compute_image_hash(const void* image_or_file, std::size_t size) noexcept {
    return pe::image_identity(static_cast<const std::uint8_t*>(image_or_file), size);
}

std::uint64_t cache_cpu_feature_mask() noexcept {
    const CpuFeatures& f = cpu_features();
    const bool bits[] = {f.sse3,    f.ssse3,    f.sse41,    f.sse42,    f.popcnt, f.aes,    f.pclmul, f.avx,
                         f.avx2,    f.fma,      f.f16c,     f.bmi1,     f.bmi2,   f.lzcnt,  f.movbe,  f.sha,
                         f.avx512f, f.avx512bw, f.avx512vl, f.avx512dq, f.apx,    f.os_avx,
                         f.rdrand,  f.rdseed,   f.invariant_tsc,
                         f.gfni};   // translator core: RBIT via GF2P8AFFINEQB
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < std::size(bits); ++i)
        if (bits[i]) mask |= std::uint64_t{1} << i;
    return mask | (std::uint64_t{sizeof(CpuFeatures)} << 56);
}

std::uint64_t cache_options_hash(const TranslateOptions& options) noexcept {
    // sizeof() catches fields appended to TranslateOptions until they are hashed here.
    std::uint64_t h = cachefmt::mix64(0x6f70'7473ULL, sizeof(TranslateOptions));
    h = cachefmt::mix64(h, options.max_instructions);
    h = cachefmt::mix64(h, options.interrupt_checks ? 1 : 0);
    h = cachefmt::mix64(h, options.profile ? 1 : 0);
    h = cachefmt::mix64(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(options.tier)));
    h = cachefmt::mix64(h, options.abi_flags ? 1 : 0);   // translator core: NZCV dead at BL/RET
    h = cachefmt::mix64(h, static_cast<std::uint64_t>(options.optimize + 1));   // translator core: tier-2 policy
    return h;
}

std::wstring cache_file_path(const GuestModule& module) {
    Global& g = configured();
    std::uint64_t image_hash = 0, file_size = 0;
    if (!module_identity(module, image_hash, file_size)) return {};
    std::wstring directory;
    TranslateOptions options;
    {
        Shared lock(g.config_lock);
        directory = g.directory;
        options = g.config.translate_options;
    }
    return cache_detail::stem_path(directory, module_name(module),
                                   cache_detail::make_key(image_hash, file_size, options)) +
           L".fxc";
}

namespace {

void register_module(Global& g, const GuestModule& module, bool published) {
    auto task = std::make_shared<OpenTask>();
    task->published.store(published);
    task->module = module;
    task->sequence = g_load_sequence.fetch_add(1, std::memory_order_relaxed);
    {
        Shared lock(g.config_lock);
        task->directory = g.directory;
        task->options = g.config.translate_options;
    }
    auto m = std::make_unique<CacheModule>();
    m->base = module.base;
    m->size = module.size;
    m->task = task;
    g.modules_loaded.fetch_add(1);

    std::vector<CacheModule*> removed;
    {
        Exclusive lock(g.registry_lock);
        // A module reported twice (or a stale one at the same range) is replaced.
        auto it = std::remove_if(g.modules.begin(), g.modules.end(), [&](CacheModule* other) {
            const bool overlaps = other->base < m->base + m->size && m->base < other->base + other->size;
            if (overlaps) removed.push_back(other);
            return overlaps;
        });
        g.modules.erase(it, g.modules.end());
        g.modules.push_back(m.release());
        publish_locked(g);
    }
    for (CacheModule* old : removed) delete old;
    submit(g, task);
}

} // namespace

void cache_module_mapped(const GuestModule& module) {
    Global& g = configured();
    if (!g_enabled.load(std::memory_order_relaxed) || module.base == 0 || module.size == 0) return;
    register_module(g, module, false);
}

void cache_module_loaded(const GuestModule& module) {
    Global& g = configured();
    if (!g_enabled.load(std::memory_order_relaxed) || module.base == 0 || module.size == 0) return;
    // Opened early (cache_module_mapped)? Then only publish it.
    std::shared_ptr<OpenTask> early;
    {
        Shared lock(g.registry_lock);
        for (CacheModule* m : g.modules) {
            if (m->base != module.base || m->size != module.size || !m->task) continue;
            if (!m->task->published.load() && m->task->module.path == module.path) early = m->task;
            break;
        }
    }
    if (early) {
        early->published.store(true);
        if (early->state.load() == 2 && early->has_main.load()) fire_ready(*early);
        return;
    }
    register_module(g, module, true);
}

void cache_module_unloaded(const GuestModule& module) {
    Global& g = G();
    std::vector<CacheModule*> removed;
    {
        Exclusive lock(g.registry_lock);
        auto it = std::remove_if(g.modules.begin(), g.modules.end(), [&](CacheModule* m) {
            if (m->base != module.base) return false;
            removed.push_back(m);
            return true;
        });
        if (removed.empty()) return;
        g.modules.erase(it, g.modules.end());
        publish_locked(g);  // waits for readers still using the removed modules
    }
    cache_detail::external_aot_module_unloaded(module.base);
    // Pending regions stay with the module key's writer and reach the journal.
    for (CacheModule* m : removed) delete m;
}

bool cache_lookup(std::uint64_t guest_pc, TranslatedRegion& region) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    Global& g = G();
    g.lookups.fetch_add(1, std::memory_order_relaxed);
    return with_module(g, guest_pc, [&](const CacheModule& m, const ModuleFiles& f) {
        return lookup_files(g, m, f, guest_pc, region);
    });
}

void cache_store(const TranslatedRegion& region) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    Global& g = G();
    g.stores.fetch_add(1, std::memory_order_relaxed);
    if (cache_detail::external_aot_active()) cache_detail::external_aot_note_region(region);
    const bool queued = with_module(g, region.guest_start, [&](const CacheModule& m, const ModuleFiles& f) {
        return store_in(g, m, f, region);
    });
    if (!queued) g.stores_ignored.fetch_add(1, std::memory_order_relaxed);
}

void cache_flush() {
    Global& g = G();
    std::wstring directory;
    bool force = false;
    {
        Shared lock(g.config_lock);
        if (!g.configured || !g_enabled.load()) return;
        directory = g.directory;
        force = g.config.merge_always;
    }
    // Bounded wait: at process exit another thread may have died holding the lock.
    if (!drain_all(g, true, 2000)) return;
    // Journals per stem (count, payload bytes, this process's), from one directory listing.
    struct Pending {
        std::size_t journals = 0;
        std::size_t mine = 0;
        std::uint64_t bytes = 0;
    };
    const std::wstring own_tag = L".j" + hex(host::process_id(), 8) + L"-";
    std::unordered_map<std::wstring, Pending> stems;
    for (const file::Entry& entry : file::list(directory + L"\\*.fxj", /*large=*/true)) {
        const std::wstring name = lower(entry.name);
        const std::wstring base = name.substr(0, name.size() - 4);
        const std::size_t dot = base.rfind(L'.');
        if (dot == std::wstring::npos) continue;
        Pending& p = stems[directory + L"\\" + base.substr(0, dot)];
        ++p.journals;
        if (name.find(own_tag) != std::wstring::npos) ++p.mine;
        const std::uint64_t size = entry.size;
        p.bytes += size > sizeof(cachefmt::FileHeader) ? size - sizeof(cachefmt::FileHeader) : 0;
    }
    for (const auto& [stem, pending] : stems) {
        // merge_stem keeps journals below its threshold; checking that here
        // from the listing (one attribute query instead of locking, opening
        // and mapping every journal and the main file) keeps a warm exit
        // cheap while small journals wait for more records. Journals that
        // earlier processes left (e.g. regions first used by guest
        // DLL_PROCESS_DETACH, see cache_flush_journals) are folded into a
        // small main file once instead, so the preloader installs them.
        bool settled = false;
        if (!force && pending.journals < 8) {
            std::uint64_t main_size = 0;
            if (file::attributes(stem + L".fxc", &main_size, nullptr)) {
                if (pending.bytes < std::max<std::uint64_t>(cache_detail::merge_min_bytes(), main_size / 8)) {
                    settled = pending.journals > pending.mine && main_size <= cache_detail::kSettledRewriteBytes;
                    if (!settled) continue;
                }
            }
        }
        if (cache_detail::merge_stem(stem, force, settled)) g.merges.fetch_add(1);
    }
}

void cache_flush_journals() noexcept {
    Global& g = G();
    if (!g_enabled.load() || g.pending_bytes.load() == 0) return;
    try {
        drain_all(g, true, 500);
    } catch (...) {
        // Out of memory while swapping buffers: the records are lost, which only costs a JIT later.
    }
}

bool cache_exit_quiescent() noexcept {
    Global& g = G();
    if (g.readers[0].load() != 0 || g.readers[1].load() != 0 || g.tasks_running.load() != 0) return false;
    if (!cache_detail::background_aot_quiescent() || !cache_detail::external_aot_quiescent()) return false;
    for (host::RwLock* lock : {&g.config_lock, &g.registry_lock, &g.writers_lock, &g.write_lock, &g.wake_lock,
                               &g.listing_lock}) {
        if (!lock->try_lock()) return false;
        lock->unlock();
    }
    if (!g.writers_lock.try_lock_shared()) return false;
    bool free = true;
    for (const auto& [stem, writer] : g.writers) {
        if (!writer->lock.try_lock()) {
            free = false;
            break;
        }
        writer->lock.unlock();
    }
    g.writers_lock.unlock_shared();
    return free;
}

void cache_load_cold_files() {
    if (!g_enabled.load(std::memory_order_relaxed) || cache_detail::background_aot_stopping()) return;
    Global& g = G();
    struct Wanted {
        std::uint64_t base;
        std::wstring stem;
        Key key;
    };
    std::vector<Wanted> wanted;
    {
        ReadGuard guard(g);
        if (const ModuleTable* table = g.table.load(std::memory_order_acquire)) {
            for (CacheModule* m : table->modules) {
                const ModuleFiles* f = ready_files(*m);
                if (f && f->identity && !f->cold.load(std::memory_order_acquire)) wanted.push_back({m->base, f->stem, f->key});
            }
        }
    }
    // Opened outside the epoch (file I/O), installed under it if the module is still there.
    for (const Wanted& w : wanted) {
        ColdFile* cold = open_cold(g, w.stem, w.key);
        if (!cold) continue;
        {
            ReadGuard guard(g);
            CacheModule* m = find_module(g.table.load(std::memory_order_acquire), w.base);
            const ModuleFiles* f = m && m->base == w.base ? ready_files(*m) : nullptr;
            const ColdFile* expected = nullptr;
            if (f && f->stem == w.stem && f->cold.compare_exchange_strong(expected, cold)) cold = nullptr;
        }
        if (cold) {
            g.cold_files.fetch_sub(1);
            delete cold;
        }
    }
}

void cache_set_module_ready_handler(CacheModuleReadyHandler handler) {
    g_ready_handler.store(handler, std::memory_order_release);
}

std::size_t cache_region_starts(std::uint64_t base, std::uint64_t& cursor, std::uint64_t end_cursor,
                                std::uint64_t* out, std::size_t max) {
    if (!g_enabled.load(std::memory_order_relaxed) || max == 0) return 0;
    Global& g = G();
    ReadGuard guard(g);
    CacheModule* m = find_module(g.table.load(std::memory_order_acquire), base);
    if (!m || m->base != base) return 0;
    const ModuleFiles* f = ready_files(*m);
    if (!f || !f->has_main) return 0;
    const MainView& view = f->main;
    if (cursor >= view.records_size || cursor >= end_cursor) return 0;
    const std::uint64_t end = view.records_offset + std::min(view.records_size, end_cursor);
    std::uint64_t offset = view.records_offset + cursor;
    std::size_t n = 0;
    while (n < max && offset < end && view.records_offset + view.records_size - offset >= sizeof(cachefmt::RecordHeader)) {
        RecordView record;
        // Structure only: every returned pc is fully validated by cache_lookup.
        const std::uint64_t avail = view.records_offset + view.records_size - offset;
        if (!cachefmt::decode_record(view.data + offset, static_cast<std::size_t>(avail), false, record)) {
            offset = view.records_offset + view.records_size;
            break;
        }
        if (cachefmt::usable_at(*record.header, m->base)) out[n++] = m->base + record.header->start_rva;
        offset += record.header->size;
    }
    cursor = offset - view.records_offset;
    return n;
}

std::uint64_t cache_region_cursor(std::uint64_t pc) {
    if (!g_enabled.load(std::memory_order_relaxed)) return kCacheCursorEnd;
    Global& g = G();
    ReadGuard guard(g);
    CacheModule* m = find_module(g.table.load(std::memory_order_acquire), pc);
    if (!m) return kCacheCursorEnd;
    const ModuleFiles* f = ready_files(*m);
    if (!f || !f->has_main || pc - m->base > 0xFFFF'FFFFull) return kCacheCursorEnd;
    std::uint64_t offset = 0;
    if (cachefmt::main_find(f->main, static_cast<std::uint32_t>(pc - m->base), &offset, 1) == 0) return kCacheCursorEnd;
    return offset - f->main.records_offset;
}

CacheStats cache_stats() noexcept {
    Global& g = G();
    CacheStats s{};
    s.modules_loaded = g.modules_loaded.load();
    s.modules_with_cache = g.modules_with_cache.load();
    s.files_rejected = g.files_rejected.load();
    s.lookups = g.lookups.load();
    s.hits = g.hits.load();
    s.records_rejected = g.records_rejected.load();
    s.guest_mismatches = g.guest_mismatches.load();
    s.stores = g.stores.load();
    s.stores_ignored = g.stores_ignored.load();
    s.stores_unrelocatable = g.stores_unrelocatable.load();
    s.pinned_mismatches = g.pinned_mismatches.load();
    s.records_queued = g.records_queued.load();
    s.journal_bytes = g.journal_bytes.load();
    s.merges = g.merges.load();
    s.cold_files = g.cold_files.load();
    s.cold_hits = g.cold_hits.load();
    s.cold_promoted = g.cold_promoted.load();
    s.aot_requests = cache_detail::background_aot_requests();
    s.aot_helpers = cache_detail::background_aot_helpers();
    return s;
}

} // namespace transmute::fx
