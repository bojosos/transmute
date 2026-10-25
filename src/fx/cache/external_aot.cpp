// Background translation of external (non-PE) guest modules (owner: cache).
//
// A personality that maps code itself (the macOS personality's Mach-O
// images, register_external_module) hands fx a module's function starts and
// a few roots it will run first (initializers, the entry point). Worker
// threads translate regions from the module's mapped executable memory into
// an in-memory pool, off the guest's critical path:
//
//  * flow-ahead (immediately): every region the guest starts using in the
//    module (a JIT translation stored by cache_store, or a pool hit) queues
//    its direct branch/call targets and the next function after it, newest
//    first, and worker regions queue theirs up to a depth limit. Measured on
//    macOS Blender's cold start, direct targets of earlier regions start 43%
//    of the main image's regions (70% of their instructions) and the function
//    right after an earlier region another 15%.
//  * static (later, idle priority): every function start of modules up to a
//    text size limit, after the process has run as long as PE background AOT
//    waits (TRANSMUTE_FX_BACKGROUND_AOT_DELAY). A module done completely is
//    written as its cold AOT file (<stem>.fxa), which later processes look up
//    after the main file and journals, and the pool is released.
//
// cache_lookup consults the pool after the main file and journals (optionally
// waiting when a worker is translating that very pc). A hit is installed like
// any cached region and appended to this process's journal, so the regions
// the guest used reach the main file in first-use order and warm starts
// preload them. Unused speculation is dropped at exit.
//
// Every pool record is translated from live guest memory, hashed, and
// rejected if any word the translator read changed meanwhile; lookups verify
// the guest hash again (CacheConfig::verify_guest_code), and the runtime
// installs hits with the same epoch/write-protection checks as other cached
// regions.
//
// Knobs: TRANSMUTE_FX_EXTERNAL_AOT=0|<threads> (default hardware threads / 4,
// 1..3), TRANSMUTE_FX_EXTERNAL_AOT_DEPTH=<n> (flow-ahead levels beyond the
// guest's own regions, default 1), TRANSMUTE_FX_EXTERNAL_AOT_STATIC=<MiB>
// (largest text translated whole, default 2, 0 = flow-ahead only),
// TRANSMUTE_FX_EXTERNAL_AOT_WAIT=1 (a lookup waits for an in-flight
// translation of its pc instead of missing), TRANSMUTE_FX_EXTERNAL_AOT_BOUNDS=1
// (worker regions end at the next function start instead of the JIT's
// synthetic window). The defaults were the cheapest on macOS Blender's cold
// start: waiting and function-bounded (larger) regions cost the guest more in
// lookups and installs than they saved in JIT time.

#include "fx/cache/cache_internal.hpp"
#include "fx/cache/format.hpp"
#include "fx/host/env.hpp"
#include "fx/host/guard.hpp"
#include "fx/host/sync.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/vm.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transmute::fx::cache_detail {
namespace {

using Exclusive = host::ExclusiveLock;
using Shared = host::SharedLock;

constexpr std::size_t kMaxQueue = 1u << 16;
constexpr unsigned kNextFunctions = 2;      // functions after a used region queued by flow-ahead
constexpr std::size_t kStaticChunk = 64;    // function starts per static refill

unsigned env_unsigned(const wchar_t* name, unsigned fallback) {
    wchar_t value[32] = {};
    const auto n = host::get_env(name, value, 32);
    if (n == 0 || n >= 32) return fallback;
    return static_cast<unsigned>(std::wcstoul(value, nullptr, 10));
}

std::uint64_t ticks_now() noexcept { return host::ticks(); }

double tick_seconds() {
    static const double s = 1.0 / static_cast<double>(host::tick_frequency());
    return s;
}

struct PoolRecord {
    std::vector<std::uint8_t> bytes;
};

struct XModule : std::enable_shared_from_this<XModule> {
    std::uint64_t base{}, size{}, text_begin{}, text_end{};
    std::vector<std::uint32_t> starts;   // function start rvas, sorted, unique
    // State shared with workers and lookups (pool_lock).
    host::RwLock pool_lock;
    host::CondVar pool_done;
    std::unordered_map<std::uint32_t, std::shared_ptr<const PoolRecord>> index;   // entry rva -> record
    std::vector<std::shared_ptr<const PoolRecord>> records;
    std::unordered_set<std::uint32_t> in_flight;   // region starts being translated
    std::unique_ptr<std::atomic<std::uint64_t>[]> claimed;   // bit per text word: translated or queued for good
    std::size_t claimed_words{};
    std::atomic<bool> dead{false};
    // 0 unchecked, 1 active, 2 disabled (the module already has a cache);
    // written under the queue lock, read anywhere.
    std::atomic<int> state{0};
    // Worker-only state (queue lock).
    bool static_wanted = false, static_done = false, has_cold = false;
    std::size_t static_cursor = 0;
    std::wstring stem;
    cachefmt::Key key{};

