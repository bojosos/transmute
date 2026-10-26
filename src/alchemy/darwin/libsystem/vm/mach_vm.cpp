#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_vm_space.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy {
namespace {

constexpr std::uint32_t success = 0, invalid_address = 1, protection_failure = 2,
    no_space = 3, invalid_argument = 4, resource_shortage = 6,
    invalid_task = 16, not_supported = 46;
constexpr std::uint64_t page_size = darwin_arm64_page_size;
constexpr std::uint64_t allocation_limit = 64ULL * 1024 * 1024;
// The Mach VM pages a process may hold at once. Darwin has no such limit; this one only stops a runaway. 256 MiB
// was too little for a browser: Chrome backs each Mojo data pipe with a 2 MiB memory entry (a vm_allocate anchor
// and a mapped view), and JetStream's parallel script loads ended the browser with KERN_RESOURCE_SHORTAGE.
constexpr std::uint64_t resident_limit = 4ULL * 1024 * 1024 * 1024;
constexpr std::uint32_t self_task = 0xa1c1;
constexpr unsigned anywhere = 1, purgable = 2, overwrite = 0x4000;
constexpr unsigned read = 1, write = 2, execute = 4;

std::string hex(std::uint64_t value) {
    char text[20];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

bool checked_range(std::uint64_t address, std::uint64_t size,
                   std::uint64_t& begin, std::uint64_t& end) {
    if (size == 0) { begin = end = address; return true; }
    if (size > UINT64_MAX - address) return false;
    const auto last = address + size;
    if (last > UINT64_MAX - (page_size - 1)) return false;
    begin = address & ~(page_size - 1);
    end = (last + page_size - 1) & ~(page_size - 1);
    return begin >= page_size;
}

bool accessible(transmute::GuestMemory& memory, std::uint64_t address,
                std::uint64_t size, bool writable) {
    if (address == 0 || size > UINT64_MAX - address) return false;
    while (size != 0) {
        const auto region = memory.query_region(address);
        // RegionInfo::mapped describes external backing. Internal guest-owned
        // mappings are equally valid; reservations have no access permissions.
        if (!region || !region->readable ||
            (writable && !region->writable) || address < region->base ||
            address - region->base >= region->size) return false;
        const auto count = std::min(size, region->size - (address - region->base));
        address += count;
        size -= count;
    }
    return true;
}

// <mach/vm_purgable.h>: controls, states and the modifier bits a state may carry.
constexpr std::uint32_t purgable_set_state = 0, purgable_get_state = 1, purgable_purge_all = 2;
constexpr std::uint32_t purgable_nonvolatile = 0, purgable_volatile = 1, purgable_empty = 2, purgable_deny = 3,
    purgable_state_mask = 3;
// VM_PURGABLE_ALL_MASKS: state (0x3), volatile order (0x10), ordering
// (0x20), behavior (0x40), volatile group (0x700), debug (0x3000), no aging
// (0x10000), from the public header's shifts and masks. A bit outside them is
// KERN_INVALID_ARGUMENT, as in the kernel.
constexpr std::uint32_t purgable_all_masks = 0x3 | 0x10 | 0x20 | 0x40 | 0x700 | 0x3000 | 0x10000;
// <mach/vm_region.h>: struct vm_region_submap_info_64 (pack 4) in its three
// versions, and vm_region_submap_short_info_64, counted in natural_t words.
constexpr std::uint32_t submap_info_v0_count = 16, submap_info_v1_count = 17, submap_info_v2_count = 19,
    submap_short_info_count = 12;
constexpr std::uint32_t share_private = 2, share_empty = 3;   // SM_PRIVATE, SM_EMPTY
constexpr std::uint32_t inherit_copy = 1;                     // VM_INHERIT_COPY
constexpr std::uint32_t inherit_last_valid = 2;               // VM_INHERIT_NONE (<mach/vm_inherit.h>)

// Public Mach interfaces are described by XNU's mach/vm_map.defs and
// mach/kern_return.h. The implementation below is independently authored.
struct MachVm;
std::mutex registry_mutex;
std::map<const ProcessContext*, std::weak_ptr<MachVm>> registry;

struct MachVm : std::enable_shared_from_this<MachVm> {
    struct Page { unsigned current{read | write}, maximum{read | write | execute}; };
    explicit MachVm(ProcessContext& selected) : process(selected) {}
    ~MachVm() {
        {
            std::lock_guard lock(registry_mutex);
            const auto found = registry.find(&process);
            if (found != registry.end() && found->second.expired()) registry.erase(found);
        }
        // The pages were released by the process's teardown hook (release_all);
        // `process` may be gone by now (FxEngine keeps retired call handlers,
        // and so this object, until host process exit).
    }
    // ~ProcessContext (at_teardown): unmaps every page Mach VM still owns.
    void release_all() {
        std::lock_guard lock(metadata);
        for (const auto& [address, permissions] : pages) {
            static_cast<void>(permissions);
            try { process.main_engine().unmap_memory(address); } catch (...) { }
        }
        pages.clear();
        purgeable.clear();
    }
    ProcessContext& process;
    std::mutex metadata;
    std::map<std::uint64_t, Page> pages;
    std::uint64_t next_address{0x800000000ULL};
    // Purgeable objects (vm_allocate VM_FLAGS_PURGABLE, or registered by
    // another provider) by base: their size and purgeable state. The model
    // never purges on its own (no memory pressure is simulated); only
    // VM_PURGABLE_PURGE_ALL and setting VM_PURGABLE_EMPTY discard contents.
    struct Purgeable { std::uint64_t size{}; std::uint32_t state{purgable_nonvolatile}; };
    std::map<std::uint64_t, Purgeable> purgeable;

    bool task(std::uint64_t value) const { return static_cast<std::uint32_t>(value) == self_task; }

    // One stderr line per distinct refusal (Mach VM calls Alchemy cannot express).
    std::mutex notices_mutex;
    std::set<std::string> notices;
    void notice(const std::string& message) {
        {
            std::lock_guard lock(notices_mutex);
            if (!notices.insert(message).second) return;
        }
        std::fprintf(stderr, "alchemy: %s\n", message.c_str());
    }

    std::uint32_t allocate(transmute::CpuState& state, transmute::GuestMemory& memory) {
        if (!task(state.x[0])) return invalid_task;
        const auto output = state.x[1], size = state.x[2];
        // The top byte (VM_FLAGS_ALIAS_MASK, VM_MAKE_TAG) only labels the region.
        const auto flags = static_cast<unsigned>(state.x[3]) & 0x00ffffffU;
        if ((flags & ~(anywhere | purgable)) != 0) return not_supported;
        if (!accessible(memory, output, 8, true)) return invalid_address;
        if (size == 0) { memory.write64(output, 0); return success; }
        std::uint64_t begin{}, end{};
        const auto requested = (flags & anywhere) ? page_size : memory.read64(output) & ~(page_size - 1);
        if (!checked_range(requested, size, begin, end)) return invalid_address;
        const auto extent = end - begin;
        if (extent > allocation_limit) return resource_shortage;
        std::lock_guard lock(metadata);
        if (pages.size() > (resident_limit - extent) / page_size) return resource_shortage;
        auto& engine = process.calling_engine();
        if (flags & anywhere) {
            begin = next_address;
            while (!engine.is_range_available(begin, extent)) {
                if (begin > UINT64_MAX - extent - page_size) return no_space;
                begin += extent + page_size;
            }
            if (begin > UINT64_MAX - extent - page_size) return no_space;
        } else if (!engine.is_range_available(begin, extent)) return no_space;
        // Shared memory where the engine has it, so vm_remap can alias these
        // pages later (one region per Darwin page, as below).
        if (engine_map_shared_memory(engine, begin, extent, page_size)) {
            for (auto offset = std::uint64_t{}; offset < extent; offset += page_size) pages.emplace(begin + offset, Page{});
            memory.write64(output, begin);
            if (flags & anywhere) next_address = begin + extent + page_size;
            if (flags & purgable) purgeable[begin] = Purgeable{extent};
            return success;
        }
        std::vector<std::uint64_t> mapped;
        try {
            mapped.reserve(static_cast<std::size_t>(extent / page_size));
            for (auto offset = std::uint64_t{}; offset < extent; offset += page_size) {
                const auto address = begin + offset;
                engine.map_memory(address, static_cast<std::size_t>(page_size));
                mapped.push_back(address);
                engine.set_memory_access(address, true, true, false);
                pages.emplace(address, Page{});
            }
            memory.write64(output, begin);
            if (flags & anywhere) next_address = begin + extent + page_size;
            if (flags & purgable) purgeable[begin] = Purgeable{extent};
        } catch (...) {
            for (const auto address : mapped) {
                pages.erase(address);
                try { engine.unmap_memory(address); } catch (...) { }
            }
            throw;
        }
        return success;
    }

    // vm_allocate(VM_FLAGS_ANYWHERE) for a provider (no guest arguments).
    std::uint32_t allocate_anywhere(std::uint64_t size, std::uint64_t& address) {
        const auto slot = process.allocate_guest(8);
        transmute::CpuState state{};
        state.x[0] = self_task;
        state.x[1] = slot;
        state.x[2] = size;
        state.x[3] = anywhere;
        std::uint32_t result{};
        try {
            result = allocate(state, process.calling_engine());
            address = result == success ? process.calling_engine().read64(slot) : 0;
        } catch (...) {
            process.release_guest(slot);
            throw;
        }
        process.release_guest(slot);
        return result;
    }

    // A host section mapped as Mach-owned pages (cross_process_memory.hpp):
    // one fx shared region per page, at `address` or anywhere (0). The
    // maximum protection never includes execute (memory shared with other
    // processes) and includes write only for a writable section. With
    // `replace` (VM_FLAGS_OVERWRITE) a fixed address may hold Mach-owned
    // pages, which the section replaces.
    std::uint32_t map_section(void* section, std::uint64_t offset, std::uint64_t size, bool writable_section,
                              std::uint64_t& address, std::uint32_t protection, std::uint32_t maximum, bool replace) {
        if (!size || address % page_size || offset % page_size || (replace && !address)) return invalid_argument;
        maximum &= read | (writable_section ? write : 0U);
        if (protection & ~maximum) return protection_failure;
        std::uint64_t begin{}, end{};
        if (!checked_range(address ? address : page_size, size, begin, end)) return invalid_address;
        const auto extent = end - begin;
        std::lock_guard lock(metadata);
        auto& engine = process.calling_engine();
        if (!address) {
            begin = next_address;
            while (!engine.is_range_available(begin, extent)) {
                if (begin > UINT64_MAX - extent - page_size) return no_space;
                begin += extent + page_size;
            }
        } else if (replace) {
            for (auto at = begin; at < begin + extent; at += page_size)
                if (!pages.contains(at) && !engine.is_range_available(at, page_size)) return no_space;
            for (auto at = begin; at < begin + extent; at += page_size) {
                if (!pages.contains(at)) continue;
                engine.unmap_memory(at);
                pages.erase(at);
            }
            forget_purgeable(begin, begin + extent);
        } else if (!engine.is_range_available(begin, extent)) return no_space;
        try {
            if (!engine_map_section_pages(engine, begin, extent, section, offset, page_size, writable_section,
                                          (protection & read) != 0, (protection & write) != 0))
                return not_supported;
        } catch (const std::runtime_error&) {
            return no_space;
        }
        const Page permissions{protection & (read | write), maximum};
        for (auto at = begin; at < begin + extent; at += page_size) pages[at] = permissions;
        if (!address) next_address = begin + extent + page_size;
        address = begin;
        return success;
    }

    std::uint32_t deallocate(transmute::CpuState& state) {
        if (!task(state.x[0])) return invalid_task;
        std::uint64_t begin{}, end{};
        if (!checked_range(state.x[1], state.x[2], begin, end)) return invalid_address;
        if (begin == end) return success;
        if (end - begin > allocation_limit) return resource_shortage;
        std::lock_guard lock(metadata);
        // Mach-owned pages, and mmap'd ones (Darwin's vm_deallocate releases
        // those too: munmap). Loader, heap, file mappings and worker stacks
        // retain their own lifetime managers.
        auto* space = darwin_vm_space(process);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> mmapped;   // runs of mmap'd pages
        for (auto address = begin; address < end; address += page_size) {
            if (pages.contains(address)) continue;
            if (!space || !space->intersects(address, page_size))
                return process.calling_engine().query_region(address) ? not_supported : invalid_address;
            if (!mmapped.empty() && mmapped.back().second == address) mmapped.back().second += page_size;
            else mmapped.emplace_back(address, address + page_size);
        }
        for (auto address = begin; address < end; address += page_size) {
            if (!pages.contains(address)) continue;
            process.calling_engine().unmap_memory(address);
            pages.erase(address);
        }
        for (const auto& [from, to] : mmapped) space->unmap(from, to - from);
        forget_purgeable(begin, end);
        return success;
    }

    void forget_purgeable(std::uint64_t begin, std::uint64_t end) {   // metadata held
        for (auto it = purgeable.begin(); it != purgeable.end();)
            it = it->first < end && begin < it->first + it->second.size ? purgeable.erase(it) : std::next(it);
    }

    // The object's pages read as zeros afterwards (a purge).
    void purge(transmute::GuestMemory& memory, std::uint64_t base, std::uint64_t size) {   // metadata held
        auto& engine = process.calling_engine();
        for (auto address = base; address < base + size; address += page_size) {
            const auto found = pages.find(address);
            const bool locked = found != pages.end() && (found->second.current & write) == 0;
            if (locked) engine.set_memory_access(address, true, true, false);
            memory.fill_bytes(address, 0, page_size);
            if (locked)
                engine.set_memory_access(address, (found->second.current & read) != 0, false,
                                         (found->second.current & execute) != 0);
        }
    }

    // kern_return_t mach_vm_purgable_control(vm_map_t, mach_vm_address_t, vm_purgable_t, int* state)
    std::uint32_t purgable_control(transmute::CpuState& state, transmute::GuestMemory& memory) {
        if (!task(state.x[0])) return invalid_task;
        const auto address = state.x[1];
        const auto control = static_cast<std::uint32_t>(state.x[2]);
        const auto output = state.x[3];
        // VM_PURGABLE_SET_STATE_FROM_KERNEL (3) is not allowed from user space.
        if (control > purgable_purge_all) return invalid_argument;
        std::lock_guard lock(metadata);
        if (control == purgable_purge_all) {
            for (auto& [base, object] : purgeable)
                if ((object.state & purgable_state_mask) == purgable_volatile) {
                    purge(memory, base, object.size);
                    object.state = (object.state & ~purgable_state_mask) | purgable_empty;
                }
            return success;
        }
        if (!accessible(memory, output, 4, true)) return invalid_argument;
        const auto requested = memory.read32(output);
        if (control == purgable_set_state &&
            ((requested & ~purgable_all_masks) != 0 || (requested & purgable_state_mask) == purgable_deny))
            return invalid_argument;
        auto found = purgeable.upper_bound(address);
        if (found != purgeable.begin()) --found;
        if (found == purgeable.end() || address < found->first || address - found->first >= found->second.size) {
            // Mapped memory that is not a purgeable object, or nothing mapped there.
            return process.calling_engine().query_region(address) ? invalid_argument : invalid_address;
        }
        auto& object = found->second;
        if (control == purgable_set_state) {
            const auto page = pages.find(address & ~(page_size - 1));
            if (page != pages.end() && (page->second.current & write) == 0) return protection_failure;
        }
        const auto previous = object.state & purgable_state_mask;
        memory.write32(output, previous);
        if (control == purgable_get_state) return success;
        // An object made empty loses its contents now; a volatile one keeps
        // them until a purge. Made nonvolatile after a purge, it reads as
        // zeros (the guest learns it from the VM_PURGABLE_EMPTY it got back).
        const auto next = requested & purgable_state_mask;
        if (next == purgable_empty && previous != purgable_empty) purge(memory, found->first, object.size);
        object.state = requested;
        return success;
    }

    // kern_return_t mach_vm_region_recurse(vm_map_t, mach_vm_address_t* address, mach_vm_size_t* size,
    //     natural_t* nesting_depth, vm_region_recurse_info_t info, mach_msg_type_number_t* count)
    // Alchemy's address space has no submaps (no shared region), so every
    // region is at depth 0. The region is the guest region containing
    // *address or the next one above it; inside a reservation, the run of
    // pages with one protection, as Darwin splits map entries.
    std::uint32_t region_recurse(transmute::CpuState& state, transmute::GuestMemory& memory) {
        if (!task(state.x[0])) return invalid_task;
        const auto address_output = state.x[1], size_output = state.x[2], depth_output = state.x[3];
        const auto info = state.x[4], count_output = state.x[5];
        if (!accessible(memory, address_output, 8, true) || !accessible(memory, size_output, 8, true) ||
            !accessible(memory, depth_output, 4, true) || !accessible(memory, count_output, 4, true))
            return invalid_argument;
        const auto count = memory.read32(count_output);
        const auto words = count >= submap_info_v2_count ? submap_info_v2_count
                         : count >= submap_info_v1_count ? submap_info_v1_count
                         : count >= submap_info_v0_count ? submap_info_v0_count
                         : count >= submap_short_info_count ? submap_short_info_count : 0U;
        if (!words || !accessible(memory, info, words * 4ULL, true)) return invalid_argument;
        const auto region = engine_region_from(process.calling_engine(), memory.read64(address_output));
        if (!region || region->size == 0) return invalid_address;
        const std::uint32_t protection = (region->readable ? read : 0U) | (region->writable ? write : 0U) |
                                         (region->executable ? execute : 0U);
        // Committed pages are resident: anonymous memory commits on first
        // touch. Engines that cannot tell count every accessible page.
        const auto committed = engine_resident_bytes(process.calling_engine(), region->base, region->size);
        const auto resident_pages = committed ? (*committed + page_size - 1) / page_size
                                              : protection ? region->size / page_size : 0;
        const auto resident = static_cast<std::uint32_t>(std::min<std::uint64_t>(resident_pages, UINT32_MAX));
        std::vector<std::uint8_t> bytes(words * 4, 0);
        const auto put32 = [&bytes](std::size_t offset, std::uint32_t value) {
            for (int index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
        };
        put32(0, protection);
        put32(4, read | write | execute);   // max_protection
        put32(8, inherit_copy);
        // offset (12, 8 bytes) and user_tag (20) stay 0.
        if (words == submap_short_info_count) {
            put32(24, 1);                                                  // ref_count
            bytes[31] = static_cast<std::uint8_t>(resident ? share_private : share_empty);
            // shadow_depth, external_pager, is_submap, behavior, object_id, user_wired_count: 0
        } else {
            put32(24, resident);                                           // pages_resident
            put32(36, region->writable ? resident : 0U);                   // pages_dirtied
            put32(40, 1);                                                  // ref_count
            bytes[47] = static_cast<std::uint8_t>(resident ? share_private : share_empty);
            // is_submap, behavior, object_id, user_wired_count, pages_reusable, object_id_full: 0
        }
        memory.write_bytes(info, bytes);
        memory.write64(address_output, region->base);
        memory.write64(size_output, region->size);
        memory.write32(depth_output, 0);
        memory.write32(count_output, words);
        return success;
    }

    std::uint32_t protect(transmute::CpuState& state) {
        if (!task(state.x[0])) return invalid_task;
        const auto maximum = static_cast<std::uint32_t>(state.x[3]);
        const auto permissions = static_cast<unsigned>(state.x[4]);
        if (maximum > 1 || (permissions & ~(read | write | execute)) != 0) return invalid_argument;
        if ((permissions & (write | execute)) == (write | execute)) return protection_failure;
        std::uint64_t begin{}, end{};
        if (!checked_range(state.x[1], state.x[2], begin, end)) return invalid_address;
        if (begin == end) return success;
        if (end - begin > allocation_limit) return resource_shortage;
        std::lock_guard lock(metadata);
        // Mach-owned pages, and mmap'd ones (Darwin's vm_protect covers those
        // too: mach_vm_map of a memory entry over mmap'd pages sets the
        // maximum of what vm_remap put there, Chrome's GPU ring buffer in V8's
        // sandbox among it).
        auto* space = darwin_vm_space(process);
        for (auto address = begin; address < end; address += page_size) {
            const auto found = pages.find(address);
            if (found == pages.end()) {
                if (!space || !space->intersects(address, page_size))
                    return process.calling_engine().query_region(address) ? not_supported : invalid_address;
                continue;
            }
            if ((permissions & ~found->second.maximum) != 0) return protection_failure;
        }
        for (auto address = begin; address < end; address += page_size) {
            const auto found = pages.find(address);
            if (found == pages.end()) {
                // mmap'd pages keep no maximum: setting one lowers the current
                // protection to it, as on Darwin, but a later mprotect above it
                // is not refused.
                auto current = permissions;
                if (maximum) {
                    const auto region = process.calling_engine().query_region(address);
                    const unsigned now = (region && region->readable ? read : 0U) | (region && region->writable ? write : 0U) |
                                         (region && region->executable ? execute : 0U);
                    if ((now & ~permissions) == 0) continue;
                    current = now & permissions;
                }
                if (!space->protect(address, page_size, current)) return not_supported;
                continue;
            }
            auto& page = found->second;
            const auto current = maximum ? page.current & permissions : permissions;
            process.calling_engine().set_memory_access(address, (current & read) != 0,
                (current & write) != 0, (current & execute) != 0);
            page.current = current;
            if (maximum) page.maximum = permissions;
        }
        return success;
    }

    std::uint32_t remap(transmute::CpuState& state, transmute::GuestMemory& memory) {
        if (!task(state.x[0]) || !task(state.x[5])) return invalid_task;
        const auto flags = static_cast<unsigned>(state.x[4]) & 0x00ffffffU;   // without the VM tag byte
        if ((flags & ~(anywhere | overwrite)) != 0 || (flags & anywhere && flags & overwrite))
            return invalid_argument;
        const auto refuse = [&](const char* why) {
            std::string target = "?";
            try { target = "0x" + hex(memory.read64(state.x[1])); } catch (...) {}
            notice("vm_remap(target " + target + ", size 0x" + hex(state.x[2]) + ", mask 0x" + hex(state.x[3]) + ", flags 0x" + hex(flags) +
                   ", source 0x" + hex(state.x[6]) + ", copy " + std::to_string(static_cast<std::uint32_t>(state.x[7])) +
                   ") -> KERN_NOT_SUPPORTED: " + why);
            return not_supported;
        };
        if (state.x[3] != 0) return refuse("alignment masks are unsupported");
        const bool copy = static_cast<std::uint32_t>(state.x[7]) != 0;
        // vm_remap's final three fixed arguments use Apple's ARM64 stack ABI.
        if (!accessible(memory, state.sp, 20, false)) return invalid_address;
        const auto current_output = memory.read64(state.sp);
        const auto maximum_output = memory.read64(state.sp + 8);
        // Inheritance only matters to fork, whose emulation shares the whole
        // address space: any valid value is accepted (V8 passes VM_INHERIT_NONE).
        if (memory.read32(state.sp + 16) > inherit_last_valid) return invalid_argument;
        if (!accessible(memory, state.x[1], 8, true) ||
            !accessible(memory, current_output, 4, true) ||
            !accessible(memory, maximum_output, 4, true)) return invalid_address;
        std::uint64_t begin{}, end{};
        if (state.x[6] % page_size != 0 || !checked_range(state.x[6], state.x[2], begin, end) ||
            begin == end || end - begin > allocation_limit) return invalid_address;
        if (!accessible(memory, begin, end - begin, false)) return invalid_address;
        const auto extent = end - begin;
        std::lock_guard lock(metadata);
        bool mach_target = true;   // the destination is free or Mach VM pages
        if ((flags & anywhere) == 0) {
            const auto target = memory.read64(state.x[1]);
            std::uint64_t target_begin{}, target_end{};
            if (target % page_size || !checked_range(target, extent, target_begin, target_end))
                return invalid_address;
            if (target_begin < end && begin < target_end) return invalid_argument;
            if ((flags & overwrite) == 0) {
                if (!process.calling_engine().is_range_available(target, extent)) return no_space;
            } else for (auto address = target_begin; address < target_end; address += page_size) {
                // Mach pages, or (checked when it is replaced) mmap'd memory.
                if (!pages.contains(address)) mach_target = false;
            }
            for (const auto output : {state.x[1], current_output, maximum_output}) {
                const auto output_size = output == state.x[1] ? 8ULL : 4ULL;
                if (output < target_end && target_begin < output + output_size) return invalid_argument;
            }
        }
        bool immutable = true, executable = true;
        for (auto address = begin; address < end; address += page_size) {
            const auto region = memory.query_region(address);
            if (!region || region->size - (address - region->base) < page_size) return invalid_address;
            if (region->writable && region->executable) return protection_failure;
            immutable = immutable && !region->writable;
            executable = executable && region->executable;
        }
        // Shared memory (vm_allocate on an engine with shared guest memory):
        // a real alias, the same pages at both addresses, keeping the source's
        // protections. Backing lives until both are deallocated.
        auto& engine = process.calling_engine();
        if (copy) return remap_copy(state, memory, begin, extent, flags, mach_target, executable, current_output, maximum_output, refuse);
        if (mach_target && engine_is_shared_memory(engine, begin, extent)) {
            std::uint64_t target = memory.read64(state.x[1]);
            if (flags & anywhere) {
                target = next_address;
                while (!engine.is_range_available(target, extent)) {
                    if (target > UINT64_MAX - extent - page_size) return no_space;
                    target += extent + page_size;
                }
            }
            const auto source_page = pages.find(begin);
            const Page permissions = source_page != pages.end() ? source_page->second : Page{};
            if (engine_alias_memory(engine, target, begin, extent, (permissions.current & read) != 0,
                                    (permissions.current & write) != 0, (permissions.current & execute) != 0)) {
                for (auto offset = std::uint64_t{}; offset < extent; offset += page_size) {
                    const auto from = pages.find(begin + offset);
                    pages[target + offset] = from != pages.end() ? from->second : permissions;
                }
                if (flags & anywhere) {
                    memory.write64(state.x[1], target);
                    next_address = target + extent + page_size;
                }
                memory.write32(current_output, permissions.current);
                memory.write32(maximum_output, permissions.maximum);
                return success;
            }
        }
        // Shared memory over mmap'd pages (Chrome maps shared memory into V8's
        // sandbox this way, a GPU command buffer's ring buffer among it): a real
        // alias takes their place. The pages stay mmap's (munmap, mprotect).
        if (!mach_target && (flags & overwrite) != 0 && engine_is_shared_memory(engine, begin, extent)) {
            const auto target = memory.read64(state.x[1]);
            const auto source_page = pages.find(begin);
            const Page permissions = source_page != pages.end() ? source_page->second : Page{};
            bool aliased = false;
            std::string why = "the engine has no shared-memory aliases";
            auto* space = darwin_vm_space(process);
            if (space && space->replace_with_alias(target, extent, [&] {
                    try {
                        aliased = engine_alias_memory(engine, target, begin, extent, (permissions.current & read) != 0,
                                                      (permissions.current & write) != 0, (permissions.current & execute) != 0);
                    } catch (const std::runtime_error& error) {
                        why = error.what();
                    }
                })) {
                if (!aliased) return refuse(("the alias could not take the mmap'd pages: " + why).c_str());
                memory.write32(current_output, permissions.current);
                memory.write32(maximum_output, permissions.maximum);
                return success;
            }
        }
        // A shared alias of memory that can never change (e.g. libffi's __TEXT
        // trampoline page) is observably a copy with the source's protection.
        // Other writable sources still need a real shared-backing alias.
        if (immutable && (flags & overwrite) != 0) {
            const auto target = memory.read64(state.x[1]);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(extent));
            memory.read_bytes(begin, bytes);
            const unsigned current = read | (executable ? execute : 0);
            if (!mach_target) {
                // Over mmap'd memory (V8 remapping its embedded builtins into
                // its code range): the pages stay mmap's (munmap, mprotect).
                auto* space = darwin_vm_space(process);
                bool replaced = false;
                try {
                    replaced = space && space->replace_with_copy(target, bytes, current);
                } catch (const DarwinVmError&) {
                    return resource_shortage;
                }
                if (!replaced) return refuse("the overwritten range is neither Mach VM pages nor mmap'd memory");
                // Executable image text copied: its translations can come from
                // the image's cache (validated by content, per region).
                if (executable) engine_code_copy(engine, target, begin, extent);
                memory.write32(current_output, current);
                memory.write32(maximum_output, current);
                return success;
            }
            for (auto address = target; address < target + extent; address += page_size)
                engine.set_memory_access(address, true, true, false);
            memory.write_bytes(target, bytes);
            for (auto address = target; address < target + extent; address += page_size) {
                engine.set_memory_access(address, true, false, executable);
                pages.at(address).current = current;
            }
            if (executable) engine_code_copy(engine, target, begin, extent);
            memory.write32(current_output, current);
            memory.write32(maximum_output, current);
            return success;
        }
        if (!mach_target) return refuse("only immutable or shared memory can be remapped over mmap'd memory");
        EngineAdapter::require("shared guest-memory alias mapping with destination subrange overwrite for Mach vm_remap");
    }

    // vm_remap(copy = TRUE): the target gets a copy-on-write copy of the
    // source, which the guest cannot tell from a copy made now. Mach pages at
    // the target (anywhere, a free fixed range, or Mach pages it overwrites),
    // or mmap'd pages it overwrites (they stay mmap's). The protections are
    // the source's. metadata held.
    template <typename Refuse>
    std::uint32_t remap_copy(transmute::CpuState& state, transmute::GuestMemory& memory, std::uint64_t begin,
                             std::uint64_t extent, unsigned flags, bool mach_target, bool executable,
                             std::uint64_t current_output, std::uint64_t maximum_output, const Refuse& refuse) {
        auto& engine = process.calling_engine();
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(extent));
        memory.read_bytes(begin, bytes);
        const auto source_page = pages.find(begin);
        Page permissions{};
        if (source_page != pages.end()) {
            permissions = source_page->second;
        } else {
            const auto region = memory.query_region(begin);
            permissions.current = (region && region->readable ? read : 0U) | (region && region->writable ? write : 0U) |
                                  (executable ? execute : 0U);
        }
        std::uint64_t target = memory.read64(state.x[1]);
        if ((flags & overwrite) != 0 && !mach_target) {
            auto* space = darwin_vm_space(process);
            bool replaced = false;
            try {
                replaced = space && space->replace_with_copy(target, bytes, permissions.current);
            } catch (const DarwinVmError&) {
                return resource_shortage;
            }
            if (!replaced) return refuse("the overwritten range is neither Mach VM pages nor mmap'd memory");
        } else {
            if (flags & anywhere) {
                target = next_address;
                while (!engine.is_range_available(target, extent)) {
                    if (target > UINT64_MAX - extent - page_size) return no_space;
                    target += extent + page_size;
                }
            } else if (flags & overwrite) {
                for (auto at = target; at < target + extent; at += page_size) {
                    engine.unmap_memory(at);
                    pages.erase(at);
                }
                forget_purgeable(target, target + extent);
            }
            if (engine_map_shared_memory(engine, target, extent, page_size)) {
                for (auto at = target; at < target + extent; at += page_size) pages[at] = permissions;
            } else {
                for (auto at = target; at < target + extent; at += page_size) {
                    engine.map_memory(at, static_cast<std::size_t>(page_size));
                    engine.set_memory_access(at, true, true, false);
                    pages[at] = permissions;
                }
            }
            memory.write_bytes(target, bytes);
            for (auto at = target; at < target + extent; at += page_size)
                engine.set_memory_access(at, (permissions.current & read) != 0, (permissions.current & write) != 0,
                                         (permissions.current & execute) != 0);
            if (flags & anywhere) {
                memory.write64(state.x[1], target);
                next_address = target + extent + page_size;
            }
        }
        if (executable) engine_code_copy(engine, target, begin, extent);
        memory.write32(current_output, permissions.current);
        memory.write32(maximum_output, permissions.maximum);
        return success;
    }

    void install() {
        const auto owned = shared_from_this();
        process.at_teardown([weak = std::weak_ptr<MachVm>(owned)] { if (const auto self = weak.lock()) self->release_all(); });
        const auto task_address = process.allocate_guest(4);
        process.calling_engine().write32(task_address, self_task);
        process.calling_engine().set_memory_access(task_address, true, false, false);
        process.install_export("/usr/lib/libSystem.B.dylib", "_mach_task_self_",
            {task_address, ExportKind::data});
        static_cast<void>(process.install_function("__dyld_shared_cache_contains_path", [](auto& state, auto& memory) {
            if (state.x[0] == 0) throw std::runtime_error("null dyld shared-cache path");
            bool terminated{};
            static_cast<void>(memory.read_string(state.x[0], 1, 65536, &terminated));
            if (!terminated) throw std::runtime_error("dyld shared-cache path exceeds 65535 bytes");
            state.x[0] = 0; // Alchemy has no Apple shared cache.
        }));
        const auto add = [&](const char* name, auto operation) {
            static_cast<void>(process.install_function(name, [owned, operation](auto& state, auto& memory) {
                try { state.x[0] = operation(*owned, state, memory); }
                catch (const std::bad_alloc&) { state.x[0] = resource_shortage; }
            }));
        };
        add("_vm_allocate", [](auto& vm, auto& state, auto& memory) { return vm.allocate(state, memory); });
        add("_vm_deallocate", [](auto& vm, auto& state, auto&) { return vm.deallocate(state); });
        add("_vm_protect", [](auto& vm, auto& state, auto&) { return vm.protect(state); });
        add("_vm_remap", [](auto& vm, auto& state, auto& memory) { return vm.remap(state, memory); });
        for (const auto* name : {"_mach_vm_region_recurse", "_vm_region_recurse_64"})
            add(name, [](auto& vm, auto& state, auto& memory) { return vm.region_recurse(state, memory); });
        for (const auto* name : {"_mach_vm_purgable_control", "_vm_purgable_control"})
            add(name, [](auto& vm, auto& state, auto& memory) { return vm.purgable_control(state, memory); });
    }
};

} // namespace

