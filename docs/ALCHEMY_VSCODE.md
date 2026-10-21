# Visual Studio Code (macOS arm64) under Alchemy

Visual Studio Code is an Electron app: Chromium for the windows, and Node.js
for the main process, the extension host, the language servers, the pty host
and the file watchers. Under Alchemy it opens a folder and a file with syntax
highlighting, searches the workspace with ripgrep, and runs integrated
terminals through node-pty.

Every gap was fixed generically, in the framework or libSystem area that owns
the name, with a test. VS Code and Electron run unmodified.

## Input

* **Build.** VS Code 1.142.0, stable, darwin-arm64 (commit `6d62b1d5…`), from
  the update service's zip:
  * file `VSCode-darwin-arm64.zip`, 316,862,344 bytes;
  * SHA-256 `099d3514fda308822a150d65ea36d4c4bfc565ee1b7fbc5a3fff97700f7af408`,
    which is the hash the update service publishes.

  Its Electron runs Node v24.21.0.
* **Root.** `artifacts/alchemy-root-vscode`, installed with
  `scripts/alchemy_install_dmg.ps1`, which also takes a zip. The zip is a
  local test input and is not redistributed.
* **Network.**
  * The harness's settings.json turns off updates, telemetry, experiments, and
    the online extension and settings-search lookups.
  * Runs use `ALCHEMY_NETWORK=loopback`.
  * Only the loopback DevTools connection and the processes' own IPC are
    allowed.

## Running it

`scripts/vscode/run_vscode.ps1` (see its header) has two modes:

* **CLI mode.** For example:
  * `-Cli @('--version')`;
  * `-RunAsNode -Cli @('<script.js>')`, which runs Electron as plain Node.

  It runs Code and waits for it to exit.
* **Windowed mode.** Code starts with `--remote-debugging-port=0`, and the
  harness talks to it over the DevTools protocol on 127.0.0.1:
  * it waits for a page title;
  * it can evaluate expressions and save the renderer's own frame
    (`-Screenshot`);
  * it quits with `Browser.close`.

  The run is cloaked. User data and HOME go under `<root>\Users\vscode\`,
  with a fresh profile per run. VS Code does not watch paths under
  `/Volumes`, so the profile can't live on a mapped drive.

`-Probe` adds a workspace and loads `scripts/vscode/probe-extension` as a
development extension (`--extensionDevelopmentPath`, plus
`--enable-proposed-api` for that extension). The workspace holds `hello.ts`,
`README.md`, `src/data.json` and `.vscode/settings.json`. Through the public
extension API only, and without any input events, the extension:

* opens the file;
* runs `workspace.findFiles` and `findTextInFiles`, both through ripgrep;
* shows the Search view;
* starts two integrated terminals. Their process is Electron's Node
  (`ELECTRON_RUN_AS_NODE`), so no guest shell is needed.

It writes every outcome to `<workspace>/.alchemy-probe.json`, which the
harness copies to `probe.json`. When it finishes, it sets the window title to
"probe done". The harness then saves the window capture as
`capture\window-1-settled.bmp`. Captures stay out of git.

```
powershell -File scripts\vscode\run_vscode.ps1 -Exe build-ninja\Release\alchemy-fx.exe -OutDir <scratch> `
    -Root artifacts\alchemy-root-vscode -RuntimeDir artifacts\alchemy-llvm-arm64 -Probe -Frames 2 -Settle 15
```

## Results (2026-10-21)

Windowed probe run, alchemy-fx, on a 13th Gen Intel Core i7-1355U laptop:

