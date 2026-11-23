// Table shadows (GuestContext::table_shadow, runtime.hpp): per (thread, guest
// table base), the targets its BRs took and their host entries, so a BR through
// an interpreter's dispatch table or JIT dispatch table (e.g. V8's Ignition
// and JSDispatchTable) finds its host entry by the table index instead of
// hashing into the 8 MiB lookup table.
//
// Each thread owns its own shadow arrays (only that thread reads pcs[i] and
// calls table_shadow_miss), so a slot whose guest table entry changes (e.g. V8
// function tier-up from CompileLazy/Ignition to TurboFan) can safely update its
// (host, pc) up to kMaxSlotUpdates times before falling back to lookup. Host
// entries are filled under the region map's shared lock (with_map_entry) and
// scrubbed back to the miss stub under its exclusive lock when the entry dies
// (lookup_remove), so after a kill nothing here reaches the dead region.

#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/vm.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace transmute::fx::rt {
namespace {

constexpr std::size_t kSlots = kTableShadowSlots;
constexpr std::size_t kMaxShadowsPerThread = 16;   // further tables take the lookup
constexpr std::size_t kMaxTotalShadows = 256;
constexpr std::uint8_t kMaxSlotUpdates = 8;        // updates allowed per slot (handles V8 tier-ups)

struct ShadowKey {
    const GuestContext* owner;
    std::uint64_t base;
    bool operator==(const ShadowKey&) const noexcept = default;
};

struct ShadowKeyHash {
    std::size_t operator()(const ShadowKey& k) const noexcept {
        const auto a = reinterpret_cast<std::uintptr_t>(k.owner);
        return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(a) ^ (k.base * 0x9e3779b97f4a7c15ull));
    }
};

struct ShadowEntry {
    std::uint64_t* pcs{};
    std::vector<std::uint8_t> updates;   // per-slot target replacement count
};

std::mutex g_mutex;   // the maps (fills run concurrently under the shared map lock)
std::unordered_map<ShadowKey, ShadowEntry, ShadowKeyHash> g_shadows;
std::unordered_multimap<const void*, std::uint64_t*> g_slots;   // host entry -> host slots filled with it
std::atomic<std::size_t> g_slot_count{0};
std::uint64_t* g_empty{};      // a thread's first shadow: every slot misses
std::uint64_t* g_disabled{};   // tables past limit: every slot takes the lookup

std::atomic_ref<std::uint64_t> word(std::uint64_t& w) noexcept { return std::atomic_ref<std::uint64_t>(w); }
std::uint64_t address(const void* p) noexcept { return reinterpret_cast<std::uint64_t>(p); }

constexpr std::uint64_t kDisabledPc = 1u;   // impossible aligned A64 PC -> site's own inline lookup

// [pad] [miss stub] {pc, host}[kSlots]; returns &entries[0].pc (16-byte aligned).
std::uint64_t* make_shadow(std::uint64_t initial_pc) {
    constexpr std::size_t words = 2 + 2 * kSlots;
    auto* w = static_cast<std::uint64_t*>(host::vm::allocate(nullptr, words * sizeof(std::uint64_t), host::vm::kReadWrite));
    if (!w) throw std::bad_alloc();
    w[0] = 0;
    w[1] = address(g_stubs.table_shadow_miss);
    auto* entries = w + 2;
    const auto miss_addr = address(g_stubs.table_shadow_miss);
    for (std::size_t i = 0; i < kSlots; ++i) {
        entries[2 * i] = initial_pc;
        entries[2 * i + 1] = miss_addr;
    }
    return entries;
}

void remove_slot_registration(std::uint64_t* slot, std::uint64_t old_host_addr) {
    if (!old_host_addr || old_host_addr == address(g_stubs.table_shadow_miss) ||
        old_host_addr == address(g_stubs.table_shadow_lookup))
        return;
    const auto* old_host = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(old_host_addr));
    const auto [begin, end] = g_slots.equal_range(old_host);
    for (auto it = begin; it != end; ++it) {
        if (it->second == slot) {
            g_slots.erase(it);
            return;
        }
    }
}

struct Fill {
    ShadowKey key;
    std::uint64_t* pcs;
    std::uint64_t index;
    std::uint64_t target;
};