    bool test(std::uint32_t rva) const {
        const std::uint64_t slot = (rva - static_cast<std::uint32_t>(text_begin - base)) / 4;
        return slot / 64 < claimed_words && ((claimed[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1);
    }
    // True if the bit was clear (this caller claimed it).
    bool claim(std::uint32_t rva) {
        const std::uint64_t slot = (rva - static_cast<std::uint32_t>(text_begin - base)) / 4;
        if (slot / 64 >= claimed_words) return false;
        const std::uint64_t mask = std::uint64_t{1} << (slot % 64);
        return (claimed[slot / 64].fetch_or(mask, std::memory_order_relaxed) & mask) == 0;
    }
    bool in_text(std::uint64_t pc) const { return pc >= text_begin && pc < text_end && (pc & 3) == 0; }
};

struct Job {
    std::shared_ptr<XModule> module;
    std::uint32_t rva;
    std::uint8_t depth;
};

constexpr unsigned kMaxModules = 4096;

struct State {
    host::RwLock lock;   // queue, modules, threads
    host::CondVar wake;
    std::deque<Job> queue;          // front = next (flow-ahead pushes to the front)
    std::vector<std::shared_ptr<XModule>> modules;   // never shrinks (lookups hold raw pointers)
    // Lock-free view of `modules` for the guest's lookup and note paths.
    std::atomic<XModule*> table[kMaxModules]{};
    std::atomic<unsigned> table_count{0};
    // Regions the guest started using: (module, end rva, target count,
    // targets...) appended by guest threads under this lock only; workers move
    // them to the queue. Keeps guest threads off the queue lock.
    host::RwLock inbox_lock;
    std::vector<std::uint64_t> inbox;
    std::atomic<bool> inbox_pending{false};
    unsigned threads = 0, busy = 0, max_threads = 0;
    unsigned depth = 1;
    bool wait_in_flight = false;  // lookups wait for a running translation of their pc
    bool bounds = false;          // regions end at the next function start
    std::uint64_t static_limit = 2u << 20;
    std::uint64_t static_after_ms = 20000;
    bool configured = false;
    ExternalAotTranslateFn translate = nullptr;
    std::atomic<bool> active{false};    // any module registered
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> regions{}, instructions{}, ticks{}, discarded{}, hits{}, waits{}, wait_ticks{},
        flow_jobs{}, static_regions{}, cold_files{}, lookups{}, lookup_ticks{};
};

State& S() {
    static State* const s = new State();   // never destroyed: usable during process exit
    return *s;
}

std::uint64_t process_age_ms() {
    host::ProcessTimes times;
    if (!host::process_times(times)) return 0;
    const auto t0 = times.created;
    const auto t1 = host::wall_time();
    return t1 > t0 ? (t1 - t0) / 10000 : 0;
}

// lock held.
void configure_locked(State& s) {
    if (s.configured) return;
    s.configured = true;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    s.max_threads = std::min(env_unsigned(L"TRANSMUTE_FX_EXTERNAL_AOT", std::clamp(hardware / 4, 1u, 3u)), 8u);
    s.depth = std::min(env_unsigned(L"TRANSMUTE_FX_EXTERNAL_AOT_DEPTH", 1), 8u);
    s.static_limit = std::uint64_t{env_unsigned(L"TRANSMUTE_FX_EXTERNAL_AOT_STATIC", 2)} << 20;
    s.static_after_ms = std::uint64_t{env_unsigned(L"TRANSMUTE_FX_BACKGROUND_AOT_DELAY", 20)} * 1000;
    s.wait_in_flight = env_unsigned(L"TRANSMUTE_FX_EXTERNAL_AOT_WAIT", 0) != 0;
    s.bounds = env_unsigned(L"TRANSMUTE_FX_EXTERNAL_AOT_BOUNDS", 0) != 0;
}

// Guarded 4-byte read: access violations and in-page errors fail.
bool read_word(std::uint64_t address, std::uint32_t& word) noexcept {
    return host::call_guarded_access([&] { std::memcpy(&word, reinterpret_cast<const void*>(address), 4); });
}

// What the JIT sees, restricted to the module's text, plus real function
// bounds from the function starts and the pool's entries as region stops.
class ModuleSource final : public CodeSource {
public:
    ModuleSource(const XModule& m, bool bounds) : m_(m), bounds_(bounds) {}
    bool read(std::uint64_t address, std::uint32_t& word) const override {
        if (!m_.in_text(address) || !read_word(address, word)) return false;
        reads_.emplace_back(address, word);
        return true;
    }
    // The module's code, or its read-only data (__DATA_CONST after fixups:
    // jump and label tables), as the JIT's LiveCodeSource::read_data.
    bool read_data(std::uint64_t address, std::uint32_t& word) const override {
        if (read(address, word)) return true;
        if ((address & 3) != 0 || address < m_.base || address - m_.base >= m_.size) return false;
        const auto page = address & ~std::uint64_t{0xfff};
        if (page != data_page_) {
            namespace vm = host::vm;
            data_page_ = page;
            vm::RegionInfo info;
            data_page_ok_ = vm::query(reinterpret_cast<const void*>(page), info) && info.committed() &&
                            (info.protect & vm::kGuard) == 0 &&
                            ((info.protect & 0xff) == vm::kReadOnly || (info.protect & 0xff) == vm::kExecuteRead);
        }
        if (!data_page_ok_ || !read_word(address, word)) return false;
        reads_.emplace_back(address, word);
        return true;
    }
    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override {
        if (!bounds_ || m_.starts.empty() || address < m_.base) return false;
        const auto rva = static_cast<std::uint32_t>(address - m_.base);
        auto it = std::upper_bound(m_.starts.begin(), m_.starts.end(), rva);
        if (it == m_.starts.begin()) return false;
        begin = m_.base + *(it - 1);
        end = it == m_.starts.end() ? m_.text_end : m_.base + *it;
        return address < end;
    }
    bool has_entry(std::uint64_t pc) const override {
        return m_.in_text(pc) && m_.test(static_cast<std::uint32_t>(pc - m_.base));
    }
    bool image_code(std::uint64_t address) const override { return m_.in_text(address); }
    // Every word read still holds the value the translator saw.
    bool unchanged() const {
        for (const auto& [address, word] : reads_) {
            std::uint32_t now = 0;
            if (!read_word(address, now) || now != word) return false;
        }
        return true;
    }

private:
    const XModule& m_;
    bool bounds_;
    mutable std::uint64_t data_page_{~std::uint64_t{0}};
    mutable bool data_page_ok_{};
    mutable std::vector<std::pair<std::uint64_t, std::uint32_t>> reads_;
};

// lock held. Queues `rva` of `m` (front: flow-ahead, back: roots).
void push_locked(State& s, const std::shared_ptr<XModule>& m, std::uint64_t pc, unsigned depth, bool front) {
    if (!m->in_text(pc)) return;
    const auto rva = static_cast<std::uint32_t>(pc - m->base);
    if (m->test(rva)) return;
    if (front) s.queue.push_front({m, rva, static_cast<std::uint8_t>(depth)});
    else s.queue.push_back({m, rva, static_cast<std::uint8_t>(depth)});
    if (s.queue.size() > kMaxQueue) s.queue.pop_back();
}

// Lock-free: the live module containing `pc`.
XModule* find_module(State& s, std::uint64_t pc) noexcept {
    const unsigned n = std::min(s.table_count.load(std::memory_order_acquire), kMaxModules);
    for (unsigned i = 0; i < n; ++i) {
        XModule* m = s.table[i].load(std::memory_order_acquire);
        if (m && pc - m->base < m->size && !m->dead.load(std::memory_order_relaxed)) return m;
    }
    return nullptr;
}

// First function start at or after `rva` (the function laid out next).
template <typename Fn>
void next_functions(const XModule& m, std::uint32_t end_rva, Fn&& fn) {
    auto it = std::lower_bound(m.starts.begin(), m.starts.end(), end_rva);
    for (unsigned k = 0; k < kNextFunctions && it != m.starts.end(); ++k, ++it) fn(m.base + *it);
}

// lock held: successors of a region of `m` (absolute targets, region ends).
void push_successors_locked(State& s, const std::shared_ptr<XModule>& m, const std::uint64_t* targets,
                            std::size_t n_targets, std::uint32_t end_rva, unsigned depth) {
    if (depth > s.depth) return;
    // Each push goes to the front: the next functions first, then the direct
    // targets (likelier), last to first, so the first target is taken first.
    std::uint64_t next[kNextFunctions];
    unsigned n_next = 0;
    if (end_rva) next_functions(*m, end_rva, [&](std::uint64_t pc) { next[n_next++] = pc; });
    while (n_next > 0) push_locked(s, m, next[--n_next], depth, true);
    for (std::size_t i = n_targets; i-- > 0;) push_locked(s, m, targets[i], depth, true);
}

void start_workers_locked(State& s);

// Guest side: a region of `m` the guest uses now.
void note(State& s, XModule& m, std::uint32_t end_rva, const std::uint64_t* targets, std::size_t n) {
    {
        Exclusive lock(s.inbox_lock);
        s.inbox.push_back(reinterpret_cast<std::uint64_t>(&m));
        s.inbox.push_back(end_rva);
        s.inbox.push_back(n);
        s.inbox.insert(s.inbox.end(), targets, targets + n);
        s.inbox_pending.store(true, std::memory_order_release);
    }
    s.wake.notify_one();
}

// lock held: moves the inbox to the queue (depth 1: the guest is there now).
void drain_inbox_locked(State& s) {
    if (!s.inbox_pending.load(std::memory_order_acquire)) return;
    thread_local std::vector<std::uint64_t> notes;
    notes.clear();
    {
        Exclusive lock(s.inbox_lock);
        notes.swap(s.inbox);
        s.inbox_pending.store(false, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i + 3 <= notes.size();) {
        auto* m = reinterpret_cast<XModule*>(notes[i]);
        const auto end_rva = static_cast<std::uint32_t>(notes[i + 1]);
        const auto n = static_cast<std::size_t>(notes[i + 2]);
        if (!m->dead.load()) push_successors_locked(s, m->shared_from_this(), notes.data() + i + 3, n, end_rva, 1);
        i += 3 + n;
    }
}

// Worker: checks once per module whether it still needs translating (a main
// file means a warm start: nothing to do; a cold file means static AOT ran).
bool module_active(State& s, const std::shared_ptr<XModule>& m) {
    {
        Shared lock(s.lock);
        if (m->state != 0) return m->state == 1;
    }
    std::wstring stem;
    cachefmt::Key key{};
    bool has_cold = false;
    const int state = module_cache_state(m->base, &stem, &key, &has_cold);   // may wait for the open task
    Exclusive lock(s.lock);
    if (m->state == 0) {
        m->state = state == 0 ? 1 : 2;
        m->stem = std::move(stem);
        m->key = key;
        m->has_cold = has_cold;
        if (has_cold || m->state != 1) m->static_wanted = false;   // done by an earlier process
    }
    return m->state == 1;
}

void translate_one(State& s, const Job& job) {
    XModule& m = *job.module;
    if (m.dead.load(std::memory_order_relaxed) || !m.claim(job.rva)) return;
    {
        Exclusive lock(m.pool_lock);
        m.in_flight.insert(job.rva);
    }
    struct Done {
        XModule& m;
        std::uint32_t rva;
        ~Done() {
            {
                Exclusive lock(m.pool_lock);
                m.in_flight.erase(rva);
            }
            m.pool_done.notify_all();
        }
    } done{m, job.rva};
    const std::uint64_t start = ticks_now();
    ModuleSource source(m, s.bounds);
    TranslatedRegion region;
    TranslateOptions options = configured_options();
    options.lazy = false;   // regions the cache keeps are final (tier 1), as AOT output
    try {
        region = s.translate(m.base + job.rva, source, options);
    } catch (...) {
        s.discarded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Guest ranges inside the module, hashed from live memory; the source must
    // still read what the translator saw (else the guest changed it meanwhile).
    std::vector<cachefmt::DiskRange> ranges;
    std::uint32_t end_rva = 0;
    for (const auto& [b, e] : region.guest_ranges) {
        if (b < m.base || e < b || e - m.base > m.size) {
            s.discarded.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ranges.push_back({static_cast<std::uint32_t>(b - m.base), static_cast<std::uint32_t>(e - m.base)});
        end_rva = std::max(end_rva, static_cast<std::uint32_t>(e - m.base));
    }
    std::uint64_t hash = 0;
    auto record = std::make_shared<PoolRecord>();
    const bool pinned = cachefmt::unrecorded_address(region, m.base, m.size) >= 0;
    if (!guest_memory_hash(m.base, ranges.data(), static_cast<std::uint32_t>(ranges.size()), hash) ||
        !source.unchanged() || !cachefmt::encode_record(region, m.base, m.size, hash, record->bytes, pinned)) {
        s.discarded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        Exclusive lock(m.pool_lock);
        for (const RegionEntry& e : region.entries) {
            if (!m.in_text(e.guest_pc)) continue;
            const auto rva = static_cast<std::uint32_t>(e.guest_pc - m.base);
            m.claim(rva);
            m.index.emplace(rva, record);
        }
        m.records.push_back(record);
    }
    s.regions.fetch_add(1, std::memory_order_relaxed);
    s.instructions.fetch_add(region.stats.guest_instructions, std::memory_order_relaxed);
    s.ticks.fetch_add(ticks_now() - start, std::memory_order_relaxed);
    if (job.depth > s.depth) {
        s.static_regions.fetch_add(1, std::memory_order_relaxed);
        return;   // static root: no flow-ahead
    }
    std::vector<std::uint64_t> targets;
    targets.reserve(region.exits.size());
    for (const RegionExit& x : region.exits) targets.push_back(x.target_pc);
    Exclusive lock(s.lock);
    push_successors_locked(s, job.module, targets.data(), targets.size(), end_rva, job.depth + 1u);
}

// Static phase of `m` done: every function start translated. Writes the
// cold file (all pool records), maps it and releases the pool.
void finish_static(State& s, XModule& m) {
    std::vector<std::shared_ptr<const PoolRecord>> keep;
    {
        Shared lock(m.pool_lock);
        keep = m.records;
    }
    std::vector<cachefmt::RecordRef> refs;
    refs.reserve(keep.size());
    for (const auto& r : keep) refs.push_back({r->bytes.data(), static_cast<std::uint32_t>(r->bytes.size())});
    if (refs.empty() || m.stem.empty()) return;
    if (!write_main(m.stem, m.key, refs, true, nullptr, nullptr, L".fxa")) return;
    cache_load_cold_files();
    // Lookups now find these regions in the cold file.
    {
        Exclusive lock(m.pool_lock);
        m.index.clear();
        m.records.clear();
    }
    // Counted once the cold file serves lookups (a test waiting for it then
    // looked up a region from the pool, intermittently: no cold hit).
    s.cold_files.fetch_add(1, std::memory_order_relaxed);
}

// lock held: a module whose static phase is due and not finished, or null.
std::shared_ptr<XModule> static_candidate_locked(State& s, bool& later) {
    later = false;
    for (const auto& m : s.modules) {
        // state 0 (unchecked): its first static job checks it (module_active).
        if (m->dead.load() || m->state == 2 || !m->static_wanted || m->static_cursor >= m->starts.size()) continue;
        if (process_age_ms() >= s.static_after_ms) return m;
        later = true;
    }
    return nullptr;
}

// lock held, queue empty and no job running: a module whose every function
// start was handed out (and so translated) still waiting for its cold file.
std::shared_ptr<XModule> finished_static_locked(State& s) {
    for (const auto& m : s.modules)
        if (!m->dead.load() && m->static_wanted && !m->static_done && !m->starts.empty() &&
            m->static_cursor >= m->starts.size()) {
            m->static_done = true;   // written once
            return m;
        }
    return nullptr;
}

std::uint32_t worker_main(void*) {
    State& s = S();
    bool idle_priority = false;
    unsigned idle_wait = 10;   // ms
    for (;;) {
        Job job;
        {
            Exclusive lock(s.lock);
            for (;;) {
                if (s.stopping.load()) {
                    --s.threads;
                    s.wake.notify_all();
                    return 0;
                }
                drain_inbox_locked(s);
                if (!s.queue.empty()) {
                    job = std::move(s.queue.front());
                    s.queue.pop_front();
                    break;
                }
                // Idle: the static phase refills the queue, a chunk of function starts at a time.
                bool later = false;
                if (const auto pick = static_candidate_locked(s, later)) {
                    // Unchecked module: one job decides (module_active) before the rest is queued.
                    const std::size_t chunk = pick->state == 0 ? 1 : kStaticChunk;
                    const std::size_t end = std::min(pick->starts.size(), pick->static_cursor + chunk);
                    for (; pick->static_cursor < end; ++pick->static_cursor)
                        push_locked(s, pick, pick->base + pick->starts[pick->static_cursor], s.depth + 1, false);
                    continue;
                }
                // Guest notes wake a worker without the queue lock, so a wake-up can
                // fall between the inbox check and the sleep: sleep briefly after
                // recent work, longer (up to 0.5 s) the longer nothing arrives.
                const std::uint64_t age = process_age_ms();
                std::uint64_t wait = idle_wait;
                if (later) wait = std::clamp<std::uint64_t>(s.static_after_ms - std::min(age, s.static_after_ms), 1, wait);
                idle_wait = std::min(idle_wait * 2, 500u);
                s.wake.wait_for(s.lock, static_cast<unsigned long>(wait));
            }
            ++s.busy;
        }
        idle_wait = 10;
        const bool is_static = job.depth > s.depth;
        if (is_static != idle_priority) {
            idle_priority = is_static;
            // Flow-ahead races the guest: normal QoS, just below guest threads.
            // Static AOT only uses idle time.
            host::set_thread_priority(host::current_thread(),
                                      is_static ? host::ThreadPriority::idle : host::ThreadPriority::below_normal);
            if (is_static) set_background_thread_qos(host::current_thread());
        }
        if (module_active(s, job.module)) {
            bool wanted = !is_static;
            if (is_static) {
                Shared lock(s.lock);
                wanted = job.module->static_wanted;   // false once a cold file turned up
            }
            if (!is_static) s.flow_jobs.fetch_add(1, std::memory_order_relaxed);
            if (wanted) translate_one(s, job);
        }
        job.module.reset();
        std::shared_ptr<XModule> finished;
        {
            Exclusive lock(s.lock);
            --s.busy;
            if (s.queue.empty() && s.busy == 0) finished = finished_static_locked(s);
        }
        if (finished && !s.stopping.load()) {
            try {
                finish_static(s, *finished);
            } catch (...) {
                // Out of memory or an I/O failure: the module stays served by its pool.
            }
        }
        s.wake.notify_all();
    }
}

// lock held.
void start_workers_locked(State& s) {
    while (s.threads < s.max_threads && !s.stopping.load()) {
        host::ThreadHandle thread = host::create_thread(worker_main, nullptr, 512 * 1024, host::kThreadStackReservation, nullptr);
        if (!thread) break;
        ++s.threads;
        host::close_thread(thread);
    }
    s.wake.notify_all();
}

} // namespace

void external_aot_start(const ExternalAotRequest& request, ExternalAotTranslateFn translate) {
    State& s = S();
    if (request.base == 0 || request.size == 0 || translate == nullptr) return;
    auto m = std::make_shared<XModule>();
    m->base = request.base;
    m->size = request.size;
    m->text_begin = request.text_begin ? request.text_begin : request.base;
    m->text_end = request.text_end ? request.text_end : request.base + request.size;
    if (m->text_begin < m->base || m->text_end > m->base + m->size || m->text_end <= m->text_begin ||
        m->size > 0xFFFF'FFFFull)
        return;
    m->text_begin = (m->text_begin + 3) & ~std::uint64_t{3};
    for (const std::uint64_t pc : request.function_starts)
        if (m->in_text(pc)) m->starts.push_back(static_cast<std::uint32_t>(pc - m->base));
    std::sort(m->starts.begin(), m->starts.end());
    m->starts.erase(std::unique(m->starts.begin(), m->starts.end()), m->starts.end());
    m->claimed_words = static_cast<std::size_t>((m->text_end - m->text_begin) / 4 / 64 + 1);
    m->claimed.reset(new std::atomic<std::uint64_t>[m->claimed_words]);
    for (std::size_t i = 0; i < m->claimed_words; ++i) m->claimed[i].store(0, std::memory_order_relaxed);
    Exclusive lock(s.lock);
    configure_locked(s);
    if (s.max_threads == 0 || s.stopping.load()) return;
    s.translate = translate;
    // A module registered again at the same base replaces the old one (which
    // stays allocated: lookups may still hold it).
    for (auto& old : s.modules)
        if (old->base == m->base) old->dead.store(true);
    std::erase_if(s.queue, [](const Job& j) { return j.module->dead.load(); });
    const unsigned slot = s.table_count.load();
    if (slot >= kMaxModules) return;
    m->static_wanted = s.static_limit != 0 && m->text_end - m->text_begin <= s.static_limit && !m->starts.empty();
    s.modules.push_back(m);
    s.table[slot].store(m.get(), std::memory_order_release);
    s.table_count.store(slot + 1, std::memory_order_release);
    for (const std::uint64_t pc : request.roots) push_locked(s, m, pc, 0, false);   // as if the guest ran them
    s.active.store(true);
    start_workers_locked(s);
}

bool external_aot_active() noexcept { return S().active.load(std::memory_order_relaxed); }

bool external_aot_lookup(std::uint64_t base, std::uint32_t rva, ExternalAotTryFn try_record, void* context) {
    State& s = S();
    if (!s.active.load(std::memory_order_relaxed) || s.stopping.load(std::memory_order_relaxed)) return false;
    XModule* m = find_module(s, base);
    if (!m || m->base != base || m->state.load(std::memory_order_relaxed) == 2) return false;
    s.lookups.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t lookup_start = ticks_now();
    struct Timed {
        State& s;
        std::uint64_t start;
        ~Timed() { s.lookup_ticks.fetch_add(ticks_now() - start, std::memory_order_relaxed); }
    } timed{s, lookup_start};
    std::shared_ptr<const PoolRecord> record;
    {
        Shared lock(m->pool_lock);
        auto it = m->index.find(rva);
        if (it == m->index.end() && s.wait_in_flight && m->in_flight.count(rva)) {
            // A worker is translating this pc: waiting for it is cheaper than a JIT.
            s.waits.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t start = ticks_now();
            for (int i = 0; i < 20 && !m->index.count(rva) && m->in_flight.count(rva); ++i)
                m->pool_done.wait_shared_for(m->pool_lock, 1);
            s.wait_ticks.fetch_add(ticks_now() - start, std::memory_order_relaxed);
            it = m->index.find(rva);
        }
        if (it == m->index.end()) return false;
        record = it->second;
    }
    if (!try_record(context, record->bytes.data(), record->bytes.size())) return false;
    s.hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void external_aot_note_region(const TranslatedRegion& region) {
    State& s = S();
    if (!s.active.load(std::memory_order_relaxed) || s.stopping.load(std::memory_order_relaxed)) return;
    XModule* m = find_module(s, region.guest_start);
    if (!m || m->state.load(std::memory_order_relaxed) == 2) return;
    // The guest has these: never translate them again here.
    for (const RegionEntry& e : region.entries)
        if (m->in_text(e.guest_pc)) m->claim(static_cast<std::uint32_t>(e.guest_pc - m->base));
    std::uint32_t end_rva = 0;
    for (const auto& [b, e] : region.guest_ranges)
        if (e > m->base && e - m->base <= m->size) end_rva = std::max(end_rva, static_cast<std::uint32_t>(e - m->base));
    thread_local std::vector<std::uint64_t> targets;
    targets.clear();
    for (const RegionExit& x : region.exits) targets.push_back(x.target_pc);
    note(s, *m, end_rva, targets.data(), targets.size());
}

void external_aot_note_record(std::uint64_t base, const cachefmt::RecordView& record) {
    State& s = S();
    if (!s.active.load(std::memory_order_relaxed) || s.stopping.load(std::memory_order_relaxed)) return;
    XModule* m = find_module(s, base);
    if (!m || m->base != base || m->state.load(std::memory_order_relaxed) == 2) return;
    std::uint32_t end_rva = 0;
    for (std::uint32_t i = 0; i < record.header->n_ranges; ++i) end_rva = std::max(end_rva, record.ranges[i].end_rva);
    thread_local std::vector<std::uint64_t> targets;
    targets.clear();
    for (std::uint32_t i = 0; i < record.header->n_exits; ++i) targets.push_back(base + record.exits[i].target_rva);
    note(s, *m, end_rva, targets.data(), targets.size());
}

void external_aot_module_unloaded(std::uint64_t base) {
    State& s = S();
    if (!s.active.load(std::memory_order_relaxed)) return;
    Exclusive lock(s.lock);
    for (auto& m : s.modules)
        if (m->base == base) m->dead.store(true);
    std::erase_if(s.queue, [](const Job& j) { return j.module->dead.load(); });
}

bool external_aot_stop(unsigned wait_ms) noexcept {
    State& s = S();
    s.stopping.store(true);
    if (!s.active.load()) return true;
    if (wait_ms == 0) {
        if (!s.lock.try_lock()) return false;
    } else {
        s.lock.lock();
    }
    s.queue.clear();
    s.wake.notify_all();
    // A worker stops between two regions; translations take well under a millisecond.
    const std::uint64_t deadline = host::monotonic_ms() + wait_ms;
    while (s.busy != 0 && host::monotonic_ms() < deadline) s.wake.wait_for(s.lock, 5);
    const bool idle = s.busy == 0;
    s.lock.unlock();
    return idle;
}

bool external_aot_quiescent() noexcept {
    State& s = S();
    if (!s.active.load()) return true;
    if (!s.lock.try_lock()) return false;
    bool free = s.busy == 0;
    for (const auto& m : s.modules) {
        if (!free) break;
        if (!m->pool_lock.try_lock()) {
            free = false;
            break;
        }
        m->pool_lock.unlock();
    }
    s.lock.unlock();
    return free;
}

void external_aot_testing_reset() {
    State& s = S();
    external_aot_stop(5000);
    for (;;) {
        Exclusive lock(s.lock);
        if (s.threads == 0) break;
        s.wake.wait_for(s.lock, 5);
    }
    Exclusive lock(s.lock);
    for (auto& slot : s.table) slot.store(nullptr);
    s.table_count.store(0);
    {
        Exclusive inbox(s.inbox_lock);
        s.inbox.clear();
        s.inbox_pending.store(false);
    }
    s.modules.clear();
    s.queue.clear();
    s.configured = false;
    s.active.store(false);
    s.stopping.store(false);
    for (auto* c : {&s.regions, &s.instructions, &s.ticks, &s.discarded, &s.hits, &s.waits, &s.wait_ticks, &s.flow_jobs,
                    &s.static_regions, &s.cold_files, &s.lookups, &s.lookup_ticks})
        c->store(0);
}

void external_aot_testing_wait_idle(unsigned timeout_ms) {
    State& s = S();
    const std::uint64_t deadline = host::monotonic_ms() + timeout_ms;
    for (;;) {
        {
            Shared lock(s.lock);
            if (s.queue.empty() && s.busy == 0 && !s.inbox_pending.load()) return;
        }
        if (host::monotonic_ms() >= deadline) return;
        host::sleep_ms(1);
    }
}

} // namespace transmute::fx::cache_detail

namespace transmute::fx {

ExternalAotStats cache_external_aot_stats() noexcept {
    using namespace cache_detail;
    State& s = S();
    ExternalAotStats out{};
    out.regions = s.regions.load();
    out.guest_instructions = s.instructions.load();
    out.translate_seconds = static_cast<double>(s.ticks.load()) * tick_seconds();
    out.discarded = s.discarded.load();
    out.hits = s.hits.load();
    out.waits = s.waits.load();
    out.wait_seconds = static_cast<double>(s.wait_ticks.load()) * tick_seconds();
    out.flow_jobs = s.flow_jobs.load();
    out.static_regions = s.static_regions.load();
    out.cold_files = s.cold_files.load();
    out.lookups = s.lookups.load();
    out.lookup_seconds = static_cast<double>(s.lookup_ticks.load()) * tick_seconds();
    return out;
}

} // namespace transmute::fx
