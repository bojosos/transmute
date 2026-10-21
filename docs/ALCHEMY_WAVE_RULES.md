# Alchemy wave rules

These rules hold for every multi-session wave: an integrator session plus
groups, each group a separate session. Each wave brief
(`ALCHEMY_BRIEF_*.md`) names its groups and what each owns, and refers
here for everything else. They were the Chrome wave 1 rules, kept with
what that wave taught.

## Roles

* **Integrator.** It starts the groups and gives each a brief. When a group
  reports, the integrator checks the result and merges the group's branch
  into `integ/<wave>`. It routes each new stop to the group that owns it,
  re-runs the wave's check and lands the branch in `main` once the user
  says so.
  * Groups never merge into `main`.
* **Group.** It owns the files or directories its brief names. It works on
  `mac/<wave>-<group>` in `.claude/worktrees/<wave>-<group>`.
  * It fast-forwards or merges `integ/<wave>` whenever the integrator says
    the integration branch moved.

## Branch and build

* **Worktree:**

  ```text
  git -C C:\dev\transmute worktree add .claude/worktrees/<wave>-<group> -b mac/<wave>-<group> main
  ```

  Work only there. The main checkout `C:\dev\transmute` stays clean.
* **Build directory and configuration.**
  * Use your own build directory inside your worktree.
  * Configure with `-DTRANSMUTE_WITH_FX=ON`; without it there's no
    `alchemy-fx` and no GUI tests.
  * Copy the other `ALCHEMY_*` cache values from an existing build.
* **What to build.**
  * Build `alchemy-fx` explicitly. A full build stops at the macOS libc++
    fixture on machines without its headers, before relinking
    `alchemy-fx`.
  * Build only the targets you need, at `/m:2` to `/m:4` (`/MP` is on).
  * Several sessions build at once on one 12-core machine, and an
    integration build under that load takes 15–25 minutes.
* **Shared junction.** Each worktree needs an ignored junction,
  `artifacts\alchemy-llvm-source`, pointing at the main checkout's copy.
* **Tests.**
  * Add unit tests for what you implement, in the provider's existing test
    layout. Write a regression test that fails on the old code for every
    bug fix.
  * Before reporting, run `ctest -R "^alchemy_"`, cloaked.
  * `alchemy_gui_windowserver_win32` fails while the workstation is locked
    (clipboard error 5, activation). It is not a regression.
* **Commits.** Small logical commits, ending with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Implementation policy

* **Real behaviour first:** implement real behaviour over Windows wherever
  it's feasible and plausibly reached.
* **Otherwise fail explicitly:** use the documented error return or NULL,
  or an explicit "unsupported" stop with a clear message. Never invent a
  success.
* **Constants:** use Apple's public-header values. Flag any value you are
  unsure of in a code comment and in your report.
* **Classes:** referenced Objective-C classes are registered, and classes
  the guest subclasses are real, subclassable host classes.
* **No app-specific behaviour:** never branch on bundle IDs or executable
  names. Every provider serves every program.
* **Locks:**
  * Never hold a host lock across a guest call, an Objective-C
    release/dealloc or a callback.
  * Release outside the lock. Wave 1's shutdown deadlock was exactly this.
* **Struct layouts:** write Darwin arm64 layouts exactly, and test sizes
  and offsets. Wave 1's `statvfs` wrote 88 bytes into a 64-byte struct and
  corrupted the caller's stack.

## Hard rules

* **No Apple code, SDKs or binaries.** Use public documentation and public
  open-source designs (Chromium, Apple's open-source releases) only.
  * Don't disassemble guest apps.
  * Use load commands, symbol tables, `doctor`, logs and Alchemy's traces
    (`ALCHEMY_TRACE_CALLS`, `ALCHEMY_TRACE_OBJC`, and `ALCHEMY_PERF=1`,
    which names synthetic calls in error messages).
  * Never patch guest binaries.
* **No downloads.** If you think you need one, stop and report it.
* **GUI runs are cloaked:** `ALCHEMY_GUI_CLOAK=1`,
  `ALCHEMY_GUI_NO_ACTIVATE=1`, `ALCHEMY_GUI_NO_CRASH_DIALOG=1`. Never
  inject input.
* **Network:**
  * Every guest app run sets `ALCHEMY_NETWORK=loopback`, which keeps the
    whole process tree off the network and logs each denied attempt.
  * Audit each run's stderr for `network denied` lines and report any new
    target.
  * An app's own offline flags are not enough: Chrome's updater child went
    out through NSURLSession, and the browser probed a numeric IPv6
    address.
* **Never commit** anything under `artifacts/`: downloads, DMGs,
  extracted apps or roots.
* **Shared runtimes are read-only:** `artifacts/alchemy-llvm-arm64` and
  `artifacts/alchemy-swift-arm64`.
* **Test roots are read-only.** These are the renderer add-on's, the QLab,
  Blender and Chrome roots.
  * Apps that write into their root run on a scratch copy.
  * Scratch HOME and user-data directories go under `<root>/Users/<group>/`
    or your scratchpad.
* **Processes:** kill only processes you started, never by image name
  machine-wide.

## Regression check before landing

A wave lands in `main` only after these pass, compared against `main`:

* the full `ctest -R "^alchemy_"`;
* Blender windowed startup (`scripts/perf/startup.ps1`) and headless
  (`-b --factory-startup`);
* QLab launch to its welcome window;
* the renderer add-on headless cube;
* Chrome strict headless `--dump-dom` of `probe/test.html`, which must
  exit 0 with `data-js="ran"` in the dump.

Wave 1's comparison results and method are in the c1 regression notes
(see `ALCHEMY_CHROME_AUDIT.md`, "Wave 1 result").

## Report (a group's final message)

* Branch and commits.
* What is now real, and what fails explicitly.
* Tests run.
* Doctor numbers for your area, before and after.
* Every app run: where it stopped, and the `network denied` lines.
* Edits outside your area.
* What you didn't verify.
