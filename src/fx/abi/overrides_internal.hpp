#pragma once

// Helpers shared by the override implementations (owner: abi).

#include "fx/abi/abi_internal.hpp"
#include "transmute/fx/process.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace transmute::fx::abi {

std::wstring guest_wide(std::uint64_t address);
std::wstring guest_ansi_to_wide(std::uint64_t address);
std::string wide_to_ansi(const std::wstring& text);
bool is_current_process(std::uint64_t handle);
std::uint64_t main_guest_module_base();
// Guest module whose base is `handle` (0 = the main image), or nullptr.
const GuestModule* guest_module_by_handle(std::uint64_t handle);

// Guest stack reserve for a CreateThread-style (size, flags) request: 0 =
// image default; STACK_SIZE_PARAM_IS_A_RESERVATION passes `size` through;
// otherwise `size` is the commit and the reserve is the image default or the
// commit rounded up to 1 MiB. Large sizes are never rejected.
std::uint64_t thread_stack_reserve(std::uint64_t size, std::uint32_t flags);
// create_guest_thread with the stack policy above. Returns the handle or 0.
std::uint64_t start_guest_thread(std::uint64_t entry, std::uint64_t argument, std::uint64_t stack_size,
                                 std::uint32_t flags, std::uint32_t* thread_id);

OverrideHandler find_crt_override(std::string_view name) noexcept;
std::size_t crt_override_count() noexcept;
OverrideHandler find_fiber_override(std::string_view name) noexcept;
std::size_t fiber_override_count() noexcept;

} // namespace transmute::fx::abi
