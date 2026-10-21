# Brief: provider refactor (by framework, behind a host OS layer)

The rules for running this wave are in
[ALCHEMY_WAVE_RULES.md](ALCHEMY_WAVE_RULES.md). Run it after Chrome stage 1
has landed in `main` (2026-10-17) and before Chrome stage 2 (windowed)
starts.

**Goals**, in priority order:

1. **Clean and extensible.** Code is organized by the macOS framework it
   implements, each export has one owner, and adding a framework or an API
   follows one obvious pattern.
2. **Portable to another host.** All macOS behaviour is written once,
   against a small host OS layer. Windows is the first backend. Linux
   becomes "write `host/linux/`", not "rewrite the providers". This wave
   does not build the Linux backend, but proves the boundary holds.
3. **Behaviour unchanged.** Every program behaves exactly as on `main`
   before the wave.

## Why

* **Provider code is organized by the app that first needed it**, not by
  the framework it implements. Each app wave got a new directory
  (`kit/completion/`, `kit/support/`, `chromesupport/`, `export_fill/`, `fwgaps/`)
  so parallel groups never collided.
  * There is no app-specific behaviour: `install_darwin_gui` installs
    everything for every program, and nothing branches on a bundle ID.
  * The layout is still debt:
    * One framework lives in up to five places.
    * Fill-only gap fillers and later `add_native_method` calls make
      behaviour depend on install order (`src/alchemy/gui/install.cpp`).
    * Real duplicates exist.
    * Hot files rebuild most of the tree; `darwin_services.cpp` alone is
      about 4,900 lines.
* **Host OS code is mixed into the macOS code.** 63 of 357 provider
  `.cpp` files include Windows headers directly, and about 45 call Win32
  APIs: handles, virtual memory, file mappings, waits, sockets, WinHTTP.
  * The worst are the libSystem, Mach, dispatch, network, IOKit, run loop
    and keychain code, and `export_fill/libsystem_*`.
  * Darwin semantics (errno values, struct layouts, Mach rules) and Win32
    mechanics sit in the same functions. A second host would mean
    re-reading and re-splitting all of it.

## Architecture after the wave

```text
guest Mach-O ─▶ loader / dyld (macho, module_loader) ─▶ exports
                                                        │
          macOS layer: src/alchemy/darwin/<framework>/  │  Darwin semantics only:
          libsystem, mach, dispatch, corefoundation,    │  ABI layouts, errno/kern_return,
          foundation, appkit, quartzcore, ...           │  object model, framework rules
                                                        ▼
          host layer API: include/alchemy/host/*.hpp    host-neutral types and errors
                                                        │
          host backends: src/alchemy/host/win32/  (now) │  the only code that includes
                         src/alchemy/host/linux/  (later)│  OS headers
                         src/alchemy/host/null/   (test)
engine: fx (src/fx/...) with its own host seams (see "fx host seams")
```

### The host layer

* **The API is plain C++ in `include/alchemy/host/`.** It speaks
  host-neutral types (`host::Handle`, `host::Error`, byte spans, paths in
  UTF-8), never Darwin types and never OS types.
  * Darwin translation stays in the macOS layer: errno values,
    `struct stat` layouts, `kern_return_t`. For example,
    `host::Error::would_block` becomes `EAGAIN` in `libsystem/`, not in
    the backend.
* **Areas** (one header each; names are a proposal):

  | Header | Covers |
  | --- | --- |
  | `vm` | reserve, commit, protect, placeholders and views |
  | `sections` | shareable memory and cross-process export |
  | `files` | open/read/write, stat, directories, links, xattrs, locks, change watching |
  | `net` | sockets including AF_UNIX, poll, resolver |
  | `http` | the system HTTP stack, used by NSURLSession today over WinHTTP |
  | `process` | spawn, wait, exit watching, pids, environment |
  | `ipc` | the Mach transport's byte pipes and handle passing |
  | `sync` | threads, waits, events, timers |
  | `time`, `locale` | clocks, timezone, ICU/locale |
  | `crypto` | random, digests and ciphers |
  | `system` | CPU, memory pressure, power, battery, adapters |
  | `ui` | windows, input, display, vsync, clipboard |
  | `audio`, `midi` | device I/O |
  | `gpu` | Vulkan instance and surface; Vulkan itself is already portable |

* **Backends are chosen at compile time by CMake** (`ALCHEMY_HOST=win32`).
  * There are no virtual calls on hot paths: headers declare free
    functions or small concrete classes, and each backend defines them.
  * `host/null/` is a backend that compiles everything and fails each call
    explicitly. Building the macOS layer against it is the proof that no
    OS header leaks out of the backends.
* **The network policy (`ALCHEMY_NETWORK`) lives above the backends**, in
  the host API's common part. Every backend inherits it.

### The macOS layer

