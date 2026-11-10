// Linux host backend, area "vm".
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/vm.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace alchemy::host::vm {

// The 64 KiB granularity matches Windows, so the macOS layer and fx align
// reservations the same way on both hosts.
PageInfo page_info() noexcept { return {4096, 65536}; }

Result<Region> query(std::uintptr_t address) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return linux_backend::last_error();
    char line[512];
    Region region{};
    region.base = address & ~std::uintptr_t{0xfff};
    std::uintptr_t next = 0x00007ffffffff000ull;
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long begin = 0, end = 0;
        char perms[8]{};
        if (std::sscanf(line, "%llx-%llx %7s", &begin, &end, perms) != 3) continue;
        if (address >= end) continue;
        if (address < begin) {
            next = begin;
            break;
        }
        region.base = static_cast<std::uintptr_t>(begin);
        region.size = static_cast<std::size_t>(end - begin);
        Protection protection = Protection::none;
        if (perms[0] == 'r') protection = protection | Protection::read;
        if (perms[1] == 'w') protection = protection | Protection::write;
        if (perms[2] == 'x') protection = protection | Protection::execute;
        region.protection = protection;
        // A PROT_NONE mapping is reserved address space, not memory.
        region.committed = protection != Protection::none;
        found = true;
        break;
    }
    std::fclose(maps);
    if (!found) region.size = next - region.base;
    return region;
}

Status lock(std::uintptr_t address, std::size_t size) {
    if (mlock(reinterpret_cast<void*>(address), size) == 0) return {};
    return errno == ENOMEM || errno == EAGAIN ? Error::would_block : linux_backend::last_error();
}

Status unlock(std::uintptr_t address, std::size_t size) {
    if (munlock(reinterpret_cast<void*>(address), size) == 0) return {};
    return linux_backend::last_error();
}

} // namespace alchemy::host::vm
