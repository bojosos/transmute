// Translated regions: installation, the authoritative guest pc -> entry map,
// direct-branch linking, invalidation and write protection of translated
// guest pages (self-modifying code), and the reclaim of dead code chunks.
//
// Locks (cold paths only; the lookup-table hit path takes none):
//   g_translate  serializes translations (cache lookup, translate_region, install)
//   g_reclaim_pass  serializes reclaim passes (taken before g_map)
//   g_map        SRW lock over every structure below and the lookup-table writes
//   registry     (threads.cpp) taken inside g_map to scrub RSBs / kick threads
//   arena        (arena.cpp) taken inside g_map (never held while threads are suspended)
//   g_preload, g_reclaim.lock  queues (innermost; never held while taking another lock)
//
// Linking is eager in both directions: installing a region links its exits
// to existing entries and links the pending (unlinked) exits of live regions
// that target its entry points, so code that is installed ahead of use (the
// preloader) runs without dispatcher round trips.

#include "fx/cache/cache.hpp"
#include "fx/common/x64_assembler.hpp"
#include "fx/runtime/fp_boundary.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/runtime/shared_memory.hpp"
#include "fx/host/env.hpp"
#include "fx/host/vm.hpp"

#include <immintrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace transmute::fx::rt {
namespace {

constexpr std::uint64_t kPage = 0x1000;

struct MapEntry {
    const void* host;
    Region* region;          // nullptr: registered host entry
    bool flags_dead = false; // RegionEntry::flags_dead: exits may link past their flag store
};
struct ExitRef {
    Region* region;
    std::uint32_t index;
};

// Open-addressing hash map keyed by guest pcs or guest pages (linear probing,
// power-of-two capacity, Fibonacci hashing). Installing a region inserts a
// handful of keys under the exclusive map lock; a node-based map spends most
// of that lock hold in the allocator. kEmpty/kErased are never valid keys
// (pcs are 4-byte aligned, pages 4 KiB aligned). Readers may call find()
// concurrently with each other (shared map lock).
template <typename V>
class FlatMap {
public:
    static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};
    static constexpr std::uint64_t kErased = ~std::uint64_t{1};

    V* find(std::uint64_t key) noexcept {
        if (slots_.empty()) return nullptr;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return &s.value;
            if (s.key == kEmpty) return nullptr;
        }
    }
    const V* find(std::uint64_t key) const noexcept { return const_cast<FlatMap*>(this)->find(key); }
    // Inserts `value` unless `key` is present. Returns the stored value and
    // whether it was inserted. Pointers stay valid until the next insertion.
    std::pair<V*, bool> insert(std::uint64_t key, V value) {
        if ((used_ + 1) * 4 > slots_.size() * 3) grow();
        std::size_t erased = SIZE_MAX;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return {&s.value, false};
            if (s.key == kErased && erased == SIZE_MAX) erased = i;
            if (s.key == kEmpty) {
                Slot& target = erased != SIZE_MAX ? slots_[erased] : s;
                if (erased == SIZE_MAX) ++used_;
                target.key = key;
                target.value = std::move(value);
                ++live_;
                return {&target.value, true};
            }
        }
    }
    bool erase(std::uint64_t key) noexcept {
        if (slots_.empty()) return false;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) {
                s.key = kErased;
                s.value = V{};
                --live_;
                return true;
            }
            if (s.key == kEmpty) return false;
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return live_; }
    template <typename F>
    void for_each_key(F&& fn) const {
        for (const Slot& s : slots_)
            if (s.key != kEmpty && s.key != kErased) fn(s.key);
    }

private:
    struct Slot {
        std::uint64_t key = kEmpty;
        V value{};
    };
    std::size_t home(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>(((key >> 2) * 0x9E3779B97F4A7C15ull) >> shift_);
    }
    void grow() {
        // Rehash into four times the live size (erased slots are dropped).
        std::size_t capacity = std::size_t{1} << 12;
        while (capacity * 3 < (live_ + 1) * 4 * 4) capacity <<= 1;
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
        shift_ = 64;
        for (std::size_t c = capacity; c > 1; c >>= 1) --shift_;
        used_ = live_ = 0;
        for (Slot& s : old) {
            if (s.key == kEmpty || s.key == kErased) continue;
            for (std::size_t i = home(s.key);; i = (i + 1) & mask_) {
                if (slots_[i].key == kEmpty) {
                    slots_[i].key = s.key;
                    slots_[i].value = std::move(s.value);
                    ++used_;
                    ++live_;
                    break;
                }
            }
        }
    }
    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t used_ = 0;   // live + erased
    std::size_t live_ = 0;
};

// Guest pc -> list of exit references (linked exits targeting it, or pending
// exits waiting for it). Lists are doubly linked through a shared node pool,
// so adding a reference allocates nothing in steady state, and each exit
// records its node (ExitInfo::ref_node): killing a region removes each of its
// exits in O(1). A popular target (a V8 builtin trampoline) has thousands of
// exits waiting for it; a list walk per removal made every SMC kill of V8 code
// quadratic (a third of a Speedometer renderer's main thread).
class ExitRefs {
public:
    void add(std::uint64_t key, ExitRef ref) {
        const std::uint32_t node = allocate(ref);
        auto [head, inserted] = heads_.insert(key, node);
        if (!inserted) {
            nodes_[node].next = *head;
            nodes_[*head].prev = node;
            *head = node;
        }
        ref.region->exits[ref.index].ref_node = node;
    }
    [[nodiscard]] bool contains(std::uint64_t key) const noexcept { return heads_.find(key) != nullptr; }
    // Number of references (diagnostics and tests).
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size() - free_count_; }
    // Removes the list of `key` and calls fn(ref) for each reference; fn may
    // add references (to any key).
    template <typename F>
    void take(std::uint64_t key, F&& fn) {
        const std::uint32_t* head = heads_.find(key);
        if (!head) return;
        std::uint32_t node = *head;
        heads_.erase(key);
        while (node != kNone) {
            const Node n = nodes_[node];
            release(node);
            n.ref.region->exits[n.ref.index].ref_node = kNone;
            fn(n.ref);
            node = n.next;
        }
    }
    // Removes the reference to exit `index` of `region` from the list of `key`.
    void remove(std::uint64_t key, Region* region, std::uint32_t index) {
        ExitInfo& exit = region->exits[index];
        const std::uint32_t node = exit.ref_node;
        exit.ref_node = kNone;
        if (node >= nodes_.size() || nodes_[node].ref.region != region || nodes_[node].ref.index != index) return;
        const Node n = nodes_[node];
        if (n.prev != kNone) {
            nodes_[n.prev].next = n.next;
        } else {
            std::uint32_t* head = heads_.find(key);
            if (!head || *head != node) return;   // not in this list (never: an exit is in one list)
            if (n.next == kNone) heads_.erase(key);
            else *head = n.next;
        }
        if (n.next != kNone) nodes_[n.next].prev = n.prev;
        release(node);
    }

private:
    static constexpr std::uint32_t kNone = UINT32_MAX;
    struct Node {
        ExitRef ref;
        std::uint32_t next;
        std::uint32_t prev;
    };
    std::uint32_t allocate(ExitRef ref) {
        std::uint32_t node = free_;
        if (node != kNone) {
            free_ = nodes_[node].next;
            --free_count_;
        } else {
            node = static_cast<std::uint32_t>(nodes_.size());
            nodes_.push_back({});
        }
        nodes_[node] = {ref, kNone, kNone};
        return node;
    }
    void release(std::uint32_t node) {
        nodes_[node] = {{}, free_, kNone};
        free_ = node;
        ++free_count_;
    }
    FlatMap<std::uint32_t> heads_;
    std::vector<Node> nodes_;
    std::uint32_t free_ = kNone;
    std::size_t free_count_ = 0;
};

// Guest page -> live regions, as lists through a shared node pool (like
// ExitRefs): adding a region to a page allocates nothing in steady state.
class PageLists {
public:
    void add(std::uint64_t page, Region* region) {
        std::uint32_t node = free_;
        if (node != kNone) {
            free_ = nodes_[node].next;
        } else {
            node = static_cast<std::uint32_t>(nodes_.size());
            nodes_.push_back({});
        }
        nodes_[node] = {region, kNone};
        auto [head, inserted] = heads_.insert(page, node);
        if (!inserted) {
            nodes_[node].next = *head;
            *head = node;
        }
    }
    void remove(std::uint64_t page, const Region* region) {
        std::uint32_t* head = heads_.find(page);
        if (!head) return;
        for (std::uint32_t* link = head; *link != kNone;) {
            const std::uint32_t node = *link;
            if (nodes_[node].region != region) {
                link = &nodes_[node].next;
                continue;
            }
            *link = nodes_[node].next;
            nodes_[node] = {nullptr, free_};
            free_ = node;
        }
        if (*head == kNone) heads_.erase(page);
    }
    template <typename F>
    void for_each(std::uint64_t page, F&& fn) const {
        const std::uint32_t* head = heads_.find(page);
        for (std::uint32_t node = head ? *head : kNone; node != kNone; node = nodes_[node].next) fn(nodes_[node].region);
    }
    [[nodiscard]] bool any(std::uint64_t page) const { return heads_.find(page) != nullptr; }
    [[nodiscard]] std::size_t pages() const { return heads_.size(); }
    template <typename F>
    void for_each_page(F&& fn) const { heads_.for_each_key(fn); }
    [[nodiscard]] std::size_t count(std::uint64_t page) const {
        std::size_t n = 0;
        for_each(page, [&](Region*) { ++n; });
        return n;
    }

private:
    static constexpr std::uint32_t kNone = UINT32_MAX;
    struct Node {
        Region* region;
        std::uint32_t next;
    };
    FlatMap<std::uint32_t> heads_;
    std::vector<Node> nodes_;
    std::uint32_t free_ = kNone;
};

struct State {
    host::RwLock map;
    std::mutex translate;
    FlatMap<MapEntry> entries;
    // Code address -> region (incl. dead) is each code chunk's `regions`
    // (ArenaChunk), sorted. Code is bump allocated within a chunk, so installs
    // append (concurrent installs may insert slightly out of order).
    ExitRefs links;                                            // target pc -> linked exits
    ExitRefs pending;                                          // target pc -> unlinked exits (live regions)
    PageLists pages;                                           // guest page -> live regions
    std::unordered_map<std::uint64_t, host::vm::Protection> protected_pages;  // page -> guest protection
    std::uint64_t protection_changes{};   // pages write-protected or given back (smc_handle_write_fault)
    // Disjoint [begin, end) runs already seen non-writable (almost all guest
    // code). One VirtualQuery covers a whole run of equally protected pages,
    // so installing regions does not query per page. Invalidating a range
    // forgets the overlap; every guest protection change invalidates first.
    std::map<std::uint64_t, std::uint64_t> readonly_ranges;
    // Guest pages that lost a live region (stats only: an install on one of
    // them is a re-translation). Never shrinks; bounded by the code pages.
    std::unordered_set<std::uint64_t> killed_pages;
    // Pages an SMC fault inside a JIT write window gave back to the guest
    // without invalidating them; checked when a window closes.
    std::unordered_set<std::uint64_t> deferred_pages;
    // Regions are freed with their code chunk, by reclaim, once dead and unreachable.
    std::atomic<std::uint64_t> epoch{0};                       // bumped by every invalidation
};
State g;

// Tests: called under the exclusive map lock while a region is installed.
std::atomic<void (*)(std::uint64_t)> g_install_hook{nullptr};

thread_local bool t_preloader = false;   // this thread is a preloader (stats only)
// Map lock acquisitions on the guest-thread paths. At startup the preloaders
// take the map lock exclusively tens of thousands of times; each hold is
// short (~2 us), but a guest thread that blocks on the SRW lock takes much
// longer than that to be woken. So a guest thread spins (bounded) before it
// blocks, and preloaders step aside while one spins. Only contended guest
// acquisitions are timed (map_waits / map_wait_ticks).
std::atomic<int> g_guest_spinning{0};

std::uint64_t spin_ticks() noexcept {
    static const std::uint64_t ticks = host::tick_frequency() / 20000;   // 50 us
    return ticks;
}

template <bool Exclusive>
bool try_acquire_map(host::RwLock& lock) noexcept {
    return Exclusive ? lock.try_lock() : lock.try_lock_shared();
}

template <bool Exclusive>
void acquire_map(host::RwLock& lock) noexcept {
    if (t_preloader) {
        // Bounded: a guest thread that keeps spinning gets the lock anyway.
        for (unsigned i = 0; i < 4096 && g_guest_spinning.load(std::memory_order_relaxed) > 0; ++i) _mm_pause();
        // Preloaders contend with each other too: spin a little (holds are
        // short) rather than sleep and pay a wake-up per region.
        for (unsigned i = 0; i < 2048; ++i) {
            if (g_guest_spinning.load(std::memory_order_relaxed) == 0 && try_acquire_map<Exclusive>(lock)) return;
            _mm_pause();
        }
        if (Exclusive) lock.lock();
        else lock.lock_shared();
        return;
    }
    if (try_acquire_map<Exclusive>(lock)) return;
    const std::uint64_t start = ticks_now();
    g_guest_spinning.fetch_add(1, std::memory_order_relaxed);
    bool acquired = false;
    for (unsigned i = 1;; ++i) {
        _mm_pause();
        if (try_acquire_map<Exclusive>(lock)) {
            acquired = true;
            break;
        }
        if ((i & 31) == 0 && ticks_now() - start > spin_ticks()) break;
    }
    g_guest_spinning.fetch_sub(1, std::memory_order_relaxed);
    if (!acquired) {
        if (Exclusive) lock.lock();
        else lock.lock_shared();
    }
    count(g_counters.map_waits);
    count(g_counters.map_wait_ticks, ticks_now() - start);
}