* **One directory per framework under `src/alchemy/darwin/`:**
  * `libsystem/` with subareas `vm/`, `mach/`, `net/`, `fs/`, `process/`,
    `malloc/`, `misc/`;
  * `dispatch/`, `corefoundation/`, `foundation/`, `appkit/`;
  * `quartzcore/`, `coregraphics/`, `coretext/`, `media/`;
  * `iokit/`, `systemconfiguration/`, `network/`, `security/`;
  * `compression/`, `metal/`, `msl/`;
  * `identities/`, for framework identities and small frameworks.
* **Each framework has:**
  * an `install.cpp`, registered in one ordered table;
  * a `README.md` of a few lines: what it covers, its host dependencies,
    and what is explicitly unsupported;
  * a `tests/` directory.
* **One registration API.** The overriding rules in
  `include/alchemy/gui/install.hpp` become one table-driven pattern:
  * a framework declares the exports, classes and selectors it owns;
  * the loader checks at start-up, in debug builds, that no two
    frameworks own the same name.
  * Fill-only remains only for stubs that are clearly marked as stubs.
    The registration audit lists any that are left.
* **Adding something follows one pattern.** A new API goes into its
  framework's directory, uses host calls for OS work, and registers in its
  framework's table. A new framework is a new directory and one table row.
  The pattern is documented in `docs/ALCHEMY_PROVIDERS.md`, which this
  wave writes.

### fx host seams

The engine is also Windows-specific in places, and a Linux host needs
those parts too. This wave:

* **Lists every host dependency of fx:**
  * fault handling: VEH today, signals on Linux;
  * the host calling convention of generated code: Win64 today, SysV on
    Linux (`jit_x64_windows.cpp`);
  * memory placeholders and views (`VirtualAlloc2`/`MapViewOfFile3`
    against `mmap`/`MAP_FIXED_NOREPLACE`/`memfd`);
  * thread context and TLS;
  * the code-cache file mapping;
  * the PE-specific pieces.
* **Moves each one behind an fx-internal seam**, `src/fx/host/` with a
  Win32 implementation. Use compile-time selection, with zero cost on hot
  paths.
* **Gate:** fx performance is unchanged. Measure the `ALCHEMY_PERF`
  start-up numbers and Blender headless CPU time before and after, and
  check the FX benchmark status in `docs/BENCHMARK_STATUS.md`.

If the seams can't be made zero-cost in this wave, document them precisely
in `docs/FX_HOST_SEAMS.md` and leave the code. Correctness and speed
come before the port.

## Current map (where each framework lives today)

| Framework | Places |
| --- | --- |
| libSystem / libc / Mach | `darwin_*.cpp`, `darwin_network.inc`, `darwin_process.inc`, `darwin_vm_space.cpp`, `darwin_mach_vm.cpp`, `cross_process_memory.cpp`, `shared_wait.cpp`, `machsys/`, `export_fill/libsystem_*.cpp`, `chromesupport/libsystem/`, `chromesupport/mach/`, `kit/completion/runtime/{system_c,process_io,threads}.cpp` |
| libdispatch | `darwin_dispatch.cpp`, `export_fill/dispatch_data.cpp`, `kit/completion/runtime/dispatch_objects.cpp` |
| CoreFoundation | `core_foundation.cpp`, `object_store.cpp`, `fwgaps/cf.cpp`, `export_fill/cf_*.cpp`, `kit/completion/runtime/cf_extras.cpp` |
| Foundation | `foundation_bridge.cpp`, `foundation_extra.cpp`, `fwgaps/foundation.cpp`, `export_fill/foundation_classes.cpp`, `kit/completion/{foundation,collections,formatters,json*,kvc,keyed_archive,…}.cpp`, `kit/completion/data/*`, `kit/completion/runtime/{kvo,undo,notification_queue,…}.cpp`, `kit/support/{kvo,mutable_string,notification_blocks,objc_assoc}.cpp`, `chromesupport/frameworks/foundation.cpp` |
| AppKit | `appkit/`, `kit/completion/controls/`, `kit/completion/windows/`, `kit/completion/graphics/appkit_drawing.cpp`, `kit/support/{appkit_extra,accessibility,pasteboard}.cpp`, `chromesupport/frameworks/{appkit,menus,windows,spelling,selectors}.cpp` |
| Window server / run loop | `windowserver/` (Win32 windows), `runloop/` |
| QuartzCore / CoreVideo / IOSurface | `quartzcore/`, `kit/support/{layer_contents,display_link,iosurface}.cpp`, `kit/completion/graphics/{animation,transform3d}.cpp` |
| CoreGraphics | `quartz2d/`, `kit/support/cg_extra.cpp`, `fwgaps/graphics.cpp` |
| Media | `media/`, `kit/completion/media/` (WASAPI, WinMM MIDI), `chromesupport/frameworks/media.cpp` |
| IOKit | `devices/`, `export_fill/iokit_registry.cpp` |
| SystemConfiguration / Network / CFNetwork | `sysconfig/`, `network/` (WinHTTP), `chromesupport/frameworks/network.cpp`, `chromesupport/libsystem/network_policy.cpp` |
| Security / CommonCrypto | `security/`, `export_fill/cc_cryptor.cpp`, `kit/completion/graphics/crypto_*.cpp` |
| Compression | `export_fill/zlib_*.cpp`, `kit/completion/data/{bzip2,compression*}.cpp`, plus bz2 streaming in both `chromesupport/libsystem/leftovers.cpp` and `chromesupport/frameworks/foundation.cpp` |
| Identities and misc | `darwin_frameworks.cpp`, `chromesupport/frameworks/identities.cpp`, `export_fill/frameworks.cpp`, `fwgaps/{accelerate,charset,system}.cpp`, `kit/completion/disk_arbitration.cpp`, `kit/support/{gl_stubs,qt_imports}.cpp` |

