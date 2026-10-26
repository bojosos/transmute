#include "alchemy/darwin_vm_space.hpp"

#include "alchemy/darwin_services.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/process_context.hpp"

#include <algorithm>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace alchemy {
namespace {

using Engine = std::remove_reference_t<decltype(std::declval<ProcessContext&>().main_engine())>;

// Darwin <sys/errno.h>, <sys/mman.h>.
constexpr std::uint32_t enomem = 12, einval = 22;
constexpr std::uint64_t prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr std::uint64_t map_private = 0x2, map_fixed = 0x10, map_noreserve = 0x40, map_hassemaphore = 0x200,
    map_nocache = 0x400, map_jit = 0x800, map_anonymous = 0x1000;
// MAP_NORESERVE, MAP_HASSEMAPHORE and MAP_NOCACHE are hints. MAP_JIT marks a
// region that may toggle between writable and executable; fx allows RWX on
// any guest page (code coherence comes from its write faults), so it adds
// nothing here.
constexpr std::uint64_t accepted_flags = map_private | map_anonymous | map_fixed | map_noreserve | map_hassemaphore |
    map_nocache | map_jit;
// <sys/mman.h> madvise advice.
constexpr std::uint64_t madv_free = 5, madv_free_reusable = 7, madv_can_reuse = 9;
constexpr std::uint64_t page = darwin_arm64_page_size;
// __PAGEZERO: the guest never gets the low 4 GiB.
constexpr std::uint64_t lowest_address = 0x100000000ULL;

std::uint64_t round_up(std::uint64_t length) {
    if (length > UINT64_MAX - (page - 1)) throw DarwinVmError{enomem};
    return (length + page - 1) & ~(page - 1);
}

// The host cannot read execute-only pages, and fx reads code through them.
bool host_readable(std::uint64_t protection) { return (protection & (prot_read | prot_exec)) != 0; }

// Every DarwinVmSpace by process (Mach vm_remap reaches the space of its process).
std::mutex registry_mutex;
std::map<const ProcessContext*, DarwinVmSpace*> registry;

} // namespace

DarwinVmSpace::DarwinVmSpace(ProcessContext& context) : context_(context) {
    std::lock_guard lock(registry_mutex);
    registry[&context] = this;
}

DarwinVmSpace::~DarwinVmSpace() {
    std::lock_guard lock(registry_mutex);
    if (const auto found = registry.find(&context_); found != registry.end() && found->second == this) registry.erase(found);
}

DarwinVmSpace* darwin_vm_space(const ProcessContext& context) {
    std::lock_guard lock(registry_mutex);
    const auto found = registry.find(&context);
    return found != registry.end() ? found->second : nullptr;
}

bool DarwinVmSpace::enabled() const noexcept { return engine_has_vm_ranges<Engine>; }

bool DarwinVmSpace::owns(std::uint64_t begin, std::uint64_t end) const {
    auto it = owned_.upper_bound(begin);
    if (it == owned_.begin()) return false;
    --it;
    // Adjacent owned ranges are merged, so one range covers an owned span.
    return it->first <= begin && it->second >= end;
}

void DarwinVmSpace::own(std::uint64_t begin, std::uint64_t end) {
    auto it = owned_.upper_bound(begin);
    if (it != owned_.begin() && std::prev(it)->second >= begin) {
        --it;
        begin = it->first;
        end = std::max(end, it->second);
        it = owned_.erase(it);
    }
    while (it != owned_.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = owned_.erase(it);
    }
    owned_.emplace(begin, end);
}

void DarwinVmSpace::disown(std::uint64_t begin, std::uint64_t end) {
    auto it = owned_.upper_bound(begin);
    if (it != owned_.begin()) --it;
    while (it != owned_.end() && it->first < end) {
        const auto range_begin = it->first, range_end = it->second;
        if (range_end <= begin) { ++it; continue; }
        it = owned_.erase(it);
        if (range_begin < begin) owned_.emplace(range_begin, begin);
        if (range_end > end) {
            owned_.emplace(end, range_end);
            break;
        }
    }
}

bool DarwinVmSpace::intersects(std::uint64_t address, std::uint64_t length) const {
    if (!length || address > UINT64_MAX - length) return false;
    const auto end = address + length;
    std::lock_guard lock(mutex_);
    auto it = owned_.upper_bound(address);
    if (it != owned_.begin() && std::prev(it)->second > address) return true;
    return it != owned_.end() && it->first < end;
}