struct TimedShared {
    host::RwLock& lock;
    explicit TimedShared(host::RwLock& l) : lock(l) { acquire_map<false>(lock); }
    ~TimedShared() { lock.unlock_shared(); }
    TimedShared(const TimedShared&) = delete;
    TimedShared& operator=(const TimedShared&) = delete;
};
struct TimedExclusive {
    host::RwLock& lock;
    explicit TimedExclusive(host::RwLock& l) : lock(l) { acquire_map<true>(lock); }
    ~TimedExclusive() { lock.unlock(); }
    TimedExclusive(const TimedExclusive&) = delete;
    TimedExclusive& operator=(const TimedExclusive&) = delete;
};

namespace vm = host::vm;

bool writable(vm::Protection protect) {
    switch (protect & 0xff) {
    case vm::kReadWrite: case vm::kWriteCopy: case vm::kExecuteReadWrite: case vm::kExecuteWriteCopy:
        return true;
    default:
        return false;
    }
}

vm::Protection read_only_for(vm::Protection protect) {
    const vm::Protection extra = protect & ~0xffu;
    switch (protect & 0xff) {
    case vm::kExecuteReadWrite: case vm::kExecuteWriteCopy: return vm::kExecuteRead | extra;
    default: return vm::kReadOnly | extra;
    }
}

bool known_readonly(std::uint64_t page) {
    auto it = g.readonly_ranges.upper_bound(page);
    if (it == g.readonly_ranges.begin()) return false;
    --it;
    return page < it->second;
}

void forget_readonly(std::uint64_t begin, std::uint64_t end) {
    auto it = g.readonly_ranges.upper_bound(begin);
    if (it != g.readonly_ranges.begin()) --it;
    while (it != g.readonly_ranges.end() && it->first < end) {
        const auto [rb, re] = *it;
        if (re <= begin) { ++it; continue; }
        it = g.readonly_ranges.erase(it);
        if (rb < begin) g.readonly_ranges.emplace(rb, begin);
        if (re > end) g.readonly_ranges.emplace(end, re);
    }
}

vm::Protection current_protection(std::uint64_t page) {
    vm::RegionInfo info;
    if (!vm::query(reinterpret_cast<const void*>(page), info) || !info.committed()) return 0;
    return info.protect;
}

// Map lock held.
Region* region_at(std::uint64_t host) {
    const std::uint32_t chunk = arena_chunk_of(host);
    if (chunk == ~0u) return nullptr;
    const auto& regions = arena_chunk(chunk).regions;
    auto it = std::upper_bound(regions.begin(), regions.end(), host,
                               [](std::uint64_t h, const Region* r) { return h < reinterpret_cast<std::uint64_t>(r->code); });
    if (it == regions.begin()) return nullptr;
    Region* r = *--it;
    return host - reinterpret_cast<std::uint64_t>(r->code) < r->size ? r : nullptr;
}

// Map lock held exclusively: `r` is installed in its chunk.
void add_to_chunk(Region* r) {
    ArenaChunk& chunk = arena_chunk(r->chunk);
    auto& regions = chunk.regions;
    const auto before = [](const Region* a, const Region* b) { return a->code < b->code; };
    if (regions.empty() || before(regions.back(), r)) regions.push_back(r);
    else regions.insert(std::upper_bound(regions.begin(), regions.end(), r, before), r);
    ++chunk.live;
    chunk.live_bytes += r->size;
}

ExitInfo* find_exit(Region& r, std::uint32_t rel32_offset, std::uint32_t& index) {
    auto it = std::lower_bound(r.exits.begin(), r.exits.end(), rel32_offset,
                               [](const ExitInfo& e, std::uint32_t off) { return e.rel32_offset < off; });
    if (it == r.exits.end() || it->rel32_offset != rel32_offset) return nullptr;
    index = static_cast<std::uint32_t>(it - r.exits.begin());
    return &*it;
}

void restore_exit(Region& r, ExitInfo& e) {
    auto* field = r.code + e.rel32_offset;
    *reinterpret_cast<volatile std::int32_t*>(field) = e.original;
    if (e.skip_offset) *reinterpret_cast<volatile std::int32_t*>(r.code + e.skip_offset) = e.skip_original;
    e.linked = false;
}

// Map lock held exclusively. Invariant: an exit has a `pending` reference
// exactly while its region is live and the exit is unlinked; a `links`
// reference exactly while it is linked (always to the map entry of its target).
// An exit with a flag store (skip_offset) links its jmp A past the store when
// the target's entry writes NZCV first, else its jmp B after the store.
void patch_link(Region& r, std::uint32_t index, const MapEntry& target) {
    auto& e = r.exits[index];
    const void* host = target.host;
    x64::Assembler::patch_rel32(r.code + (e.skip_offset && target.flags_dead ? e.skip_offset : e.rel32_offset), host);
    e.linked = true;
    g.links.add(e.target_pc, {&r, index});
    count(g_counters.links);
}

// Map lock held exclusively: every exit linked to `pc` returns to its
// unlinked tail and waits (pending) for pc's next entry, as an exit the
// dispatcher would relink on first use.
void unlink_target(std::uint64_t pc) {
    g.links.take(pc, [&](const ExitRef& ref) {
        auto& e = ref.region->exits[ref.index];
        if (!e.linked || e.target_pc != pc) return;
        restore_exit(*ref.region, e);
        count(g_counters.unlinks);
        if (!ref.region->dead) g.pending.add(pc, ref);
    });
}

// Map lock held exclusively: links every pending exit of a live region that
// targets `pc` to `entry` (the entry just installed for pc).
void link_pending(std::uint64_t pc, const MapEntry& entry) {
    g.pending.take(pc, [&](const ExitRef& ref) {
        const auto& e = ref.region->exits[ref.index];
        if (!ref.region->dead && !e.linked && e.target_pc == pc) patch_link(*ref.region, ref.index, entry);
    });
}

void forget_pending(std::uint64_t target, Region* region, std::uint32_t index) {
    g.pending.remove(target, region, index);
}

void forget_link(std::uint64_t target, Region* region, std::uint32_t index) {
    g.links.remove(target, region, index);
}

template <typename F>
void for_each_page(const Region& r, F&& f) {
    for (const auto& range : r.guest_ranges) {
        if (range.second <= range.first) continue;
        for (auto page = range.first & ~(kPage - 1); page < range.second; page += kPage) f(page);
    }
}

bool overlaps(const Region& r, std::uint64_t begin, std::uint64_t end) {
    for (const auto& range : r.guest_ranges)
        if (range.first < end && begin < range.second) return true;
    return false;
}

// Why a region dies: its guest code changed (or was dropped); reclaim takes
// its chunk back.
enum class Kill { invalidated, evicted };

// Map lock held exclusively. The region's code stays mapped (stale threads,
// fault sites); it just becomes unreachable from the map, the lookup table,
// links and RSBs. Its chunk can be reclaimed once all its regions are dead.
void kill(Region& r, Kill why = Kill::invalidated) {
    if (r.dead) return;
    r.dead = true;
    if (!r.retired) {
        ArenaChunk& chunk = arena_chunk(r.chunk);
        --chunk.live;
        chunk.live_bytes -= r.size;
    }
    for (const auto& e : r.entries) {
        const MapEntry* entry = g.entries.find(e.guest_pc);
        if (entry && entry->region == &r) {
            lookup_remove(e.guest_pc, entry->host);
            g.entries.erase(e.guest_pc);
            unlink_target(e.guest_pc);
        }
    }
    for (std::uint32_t i = 0; i < r.exits.size(); ++i) {
        auto& e = r.exits[i];
        if (!e.linked) {
            forget_pending(e.target_pc, &r, i);
            continue;
        }
        forget_link(e.target_pc, &r, i);
        restore_exit(r, e);
    }
    for_each_page(r, [&](std::uint64_t page) {
        g.pages.remove(page, &r);
        if (why == Kill::invalidated) g.killed_pages.insert(page);
    });
    const auto begin = reinterpret_cast<std::uint64_t>(r.code);
    const auto end = begin + r.size;
    const void* redispatch = g_stubs.rsb_redispatch;
    for_each_context([&](GuestContext& ctx) {
        for (auto& entry : ctx.rsb) {
            const auto host = reinterpret_cast<std::uint64_t>(entry.host_entry);
            if (host >= begin && host < end) entry.host_entry = redispatch;
        }
    });
    if (why == Kill::evicted) {
        count(g_counters.evicted_regions);
        return;
    }
    count(g_counters.invalidated_regions);
    count(g_counters.arena_dead, r.size);
}

// Map lock held exclusively, after the page's regions that had to die were
// killed. Gives a page fx write-protected its guest protection back, unless
// live regions remain on it: a range that ends inside a page (flush_guest_code)
// keeps the translations outside it, and they stay protected. Restores only if
// the page still has the protection fx gave it, so a protection change made
// by the guest before this call survives.
void restore_page(std::uint64_t page) {
    auto it = g.protected_pages.find(page);
    if (it == g.protected_pages.end()) return;
    if (g.pages.any(page)) {
        count(g_counters.invalidation_pages_kept);
        return;
    }
    const vm::Protection now = current_protection(page);
    if (now != 0 && (now & 0xff) == (read_only_for(it->second) & 0xff))
        vm::protect(reinterpret_cast<void*>(page), kPage, it->second);
    g.protected_pages.erase(it);
    ++g.protection_changes;
}

// Map lock held exclusively. Returns true if any region died. Regions die if
// their guest bytes overlap [begin, end) (not merely share a page with it).
bool invalidate_locked(std::uint64_t begin, std::uint64_t end, bool restore_protection) {
    std::vector<Region*> victims;
    std::vector<std::uint64_t> alias_pages;
    std::unordered_set<Region*> seen;
    const auto first = begin & ~(kPage - 1);
    // A huge range (a code range unregistered or made writable, a terabyte
    // reservation) visits only the pages that hold regions or protection.
    const bool sparse = end - first > (64u << 20) &&
                        (end - first) / kPage > 4 * (g.pages.pages() + g.protected_pages.size());
    std::vector<std::uint64_t> listed;
    if (sparse) {
        g.pages.for_each_page([&](std::uint64_t page) {
            if (page >= first && page < end) listed.push_back(page);
        });
        if (restore_protection)
            for (const auto& [page, saved] : g.protected_pages)
                if (page >= first && page < end) listed.push_back(page);
        std::sort(listed.begin(), listed.end());
        listed.erase(std::unique(listed.begin(), listed.end()), listed.end());
    }
    const auto for_each_listed = [&](auto&& fn) {
        if (sparse) {
            for (const std::uint64_t page : listed) fn(page);
        } else {
            for (auto page = first; page < end; page += kPage) fn(page);
        }
    };
    for_each_listed([&](std::uint64_t page) {
        g.pages.for_each(page, [&](Region* r) {
            if (overlaps(*r, begin, end) && seen.insert(r).second) victims.push_back(r);
        });
    });
    // Shared guest memory mapped at other addresses too (shared_memory.cpp):
    // a change through one address changes the code at all of them.
    if (g_shared_aliases.load(std::memory_order_acquire) && end - begin <= (64u << 20)) {
        std::vector<std::uint64_t> aliases;
        shared_memory_aliases(begin, end, aliases);
        for (const std::uint64_t alias : aliases) {
            g.pages.for_each(alias, [&](Region* r) {
                if (overlaps(*r, alias, alias + kPage) && seen.insert(r).second) victims.push_back(r);
            });
            if (restore_protection) alias_pages.push_back(alias);
        }
    }
    for (Region* r : victims) kill(*r);
    if (!g.readonly_ranges.empty()) forget_readonly(begin & ~(kPage - 1), end);
    if (restore_protection) {
        for_each_listed([](std::uint64_t page) { restore_page(page); });
        for (const std::uint64_t alias : alias_pages) restore_page(alias);
    }
    g.epoch.fetch_add(1, std::memory_order_acq_rel);
    if (!victims.empty()) {
        // Threads still inside dead code leave it at their next safe point.
        for_each_context([](GuestContext& ctx) { ctx.interrupt_pending = 1; });
        count(g_counters.invalidation_interrupts);
    }
    return !victims.empty();
}

// Map lock held exclusively. Write-protects one guest page unless fx already
// protects it or it is not writable. True if it was writable by the guest.
bool protect_page(std::uint64_t page) {
    if (g.protected_pages.count(page) || known_readonly(page)) return false;
    vm::RegionInfo info;
    if (!vm::query(reinterpret_cast<const void*>(page), info) || !info.committed()) return false;
    const vm::Protection protect = info.protect;
    if (!writable(protect)) {
        const auto run_begin = info.base;
        forget_readonly(run_begin, run_begin + info.size);   // keep ranges disjoint
        g.readonly_ranges.emplace(run_begin, run_begin + info.size);
        return false;
    }
    vm::Protection old;
    if (vm::protect(reinterpret_cast<void*>(page), kPage, read_only_for(protect), &old)) {
        g.protected_pages.emplace(page, old);
        ++g.protection_changes;
    }
    return true;
}

