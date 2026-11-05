# The session launchd

Alchemy's analogue of the per-user launchd (and of Wine's wineserver): the job
table that `launchctl`, `SMJobSubmit` and friends talk to.

## The server

* **One per Windows user, Alchemy root and Alchemy executable.** It is the
  Alchemy executable itself, started as `alchemy-fx launchd --root <root>
  [runtime options]`, listening on the named pipe
  `alchemy-launchd-<hash of user, root, executable>`. The pipe's default
  security gives read-write access to the creating user only (and to SYSTEM
  and administrators), and remote clients are refused.
* **Started on demand only**, by the first client that needs it: `launchctl`
  (load, bootstrap, enable, submit, kickstart, setenv) and `SMJobSubmit`.
  Read-only calls (`launchctl list`, `print`, `SMCopyAllJobDictionaries`,
  `SMJobCopyDictionary`) never start one. Ordinary application runs never do.
* **Not persistent.** It is not a host service and creates no startup entry,
  scheduled task or registry key. It writes only inside the Alchemy root
  (`/private/var/db/com.apple.xpc.launchd/disabled.1000.plist`, the
  enable/disable overrides) and its jobs' own output files.
* **Detached.** It is started outside the starting application's process job,
  with no console and no window, so that jobs can outlive the application
  (updaters install after the application quits). Where a job object Alchemy
  did not make refuses breakaway (a test driver's), the server stays in it and
  ends with it.
* **Lifetime.** Every client names the root of its Alchemy process tree (the
  process the user started, `ALCHEMY_MACH_BROKER`). The session stays alive
  while one of those trees runs. After the last one ends:
  * no timer, KeepAlive restart or demand launch starts a job any more;
  * running jobs get a grace window, `ALCHEMY_LAUNCHD_GRACE` seconds (default
    300; 0 ends them at once), then end as by SIGTERM;
  * the server exits `ALCHEMY_LAUNCHD_LINGER` seconds (default 3) after it has
    nothing left. Its jobs are its children and end with it in any case.
* **Log:** standard error of the server (none by default).

## Jobs

* **launchd.plist keys acted on:** Label, Program, ProgramArguments (PATH
  search when Program is absent), RunAtLoad, KeepAlive (true, or
  SuccessfulExit, Crashed, PathState, OtherJobEnabled), OnDemand,
  StartInterval, StartCalendarInterval, EnvironmentVariables,
  WorkingDirectory, StandardInPath, StandardOutPath, StandardErrorPath,
  ThrottleInterval, Disabled, LimitLoadToSessionType (Aqua, Background and
  StandardIO load). MachServices and Sockets are read and reported (on-demand
  launch is the next batch).
* **Accepted without effect:** resource limits, priorities, ProcessType, user
  and group names, and other keys launchd honours on macOS. `launchctl print`
  lists them under "keys without effect under Alchemy".
* **Processes.** A job runs as an Alchemy child of the server (its own process
  tree), with `getppid()` 1, a new session, and launchd's environment: PATH,
  HOME, USER, LOGNAME, SHELL, TMPDIR, `XPC_SERVICE_NAME` (the label),
  `XPC_FLAGS`, the session's `launchctl setenv` values and the job's
  EnvironmentVariables. Standard streams default to /dev/null; output paths
  are appended to.
* **Signals.** As kill(2) between Alchemy processes: a signal whose default
  action ends a process ends the job (it cannot run a handler for a signal
  sent from outside); stopping signals are refused.

## launchctl

A built-in tool child (`launchctl.cpp`), like `sh`. `asuser` still execs its
program directly (`process/launch.cpp`).

* **Domains:** `gui/1000` and `user/1000` (Alchemy's one user) are the
  session. `system` is refused with EPERM, as for a user who is not root.
  Alchemy runs no system launchd.
* **Subcommands:** bootstrap, bootout, enable, disable, kickstart [-k] [-p],
  kill, print, print-disabled, load [-w] [-F], unload [-w], list [label],
  start, stop, remove, submit, setenv, unsetenv, getenv, managerpid,
  manageruid, managername, error, version, help.
* **Refused by name:** reboot, bsexec, config, dumpstate, procinfo, blame,
  attach, debug and the other subcommands about the macOS boot, the kernel or
  debugging. They exit 45 (ENOTSUP) with a message.
* **Exit statuses** are launchctl's: errno values, and 113 (no such service),
  119 (disabled), 37 (already loaded).

## ServiceManagement

`service_management.cpp` (alchemy-fx only):

* **SMJobSubmit and SMJobRemove** work in the user domain.
* **SMJobCopyDictionary and SMCopyAllJobDictionaries** answer from the session.
* **launch_activate_socket** answers ESRCH for a process that is not a job,
  and ENOENT for a job (no Sockets yet).
* **The system domain fails** with kSMErrorAuthorizationFailure.
* **Stay in `darwin/servicemanagement/`:** SMJobBless (refused) and
  SMAppService and login items (not registered: the user's decision is
  pending).

## Not here (refused)

These are refused, by the user's decisions of 2026-11-05:

* **Nothing at Windows login.** No host service, scheduled task, registry key
  or startup entry.
* **No system domain**, LaunchDaemons or privileged helpers.
* **On-disk LaunchAgents load at session start only with
  `ALCHEMY_LAUNCHD_AGENTS=1`.** They load from `~/Library/LaunchAgents` and
  `/Library/LaunchAgents`, as at a macOS login.

## Tests

* `alchemy_launchd_units` (`tests/launchd_tests.cpp`): the property-list
  reader and writer, and the job model.
* `alchemy_gui_launchd_service_management`
  (`tests/service_management_tests.cpp`): the ServiceManagement calls without
  a session launchd.
* `alchemy_launchd_session_fx` (`tests/fixtures/macos/launchd_model.c`, under
  alchemy-fx): `launchctl` end to end. It covers bootstrap, list, print,
  kickstart, kill, bootout, the system domain, submit and remove, setenv and
  getenv, and `system("launchctl ...")`.
