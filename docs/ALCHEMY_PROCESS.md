# Alchemy child processes

Alchemy runs Darwin child processes on a Windows host. A guest can create
pipes, `fork` and `exec`, call `posix_spawn`, `popen` and `system`, wait for
its children and signal them. The work is split across these files:

- `src/alchemy/darwin/libsystem/process/process_model.cpp`: the libSystem interface, part of `DarwinServices`.
- `src/alchemy/darwin/libsystem/process/launch.cpp`: launch specifications, exec target resolution and process creation.
- `src/alchemy/darwin/libsystem/process/shell.cpp`: the built-in shell.
- `include/alchemy/darwin_process.hpp`: the contract between them.

The model works with both engines (`alchemy.exe` and `alchemy-fx.exe`).

## Children are Alchemy processes

Every guest child is a new instance of the running Alchemy executable
(`GetModuleFileNameW`).

- **Launch specification.** The parent writes the child's settings into a
  read-only launch specification: argv (including argv[0] as given), envp,
  working directory, Darwin identity, umask, ignored signals, signal mask and
  descriptor table. It is an inheritable anonymous file mapping, named on the
  child's command line by the internal option `--alchemy-child-spec <handle>`.
  The child maps it and closes it before any guest code runs.
- **Runtime options.** The child's command line repeats the parent's runtime
  options: `--jit`, `--load-base`, `--dylib-dir`, `--runtime-dir`,
  `--instruction-limit` and the configured `--root`. `--jit-stats` is passed as
  `--jit` so that statistics never mix into a child's stderr.
- **Environments.** The guest environment of a child is exactly the envp that
  exec received. The host environment of the Alchemy process (`ALCHEMY_*`,
  `PATH` for host DLLs, `SystemRoot`) is inherited separately.
- **Descriptors.** Descriptors travel as inheritable duplicates of their
  Windows handles. `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` restricts inheritance
  to exactly those handles. Guest descriptors 0, 1 and 2 also become the
  child's host standard handles. Kinds:
  - Files, pipes and `/dev/null` (the host `NUL` device): inherited handles.
  - Directories: reopened by path.
  - `/dev/urandom`: recreated.
  - Sockets: duplicated with `WSADuplicateSocketW` for the suspended child.
  - kqueues: not inherited, as on Darwin.
- **Job object.** Every child is created suspended and placed in a
  kill-on-close job object that belongs to its parent, then resumed. When an
  Alchemy process ends, by any means, its children end too. A daemon that
  calls `setsid()` and double-forks therefore does not outlive its Alchemy
  ancestor. This is a deliberate difference from Darwin (no reparenting to
  launchd). It means a test never leaves an orphaned `alchemy-fx`.
- **No windows.** A child never opens a console window: it shares its
  parent's console, or runs with `CREATE_NO_WINDOW` when the parent has none.

### Identity

- `getpid()` is the child's Windows process id.
- `getppid()` is the parent's guest pid.
- `getpgrp()` and `getsid()` come from the specification.
- A top-level process reports `getppid() == 1`. It is its own process-group
  leader, so `setsid()` there fails with EPERM.
- `proc_pidpath` reports the guest path of the process itself and of each of
  its children. For other host processes it reports their image path.
- Users: the guest identity is uid/gid 1000. `ALCHEMY_SUPERUSER=1` in the
  environment starts a process as root, the explicit equivalent of `sudo` for
  installers. A root process may switch
  its real and effective ids between 0 and 1000 under setuid(2) rules. A
  child starts as root exactly when its parent's real or effective uid is 0:
  the parent sets or removes `ALCHEMY_SUPERUSER=1` in the child's environment,
  whatever environment `exec` or `posix_spawn` passes.

### Exit status and signals

`waitpid`, `wait`, `wait3`, `wait4` and `waitid` read the Windows exit code of
the child and decode it into a Darwin `<sys/wait.h>` status word:

| Child exit | Status |
| --- | --- |
| `exit(n)` | `WIFEXITED`, `WEXITSTATUS == n & 0xff` |
| Ended by a signal's default action: `kill()` from its parent, `raise`/`kill(getpid())`, a fatal guest fault | `WIFSIGNALED`, `WTERMSIG`. The child exits with `0x41430000 | signal`. A top-level process keeps the shell status 128 + signal. |
| Host crash (NTSTATUS) | `WIFSIGNALED`: SIGSEGV, SIGILL, SIGFPE, SIGINT or SIGABRT, otherwise SIGKILL |

Options:

- `WNOHANG` is supported. `WUNTRACED` and `WCONTINUED` are accepted and never
  report stops.
- `waitid` supports `P_ALL`, `P_PID` and `P_PGID`, with `WEXITED`, `WNOHANG`
  and `WNOWAIT`. It fills `si_signo`, `si_code`, `si_pid`, `si_uid` and
  `si_status`.
- `pid == 0` and `pid < -1` select a process group.
- `wait4` and `wait3` return a zeroed `rusage`.

`kill(pid, sig)` behaves as follows:

- **`pid` is a child.** The child cannot run a handler for a signal sent from
  outside it (Alchemy has no asynchronous signal delivery). So:
  - A terminating signal ends it with `TerminateProcess` and is reported as
    `WIFSIGNALED`. This covers SIGTERM, SIGKILL, SIGINT, SIGHUP and the other
    signals whose default action terminates. Once SIGKILL has been sent and
    `TerminateProcess` succeeded, the child is waitable at once (with the
    signal that ended it), not when the host has finished tearing the process
    down (seconds for a large process); until then its pid is not reused and
    `kill()` answers ESRCH. Other signals alone wait for the host process.
  - Signals that are ignored by default do nothing.
  - SIGSTOP and SIGTSTP fail with ENOTSUP.
  - Signal 0 checks that the child exists.
- **Process groups.** `pid == 0`, `pid < -1` and `killpg` signal every child
  in the group. `pid == -1` signals every child.
- **The caller itself.** The same applies to `raise`. The calling thread
  receives the signal synchronously:
  - With `SIG_IGN`, the signal does nothing.
  - An installed handler runs at once, with `SA_SIGINFO` and `SA_RESETHAND`
    honoured.
  - The default action ends the process with that signal and a backtrace.
- **Other host processes.** Signal 0 checks that the process exists. Any other
  signal fails with EPERM.

There is no SIGCHLD: guest signal handlers are only called for signals a
thread sends to its own process.

## Pipes and descriptors

- **`pipe`.** It creates an anonymous Windows pipe with a 64 KiB buffer. Pipe
  descriptors support:
  - `read`/`write`, `dup`/`dup2`/`F_DUPFD`/`F_DUPFD_CLOEXEC`, `close`.
  - `fcntl` `FD_CLOEXEC`, `F_GETFL`, and `F_SETFL` with `O_NONBLOCK`.
  - `ioctl(FIONREAD)` and `fstat` (`S_IFIFO`).
  - `poll`, `select` and `kevent` (`EVFILT_READ`/`EVFILT_WRITE`). A read end
    is readable when data is buffered, and reports POLLHUP (and `read`
    returns 0) once every write end is closed.
- **Read waits.** A blocking read polls the pipe without holding the
  descriptor's lock. A `close()` from another thread is therefore never
  blocked by a reader.
- **Write limits.** Anonymous pipes cannot report free buffer space, so a
  write blocks while the pipe is full, even with `O_NONBLOCK`. A write to a
  pipe whose read end is closed fails with EPIPE. SIGPIPE is not delivered.
- **Other descriptors.**
  - `socketpair` stays the existing TCP loopback pair; its descriptors are
    inherited as sockets.
  - `/dev/null` opens the host `NUL` device. Descriptors of the parent's own
    standard streams are inherited as the parent's host standard handles.
- **Descriptors of the top-level process.** `alchemy(-fx) --fd N=MODE:TARGET`
  starts the guest with descriptor N (3 to 4095) open on a host object, like a
  shell's `N<`, `N>`, `N>>` and `N<>` redirections. MODE is `r`, `w` (a file
  is created or truncated), `a` (append) or `rw`. TARGET is a host path (a
  file, a named pipe such as `\\.\pipe\name`, a device) or `handle:VALUE`, a
  handle the launcher inherited. They are described and installed as a
  child's launch descriptors are, and children inherit them through their
  descriptor tables. Chrome's `--remote-debugging-pipe` (DevTools on fds 3 and
  4) uses this; see `scripts/chrome/run_windowed.ps1`.
- **Close-on-exec.** It is tracked per descriptor (`O_CLOEXEC`, `FD_CLOEXEC`,
  `F_DUPFD_CLOEXEC`, `ioctl(FIOCLEX)`). Exec and spawn pass every descriptor
  without it. There is no `pipe2` or `dup3` because Darwin has neither.

## posix_spawn

`posix_spawn` and `posix_spawnp` take file actions and attributes.

- **File actions.** These run in order on a copy of the caller's descriptor
  table:
  - `addclose`, `adddup2` (`fd == newfd` clears close-on-exec), and `addopen`.
  - `addinherit_np`, `addchdir_np` and `addfchdir_np`.
- **Flags.**
  - `POSIX_SPAWN_SETPGROUP` (group 0 makes the child a group leader).
  - `POSIX_SPAWN_SETSID`.
  - `POSIX_SPAWN_SETSIGDEF` (removes the signals it names from the inherited
    `SIG_IGN` set).
  - `POSIX_SPAWN_SETSIGMASK`.
  - `POSIX_SPAWN_CLOEXEC_DEFAULT` (only descriptors named by `adddup2`,
    `addopen` or `addinherit_np` reach the child).
  - `POSIX_SPAWN_SETEXEC`.
  - `POSIX_SPAWN_RESETIDS` is accepted (there is one identity).
  - `POSIX_SPAWN_START_SUSPENDED` fails with ENOTSUP.
- **Darwin extensions.** `setbinpref_np` accepts arm64 or any architecture.
  The QoS class is stored.
- **`posix_spawnp`** searches the caller's `PATH`.
- **Errors** are returned as error numbers. A missing program is ENOENT.

## exec