// Map lock held exclusively. Write-protects writable guest pages the region
// depends on (a page that cannot be protected just loses SMC detection).
// Returns true if a page was writable by the guest until now: a check of its
// contents made before this call (a cached region's guest hash) may be stale.
bool protect_pages(const Region& r) {
    bool newly_protected = false;
    const bool shared = g_shared_aliases.load(std::memory_order_acquire);
    std::vector<std::uint64_t> aliases;
    for_each_page(r, [&](std::uint64_t page) { newly_protected |= protect_page(page); });
    // The same pages mapped elsewhere (shared guest memory): protect them there too.
    if (shared) {
        for (const auto& [begin, end] : r.guest_ranges) shared_memory_aliases(begin, end, aliases);
        for (const std::uint64_t alias : aliases) newly_protected |= protect_page(alias);
    }
    return newly_protected;
}

// Hash of a region's guest bytes (its guest ranges, which the translator
// names as everything the code depends on); never 0, and 0 if a word cannot
// be read.
std::uint64_t guest_bytes_hash(const Region& r) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    for (const auto& [begin, end] : r.guest_ranges) {
        h = (h ^ begin) * 0xff51afd7ed558ccdull;
        for (std::uint64_t address = begin & ~std::uint64_t{3}; address < end; address += 4) {
            std::uint32_t word;
            if (!safe_read32(address, word)) return 0;
            h = (h ^ word) * 0xc4ceb9fe1a85ec53ull;
            h ^= h >> 29;
        }
    }
    return h | 1;
}

bool verify_reads(const LiveCodeSource& source) {
    for (const auto& [address, word] : source.reads()) {
        std::uint32_t now;
        if (!safe_read32(address, now) || now != word) return false;
    }
    return true;
}

// Load/store exclusive class (LDXR/STXR/LDAXR/CAS/CASP, also LDAR/STLR) or
// an LSE atomic memory operation (LDADD/SWP/... ).
bool is_exclusive_or_atomic(std::uint32_t word) {
    return (word & 0x3f000000u) == 0x08000000u || (word & 0x3b200c00u) == 0x38200000u;
}

bool region_is_valid(const TranslatedRegion& r, std::uint64_t pc) {
    if (r.code.empty() || r.code.size() > 0x7fffffffu || r.entries.empty()) return false;
    bool has_pc = false;
    for (const auto& e : r.entries) {
        if (e.offset >= r.code.size()) return false;
        has_pc |= e.guest_pc == pc;
    }
    for (const auto& e : r.exits) {
        if ((e.rel32_offset & 3) != 0 || std::uint64_t{e.rel32_offset} + 4 > r.code.size()) return false;
        if ((e.skip_rel32_offset & 3) != 0 || std::uint64_t{e.skip_rel32_offset} + 4 > r.code.size() ||
            (e.skip_rel32_offset != 0 && e.skip_rel32_offset == e.rel32_offset))
            return false;
    }
    for (const auto& f : r.fixups)
        if (std::uint64_t{f.offset} + 8 > r.code.size()) return false;
    if (r.tier == 0 && r.tier_counter != 0 && std::uint64_t{r.tier_counter} + 4 > r.code.size()) return false;
    return has_pc;
}

// ---- tiering ------------------------------------------------------------------
// First translations are formed lazily (TranslateOptions::lazy): a region cut
// short (first-tier budget, or call continuations not followed) is tier 0 and
// counts its region-start entries. When the count runs out it exits with
// ExitReason::tier_up and is retranslated as a tier-1 (eager) region, which
// replaces the entries of lower-tier regions it covers (install below).
//
// Knobs (runtime; formation only, so not part of the cache key):
//   TRANSMUTE_FX_TIER_THRESHOLD=<entries>      tier-0 entries before tier-up (4096)
//   TRANSMUTE_FX_TIER_BUDGET=<instructions>    first-tier budget (4096)
//   TRANSMUTE_FX_TIER_SPLIT=1                  first tier stops at call continuations
//   TRANSMUTE_FX_TIER_WINDOW=<fwd>[,<back>]    bytes; synthetic bounds for code
//                                              without .pdata (8192,2048; 0 = none)
//   TRANSMUTE_FX_JIT_CODE_BUDGET=<instructions>  first-tier budget and window for
//   TRANSMUTE_FX_JIT_CODE_WINDOW=<fwd>[,<back>]  registered (JIT) code (as above)
// TRANSMUTE_FX_CORE=notier disables tier-0 formation (translator knob, folded
// into the cache key).
struct TierConfig {
    unsigned threshold = 4096;
    unsigned budget = 4096;
    bool split = false;
    std::uint32_t window_forward = 8192;
    std::uint32_t window_backward = 2048;
    // First tier of registered (non-image) code, e.g. V8's JIT code: much of
    // it runs a few times and is freed (TRANSMUTE_FX_JIT_CODE_BUDGET,
    // TRANSMUTE_FX_JIT_CODE_WINDOW=<fwd>[,<back>]; default: as above).
    unsigned jit_budget = 4096;
    std::uint32_t jit_window_forward = 8192;
    std::uint32_t jit_window_backward = 2048;
};
const TierConfig& tier_config() {
    static const TierConfig c = [] {
        TierConfig t;
        char value[48] = {};
        if (host::get_env("TRANSMUTE_FX_TIER_THRESHOLD", value, sizeof(value)) > 0)
            t.threshold = static_cast<unsigned>(std::clamp<unsigned long>(std::strtoul(value, nullptr, 10), 1, 1u << 30));
        if (host::get_env("TRANSMUTE_FX_TIER_BUDGET", value, sizeof(value)) > 0)
            t.budget = static_cast<unsigned>(std::clamp<unsigned long>(std::strtoul(value, nullptr, 10), 16, 4096));
        if (host::get_env("TRANSMUTE_FX_TIER_SPLIT", value, sizeof(value)) > 0) t.split = value[0] == '1';
        if (host::get_env("TRANSMUTE_FX_TIER_WINDOW", value, sizeof(value)) > 0) {
            char* end = nullptr;
            t.window_forward = static_cast<std::uint32_t>(std::min<unsigned long>(std::strtoul(value, &end, 0), 1u << 24));
            t.window_backward = end && *end == ',' ? static_cast<std::uint32_t>(std::min<unsigned long>(
                                                         std::strtoul(end + 1, nullptr, 0), 1u << 24))
                                                   : t.window_forward / 4;
        }
        t.jit_budget = t.budget;
        t.jit_window_forward = t.window_forward;
        t.jit_window_backward = t.window_backward;
        if (host::get_env("TRANSMUTE_FX_JIT_CODE_BUDGET", value, sizeof(value)) > 0)
            t.jit_budget = static_cast<unsigned>(std::clamp<unsigned long>(std::strtoul(value, nullptr, 10), 16, 4096));
        if (host::get_env("TRANSMUTE_FX_JIT_CODE_WINDOW", value, sizeof(value)) > 0) {
            char* end = nullptr;
            t.jit_window_forward = static_cast<std::uint32_t>(std::min<unsigned long>(std::strtoul(value, &end, 0), 1u << 24));
            t.jit_window_backward = end && *end == ',' ? static_cast<std::uint32_t>(std::min<unsigned long>(
                                                             std::strtoul(end + 1, nullptr, 0), 1u << 24))
                                                       : t.jit_window_forward / 4;
        }
        return t;
    }();
    return c;
}

template <typename T, typename Source>
RegionArray<T> place_array(std::uint8_t*& at, const Source& source) {
    RegionArray<T> a;
    a.data_ = reinterpret_cast<T*>(at);
    a.size_ = static_cast<std::uint32_t>(source.size());
    std::uninitialized_copy(source.begin(), source.end(), a.data_);
    at += (source.size() * sizeof(T) + 15) & ~std::size_t{15};
    return a;
}

template <typename T>
std::size_t array_bytes(std::size_t n) { return (n * sizeof(T) + 15) & ~std::size_t{15}; }

// An entry owned by a live region of lower tier than `tier` is replaced when
// a region covering it is installed.
bool replaceable(const MapEntry& entry, std::uint8_t tier) {
    return entry.region && !entry.region->dead && entry.region->tier < tier;
}

// Installs a region. JIT regions (`source` set) are validated by re-reading,
// under the exclusive map lock and after write protection, every guest word
// the translator saw: the translation matches memory at publication, and any
// later change faults or is invalidated. Cached regions (no source) are
// validated with the invalidation epoch instead: the cache hashed the guest
// bytes, and a write since then to a protected page bumps the epoch. A page
// that only becomes protected here could have been written between that hash
// and the protection, unseen; then the region is not installed and
// `recheck` is set, so the caller repeats the cache lookup (the hash is now
// taken under protection). Returns nullptr if the region was not installed.
const void* install(const TranslatedRegion& t, std::uint64_t pc, std::uint64_t epoch,
                    const LiveCodeSource* source, bool* recheck = nullptr) {
    // Another thread (or the preloader) may have installed pc meanwhile:
    // do not copy code that would never be reachable (unless this region
    // replaces a lower tier there). Without tier-0 regions no entry is
    // replaceable, so the lock-free lookup table answers (a slot lost to a
    // collision only costs the copy: the check below is authoritative).
    if (g_counters.tier0_regions.load(std::memory_order_acquire) == 0) {
        if (const void* host = lookup_probe(pc)) return host;
    } else {
        TimedShared lock(g.map);
        if (const MapEntry* entry = g.entries.find(pc); entry && !replaceable(*entry, t.tier)) return entry->host;
    }
    // Copy and relocate outside the map lock. JIT code of registered guest
    // code memory goes into the volatile stream: a guest JIT discards it
    // together, so its chunks empty out without evicting anything.
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    const std::size_t meta = array_bytes<Region>(1) + array_bytes<RegionEntry>(t.entries.size()) +
                             array_bytes<ExitInfo>(t.exits.size()) + array_bytes<FaultSite>(t.faults.size()) +
                             array_bytes<PinnedRegister>(t.pins.size()) + array_bytes<FaultMap>(t.fault_maps.size()) +
                             array_bytes<PinnedRegister>(t.fault_map_regs.size()) + array_bytes<Range>(t.guest_ranges.size());
    const bool counted = t.tier == 0 && t.tier_counter != 0;
    const CodeStream stream = source && in_registered_code_memory(t.guest_start) ? CodeStream::volatile_code
                                                                                 : CodeStream::stable;
    const CodeBlock block = code_allocate(t.code.size(), meta, counted, stream);
    // Released after the map lock below (installed or not): until then
    // reclaim leaves the chunk alone.
    struct Release {
        const CodeBlock& block;
        ~Release() { code_release(block); }
    } release{block};
    std::uint8_t* const code = block.code;
    std::memcpy(code, t.code.data(), t.code.size());
    for (const auto& f : t.fixups) {
        if (f.kind == FixupKind::guest_address_imm64) std::memcpy(code + f.offset, &f.value, 8);
    }
    std::uint32_t* counter = nullptr;
    if (counted) {
        counter = block.counter;   // a counter page of the same chunk (never next to code)
        *counter = tier_config().threshold;
        const auto next = reinterpret_cast<std::int64_t>(code + t.tier_counter + 4);
        const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::int64_t>(counter) - next);
        std::memcpy(code + t.tier_counter, &rel, 4);
    }
    // x64 keeps instruction fetch coherent with stores, and bytes of a fresh
    // chunk never executed, so they need no FlushInstructionCache (a no-op
    // system call on x64, ~0.5 us each). A reused chunk did hold code: every
    // thread was suspended and resumed after that code died (reclaim), and
    // the flush is the documented contract for code written over code. The
    // code is published by the map lock release below.
    if (block.reused) vm::flush_instruction_cache(code, t.code.size());

    auto* at = static_cast<std::uint8_t*>(block.meta);
    Region* const region = new (at) Region();
    at += array_bytes<Region>(1);
    region->guest_start = t.guest_start;
    region->code = code;
    region->size = static_cast<std::uint32_t>(t.code.size());
    region->hot_size = t.stats.hot_bytes != 0 && t.stats.hot_bytes <= region->size ? t.stats.hot_bytes : region->size;
    region->entries = place_array<RegionEntry>(at, t.entries);
    region->faults = place_array<FaultSite>(at, t.faults);
    std::sort(region->faults.begin(), region->faults.end(),
              [](const FaultSite& a, const FaultSite& b) { return a.host_begin < b.host_begin; });
    region->pins = place_array<PinnedRegister>(at, t.pins);
    region->fault_maps = place_array<FaultMap>(at, t.fault_maps);
    region->fault_map_regs = place_array<PinnedRegister>(at, t.fault_map_regs);
    region->guest_ranges = place_array<Range>(at, t.guest_ranges);
    region->tier = t.tier;
    region->counter = counter;
    region->chunk = block.chunk;
    region->exits.data_ = reinterpret_cast<ExitInfo*>(at);
    region->exits.size_ = static_cast<std::uint32_t>(t.exits.size());
    for (std::size_t i = 0; i < t.exits.size(); ++i) {
        const auto& e = t.exits[i];
        std::int32_t original;
        std::memcpy(&original, code + e.rel32_offset, 4);
        new (&region->exits[i]) ExitInfo{e.rel32_offset, original, e.target_pc, false};
        if (e.skip_rel32_offset) {
            region->exits[i].skip_offset = e.skip_rel32_offset;
            std::memcpy(&region->exits[i].skip_original, code + e.skip_rel32_offset, 4);
        }
    }
    std::sort(region->exits.begin(), region->exits.end(),
              [](const ExitInfo& a, const ExitInfo& b) { return a.rel32_offset < b.rel32_offset; });

    TimedExclusive lock(g.map);
    if (const MapEntry* entry = g.entries.find(pc); entry && !replaceable(*entry, t.tier)) return entry->host;
    if (!is_guest_code(pc)) return nullptr;                                 // unregistered meanwhile
    if (!source && g.epoch.load(std::memory_order_acquire) != epoch) return nullptr;
    const bool newly_protected = protect_pages(*region);
    if (source && !verify_reads(*source)) return nullptr;                   // modified meanwhile: retry
    if (source && in_registered_code_memory(pc)) region->guest_hash = guest_bytes_hash(*region);
    if (!source && newly_protected) {
        if (recheck) *recheck = true;
        return nullptr;
    }
    if (const auto hook = g_install_hook.load(std::memory_order_relaxed)) hook(pc);
    if (!g.killed_pages.empty()) {
        bool again = false;
        for_each_page(*region, [&](std::uint64_t page) { again = again || g.killed_pages.count(page) != 0; });
        if (again) count(g_counters.retranslations);
    }

    Region* r = region;
    add_to_chunk(r);
    const void* result = nullptr;
    struct Replaced {
        std::uint64_t pc;
        const void* old_host;
        const void* host;
        Region* old;
    };
    thread_local std::vector<Replaced> replaced;
    replaced.clear();
    for (const auto& e : r->entries) {
        const void* host = code + e.offset;
        auto [entry, inserted] = g.entries.insert(e.guest_pc, MapEntry{host, r, e.flags_dead});
        if (!inserted && replaceable(*entry, r->tier)) {
            // A lower tier's entry: future entries come here. Exits linked to
            // the old entry are relinked (unlink, then link_pending below).
            replaced.push_back({e.guest_pc, entry->host, host, entry->region});
            lookup_remove(e.guest_pc, entry->host);
            *entry = MapEntry{host, r, e.flags_dead};
            unlink_target(e.guest_pc);
            inserted = true;
        }
        if (e.guest_pc == pc) result = entry->host;
        if (!inserted) continue;   // first translation of a pc keeps it (same or higher tier)
        lookup_install(e.guest_pc, host);
        link_pending(e.guest_pc, *entry);
    }
    if (!replaced.empty()) {
        // RSB entries that would return into a replaced entry return into its
        // replacement (both are valid; this keeps the old code cold).
        for_each_context([&](GuestContext& ctx) {
            for (auto& slot : ctx.rsb)
                for (const auto& rp : replaced)
                    if (slot.guest_return == rp.pc && slot.host_entry == rp.old_host) slot.host_entry = rp.host;
        });
        count(g_counters.tier_replaced_entries, replaced.size());
    }
    if (r->tier == 0) count(g_counters.tier0_regions);
    // A lower-tier region none of whose entries is reachable any more (links
    // into it were moved to the new entries above) is retired: its chunk
    // counts it as dead, so reclaim may take the chunk, evicting it then. It
    // stays live otherwise: on its pages, so a write there still kills it
    // and interrupts threads still inside it.
    for (const auto& rp : replaced) {
        Region* old = rp.old;
        if (old->dead || old->retired) continue;
        bool reachable = false;
        for (const auto& e : old->entries) {
            const MapEntry* entry = g.entries.find(e.guest_pc);
            reachable = reachable || (entry && entry->region == old);
        }
        if (reachable) continue;
        old->retired = true;
        ArenaChunk& chunk = arena_chunk(old->chunk);
        --chunk.live;
        chunk.live_bytes -= old->size;
        count(g_counters.retired_regions);
    }
    for_each_page(*r, [&](std::uint64_t page) { g.pages.add(page, r); });
    // Link exits whose targets already exist; the others link when their target is installed.
    for (std::uint32_t i = 0; i < r->exits.size(); ++i) {
        const std::uint64_t target = r->exits[i].target_pc;
        if (const MapEntry* entry = g.entries.find(target)) patch_link(*r, i, *entry);
        else g.pending.add(target, {r, i});
    }
    count(g_counters.regions);
    return result;
}

} // namespace