| Step | Result |
| --- | --- |
| DevTools connected | 3.6 s |
| Workbench page | 22 s |
| Window with the folder and `hello.ts` (title) | 66 s |
| `hello.ts` opens, TypeScript highlighting | ok, 0.6 s |
| `findFiles('**/*')` (ripgrep `--files`) | all 5 files, 8.0 s |
| `findTextInFiles('greet')` (ripgrep) | 8 matches in 3 files, 4.4 s |
| Search view | "18 results in 4 files" (the probe's own report is the fourth) |
| Integrated terminal (node-pty, Electron's Node) | runs, output arrives, exit code 0 |
| `Browser.close` | Code exits 0 about 16 s later |

The terminal's process reports what it sees. Through the pseudo-terminal,
the extension receives:

```
hello from the integrated terminal darwin arm64 tty 114x16
```

So Node sees a TTY of the panel's size, and its `\n` arrives as `\r\n`. The
second terminal, left open, shows `Node v24.21.0 in a pseudo-terminal: true
114x16` in the captured window.

Timings vary with the machine's load. In two later runs under load, the
title appeared at 133 s and the probe finished at 403–413 s; every step
still passed.

The final run on a quiet machine, rebased on the landing base, finished
the probe at 149 s. Code exited 0 about 15 s after `Browser.close`. Its
main log has no error line: the login environment resolved, and the
storage backups succeeded.

The window capture of the settled state shows the workbench, with the Search
view, the highlighted editor and the terminal panel. The headless milestone
also works: `ELECTRON_RUN_AS_NODE=1 Code --version` prints `v24.21.0` in
3.7 s.

## What was needed

Missing names were found with `ALCHEMY_PROBE_MISSING=trap`, which stops at
the first unresolved name and prints the guest backtrace. Each fix landed in
the area that owns the name. Commits are on `claude/vscode-c5c8bc`.

* **Three new framework providers.** Each answers the documented
  "unavailable" way and has a README and a test:
  * QuickLookUI (`darwin/quicklookui/`): no previews;
  * StoreKit (`darwin/storekit/`): no App Store;
  * CoreData (`darwin/coredata/`): the identity only.
* **About 85 missing names**, each implemented in its owner:
  * libSystem:
    * `qsort_r`/`qsort_b`;
    * the `OSAtomic*32` family;
    * `recvmsg_x`/`sendmsg_x`;
    * `getgrnam_r`, `lgamma`, `getsectdata`, `task_name_for_pid`;
    * `fclonefileat` (ENOTSUP).
  * libobjc: `class_copyMethodList`.
  * CoreFoundation: `_CFCopySystemVersionDictionary`.
  * Foundation:
    * `NSValueTransformer`, `NSFileCoordinator`;
    * the Apple-event manager's current event.
  * AppKit:
    * Touch Bar items, the `NSScrubber` family, `NSColorList`;
    * workspace session notifications.
  * QuartzCore: `CATransaction` commit handlers and completion blocks.
  * Security, ServiceManagement, LaunchServices, CoreGraphics window levels,
    and AX trust.
* **Loader.**
  * Flat-namespace imports (`-undefined dynamic_lookup`, as Node add-ons are
    built) now also search the provider frameworks that the loaded images
    link.
  * AudioToolbox and AudioUnit resolve as re-export partners.
  * `/usr/lib/libSystem.dylib` is an alias.

  The test files `electron_imports.inc` and `vscode_native_imports.inc` list
  every strong system import of Electron and of VS Code's native modules.
* **libuv (Node) needs:**
  * `ioctl(FIONBIO)` on pipes;
  * `kqueue` `EVFILT_PROC` `NOTE_EXIT` for every child (an abort without it);
  * `isatty` of a socket on descriptors 0–2 is 0;
  * `sendfile` returning `ENOTSOCK` for files (the copy fallback);
  * an `AF_UNIX` stream whose peer reset reads end-of-file;
  * `chmod` ignoring the file-type bits (`fs.copyFile` passes `st_mode`).

  Without the `isatty` fix every piped Node child exited 1: Node called
  `tcgetattr` on what it took for the console. That covered the JSON language
  server and the `ECONNRESET`s in main and the extension host.
* **FSEvents.** Real streams (`coreservices/fsevents.cpp`) over a new host
  directory-tree watch (`host::files::watch_tree`, ReadDirectoryChangesW).
* **Run loop.** `NSRunLoop` perform requests and their cancellation.
* **AppKit.** `setDelegate:` no longer messages a freed old delegate.
* **Saving settings.** `rename` over a file that is open elsewhere
  (POSIX-semantics rename on the host).
* **ripgrep.** Rust maps its main-thread guard page with `mmap(MAP_FIXED)`
  inside the stack; that is now allowed below the stack pointer. Before, rg
  aborted at start, and search fell back to open editors only.
* **Integrated terminal: pseudo-terminals** (`libsystem/fs/terminal.cpp`).
  * **Calls:** `posix_openpt` and `/dev/ptmx`, `grantpt`, `unlockpt`,
    `ptsname`, opening `/dev/ttys<n>`, `openpty`, `ttyname`,
    `tcgetattr`/`tcsetattr`, `tcgetpgrp`/`tcsetpgrp`, and the terminal ioctls.
  * **Design:**
    * a terminal is a connected stream pair;
    * its termios, window size and foreground group are in a named section
      that every process with an end maps;
    * ends keep their identity across `posix_spawn` and `exec`.

    So node-pty's spawn-helper reopens its slave by name, and Node in the
    terminal sees a TTY with the right size.
* **Shell environment.** VS Code reads the login environment with
  `$SHELL -i -l -c "'Code' -p '…JSON.stringify(process.env)…'"`. Alchemy's
  built-in shell now:
  * accepts `-i` and `-l` with `-c`;
  * keeps the socket pairs Node gives a child as its stdio, and passes them on
    to the programs it runs. Before, it dropped them, and the command exited 1
    with no output.

  The command returns the environment in about 2 s.
  `application.shellEnvironmentResolutionTimeout` is set to 60 in the
  harness: starting a second Code under load can exceed the 10 s default.

## Open items

* **Line discipline.** The pseudo-terminal applies only the byte mappings
  (`OPOST|ONLCR` on output; `ICRNL`, `INLCR` and `IGNCR` on input). It does
  not provide:
  * echo;
  * canonical-mode line editing;
  * signals from control characters (`ISIG`);
  * `SIGWINCH` on a resize.

  Programs that switch the terminal to raw mode, as shells and REPLs do, do
  not need these.
* **No interactive shell.** The guest has no interactive shell; Apple's shells
  are not used. The default terminal profile (`$SHELL`) reaches Alchemy's
  built-in non-interactive shell, which reads a script from standard input
  until end-of-file. The probe uses Electron's Node as the terminal process.
* **No `lsof`.** VS Code asks `lsof` for a terminal's working directory, and
  logs that it is missing.
* **Window geometry.** A full-size-content window's AppKit content
  (1200x800) is larger than the host client area (1184x761). The top 39
  points and the right 16 are cut off. Chrome has the same gap.
* **Startup time.**
  * The workbench needs about 22 s.
  * The extension host needs about 2 minutes before `onStartupFinished`
    extensions activate. VS Code warns that it "did not start in 10 seconds".
* **Harmless log noise.** `task_name_for_pid` failures from Electron's
  code-signing check.
* **Network denials.** These are expected with the loopback policy:
  * `[2001:4860:4860::8888]:443`, Chromium's connectivity check;
  * `main.vscode-cdn.net`;
  * `marketplace.visualstudio.com`.
