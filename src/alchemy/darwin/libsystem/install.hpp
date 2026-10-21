#pragma once

#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/kit/completion/runtime/common.hpp"

// libSystem's rows of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only). The core (DarwinServices and what it installs, mach_vm,
// native, numeric, threads) is installed by alchemy::DarwinProviders.
namespace alchemy::libsystem {

// Networking, SysV and POSIX IPC, process queries, runtime and misc leftovers
// (net/, process/ and misc/libsystem_*.cpp; fill-only), at the position of
// the former gap-fill group.
void install_libsystem_gaps(gui::InstallContext& context);
// ptrace, xpc dates, __getdirentries64, fsctl, mach_vm_region
// (misc/libsystem_services.cpp; fill-only), later in that position.
void install_libsystem_services(gui::InstallContext& context);
// In-process and cross-process Mach IPC (mach/mach_exports.cpp) and notify(3)
// (mach/notify.cpp), before the fill-only set (the former install_chrome_support).
void install_mach(gui::InstallContext& context);
// Its pieces: Mach IPC (mach/mach_exports.cpp; namespace libsystem::mach) and
// notify(3) over it (mach/notify.cpp), in that order.
void install_mach_ipc(gui::InstallContext& context);
void install_notify(gui::InstallContext& context);

// Installed by the shared QLab kit's aggregators, in their order (they take its
// gui::completion_kit::Support and complete classes other providers declare):
using gui::completion_kit::install_os_log;            // misc/oslog.cpp (kit/completion/install.cpp)
using gui::completion_kit::install_threads;           // process/foundation_threads.cpp (runtime/install.cpp)
using gui::completion_kit::install_locks;             // process/foundation_threads.cpp
using gui::completion_kit::install_process_io;        // process/process_io.cpp
using gui::completion_kit::install_system_functions;  // misc/system_c.cpp
using gui::completion_kit::install_os_log_store;      // misc/oslog_store.cpp
using gui::completion_kit::install_xpc;               // misc/xpc.cpp

} // namespace alchemy::libsystem