TranslateOptions jit_translate_options() noexcept {
    TranslateOptions topts;
    topts.profile = options().profile;
    // First translations are tier 0 (the cache key ignores these: see TranslateOptions::lazy).
    topts.lazy = true;
    topts.lazy_max_instructions = tier_config().budget;
    topts.lazy_split_calls = tier_config().split;
    topts.tier_threshold = tier_config().threshold;
    topts.window_forward = tier_config().window_forward;
    topts.window_backward = tier_config().window_backward;
    return topts;
}

namespace {
// The first-tier options for `pc`: registered (non-image) code gets its own
// budget and window (TierConfig::jit_*). Formation only: not in the cache key.
TranslateOptions first_tier_options(std::uint64_t pc) {
    TranslateOptions topts = jit_translate_options();
    if (in_registered_code_memory(pc)) {
        topts.lazy_max_instructions = tier_config().jit_budget;
        topts.window_forward = tier_config().jit_window_forward;
        topts.window_backward = tier_config().jit_window_backward;
    }
    return topts;
}
} // namespace

std::uint64_t invalidation_epoch() noexcept { return g.epoch.load(std::memory_order_acquire); }

namespace {
// Bumped by explicit invalidations (protection changes, unmaps, decommits),
// not by SMC faults or code flushes, which change no page's accessibility.
std::atomic<std::uint64_t> g_access_epoch{0};
} // namespace

std::uint64_t memory_access_epoch() noexcept { return g_access_epoch.load(std::memory_order_acquire); }

bool has_host_entry(std::uint64_t pc) {
    SharedLock lock(g.map);
    const MapEntry* entry = g.entries.find(pc);
    return entry && !entry->region;
}

bool with_map_entry(std::uint64_t pc, void (*fn)(const void* host, void* arg), void* arg) {
    TimedShared lock(g.map);
    const MapEntry* entry = g.entries.find(pc);
    if (!entry) return false;
    fn(entry->host, arg);
    return true;
}

const void* find_entry(std::uint64_t pc) {
    if (const void* host = lookup_probe(pc)) return host;
    TimedShared lock(g.map);
    const MapEntry* entry = g.entries.find(pc);
    if (!entry) return nullptr;
    lookup_install(pc, entry->host);
    return entry->host;
}

namespace {
struct TickScope {
    std::atomic<std::uint64_t>& counter;
    std::uint64_t start = ticks_now();
    ~TickScope() { count(counter, ticks_now() - start); }
};
} // namespace

namespace {
// JIT translations of registered (non-image) guest code, kept for reuse (g.translate held).
// V8 writes new code next to code fx translated: the SMC fault kills every
// region on the page, and most of them are needed again with the same bytes.
// A kept region is installed only if the words it was translated from are
// unchanged, verified under write protection like a fresh translation.
struct Reusable {
    TranslatedRegion region;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
};
std::unordered_map<std::uint64_t, Reusable> g_reusable;
std::size_t g_reusable_bytes = 0;
constexpr std::size_t kReusableLimit = std::size_t{64} << 20;

bool same_words(const std::vector<std::pair<std::uint64_t, std::uint32_t>>& reads) {
    for (const auto& [address, word] : reads) {
        std::uint32_t now;
        if (!safe_read32(address, now) || now != word) return false;
    }
    return true;
}

// Reuse is for regions an SMC fault killed. Explicit invalidations
// (protection changes, unmaps, unregistering) drop the kept regions of their
// range, and a host entry installed or removed drops all of them (a region
// translated before it would not stop there). Recorded under a leaf lock and
// applied by translate_and_install under g.translate.
std::mutex g_reusable_drops_mutex;
std::vector<std::pair<std::uint64_t, std::uint64_t>> g_reusable_drops;
bool g_reusable_drop_all = false;
std::atomic<bool> g_reusable_drops_pending{false};

void drop_reusable(std::uint64_t begin, std::uint64_t end, bool all = false) {
    std::lock_guard lock(g_reusable_drops_mutex);
    if (all || g_reusable_drops.size() >= 64) g_reusable_drop_all = true;
    else g_reusable_drops.emplace_back(begin, end);
    g_reusable_drops_pending.store(true, std::memory_order_release);
}

void apply_reusable_drops() {
    if (!g_reusable_drops_pending.load(std::memory_order_acquire)) return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> drops;
    bool all;
    {
        std::lock_guard lock(g_reusable_drops_mutex);
        drops.swap(g_reusable_drops);
        all = g_reusable_drop_all;
        g_reusable_drop_all = false;
        g_reusable_drops_pending.store(false, std::memory_order_relaxed);
    }
    if (all) {
        g_reusable.clear();
        g_reusable_bytes = 0;
        return;
    }
    for (auto it = g_reusable.begin(); it != g_reusable.end();) {
        bool hit = false;
        for (const auto& range : it->second.region.guest_ranges)
            for (const auto& drop : drops) hit = hit || (range.first < drop.second && drop.first < range.second);
        if (!hit) { ++it; continue; }
        g_reusable_bytes -= std::min(g_reusable_bytes, it->second.region.code.size() + it->second.reads.size() * 16 + 256);
        it = g_reusable.erase(it);
    }
}

void keep_reusable(std::uint64_t pc, const TranslatedRegion& region, const LiveCodeSource& source) {
    const std::size_t bytes = region.code.size() + source.reads().size() * 16 + 256;
    if (g_reusable_bytes + bytes > kReusableLimit) {
        g_reusable.clear();
        g_reusable_bytes = 0;
    }
    auto& kept = g_reusable[pc];
    if (!kept.reads.empty()) g_reusable_bytes -= std::min(g_reusable_bytes, kept.region.code.size() + kept.reads.size() * 16 + 256);
    kept.region = region;
    kept.reads = source.reads();
    g_reusable_bytes += bytes;
}
} // namespace

namespace {
// A region for the copy at `delta` from its source (register_guest_code_copy)
// is usable only where the copy's guest bytes equal the source's: fills
// `source` with the copy's words over the region's guest ranges (install
// verifies them again under write protection). False if any differs.
bool copy_matches_source(const TranslatedRegion& region, std::int64_t delta, LiveCodeSource& source) {
    thread_local std::vector<std::pair<std::uint64_t, std::uint32_t>> words;
    words.clear();
    const auto d = static_cast<std::uint64_t>(delta);
    for (const auto& [begin, end] : region.guest_ranges) {
        for (std::uint64_t address = begin & ~std::uint64_t{3}; address < end; address += 4) {
            std::uint32_t here, there;
            if (!safe_read32(address, here) || !safe_read32(address - d, there) || here != there) return false;
            words.emplace_back(address, here);
        }
    }
    source.reset();
    source.assign_reads(words);
    return true;
}

// A translation of a copy of image code from the source module's cache.
const void* install_from_copy_source(std::uint64_t pc, std::int64_t delta, LiveCodeSource& source, bool tier1) {
    thread_local TranslatedRegion copied;
    bool hit;
    {
        TickScope t{g_counters.cache_ticks};
        hit = cache_lookup_copy(pc - static_cast<std::uint64_t>(delta), delta, copied) &&
              (!tier1 || copied.tier >= 1) && region_is_valid(copied, pc) && copy_matches_source(copied, delta, source);
    }
    if (!hit) return nullptr;
    TickScope t{g_counters.install_ticks};
    const void* host = install(copied, pc, 0, &source);
    if (host) count(g_counters.copy_cache_hits);
    return host;
}

// Persists a JIT translation: in its module's cache, or in the source
// module's cache for a copy whose guest bytes equal the source's.
void store_translation(const TranslatedRegion& region, bool copy, std::int64_t delta) {
    if (!copy) {
        cache_store(region);
        return;
    }
    thread_local LiveCodeSource check;
    if (copy_matches_source(region, delta, check)) cache_store_copy(region, delta);
}
} // namespace

