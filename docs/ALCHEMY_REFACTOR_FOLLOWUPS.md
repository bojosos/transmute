# Refactor wave: later-wave items

The provider refactor wave ([ALCHEMY_BRIEF_REFACTOR.md](ALCHEMY_BRIEF_REFACTOR.md))
excluded behaviour fixes on purpose. Each group listed what it left for a
later wave. This page gathers those lists in one place and says what the
follow-up branch `mac/refactor-followups` did with each item.

Sources:

* **r1, r2, r3:** the groups' final reports (`r1-final.md`, `r2-final.md`,
  `r3-final.md`) and `r1-errno-fidelity.md`, in the wave's side-session
  notes.
* **r4:** [FX_HOST_SEAMS.md](FX_HOST_SEAMS.md), the items marked **left**.
* **integ:** the integrator's wave result, "Later waves", in
  [ALCHEMY_BRIEF_REFACTOR.md](ALCHEMY_BRIEF_REFACTOR.md).

Sizes: **S** is under a day with its test, **M** is a few days or needs an
app regression, **L** is a wave of its own.

## The list

| # | Source | What | Area | Size | Decision |
|---|---|---|---|---|---|
| 1 | r1, integ | `alchemy_process_tree_fx` "(rebased)" flake: reserve the rebased load range first thing in the process | fx startup | S | **do** |
| 2 | r1, r2 | libresolv waits on a `net::ready(socket, events, timeout)` instead of polling every 1 ms | host net, network | S | **do** |
| 3 | r1 | errno residuals of `r1-errno-fidelity.md`: decide each row by Darwin's semantics, fix and pin | host win32, libSystem | S | **do** (see "errno residuals") |
| 4 | r3 | AppKit's `NSEventTrackingRunLoopMode` / `NSModalPanelRunLoopMode` are not the run loop's mode objects, so a guest that compares the current mode by pointer fails | appkit, runloop | S | **do** |
| 5 | r3 | `host::Result<T>::operator->` returns `&value_`, which a type that overloads unary `&` (WRL `ComPtr`) breaks; use `std::addressof` | host common | S | **do** |
| 6 | r3 | Stale pre-refactor paths in docs and header comments | docs | S | **do** |
| 7 | r3, integ | Restore the real `CGDisplay*` implementations over the window server's screens, with alchemy-fx as the owner and a headless pair | coregraphics | M | **done** later, on its own branch with the app regression (see "7. CGDisplay") |
| 8 | r3, integ | The Swift runtime was not rebuilt after the overlays moved | build | S | **defer**: runtime targets write the shared artifacts and may not be built from a worktree (wave rules); the user or the integrator rebuilds it from the main checkout |
| 9 | integ | The LLVM runtime embeds a worktree path in assertion `__FILE__` strings | build | S | **done** by the Chrome audit integrator: `artifacts/alchemy-llvm-arm64` restored to its verified hashes, C++ runtime and exception tests passing |
| 10 | r3 | Re-check `artifacts/alchemy-expat-arm64` against its provenance after the step 1 rebuild | build | S | **done already**: the integrator found the expat and Swift deliverables byte-identical |
| 11 | integ | Merge the five kits, `gui::fill::Provider` and `security::Bridge` into one provider helper; one installer row per framework | providers | L | **defer**: a refactor wave of its own |
| 12 | integ | Forward declarations in wide headers (build time) | build | M | **defer**: build-time work with no behaviour, best done with the kit merge (#11) |
| 13 | r4, integ | `TRANSMUTE_FX_SAMPLE` plus `TRANSMUTE_FX_CORESTATS` can hang Blender headless | fx diagnostics | M | **defer**: pre-existing, diagnostic-only (both are opt-in measurement switches), and reproducing it needs Blender runs at length |
| 14 | r2 | NSURL volume capacity keys now need `GetVolumeInformationW` to succeed as well as `GetDiskFreeSpaceExW` | host files | S | **defer**: no such volume on this machine, so no failing test can be written; revisit if a guest reports missing capacity keys |
| 15 | r2 | `HttpExchange::read` before send gives `NSURLErrorUnknown` (restored and pinned) | http | — | **no action**: a note for whoever changes URL loading's order |
| 16 | r1 | Linux backends for process, files, sections, sync, time, net, ipc | host | L | **defer**: Linux is out of scope until the user asks |
| 17 | r2 | Linux backends for locale, crypto, dns_sd (ICU, OpenSSL, Avahi) and http (libcurl) | host | L | **defer**: Linux |
| 18 | r3 | Linux backends for ui, gpu, audio, midi | host | L | **defer**: Linux |
| 19 | integ | A Linux backend from the null build (`ALCHEMY_HOST_LINUX.md`) | host | L | **defer**: Linux |
| 20 | r4 | Win64 calling convention and unwind format of generated code (SysV for Linux) | fx translator | L | **defer**: a code-generation change for Linux |
| 21 | r4 | PE guest personality left on Win32 (loader, abi, seh, PE AOT) | fx | — | **no action**: Windows-on-Windows by purpose; a Linux host builds without it |
| 22 | r4 | `std::wstring` paths in `GuestModule`, MSVC `/alternatename`, CRT `_stricmp`, x86 intrinsics | fx | M | **defer**: compiler and Linux portability, not behaviour |
| 23 | r1 | Callers' remaining non-Windows fallback branches | host callers | M | **defer**: they go when the Linux backends exist (#16–#19) |
| 24 | r1 | errno residuals that would need new `host::Error` kinds | host | — | **covered by #3**: no row needed a new kind |

## What was done

### 1. The rebased load flake

* **Cause.** Windows places thread stacks and heap segments bottom-up from
  a random base chosen per process. alchemy-fx kept
  `[0x1'0000'0000, 0x2'0000'0000)` reserved only while the fx runtime
  started. Between that and the executable's mapping, providers start
  threads, and a stack placed bottom-up from a base just below the load
  address climbs into it. Mapping then fails with "requested segment address
  is unavailable".
  * A rebase to `0x1'2000'0000` leaves a 512 MiB window below the image for
    that base to fall into. The default image at `0x1'0000'0000` has almost
    none, which is why only the rebased runs failed.
* **Fix.**
  * `wmain` calls `FxEngine::hold_load_range` before anything else
    allocates. It reserves the range: `--load-base`, else
    `0x1'0000'0000`, for 4 GiB, merged with the engine's window when it
    overlaps.
  * Every guest placement at a fixed address checks the hold. The first one
    inside the range (the executable's own segments) releases it and maps
    there at once.
  * `--load-base` is passed on to guest children, so each child holds its
    range too.
  * The hold reserves every free part of the range, up to 64 pieces.
    * The first version reserved the whole range in one call. Under
      `-j 12` a child once found part of the range already taken before
      `main`: the Chrome stage 2 integrator saw one `launchctl (rebased,
      loop)` failure.
    * In that case the reservation failed and nothing was held.
    * The test now also takes `0x1'f000'0000` before the hold
      (`ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD`): the all-or-nothing hold fails
      it, and the piecewise hold passes.
* **Test.** `alchemy_process_tree_fx_rebased`:
  * `ALCHEMY_FX_TEST_TAKE_LOAD_RANGE` makes a host reservation in the range
    just after the engine starts, as a stray stack would.
  * With the hold, the reservation is refused and `process_tree identity`
    loads, rebased (two addresses) and at the default base.
  * Without the hold (a build with the call removed), all three runs fail
    with "requested segment address is unavailable".
  * The test then runs `identity`, `dev` and `launchctl` rebased from 6
    drivers at once, 3 rounds: 54 runs.
  * Before the fix, about 480 rebased runs from 8–12 drivers at once (one
    during a build) passed without a failure. A loop alone cannot show the
    fix, so the hold check is the deterministic part.

### 2. libresolv waits instead of polling

`host::net::ready(socket, events, timeout_ns)` waits with `WSAPoll` and
answers 0 on timeout (the null backend: unsupported). libresolv's UDP and
TCP exchanges wait on it for the time left before their deadline. They used
to sleep 1 ms between polls. `alchemy_host` pins three cases: the timeout on
an idle socket, the wake when a datagram arrives, and a zero-timeout poll.
`alchemy_gui_network` still resolves through libresolv.

### 3. errno residuals

See "errno residuals" below. The fixes are in `host/win32/win32.cpp` (six
codes get their kind), `utimes` (file table) and `fcopyfile` (EBUSY, as
`ftruncate`).

### 4. Run-loop mode objects

AppKit exports the run loop's own mode objects as
`NSEventTrackingRunLoopMode` / `NSModalPanelRunLoopMode`
(`gui::run_loop_mode_object`). When AppKit is installed first, the run loop
adopts AppKit's objects instead. Either way, `-[NSRunLoop currentMode]` and
the constants are the same object. `alchemy_gui_appkit_unit` checks both
install orders.

### 5. `Result<T>::operator->`

It uses `std::addressof`. `alchemy_host` pins it with a type whose unary `&`
returns null.

### 6. Stale paths

* **Fixed.** Code comments in `include/` and `src/` that named
  pre-refactor directories, `ALCHEMY_QUARTZ2D.md`, and the installer table
  in `include/alchemy/gui/install.hpp`.
* **Left as they are:**
  * The comments in the embedded `runtime/msl/__msl` headers. Their text
    feeds the MSL stdlib fingerprint, so editing a comment would invalidate
    every shader and pipeline cache.
  * The ownership sections of `ALCHEMY_QLAB_GAPS.md`, which record an
    earlier wave. `ALCHEMY_PROVIDERS.md`, "Paths before the refactor", maps
    them.

### 7. CGDisplay

Done after the follow-up branch, on its own branch, because it changes what
every GUI app sees.

* **Owner.** `darwin/coregraphics/displays.cpp` registers every CGDisplay
  query in alchemy-fx over `WindowServer::screens()`. That covers:
  * the display lists, displays at a point or in a rect, and the main
    display;
  * bounds, pixel and physical size, the `CGDisplayIs*` predicates,
    mirroring and the OpenGL display mask;
  * the current mode and its getters, retain and release, and setting the
    current mode back;
  * reconfiguration callbacks, which are kept but never called;
  * `CGDisplayCreateImage`, which returns NULL.
* **Ids.** A display's id is its `ScreenInfo::id`, the number
  `NSScreenNumber` and CVDisplayLink use.
* **Headless pair.** `coregraphics_headless.cpp` answers the same names
  for `alchemy.exe` with no display.
* **Old owners removed:**
  * AppKit's `carbon.cpp` display block (the cursor functions stay);
  * the AppKit shell's `CGGetActiveDisplayList`;
  * the identities' "no displays" answers, which registered first in every
    process and so shadowed the fx code;
  * Qt's `cg_extra.cpp` fills.
* **Audit:** 0 duplicates, 0 skipped fills.
* **Tests:**
  * `alchemy_gui_coregraphics_displays`: two fixed screens, then the host's
    screens against `host::ui::monitors()`.
  * `alchemy_frameworks`: the headless answers.
  * Full `ctest -R "^alchemy_"`, cloaked, `-j 12`: 178/178 (main's 177
    plus the new test).
* **App regression against main, cloaked, `ALCHEMY_NETWORK=loopback`,
  interleaved:**
  * Blender windowed: exit 0, 3 runs per build.
  * Blender headless: exit 0, same CPU.
  * QLab's "Welcome to QLab" window: pixel-identical to main.
  * Renderer add-on cube: renderer created, render returned. There is no licence
    under loopback.
  * Chrome strict `--dump-dom`: `data-js="ran"`.
  * `doctor`: identical for QLab, the renderer add-on and Chrome.
  * Traced runs show that the startups of QLab, Blender and headless Chrome
    make no CGDisplay call. They read the screens through NSScreen.

## errno residuals

These are the rows of `r1-errno-fidelity.md`: Win32 codes that one host
kind now groups differently from the old per-site tables. Each row is
decided by the errno Darwin documents for the situation the code reports,
and every row is pinned in `alchemy_gui_libsystem_errno` ("residuals").

| Site | Win32 code | Before the refactor | After the refactor | Now | Why |
|---|---|---|---|---|---|
| file | INVALID_DRIVE, BAD_NETPATH, BAD_PATHNAME, MOD_NOT_FOUND, PROC_NOT_FOUND | EIO | ENOENT | ENOENT | a missing path, volume or share |
| file | BAD_NET_NAME (67), NO_MORE_FILES (18) | EIO | EIO | **ENOENT** | the same (now `not_found`) |
| file | LOCK_VIOLATION (33) | EIO | EACCES | EACCES | a locked range; `flock`/`fcntl` sites already answer EAGAIN themselves |
| file | CALL_NOT_IMPLEMENTED (120) | EIO | ENOSYS | ENOSYS | "function not implemented" |
| file | COMMITMENT_LIMIT (1455) | EIO | ENOMEM | ENOMEM | out of memory |
| file | BUSY (170), PIPE_BUSY (231) | EIO | EBUSY | EBUSY | resource busy |
| file, spawn | ARENA_TRASHED (7), INVALID_BLOCK (9), NOT_ENOUGH_QUOTA (1816) | EIO | EIO | **ENOMEM** | memory or quota exhausted (now `no_memory`) |
| file, spawn | INVALID_TARGET_HANDLE (114), DIRECT_ACCESS_HANDLE (130) | EIO | EIO | **EBADF** | a bad handle (now `bad_handle`) |
| C runtime | ARENA_TRASHED, INVALID_BLOCK, NOT_ENOUGH_QUOTA | ENOMEM | EINVAL | **ENOMEM** | as before the refactor |
| C runtime | NO_MORE_FILES, BAD_NET_NAME | ENOENT | EINVAL | **ENOENT** | as before the refactor |
| C runtime | INVALID_TARGET_HANDLE, DIRECT_ACCESS_HANDLE | EBADF | EINVAL | **EBADF** | as before the refactor |
| C runtime | exec-format codes 188–202, NO_PROC_SLOTS, MAX_THRDS_REACHED | EIO | EINVAL | EINVAL (kept) | No descriptor call (read, write, lseek, dup, fsync, ftruncate, chmod, access) can fail with these. Darwin's values (ENOEXEC, EAGAIN) belong to exec and spawn sites. |
| C runtime | BUSY (170) | EINVAL | EACCES | EACCES (kept) | Shares `busy` with a sharing violation, which these sites report as EACCES. Darwin's EBUSY would need a kind of its own, and descriptor I/O does not return this code. |
| `fcopyfile` truncate | USER_MAPPED_FILE (1224) | EINVAL | EACCES | **EBUSY** | as `ftruncate` (Darwin allows the shrink; the host refuses it while a view exists); pinned in `alchemy_gui_libsystem_leftovers` |
| socket | WSA_INVALID_HANDLE, WSA_INVALID_PARAMETER, WSA_OPERATION_ABORTED, WSA_IO_PENDING | EIO | EBADF, EINVAL, EINTR, EAGAIN | kept | the meaning of each; only overlapped WinSock calls return them, and none are made |
| `utimes` | SetFileTime fails after the open | EIO | ENOENT | **EIO** | `utimes(2)`: the file exists. `utimes` now uses the file table, so EPERM, EROFS and ENAMETOOLONG also reach the guest instead of ENOENT. |

No row needed a new `host::Error` kind.

## Tests run

* **Full suite.** `ctest -R "^alchemy_"`, cloaked, without `ALCHEMY_NETWORK`, at `-j 8`: **177/177**. That is main's 176 tests plus the new `alchemy_process_tree_fx_rebased`. On main, the integrator's run had one environmental failure: `windowserver_win32` on focus.
* **Per item:**
  * #1: `alchemy_process_tree_fx` and `alchemy_process_tree_fx_rebased`. The hold check was also run against a build without the hold, where all three runs failed with "requested segment address is unavailable".
  * #2, #5: `alchemy_host`, on both backends (an `ALCHEMY_HOST=null` build of `alchemy_host_tests` passes), and `alchemy_gui_network` (libresolv against real DNS).
  * #3: `alchemy_gui_libsystem_errno` and `alchemy_gui_libsystem_leftovers`.
  * #4: `alchemy_gui_appkit_unit` and `alchemy_gui_runloop`.
* **Not run:** app runs (Blender, QLab, the renderer add-on, Chrome). No item changes an app path except #1, which every fx run goes through and the full suite covers. Linux.
