# Alchemy on fx, and macOS Blender headless (2026-10-02)

Alchemy normally runs on the checked-memory reference engine (`a64.cpp` plus the
old JIT). This note records Alchemy on the fx engine (`alchemy-fx.exe`), the
state of the official macOS ARM64 Blender 4.5.14 in background mode on x64
Windows, and what remains.

## Status

The official macOS ARM64 Blender 4.5.14 renders headless with Cycles CPU on
`alchemy-fx.exe` in strict mode (`ALCHEMY_PROBE_MISSING` unset). Final
verification, fresh `build-wf-verify` build, `scripts/alchemy_blender_matrix.ps1`:

| Stage | Probe off, 1 thread | Probe zero | Result |
| --- | --- | --- | --- |
| `--version` | 9.4 s | 27.7 s | exit 0 |
| `-b --factory-startup -noaudio` | 76.8 s | 103.8 s | exit 0 |
| `--python-expr` (bpy) | 39.5 s | 47.8 s | exit 0, `ALCHEMY BPY 4.5.14 LTS 3 darwin` |
| Cycles CPU render, factory Cube 16x16, 1 sample | 77.2 s (4 threads: 72.2 s) | 77.3 s | exit 0, PNG written |

- Every stage exits 0 with 0 probe calls and an empty stderr, in both modes.
- The PNG (16x16 RGBA, 23 distinct colours, shaded cube on grey) is
  pixel-identical to the same scene rendered by native x64 Windows Blender
  4.5.14 (`artifacts/x64/blender-4.5.14-windows-x64`, 1.3 s). Only metadata
  bytes differ.
- Blender's own report: render time 8.9 s at 1 thread and 3.75 s at 4 threads.
  Saving takes most of it (6.0 s and 2.7 s).
- The machine was shared with other builds, so wall times vary by up to 2x. An
  unloaded `-b --factory-startup` took 34 s.
- Logs: `artifacts/alchemy-blender/runs-final-{off,off-t4,zero,native}`, plus
  `runs-final-{python-fx,python-ref,ctest}.txt`.

Regressions on the same build:

- Python acceptance suite on alchemy-fx: 48/48, both bases.
- Python acceptance suite on reference `alchemy.exe --jit`: 24/24, default base.
- `ctest -R "^alchemy_" -E alchemy_python`: 40/40.
- The `alchemy_python_{services,support,loader}` unit groups: 3/3.

Without `-b`, Blender takes the GUI path. It stops explicitly at
`+[NSApplication sharedApplication]` (unsupported message), as expected for
headless-only AppKit classes. `scripts/alchemy_blender_gui.ps1` reproduces
this; see "Interactive Blender" below.

## alchemy-fx

`alchemy-fx.exe` compiles Alchemy's unchanged sources against
`transmute::FxEngine` (`src/alchemy/fx/`, `cmake/AlchemyFx.cmake`, forced
prelude `alchemy_fx_prelude.hpp`). Build it with:

```powershell
cmake -S . -B build-alchemy-fx -G "Visual Studio 17 2022" -A x64 -DTRANSMUTE_WITH_FX=ON
cmake --build build-alchemy-fx --config Release --target alchemy-fx
```

- **Memory.** Guest memory is host memory at the guest address. Optional
  capabilities, detected at compile time through `include/alchemy/engine_adapter.hpp`:
  partial `mprotect` and reserve-then-commit `mmap`.
  * The main executable's load range (`--load-base`, else `0x1'0000'0000`)
    is reserved first thing in `wmain` (`FxEngine::hold_load_range`). Host
    thread stacks and heap segments are placed bottom-up from a random base
    and could otherwise take it before the executable maps.
    * Every part of the range that is still free is reserved. Allocations
      made before `main` may already sit in it.
    * The first guest placement inside the range takes it over.
  * Two diagnostics for the `alchemy_process_tree_fx_rebased` test:
    * `ALCHEMY_FX_TEST_TAKE_LOAD_RANGE=<address>` makes a host reservation
      there when the engine starts.
    * `ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD=<address>` makes one there before
      the hold.
- **Calls.** Synthetic calls are `svc #0x7a1c` slots at the reference
  addresses. By default fx enters them as native host callbacks
  (`fx::register_host_callback`), without a dispatcher exit;
  `ALCHEMY_FX_SVC_CALLS=1` restores the SVC exit path for A/B measurement. `run`, `invoke_callback` and workers use `fx::call_guest_prepared`.
- **Instruction families.** FxEngine now registers fx's FP, NEON and system
  families (`ALCHEMY_FX_FAMILIES` overrides this for measurement). Before, every
  FP, NEON, atomic or system instruction was interpreted. At the time the
  interpreter's LSE atomics were not atomic across threads (since fixed in fx).
  `fx_frint_probe.s` checks FRINT32X/Z, FRINT64X, FMLAL/FMLAL2/FMLSL
  bit-exactly.