void fill_slot(const void* host, void* arg) {
    const auto& f = *static_cast<const Fill*>(arg);
    if (!in_arena(address(host))) return;   // translated code only, like the lookup table
    std::lock_guard lock(g_mutex);
    const auto sit = g_shadows.find(f.key);
    if (sit == g_shadows.end() || sit->second.pcs != f.pcs) return;
    auto& entry = sit->second;
    auto& pc = f.pcs[2 * f.index];
    auto& slot = f.pcs[2 * f.index + 1];
    const auto old_pc = word(pc).load(std::memory_order_relaxed);
    if (old_pc == kDisabledPc) return;
    if (old_pc != 0 && old_pc != f.target) {
        if (entry.updates.empty()) entry.updates.assign(kSlots, 0);
        if (entry.updates[f.index] >= kMaxSlotUpdates) {
            const auto old_host = word(slot).load(std::memory_order_relaxed);
            remove_slot_registration(&slot, old_host);
            word(slot).store(address(g_stubs.table_shadow_miss), std::memory_order_release);
            g_slot_count.store(g_slots.size(), std::memory_order_relaxed);
            word(pc).store(kDisabledPc, std::memory_order_release);
            return;
        }
        ++entry.updates[f.index];
    }
    const auto old_host = word(slot).load(std::memory_order_relaxed);
    if (old_host != address(host)) {
        remove_slot_registration(&slot, old_host);
        word(slot).store(address(host), std::memory_order_release);
        g_slots.emplace(host, &slot);
        g_slot_count.store(g_slots.size(), std::memory_order_relaxed);
    }
    if (old_pc != f.target) {
        word(pc).store(f.target, std::memory_order_release);
    }
}

} // namespace

void table_shadow_initialize() {
    g_empty = make_shadow(0u);
    g_disabled = make_shadow(kDisabledPc);
}

void table_shadow_init_context(GuestContext& ctx) noexcept {
    ctx.table_shadow_base = 0;   // no BR has a null table (its load faulted)
    ctx.table_shadow = g_empty;
    ctx.table_shadow_retry = 1;   // the first table becomes this thread's at once
}

void table_shadow_destroy_context(GuestContext& ctx) noexcept {
    std::vector<std::uint64_t*> to_free;
    {
        std::lock_guard lock(g_mutex);
        for (auto it = g_shadows.begin(); it != g_shadows.end();) {
            if (it->first.owner == &ctx) {
                std::uint64_t* pcs = it->second.pcs;
                for (std::size_t i = 0; i < kSlots; ++i) {
                    auto& slot = pcs[2 * i + 1];
                    remove_slot_registration(&slot, word(slot).load(std::memory_order_relaxed));
                }
                to_free.push_back(pcs - 2);
                it = g_shadows.erase(it);
            } else {
                ++it;
            }
        }
        g_slot_count.store(g_slots.size(), std::memory_order_relaxed);
    }
    ctx.table_shadow = g_empty;
    ctx.table_shadow_base = 0;
    for (std::uint64_t* w : to_free) host::vm::release(w);
}

void table_shadow_miss(GuestContext& ctx, std::uint64_t index, std::uint64_t target) {
    const ShadowKey key{&ctx, ctx.table_shadow_base};
    std::uint64_t* pcs = g_disabled;
    {
        std::lock_guard lock(g_mutex);
        if (const auto it = g_shadows.find(key); it != g_shadows.end()) {
            pcs = it->second.pcs;
        } else if (g_shadows.size() < kMaxTotalShadows) {
            std::size_t owned = 0;
            for (const auto& [k, _] : g_shadows)
                if (k.owner == &ctx) ++owned;
            if (owned < kMaxShadowsPerThread) {
                try {
                    ShadowEntry e;
                    e.pcs = make_shadow(0u);
                    pcs = g_shadows.emplace(key, std::move(e)).first->second.pcs;
                } catch (const std::bad_alloc&) {
                }
            }
        }
    }
    ctx.table_shadow = pcs;
    ctx.table_shadow_retry = kTableShadowRetry;
    if (pcs == g_disabled || index >= kSlots) return;
    Fill f{key, pcs, index, target};
    with_map_entry(target, &fill_slot, &f);
}

void table_shadow_scrub(const void* host) noexcept {
    if (g_slot_count.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard lock(g_mutex);
    const auto [begin, end] = g_slots.equal_range(host);
    for (auto it = begin; it != end; ++it) {
        auto slot = word(*it->second);
        if (slot.load(std::memory_order_relaxed) == address(host))
            slot.store(address(g_stubs.table_shadow_miss), std::memory_order_release);
    }
    g_slots.erase(begin, end);
    g_slot_count.store(g_slots.size(), std::memory_order_relaxed);
}

} // namespace transmute::fx::rt