## Known duplicates and order traps

* **bzip2 streaming** (`BZ2_bzDecompress*`) is implemented twice: in
  `chromesupport/libsystem/leftovers.cpp` and
  `chromesupport/frameworks/foundation.cpp`. Keep one, over
  `kit/completion/data/bzip2.cpp`'s decoder.
* **KVO** is in both `kit/support/kvo.cpp` and `kit/completion/runtime/kvo.cpp`,
  and `network/operation_queue.cpp` observes as well. Keep one
  implementation.
* **NSSpellChecker** is a class shell in `fwgaps/foundation.cpp` and the
  real class in `chromesupport/frameworks/spelling.cpp`.
* **`gui/install.cpp` order:**
  * Mach installs before the fill-only set (`install_chrome_support`), and
    Chrome's frameworks after it (`install_chrome_strict`).
  * QLab support installs before the fill-only set, and completes classes
    that others declare.
  * With one owner per export, this ordering should become irrelevant.
    Check that it is.
* **Repeated registrations.** First write the registration audit: a debug
  mode that logs each export or selector registered more than once, with
  both owners. It is the to-do list for deduplication.

## Phases and groups

The integrator owns these throughout:
* the host API headers' common part;
* `gui/install.cpp`, the registration table and `cmake/` wiring;
* the registration audit;
* the `host/null` build check.

**Phase 1: move by framework.** Only moves and renames, so history
follows.
* Three groups, each using `git mv` in commits that only move:
  * **r1 system:** libSystem, Mach, dispatch, compression, identities.
    Includes splitting `darwin_services.cpp`.
  * **r2 foundation:** CoreFoundation, Foundation, Security, Network,
    SystemConfiguration.
  * **r3 ui:** AppKit, window server, run loop, QuartzCore, CoreGraphics,
    CoreText, media, IOKit, Metal/MSL.
* Deduplication and one-owner registration come after the moves, in
  separate commits.
* Land phase 1 (the regression check passes) before phase 2.

**Phase 2: the host boundary.**
* The integrator first commits the host API skeleton: headers,
  `host/win32/` and `host/null/` stubs, and CMake selection. Groups then
  extract their Win32 calls behind it, each owning the host areas it uses:
  * **r1:** `vm`, `sections`, `files`, `net`, `process`, `ipc`, `sync`,
    `time`.
  * **r2:** `http`, `crypto`, `locale`, `system` (adapters, power).
  * **r3:** `ui`, `audio`, `midi`, `gpu`.
  * **r4 fx:** fx host seams (`src/fx/host/`), with the performance gate.
* When an area has several callers, its owner extracts the call and the
  others use it.

## Done when

* **Layout:**
  * No `kit/completion/`, `kit/support/`, `chromesupport/`, `export_fill/`,
    `fwgaps/` or `machsys/` is left.
  * Every framework directory has an `install.cpp`, a `README.md` and
    `tests/`.
  * `docs/ALCHEMY_PROVIDERS.md` describes the pattern.
* **Registration:**
  * Each export and each selector has exactly one owner. The audit runs
    as a test and passes.
  * There are no duplicate implementations, and install order no longer
    changes behaviour.
* **Host boundary:**
  * Outside `src/alchemy/host/<backend>/`, `src/fx/host/` and the tests,
    no file includes OS headers (`windows.h`, `winsock2.h`, WinHTTP, COM,
    WASAPI, and so on).
  * A check (`scripts/check_host_boundary.ps1` or a CMake test) enforces
    it.
  * The macOS layer builds and links against `host/null`.
* **Behaviour unchanged:**
  * Every check in the wave rules' "Regression check before landing"
    gives the same result as `main` before the wave: ctest; Blender, QLab
    and the renderer add-on; Chrome strict `--dump-dom` with exit 0.
  * `doctor --deps` numbers for the QLab, the renderer add-on and Chrome trees are the
    same.
* **Build time:**
  * An incremental build after touching `process_context.hpp` (or its
    successor) is measured before and after.
  * Wide headers use forward declarations.