- **Stop and exit.** `request_stop` interrupts every guest thread. Each one
  unwinds at its next safe point. The Darwin exit syscall and a return from
  main run atexit handlers and then stop the workers. A 5 s watchdog ends the
  process if a worker stays blocked in a host call.
- **Fatal signals.** Guest faults reach fx's public last-chance hook
  (`fx::set_guest_fault_hook`) and map to SIGSEGV, SIGBUS, SIGILL, SIGTRAP or SIGFPE. The process exits with
  status 128 + signal and prints a module+offset backtrace.
- **Cost.** `ALCHEMY_FX_STATS=1` (and `ALCHEMY_PERF=1`) prints runtime
  counters and the translation-cache statistics. The translation cache is on
  for Mach-O images: `FxEngine::register_image` registers each image with
  `fx::register_external_module`, keyed by its LC_UUID (`ALCHEMY_FX_CACHE=0`
  disables it). A warm GUI start of Blender looks up 35,954 regions and hits
  35,948 (55 modules, 48 with cache files under
  `%LOCALAPPDATA%\transmute\fx-cache`, about 80 MB for the main binary);
  it translates 6 regions instead of 29,436 (2.76M guest instructions, 1.5 s)
  cold. See `docs/ALCHEMY_PERF.md`. The table below predates native host
  callbacks, which removed the per-call svc exits.

  | Stage | Regions | Guest instructions | Translate | svc exits |
  | --- | --- | --- | --- | --- |
  | `--version` | 11048 | 0.88M | 1.2 s | 202K |
  | factory | 22472 | 1.78M | 3.6 s | 2.8M |

  Nothing is interpreted. The 14M-instruction eager region formation seen
  earlier is gone.
- **ALCHEMY_PROBE_MISSING=zero|trap.** A diagnostic only. Missing Apple
  libraries load as empty providers, unresolved functions log and return 0 (or
  trap), and missing classes become empty `NSObject` subclasses. Protocol
  metadata is no longer bypassed: it always loads for real. Blender no longer
  needs this mode.

## What changed for Blender

