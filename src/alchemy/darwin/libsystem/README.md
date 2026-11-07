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
  (`misc/{oslog,system_c,log_store,oslog_store}.cpp`,
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
* **stdio** (core): FILE objects over descriptors (`services.cpp`, `fs/files.cpp`), or over a
  backend without one (`fs/memory_streams.cpp`: fmemopen, open_memstream, open_wmemstream,
  funopen). Nothing is buffered: every write reaches the descriptor at once, so setvbuf,
  setbuf, setbuffer and setlinebuf accept every mode and fpurge only drops a pushed-back byte.
  fscanf/vfscanf/scanf/vscanf read the stream (`misc/native.cpp`), sscanf/vsscanf a string;
  the wide forms (fwscanf, wscanf, swscanf and their va_list forms, wprintf/vwprintf/vfwprintf,
  fputws, fgetws, putwc/putwchar/getwchar, fwide) work on ASCII formats; gets, ctermid and
  vasprintf_l too.
* **Permissions** (`fs/files.cpp`, `services_impl.hpp`): the host keeps a file's write
  permission as its read-only attribute and no other mode bit, as Darwin's msdosfs does.
  stat reports read, write unless read-only, execute for directories and .exe/.cmd/.bat/.com,
  copied to group and other (0600/0700 for an owner-only ACL). chmod, fchmod and fchmodat
  accept every mode: the owner write bit sets or clears read-only, and group, other,
  execute, set-id and sticky bits are ignored, so stat shows what the attribute implies,
  not the requested bits.
* **Core libc additions** (both builds, `misc/libc_extra.cpp`, installed last by
  `install_darwin_support`): string and wide-string helpers, `<search.h>`,
  stdlib (putenv, strtonum, reallocarray, getsubopt, heapsort/mergesort/radixsort,
  quick_exit, a64l/l64a, mkostemp(s), mkdtemps), time (timelocal,
  timespec_get; setting the clock is EPERM), ctype extensions, the BSD and
  System V signal calls over sigaction/sigprocmask, dprintf, the `_unlocked` stdio forms, dirname_r/basename_r,
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
  equality and descriptions. Activities never run. Connections, `xpc_main`
  and NSXPCConnection are `darwin/launchd/`'s (`xpc_connections.cpp`,
  `nsxpc.cpp`; the shared internals are `misc/xpc_internal.hpp`).
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
  a built-in xterm-256color entry for the xterm family. On the host's console,
  tcgetattr/tcsetattr map ICANON, ECHO and ISIG onto its input modes (raw mode
  delivers special keys as escape sequences; output escape sequences are
  interpreted), so these libraries run there too; the console echoes only in
  line mode.
* **libm** (`misc/numeric_math.cpp`) also has lround/llround/lrint/llrint,
  nearbyint, fmax/fmin/fdim/fma, tgamma, lgamma_r, remquo, ilogb, scalbln,
  the Bessel functions (j0..yn), gamma/drem/significand/finite,
  __sinpi/__cospi/__tanpi/__sincospi_stret, fegetexceptflag/fesetexceptflag
  and conj/cproj/creal/cimag.
* **More fill-only pieces** (alchemy-fx, `install_libsystem_gaps`): `misc/stdlib_extra.cpp` (the
  rand48 generators), `misc/ndbm.cpp` (ndbm(3) over an append-only record file `<name>.db`, in
  Alchemy's own format, through the guest's open/read/write) and `net/rpc_nis.cpp`.
* **BSD leftovers** (alchemy-fx, `install_libsystem_gaps`): `misc/bsd_extra.cpp` (memrchr,
  strchrnul, explicit_bzero, wcstoimax/wcstoumax, wctrans/towctrans, psort, sranddev, devname_r,
  getbsize, rpmatch, timeoff, strptime_l, getpass/readpassphrase, the pwcache lookups
  user_from_uid and friends, backtrace_symbols_fd, get/setiopolicy_np (kept, no host hint),
  pthread_is_threaded_np, the `__stpcpy_chk` family, fmtcheck and the out-of-line ctype helpers)
  and `net/netdb_extra.cpp` (gethostbyname2 and getipnodeby* over getaddrinfo, herror,
  inet_net_ntop, the /etc/hosts, services, protocols, networks and ethers databases read from
  the guest file system, the RFC 3542 inet6_opt_*/inet6_rth_* helpers). Also getwd, fgetwln,
  mkfifoat/mknodat/mkpathat_np (relative to a directory descriptor), telldir/seekdir (core), and
  the malloc zone diagnostics (malloc_zone_check/print/log/claimed_address, discharge checking,
  malloc_printf; `malloc/malloc_zones.cpp`: a guest zone answers through its introspection
  table, the system zone is consistent by construction).
* **Process identity and accounting** (core, `process/identity.cpp`): getpriority/setpriority/nice
  for this process (a nice value as the host priority class nearest to it; only root lowers it;
  PRIO_DARWIN_THREAD/PROCESS are the host's background modes; another process, PRIO_PGRP and
  PRIO_USER are ENOTSUP), getrusage and times with the children's CPU time (read when wait*
  reaps them, as wait4's rusage), getpwent/setpwent/endpwent over the one account,
  getgrouplist, lockf over fcntl locks, ctermid_r. lutimes and utimensat(AT_SYMLINK_NOFOLLOW)
  set a symbolic link's own times (`fs/files.cpp`). chflags/lchflags/fchflags keep UF_HIDDEN as the
  host's hidden attribute, which stat reports in st_flags; other flags are ENOTSUP. Network
  interfaces have one set of names (`net/interface_names.hpp`: lo0, en<n>, utun<n>) for getifaddrs
  and the if_* calls.