* **Linux readiness:**
  * `docs/ALCHEMY_HOST_LINUX.md` maps each host API area to the Linux
    calls a backend would use, and lists the fx seams with their Linux
    counterparts. For example, `ipc` maps to UNIX sockets with
    `SCM_RIGHTS`, `ui` to Wayland/X11, `audio` to PipeWire/ALSA, `http`
    to libcurl or the guest's own stack, `vm` to `mmap` with
    `MAP_FIXED_NOREPLACE`, and fx faults to `sigaction(SIGSEGV)`.
  * It notes what a Linux arm64 host could skip or simplify.

## Not in scope

* Implementing the Linux backend.
* New macOS APIs, or behaviour fixes found along the way. Report them for
  a later wave, unless they are duplicates that this wave must resolve
  anyway.

## Integrator's working rules (2026-10-17)

These rules refine the brief for the groups. They are binding for this wave.
Where they change the brief, the reason is given.

### Baseline (main 764b2df)

* **ctest:** `ctest -R "^alchemy_"`, cloaked, gives 151/151. Run ctest
  **without** `ALCHEMY_NETWORK`: `alchemy_gui_network` and
  `alchemy_gui_bonjour_fixture` need real loopback mDNS and fail under
  `loopback`. Guest app runs still use `ALCHEMY_NETWORK=loopback`.
* **Apps,** cloaked, with `ALCHEMY_NETWORK=loopback`:
  * Blender windowed: exit 0 ×3.
  * Blender headless: exit 0, 3.7–4.5 s CPU.
  * QLab: welcome window drawn.
  * Renderer add-on cube: renderer created, render 6.4 s, no license.
  * Chrome strict `--dump-dom`: exit 0 with `data-js="ran"` ×2.
  * The only denials are figure53, 10.0.4.24:30304 and
    `[2001:4860:4860::8888]:443`.
* **`doctor --deps --scan-bundle --json`,** unique imports:

  | Tree | Images | To providers (strong / weak) | Weak-null | Unresolved or unsupported |
  | --- | --- | --- | --- | --- |
  | QLab | 23 | 1,876 / 10 | 5 | 1 unsupported |
  | The renderer add-on's render server | 29 | 2,645 / 65 | 8 | 0 |
  | Chrome | 28 | 2,745 / 33 | 27 | 3 unresolved (bundle-scan artifacts) |

* **Build time:** touching `include/alchemy/process_context.hpp`
  recompiles 273 translation units of `alchemy-fx`. That takes 12.2 min at
  `/m:4` on an idle machine.
* **Registration audit census** (the `alchemy_gui_registration_audit`
  test): 11,651 export, 9,879 method and 719 class registrations.
  * 1,290 names are registered more than once: 432 `export`, 288
    `method` and 570 `republish`.
  * A real process (`ALCHEMY_REGISTRATION_AUDIT=1 alchemy-fx doctor`)
    also shows 2 `runtime` duplicates, `_printf` and `_puts`, which
    shadow DarwinRuntime's base table.
  * 115 fill-only registrations are skipped.
  * 1,820 fill-only registrations are spread over 50 files.

### Registration audit

* **How to run it:** `ALCHEMY_REGISTRATION_AUDIT=1 alchemy-fx doctor <any Mach-O>`,
  or the `alchemy_gui_registration_audit` test.
  * Both print every name registered more than once, with all its owners
    in registration order (`<installer> <file>:<line>`). The last owner
    wins.
  * `ALCHEMY_REGISTRATION_AUDIT=<file>.json` writes JSON instead.
* **Kinds:**
  * `export`: two implementations; the last one wins.
  * `republish`: the same data address published again, usually a class
    symbol. It is harmless, but the symbol still has two owners.
  * `method`: a native method replaced on the same class.
  * `class`: a class registered twice.
  * `alias`: registered under `/usr/lib/system/...`, which `resolve()`
    never looks up, so the registration is dead.
  * `fill-skipped`: a fill-only registration that lost to another owner.
    It is dead code or a second implementation.
* **Ratchets.** The test holds two ratchets, `max_duplicates` and
  `max_fill_skipped`, in
  `src/alchemy/gui/tests/registration_audit_tests.cpp`.
  * A commit that removes duplicates lowers them in the same commit. No
    commit raises them.
  * Both reach 0 by the end of the wave.

### Target layout and ownership

Everything goes under `src/alchemy/darwin/<framework>/`. Ownership follows
the **destination** framework: the group that owns a framework moves every
file that belongs there, wherever it lives today.