| Area | Change |
| --- | --- |
| Loader | `module_loader.cpp` `provider_symbol` resolves libSystem / `/usr/lib/system/*` names from the resident guest `/usr/lib/libunwind.dylib` first (`__Unwind_Resume`, `__register_frame`, ...), then the providers. Classic `SET_ADDEND_SLEB` addends use 64-bit arithmetic (libOpenEXR RTTI) |
| Objective-C | Real protocols: `__objc_protolist`/`__objc_protorefs`, all `protocol_t` fields, canonicalization by name across images, class/category protocol lists, rollback. `objc_enumerationMutation` calls the installed handler or fails explicitly |
| CoreFoundation / Foundation | All 209 CF, 40 Foundation and 54 libobjc imports of Blender's load graph resolve. `CFLocaleCopyCurrent` builds a real CFLocale bridged to NSLocale from the host locale. `NSSearchPathForDirectoriesInDomains`, CF-object bridge classes, constant literal classes |
| libSystem | Blocks runtime, dispatch queues, `setjmp`/`longjmp`, ucontext, regex, glob and others. `bzero`/`bcopy` keep x0, because Apple clang tail-calls `bzero` for `return memset(p, 0, n)`, which broke `BLI_mempool_calloc`. Path-valued environment (TMP, TEMP, TMPDIR, HOME) becomes `/Volumes/C/...`, with TMPDIR supplied if absent. `gettimeofday` fills timezone. `exp10`/`exp10f`. Blocked workers stop cleanly at exit |
| Frameworks (`darwin_frameworks.cpp`) | Headless providers for every Apple framework and `/usr/lib` identity in the graph. Doctor reports 0 missing identities. Metal has no device. OpenAL 1.1 has no device. CoreGraphics has zero displays. IOKit, CoreVideo, AudioUnit and CoreServices return real failure codes. iconv. AppKit, Metal and QuartzCore classes. `LSSharedFileListCreate`/`CopySnapshot` return a list object and an empty +1 CFArray (Blender's `fsmenu_read_system` does not NULL-check) |
| Mach VM | `vm_remap` of an immutable source is an exact copy with the source protection (libffi trampolines) |

## Remaining work (Alchemy)

Nothing blocks the headless path. Open items, each failing explicitly today:

- **abort.** `abort()` and `___stack_chk_fail` throw `runtime_error` and exit 1.
  They should call `alchemy::terminate_by_signal(6, ...)` and exit 134 with a
  backtrace. The same applies to raise, kill and pthread_kill of the process
  itself with SIG_DFL.
- **Signal handlers.** Faults and abort are not delivered to guest `sigaction`
  handlers (Blender's crash handler, Python `faulthandler`). This needs
  libSystem `signal_actions` plus an engine hook that runs a guest handler on
  the faulting thread.
- **Dispatch.** Queues run on pooled guest workers with one timer thread;
  groups, barriers, `dispatch_apply`, suspension and timer/data sources are
  implemented. Missing: queue hierarchies (`set_target_queue` to a non-global
  queue), inactive attributes, finalizers, dispatch blocks/data/IO and
  file-descriptor, signal, process, vnode and Mach sources (each fails
  explicitly). Main-queue work runs through `dispatch_main`,
  `_dispatch_main_queue_callback_4CF` or, in alchemy-fx GUI processes, every
  common-mode step of the main run loop.
- **Objective-C.** Missing: weak references, exceptions, `objc_retainBlock`,
  Swift stub categories (`__objc_catlist2`) and runtime protocol creation.
  `objc_retain`/`objc_release` on blocks or dispatch objects should route to
  `_Block_copy` or `dispatch_retain`.
- **CF / Foundation.** Missing: CFError/NSError objects, non-file NSURL,
  NSMutableString, binary plists, block notification observers, NSRunLoop
  timers and ports (alchemy-fx GUI builds provide CFRunLoop, NSRunLoop and
  NSTimer, `src/alchemy/runloop`; ports remain missing). MacRoman and Windows-1252 conversions accept ASCII only.
- **OS version profile.** Resolved: every interface answers macOS 14.0 /
  Darwin 23.0.0 (`include/alchemy/darwin_version.hpp`): sysctl
  `kern.osrelease`/`kern.osproductversion`/`kern.osversion`, `uname`,
  `NSProcessInfo operatingSystemVersion(String)`, `__isPlatformVersionAtLeast`
  and `_availability_version_check`. Blender's `@available` checks gate its
  Metal backend on this.
- **Reference engine.** `invoke_callback` requires sp inside the engine's own
  stack. Synthetic calls that call back into the guest from a `makecontext`
  stack (qsort, scandir) fail there. fx has no such limit.
- **`sys_icache_invalidate`.** Resolved: it reaches `FxEngine::invalidate_code`
  (`invalidate_engine_code`), which calls `fx::flush_guest_code`: the
  translations whose guest bytes overlap the range die, their neighbours on
  the same page survive and stay write-protected. Stores through mappings fx
  knows still fault (SMC) and invalidate the whole page; the flush covers
  writes fx cannot see (a file written under a `MAP_SHARED` view, another
  process). `ALCHEMY_FX_STATS=1` prints an `alchemy-fx invalidation:` line
  (SMC faults and their time, flushes, re-translations, code ranges).
- **Guest heap and `query_region`.** Guest malloc blocks come from the fx
  engine heap (`FxEngine::heap_allocate`), which has no region record.
  `query_region` reports the heap's reservations as read/write guest memory;
  before that, every validity check on a heap pointer failed (Embree's
  `pthread_mutex_init` returned EINVAL and Blender aborted at load).
- **Interactive Blender.** The macOS binary is Metal-only: it contains no
  `GLBackend` or `VKBackend`, so there is no `--gpu-backend opengl` route and
  no NSOpenGL/CGL to map to WGL. The plan is Wine-style: project-authored
  AppKit, QuartzCore and CoreGraphics providers over a Win32 window server,
  and Metal translated to host Vulkan (`vulkan-1.dll`, loaded at run time),
  with MSL compiled by the host libclang front end plus an in-house AST to
  SPIR-V lowering. It is fx-only (`ALCHEMY_WITH_GUI` in `alchemy_core_fx`);
  `alchemy.exe` gains no dependency.
  - Scaffolding: `cmake/AlchemyGui.cmake` and one fragment per subsystem in
    `cmake/gui/`; registration in `include/alchemy/gui/install.hpp`; contracts
    `include/alchemy/gui/window_server.hpp`, `include/alchemy/gui/runloop.hpp`,
    `include/alchemy/msl/compiler.hpp`, the host->guest send API in
    `include/alchemy/objc_runtime.hpp` and `FxEngine::invoke_callback_regs`
    (`include/alchemy/call_regs.hpp`).
  - Diagnostics: `ALCHEMY_TRACE_OBJC=<class prefixes>` logs each message as
    `[objc] -[Class sel] from <module>+0x<offset>`; `ALCHEMY_MSL_DUMP=<dir>`
    collects every `newLibraryWithSource:` source.
  - Probe: `scripts/alchemy_blender_gui.ps1` launches `Blender.app
    --factory-startup` on `build-wf-gui`, captures any window with
    PrintWindow and writes `artifacts/alchemy-blender/gui/<timestamp>/`.

## Requests to the Transmute/fx owner

1. **Section-backed `vm_remap`** for writable sources (`MapViewOfFile3` into
   placeholders). Immutable sources are handled in Alchemy.
2. **Profiler measurement window (perf, 2026-10-04).** `TRANSMUTE_FX_SAMPLE`
   samples from `runtime_initialize` to exit, so a profile of an interactive
   phase (a 60 s viewport orbit) also contains the 5-20 s startup, and a
   killed process can leave the report empty (it is rewritten every 2 s by
   truncating the file). Requested: a start delay or a public
   `fx::profiler_reset()` / `fx::profiler_set_enabled(bool)` (Alchemy would
   bind them to `ALCHEMY_PERF` hot keys or a period), and writing the report
   to a temporary file renamed over the old one. Data:
   `artifacts/alchemy-blender/perf-w1/orbit-sample4-profile.txt.keep`
   (50,864 main-thread samples; the startup share had to be subtracted by
   hand, `docs/ALCHEMY_PERF.md`).
3. **Background translation of Mach-O images (low priority).** Cold
   (`ALCHEMY_FX_CACHE=0`) Blender translates 29,436 regions (2.76M guest
   instructions) in 1.5 s on the critical path, against about 10 s of cold MSL
   compiles; `aot_translate_image` takes PE files only. A background AOT of a
   registered external module (from its file and the module's base) would
   take this off the first start too.
4. **Done since the audit:**
   - Mach-O translation-cache keying: Alchemy registers every image with
     `register_external_module` (LC_UUID identity); warm starts hit 35,948 of
     35,954 lookups (`docs/ALCHEMY_PERF.md`).
   - FRINT32X/Z, FRINT64X/Z and FMLAL/FMLSL translation.
   - Atomic LSE, CAS/CASP and exclusives in the interpreter.
   - The public fault hook `set_guest_fault_hook`, which Alchemy now uses.
   - Thread interruption (`request_interrupt`).
   - Native callback entries (`fx::register_host_callback`), adopted by
     Alchemy for every synthetic call (default on).
   - Lazy region formation: 0.88M instead of 14M guest instructions for
     `--version`.

## Working method

- `alchemy-fx doctor --deps --json` lists every unresolved import by library up
  front (`artifacts/alchemy-blender/unresolved.txt` is the original list).
- `scripts/alchemy_blender_matrix.ps1 -Probe off|zero|trap -Stages ... -Threads N -OutDir ...`
  runs the stage matrix. Call it from PowerShell with an array for `-Stages`.
  `powershell -File` passes one string, and Blender then starts without `-b`.
- Symbolize Blender frames with `llvm-symbolizer --obj=.../MacOS/Blender 0x100xxxxxx`.
  The main binary loads at `0x100000000`.
- `scripts/alchemy_python_suite.ps1` runs the Python acceptance suite on either
  engine.
- `scripts/fx_alchemy_cold_start.ps1 -Exe <alchemy-fx> -Mode fresh|off|warm -Repeat 3`
  times a headless start (`-b --factory-startup --python-expr`): seconds to the
  startup script and to exit, fx translation counters and the machine's CPU
  load during the run. Opens no window.

## fx requests delivered (branch fx/alchemy-requests)

- **Profiler window.** `TRANSMUTE_FX_SAMPLE=<file>,...,delay=<s>,duration=<s>,paused`
  and `fx::profiler_reset()` / `fx::profiler_set_enabled(bool)`; reports are
  written to a temporary file and renamed over the old one. alchemy-fx:
  `ALCHEMY_FX_PROFILE_WINDOW=first-frame[,<s>]` (reset and enable at the first
  presented frame, stop `<s>` later) or `period=<s>` (restart every `<s>`),
  in `src/alchemy/fx/fx_profile_window.cpp`; the presenter calls
  `alchemy::fx_profile_window::frame_presented()`. ALCHEMY_PERF can call it from
  its own frame hook instead.
- **Section-backed `vm_remap`.** fx shared guest memory
  (`map_shared_guest_memory` / `alias_guest_memory`, docs/FX_ENGINE.md);
  `FxEngine::map_shared_memory` / `alias_memory`. `vm_allocate` maps shared
  memory on alchemy-fx, and `vm_remap` of such memory is a real alias (data
  and code; translations are invalidated through either mapping). Writable
  sources that are not `vm_allocate` memory (heap, image data) still fail with
  the capability diagnostic; immutable sources keep the copy path. Test:
  `alchemy_fx_engine` (fixture `fx_vm_remap`).
- **Background translation of Mach-O images.** `FxEngine::register_image`
  passes LC_FUNCTION_STARTS, the initializers and LC_MAIN to
  `fx::aot_external_module`, which translates ahead of the guest in-process
  (flow-ahead) and, after 20 s, whole small images into their `.fxa` cold
  files. `TRANSMUTE_FX_EXTERNAL_AOT=0` turns it off. docs/FX_ENGINE.md has the
  knobs and the measured effect.
