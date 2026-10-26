// win32 backend of fx/host/vm.hpp.

#include "fx/host/vm.hpp"

#include <windows.h>

#pragma comment(lib, "onecore.lib")   // VirtualAlloc2, MapViewOfFile3, UnmapViewOfFile2, CreateFileMapping2

namespace transmute::fx::host::vm {

static_assert(kNoAccess == PAGE_NOACCESS && kReadOnly == PAGE_READONLY && kReadWrite == PAGE_READWRITE &&
              kWriteCopy == PAGE_WRITECOPY && kExecute == PAGE_EXECUTE && kExecuteRead == PAGE_EXECUTE_READ &&
              kExecuteReadWrite == PAGE_EXECUTE_READWRITE && kExecuteWriteCopy == PAGE_EXECUTE_WRITECOPY &&
              kGuard == PAGE_GUARD && kNoCache == PAGE_NOCACHE && kWriteCombine == PAGE_WRITECOMBINE);

bool query(const void* address, RegionInfo& info) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    info.base = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
    info.allocation_base = reinterpret_cast<std::uint64_t>(mbi.AllocationBase);
    info.size = mbi.RegionSize;
    info.protect = mbi.Protect;
    info.allocation_protect = mbi.AllocationProtect;
    info.state = mbi.State == MEM_COMMIT ? State::committed : mbi.State == MEM_RESERVE ? State::reserved : State::free;
    info.type = mbi.Type == MEM_PRIVATE ? Type::private_memory
                : mbi.Type == MEM_MAPPED ? Type::mapped
                : mbi.Type == MEM_IMAGE  ? Type::image
                                         : Type::none;
    return true;
}

void* allocate(void* address, std::size_t size, Protection protect) noexcept {
    return VirtualAlloc(address, size, MEM_RESERVE | MEM_COMMIT, protect);
}

void* reserve(void* address, std::size_t size, Protection protect, bool top_down) noexcept {
    return VirtualAlloc(address, size, MEM_RESERVE | (top_down ? MEM_TOP_DOWN : 0), protect);
}

void* reserve_above(std::uint64_t lowest, std::size_t size) noexcept {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(lowest);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return VirtualAlloc2(nullptr, nullptr, size, MEM_RESERVE, PAGE_NOACCESS, &parameter, 1);
}

void* commit(void* address, std::size_t size, Protection protect) noexcept {
    return VirtualAlloc(address, size, MEM_COMMIT, protect);
}

bool decommit(void* address, std::size_t size) noexcept { return VirtualFree(address, size, MEM_DECOMMIT) != FALSE; }

bool release(void* address) noexcept { return VirtualFree(address, 0, MEM_RELEASE) != FALSE; }

bool protect(void* address, std::size_t size, Protection protection, Protection* old) noexcept {
    DWORD previous = 0;
    bool ok = VirtualProtect(address, size, protection, &previous) != FALSE;
    if (!ok && ((protection & 0xff) == PAGE_READWRITE || (protection & 0xff) == PAGE_EXECUTE_READWRITE)) {
        // A copy-on-write view (map_file_copy_guest_memory) refuses PAGE_READWRITE, even on pages
        // already copied: writable there means write-copy.
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Type == MEM_MAPPED &&
            (mbi.AllocationProtect == PAGE_WRITECOPY || mbi.AllocationProtect == PAGE_EXECUTE_WRITECOPY)) {
            const DWORD copy = (protection & ~0xffu) |
                ((protection & 0xff) == PAGE_READWRITE ? PAGE_WRITECOPY : PAGE_EXECUTE_WRITECOPY);
            ok = VirtualProtect(address, size, copy, &previous) != FALSE;
        }
    }
    if (old) *old = previous;
    return ok;
}

void discard(void* address, std::size_t size) noexcept {
    static_cast<void>(VirtualAlloc(address, size, MEM_RESET, PAGE_NOACCESS));
}

void prefetch(const void* address, std::size_t size) noexcept {
    WIN32_MEMORY_RANGE_ENTRY range{const_cast<void*>(address), size};
    static_cast<void>(PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0));
}

void flush_instruction_cache(const void* address, std::size_t size) noexcept {
    FlushInstructionCache(GetCurrentProcess(), address, size);
}

void* reserve_placeholder(void* address, std::size_t size) noexcept {
    return VirtualAlloc2(nullptr, address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
}

bool split_placeholder(void* address, std::size_t size) noexcept {
    return VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

bool coalesce_placeholders(void* address, std::size_t size) noexcept {
    return VirtualFree(address, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != FALSE;
}

void* reserve_placeholder_above(std::uint64_t lowest, std::size_t size) noexcept {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(lowest);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return VirtualAlloc2(nullptr, nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &parameter, 1);
}

bool replace_placeholder_reserved(void* address, std::size_t size) noexcept {
    return VirtualAlloc2(nullptr, address, size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0) ==
           address;
}

bool release_to_placeholder(void* address, std::size_t size) noexcept {
    return VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

Section create_section(std::uint64_t size) noexcept {
    return CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_READ | FILE_MAP_WRITE, PAGE_READWRITE, SEC_COMMIT,
                              size, nullptr, nullptr, 0);
}

Section duplicate_section(Section section) noexcept {
    HANDLE copy{};
    if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    return copy;
}

void close_section(Section section) noexcept {
    if (section) CloseHandle(section);
}

void* map_view_into_placeholder(Section section, void* address, std::uint64_t offset, std::size_t size,
                                Protection protect) noexcept {
    return MapViewOfFile3(section, GetCurrentProcess(), address, offset, size, MEM_REPLACE_PLACEHOLDER, protect,
                          nullptr, 0);
}

bool unmap_view_to_placeholder(void* address) noexcept {
    return UnmapViewOfFile2(GetCurrentProcess(), address, MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

Heap create_heap() noexcept {
    const HANDLE created = HeapCreate(0, 0, 0);
    ULONG low_fragmentation = 2;
    if (created) HeapSetInformation(created, HeapCompatibilityInformation, &low_fragmentation, sizeof(low_fragmentation));
    return created;
}

void* heap_allocate(Heap heap, std::size_t size) noexcept { return HeapAlloc(heap, HEAP_ZERO_MEMORY, size); }

bool heap_free(Heap heap, void* block) noexcept { return HeapFree(heap, 0, block) != FALSE; }

} // namespace transmute::fx::host::vm