- **The family.** `execve`, `execv`, `execvp`, `execvP`, `execl`, `execle`
  and `execlp` are supported. The `execl` forms read their variadic arguments
  from the Apple ARM64 stack slots.
- **Exec in place.** Outside a fork child, exec replaces the process image:
  1. The new image starts as a child with this process's pid, parent pid,
     group and session in its launch specification. It inherits every
     descriptor that is not close-on-exec.
  2. Every other thread of the old image is suspended and never runs again.
  3. The old process waits for the new image and exits with its exit code
     unchanged.

  To its parent, which waits on the old Windows process, and to the new image,
  which reports the old pid from `getpid()`, the pid is unchanged. The
  differences:
  - The Windows process id of the new image differs: `tasklist` shows two
    processes.
  - Memory of the old image stays mapped until the new image ends.
- **Exec targets.** Exec resolves a guest path in the guest namespace:
  `/Volumes/<drive>/...`, the Alchemy root (`/Applications`, `/Library`),
  `/tmp`, and paths relative to the working directory.
  - **Mach-O files** (thin or universal) and `.app` bundles run as an Alchemy
    child.
  - **`#!` scripts** run their interpreter with the usual argument order
    (interpreter, optional argument, script path, arguments). Up to five
    nested interpreters are followed.
  - **`/bin/sh`, `/bin/bash`, `/bin/zsh`, `/bin/dash` and `/usr/bin/sh`** are
    the built-in shell.
  - **`/usr/bin/env`** is resolved by the spawner itself: `-i`, `-u NAME` and
    `NAME=VALUE` are applied, and the utility is found on `PATH`. Without a
    utility, it prints the environment.
  - **Python aliases.** `/usr/bin/python3`, `/usr/bin/python`,
    `/usr/bin/python3.N` and the same names under `/usr/local/bin` and
    `/opt/homebrew/bin` run the guest Python. That is, in order:
    1. `ALCHEMY_GUEST_PYTHON` (a host path, from Alchemy's environment).
    2. The running program itself, when it is a `python3*` executable.
    3. The interpreter bundled with the running application
       (`<App>.app/Contents/Resources/<version>/python/bin/python3*`, which is
       Blender's bundled Python).

    Without one of these, the alias does not exist (ENOENT).
  - **Windows executables (`MZ`)** are never run from guest paths. Exec fails
    with ENOEXEC and a diagnostic. There is no mapping from guest paths to
    host programs.
  - Any other file fails with ENOEXEC.
- **Execute bit.** `access(path, X_OK)` reports these targets executable:
  Mach-O files, `#!` scripts, bundles, the built-in shell and env, and the
  configured Python alias. A plain file is not executable (EACCES); Windows
  has no execute bit.
- **Guest PATH.** A Windows-style `PATH` (drive letters, `;` or `\`) in a
  top-level guest's environment becomes `/usr/bin:/bin:/usr/sbin:/sbin`.
  `execvp`, `posix_spawnp`, Python's `shutil.which` and the shell then find
  `sh`, `env` and `python3`. Children receive their parent's guest
  environment unchanged.

## fork and vfork: the fork-exec emulation

A full fork, which copies the address space, is not implemented. Alchemy
emulates the pattern almost every caller uses, `fork` followed by descriptor
set-up and `exec`. `vfork` takes the same path.

1. **The temporary child.** `fork()` saves the caller's stack, from SP to the
   top of its stack region. It then starts a temporary guest thread, the child
   side, on a new engine worker. That thread has the caller's registers
   (x1-x30, q0-q31, SP) and `x0 = 0`, and continues at the return address.
   It shares the parent's memory and runs on the parent's stack, as with
   vfork; the parent thread waits.
2. **The child's private table.** The child works on a private copy of the
   descriptor table: `close`, `dup2`, `open`, `fcntl` and `pipe` change only
   that copy. It also has its own copies of the signal dispositions
   (`signal`, `sigaction`), working directory (`chdir`, `fchdir`), umask, and
   process group and session requests (`setpgid`, `setpgrp`, `setsid`).
   Reads and writes reach the shared open files, as after a real fork. This
   covers the error pipe of boost::process and Python.
3. **Exec.** `exec*` or `posix_spawn` in the child creates the real child
   process from that table. If exec fails, `execve` returns the error to the
   child, which continues: it typically writes errno to a pipe and calls
   `_exit`.
4. **The parent resumes.** When the child side has exec'd or called `_exit`,
   the parent's stack is restored from its saved copy, so the child's writes
   to the frames it shared with the parent never reach the parent. Every
   `malloc` block the child did not free is released. `fork()` then returns
   the real child's pid. If the child called `_exit` after a failed exec, or
   after it started a grandchild with a nested fork, `fork()` returns a pseudo
   pid (an odd number; Windows pids are multiples of 4). `waitpid` reports
   that pid's exit status.

**Checks.** Every libSystem call on the child side passes a guard:
`ProcessContext::install_function` wraps every provider. Allowed calls:

- Descriptor, directory, umask, identity, signal-disposition, wait and exec
  operations, including a nested `fork`.
- `posix_spawn`.
- Memory and string helpers, `malloc`/`free`, read-only queries (`stat`,
  `access`, `getenv`, `sysconf`, `opendir`), clocks, sleeps and output
  (`write`, `printf`). A `free` or `realloc` of a block the parent
  allocated leaves that block to the parent: after a real fork it would
  free the child's copy, which the exec discards anyway. The renderer add-on
  installer's process launcher (its license settings tool) frees its
  argument strings between fork and exec.

Anything else fails explicitly. These cases also count as failures:

- `_exit` or `exit()` without any exec or grandchild. `exit()` on the child
  side never runs the parent's atexit handlers.
- Returning from the function that called `fork`.

On failure, the parent's `fork()` returns -1 with errno ENOSYS, and stderr
says which call the child made:

```
alchemy: fork emulation: the child called _pthread_mutex_lock before exec; ... fork() fails with ENOSYS
```

**Limits:**

- `os.fork()` without exec, fork-based worker pools and anything else that
  needs a second address space fail as described above. Python's
  `multiprocessing` on macOS uses spawn (fork + exec through
  `_posixsubprocess`), which is supported.
- Writes the child makes to global or heap memory are not detected. Only the
  stack is restored.
- Another parent thread that writes into the forking thread's stack while the
  child runs has those writes overwritten by the restore.
- The child side has fresh thread-local storage, errno and signal mask. Its
  `pthread_self()` differs from the parent thread's.
- `setsid()` on the child side returns a placeholder (the pseudo pid). The
  real session id exists only after exec.

### What the renderer add-on, Blender and Python do between fork and exec

- **The add-on's native library** starts its render server through
  `boost::process`. It imports `fork`, `execve`, `pipe`, `waitpid`, `kill`,
  `_exit`, `fcntl`, `open` and `environ`.
- **The add-on's render server** imports `fork`, `execv`, `waitpid`, `kill`,
  `_exit`, `pipe`, `popen`, `pclose` and `socketpair`. `popen` runs exactly
  two commands:
  - `ps ax -o pid,command 2> /dev/null`.
  - `getconf ARG_MAX 2> /dev/null`.

  Both are shell built-ins: `ps` prints only its header (no process is
  visible to a guest), so the reader sees an empty process list; `getconf
  ARG_MAX` prints Darwin's 1048576.
- **The renderer's core library** imports `posix_spawn`, `fork` and `execve`.
- **Python 3.11 `subprocess`.** It uses `_posixsubprocess.fork_exec`:
  - `fork`.
  - In the child: `sigaction` resets, `umask`, `chdir`, `setsid`/`setpgid`,
    `dup2` and `fcntl` on the standard descriptors, and `close` up to
    `sysconf(_SC_OPEN_MAX)`. `/dev/fd` does not exist, so `opendir` fails and
    Python closes by number.
  - `execve` over each `PATH` candidate.
  - On failure, the error is written to the error pipe and the child calls
    `_exit(255)`.

  All of this is supported. A `preexec_fn` runs Python code in the child,
  which is unsupported and fails the fork.
- **The add-on's Python.** `subprocess` runs a bundled tool (a Mach-O) and, on
  x86_64 macOS only, `sys.executable -m pip`. Its Windows-only branches call
  `os.system` with `robocopy` and `clip.exe`; these never run on macOS.
- **Installer scripts.** The installer payload carries four `#!/bin/bash`
  scripts, for the License Server and Unified Login services:
  `registerLicenseServerAsService`, `unregisterLicenseServerAsService`,
  `registerService` and `unregisterService`. They register launchd agents
  with `launchctl`, `sudo`, `chown` and `cp`. They are unsupported: the
  launchd tools do not exist, so the built-in shell reports `command not found`
  and the scripts that use `if` fail with status 2.
- **The installer** (running as root, `ALCHEMY_SUPERUSER=1`) changes
  credentials and the environment between fork and exec, asks the shell for
  `command -v`, `ps` and `getconf ARG_MAX`, and calls
  `/usr/libexec/ApplicationFirewall/socketfilterfw --add` for the render
  server. The fork child's `setenv`/`unsetenv` and `setuid`/`setgid` family
  change only the child's own state, which `exec` passes on.
  `socketfilterfw` is a tool child: no application firewall exists under
  Alchemy, so `--add`, `--remove`, `--blockapp` and `--unblockapp` succeed
  without effect, `--getglobalstate` reports a disabled firewall, and other
  options fail with status 1.

## The shell decision

macOS has no shell binary Alchemy may run, and no Windows shell is ever used.
`/bin/sh`, `system()` and `popen()` use a project-authored POSIX shell subset.
It runs as a tool child: an Alchemy process with `--alchemy-child-spec`,
where the specification names the tool `sh` and no guest code is loaded. It
gives `system()` and `popen()` a real pid and the usual status.

**Supported:**

- Simple commands with `NAME=VALUE` prefixes.
- Single and double quotes, and backslash escapes.
- `$NAME`, `${NAME}`, `${NAME:-word}`, `${NAME-word}`, `$?`, `$$`, `$#`,
  `$@`, `"$@"`, `$*` and `$0`-`$9`, with field splitting of unquoted
  expansions.
- `~` (HOME).
- Redirections `<`, `>`, `>|`, `>>`, `<>`, `N>&M`, `N<&M` and `N>&-`.
  `/dev/null` is supported.
- Pipelines with `|` and `!`.
- Lists with `;`, newlines, `&&` and `||`.
- Comments.
- The built-ins `:`, `true`, `false`, `exit`, `echo` (with `-n`), `cd`,
  `export`, `unset` and `exec`, plus `command -v NAME`, `getconf ARG_MAX`
  (1048576) and `ps` (the header only: no process is visible).
- Options `-c`, `-e`, and script files. `-u`, `-x`, `-f` and `-h` are
  accepted and have no effect.

**Unsupported, with an explicit `sh: ...` message and exit status 2:**

- `if`, `case`, loops, functions and `{ }`.
- Subshells.
- `$(...)`, backquotes and arithmetic.
- Pathname expansion (unquoted `*` and `?`).
- Here-documents.
- Background jobs.
- `$!`.
- The built-ins `[`/`test`, `read`, `set`, `shift`, `eval`, `.`, `trap`,
  `wait`, `printf` and `local`.

A command that is not found reports status 127. A command that cannot run
reports 126.

## Shared memory between Alchemy processes

Two Alchemy processes can share pages. Only alchemy-fx supports this: the
reference engine keeps private copies and reports that explicitly.

### Mappings

- **`mmap(MAP_SHARED)` of a file.** `DarwinServices::map_shared_file`:
  - It creates a host file mapping (`CreateFileMappingW`) on the descriptor's
    Windows handle: read-write when the descriptor is open for writing,
    otherwise read-only.
  - The engine views that mapping at a guest address it picks
    (`engine_map_section` → `FxEngine::map_section` →
    `fx::map_section_guest_memory`). The view is a `MapViewOfFile3` view in an
    fx placeholder; see FX_ENGINE.md, "Shared guest memory".
  - Every process that maps the same host file sees the same pages, and
    stores reach the file. Windows keeps views of one file coherent across
    processes.
  - A file whose size is not a page multiple is viewed to its last byte;
    the rest of that page reads as zeros, as on Darwin.
  - Pages wholly past the end of the file stay inaccessible placeholder
    space. They correspond to the SIGBUS pages on Darwin.
  - The mapping outlives its descriptor.
  - `munmap` of the whole mapping releases it. `mprotect` works on it.
    `PROT_WRITE` needs a descriptor that was open for writing (EACCES
    otherwise), and `PROT_EXEC` is refused.
  - `msync` flushes the view (`FlushViewOfFile`).
  - An engine without section views (not alchemy-fx) serves a read-only
    request (`PROT_READ`, e.g. Qt's `QFile::map`) as a private copy of the
    file range: later writes to the file by others are not reflected. A
    writable request fails there with EINVAL and a diagnostic.
- **`shm_open`/`shm_unlink`.** A POSIX shared memory object is a host file
  in a per-root namespace: `<root>/.alchemy/shm/` with an Alchemy root, or
  `%TEMP%\alchemy-shm\` without one.
  - The name is escaped: everything except lower-case letters, digits and
    `.-_` becomes `%xx`, because Darwin names are case-sensitive.
  - The object is opened close-on-exec. `O_CREAT`, `O_EXCL`, `O_TRUNC` and
    `O_RDONLY`/`O_RDWR` are supported. Names are limited to PSHMNAMLEN (31)
    characters.
  - A file, not a pagefile section, so that `ftruncate` after `shm_open`
    (the usual order) can size it. `fstat`, `ftruncate` and `mmap` then
    behave as for any file.
- **`ftruncate`** moves the end of the file without moving the file position.
  - Growing a mapped file works.
  - Shrinking a mapped file is refused by Windows. It fails with EBUSY and
    prints a diagnostic. Darwin would allow it.
- **`unlink` of a mapped file** removes the name with POSIX semantics. The
  pages stay mapped, and the name can be created again at once.
- **`MAP_SHARED | MAP_ANON`** is ordinary anonymous memory. The fork
  emulation shares the whole address space anyway, and exec'd children start
  fresh.
- **Not supported**:
  - `MAP_SHARED | MAP_FIXED` (EINVAL and a diagnostic).
  - Mapping a range wholly past the end of a file.
  - System V shared memory (`shmget`, `shmat`: ENOSYS).

`ProcessContext::shared_location(address)` names shared memory independently
of the process. The name is the host file's volume serial and file index,
plus the byte offset in the file.

### Synchronization on shared memory

- **Plain atomics.** Spin locks and atomic counters work unchanged: the guest
  atomics are host atomics on the same physical pages. This is how
  Boost.Interprocess builds its mutexes on macOS.
- **Waits** (`alchemy/shared_wait.hpp`, `src/alchemy/darwin/libsystem/process/shared_wait.cpp`).
  These are waits that must be woken from another process:
  - The keys are shared locations. The table is session-wide: a named file
    mapping of 4096 slots and a named mutex (`Local\alchemy.shared-waits.v1.*`),
    plus a named semaphore per slot.
  - A waiter registers in its key's slot and waits on the semaphore. A waker
    releases one permit, or one per registered waiter.
  - Waiters also re-check their condition every 5 ms, so a lost wake is
    impossible. A wake can only be early.
- **`__ulock_wait`/`__ulock_wake`** (`UL_COMPARE_AND_WAIT` and
  `UL_COMPARE_AND_WAIT64`) on shared memory use these waits. Private memory
  keeps the process-local waits.
- **`PTHREAD_PROCESS_SHARED` mutexes and condition variables.** These are
  objects initialized with a process-shared attribute inside a shared mapping.
  Their whole state lives in the guest object:
  - Mutex: `'ALPS'`, type, state, owner, depth. The state is 0 (free),
    1 (locked) or 2 (locked with waiters); the owner is the host thread id.
  - Condition variable: `'ALPC'` and a sequence word.
  - Any process that maps the object can use it without having initialized
    it. Normal, error-checking and recursive mutexes, `trylock`, timed
    condition waits, and pairing a shared condition variable with either
    kind of mutex are supported. Process-shared objects in private memory
    use the process-local implementation.
- **Named POSIX semaphores** (`sem_open`, export-fill) are Windows named
  semaphores and were already shared between processes.
- **Not supported**: `os_unfair_lock` is not provided at all.

## Mach ports between processes

Mach IPC works across the Alchemy process tree on alchemy-fx
(`src/alchemy/darwin/libsystem/mach/`: `transport.cpp`, `mach_remote.cpp`,
`mach_exports.cpp`). Each process keeps its own port name space
(`mach_ipc.cpp`); rights and messages travel between processes as frames.

### Transport

- **Endpoints.** A process that exports a right runs one endpoint, a named
  pipe `\\.\pipe\alchemy-mach-<pid>`, named by the guest pid
  (`process_tree`; the Windows pid unless the image was exec'd in place).
  Host handles go to the process's `host_pid`. It starts lazily: on the first right that leaves the process, the
  first bootstrap check-in, or before any child is created
  (`ProcessContext::spawn_starting`, from every spawn path).
- **Connections.** A peer connects once and keeps the connection.
  - One-way frames are written in order by a writer thread: messages,
    reference releases, send-once-destroyed and dead-port notices.
  - Requests wait for their response on the same pipe: reference grants,
    bootstrap, special ports.
  - All pipe I/O is overlapped, so a pending read never blocks a write.
- **Peer death.** `process_tree::watch_exit` with the peer's pid and
  generation (pidversion) reports a peer's death once; a reused pid is
  another process. A pid not yet in the tree falls back to a wait on its
  process handle.
- **Threads.** Frame handlers run on transport threads, never on guest
  threads, and never call guest code. Kernel objects that need guest calls
  (memory entries, fileports) are materialized when the message is received.
- **Handles in flight.** A section or file handle travels already
  duplicated into the recipient. A message forwarded on (its port's receive
  right moved) moves the handle to the next process; a message destroyed
  undelivered closes it.

### Ports, rights and references

- **Keys.** A port gets a key, (origin pid, port id), when a right to it
  first leaves the process. Frames name ports by key.
- **Proxies.** A send or send-once right to another process's port is a
  proxy in the receiving process. Its local rights are counted as usual. The
  units the proxy holds at the port's host are counted by the host
  (`holders`, `once_holders`).
- **Grants.** Before a right to a proxy travels to a third process, the host
  is asked to count a unit for the recipient (a request). A unit therefore
  always exists before the right arrives. When a proxy's last local right
  goes away, its units return to the host (a release frame).
- **Receive rights.** A receive right that leaves the process moves its
  port. The recipient hosts a port under the same key. The old host becomes
  a forwarding proxy that holds one send unit there, and its queued messages
  follow, in order. A receive right that comes back turns the forwarder into
  the real port again.
- **Messages.** `mach_msg` to a proxy sends a frame. The frame carries the
  message image, the sender's audit token (the receiver's trailer reports
  it: `audit_token_to_pid` is the sender's pid, `audit_token_to_pidversion`
  its generation) and the rights.
  - Out-of-line memory below 64 KiB travels in the frame. Larger regions
    travel in a new host section (`xmem`), copied into new memory on
    receive.
  - Port and out-of-line-port descriptors travel as rights.
  - Task ports travel as "the task of pid N": `pid_for_task` answers N.
  - Queue limits do not apply to remote senders.

### Notifications

- **No-senders** waits for local senders and for every remote holder.
- **Send-once.** A send-once right destroyed in another process makes its
  host send the send-once notification.
- **Dead ports.** A port that dies (its receive right destroyed) is
  announced to every holder. Their names become dead names, with dead-name
  notifications.
- **Dead processes.** When a process dies, its proxies' ports die for
  everyone else, and the units it held are dropped. Lost send-once units
  produce send-once notifications.

### Bootstrap and special ports

- **Broker.** The root Alchemy process of the tree hosts the bootstrap
  registry. `ALCHEMY_MACH_BROKER=<pid>` is set by the root and inherited
  through the host environment.
  - `bootstrap_check_in`, `bootstrap_look_up` and `bootstrap_register` work
    across the tree.
  - Entries of a process that dies are removed.
  - `bootstrap_parent` answers BOOTSTRAP_NOT_PRIVILEGED.
- **Special ports.**
  - `posix_spawnattr_setspecialport_np` ports are exported to the child when
    `posix_spawn` has created it (`ProcessContext::spawn_started`).
  - The child fetches them from its parent on its first
    `task_get_special_port` for that slot.
  - The `bootstrap_port` variable always names the tree's broker.

### File descriptors and memory between processes

- **Fileports.** `fileport_makeport` and `fileport_makefd` wrap a guest
  descriptor (DarwinServices: `ProcessContext::export_descriptor` and
  `import_descriptor`). A fileport sent to another process carries the file's
  host handle, duplicated into the recipient (`DuplicateHandle`).
  - Files, pipes, `/dev/null`, directories and `/dev/urandom` can travel.
  - Sockets cannot be wrapped in a fileport.
- **Memory entries.** Entries name fx shared guest memory (an anchor made
  with `vm_allocate`, or the task's own shared pages); entries over private
  memory are refused (KERN_NOT_SUPPORTED). They cross processes through
  host sections (`include/alchemy/cross_process_memory.hpp`, `xmem`):
  - The sender finds the section behind the entry (`section_of`) and
    exports the range to the recipient (`export_section`), read-only unless
    the entry is writable.
  - The recipient maps the section as the new entry's anchor
    (`map_section`, Mach-owned pages), so `mach_vm_map`, `vm_deallocate`,
    `vm_protect` and `vm_remap` work as for local entries, and both
    processes share the pages.
  - Cross-process memory is never executable.
  - Engines without section views copy entries when they cross processes.

### Task and thread policy, task_info

- **Task policy.** `task_policy_set` and `task_policy_get` store and read
  back TASK_CATEGORY_POLICY 1 (roles foreground, background, throttle,
  non-UI, default and DarwinBG; the control and graphics-server roles are
  refused), TASK_SUPPRESSION_POLICY 3, TASK_BASE_QOS_POLICY 8 and
  TASK_OVERRIDE_QOS_POLICY 9 (latency and throughput tiers). Other flavors
  fail with KERN_INVALID_ARGUMENT.
  - `get_default` TRUE on input returns the defaults (TASK_UNSPECIFIED,
    zeros); on output it says whether the default is in effect.
- **Thread policy.** `thread_policy_set` and `thread_policy_get` store per
  thread: STANDARD 0 (resets), EXTENDED 1, TIME_CONSTRAINT 2, PRECEDENCE 3,
  BACKGROUND 5, LATENCY_QOS 7 and THROUGHPUT_QOS 8. AFFINITY 4 is not
  supported, as on arm64 macOS.
- **Scheduling.** The values are hints. They are stored and readable, but
  Alchemy does not change host priorities or scheduling for them.
- **task_info.** MACH_TASK_BASIC_INFO 20, TASK_BASIC_INFO_64 5,
  TASK_THREAD_TIMES_INFO 3, TASK_VM_INFO 22 and TASK_POWER_INFO 21. CPU
  times and memory are the host process's; wakeup counters are 0.
- **Other processes.** Through another process's task port (received in a
  message), `task_policy_*` and `task_info` are answered by that process
  over the Mach transport. Its policies are its own, and its `task_info`
  reports its own figures.

### Tests

- `alchemy_fx_mach_cross` runs `fx_mach_cross` under alchemy-fx: a parent
  and its `posix_spawn`'d copy exchange:
  - a bootstrap rendezvous with a send-once reply and audit pids;
  - a special port from the spawn attributes;
  - a send right, and a receive right whose port has a queued message;
  - a fileport for a pipe;
  - a memory entry both processes map and write;
  - 128 KiB of out-of-line memory (through a section), and an audit token
    with the sender's pid version;
  - the child's task port, through which the parent sets and reads the
    child's task category and reads its `task_info`.
- When the child exits, the parent receives a dead-name and a no-senders
  notification.

## Tests

The `proc_model` guest fixture (`tests/fixtures/macos/proc_model.c`, linked
against the link-only `proc_link_system.dylib`) is both parent and child.
`alchemy_process_model_tests` runs each case in its own Alchemy process. The
cases run at the default load base and at `--load-base 0x120000000`, inside a
job object that must be empty once the top-level process has exited:

| Case | Checks |
| --- | --- |
| `pipe` | Round trip, poll, `O_NONBLOCK` EAGAIN, dup, EOF and POLLHUP after the last writer closes, double close |
| `spawn` | `posix_spawn` with `adddup2`/`addclose` capturing child stdout (argv with spaces), argv[0], `posix_spawnp("sh")`, ENOENT, `addopen` with a relative path |
| `boost` | The boost::process sequence: pipe, FD_CLOEXEC, fork, execve, error through the pipe, `_exit(1)`, plus the parent-frame write staying private |
| `forkdup` | `vfork`, `dup2` onto stdout, `close`, `execv`. The parent's own table is unchanged |
| `wait` | `exit(7)`, WNOHANG, `wait4(-1)`, a child killing itself with SIGTERM (`WIFSIGNALED`), ECHILD |
| `popen` | `popen` "r" with `fgets`, `pclose` statuses, `popen` "w", `system(NULL)`, `system` status, 127, an unsupported-syntax status of 2 |
| `cloexec` | `O_CLOEXEC`, `FD_CLOEXEC` and dup: exactly the right descriptors are inherited; `POSIX_SPAWN_CLOEXEC_DEFAULT` with `addinherit_np` |
| `kill` | `kill(pid, 0)`, SIGTERM and SIGKILL to a process group, then `waitpid` |
| `violation` | A forbidden call and a bare `_exit` on the child side make `fork()` fail with ENOSYS |
| `shell` | Quoting, expansion, defaults, `&&`/`||`, a pipeline into a guest child, environment prefixes |
| `installer` | `command -v`, `getconf ARG_MAX`, `ps` and `socketfilterfw` through `popen` and `system` |
| `superuser` | Under `ALCHEMY_SUPERUSER=1` (alchemy-fx only): root identity; a fork child's `setenv`, `setgid` and `setuid` reach its exec'd image and leave the parent unchanged; a child spawned with an empty environment is still root |
| `exec` | `execve` in place keeps the pid and passes the exit status through |

`ctest -R alchemy_process_model` runs the reference engine (`alchemy.exe`) and
alchemy-fx.

`ALCHEMY_TRACE_PROCESS=1` logs every child with its argv on stderr
(`[alchemy process] child <pid> of <pid>: ...`).

The `shm_model` fixture (`tests/fixtures/macos/shm_model.c`, linked against
`shm_link_system.dylib`) is run by `alchemy_shared_memory_fx` with the same
driver. It runs only under alchemy-fx, at both load addresses. In each case a
parent and the child it starts with `posix_spawn` share one segment:

| Case | Checks |
| --- | --- |
| `filemap` | The Boost.Interprocess sequence: `$TMPDIR/boost_interprocess`, `open(O_CREAT\|O_EXCL)`, `ftruncate`, `mmap(MAP_SHARED)` in both processes. Both processes count 20,000 times under one spin lock, released by a named semaphore. Also: `msync` and `read(2)` see the child's store; EACCES for `PROT_WRITE` on `O_RDONLY`; a read-only view; MAP_FIXED EINVAL; growing while mapped; EBUSY when shrinking while mapped; `unlink` while mapped with the name reusable; an 8-byte file |
| `shm` | `shm_open` create/EXCL, `ftruncate`, 200 ping-pong rounds, `shm_unlink`, ENOENT afterwards, PSHMNAMLEN |
| `ulock` | `__ulock_wait`/`__ulock_wake` on a shared word, in both directions |
| `pmutex` | A `PTHREAD_PROCESS_SHARED` mutex held by the parent blocks the child; both processes count under it. A shared condition variable hand-off; `pthread_cond_timedwait` ETIMEDOUT; `trylock` EBUSY |

`ALCHEMY_TRACE_CALLS=<prefixes>` (`-prefix` excludes) logs provider calls with
the process id and a thread hash. `ALCHEMY_TRACE_CALLS_FILTER=--alchemy-child-spec`
limits the log to guest children.

## Validation under headless Blender (2026-10-04)

The official macOS Blender 4.5.14 ran under alchemy-fx (`-b --factory-startup
-noaudio --python`, no windows).

- **Python `subprocess`.** Every case below works (fork emulation through
  `_posixsubprocess`, pipes, the shell and the Python alias):
  - `subprocess.run(['sh', '-c', 'echo hi; echo err >&2; exit 3'],
    capture_output=True)` captures stdout and stderr and returns status 3.
  - `subprocess.run([sys.executable, ...])` runs the bundled
    `python3.11` as a child; its `os.getppid()` is Blender's pid.
  - `subprocess.run(['python3', ...])` resolves through the PATH alias to the
    same interpreter.
  - `Popen(..., stdin=PIPE, stdout=PIPE).communicate(...)` round-trips data.
- **The renderer add-on for Blender.** The add-on's `register()` now gets past
  `boost::process`:
  - `pipe(2)` works, the fork/exec emulation runs, and the add-on's render server starts
    as a child `alchemy-fx` (argv `-p -1 -log 2 -blenderPID <pid> ...
    -headlessMode`).
  - The server then stops at its own load:
    - Strict mode: `missing guest dylib ... Security.framework` (owner:
      v-netsec).
    - With `ALCHEMY_PROBE_MISSING=zero`, a diagnostic only:
      `libQt6WebEngineCore.6.dylib` exceeds the loader's 512 MiB image limit
      (owner: loader). An experiment with a 1 GiB limit stopped next at
      `Objective-C metadata: unsupported Blocks method type`.
  - The add-on reports "ZMQ Server failed to start in 30 s" and exits, so
    ZeroMQ never connects and no render was attempted.

## Validation under headless Blender (2026-10-12)

The renderer add-on for Blender renders headless.

- The add-on's render server starts as a child, and the endpoint region is shared through
  `mmap(MAP_SHARED)`.
- ZeroMQ connects, and the license client reaches the `Host1` license
  server.
- The default cube renders to a PNG under `/private/tmp` in the
  install root.
- The node-thumbnail and material-preview renders work too.
