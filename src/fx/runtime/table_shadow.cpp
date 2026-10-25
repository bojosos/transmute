// Table shadows (GuestContext::table_shadow, runtime.hpp): per guest table
// base, the targets its BRs took and their host entries, so a BR through an
// interpreter's dispatch table finds its host entry by the table index
// instead of hashing into the 8 MiB lookup table.
//
// A slot's pc is written once, after its host entry, so a BR that sees the
// pc sees a host entry that was valid for it. A slot whose table entry later
// holds another target keeps its pc and sends the others to the lookup. Host
// entries are filled under the region map's shared lock (with_map_entry) and
// scrubbed back to the miss stub under its exclusive lock when the entry dies
// (lookup_remove), so after a kill nothing here reaches the dead region.

#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/vm.hpp"

#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>

namespace transmute::fx::rt {
namespace {

constexpr std::size_t kSlots = kTableShadowSlots;
constexpr std::size_t kMaxShadows = 64;   // further tables take the lookup

std::mutex g_mutex;   // the maps (fills run concurrently under the shared map lock)
std::unordered_map<std::uint64_t, std::uint64_t*> g_shadows;     // table base -> its pcs
std::unordered_multimap<const void*, std::uint64_t*> g_slots;   // host entry -> host slots filled with it
std::atomic<std::size_t> g_slot_count{0};
std::uint64_t* g_empty{};      // a thread's first shadow: every slot misses
std::uint64_t* g_disabled{};   // tables past kMaxShadows: every slot takes the lookup

std::atomic_ref<std::uint64_t> word(std::uint64_t& w) noexcept { return std::atomic_ref<std::uint64_t>(w); }
std::uint64_t address(const void* p) noexcept { return reinterpret_cast<std::uint64_t>(p); }

// [miss stub] pcs[kSlots] hosts[kSlots] misses[kSlots]; returns pcs.
std::uint64_t* make_shadow(const void* empty_target) {
    constexpr std::size_t words = 1 + 3 * kSlots;
    auto* w = static_cast<std::uint64_t*>(host::vm::allocate(nullptr, words * sizeof(std::uint64_t), host::vm::kReadWrite));
    if (!w) throw std::bad_alloc();
    w[0] = address(g_stubs.table_shadow_miss);
    auto* pcs = w + 1;
    for (std::size_t i = 0; i < kSlots; ++i) {
        pcs[kSlots + i] = address(empty_target);
        pcs[2 * kSlots + i] = address(empty_target);
    }
    return pcs;
}

struct Fill {
    std::uint64_t* pcs;
    std::uint64_t index;
    std::uint64_t target;
};

void fill_slot(const void* host, void* arg) {
    const auto& f = *static_cast<const Fill*>(arg);
    if (!in_arena(address(host))) return;   // translated code only, like the lookup table
    std::lock_guard lock(g_mutex);
    auto& pc = f.pcs[f.index];
    auto& slot = f.pcs[kSlots + f.index];
    const auto old = word(pc).load(std::memory_order_relaxed);
    if (old != 0 && old != f.target) return;   // the table entry changed: other targets take the lookup
    word(slot).store(address(host), std::memory_order_release);
    if (old == 0) {
        word(pc).store(f.target, std::memory_order_release);
        word(f.pcs[2 * kSlots + f.index]).store(address(g_stubs.table_shadow_lookup), std::memory_order_release);
    }
    g_slots.emplace(host, &slot);
    g_slot_count.store(g_slots.size(), std::memory_order_relaxed);
}

} // namespace

void table_shadow_initialize() {
    g_empty = make_shadow(g_stubs.table_shadow_miss);
    g_disabled = make_shadow(g_stubs.table_shadow_lookup);
}

void table_shadow_init_context(GuestContext& ctx) noexcept {
    ctx.table_shadow_base = 0;   // no BR has a null table (its load faulted)
    ctx.table_shadow = g_empty;
    ctx.table_shadow_retry = 1;   // the first table becomes this thread's at once
}

void table_shadow_miss(GuestContext& ctx, std::uint64_t index, std::uint64_t target) {
    std::uint64_t* pcs = g_disabled;
    {
        std::lock_guard lock(g_mutex);
        if (const auto it = g_shadows.find(ctx.table_shadow_base); it != g_shadows.end()) {
            pcs = it->second;
        } else if (g_shadows.size() < kMaxShadows) {
            try {
                pcs = g_shadows.emplace(ctx.table_shadow_base, make_shadow(g_stubs.table_shadow_miss)).first->second;
            } catch (const std::bad_alloc&) {
            }
        }
    }
    ctx.table_shadow = pcs;
    ctx.table_shadow_retry = kTableShadowRetry;
    if (pcs == g_disabled || index >= kSlots) return;
    Fill f{pcs, index, target};
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
