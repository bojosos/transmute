#pragma once

// win32 backend of fx/host/thread.hpp: the inline thread queries.

#include "fx/host/win32/kernel32.hpp"

#include <intrin.h>

#include <cstdint>

namespace transmute::fx::host {

inline std::uint32_t current_thread_id() noexcept { return GetCurrentThreadId(); }

// NtCurrentTeb(): the TEB's NT_TIB::Self, at gs:[0x30] on x64.
inline void* thread_block() noexcept { return reinterpret_cast<void*>(__readgsqword(0x30)); }

// NT_TIB::StackBase of the calling thread (gs:[0x08] on x64).
inline std::uint64_t thread_stack_base() noexcept { return __readgsqword(0x08); }

} // namespace transmute::fx::host
