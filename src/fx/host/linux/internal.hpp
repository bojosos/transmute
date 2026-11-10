#pragma once

// Pieces the linux backend's files share; not part of any seam.

#include <cstdint>
#include <string>

namespace transmute::fx::host::linux_internal {

// Gives the calling thread an alternate signal stack, so the fault handler
// also runs when the fault is a stack overflow (fault.cpp). Idempotent.
void ensure_signal_stack() noexcept;

// If `address` lies on a page protected kGuard, restores the page's base
// protection (a guard page fires once, as on Windows) and returns true
// (vm.cpp).
bool take_guard_page(std::uint64_t address) noexcept;

// errno as the Win32 error code the seams report (thread::last_error): the
// common cases are mapped, others become 0x20000000 | errno (the
// customer-code bit, which Win32 codes never use). errno_of reverses it.
std::uint32_t win32_error(int error) noexcept;
int errno_of(std::uint32_t win32) noexcept;

// UTF-32 wide strings (wchar_t is 32 bits here) to and from UTF-8 (text.cpp).
std::string narrow(const std::wstring& text);
std::wstring widen(const std::string& text);

} // namespace transmute::fx::host::linux_internal