* **r1 system** owns `darwin/libsystem/` (subareas `vm/`, `mach/`, `net/`,
  `fs/`, `process/`, `malloc/`, `misc/`), `darwin/dispatch/`,
  `darwin/compression/` and `darwin/identities/`. That covers:
  * every top-level `darwin_*.cpp`/`.inc`, `cross_process_memory.cpp`,
    `shared_wait.cpp` and `machsys/`;
  * `export_fill/{libsystem_*,dispatch_data,zlib_*,frameworks}.*`;
  * `chromesupport/{libsystem,mach,memory}/` and
    `chromesupport/frameworks/identities.cpp`;
  * `kit/completion/runtime/{system_c,process_io,threads,dispatch_objects,xpc,log_store,oslog_store}.*`,
    `kit/completion/{oslog,disk_arbitration}.cpp` and
    `kit/completion/data/{bzip2,compression,compression_provider,xar}.*`;
  * `fwgaps/{accelerate,charset,system}.cpp` and
    `kit/support/{gl_stubs,qt_imports}.cpp`;
  * `frameworks/installer_frameworks.cpp`, and the Dispatch and OSLog
    Swift overlays;
  * splitting `darwin_services.cpp`.
  * `darwin_frameworks.cpp` goes to `identities/` whole. Its AppKit,
    Metal, QuartzCore and CoreGraphics headless shells move to r3 in the
    deduplication step.
* **r2 foundation** owns `darwin/corefoundation/`, `darwin/foundation/`,
  `darwin/coreservices/`, `darwin/security/` (with GSS),
  `darwin/network/` (CFNetwork, URL loading, Bonjour) and
  `darwin/systemconfiguration/`. That covers:
  * `core_foundation.cpp`, `object_store.cpp`, `foundation_bridge.cpp`
    and `foundation_extra.cpp`;
  * `fwgaps/{cf,foundation}.cpp`;
  * `export_fill/{cf_*,foundation_classes,coreservices,cc_cryptor,md4}.*`;
  * every `kit/completion/` file not named for r1 or r3, including `data/*`,
    `runtime/*`, `json*`, `kvc`, `keyed_archive`, formatters, dates and
    numbers;
  * `kit/support/{kvo,mutable_string,notification_blocks,objc_assoc}.cpp`;
  * `chromesupport/frameworks/{foundation,network,services}.cpp`;
  * `kit/completion/graphics/crypto_*` and the CryptoKit Swift overlay;
  * `security/`, `gss/`, `network/` and `sysconfig/`, with
    `network/sysconfig_store.cpp` going to `systemconfiguration/`.
* **r3 ui** owns these destinations:
  * `darwin/appkit/`, `darwin/windowserver/` and `darwin/runloop/`;
  * `darwin/quartzcore/`, with CoreVideo, IOSurface and CoreImage;
  * `darwin/coregraphics/`, with ImageIO;
  * `darwin/coretext/`;
  * `darwin/media/`, with the AVFoundation, CoreAudio, CoreMedia and
    CoreMIDI Swift overlays;
  * `darwin/iokit/`;
  * `darwin/metal/`, with MetalKit;
  * `darwin/msl/` and `darwin/printing/`.

  That covers:
  * `appkit/`, `windowserver/`, `runloop/`, `quartzcore/`, `quartz2d/`,
    `coretext/`, `imageio/`, `media/`, `devices/`, `printing/`, `metal/`
    and `msl/`;
  * `kit/completion/{controls,windows,media}/` and the rest of
    `kit/completion/graphics/`;
  * `kit/completion/{animator,audio,media_timing}.cpp`;
  * `kit/support/{appkit_extra,accessibility,pasteboard,layer_contents,display_link,iosurface,cg_extra,selector_audit,wait_trace}.cpp`;
  * `fwgaps/graphics.cpp` and `export_fill/iokit_registry.cpp`;
  * `chromesupport/frameworks/{appkit,menus,windows,spelling,selectors,media}.cpp`.
* **Integrator:**
  * `chromesupport/libsystem/network_policy.cpp` becomes the host API's
    common part in phase 2. Until then r1 moves it with libSystem.
* **Stays put:** the loader and runtime core stays where it is:
  `macho`, `module_loader`, `process_context`, `objc_runtime`,
  `objc_metadata`, `bundle`, `runtime_libraries`, `inspection`,
  `process`, `main`, `perf`, `registration_audit`, `install/`, `fx/`,
  `objc/` and `gui/`.
* **Unlisted files:** the destination framework decides who moves them.
  If that is unclear, ask the integrator first.

### Moving files

* **Pure moves first.** A move commit contains only `git mv`, with the
  subject `move: ...`.
* **Fixups next.** The commit right after it fixes the build: CMake
  source lists, `#include` lines and test registration, with the subject
  `fixup: ...`. The pair builds.
* **Includes.** Includes are src-rooted (`"alchemy/<path>"`) since the
  integrator's first commit, so moving a `.cpp` needs no include edits.
  When you move a header, rewrite `"alchemy/<old>"` to `"alchemy/<new>"`
  repository-wide in the fixup commit, even in other groups' files.
  Nobody else edits that line, so merges stay clean.
* **`include/alchemy/` stays in place in this wave.** Moving its public
  headers would rewrite almost every file. A header may move into its
  framework directory only when its framework alone includes it.