std::uint64_t DarwinVmSpace::map(std::uint64_t address, std::uint64_t length, std::uint64_t protection,
                                 std::uint64_t flags) {
    if (!length || (protection & ~(prot_read | prot_write | prot_exec)) || (flags & ~accepted_flags) ||
        !(flags & map_private) || !(flags & map_anonymous))
        throw DarwinVmError{einval};
    const auto size = round_up(length);
    auto& engine = context_.main_engine();
    const bool readable = host_readable(protection), writable = (protection & prot_write) != 0;
    const bool executable = (protection & prot_exec) != 0;
    std::lock_guard lock(mutex_);
    if (flags & map_fixed) {
        if (address % page || address > UINT64_MAX - size) throw DarwinVmError{einval};
        const auto end = address + size;
        // Pages the guest does not own here must be free address space: Alchemy
        // never replaces images, the malloc heap, stacks or file views.
        if (address < lowest_address) throw DarwinVmError{enomem};
        auto cursor = address;
        auto it = owned_.upper_bound(address);
        if (it != owned_.begin() && std::prev(it)->second > address) cursor = std::prev(it)->second;
        while (cursor < end) {
            const auto gap_end = it == owned_.end() ? end : std::min(end, it->first);
            if (gap_end > cursor && !engine.is_range_available(cursor, gap_end - cursor)) throw DarwinVmError{enomem};
            if (it == owned_.end()) break;
            cursor = it->second;
            ++it;
        }
        try {
            engine_replace_range(engine, address, size, readable, writable, executable);
        } catch (const std::bad_alloc&) {
            throw DarwinVmError{enomem};
        } catch (const std::runtime_error&) {
            // Another host thread took part of a free gap meanwhile.
            disown(address, end);
            throw DarwinVmError{enomem};
        }
        own(address, end);
        return address;
    }
    std::uint64_t base{};
    const auto hint = address & ~(page - 1);
    if (hint >= lowest_address && hint <= UINT64_MAX - size) {
        try {
            if (engine_reserve_at(engine, hint, size, readable, writable)) base = hint;
        } catch (const std::bad_alloc&) {
            throw DarwinVmError{enomem};
        }
    }
    if (!base) {
        try {
            const auto reserved = reserve_engine_range(engine, size, page, readable, writable);
            if (!reserved) throw DarwinVmError{enomem};
            base = *reserved;
        } catch (const std::bad_alloc&) {
            throw DarwinVmError{enomem};
        } catch (const std::runtime_error&) {
            throw DarwinVmError{enomem};
        }
    }
    if (executable) protect_engine_range(engine, base, size, readable, writable, true);
    own(base, base + size);
    return base;
}

std::uint64_t DarwinVmSpace::unmap(std::uint64_t address, std::uint64_t length) {
    if (!length || address % page || address > UINT64_MAX - length) throw DarwinVmError{einval};
    const auto end = address + round_up(length);
    std::lock_guard lock(mutex_);
    // Only the owned parts: the rest of the range is not this space's memory.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> parts;
    auto it = owned_.upper_bound(address);
    if (it != owned_.begin()) --it;
    for (; it != owned_.end() && it->first < end; ++it)
        if (it->second > address) parts.emplace_back(std::max(address, it->first), std::min(end, it->second));
    for (const auto& [begin, part_end] : parts) {
        engine_unmap_range(context_.main_engine(), begin, part_end - begin);
        disown(begin, part_end);
    }
    return 0;
}

bool DarwinVmSpace::protect(std::uint64_t address, std::uint64_t length, std::uint64_t protection) {
    if (!length) return false;
    if (address % page || (protection & ~(prot_read | prot_write | prot_exec)) || address > UINT64_MAX - length)
        throw DarwinVmError{einval};
    const auto end = address + round_up(length);
    std::lock_guard lock(mutex_);
    if (!owns(address, end)) return false;
    try {
        protect_engine_range(context_.main_engine(), address, end - address, host_readable(protection),
                             (protection & prot_write) != 0, (protection & prot_exec) != 0);
    } catch (const std::bad_alloc&) {
        throw DarwinVmError{enomem};   // the host cannot commit that much
    }
    return true;
}

bool DarwinVmSpace::replace_with_copy(std::uint64_t address, std::span<const std::uint8_t> bytes,
                                      std::uint64_t protection) {
    if (!enabled() || bytes.empty() || address % page || bytes.size() % page || address > UINT64_MAX - bytes.size() ||
        (protection & ~(prot_read | prot_write | prot_exec)))
        return false;
    const auto end = address + bytes.size();
    auto& engine = context_.main_engine();
    std::lock_guard lock(mutex_);
    if (!owns(address, end)) return false;
    try {
        engine_replace_range(engine, address, bytes.size(), true, true, false);
    } catch (const std::bad_alloc&) {
        throw DarwinVmError{enomem};
    }
    engine.write_bytes(address, bytes);
    protect_engine_range(engine, address, bytes.size(), host_readable(protection), (protection & prot_write) != 0,
                         (protection & prot_exec) != 0);
    return true;
}

bool DarwinVmSpace::replace_with_alias(std::uint64_t address, std::uint64_t length, const std::function<void()>& alias) {
    if (!enabled() || !length || address % page || length % page || address > UINT64_MAX - length) return false;
    std::lock_guard lock(mutex_);
    if (!owns(address, address + length)) return false;
    alias();
    return true;
}

bool DarwinVmSpace::advise(std::uint64_t address, std::uint64_t length, std::uint64_t advice) {
    if (!length || address > UINT64_MAX - length) return false;
    const auto begin = address & ~(page - 1);
    const auto end = (address + length + page - 1) & ~(page - 1);
    std::lock_guard lock(mutex_);
    if (!owns(begin, end)) return false;
    if (advice > madv_can_reuse) throw DarwinVmError{einval};
    // MADV_FREE and MADV_FREE_REUSABLE: the contents may go (later reads see
    // the old bytes or zeros). Darwin's MADV_DONTNEED keeps contents, and the
    // other advice only steers paging, so they change nothing visible.
    if (advice == madv_free || advice == madv_free_reusable) engine_discard_range(context_.main_engine(), begin, end - begin);
    return true;
}

} // namespace alchemy
