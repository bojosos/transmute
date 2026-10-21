#pragma once

// Guest vectored handler registry and unhandled-exception filter.

#include "fx/seh/seh.hpp"

#include <cstdint>
#include <vector>

namespace transmute::fx::seh {

std::uint64_t add_vectored_exception_handler(bool first, std::uint64_t handler);
std::uint64_t add_vectored_continue_handler(bool first, std::uint64_t handler);
bool remove_vectored_exception_handler(std::uint64_t handle);
std::vector<std::uint64_t> vectored_exception_routines();
std::vector<std::uint64_t> vectored_continue_routines();

// The guest's unhandled-exception filter (SetUnhandledExceptionFilter). The
// dispatcher runs it through the built-in fault hook
// unhandled_filter_fault_hook (dispatch.cpp), before the personality's hook.
std::uint64_t set_unhandled_exception_filter(std::uint64_t filter);
std::uint64_t unhandled_exception_filter_routine();

// The personality fault hook (transmute::fx::set_guest_fault_hook).
struct FaultHookEntry {
    GuestFaultHook hook = nullptr;
    void* user = nullptr;
};
FaultHookEntry personality_fault_hook();

} // namespace transmute::fx::seh
