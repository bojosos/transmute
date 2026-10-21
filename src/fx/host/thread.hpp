#pragma once

// Host area "thread": host threads, the per-thread error code and the
// thread environment block (docs/FX_HOST_SEAMS.md, "thread").
//
// Thread handles are opaque host handles (a Win32 HANDLE; a pthread record on
// Linux). create_thread's flags are the Win32 CreateThread flags, which the
// PE abi passes through from the guest; kThreadStackReservation makes
// `stack_size` the reserve instead of the commit.

#include "fx/host/host.hpp"

#include <cstddef>
#include <cstdint>

#if defined(TRANSMUTE_FX_HOST_WIN32)
#include "fx/host/win32/thread.hpp"
#endif

namespace transmute::fx::host {

using ThreadHandle = void*;
// The entry point of a host thread; returns the thread's exit code.
using ThreadEntry = std::uint32_t (*)(void* argument);

inline constexpr std::uint32_t kThreadSuspended = 0x4u;             // CREATE_SUSPENDED
inline constexpr std::uint32_t kThreadStackReservation = 0x10000u;  // STACK_SIZE_PARAM_IS_A_RESERVATION

enum class ThreadPriority { normal, below_normal, idle, above_normal, highest };

// Creates a host thread; nullptr on failure (last_error() says why).
ThreadHandle create_thread(ThreadEntry entry, void* argument, std::size_t stack_size, std::uint32_t flags,
                           std::uint32_t* thread_id) noexcept;
void close_thread(ThreadHandle thread) noexcept;
// The calling thread (a pseudo handle: never closed).
ThreadHandle current_thread() noexcept;
void set_thread_priority(ThreadHandle thread, ThreadPriority priority) noexcept;
// Restricts `thread` to the logical processors in `mask` (bit n = processor n).
void set_thread_affinity(ThreadHandle thread, std::uint64_t mask) noexcept;
// Efficiency-class scheduling (Windows EcoQoS: execution-speed throttling),
// so the thread runs on efficiency cores. No-op where the host has none.
void set_thread_eco_qos(ThreadHandle thread) noexcept;
[[noreturn]] void exit_thread(std::uint32_t exit_code) noexcept;
// Gives the rest of this time slice to another ready thread (SwitchToThread;
// sched_yield).
void yield_thread() noexcept;
// Runs work(context) on a host thread-pool thread (TrySubmitThreadpoolCallback);
// false if it could not be queued (the caller runs it some other way).
bool submit_to_thread_pool(void (*work)(void* context), void* context) noexcept;

// The calling thread's host error code (GetLastError; errno on Linux).
std::uint32_t last_error() noexcept;
void set_last_error(std::uint32_t error) noexcept;

// inline (backend): std::uint32_t current_thread_id() noexcept;
// inline (backend): void* thread_block() noexcept
//     the host thread environment block (the TEB; nullptr where there is none)
// inline (backend): std::uint64_t thread_stack_base() noexcept
//     the upper end of the calling host thread's stack

// Copies up to `size` bytes of the calling thread's environment block into
// `out` (the shadow TEB starts as a copy of the host's); returns the bytes
// copied (0 where the host has no TEB).
std::size_t copy_thread_block(void* out, std::size_t size) noexcept;

// Activation contexts (Windows side-by-side assemblies; PE guests only).
// activate_context() pushes `context` on the calling thread's activation
// stack and returns the cookie for deactivate_context(), or 0 if it could
// not. No-op (0) on hosts without them.
std::uintptr_t activate_context(void* context) noexcept;
void deactivate_context(std::uintptr_t cookie) noexcept;

// `hook` runs on every host thread that exits, after the thread's fiber-local
// storage callbacks and before its C++ thread_local destructors (a PE TLS
// callback in .CRT$XLB on win32). One hook per process.
void set_thread_exit_hook(void (*hook)()) noexcept;

} // namespace transmute::fx::host
