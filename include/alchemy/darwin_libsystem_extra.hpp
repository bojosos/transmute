#pragma once

#include "alchemy/process_context.hpp"

#include <cstdint>
#include <functional>

namespace alchemy {

// The calling thread's Darwin signal mask, owned by DarwinServices. setjmp and
// ucontext use it directly: a nested guest callback is not always possible
// there (the reference engine requires its own stack for callbacks).
struct SignalMaskAccess {
    std::function<std::uint32_t()> get;
    std::function<void(std::uint32_t)> set;
};

// Independently authored libSystem pieces beyond the base services: the Blocks
// runtime, setjmp/longjmp and ucontext register transfer, Mach semaphores and
// task/thread/host queries, POSIX regex/fnmatch/glob/scandir, syslog and small
// libc leftovers. Installed by DarwinServices after its own exports.
void install_darwin_libsystem_extra(ProcessContext& context, SignalMaskAccess masks);

} // namespace alchemy
