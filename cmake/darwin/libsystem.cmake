# libSystem (src/alchemy/darwin/libsystem/; owner: refactor group r1): libc,
# libm, pthreads, Mach, BSD files, sockets and processes. Subareas: vm, mach,
# net, fs, process, malloc, misc. See cmake/AlchemyDarwin.cmake for the
# fragment pattern.

# Core (alchemy_core: alchemy.exe and alchemy-fx): DarwinServices,
# DarwinRuntime and what they install, the malloc zones and Chrome's
# libSystem records (installed with libSystem itself).
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/libsystem/runtime.cpp
    src/alchemy/darwin/libsystem/services.cpp
    src/alchemy/darwin/libsystem/fs/files.cpp
    src/alchemy/darwin/libsystem/fs/record_locks.cpp
    src/alchemy/darwin/libsystem/fs/symlink_records.cpp
    src/alchemy/darwin/libsystem/fs/terminal.cpp
    src/alchemy/darwin/libsystem/malloc/malloc_zones.cpp
    src/alchemy/darwin/libsystem/misc/locale.cpp
    src/alchemy/darwin/libsystem/misc/support.cpp
    src/alchemy/darwin/libsystem/misc/libc_extra.cpp
    src/alchemy/darwin/libsystem/misc/native.cpp
    src/alchemy/darwin/libsystem/misc/numeric.cpp
    src/alchemy/darwin/libsystem/misc/numeric_math.cpp
    src/alchemy/darwin/libsystem/misc/libsystem_extra.cpp
    src/alchemy/darwin/libsystem/net/sockets.cpp
    src/alchemy/darwin/libsystem/process/identity.cpp
    src/alchemy/darwin/libsystem/process/launch.cpp
    src/alchemy/darwin/libsystem/process/process_model.cpp
    src/alchemy/darwin/libsystem/process/process_tree.cpp
    src/alchemy/darwin/libsystem/process/shell.cpp
    src/alchemy/darwin/libsystem/process/threads.cpp
    src/alchemy/darwin/libsystem/process/shared_wait.cpp
    src/alchemy/darwin/libsystem/vm/mach_vm.cpp
    src/alchemy/darwin/libsystem/vm/vm_space.cpp
    src/alchemy/darwin/libsystem/vm/cross_process_memory.cpp
    src/alchemy/darwin/libsystem/mach/task_headless.cpp)
# Headless-only shells (alchemy.exe): names a GUI provider owns in alchemy-fx,
# which builds the empty <x>_fx.cpp counterpart below instead.
#   mach/task_headless.cpp  mach_port_deallocate, task_info, thread_policy_*
#                           (alchemy-fx: mach/mach_exports.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES
    src/alchemy/darwin/libsystem/mach/task_headless.cpp)

# GUI (alchemy-fx only).
#   install.cpp: libSystem's rows of the GUI installer table (install.hpp).
#   mach/{mach_ipc,mach_remote,transport,mach_exports,notify}.cpp: in-process
#     and cross-process Mach IPC (ports, port sets, notifications, mach_msg,
#     EVFILT_MACHPORT, bootstrap, memory entries), libsystem::install_mach's
#     row before the fill-only set.
#   mach/machsys.cpp: Mach task/thread calls, bootstrap/launchd, sandbox and
#     libc leftovers for strict mode, fill-only (gui::fill::install_machsys).
#   misc/leftovers.cpp: libSystem, libbsm and libbz2 leftovers (first imported by Chrome),
#     fill-only, installed last (gui::fill::install_libsystem_leftovers).
#   misc/{stdlib_extra,ndbm}.cpp, net/rpc_nis.cpp: rand48 and crypt, ndbm(3), NIS and
#     Sun RPC (unavailable); pieces of install_libsystem_gaps (fill-only).
#   */libsystem_*.cpp: libSystem names a renderer plug-in's images import, fill-only
#     (libsystem::install_libsystem_gaps and install_libsystem_services rows;
#     shared kit src/alchemy/darwin/kit/export_fill/).
#   misc/{system_c,xpc,log_store,oslog_store}.cpp, process/{process_io,
#     foundation_threads}.cpp: OSAtomic/OSSpinLock and other C leftovers, NSXPC*,
#     the os_log message store and OSLogStore, NSFileHandle/NSPipe/NSTask,
#     NSThread additions and Foundation locks (QLab runtime group;
#     installed by kit/completion/runtime/install.cpp, shared kit).
#   misc/oslog.cpp: os_log objects as Objective-C objects (installed by
#     kit/completion/install.cpp, shared kit).
#   misc/fts_err.cpp: fts(3), err/warn and _c_locale (from r2; installed by
#     the QLab runtime kit's install_cf_extras).
#   misc/sandbox.cpp: libsandbox (from r2; a values::Provider member,
#     installed by install_services in install_values_kit).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/libsystem/install.cpp
    src/alchemy/darwin/libsystem/mach/task_fx.cpp
    src/alchemy/darwin/libsystem/mach/mach_ipc.cpp
    src/alchemy/darwin/libsystem/mach/mach_remote.cpp
    src/alchemy/darwin/libsystem/mach/transport.cpp
    src/alchemy/darwin/libsystem/mach/mach_exports.cpp
    src/alchemy/darwin/libsystem/mach/notify.cpp
    src/alchemy/darwin/libsystem/mach/machsys.cpp
    src/alchemy/darwin/libsystem/misc/leftovers.cpp
    src/alchemy/darwin/libsystem/net/libsystem_net.cpp
    src/alchemy/darwin/libsystem/process/libsystem_ipc.cpp
    src/alchemy/darwin/libsystem/process/libsystem_sysv.cpp
    src/alchemy/darwin/libsystem/process/libsystem_process.cpp
    src/alchemy/darwin/libsystem/misc/libsystem_runtime.cpp
    src/alchemy/darwin/libsystem/misc/libsystem_misc.cpp
    src/alchemy/darwin/libsystem/misc/libsystem_services.cpp
    src/alchemy/darwin/libsystem/misc/xpc_objects.cpp
    src/alchemy/darwin/libsystem/misc/stdlib_extra.cpp
    src/alchemy/darwin/libsystem/misc/ndbm.cpp
    src/alchemy/darwin/libsystem/net/rpc_nis.cpp
    src/alchemy/darwin/libsystem/terminal/readline.cpp
    src/alchemy/darwin/libsystem/terminal/curses.cpp
    src/alchemy/darwin/libsystem/terminal/panel.cpp
    src/alchemy/darwin/libsystem/process/threads_extra.cpp
    src/alchemy/darwin/libsystem/process/foundation_threads.cpp
    src/alchemy/darwin/libsystem/process/process_io.cpp
    src/alchemy/darwin/libsystem/misc/system_c.cpp
    src/alchemy/darwin/libsystem/misc/log_store.cpp
    src/alchemy/darwin/libsystem/misc/oslog_store.cpp
    src/alchemy/darwin/libsystem/misc/xpc.cpp
    src/alchemy/darwin/libsystem/misc/oslog.cpp
    src/alchemy/darwin/libsystem/misc/fts_err.cpp
    src/alchemy/darwin/libsystem/misc/sandbox.cpp)