* **Shared helper kits stay in place during the moves.** They are
  included or called across groups, so one owner moving them would race
  the others:
  * `kit/completion/{completion kit.hpp,install.cpp}`,
    `kit/completion/data/{providers,text}.hpp`,
    `kit/completion/graphics/providers.hpp` and
    `kit/completion/runtime/common.*`;
  * `export_fill/{provider_util,export-fill}.hpp`;
  * `kit/support/{support kit.hpp,install.cpp}`;
  * `chromesupport/{chromesupport.hpp,install.cpp}` and
    `chromesupport/frameworks/{frameworks.hpp,install.cpp}`;
  * `fwgaps/{fwgaps.hpp,install.cpp}`;
  * every subgroup `install.cpp` (aggregator).

  They dissolve in the registration step. The integrator moves or deletes
  what is left.
* **Tests move with their framework** into
  `src/alchemy/darwin/<framework>/tests/`. That includes the headless
  `tests/alchemy_<x>_tests.cpp` of frameworks you own, if you want them
  there. Keep the ctest names.

### CMake

* **Framework fragments.** Each framework gets
  `cmake/darwin/<framework>.cmake`, picked up automatically by
  `cmake/AlchemyDarwin.cmake`. A fragment does three things:
  * adds its headless sources with
    `target_sources(alchemy_core PRIVATE ...)`;
  * appends its GUI sources to `ALCHEMY_GUI_SOURCES`;
  * registers its tests.
* **Old lists.** When you move a file, remove its line from the old list
  (`cmake/Alchemy.cmake`, `cmake/chrome_libsystem.cmake` or
  `cmake/gui/*.cmake`). Delete an old `cmake/gui/` fragment once it is
  empty, and remove its name from the list in `AlchemyGui.cmake`.
  * These files are shared, so expect small merge conflicts. The
    integrator resolves them.

### Registration (after the moves)

* **Commits.** Deduplication and one-owner registration go in their own
  commits (`dedup: ...`, `register: ...`), never mixed with moves.
* **One `install.cpp` per framework.** Each framework directory gets an
  `install.cpp` with one entry point that calls the per-file installers
  now living there:
  * `install_<framework>(gui::InstallContext&)` for GUI frameworks;
  * a core function, called where `process.cpp` and `inspection.cpp`
    install the core today, for headless ones.
* **The installer table** in `src/alchemy/gui/install.cpp` is the
  integrator's, but groups may edit its rows.
  * Replace an old aggregator call with the framework's rows, in the
    aggregator's position.
  * Keep the relative order until the audit shows no remaining overlap
    for that framework. Then the position no longer matters; say so in
    the commit.
* **One owner per name.** For each duplicate that the audit lists, keep
  one implementation in the owning framework, the real one over the
  stub, and delete the other registration.
  * If the loser had behaviour that the winner lacks, merge it into the
    winner, with a test that fails on the old code.
* **Before and after each dedup commit,** run the audit. Only the names
  the commit handles may change owner.

### Builds and runs

* **Two builds at once, at `/m:2`.** At most two groups build at the same
  time.
  * Before a build that compiles more than about 20 files, create
    `build/<group>.lock` in the shared notes folder, containing your
    start time. If two other locks exist, wait.
  * Delete your lock when the build ends.
* **Build `alchemy-fx` and your test targets explicitly.**
* **App runs** follow the wave rules: cloaked, `loopback`, scratch roots.
  Groups need the full regression check only for their final report;
  while moving files, ctest is enough.

### Cadence

Phase 1 runs in two steps.

1. **Moves.** Each group reports when its moves and fixups build and
   ctest passes. The integrator merges all three into `integ/refactor`
   and tells the groups to merge it.
2. **Deduplication and registration,** on the merged layout. Each group
   reports again, with the audit numbers before and after.

The integrator then runs the full regression check before phase 2.

## Phase 2 working rules (integrator, 2026-10-18)

The host API skeleton is on integ/refactor:

* `include/alchemy/host/host.hpp` has the rules and the common types:
  * `host::Error` names POSIX error kinds, not their values;
  * `Result<T>`, `Status`, `Handle` and `OwnedHandle`.
* Each area has one header, which names its owning group and starts
  with a few calls.
* `src/alchemy/host/{common,win32,null}/` holds the shared code and the
  two backends. `<backend>/gui/*.cpp` is for fx-only areas.
* `cmake/AlchemyHost.cmake` selects the backend with `ALCHEMY_HOST`.
* `alchemy_host_tests` tests both backends.
* `scripts/check_host_boundary.ps1` (ctest `alchemy_host_boundary`)
  enforces the boundary, with `scripts/host_boundary_allow.txt` as the
  to-do list.

### How a group extracts its OS calls

* **Callers.** Replace the Win32 calls in your provider files with
  `host::<area>::` calls. Translate `host::Error` into the Darwin result at
  the call site, never in the backend.
  * libSystem gets one shared translation, `libsystem::darwin_errno(host::Error)`.
    r1 writes it first, and everyone uses it.
* **Backends.** A new host call goes in the area header, `win32/<area>.cpp`
  and `null/<area>.cpp` in the same commit, with a case in
  `src/alchemy/host/tests/host_tests.cpp`.
  * The Win32 helpers stay inside the backend: UTF-8 and UTF-16
    conversion, `win32::error`, COM, dynamic loading.
