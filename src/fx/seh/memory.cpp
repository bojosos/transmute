// Guarded guest memory access for the unwinder and dispatcher.

#include "fx/seh/seh.hpp"

#include <cstring>

namespace transmute::fx::seh {
namespace {

int filter_memory_fault(unsigned code) noexcept {
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
            code == EXCEPTION_GUARD_PAGE || code == EXCEPTION_DATATYPE_MISALIGNMENT)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool copy_guarded(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (filter_memory_fault(GetExceptionCode())) {
        return false;
    }
}

} // namespace

bool read_guest(std::uint64_t address, void* out, std::size_t size) noexcept {
    if (size == 0) return true;
    if (address == 0 || address + size < address) return false;
    return copy_guarded(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), size);
}

bool write_guest(std::uint64_t address, const void* data, std::size_t size) noexcept {
    if (size == 0) return true;
    if (address == 0 || address + size < address) return false;
    return copy_guarded(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), data, size);
}

} // namespace transmute::fx::seh
