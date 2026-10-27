#pragma once

#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/kit/completion/runtime/common.hpp"

namespace alchemy::export_fill { class Provider; }

// libSystem's rows of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only). The core (DarwinServices and what it installs, mach_vm,
// native, numeric, threads) is installed by alchemy::DarwinProviders.
namespace alchemy::libsystem {

// Networking, SysV and POSIX IPC, process queries, runtime and misc leftovers
// (net/, process/ and misc/libsystem_*.cpp; fill-only), at the position of
// the former gap-fill group.
void install_libsystem_gaps(gui::InstallContext& context);
// ptrace, __getdirentries64, fsctl, mach_vm_region (misc/libsystem_services.cpp)
// and the XPC C API (misc/xpc_objects.cpp); fill-only, later in that position.
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

// Pieces of install_libsystem_gaps (fill-only, shared kit src/alchemy/darwin/kit/export_fill/).
namespace alchemy::export_fill {
void install_libsystem_stdlib(Provider& p);     // misc/stdlib_extra.cpp: rand48, crypt
void install_libsystem_ndbm(Provider& p);       // misc/ndbm.cpp: ndbm(3)
void install_libsystem_rpc_nis(Provider& p);    // net/rpc_nis.cpp: NIS and Sun RPC (unavailable)
void install_libedit_readline(Provider& p);     // terminal/readline.cpp: libedit's readline API
void install_ncurses(Provider& p);              // terminal/curses.cpp: curses over an xterm-compatible terminal
void install_libpanel(Provider& p);             // terminal/panel.cpp: libpanel over the curses windows
void install_libsystem_xpc(Provider& p, ObjcRuntime& objc);   // misc/xpc_objects.cpp: XPC objects and connections
void install_libsystem_threads_extra(Provider& p);   // process/threads_extra.cpp: QoS, stack attributes, ceilings
void install_libsystem_bsd_extra(Provider& p);       // misc/bsd_extra.cpp: BSD libc leftovers (string, stdlib, pwcache, ...)
void install_libsystem_netdb_extra(Provider& p);     // net/netdb_extra.cpp: netdb leftovers and the /etc databases
void install_libsystem_mach_extra(Provider& p);      // mach/mach_extra.cpp: the MIG server loop, mig_*, clock_sleep, ...
} // namespace alchemy::export_fill