* **Signals within the process** (core, `process/process_model.cpp`): a signal blocked where it
  goes stays pending (for the calling thread: raise; another thread: pthread_kill; the process:
  kill to itself, the interval timer, which follows the mask of the thread that armed it) until
  sigprocmask/pthread_sigmask unblocks it or sigsuspend/pause wait with it unblocked, which run
  its handler on that thread; sigwait takes it without the handler, sigpending reports it,
  SIG_IGN discards it. New threads inherit their creator's mask. A signal for another thread
  does not interrupt it: it waits for that thread's next unblocking or wait. sigaltstack keeps an
  alternate stack per thread; a handler with SA_ONSTACK runs on it (a trampoline switches sp).
* **Attribute lists** (alchemy-fx, `install_libsystem_gaps`): `fs/attrlist.cpp`, the getattrlist(2)
  family (get/set, f and at forms) packed from stat, statfs, the extended attributes and the host
  volume: the common attributes this file system keeps, the directory and file ones, the volume's
  (capabilities as the host file system has them) and the extended common ones; the rest is left
  out (or zeros with FSOPT_PACK_INVAL_ATTRS) and ATTR_CMN_RETURNED_ATTRS says so. setattrlist sets
  the creation, modification and access times, the Finder information, the mode and the flags
  (chflags).
* **Word expansion and dates** (alchemy-fx, `install_libsystem_gaps`): `fs/wordexp.cpp`
  (wordexp/wordfree: the shell's expansions in the shell's order — tilde, the ${...} parameter
  forms over the environment, command substitution through the guest's popen and /bin/sh,
  $((...)) arithmetic, IFS splitting, pathname expansion through glob, quote removal; unquoted
  shell operators are WRDE_BADCHAR); in `misc/bsd_extra.cpp` getvfsbyname (the types of the
  mounted volumes) and getdate/getdate_err (DATEMSK templates through strptime, POSIX's defaults).
* **Mach user-library and process leftovers** (alchemy-fx, `install_libsystem_gaps`):
  `mach/mach_extra.cpp` (the MIG server loop mach_msg_server(_once), mig_allocate/
  deallocate/strncpy, host_kernel_version, clock_sleep, mach_task_is_self,
  mach_vm_behavior_set, bootstrap_look_up2/register2); in `misc/bsd_extra.cpp` libproc dirty
  tracking (this process's own; no launchd acts on it), the membership compatibility UUIDs with
  getpwuuid(_r), backtrace_from_fp/backtrace_image_offsets, minherit, setmode/getmode, the
  statfs64 forms, atof_l and querylocale; the getsectbyname/getsegbyname/get_etext family in
  `mach/machsys.cpp`; the *printf_l forms and posix_spawnattr_{set,get}archpref_np (core). random's initstate/setstate keep one generator per state array
  (`misc/libsystem_runtime.cpp`, with the compiler-rt 128-bit, overflow, half-precision and
  complex builtins); Blocks introspection is `misc/libsystem_extra.cpp`'s. sigsuspend, sigpause
  and pause (`process/process_model.cpp`) wait until a handler has run for a signal raised in
  this process; ualarm stops, as alarm does (no asynchronous SIGALRM). getopt_long_only is
  `misc/leftovers.cpp`'s; err_set_file/err_set_exit and the whole err/warn family are
  `misc/fts_err.cpp`'s.
* **Unavailable services, answered with their own error codes:** NIS (`yp_get_default_domain`
  YPERR_NODOM, the map calls YPERR_YPBIND) and Sun RPC (`clnt_create` fails with
  RPC_PMAPFAILURE; the XDR codecs stop explicitly, since no client handle can exist). `crypt`
  (the traditional and extended DES hashes), `encrypt` and `setkey` use `misc/crypt_des.hpp`
  (DES from FIPS 46-3's tables). Attribute lists
  (`getattrlist` family) answer ENOTSUP, `fchflags` ENOSYS as `chflags`, no file carries an ACL.
  `proc_pidfdinfo` describes this process's vnode descriptors only. Another thread's registers
  (`thread_get_state`, `thread_set_state`, `thread_abort(_safely)`) are not reachable.
* **Unsupported** (each fails explicitly): guest signal delivery to other
  processes and asynchronous signals (SIGALRM, SIGCHLD), real credentials (a virtual user 1000, or root with
  ALCHEMY_SUPERUSER=1), SysV shared memory, launchd services, sandboxing,
  terminfo.
* **Tests:** `tests/`, `mach/tests/`, `vm/tests/`. The headless
  `tests/alchemy_{darwin,services,threads,mach_vm,native,numeric,network}_tests.cpp`
  are at the repository root.

## Moved from darwin/identities/

* `misc/crt_and_host_info.cpp`: headless, every build: _NSGetArgc/_NSGetArgv, getchar, wcsstr/wcspbrk, host_info, tcgetattr/tcsetattr.
* Mach IPC's namespace is `libsystem::mach` (`install_mach_ipc`, `install_notify`; it was `chromesupport`).