void install_darwin_mach_vm(ProcessContext& context) {
    const auto vm = std::make_shared<MachVm>(context);
    {
        std::lock_guard lock(registry_mutex);
        registry[&context] = vm;
    }
    vm->install();
}

namespace {
std::shared_ptr<MachVm> installed_vm(ProcessContext& context) {
    std::lock_guard lock(registry_mutex);
    const auto found = registry.find(&context);
    return found != registry.end() ? found->second.lock() : nullptr;
}
} // namespace

std::uint32_t map_darwin_section(ProcessContext& context, void* section, std::uint64_t offset, std::uint64_t size,
                                 bool writable_section, std::uint64_t& address, std::uint32_t protection,
                                 std::uint32_t maximum, bool replace) {
    const auto vm = installed_vm(context);
    return vm ? vm->map_section(section, offset, size, writable_section, address, protection, maximum, replace)
              : not_supported;
}

std::uint32_t allocate_darwin_vm(ProcessContext& context, std::uint64_t size, std::uint64_t& address) {
    const auto vm = installed_vm(context);
    return vm ? vm->allocate_anywhere(size, address) : not_supported;
}

bool register_darwin_purgeable(ProcessContext& context, std::uint64_t base, std::uint64_t size, bool purgeable) {
    const auto vm = installed_vm(context);
    if (!vm) return false;
    std::lock_guard lock(vm->metadata);
    if (purgeable) vm->purgeable[base] = MachVm::Purgeable{size};
    else vm->forget_purgeable(base, base + size);
    return true;
}

} // namespace alchemy
