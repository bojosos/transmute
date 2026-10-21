# libSystem

libc, libm, pthreads, Mach, BSD files and sockets, processes, malloc zones,
os_log and the small libSystem libraries (libbsm, libnotify, libxpc pieces,
libproc, SysV IPC), for `/usr/lib/libSystem.B.dylib` and its re-exports.

* **Core** (alchemy_core, so alchemy.exe and alchemy-fx): `DarwinServices`
  (`services.cpp`, `services_impl.hpp`, `fs/files.cpp`, `net/sockets.cpp`,
  `process/{identity,process_model}.cpp`), `DarwinRuntime` (`runtime.cpp`),
  `misc/{support,locale,native,numeric*,libsystem_extra}.cpp`,
  `process/{threads,launch,shell,shared_wait,process_tree}.cpp`,
  `vm/`, `malloc/` and `fs/{record_locks,symlink_records}.cpp`. One owner per
  name: libm is `misc/numeric_math.cpp`'s.
* **GUI** (alchemy-fx): in-process and cross-process Mach IPC (`mach/`), and
  the pieces first written for a renderer plug-in (`*/libsystem_*.cpp`), Qt WebEngine
  (`mach/machsys.cpp`), Chrome (`misc/leftovers.cpp`) and QLab
  (`misc/{oslog,system_c,xpc,log_store,oslog_store}.cpp`,
  `process/{process_io,foundation_threads}.cpp`).
* **Headless-only shells** (one declaration, one definition per build):
  `mach/task_headless.cpp` (alchemy.exe: mach_port_deallocate, task_info,
  thread_policy_*), with the empty `mach/task_fx.cpp` in alchemy-fx, where
  `mach/mach_exports.cpp` owns those names.
* **Host dependencies:** Win32 files, handles, virtual memory and sections,
  waits, Winsock, process creation in a kill-on-close job, named semaphores
  and file mappings (SysV), and NT process information. Phase 2 moves them
  behind `include/alchemy/host/` (vm, sections, files, net, process, ipc,
  sync, time).
* **Unsupported** (each fails explicitly): guest signal delivery to other
  processes, real credentials (a virtual user 1000, or root with
  ALCHEMY_SUPERUSER=1), SysV shared memory, launchd services, sandboxing,
  terminfo.
* **Tests:** `tests/`, `mach/tests/`, `vm/tests/`. The headless
  `tests/alchemy_{darwin,services,threads,mach_vm,native,numeric,network}_tests.cpp`
  are at the repository root.

## Moved from darwin/identities/

* `misc/crt_and_host_info.cpp`: headless, every build: _NSGetArgc/_NSGetArgv, getchar, wcsstr/wcspbrk, host_info, tcgetattr/tcsetattr.
* Mach IPC's namespace is `libsystem::mach` (`install_mach_ipc`, `install_notify`; it was `chromesupport`).
