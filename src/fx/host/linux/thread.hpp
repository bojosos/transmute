#pragma once

// linux backend of fx/host/thread.hpp: the inline thread queries. There is
// no thread environment block; the stack base is read once per thread.

#include <cstdint>

namespace transmute::fx::host {

namespace linux_thread {
std::uint32_t read_thread_id() noexcept;
std::uint64_t read_stack_base() noexcept;
inline thread_local std::uint32_t t_id = 0;
inline thread_local std::uint64_t t_stack_base = 0;
} // namespace linux_thread

inline std::uint32_t current_thread_id() noexcept {
    if (linux_thread::t_id == 0) linux_thread::t_id = linux_thread::read_thread_id();
    return linux_thread::t_id;
}

inline void* thread_block() noexcept { return nullptr; }

inline std::uint64_t thread_stack_base() noexcept {
    if (linux_thread::t_stack_base == 0) linux_thread::t_stack_base = linux_thread::read_stack_base();
    return linux_thread::t_stack_base;
}

} // namespace transmute::fx::host
