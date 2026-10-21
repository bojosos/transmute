#pragma once

// Darwin malloc zones (libmalloc's public malloc/malloc.h API) over the
// process allocator of DarwinServices.
//
// The process allocator is the system zone: a malloc_zone_t whose function
// table (adapters over the host heap) DarwinServices builds. Guests may
// register their own zones (PartitionAlloc's delegating zone and its malloc
// zone, docs/ALCHEMY_CHROME_AUDIT.md) and make one of them the default zone
// with libmalloc's register/unregister order: malloc_zone_register appends,
// malloc_zone_unregister moves the last zone into the freed slot, and zone 0
// is the default zone. Once a guest zone is zone 0, malloc, calloc, valloc,
// realloc of a NULL pointer and the aligned allocators call its function
// table; free, realloc and malloc_size go to the zone that owns the pointer
// (the system zone, or the first guest zone whose size callback claims it).
// While the system zone is zone 0, every call stays on the host heap with no
// guest call.

#include "alchemy/process_context.hpp"

#include <cstdint>
#include <functional>

namespace alchemy::libsystem {

// The system zone's heap. allocate returns zero-filled memory, or 0 with the
// guest errno set; release rejects a pointer it does not own (an error, as
// Darwin's free of an unallocated pointer); owns and size look a block up.
struct HostHeap {
    std::function<std::uint64_t(std::uint64_t size, std::uint64_t alignment)> allocate;
    std::function<void(std::uint64_t address)> release;
    std::function<bool(std::uint64_t address)> owns;
    std::function<std::uint64_t(std::uint64_t address)> size;
    // malloc_statistics_t of the system zone: blocks, bytes in use, peak bytes.
    std::function<void(std::uint64_t& blocks, std::uint64_t& bytes, std::uint64_t& peak)> statistics;
};

// Installs malloc, calloc, valloc, free, realloc, reallocf, malloc_size,
// posix_memalign, aligned_alloc, the malloc_zone_* calls, malloc_default_zone,
// malloc_default_purgeable_zone, malloc_get_all_zones, malloc_create_zone,
// malloc_destroy_zone, malloc_{get,set}_zone_name and mstats into libSystem.
// `system_zone` is the guest address of the system zone's malloc_zone_t.
void install_malloc_zones(ProcessContext& context, HostHeap heap, std::uint64_t system_zone);

// malloc_zone_t (malloc/malloc.h): byte offsets of the members used here.
namespace zone_layout {
inline constexpr std::uint64_t size = 16, malloc = 24, calloc = 32, valloc = 40, free = 48, realloc = 56,
    destroy = 64, zone_name = 72, batch_malloc = 80, batch_free = 88, introspect = 96, version = 104,
    memalign = 112, free_definite_size = 120, pressure_relief = 128, claimed_address = 136;
// malloc_introspection_t: one pointer per member, then zone_type (unsigned).
namespace introspection {
inline constexpr std::uint64_t enumerator = 0, good_size = 8, check = 16, print = 24, log = 32, force_lock = 40,
    force_unlock = 48, statistics = 56, zone_locked = 64, enable_discharge_checking = 72,
    disable_discharge_checking = 80, discharge = 88, enumerate_discharged_pointers = 96, reinit_lock = 104,
    print_task = 112, task_statistics = 120, zone_type = 128, size = 136;
} // namespace introspection
inline constexpr std::uint64_t introspect_statistics = introspection::statistics;
// The version the system zone advertises: every member up to claimed_address (10) is set.
inline constexpr std::uint32_t system_version = 10;
} // namespace zone_layout

} // namespace alchemy::libsystem