function(alchemy_gui_libsystem_tests)
    # libsystem::darwin_errno, the host::Error translation every framework uses.
    alchemy_gui_add_unit_test(alchemy_gui_libsystem_errno src/alchemy/darwin/libsystem/tests/errno_tests.cpp)
    # Mach IPC. Process teardown with live vm_allocate memory and memory
    # entries (no host fault at exit).
    alchemy_gui_add_unit_test(alchemy_gui_mach_ipc src/alchemy/darwin/libsystem/mach/tests/mach_tests.cpp TIMEOUT 120)
    foreach(teardown IN ITEMS vm entry mapped-entry)
        add_test(NAME alchemy_gui_mach_ipc_teardown_${teardown} COMMAND alchemy_gui_mach_ipc teardown-${teardown})
        set_tests_properties(alchemy_gui_mach_ipc_teardown_${teardown} PROPERTIES TIMEOUT 60
            PASS_REGULAR_EXPRESSION "teardown ${teardown}: ok")
    endforeach()
    alchemy_gui_add_unit_test(alchemy_gui_machsys src/alchemy/darwin/libsystem/mach/tests/machsys_tests.cpp TIMEOUT 120)
    # The cross-process memory core (include/alchemy/cross_process_memory.hpp)
    # between real alchemy-fx host processes.
    alchemy_gui_add_unit_test(alchemy_gui_cross_process_memory src/alchemy/darwin/libsystem/vm/tests/xmem_tests.cpp TIMEOUT 180)
    # Chrome's libSystem names; the strict-mode fill fixture installs every provider.
    alchemy_gui_add_unit_test(alchemy_gui_libsystem_leftovers
        src/alchemy/darwin/libsystem/tests/libsystem_leftovers_tests.cpp TIMEOUT 120)
    # The pieces first written for a renderer plug-in (split from the export-fill tests; shared
    # fixture src/alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp).
    alchemy_gui_add_unit_test(alchemy_gui_libsystem_gaps
        src/alchemy/darwin/libsystem/tests/libsystem_gaps_tests.cpp TIMEOUT 120)
    # misc/libc_extra.cpp: string, wide, search.h, stdlib, time, ctype, signal and path helpers.
    alchemy_gui_add_unit_test(alchemy_gui_libc_extra
        src/alchemy/darwin/libsystem/tests/libc_extra_tests.cpp TIMEOUT 120)
    # misc/xpc_objects.cpp: XPC objects, and connections whose service is not there.
    alchemy_gui_add_unit_test(alchemy_gui_xpc
        src/alchemy/darwin/libsystem/tests/xpc_tests.cpp TIMEOUT 120)
    # terminal/: libedit readline, curses and panels over a pseudo-terminal.
    alchemy_gui_add_unit_test(alchemy_gui_terminal
        src/alchemy/darwin/libsystem/tests/terminal_tests.cpp TIMEOUT 120)
    # The pieces first written for QLab (split from the QLab runtime tests;
    # shared fixture src/alchemy/darwin/kit/completion/runtime/tests/runtime_test_kit.hpp).
    alchemy_gui_add_unit_test(alchemy_gui_libsystem_foundation
        src/alchemy/darwin/libsystem/tests/foundation_system_tests.cpp TIMEOUT 120)
endfunction()
alchemy_gui_register_tests(alchemy_gui_libsystem_tests)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/libsystem/misc/crt_and_host_info.cpp)
