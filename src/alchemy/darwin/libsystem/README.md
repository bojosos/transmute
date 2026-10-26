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
* **Core libc additions** (both builds, `misc/libc_extra.cpp`, installed last by
  `install_darwin_support`): string and wide-string helpers, `<search.h>`,
  stdlib (putenv, strtonum, reallocarray, getsubopt, heapsort/mergesort/radixsort,
  quick_exit, a64l/l64a, mkostemp(s), mkdtemps), time (timelocal,
  timespec_get; setting the clock is EPERM), ctype extensions, the BSD and
  System V signal calls over sigaction/sigprocmask (not sigpause, which needs
  sigsuspend), dprintf, the `_unlocked` stdio forms, dirname_r/basename_r,
  mkpath_np, ftw/nftw and ether_ntoa/aton.
* **Thread and Mach extras** (GUI, `process/threads_extra.cpp`,
  `mach/machsys.cpp`): QoS queries, stack attributes (caller-provided stacks
  are ENOTSUP), the concurrency level and priority ceilings (kept, not
  enforced), JIT write callbacks, rwlock attributes (process-private only);
  vm_write/vm_copy/vm_inherit/vm_msync, the mach_vm_* and vm_* aliases,
  host_page_size and mach_get_times. sem_init/sem_destroy/sem_getvalue are
  ENOSYS, as on Darwin.
* **XPC** (GUI, `misc/xpc_objects.cpp`): the C object API (dictionaries,
  arrays, strings, numbers, booleans, dates, data, UUIDs, descriptors, null,
  errors, endpoints, shared memory) as `OS_xpc_*` objects, with copies,
  equality and descriptions. No launchd runs, so connections act as ones whose
  service does not exist: resumed, they deliver `XPC_ERROR_CONNECTION_INVALID`;
  replies are that error; `xpc_main` stops. Activities never run.
  NSXPCConnection is `misc/xpc.cpp`'s.
* **os extras** (GUI): activities with identifiers that nest per thread
  (`OS_ACTIVITY_CURRENT`, initiate, apply, get_active) and workgroups
  (join/leave tokens, cancel, intervals, the working arena, ports through
  Mach IPC) in `misc/libsystem_misc.cpp`; the os_sync address waits over the
  ulock calls (`process/libsystem_process.cpp`); signpost identifiers
  (`mach/machsys.cpp`); `_os_log_disabled`, the per-type `_os_log_*_impl`
  entry points and `os_log_with_args` (`misc/oslog.cpp`); notify signal
  registrations and suspend/resume (`mach/notify.cpp`); dnsinfo
  (`net/libsystem_net.cpp`: no configd, so `dns_configuration_copy` is
  NULL; configd's notify key). Workgroup membership gives the scheduler no
  hint (Windows has no thread workgroups).
* **Terminal libraries** (GUI, `terminal/`): libedit's readline API
  (`readline.cpp`: the callback interface reading rl_instream's descriptor
  in whatever mode the terminal is in, GNU readline history and history
  files, completion through rl_attempted_completion_function on Tab, kept
  bindings), curses (`curses.cpp`: windows, subwindows sharing cells, pads,
  colours and attributes, written to an xterm-compatible terminal as escape
  sequences; termios modes, keypad and SGR mouse decoding, KEY_RESIZE) and
  libpanel (`panel.cpp`). terminfo (`mach/machsys.cpp`) has no database but
  a built-in xterm-256color entry for the xterm family. initscr needs a
  terminal whose attributes can be read (a pseudo-terminal); the host
  console's cannot, so it stops there.
* **libm** (`misc/numeric_math.cpp`) also has lround/llround/lrint/llrint,
  nearbyint, fmax/fmin/fdim/fma, tgamma, lgamma_r, remquo, ilogb, scalbln,
  the Bessel functions (j0..yn), gamma/drem/significand/finite,
  __sinpi/__cospi/__tanpi/__sincospi_stret, fegetexceptflag/fesetexceptflag
  and conj/cproj/creal/cimag.
* **More fill-only pieces** (alchemy-fx, `install_libsystem_gaps`): `misc/stdlib_extra.cpp` (the
  rand48 generators), `misc/ndbm.cpp` (ndbm(3) over an append-only record file `<name>.db`, in
  Alchemy's own format, through the guest's open/read/write) and `net/rpc_nis.cpp`.
* **Unavailable services, answered with their own error codes:** NIS (`yp_get_default_domain`
  YPERR_NODOM, the map calls YPERR_YPBIND) and Sun RPC (`clnt_create` fails with
  RPC_PMAPFAILURE; the XDR codecs stop explicitly, since no client handle can exist). `crypt`,
  `encrypt` and `setkey` answer ENOSYS until the DES hash is implemented. Attribute lists
  (`getattrlist` family) answer ENOTSUP, `fchflags` ENOSYS as `chflags`, no file carries an ACL.
  `proc_pidfdinfo` describes this process's vnode descriptors only. Another thread's registers
  (`thread_get_state`, `thread_set_state`, `thread_abort(_safely)`) are not reachable.
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