* **Allow-list.** When a file stops including OS headers, remove it from
  `host_boundary_allow.txt` in the same commit; the check fails until you
  do. Never add a file to the list.
* **Shared areas.** When an area has callers in several groups, its owner
  writes the call and the other groups use it. Ask the owner, using the
  shared notes, rather than adding a private copy.
* **Hot paths.** A host call is one more direct call, which is fine.
  Allocation, the read and write paths, and time are already on hot paths,
  so keep them free functions with no extra allocation. Measure with the
  ctest timings and Blender headless if in doubt.
* **The window server's Win32 half** (HWNDs, messages, DWM, clipboard,
  OLE drag and drop, IMM) moves into `src/alchemy/host/win32/gui/`
  behind `host::ui`. The window server's Darwin semantics stay in
  `darwin/windowserver/`. The same goes for D2D, DirectWrite and WIC
  under `host::gpu`, WASAPI under `host::audio` and WinMM under
  `host::midi`.
* **Network policy.** The integrator moves the policy (`ALCHEMY_NETWORK`)
  into `src/alchemy/host/common/`, with a host-neutral address type, once
  `host::net` has one (r1).

### Done for phase 2

* `host_boundary_allow.txt` lists no file under `src/alchemy/` or
  `include/alchemy/`. The `src/fx` entries are only those
  `docs/FX_HOST_SEAMS.md` documents.
* The integrator's `ALCHEMY_HOST=null` build of `alchemy-fx` compiles and
  links. In that build the OS headers are poisoned for `alchemy_core` and
  `alchemy_core_fx`.
* Behaviour is unchanged: the wave rules' regression check passes against
  the baseline.

## Wave result (2026-10-18)

