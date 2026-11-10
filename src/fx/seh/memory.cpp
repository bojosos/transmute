// Guarded guest memory access for the unwinder and dispatcher.

#include "fx/seh/seh.hpp"
#include "fx/host/guard.hpp"

#include <cstring>

namespace transmute::fx::seh {
namespace {

// memcpy raises nothing but memory faults (access violations, in-page
// errors, guard pages, misalignment), so any fault means the copy failed.
bool copy_guarded(void* destination, const void* source, std::size_t size) noexcept {
    host::FaultRecord fault{};
    return host::call_guarded([&] { std::memcpy(destination, source, size); }, fault);
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