const void* translate_and_install(std::uint64_t pc) {
    TickScope total{g_counters.translate_ticks};
    std::lock_guard serialize(g.translate);
    const TranslateOptions topts = first_tier_options(pc);
    const bool cache = options().enable_translation_cache;
    bool try_cache = cache;
    unsigned cache_attempts = 0;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        if (const void* host = find_entry(pc)) return host;
        if (attempt) count(g_counters.translation_retries);
        thread_local TranslatedRegion region;
        if (try_cache) {
            // One attempt (two if the first one had to write-protect a page):
            // a failed cached install falls back to the JIT.
            try_cache = false;
            ++cache_attempts;
            const auto epoch = g.epoch.load(std::memory_order_acquire);
            // Reused per thread: rebuilding a cached region allocates nothing.
            thread_local TranslatedRegion cached;
            bool hit;
            {
                TickScope t{g_counters.cache_ticks};
                hit = cache_lookup(pc, cached) && region_is_valid(cached, pc);
            }
            if (hit) {
                count(g_counters.cache_hits);
                preload_hint(pc);   // the preloader is behind here: let it continue ahead of us
                TickScope t{g_counters.install_ticks};
                bool recheck = false;
                if (const void* host = install(cached, pc, epoch, nullptr, &recheck)) return host;
                if (recheck && cache_attempts == 1) try_cache = true;
                continue;
            }
        }
        // Reused per thread (translations hold g.translate): no allocations
        // for the source's read log or the region's vectors once warm.
        thread_local LiveCodeSource source;
        const bool registered = in_registered_code_memory(pc);
        if (registered && attempt == 0) {
            apply_reusable_drops();
            if (auto it = g_reusable.find(pc); it != g_reusable.end() && same_words(it->second.reads)) {
                source.reset();
                source.assign_reads(it->second.reads);
                TickScope t{g_counters.install_ticks};
                if (const void* host = install(it->second.region, pc, 0, &source)) {
                    count(g_counters.jit_reuses);
                    return host;
                }
            }
        }
        std::int64_t copy_delta = 0;
        const bool copy = registered && cache && guest_code_copy_delta(pc, copy_delta);
        if (copy && attempt == 0)
            if (const void* host = install_from_copy_source(pc, copy_delta, source, false)) return host;
        source.reset();
        std::uint32_t word;
        if (!source.read(pc, word)) return nullptr;
        try {
            TickScope t{g_counters.jit_ticks};
            translate_region(pc, source, topts, region);
        } catch (const std::runtime_error&) {
            return nullptr;
        }
        if (!region_is_valid(region, pc)) {
            std::fprintf(stderr, "fx runtime: translate_region(0x%llx) returned an invalid region\n",
                         static_cast<unsigned long long>(pc));
            fatal("fx runtime: invalid translated region");
        }
        count(g_counters.translations);
        count(g_counters.jit_guest_instructions, region.stats.guest_instructions);
        if (registered) count(g_counters.registered_code_translations);
        if (copy) count(g_counters.copy_translations);
        const void* installed;
        {
            TickScope t{g_counters.install_ticks};
            installed = install(region, pc, 0, &source);
        }
        if (installed) {
            if (cache) store_translation(region, copy, copy_delta);
            if (registered) keep_reusable(pc, region, source);
            return installed;
        }
    }
    // The guest keeps rewriting this code while we translate it: execute the
    // instruction in the interpreter instead (ctx->pc == pc on this path) --
    // except exclusives and LSE atomics, whose monitor/atomicity the
    // interpreter cannot provide against translated code: keep translating.
    std::uint32_t word = 0;
    if (safe_read32(pc, word) && is_exclusive_or_atomic(word)) {
        for (;;) {
            host::yield_thread();
            if (const void* host = find_entry(pc)) return host;
            LiveCodeSource source;
            if (!source.read(pc, word)) return nullptr;
            TranslatedRegion region;
            try {
                region = translate_region(pc, source, topts);
            } catch (const std::runtime_error&) {
                return nullptr;
            }
            count(g_counters.translations);
            if (region_is_valid(region, pc))
                if (const void* host = install(region, pc, 0, &source)) return host;
        }
    }
    return g_stubs.interpret_pc;
}

void tier_up(std::uint64_t pc) {
    TickScope total{g_counters.translate_ticks};
    std::lock_guard serialize(g.translate);
    const auto pending = [&] {
        SharedLock lock(g.map);
        const MapEntry* entry = g.entries.find(pc);
        return entry && replaceable(*entry, 1);
    };
    if (!pending()) return;   // another thread tiered it up (or it was invalidated)
    TranslateOptions topts = jit_translate_options();
    topts.lazy = false;
    const bool cache = options().enable_translation_cache;
    if (cache) {
        // A tier-1 record from an earlier run (or a tier-1 region covering pc).
        thread_local TranslatedRegion cached;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto epoch = g.epoch.load(std::memory_order_acquire);
            bool hit;
            {
                TickScope t{g_counters.cache_ticks};
                hit = cache_lookup(pc, cached) && cached.tier >= 1 && region_is_valid(cached, pc);
            }
            if (!hit) break;
            count(g_counters.cache_hits);
            bool recheck = false;
            {
                TickScope t{g_counters.install_ticks};
                install(cached, pc, epoch, nullptr, &recheck);
            }
            if (!pending()) {
                count(g_counters.tier_ups);
                count(g_counters.tier_up_cached);
                return;
            }
            if (!recheck) break;
        }
    }
    std::int64_t copy_delta = 0;
    const bool copy = cache && in_registered_code_memory(pc) && guest_code_copy_delta(pc, copy_delta);
    if (copy) {
        thread_local LiveCodeSource copy_source;
        install_from_copy_source(pc, copy_delta, copy_source, true);
        if (!pending()) {
            count(g_counters.tier_ups);
            count(g_counters.tier_up_cached);
            return;
        }
    }
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (attempt && !pending()) return;
        thread_local LiveCodeSource source;
        source.reset();
        source.host_entries_only = true;
        thread_local TranslatedRegion region;
        try {
            TickScope t{g_counters.jit_ticks};
            TickScope u{g_counters.tier_up_ticks};
            translate_region(pc, source, topts, region);
        } catch (const std::runtime_error&) {
            return;
        }
        if (!region_is_valid(region, pc)) {
            std::fprintf(stderr, "fx runtime: translate_region(0x%llx) returned an invalid region\n",
                         static_cast<unsigned long long>(pc));
            fatal("fx runtime: invalid translated region");
        }
        count(g_counters.translations);
        count(g_counters.jit_guest_instructions, region.stats.guest_instructions);
        count(g_counters.tier_up_instructions, region.stats.guest_instructions);
        {
            TickScope t{g_counters.install_ticks};
            install(region, pc, 0, &source);
        }
        if (!pending()) {
            count(g_counters.tier_ups);
            if (cache) store_translation(region, copy, copy_delta);
            return;
        }
    }
}

// ---- preloading ---------------------------------------------------------------
// Once a module's main cache file is open (cache_set_module_ready_handler),
// background threads install its cached regions ahead of the guest, so guest
// threads find them in the lookup table, already linked, instead of decoding
// and installing them on their own critical path. Preloaded regions are
// installed exactly like cached regions found on a miss (validated by the
// cache, epoch-checked against invalidation); a region the guest never runs
// costs only arena space.
//
// Order: records are stored in first-execution order. Work is handed out in
// chunks, round robin over the loaded modules (earlier-loaded first), so the
// start of every module comes early. When a guest thread still has to
// install a cached region itself, the preloader is behind: that thread leaves
// a hint and the next chunk continues from the hinted record, just ahead of
// the guest; the skipped records are done later.
namespace {

struct PreloadRange {
    std::uint64_t begin, end;   // cache cursors [begin, end)
};
struct PreloadModule {
    std::uint64_t base = 0;
    std::uint64_t sequence = 0;
    std::vector<PreloadRange> ranges;   // not yet handed out; back() goes next
    unsigned chunks = 0;                // chunks handed out (round robin)
};
struct Preload {
    host::RwLock lock;
    host::CondVar wake;
    std::vector<PreloadModule> modules;
    unsigned threads = 0;            // started
    unsigned busy = 0;               // threads working on a chunk
    unsigned max_threads = 0;        // 0 = disabled
    bool stopping = false;
};
Preload g_preload;
std::atomic<bool> g_preload_stop{false};
std::atomic<std::uint64_t> g_preload_hint{0};   // last pc a guest thread installed from the cache itself
constexpr std::size_t kPreloadChunk = 32;

// g_preload.lock held. Splits the range of `m` holding `cursor` so that the
// part from `cursor` is handed out next. False if no range holds it.
bool prefer_cursor_locked(PreloadModule& m, std::uint64_t cursor) {
    for (std::size_t i = m.ranges.size(); i-- > 0;) {
        PreloadRange r = m.ranges[i];
        if (cursor < r.begin || cursor >= r.end) continue;
        m.ranges.erase(m.ranges.begin() + static_cast<std::ptrdiff_t>(i));
        if (r.begin < cursor) m.ranges.push_back({r.begin, cursor});
        m.ranges.push_back({cursor, r.end});
        return true;
    }
    return false;
}

void preload_chunk(std::uint64_t base, std::uint64_t& cursor, std::uint64_t end, TranslatedRegion& region,
                   std::size_t& found) {
    std::uint64_t pcs[kPreloadChunk];
    found = cache_region_starts(base, cursor, end, pcs, kPreloadChunk);
    for (std::size_t i = 0; i < found; ++i) {
        if (g_preload_stop.load(std::memory_order_relaxed)) return;
        const std::uint64_t pc = pcs[i];
        for (int attempt = 0; attempt < 2; ++attempt) {
            // Lock-free: install's check under the map lock is authoritative.
            if (lookup_probe(pc)) break;
            const auto epoch = g.epoch.load(std::memory_order_acquire);
            if (!cache_lookup(pc, region) || !region_is_valid(region, pc)) break;
            bool recheck = false;
            if (install(region, pc, epoch, nullptr, &recheck)) {
                count(g_counters.preloaded);
                break;
            }
            if (!recheck) break;   // a page was just write-protected: hash the guest bytes again
        }
    }
}

std::uint32_t preload_main(void*) {
    t_preloader = true;
    profiler_note_background_thread();
    // Normal QoS until the queue first drains: during startup the guest
    // consumes cached regions faster than EcoQoS preloaders (efficiency
    // cores, low clocks) install them, and a preloader preempted inside the
    // map lock stalls the guest threads that wait for it.
    bool background = false;
    TranslatedRegion region;   // reused: rebuilding a cached region allocates nothing
    for (;;) {
        // Resolve a hint outside the queue lock (it reads cache state).
        std::uint64_t hint_base = 0, hint_cursor = kCacheCursorEnd;
        if (const std::uint64_t hint = g_preload_hint.exchange(0, std::memory_order_relaxed)) {
            if (const GuestModule* module = find_guest_module(hint)) {
                hint_base = module->base;
                hint_cursor = cache_region_cursor(hint);
            }
        }
        std::uint64_t base = 0, sequence = 0;
        PreloadRange job{};
        {
            ExclusiveLock lock(g_preload.lock);
            for (;;) {
                if (g_preload.stopping) return 0;
                PreloadModule* pick = nullptr;
                if (hint_cursor != kCacheCursorEnd) {
                    for (auto& m : g_preload.modules)
                        if (m.base == hint_base && prefer_cursor_locked(m, hint_cursor)) pick = &m;
                    hint_cursor = kCacheCursorEnd;
                }
                if (!pick) {
                    for (auto& m : g_preload.modules) {
                        if (m.ranges.empty()) continue;
                        if (!pick || m.chunks < pick->chunks || (m.chunks == pick->chunks && m.sequence < pick->sequence))
                            pick = &m;
                    }
                }
                if (pick) {
                    base = pick->base;
                    sequence = pick->sequence;
                    job = pick->ranges.back();
                    pick->ranges.pop_back();
                    ++pick->chunks;
                    ++g_preload.busy;
                    break;
                }
                // Nothing queued: the startup burst is over. From now on this
                // thread leaves performance cores to the guest (background QoS).
                if (!background) {
                    background = true;
                    set_background_thread_qos(host::current_thread());
                }
                g_preload.wake.wait_for(g_preload.lock, host::kWaitForever);
            }
        }
        std::uint64_t cursor = job.begin;
        std::size_t found = 0;
        const std::uint64_t chunk_start = ticks_now();
        try {
            preload_chunk(base, cursor, job.end, region, found);
        } catch (const std::exception&) {
            found = 0;   // arena exhausted or similar: guest threads report it if it matters
        }
        const std::uint64_t chunk_end = ticks_now();
        count(g_counters.preload_ticks, chunk_end - chunk_start);
        {
            ExclusiveLock lock(g_preload.lock);
            --g_preload.busy;
            if (found != 0 && cursor < job.end) {
                // The rest of the range continues next (unless another module's turn).
                for (auto& m : g_preload.modules)
                    if (m.base == base && m.sequence == sequence) m.ranges.push_back({cursor, job.end});
            }
            bool drained = g_preload.busy == 0;
            for (const auto& m : g_preload.modules) drained = drained && m.ranges.empty();
            if (drained) g_counters.preload_drained_ticks.store(chunk_end, std::memory_order_relaxed);
        }
        g_preload.wake.notify_all();
    }
}

void module_ready(std::uint64_t base, std::uint64_t sequence) {
    // Never touches the queue lock once stopping: a preloader that ExitProcess
    // terminated may have died holding it (modules can still load during
    // guest DLL_PROCESS_DETACH).
    if (g_preload_stop.load(std::memory_order_acquire)) return;
    bool start = false;
    {
        ExclusiveLock lock(g_preload.lock);
        if (g_preload.stopping || g_preload.max_threads == 0) return;
        // A module loaded again at the same base replaces the old entry.
        auto& modules = g_preload.modules;
        modules.erase(std::remove_if(modules.begin(), modules.end(),
                                     [&](const PreloadModule& m) { return m.base == base; }),
                      modules.end());
        PreloadModule m;
        m.base = base;
        m.sequence = sequence;
        m.ranges.push_back({0, kCacheCursorEnd});
        modules.push_back(std::move(m));
        if (g_preload.threads < g_preload.max_threads) {
            ++g_preload.threads;
            start = true;
        }
    }
    if (start) {
        // Never waited for: safe even under the host loader lock.
        if (host::ThreadHandle thread = host::create_thread(preload_main, nullptr, 256 * 1024,
                                                            host::kThreadStackReservation, nullptr)) {
            host::close_thread(thread);   // background QoS once the queue drains (preload_main)
        } else {
            ExclusiveLock lock(g_preload.lock);
            --g_preload.threads;
        }
    }
    g_preload.wake.notify_all();
}

} // namespace