The wave is done on `integ/refactor` (269 commits over main 764b2df).
`main` is not fast-forwarded yet; that waits for the user's OK. The group
reports are in the shared notes (`r1-final.md`, `r2-final.md`,
`r3-final.md` and r4's `docs/FX_HOST_SEAMS.md`).

### Layout

* No `kit/completion/`, `kit/support/`, `chromesupport/`, `export_fill/`, `fwgaps/`
  or `machsys/` is left.
  * Providers live in 22 framework directories under
    `src/alchemy/darwin/`.
  * The helper kits several frameworks share, with their ordered install
    groups, are in `src/alchemy/darwin/kit/` (see its README).
* Every framework has `install.cpp`, `README.md` and `tests/`. The
  exceptions are `msl/`, a compiler library with no installer row, and
  `swift/`, which holds guest overlays.
* `docs/ALCHEMY_PROVIDERS.md` describes the pattern and maps the old paths
  to the new.

### Registration

* The registration audit (`ALCHEMY_REGISTRATION_AUDIT`, test
  `alchemy_gui_registration_audit`) went from 1,290 duplicates and 115
  skipped fills to **0 and 0**. Its ratchets are at 0.
* The brief's known duplicates have one implementation each:
  * bz2 streaming is in `compression/bzip2_stream.cpp`;
  * KVO is in `foundation/kvo.cpp`;
  * NSSpellChecker is in AppKit.
* Names that two frameworks touch follow one of two patterns, both in
  ALCHEMY_PROVIDERS.md:
  * a plain override is a `<x>_headless.cpp` plus `<x>_fx.cpp` pair
    (`ALCHEMY_HEADLESS_ONLY_SOURCES`);
  * a decorator is an extension point in the owning framework
    (`CoreFoundation::extend`).
* **Not verified by experiment:** that install order no longer changes
  behaviour. Each name has one owner, so no registration depends on order,
  but the installer table was not shuffled to test it.
  * The table keeps its order, because some installers use other
    frameworks while they install (AppKit uses the run loop and the window
    server).
* `DarwinProviders` (`include/alchemy/darwin_providers.hpp`) is now the
  one ordered install of the macOS layer, shared by `run_program`, doctor
  and the provider tests. The tests had a drifted copy before.

### Host boundary

* `include/alchemy/host/` has 19 area headers (vm, sections, files, net,
  dns_sd, http, process, ipc, sync, time, locale, crypto, system, ui, audio,
  midi, gpu, debug, network_policy) and `host.hpp`.
  * Backends: `src/alchemy/host/win32/` (with `gui/` for the fx-only
    areas) and `src/alchemy/host/null/`.
  * `ALCHEMY_HOST` selects one, with no virtual calls.
  * The network policy is in `src/alchemy/host/common/`, OS-free.
* No source in the macOS layer or the loader includes an OS header,
  directly or through a header. The count was 141 files at the start of
  phase 2.
  * `scripts/check_host_boundary.ps1` (ctest `alchemy_host_boundary`)
    enforces it.
  * Its allow-list holds only the 9 fx PE-core files, documented in
    `docs/FX_HOST_SEAMS.md`.
* **The null-backend build passes.** `ALCHEMY_HOST=null` builds and links
  `alchemy-fx`, `alchemy` and `alchemy_host_tests` with 56 OS headers
  poisoned (`#error`) for `alchemy_core` and `alchemy_core_fx`.
* fx (r4): the engine's host dependencies are behind `src/fx/host/` with a
  win32 backend. fx files with OS includes outside the seams went from 29
  to 9 (PE loading, the ABI overrides and the SEH layouts), with each
  remaining seam's Linux counterpart documented.

### Behaviour

Final run on `integ/refactor` 6417ee6, interleaved with the 764b2df binary,
cloaked, with `ALCHEMY_NETWORK=loopback`, on a quiet machine:

| Check | main 764b2df | integ/refactor |
| --- | --- | --- |
| `ctest -R "^alchemy_"` | 151/151 | 175/176 (+25 new tests). `alchemy_gui_windowserver_win32` failed once on focus under `-j 8` and passes 3/3 alone (environmental, as in the wave rules) |
| Blender windowed, first 3D viewport draw | exit 0 ×3; 2.86 / 2.92 / 3.95 s | exit 0 ×3; 2.88 / 2.88 / 2.99 s |
| Blender headless (`-b --factory-startup` + monkey) CPU | 2.5–2.6 s | 2.3–2.7 s |
| QLab | welcome window ×2 | welcome window ×2 |
| Renderer add-on headless cube | renderer created, render 6.5–6.6 s, no license (loopback) | renderer created, render 6.4 s, no license (loopback) |
| Chrome strict `--dump-dom` | exit 0, `data-js="ran"` ×2 | exit 0, `data-js="ran"` ×2 |
| `network denied` lines | figure53; 10.0.4.24:30304; `[2001:4860:4860::8888]:443` | identical |
| `doctor --deps --scan-bundle` (QLab, the renderer add-on, Chrome) | baseline table above | identical in every count |

The `alchemy_process_tree_fx` "rebased" case was intermittent under
`-j 8` ("requested segment address is unavailable"). It was also seen on a
pre-wave binary, and it passes alone.

### Build time

* Touching `include/alchemy/process_context.hpp` rebuilds 300 TUs of
  `alchemy-fx` in **8.2 min** at `/m:4` on an idle machine. The baseline
  was 273 TUs in 12.2 min.
* There are more TUs because the old monoliths are now split, but the
  rebuild is faster.
* "Wide headers use forward declarations" was not worked on specifically;
  it is left for a later wave.

### Behaviour changes, all deliberate and tested

* `getservbyname`/`getservbyport` write `s_port` zero-extended; it used to
  be sign-extended for ports ≥ 32768.
* `inet_addr` uses its own `inet_aton` parser: decimal, octal and hex, with
  1–4 parts.
* `time::sleep_ns` sleeps precisely, using a high-resolution timer. It used
  to have a granularity of 15.6 ms.
* errno values are kept per call site through three translation tables
  (file, CRT, socket).
  * A few rare Win32 codes now map differently, for example
    `ERROR_BAD_NETPATH` at a file site. They are listed in
    `r1-errno-fidelity.md`.
* An HTTP exchange read before send gives `NSURLErrorUnknown`, as before;
  this was restored and pinned.
* NSURL volume capacity keys now also need the volume-information query to
  succeed.
* Error texts of `alchemy install` name the host error kind instead of the
  Win32 code.
* An invalid UTF-16 environment entry now gets replacement characters
  instead of stopping startup. Invalid arguments are still rejected.

### Incidents

* Group builds ran `alchemy_llvm_runtimes`, `alchemy_expat_runtime` and
  `alchemy_swift_runtime`, which write into the shared, read-only runtime
  artifacts.
  * The expat and Swift deliverables are byte-identical.
  * In the LLVM runtime, `libc++abi.dylib` and `libclang_rt.alchemy.dylib`
    now embed a worktree path in their assertion `__FILE__` strings.
    Behaviour is unchanged unless an assert prints.
  * Rebuilding from the main checkout would restore them; that is the
    user's call.
  * A rule now bars groups from building those targets.

### Later waves

* Merge the five kits, `gui::fill::Provider` and `security::Bridge` into
  one provider helper, and give each framework its own installer row.
* Restore the CGDisplay* implementations removed in 79b5cef as
  alchemy-fx's owner, with a headless pair. This is a behaviour change:
  displays become visible.
* Forward declarations in wide headers.
* The `alchemy_process_tree_fx` rebased flake: reserve the load range
  first thing in the child (fx startup).
* `TRANSMUTE_FX_SAMPLE` plus `TRANSMUTE_FX_CORESTATS` can hang Blender
  headless. This is pre-existing (r4).
* AppKit's run-loop mode constants overwrite the interned mode objects,
  as before the wave (r3).
* The Swift runtime was not rebuilt after the overlays moved
  (`scripts/build_alchemy_swift.ps1` names the new paths).
* A Linux backend: `docs/ALCHEMY_HOST_LINUX.md` maps every area, and the
  null build is the starting point.
