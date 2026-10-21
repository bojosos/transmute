// Guest code memory: is_guest_code (lock-free), registered executable ranges
// outside images, the live CodeSource used by the JIT and .pdata bounds.

#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/vm.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace transmute::fx::rt {
namespace {

// Sorted, disjoint, non-adjacent [begin, end) ranges (overlapping and
// touching registrations merge). Published copy-on-write through an atomic
// pointer; old sets are retired but never freed (readers hold no lock), so a
// call that changes nothing publishes nothing: V8 registers its code range
// again on every mprotect to executable.
struct RangeSet {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
};

std::atomic<const RangeSet*> g_ranges{nullptr};
std::mutex g_ranges_mutex;
std::vector<std::unique_ptr<RangeSet>> g_range_sets;

void publish(std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges) {
    std::sort(ranges.begin(), ranges.end());
    auto set = std::make_unique<RangeSet>();
    for (const auto& r : ranges) {
        if (r.first >= r.second) continue;
        if (!set->ranges.empty() && r.first <= set->ranges.back().second) {
            set->ranges.back().second = std::max(set->ranges.back().second, r.second);
        } else {
            set->ranges.push_back(r);
        }
    }
    g_ranges.store(set.get(), std::memory_order_release);
    g_range_sets.push_back(std::move(set));
    count(g_counters.code_range_publishes);
}

// The registered range holding `address` (or the first one above it).
template <typename Ranges>
auto range_at_or_after(const Ranges& r, std::uint64_t address) {
    auto it = std::upper_bound(r.begin(), r.end(), address,
                               [](std::uint64_t a, const auto& range) { return a < range.first; });
    if (it != r.begin() && std::prev(it)->second > address) --it;
    return it;
}

// [begin, end) lies inside one registered range (g_ranges_mutex held).
bool covered(std::uint64_t begin, std::uint64_t end) {
    const auto* set = g_ranges.load(std::memory_order_acquire);
    if (!set) return false;
    const auto it = range_at_or_after(set->ranges, begin);
    return it != set->ranges.end() && it->first <= begin && end <= it->second;
}

// [begin, end) meets a registered range (g_ranges_mutex held).
bool overlaps_registered(std::uint64_t begin, std::uint64_t end) {
    const auto* set = g_ranges.load(std::memory_order_acquire);
    if (!set) return false;
    const auto it = range_at_or_after(set->ranges, begin);
    return it != set->ranges.end() && it->first < end;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> current_ranges() {
    const auto* set = g_ranges.load(std::memory_order_acquire);
    return set ? set->ranges : std::vector<std::pair<std::uint64_t, std::uint64_t>>{};
}

bool in_registered_ranges(std::uint64_t address) noexcept {
    const auto* set = g_ranges.load(std::memory_order_acquire);
    if (!set || set->ranges.empty()) return false;
    const auto& r = set->ranges;
    auto it = std::upper_bound(r.begin(), r.end(), address,
                               [](std::uint64_t a, const auto& range) { return a < range.first; });
    if (it == r.begin()) return false;
    --it;
    return address >= it->first && address < it->second;
}

bool readable_committed(std::uint64_t page) noexcept {
    namespace vm = host::vm;
    vm::RegionInfo info;
    if (!vm::query(reinterpret_cast<const void*>(page), info)) return false;
    // Code in memory committed on demand that nothing touched yet (it reads
    // as zeros once committed, as on first touch).
    if (!info.committed() &&
        (!demand_commit(page, 8) || !vm::query(reinterpret_cast<const void*>(page), info) || !info.committed()))
        return false;
    const auto protect = info.protect & 0xff;
    if ((info.protect & vm::kGuard) != 0) return false;
    return protect != vm::kNoAccess && protect != vm::kExecute && protect != 0;
}

// One 8-byte read of an IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY (4-byte aligned).
bool read_pdata_words(const std::uint32_t* entry, std::uint32_t& begin, std::uint32_t& unwind) noexcept {
    std::uint64_t words;
    if (!host::try_read64(reinterpret_cast<std::uint64_t>(entry), words)) return false;
    begin = static_cast<std::uint32_t>(words);
    unwind = static_cast<std::uint32_t>(words >> 32);
    return true;
}

} // namespace

bool in_registered_code_memory(std::uint64_t address) noexcept { return in_registered_ranges(address); }

std::size_t registered_code_range_count() noexcept {
    const auto* set = g_ranges.load(std::memory_order_acquire);
    return set ? set->ranges.size() : 0;
}

// The guest code range (registered range, else module text) holding
// `address`, as is_guest_code decides it. False if none.
bool guest_code_range(std::uint64_t address, std::uint64_t& lo, std::uint64_t& hi) noexcept {
    if (const auto* set = g_ranges.load(std::memory_order_acquire); set && !set->ranges.empty()) {
        const auto& r = set->ranges;
        auto it = std::upper_bound(r.begin(), r.end(), address,
                                   [](std::uint64_t a, const auto& range) { return a < range.first; });
        if (it != r.begin() && address < (--it)->second) {
            lo = it->first;
            hi = it->second;
            return true;
        }
    }
    const auto* module = find_guest_module(address);
    if (!module || address < module->text_begin || address >= module->text_end) return false;
    lo = module->text_begin;
    hi = module->text_end;
    // A registered range may start inside the text range: is_guest_code would
    // still say yes there, so stopping short is safe.
    return true;
}

// readable_committed with a per-thread cache of pages found readable. A
// VirtualQuery costs ~1 us normally but can take over 100 us in processes with
// large reservations (it computes the size of the region of equal attributes),
// and the JIT asks for every page it reads code from. Entries are dropped when
// the invalidation epoch changes (every guest protection change invalidates
// translated code first); a stale entry could only let safe_read32 see a fault.
bool readable_page_cached(std::uint64_t page) noexcept {
    struct Slot {
        std::uint64_t page;
        std::uint64_t epoch;
    };
    thread_local Slot slots[1024];
    const std::uint64_t epoch = invalidation_epoch();
    Slot& slot = slots[(page >> 12) & 1023];
    if (slot.page == page && slot.epoch == epoch) return true;
    if (!readable_committed(page)) return false;
    slot = {page, epoch};
    return true;
}

void LiveCodeSource::reset() {
    page_ = ~std::uint64_t{0};
    page_ok_ = false;
    code_lo_ = code_hi_ = 0;
    reads_.clear();
    host_entries_only = false;
}

bool LiveCodeSource::read(std::uint64_t address, std::uint32_t& word) const {
    if ((address & 3) != 0) return false;
    // is_guest_code, decided once per guest code range.
    if (address - code_lo_ >= code_hi_ - code_lo_ && !guest_code_range(address, code_lo_, code_hi_)) {
        code_lo_ = code_hi_ = 0;
        return false;
    }
    const auto page = address & ~std::uint64_t{0xfff};
    if (page != page_) {
        page_ = page;
        // Executable sections of mapped guest images are committed and never
        // guard pages; skip the (slow) query there. safe_read32 still guards
        // the read itself.
        const auto* module = find_guest_module(page);
        page_ok_ = (module && page >= module->text_begin && page < module->text_end) ||
                   readable_page_cached(page);
    }
    if (!page_ok_ || !safe_read32(address, word)) return false;
    reads_.emplace_back(address, word);
    return true;
}

bool LiveCodeSource::has_entry(std::uint64_t pc) const {
    return host_entries_only ? has_host_entry(pc) : find_entry(pc) != nullptr;
}

bool LiveCodeSource::function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const {
    const auto* module = find_guest_module(address);
    return module && pdata_function_bounds(*module, address, begin, end);
}

bool pdata_function_bounds(const GuestModule& module, std::uint64_t address, std::uint64_t& begin,
                           std::uint64_t& end) {
    if (module.pdata_rva == 0 || module.pdata_size < 8 || address < module.base ||
        address - module.base >= module.size) {
        return false;
    }
    if (std::uint64_t{module.pdata_rva} + module.pdata_size > module.size) return false;
    const auto* table = reinterpret_cast<const std::uint32_t*>(module.base + module.pdata_rva);
    const auto count = module.pdata_size / 8;
    const auto rva = static_cast<std::uint32_t>(address - module.base);
    // Last entry with BeginAddress <= rva.
    std::uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        std::uint32_t b, u;
        if (!read_pdata_words(table + 2 * mid, b, u)) return false;
        if (b <= rva) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return false;
    std::uint32_t function_rva, unwind;
    if (!read_pdata_words(table + 2 * (lo - 1), function_rva, unwind)) return false;
    std::uint32_t length_words;
    switch (unwind & 3) {
    case 0: {   // .xdata RVA: FunctionLength in bits 0-17 of the first word
        if (unwind >= module.size || module.size - unwind < 4) return false;
        std::uint32_t header;
        if (!safe_read32(module.base + unwind, header)) return false;
        length_words = header & 0x3ffff;
        break;
    }
    case 1:     // packed unwind data
    case 2:     // packed, function fragment
        length_words = (unwind >> 2) & 0x7ff;
        break;
    default:
        return false;
    }
    begin = module.base + function_rva;
    end = begin + std::uint64_t{length_words} * 4;
    return address >= begin && address < end;
}

} // namespace transmute::fx::rt