void preload_hint(std::uint64_t pc) noexcept { g_preload_hint.store(pc, std::memory_order_relaxed); }

unsigned preload_threads(const RuntimeOptions& runtime, const char* setting, unsigned hardware) {
    if (!runtime.enable_translation_cache) return 0;   // nothing to preload from
    // TRANSMUTE_FX_PRELOAD=<threads> decides either way, so a host that leaves
    // preloading off (Alchemy) can still turn it on for one run.
    if (setting && *setting) return static_cast<unsigned>(std::min<unsigned long>(std::strtoul(setting, nullptr, 10), 8));
    // A few threads keep well ahead of the guest; leave most cores to it.
    return runtime.preload_cached_regions ? std::clamp(std::max(1u, hardware) / 4, 1u, 3u) : 0;
}

void preload_initialize() {
    char value[16] = {};
    const bool set = host::get_env("TRANSMUTE_FX_PRELOAD", value, sizeof(value)) > 0;
    const unsigned threads = preload_threads(options(), set ? value : nullptr, std::thread::hardware_concurrency());
    {
        ExclusiveLock lock(g_preload.lock);
        g_preload.max_threads = threads;
    }
    if (threads) cache_set_module_ready_handler(&module_ready);
}

bool preload_stop(unsigned wait_ms) {
    g_preload_stop.store(true, std::memory_order_seq_cst);
    // Without waiting (other threads are already gone), only look: the queue
    // lock may be orphaned.
    if (wait_ms == 0) {
        if (!g_preload.lock.try_lock()) return false;
    } else {
        g_preload.lock.lock();
    }
    g_preload.stopping = true;
    g_preload.modules.clear();
    g_preload.wake.notify_all();
    // A preloader stops between two regions; wait (bounded) until none is
    // inside the region map, the arena or a cache lookup, so terminating it
    // (ExitProcess) cannot leave one of their locks held.
    const std::uint64_t deadline = host::monotonic_ms() + wait_ms;
    while (g_preload.busy != 0 && host::monotonic_ms() < deadline) g_preload.wake.wait_for(g_preload.lock, 10);
    const bool idle = g_preload.busy == 0;
    g_preload.lock.unlock();
    return idle;
}

// ---- arena reclaim ---------------------------------------------------------------
// Code chunks (arena.cpp) whose regions are dead are freed and reused.
// A pass, on the reclaimer thread (a runtime background thread, never a
// guest thread):
//  1. Under the exclusive map lock, picks the victims: code chunks that are
//     not in use for allocation and mostly dead (below). When free chunks
//     run low (urgent_free), also chunks with the fewest live bytes until
//     target_free chunks would be free; their live regions are killed like
//     an invalidation kills them (evicted: the guest code is unchanged, and
//     a cached region is installed again from the cache on its next miss).
//  2. After that nothing in the runtime reaches a victim: the map, the lookup
//     table, links, pending exits and page lists dropped its regions. Only
//     what a thread already holds can: its registers, its host stack (return
//     addresses of helpers called from translated code, the dispatcher's
//     next entry, exception records of a fault in translated code) or its
//     context (RSB entries that dead code pushed after the kill scrub, a link
//     exit's field). So every guest thread is suspended and those words are
//     scanned (scan_guest_threads); RSB entries into victims are repointed to
//     rsb_redispatch meanwhile. A word inside a victim pins it.
//  3. Unpinned victims are freed (decommitted; their regions' metadata goes
//     with them). Pinned ones stay dead and are tried again by a later pass:
//     the scan is conservative, so a stale stack word only delays reuse.
// Victims are also chunks that are mostly dead (at most a quarter of their
// code still live): a guest JIT leaves long-lived code (copied builtins, hot
// functions) among the code it discarded, so few chunks die entirely; their
// live regions are evicted, to be found again in the JIT's kept translations
// or the cache.
// A pass is asked for whenever a code chunk is taken (reclaim_notify; it runs
// only if such chunks add up to kSoftBytes or an eighth of the code in use),
// and waited for when none is free (reclaim_wait, with eviction). Free means
// within the arena budget (arena_free_chunks).
namespace {

// Free chunks (of the arena budget) below which a pass evicts, and the free
// chunks it evicts for; dead chunks that make a pass worth it without need.
std::size_t urgent_free() noexcept { return std::max<std::size_t>(arena_budget() / 8, 1); }
std::size_t target_free() noexcept { return std::max<std::size_t>(arena_budget() / 4, 2); }
constexpr std::size_t kSoftBytes = std::size_t{16} << 20;

// Map lock held: a chunk reclaim takes without need (all, or all but a
// quarter of its code dead).
bool mostly_dead(const ChunkSummary& c) {
    const ArenaChunk& chunk = arena_chunk(c.index);
    return !c.busy && chunk.live_bytes * 4 <= c.used;
}

struct ReclaimState {
    host::RwLock lock;
    host::CondVar wake;        // the reclaimer waits for requests
    host::CondVar done;        // reclaim_run waits for passes
    bool started = false;
    bool stopping = false;
    bool busy = false;         // between taking a request and finishing its pass
    bool requested = false;
    bool forced = false;       // run even if few chunks are dead
    bool evict = false;
    std::uint64_t passes = 0;  // requests handled
    ReclaimResult last{};
};
ReclaimState g_reclaim;
std::atomic<bool> g_reclaim_stop{false};
std::mutex g_reclaim_pass;

// Scan state of the pass in progress (g_reclaim_pass held); static, so the
// scan callbacks reach it without allocating.
struct PassScan {
    std::uint64_t base = 0;
    std::uint32_t victim_head[kArenaChunks];   // the victim run holding the chunk, or ~0u
    std::uint8_t pinned[kArenaChunks];         // by head
    const void* redispatch = nullptr;
    // TRANSMUTE_FX_RECLAIM_TRACE: where the first pins were found.
    std::uint32_t thread_id = 0;
    std::uint64_t sp = 0;
    std::uint64_t ordinal = 0;    // word of the thread: registers first (kRegisterWords), then the stack
    std::uint32_t context = 0;    // 1 while scanning a context
    struct Pin {
        std::uint32_t thread_id, context;
        std::uint64_t ordinal, sp, value;
    } pins[16];
    unsigned pin_count = 0;
};
PassScan g_pass;
constexpr std::uint64_t kRegisterWords = 49;   // host::sample::capture_words on win32

bool reclaim_trace() {
    static const bool enabled = [] {
        char value[8] = {};
        return host::get_env("TRANSMUTE_FX_RECLAIM_TRACE", value, sizeof(value)) > 0 && value[0] != '0';
    }();
    return enabled;
}

void pass_thread(void* user, std::uint32_t thread_id, std::uint64_t sp) {
    auto& s = *static_cast<PassScan*>(user);
    s.thread_id = thread_id;
    s.sp = sp;
    s.ordinal = 0;
}

void pass_word(void* user, std::uint64_t value) {
    auto& s = *static_cast<PassScan*>(user);
    const std::uint64_t ordinal = s.ordinal++;
    const std::uint64_t offset = value - s.base;
    if (offset >= kArenaReserve) return;
    const std::uint32_t head = s.victim_head[offset / kArenaChunk];
    if (head == ~0u) return;
    s.pinned[head] = 1;
    if (s.pin_count < std::size(s.pins)) s.pins[s.pin_count++] = {s.thread_id, s.context, ordinal, s.sp, value};
}

void pass_context(void* user, GuestContext& ctx) {
    auto& s = *static_cast<PassScan*>(user);
    s.context = 1;
    s.ordinal = 0;
    for (auto& entry : ctx.rsb) {
        const std::uint64_t offset = reinterpret_cast<std::uint64_t>(entry.host_entry) - s.base;
        if (offset < kArenaReserve && s.victim_head[offset / kArenaChunk] != ~0u) entry.host_entry = s.redispatch;
    }
    const auto* words = reinterpret_cast<const std::uint64_t*>(&ctx);
    constexpr std::size_t rsb_begin = offsetof(GuestContext, rsb) / 8;
    constexpr std::size_t rsb_end = rsb_begin + sizeof(GuestContext::rsb) / 8;
    for (std::size_t i = 0; i < sizeof(GuestContext) / 8; ++i) {
        s.ordinal = i;
        if (i < rsb_begin || i >= rsb_end) pass_word(user, words[i]);
    }
    s.context = 0;
}

// Map lock held (shared is enough): code bytes in mostly dead chunks, and in
// all code chunks.
void reclaimable_bytes(std::size_t& reclaimable, std::size_t& used) {
    thread_local std::vector<ChunkSummary> chunks;
    arena_code_chunks(chunks);
    reclaimable = used = 0;
    for (const ChunkSummary& c : chunks) {
        used += c.used;
        if (mostly_dead(c)) reclaimable += c.used;
    }
}

ReclaimResult reclaim_pass(bool evict) {
    std::lock_guard serialize(g_reclaim_pass);
    const std::uint64_t start = ticks_now();
    ReclaimResult result;
    TimedExclusive lock(g.map);
    static std::vector<ChunkSummary> chunks, order;
    static std::vector<std::uint32_t> victims;
    arena_code_chunks(chunks);
    const std::size_t free = arena_free_chunks();
    victims.clear();
    std::size_t victim_chunks = 0;
    for (const ChunkSummary& c : chunks) {
        if (!mostly_dead(c)) continue;
        for (Region* r : arena_chunk(c.index).regions) {
            if (r->dead) continue;
            kill(*r, Kill::evicted);
            ++result.evicted;
        }
        victims.push_back(c.index);
        victim_chunks += c.span;
    }
    if (evict && free + victim_chunks < target_free()) {
        order.clear();
        for (const ChunkSummary& c : chunks)
            if (!c.busy && arena_chunk(c.index).live != 0) order.push_back(c);
        std::sort(order.begin(), order.end(), [](const ChunkSummary& a, const ChunkSummary& b) {
            const auto la = arena_chunk(a.index).live_bytes, lb = arena_chunk(b.index).live_bytes;
            return la != lb ? la < lb : a.sequence < b.sequence;
        });
        for (const ChunkSummary& c : order) {
            if (free + victim_chunks >= target_free()) break;
            for (Region* r : arena_chunk(c.index).regions) {
                if (r->dead) continue;
                kill(*r, Kill::evicted);
                ++result.evicted;
            }
            victims.push_back(c.index);
            victim_chunks += c.span;
        }
    }
    if (result.evicted) {
        // Threads still inside evicted code leave it at their next safe point.
        for_each_context([](GuestContext& ctx) { ctx.interrupt_pending = 1; });
    }
    result.victims = victims.size();
    if (!victims.empty()) {
        g_pass.base = reinterpret_cast<std::uint64_t>(arena_base());
        g_pass.redispatch = g_stubs.rsb_redispatch;
        std::fill(std::begin(g_pass.victim_head), std::end(g_pass.victim_head), ~0u);
        for (const std::uint32_t v : victims) {
            g_pass.pinned[v] = 0;
            for (std::uint32_t i = 0; i < arena_chunk(v).span; ++i) g_pass.victim_head[v + i] = v;
        }
        g_pass.pin_count = 0;
        const std::uint64_t scan_start = ticks_now();
        result.scanned = scan_guest_threads({&pass_thread, &pass_word, &pass_context, &g_pass});
        count(g_counters.reclaim_scan_ticks, ticks_now() - scan_start);
        for (const std::uint32_t v : victims) {
            ArenaChunk& chunk = arena_chunk(v);
            if (!result.scanned || g_pass.pinned[v]) {
                ++result.pinned;
                continue;
            }
            chunk.regions.clear();
            chunk.regions.shrink_to_fit();
            chunk.live_bytes = 0;
            const std::uint32_t span = chunk.span;
            arena_free_chunk(v);
            result.freed += span;
        }
        if (!result.scanned) count(g_counters.reclaim_scan_failures);
    }
    count(g_counters.reclaim_passes);
    count(g_counters.reclaimed_chunks, result.freed);
    count(g_counters.reclaim_pinned, result.pinned);
    count(g_counters.reclaim_ticks, ticks_now() - start);
    if (reclaim_trace()) {
        std::fprintf(stderr, "fx reclaim: %zu victims, %zu chunks freed, %zu pinned, %zu evicted, %zu free of %zu, %.2f ms%s\n",
                     result.victims, result.freed, result.pinned, result.evicted, arena_free_chunks(), arena_budget(),
                     static_cast<double>(ticks_now() - start) * 1000.0 / static_cast<double>(host::tick_frequency()),
                     result.scanned || victims.empty() ? "" : " (thread scan failed)");
        for (unsigned i = 0; i < g_pass.pin_count; ++i) {
            const auto& p = g_pass.pins[i];
            if (p.context)
                std::fprintf(stderr, "  pin 0x%llx: thread %u context word %llu\n", static_cast<unsigned long long>(p.value),
                             p.thread_id, static_cast<unsigned long long>(p.ordinal));
            else if (p.ordinal < kRegisterWords)
                std::fprintf(stderr, "  pin 0x%llx: thread %u register %llu\n", static_cast<unsigned long long>(p.value),
                             p.thread_id, static_cast<unsigned long long>(p.ordinal));
            else
                std::fprintf(stderr, "  pin 0x%llx: thread %u stack sp+0x%llx\n", static_cast<unsigned long long>(p.value),
                             p.thread_id, static_cast<unsigned long long>((p.ordinal - kRegisterWords) * 8));
        }
    }
    return result;
}

std::uint32_t reclaim_main(void*) {
    profiler_note_background_thread();
    for (;;) {
        bool forced, evict;
        {
            ExclusiveLock lock(g_reclaim.lock);
            while (!g_reclaim.requested && !g_reclaim.stopping) g_reclaim.wake.wait_for(g_reclaim.lock, host::kWaitForever);
            if (g_reclaim.stopping) {
                g_reclaim.started = false;
                return 0;
            }
            forced = g_reclaim.forced;
            evict = g_reclaim.evict;
            g_reclaim.requested = g_reclaim.forced = g_reclaim.evict = false;
            g_reclaim.busy = true;
        }
        ReclaimResult result{};
        try {
            bool run = forced;
            if (!run) {
                std::size_t reclaimable = 0, used = 0;
                {
                    SharedLock lock(g.map);
                    reclaimable_bytes(reclaimable, used);
                }
                run = reclaimable >= std::max(kSoftBytes, used / 8);
            }
            if (run) result = reclaim_pass(evict || arena_free_chunks() < urgent_free());
        } catch (const std::exception&) {
            // bad_alloc growing a list: the next pass tries again
        }
        {
            ExclusiveLock lock(g_reclaim.lock);
            g_reclaim.busy = false;
            g_reclaim.last = result;
            ++g_reclaim.passes;
        }
        g_reclaim.done.notify_all();
    }
}

// g_reclaim.lock held. Never waited for: safe even under the host loader lock.
bool start_reclaimer_locked() {
    if (g_reclaim.started) return true;
    host::ThreadHandle thread = host::create_thread(reclaim_main, nullptr, 256 * 1024, host::kThreadStackReservation, nullptr);
    if (!thread) return false;
    host::close_thread(thread);
    g_reclaim.started = true;
    return true;
}

} // namespace

