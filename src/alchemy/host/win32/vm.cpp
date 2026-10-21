#include "alchemy/host/vm.hpp"

#include "alchemy/host/win32/win32.hpp"

namespace alchemy::host::vm {

PageInfo page_info() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return {info.dwPageSize, info.dwAllocationGranularity};
}

Result<Region> query(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information))) return win32::last_error();
    Region region;
    region.base = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
    region.size = information.RegionSize;
    region.committed = information.State == MEM_COMMIT;
    if (region.committed) {
        const auto protect = information.Protect & 0xff;
        Protection value = Protection::none;
        if (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            value = value | Protection::read;
        if (protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) value = value | Protection::write;
        if (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) value = value | Protection::execute;
        region.protection = value;
    }
    return region;
}

Status lock(std::uintptr_t address, std::size_t size) {
    if (!VirtualLock(reinterpret_cast<void*>(address), size)) {
        const auto code = GetLastError();
        return code == ERROR_WORKING_SET_QUOTA || code == ERROR_NO_SYSTEM_RESOURCES ? Error::would_block : win32::error(code);
    }
    return {};
}

Status unlock(std::uintptr_t address, std::size_t size) {
    if (!VirtualUnlock(reinterpret_cast<void*>(address), size) && GetLastError() != ERROR_NOT_LOCKED) return win32::last_error();
    return {};
}

} // namespace alchemy::host::vm
