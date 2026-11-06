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
  StandardIO load), MachServices (below) and Sockets (below).
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

## MachServices (alchemy-fx)

`mach_services.cpp` and the session launchd hooks in
`libsystem/mach/mach_remote.cpp`:

* **The server holds the ports.** It runs a Mach port space with no guest code
  and makes a port for each MachServices name when the job loads.
* **Look-ups work from any process tree.** A `bootstrap_look_up` that a tree's
  own registry misses asks the session launchd, if one runs, and gets a send
  right to its port. The tree's own dynamic names behave as before.
* **On-demand launch.** Messages queue at the server. The first one starts the
  job (throttled by ThrottleInterval) when an application uses the session.
* **Check-in.** The job's `bootstrap_check_in` of a name its job declares
  takes the receive right; the queued messages follow, and later look-ups
  reach the job directly. Only that job's process can check the name in.
  HideUntilCheckIn hides the name until then.
* **Job exit.** A port the job had checked in dies with it, and the server
  makes a new one for the next run. Old send rights become dead names, as an
  XPC client sees "interrupted".
* **alchemy.exe** (no cross-process Mach layer) loads such jobs, but their
  services have no ports. `launchctl print` says so.

## Sockets

* **What is made.** Listening sockets are made at load: a Unix-domain path
  (SockPathName), or a numeric TCP or UDP port on SockNodeName. Without a
  node they bind every address, or only loopback under
  `ALCHEMY_NETWORK=loopback`. IPv4 and IPv6 are both made unless SockFamily
  picks one.
* **Every run of the job gets them**, at descriptors 3 and up.
  `launch_activate_socket(key)` returns them, and `launch_msg("CheckIn")`
  answers Label, Sockets and MachServices.
* **Refused, with a log line:** connecting sockets (SockPassive false) and
  named services (only port numbers).

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
* **launch_activate_socket** returns a job's sockets (a malloc'd array). It
  answers ESRCH for a process that is not a job, and ENOENT for a key the
  job does not have.
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
  alchemy-fx). Three cases:
  * `launchctl`: launchctl end to end. It covers bootstrap, list, print,
    kickstart, kill, bootout, the system domain, submit and remove, setenv
    and getenv, and `system("launchctl ...")`.
  * `mach`: a job's service found from another process tree. The first
    message launches the job, which checks in and answers; a second request
    reaches the same job; the service is gone after bootout.
  * `sockets`: `launch_activate_socket` and its errors, and the CheckIn reply.