void reclaim_notify(std::size_t free_chunks) noexcept {
    if (g_reclaim_stop.load(std::memory_order_acquire)) return;
    {
        ExclusiveLock lock(g_reclaim.lock);
        if (g_reclaim.stopping || !start_reclaimer_locked()) return;
        g_reclaim.requested = true;
        if (free_chunks < urgent_free()) g_reclaim.forced = g_reclaim.evict = true;
    }
    g_reclaim.wake.notify_all();
}

bool reclaim_run(bool evict, ReclaimResult* result) {
    if (g_reclaim_stop.load(std::memory_order_acquire)) return false;
    clear_stack_residue();   // the wait's frames go there, and this thread is scanned while it waits
    ExclusiveLock lock(g_reclaim.lock);
    if (g_reclaim.stopping || !start_reclaimer_locked()) return false;
    // A pass already running may have picked its victims before this request.
    const std::uint64_t target = g_reclaim.passes + (g_reclaim.busy ? 2 : 1);
    g_reclaim.requested = g_reclaim.forced = true;
    g_reclaim.evict = g_reclaim.evict || evict;
    g_reclaim.wake.notify_all();
    // Bounded: a reclaimer that cannot start (a new thread waits for the host
    // loader lock, which this thread may hold) must not hang the caller.
    const std::uint64_t deadline = host::monotonic_ms() + 30000;
    while (g_reclaim.passes < target) {
        if (g_reclaim.stopping || host::monotonic_ms() >= deadline) return false;
        g_reclaim.done.wait_for(g_reclaim.lock, 100);
    }
    if (result) *result = g_reclaim.last;
    return true;
}

bool reclaim_wait() { return reclaim_run(true, nullptr); }

bool reclaim_stop(unsigned wait_ms) {
    g_reclaim_stop.store(true, std::memory_order_seq_cst);
    if (wait_ms == 0) {
        if (!g_reclaim.lock.try_lock()) return false;
    } else {
        g_reclaim.lock.lock();
    }
    g_reclaim.stopping = true;
    g_reclaim.wake.notify_all();
    g_reclaim.done.notify_all();
    // A pass holds the map lock and suspends guest threads: let it finish, so
    // terminating the reclaimer (ExitProcess) cannot leave them so.
    const std::uint64_t deadline = host::monotonic_ms() + wait_ms;
    while (g_reclaim.busy && host::monotonic_ms() < deadline) g_reclaim.done.wait_for(g_reclaim.lock, 10);
    const bool idle = !g_reclaim.busy;
    g_reclaim.lock.unlock();
    return idle;
}

void reclaim_resume_for_tests() {
    ExclusiveLock lock(g_reclaim.lock);
    while (g_reclaim.started && g_reclaim.stopping) {   // a stopped reclaimer is leaving
        g_reclaim.wake.notify_all();
        g_reclaim.done.wait_for(g_reclaim.lock, 1);
    }
    g_reclaim.stopping = false;
    g_reclaim_stop.store(false, std::memory_order_release);
}

bool region_locks_free() noexcept {
    if (!g.map.try_lock()) return false;
    g.map.unlock();
    if (!g.translate.try_lock()) return false;
    g.translate.unlock();
    return true;
}

std::size_t live_regions_on_page(std::uint64_t address) {
    SharedLock lock(g.map);
    return g.pages.count(address & ~(kPage - 1));
}

void link_exit(std::uint64_t field, std::uint64_t target_pc, const void* host) {
    ExclusiveLock lock(g.map);
    Region* r = region_at(field);
    if (!r || r->dead) return;
    std::uint32_t index;
    ExitInfo* e = find_exit(*r, static_cast<std::uint32_t>(field - reinterpret_cast<std::uint64_t>(r->code)), index);
    if (!e || e->linked || e->target_pc != target_pc) return;
    const MapEntry* entry = g.entries.find(target_pc);
    if (!entry || entry->host != host) return;
    forget_pending(target_pc, r, index);   // an unlinked exit of a live region is pending
    patch_link(*r, index, *entry);
}

std::size_t exit_refs_for_tests(std::size_t* linked) {
    SharedLock lock(g.map);
    if (linked) *linked = g.links.size();
    return g.pending.size();
}

void set_install_hook_for_tests(void (*hook)(std::uint64_t pc)) { g_install_hook.store(hook); }

namespace {
// Host entries (thunks, callbacks, trampolines) expect the flushed state, but
// translated code reaches them with the convention registers live: they are
// entered through an adapter that stores them (one per host address; map
// lock held). Stubs in the stub area handle the convention themselves.
std::unordered_map<const void*, const void*> g_adapters;
std::unordered_set<const void*> g_adapter_code;

const void* host_entry_adapter(const void* host) {
    if (translator_register_convention().guest_mask == 0) return host;
    const auto address = reinterpret_cast<std::uint64_t>(host);
    const auto stubs = reinterpret_cast<std::uint64_t>(arena_base());
    if (address - stubs < kStubAreaSize || g_adapter_code.count(host)) return host;
    if (const auto it = g_adapters.find(host); it != g_adapters.end()) return it->second;
    x64::Assembler a(96);
    store_globals(a);
    const x64::Label data = a.new_label();
    a.jmp(x64::rip(data));
    a.align(8);
    a.bind(data);
    a.dq(address);
    a.finalize();
    void* code = arena_allocate(a.size(), 16);
    std::memcpy(code, a.data(), a.size());
    g_adapters.emplace(host, code);
    g_adapter_code.insert(code);
    return code;
}
} // namespace

void install_host_entry(std::uint64_t pc, const void* host) {
    drop_reusable(0, 0, true);
    ExclusiveLock lock(g.map);
    host = host_entry_adapter(host);
    if (const MapEntry* entry = g.entries.find(pc)) {
        lookup_remove(pc, entry->host);
        g.entries.erase(pc);
        unlink_target(pc);
    }
    g.entries.insert(pc, MapEntry{host, nullptr});
    lookup_install(pc, host);
    link_pending(pc, MapEntry{host, nullptr});
}

bool remove_host_entry(std::uint64_t pc) {
    drop_reusable(0, 0, true);
    ExclusiveLock lock(g.map);
    const MapEntry* entry = g.entries.find(pc);
    if (!entry || entry->region) return false;   // none, or a translation (invalidate those)
    lookup_remove(pc, entry->host);
    g.entries.erase(pc);
    unlink_target(pc);
    return true;
}

bool find_fault_site(std::uint64_t rip, FaultInfo& info) {
    SharedLock lock(g.map);
    Region* r = region_at(rip);
    if (!r) return false;
    const auto offset = static_cast<std::uint32_t>(rip - reinterpret_cast<std::uint64_t>(r->code));
    auto it = std::upper_bound(r->faults.begin(), r->faults.end(), offset,
                               [](std::uint32_t off, const FaultSite& f) { return off < f.host_begin; });
    if (it == r->faults.begin()) return false;
    --it;
    if (offset < it->host_begin || offset >= it->host_end) return false;
    info.site = *it;
    info.pins.assign(r->pins.begin(), r->pins.end());
    info.map.clear();
    if (it->map != 0 && it->map <= r->fault_maps.size()) {
        const FaultMap& m = r->fault_maps[it->map - 1u];
        if (m.begin <= r->fault_map_regs.size() && m.count <= r->fault_map_regs.size() - m.begin)
            info.map.assign(r->fault_map_regs.begin() + m.begin, r->fault_map_regs.begin() + m.begin + m.count);
    }
    info.site.map = info.map.empty() ? 0 : 1;
    return true;
}

bool sample_region(std::uint64_t rip, RegionSample& sample) {
    SharedLock lock(g.map);
    Region* r = region_at(rip);
    if (!r) return false;
    const auto offset = static_cast<std::uint32_t>(rip - reinterpret_cast<std::uint64_t>(r->code));
    sample.guest_start = r->guest_start;
    sample.code = reinterpret_cast<std::uint64_t>(r->code);
    sample.size = r->size;
    sample.hot_size = r->hot_size ? r->hot_size : r->size;
    sample.offset = offset;
    sample.guest_pc = r->guest_start;
    auto it = std::upper_bound(r->faults.begin(), r->faults.end(), offset,
                               [](std::uint32_t off, const FaultSite& f) { return off < f.host_begin; });
    if (it != r->faults.begin()) sample.guest_pc = std::prev(it)->guest_pc;
    return true;
}

bool region_fault_sites(std::uint64_t code, std::vector<FaultSite>& faults) {
    SharedLock lock(g.map);
    Region* r = region_at(code);
    if (!r) return false;
    faults.assign(r->faults.begin(), r->faults.end());
    return true;
}

bool region_guest_ranges(std::uint64_t code, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    SharedLock lock(g.map);
    Region* r = region_at(code);
    if (!r) return false;
    ranges.assign(r->guest_ranges.begin(), r->guest_ranges.end());
    return true;
}

std::uint32_t saved_page_protection(std::uint64_t page) noexcept {
    SharedLock lock(g.map);
    if (g.protected_pages.empty()) return 0;
    const auto it = g.protected_pages.find(page & ~(kPage - 1));
    return it == g.protected_pages.end() ? 0 : it->second;
}