namespace transmute::fx {

bool is_guest_code(std::uint64_t address) noexcept {
    if (rt::in_registered_ranges(address)) return true;
    const auto* module = find_guest_module(address);
    return module && address >= module->text_begin && address < module->text_end;
}

void register_guest_code_memory(std::uint64_t base, std::uint64_t size) {
    if (size == 0) return;
    const auto end = base + size < base ? ~std::uint64_t{0} : base + size;
    std::lock_guard lock(rt::g_ranges_mutex);
    if (rt::covered(base, end)) return;
    auto ranges = rt::current_ranges();
    ranges.emplace_back(base, end);
    rt::publish(std::move(ranges));
}

void unregister_guest_code_memory(std::uint64_t base, std::uint64_t size) {
    if (size == 0) return;
    const auto end = base + size < base ? ~std::uint64_t{0} : base + size;
    {
        std::lock_guard lock(rt::g_ranges_mutex);
        if (rt::overlaps_registered(base, end)) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
            for (const auto& r : rt::current_ranges()) {
                if (r.second <= base || r.first >= end) { result.push_back(r); continue; }
                if (r.first < base) result.emplace_back(r.first, base);
                if (r.second > end) result.emplace_back(end, r.second);
            }
            rt::publish(std::move(result));
        }
    }
    invalidate_guest_code(base, size);
}

} // namespace transmute::fx
