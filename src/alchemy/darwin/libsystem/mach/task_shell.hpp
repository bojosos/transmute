#pragma once

#include "alchemy/process_context.hpp"

#include <cstdint>

namespace alchemy::libsystem {

// mach_task_self() of alchemy.exe (misc/libsystem_extra.cpp): the only task port.
inline constexpr std::uint32_t headless_task_port = 0xa1c1;

// mach_port_deallocate, task_info, thread_policy_set and thread_policy_get of
// the calling task. One declaration, one definition per build:
//   mach/task_headless.cpp  alchemy.exe (ALCHEMY_HEADLESS_ONLY_SOURCES);
//   mach/task_fx.cpp        alchemy-fx, empty: mach/mach_exports.cpp owns these
//                           names there (gui::install_chrome_support).
// Installed by install_darwin_libsystem_extra.
void install_mach_task_shell(ProcessContext& context);

} // namespace alchemy::libsystem