// ---- JIT write windows ----------------------------------------------------------
// Darwin's MAP_JIT contract: a thread writes JIT memory only between
// pthread_jit_write_protect_np(0) and (1) (on Apple silicon it cannot even
// store to it otherwise), and code it wrote need not be visible to any thread
// before sys_icache_invalidate. V8 writes each new code object next to live
// code; on the page-granular SMC fault that killed every translation on the
// page, interrupted every thread and had most of them installed again with
// the same bytes. Inside a window the fault now only gives the page back to
// the guest (deferred). Flushes still kill what they cover at once. When any
// window closes, the deferred pages are write-protected again and every
// translation on them is checked against the hash of the guest bytes it was
// translated from: unchanged ones stay, changed ones die. Faults outside a
// window, on image code or on shared memory keep the exact path.
namespace {
thread_local bool t_jit_window = false;
std::atomic<bool> g_deferred_any{false};   // !g.deferred_pages.empty(), read without the lock

// Map lock held exclusively; `page` is in g.protected_pages. False if the
// fault must take the exact path.
bool defer_page(std::uint64_t page) {
    if (!in_registered_code_memory(page) || !in_registered_code_memory(page + kPage - 1)) return false;
    if (g_shared_aliases.load(std::memory_order_acquire)) {
        std::vector<std::uint64_t> aliases;
        shared_memory_aliases(page, aliases);
        if (!aliases.empty()) return false;
    }
    const auto it = g.protected_pages.find(page);
    const vm::Protection now = current_protection(page);
    if (now != 0 && (now & 0xff) == (read_only_for(it->second) & 0xff))
        vm::protect(reinterpret_cast<void*>(page), kPage, it->second);
    g.protected_pages.erase(it);
    ++g.protection_changes;
    g.deferred_pages.insert(page);
    g_deferred_any.store(true, std::memory_order_release);
    // Cached installs validate a guest hash taken before now with the epoch.
    g.epoch.fetch_add(1, std::memory_order_acq_rel);
    count(g_counters.jit_window_deferred);
    return true;
}

// Map lock held exclusively: the check of every deferred page (see above).
void check_deferred_pages() {
    if (g.deferred_pages.empty()) return;
    std::vector<std::uint64_t> pages(g.deferred_pages.begin(), g.deferred_pages.end());
    g.deferred_pages.clear();
    g_deferred_any.store(false, std::memory_order_release);
    std::vector<Region*> victims;
    std::unordered_set<Region*> seen;
    for (const std::uint64_t page : pages) {
        if (!g.pages.any(page)) continue;
        count(g_counters.jit_window_pages_checked);
        // Protect first: a store after this faults and waits for the lock.
        protect_page(page);
        g.pages.for_each(page, [&](Region* r) {
            if (!seen.insert(r).second) return;
            if (r->guest_hash != 0 && guest_bytes_hash(*r) == r->guest_hash) {
                count(g_counters.jit_window_regions_kept);
            } else {
                victims.push_back(r);
            }
        });
    }
    if (victims.empty()) return;
    for (Region* r : victims) kill(*r);
    count(g_counters.jit_window_regions_killed, victims.size());
    for (Region* r : victims) for_each_page(*r, [](std::uint64_t page) { restore_page(page); });
    g.epoch.fetch_add(1, std::memory_order_acq_rel);
    for_each_context([](GuestContext& ctx) { ctx.interrupt_pending = 1; });
    count(g_counters.invalidation_interrupts);
}
} // namespace

WriteFault smc_handle_write_fault(std::uint64_t address, std::uint64_t& changes) {
    const auto page = address & ~(kPage - 1);
    const auto writable_now = [page, &changes] {
        changes = g.protection_changes;
        const vm::Protection now = current_protection(page);
        return writable(now) && (now & vm::kGuard) == 0 ? WriteFault::writable : WriteFault::other;
    };
    {
        SharedLock lock(g.map);
        if (g.protected_pages.empty() || !g.protected_pages.count(page)) return writable_now();
    }
    const std::uint64_t start = ticks_now();
    {
        ExclusiveLock lock(g.map);
        if (!g.protected_pages.count(page)) return writable_now();   // another thread restored it
        count(g_counters.smc_write_faults);
        if (!t_jit_window || !defer_page(page)) invalidate_locked(page, page + kPage, true);
    }
    count(g_counters.smc_fault_ticks, ticks_now() - start);
    return WriteFault::smc;
}

} // namespace transmute::fx::rt

namespace transmute::fx {

void register_host_entry(std::uint64_t guest_pc, const void* host_code) {
    rt::ensure_initialized();
    const void* host = rt::in_arena(reinterpret_cast<std::uint64_t>(host_code)) ? host_code
                                                                                 : rt::make_trampoline(host_code);
    rt::install_host_entry(guest_pc, host);
}

// The call sequence of the abi override thunks (src/fx/abi/thunk_gen.cpp,
// generate_override_thunk): entered by jmp in the flushed state with
// rsp == ctx->host_rsp (16-aligned, shadow space owned by callees), so the
// callback is an ordinary Win64 call and the arena's generic unwind entry
// covers it. rbx (host callee-saved) carries x30 across the call. The FP
// handoff (fp_boundary.hpp) makes ctx.fpsr/ctx.fpcr architectural for the
// callback and loads them into MXCSR after it ([rsp+8], shadow space outside
// the call, is its scratch slot; ThreadState::fp_host marks the callback).
//   mov rbx, [x30]; mov [pc], rbx
//   ctx.fpsr |= flags(MXCSR); ts->fp_host = 1
//   lea rcx, [ctx]; mov rdx, user; mov rax, callback; call rax
//   ts->fp_host = 0; MXCSR = state(ctx.fpcr, ctx.fpsr); ctx.fpsr &= ~mirrored
//   cmp [pc], rbx; jne -> jmp [stub_indirect]          (callback moved pc)
//   mov rbx, [x30]; RSB top == rbx ? pop, jmp entry : pc = rbx, jmp [stub_indirect]
namespace {

const void* host_callback_code(HostCallback callback, void* user) {
    using x64::Gp;
    using x64::ptr;
    const auto field = [](std::size_t offset) { return ptr(Gp::r15, ctx_disp(offset)); };
    const std::int32_t rsb = ctx_disp(offsetof(GuestContext, rsb));
    x64::Assembler a(160);
    a.mov(64, Gp::rbx, ptr(Gp::r15, x_disp(30)));
    a.mov(64, field(offsetof(GuestContext, pc)), Gp::rbx);
    rt::emit_fp_state_to_context(a, ptr(Gp::rsp, 8), rt::kThreadFpHostOffset);
    a.lea(64, Gp::rcx, ptr(Gp::r15, -kContextBias));
    a.mov_imm64(Gp::rdx, reinterpret_cast<std::uint64_t>(user));
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(callback));
    a.call(Gp::rax);
    rt::emit_fp_state_from_context(a, ptr(Gp::rsp, 8), rt::kThreadFpHostOffset);
    const x64::Label redirect = a.new_label(), miss = a.new_label();
    a.cmp(64, Gp::rbx, field(offsetof(GuestContext, pc)));
    a.jcc(x64::Cond::ne, redirect);
    a.mov(64, Gp::rbx, ptr(Gp::r15, x_disp(30)));
    a.mov(32, Gp::rax, field(offsetof(GuestContext, rsb_top)));
    a.sub(32, Gp::rax, 1);
    a.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
    a.mov(32, Gp::rdx, Gp::rax);
    a.shl(32, Gp::rdx, 4);
    a.cmp(64, Gp::rbx, ptr(Gp::r15, Gp::rdx, 1, rsb));
    a.jcc(x64::Cond::ne, miss);
    a.mov(32, field(offsetof(GuestContext, rsb_top)), Gp::rax);
    rt::load_globals(a);   // the RSB entry expects the convention registers live (rdx is not one)
    a.jmp(ptr(Gp::r15, Gp::rdx, 1, rsb + 8));
    a.bind(miss);
    a.mov(64, field(offsetof(GuestContext, pc)), Gp::rbx);
    a.bind(redirect);
    a.jmp(field(offsetof(GuestContext, stub_indirect)));   // flushed state: loads them on a hit
    a.finalize();
    void* code = rt::arena_allocate(a.size(), 16);
    std::memcpy(code, a.data(), a.size());
    host::vm::flush_instruction_cache(code, a.size());
    return code;
}

} // namespace

void register_host_callback(std::uint64_t guest_pc, HostCallback callback, void* user) {
    rt::ensure_initialized();
    rt::install_host_entry(guest_pc, host_callback_code(callback, user));
}

// The leaf entry (process.hpp): entered by jmp from translated code, flushed,
// with the convention registers live and rsp == ctx->host_rsp, so the leaf is
// an ordinary Win64 call that keeps them (and the arena's generic unwind
// entry covers it). It is installed without an adapter; the callback's own
// entry, behind its adapter, is the fallback.
//   [mov [x31], sp's host]; lea rcx, [ctx]; mov rax, leaf; call rax; test al, al; jz fallback
//   RSB top == x30 ? pop, jmp entry : pc = x30, jmp [stub_indirect_live]
void register_host_leaf(std::uint64_t guest_pc, HostLeaf leaf, HostCallback callback, void* user) {
    using x64::Gp;
    using x64::ptr;
    rt::ensure_initialized();
    const auto field = [](std::size_t offset) { return ptr(Gp::r15, ctx_disp(offset)); };
    const std::int32_t rsb = ctx_disp(offsetof(GuestContext, rsb));
    const auto& conv = translator_register_convention();
    if ((conv.guest_mask & 0x3ffu) != 0) {
        // x0..x9 live in convention hosts (TRANSMUTE_FX_GLOBALS): their context
        // slots are stale where the leaf would read them. The callback alone.
        rt::install_host_entry(guest_pc, host_callback_code(callback, user));
        return;
    }
    const void* fallback = host_callback_code(callback, user);
    {
        rt::ExclusiveLock lock(rt::g.map);
        fallback = rt::host_entry_adapter(fallback);
    }
    const int lr_host = (conv.guest_mask >> 30) & 1u ? conv.host[30] : -1;
    x64::Assembler a(128);
    const x64::Label slow = a.new_label(), miss = a.new_label(), data = a.new_label();
    if ((conv.guest_mask >> 31) & 1u)   // sp for the leaf (Darwin's stack probe)
        a.mov(64, ptr(Gp::r15, x_disp(31)), x64::gp(static_cast<unsigned>(conv.host[31])));
    a.lea(64, Gp::rcx, ptr(Gp::r15, -kContextBias));
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(leaf));
    a.call(Gp::rax);
    a.test(8, Gp::rax, Gp::rax);
    a.jcc(x64::Cond::e, slow);
    Gp x30 = Gp::r8;   // x30: in its convention host (kept by the call), else its context slot
    if (lr_host >= 0) x30 = x64::gp(static_cast<unsigned>(lr_host));
    else a.mov(64, x30, ptr(Gp::r15, x_disp(30)));
    a.mov(32, Gp::rax, field(offsetof(GuestContext, rsb_top)));
    a.sub(32, Gp::rax, 1);
    a.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
    a.mov(32, Gp::rdx, Gp::rax);
    a.shl(32, Gp::rdx, 4);
    a.cmp(64, x30, ptr(Gp::r15, Gp::rdx, 1, rsb));
    a.jcc(x64::Cond::ne, miss);
    a.mov(32, field(offsetof(GuestContext, rsb_top)), Gp::rax);
    a.jmp(ptr(Gp::r15, Gp::rdx, 1, rsb + 8));   // the continuation expects the convention registers live
    a.bind(miss);
    a.mov(64, field(offsetof(GuestContext, pc)), x30);
    a.jmp(field(offsetof(GuestContext, stub_indirect_live)));
    a.bind(slow);
    a.jmp(x64::rip(data));
    a.align(8);
    a.bind(data);
    a.dq(reinterpret_cast<std::uint64_t>(fallback));
    a.finalize();
    void* code = rt::arena_allocate(a.size(), 16);
    std::memcpy(code, a.data(), a.size());
    host::vm::flush_instruction_cache(code, a.size());
    {
        rt::ExclusiveLock lock(rt::g.map);
        rt::g_adapter_code.insert(code);   // live-state code: no adapter
    }
    rt::install_host_entry(guest_pc, code);
}

void unregister_host_entry(std::uint64_t guest_pc) {
    rt::ensure_initialized();
    rt::remove_host_entry(guest_pc);
}

std::uint32_t guest_page_protection(std::uint64_t address) noexcept { return rt::saved_page_protection(address); }

void invalidate_guest_code(std::uint64_t address, std::uint64_t size) {
    rt::ensure_initialized();
    if (size == 0) return;
    const auto end = address + size < address ? ~std::uint64_t{0} : address + size;
    rt::drop_reusable(address, end);
    rt::drop_guest_code_copies(address, end);
    rt::g_access_epoch.fetch_add(1, std::memory_order_acq_rel);
    rt::ExclusiveLock lock(rt::g.map);
    rt::invalidate_locked(address, end, true);
}

void guest_protection_lowered(std::uint64_t address, std::uint64_t size) {
    rt::ensure_initialized();
    if (size == 0) return;
    const auto begin = address & ~(rt::kPage - 1);
    const auto end = address + size < address ? ~std::uint64_t{0} : address + size;
    rt::g_access_epoch.fetch_add(1, std::memory_order_acq_rel);
    rt::ExclusiveLock lock(rt::g.map);
    auto& saved = rt::g.protected_pages;
    if (saved.empty()) return;
    if ((end - begin) / rt::kPage > saved.size()) {
        for (auto it = saved.begin(); it != saved.end();) it = it->first >= begin && it->first < end ? saved.erase(it) : std::next(it);
    } else {
        for (auto page = begin; page < end; page += rt::kPage) saved.erase(page);
    }
}

void guest_jit_write_window(bool open) {
    rt::ensure_initialized();
    rt::t_jit_window = open;
    if (open || !rt::g_deferred_any.load(std::memory_order_acquire)) return;
    rt::ExclusiveLock lock(rt::g.map);
    rt::check_deferred_pages();
}

void flush_guest_code(std::uint64_t address, std::uint64_t size) {
    rt::ensure_initialized();
    if (size == 0) return;
    const auto end = address + size < address ? ~std::uint64_t{0} : address + size;
    rt::count(rt::g_counters.code_flushes);
    rt::count(rt::g_counters.code_flush_bytes, end - address);
    rt::ExclusiveLock lock(rt::g.map);
    if (rt::invalidate_locked(address, end, true)) rt::count(rt::g_counters.code_flush_hits);
}

} // namespace transmute::fx

