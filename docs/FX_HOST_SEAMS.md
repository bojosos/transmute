# fx host seams

Every place where the fx engine depends on the host operating system, what
it is, where it lives, its Linux counterpart, whether it is on a hot path,
and whether it now goes through a seam in `src/fx/host/` or stays where it
is (and why). Written by the refactor wave's group r4
(`docs/ALCHEMY_BRIEF_REFACTOR_R4.md`).

## Summary

* **The seam layer** is `src/fx/host/`: one header per area (`sync`, `time`,
  `thread`, `process`, `vm`, `fault`, `guard`, `unwind`, `env`, `file`,
  `text`, `sample`, `diagnostics`) and the Win32 backend in `src/fx/host/win32/`. CMake picks the
  backend (`TRANSMUTE_FX_HOST=win32`, `src/fx/host/component.cmake`); there
  are no virtual calls. `src/fx/host/host.hpp` has the rules.
* **Zero cost on hot paths.** The calls that run per lookup, per
  translation or per guest memory copy are inline in the backend's headers
  (`win32/sync.hpp`, `win32/time.hpp`, `win32/thread.hpp`,
  `win32/fault.hpp`, `win32/guard.hpp`), which declare the few kernel32
  functions they need exactly as the SDK does (`win32/kernel32.hpp`), so
  they don't pull in `windows.h`. Out-of-line seams wrap system calls only.
  The performance gate (below) measures the result.
* **Done (behind a seam, no OS header left):** the runtime core
  (`runtime/`, with the sampling profiler since batch 2), the translator's
  tick counter and image-audit tool, the interpreter's memory queries, the
  whole translation cache (`cache/` except tests), the guest
  vectored-handler list, the seh scratch memory and diagnostics,
  `transmute-fx`'s `main.cpp`, and the Alchemy engine adapter
  (`src/alchemy/fx/`). The core-type statistics diagnostic, Windows
  hybrid-CPU code throughout, moved into the backend
  (`src/fx/host/win32/corestats.cpp`, `host/diagnostics.hpp`).
* **Documented and left in place:**
  * the **Win64 calling convention and unwind format of generated code**
    (stubs, translator, abi thunks, `src/jit_x64_windows.cpp`): a SysV port
    is a code-generation change, not a seam;
  * the **PE guest personality** (`loader/`, `abi/`, `seh/`'s winnt.h
    guest layouts, the `cache/aot.cpp` PE AOT driver): it implements
    Windows for Windows ARM64 guests by calling the host's Windows, so a
    Linux host builds Alchemy without it;
  * nothing else: batch 2 moved the diagnostic samplers and the image-audit
    tool behind seams.
* **Before:** 27 non-test fx files (plus the adapter's 2) included
  `windows.h`, `dbghelp.h` or `psapi.h`; every runtime file got `windows.h`
  through `runtime_internal.hpp`. **After batch 1:** 17. **After batch 2:** 9,
  all PE core (list at the end).
* **Boundary check.** `scripts/check_host_boundary.ps1` (integrator, on
  `integ/refactor-host`) already exempts `src/fx/host/` and scans `src/fx/`.
  The files this wave cleaned leave its allow list (see "Allow-list changes").

## The seams

Hot path: **hot** = per guest instruction, lookup miss, guest memory copy or
host call; **warm** = per translation, fault or thread; **cold** = start-up,
exit, cache files, diagnostics.

### Faults (VEH)

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| First-chance fault handler: SMC write faults, guest stack guard pages, host calls into guest code (execute faults), faults inside translated code | `src/fx/runtime/veh.cpp:92` (`fault_handler`), `src/fx/host/fault.hpp`, `src/fx/host/win32/fault.cpp` | `AddVectoredExceptionHandler`, `EXCEPTION_POINTERS` | `sigaction(SIGSEGV/SIGBUS/SIGILL/SIGTRAP, SA_SIGINFO)` on an alternate stack; `FaultRecord` filled from `siginfo_t` (`si_addr`, access from the page-fault error code) | warm | **done** |
| Fault record: fx's guest exception model is NTSTATUS-shaped; `host::FaultRecord` has the `EXCEPTION_RECORD` layout (static_asserts), so win32 passes the host's record through | `src/fx/host/fault.hpp` | `EXCEPTION_RECORD` | backend synthesizes `kFaultAccessViolation` + `{access, address}` | warm | **done** |
| Interrupted register state: rip, rsp, r10, r15, the 16 GPRs, xmm0-15, EFLAGS | `src/fx/host/win32/fault.hpp` (inline, CONTEXT offsets checked in `fault.cpp`) | `CONTEXT` | `ucontext_t` (`gregs[REG_RIP]`, `fpregs->_xmm`); different GPR order, a table | warm | **done** |
| Host-fault trace (`TRANSMUTE_FX_TRACE_HOST_EXCEPTIONS`) with module offsets | `src/fx/host/win32/fault.cpp` (`trace_fault`) | `RtlCaptureStackBackTrace`, `GetModuleHandleExA` | `backtrace`, `dladdr` | cold | **done** |
| Guest stack growth: guard pages below every guest stack, re-armed one page down | `src/fx/runtime/threads.cpp:205` | `PAGE_GUARD` (`host::vm::kGuard`), `STATUS_GUARD_PAGE_VIOLATION` | no guard pages: a `PROT_NONE` page plus the fault handler re-protecting it (same logic, `kGuard` mapped by the backend) | warm | **done** (semantics documented in `vm.hpp`) |

### Guarded memory access (SEH)

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| `safe_read32` (JIT code reads, interpreter fetch) | `src/fx/runtime/runtime_internal.hpp:255` -> `host::try_read32` | `__try/__except` | per-thread `sigsetjmp` recovery point the fault handler `siglongjmp`s to | warm | **done** (same call depth: the inline forwarder replaces the old out-of-line function) |
| Interpreter step under a fault guard | `src/fx/runtime/dispatcher.cpp:171` -> `host::call_guarded` (template in `win32/guard.hpp`) | `__try` + `GetExceptionInformation` | recovery point | hot (interpreted instructions) | **done** (MSVC keeps an SEH function out of line either way: one frame, as before) |
| `.pdata` reads | `src/fx/runtime/code_memory.cpp:67` -> `host::try_read64` | `__try` | recovery point | warm | **done** (one guarded 8-byte read instead of two 4-byte ones) |
| Guest memory copies for providers (`guarded_move/fill/length`) | `src/alchemy/fx/fx_engine.cpp:73` -> `src/fx/host/win32/guard.cpp` | `__try`, filter AV / in-page / guard page | recovery point | hot | **done** (moved, not wrapped: same call depth) |
| Cache guest hash and header identity, external AOT word reads | `src/fx/cache/cache.cpp:947`, `src/fx/cache/external_aot.cpp:190` -> `host::call_guarded_access` | `__try`, filter AV / in-page | recovery point | warm | **done** |

### Generated code: calling convention and unwind

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| Win64 convention of generated code: enter stub (saves rbx, rbp, rdi, rsi, r12-r15, xmm6-15; 32-byte shadow space), host-call bridge (rcx/rdx/r8/r9 + xmm0-3 slots), helper calls `helpers[id]` with rcx = GuestContext | `src/fx/runtime/stubs.cpp:12`, `:25`; `src/fx/translator/emit_context.hpp:106`; `src/fx/runtime/dispatcher.cpp:442` (`host_call_bridge_entry`) | Win64 ABI | SysV: rdi/rsi/rdx/rcx/r8/r9, no shadow space, xmm callee-volatile, red zone | hot | **left**: a code-generation change (register assignment, frame layout, every helper call site) with its own perf risk; needs a `CallConv` description the stubs and emitters consume |
| The reference interpreter's JIT (`fx_oracle`) | `src/jit_x64_windows.cpp:1279` | Win64 leaf regions, rcx = CpuState | SysV | hot (tests, one-instruction fallback) | **left**: same reason |
| PE abi thunks call host Win64 DLL exports and read `gs:[0x68]` (TEB LastErrorValue) | `src/fx/abi/thunk_gen.cpp:50` | Win64, TEB | not built (PE guests only) | hot | **left**: PE personality |
| Unwind data of the code arena: UNWIND_INFO blocks built by `build_unwind`, RUNTIME_FUNCTION-shaped `host::UnwindEntry`s | `src/fx/runtime/stubs.cpp:87`, `src/fx/runtime/arena.cpp:50` | Win64 unwind codes | DWARF CFI (`.eh_frame` FDEs) | warm (C++ exceptions through guest frames) | format **left** (part of the convention) |
| Registration of the arena's unwind lookup | `src/fx/runtime/arena.cpp:130` -> `host::register_unwind_lookup` (`src/fx/host/win32/unwind.cpp`) | `RtlInstallFunctionTableCallback` | `__register_frame` per FDE (libgcc/libunwind) | cold | **done** |

### Memory: reservations, protection, placeholders, sections

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| Code arena: 1 GiB RWX reservation (top-down), committed in 1 MiB chunks | `src/fx/runtime/arena.cpp` | `VirtualAlloc(MEM_RESERVE \| MEM_TOP_DOWN)`, `MEM_COMMIT` | `mmap(PROT_NONE)` + `mprotect(RWX)`; no top-down flag: a hint address | cold | **done** |
| Lookup table, region metadata, unwind table, shadow TEBs, guest stacks, seh scratch | `src/fx/runtime/lookup.cpp:35`, `regions.cpp`, `threads.cpp`, `src/fx/seh/guest_call.cpp` | `VirtualAlloc`/`VirtualFree` | `mmap`/`munmap` | cold | **done** |
| SMC: write-protect translated guest pages, restore the guest protection after a write fault | `src/fx/runtime/regions.cpp:599` (`protect_pages`) | `VirtualQuery`, `VirtualProtect` | `/proc/self/maps` (or fx's own page table), `mprotect` | warm | **done** |
| Readability checks (JIT code source, interpreter, dispatcher, fault handler) | `src/fx/runtime/code_memory.cpp:56`, `dispatcher.cpp`, `veh.cpp`, `src/fx/common/interpreter.cpp:36` | `VirtualQuery` | fx-side region table; `/proc/self/maps` is too slow for the per-page query | warm (cached per page) | **done** |
| Protection codes: fx keeps the Win32 `PAGE_*` values (public `process.hpp` API, PE abi) as `host::vm::Protection` | `src/fx/host/vm.hpp` | identity | `PROT_*` mapping in the backend | — | **done** |
| Shared guest memory: placeholders split per page and replaced by section views; unmapping leaves the placeholder | `src/fx/runtime/shared_memory.cpp:108`, `:164`, `:268` | `VirtualAlloc2(MEM_RESERVE_PLACEHOLDER)`, `VirtualFree(MEM_PRESERVE/COALESCE_PLACEHOLDER)`, `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`, `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)`, `CreateFileMapping2`, `DuplicateHandle` | a `PROT_NONE` reservation; views by `mmap(MAP_FIXED \| MAP_SHARED, memfd)`; unmapping re-maps `PROT_NONE` (`MAP_FIXED_NOREPLACE` for fresh reservations); split/coalesce are no-ops | warm | **done** |
| Section handles across the public API (`map_section_guest_memory`, `duplicate_shared_guest_section` take/return `void*` HANDLEs) | `include/transmute/fx/process.hpp:162` | file-mapping HANDLE | memfd file descriptor | — | **done** (opaque `host::vm::Section`) |
| Alchemy guest memory: granule reservations, commits, protections, decommit, discard, low-4 GiB keep-out, reservations above 4 GiB, the guest heap | `src/alchemy/fx/fx_engine.cpp:78`, `:1459`, `:1705` | `VirtualAlloc/Protect/Free/Query`, `VirtualAlloc2` + `MEM_ADDRESS_REQUIREMENTS`, `MEM_RESET`, `HeapCreate`+LFH, `HeapAlloc/Free` | `mmap`/`mprotect`/`madvise(MADV_FREE)`, a hint search above 4 GiB, a private allocator (or the guest's own malloc) | warm | **done** |
| Instruction-cache flush after writing code | `stubs.cpp`, `regions.cpp`, `src/fx/host/win32/vm.cpp` | `FlushInstructionCache` | nothing on x86-64 (`__builtin___clear_cache`) | cold (regions skip it on x64) | **done** |

### Threads, TLS and the thread environment block

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| Per-thread state (`ThreadState`, caches) | `src/fx/runtime/threads.cpp:23` and elsewhere | C++ `thread_local` | C++ `thread_local` | hot | portable, no seam needed |
| Guest threads: create with the guest's CreateThread flags, exit, last error | `src/fx/runtime/threads.cpp:334` -> `host::create_thread` | `CreateThread`, `ExitThread`, `Get/SetLastError` | `pthread_create` (suspended start emulated), `pthread_exit`, `errno` | warm | **done** |
| Thread teardown at the right moment (after FLS callbacks, before thread_local destructors) | `src/fx/runtime/threads.cpp:101` -> `host::set_thread_exit_hook` (`src/fx/host/win32/thread.cpp`, `.CRT$XLB` TLS callback) | PE TLS callback, `DLL_THREAD_DETACH`, `RtlDllShutdownInProgress` | a `pthread_key_create` destructor | warm | **done** |
| Shadow TEB: a copy of the host TEB per guest thread (x18), with the guest stack bounds | `src/fx/runtime/threads.cpp:59` -> `host::copy_thread_block`; TEB offsets are guest-ABI constants | `NtCurrentTeb`, `VirtualQuery` | no host TEB: the shadow block starts zeroed (Mach-O guests do not read it) | warm | **done** |
| Host TEB pointer and stack base (host callbacks size their argument copy) | `src/fx/runtime/dispatcher.cpp:449` -> `host::thread_stack_base` (inline gs:[8]); `GuestContext::host_teb` | `NT_TIB` via gs | `pthread_getattr_np` once per thread | hot (each host-to-guest callback) | **done** |
| Activation contexts on guest threads and borrowed threads | `src/fx/runtime/threads.cpp`, `dispatcher.cpp:335` -> `host::activate_context` | `ActivateActCtx`, `DeactivateActCtx` | none (PE guests only): returns 0 | warm | **done** |
| Background threads: preloaders, cache writer, AOT scheduler and workers; priorities; EcoQoS | `src/fx/runtime/regions.cpp:1167`, `src/fx/cache/cache.cpp`, `background.cpp`, `external_aot.cpp`, `src/fx/runtime/runtime.cpp:47` | `CreateThread`, `SetThreadPriority`, `SetThreadInformation(ThreadPowerThrottling)` | `pthread_create`, `setpriority`/`SCHED_IDLE`; no EcoQoS (no-op) | cold | **done** |
| Thread-pool work (cache module opens) | `src/fx/cache/cache.cpp:558` -> `host::submit_to_thread_pool` | `TrySubmitThreadpoolCallback` | a small worker pool | cold | **done** |

### Locks, condition variables, clocks

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| Reader-writer locks: region map (with its spin-then-block wait), arena, thread registry, interrupts, shared memory, cache, guest VEH list | `src/fx/runtime/regions.cpp:282`, `:320`; `src/fx/host/win32/sync.hpp` (inline) | `SRWLOCK` | `pthread_rwlock_t` or a futex lock | hot (lookup misses, installs) | **done** |
| Condition variables (preloader queue, cache writer, AOT workers, module open tasks), shared-mode waits | `src/fx/host/win32/sync.hpp` | `SleepConditionVariableSRW` (incl. `CONDITION_VARIABLE_LOCKMODE_SHARED`) | `pthread_cond_t` (shared waits need an rwlock-aware condvar) | cold | **done** |
| Tick counter and frequency: translation statistics, lock-wait timing, map-lock spin bound | `src/fx/runtime/runtime_internal.hpp:366`, `src/fx/host/win32/time.hpp` | `QueryPerformanceCounter/Frequency` | `clock_gettime(CLOCK_MONOTONIC)`, frequency 1e9 | hot | **done** |
| CNTVCT_EL0: rdtsc scaled, or the tick counter as the fallback | `src/fx/translator/emit_system_timer.cpp:6`, `:23` | QPC; `__rdtsc`, `__cpuid` (compiler intrinsics) | `clock_gettime`; the intrinsics are `<x86intrin.h>`/`<cpuid.h>` | hot (guest timer reads) | **done** |
| Monotonic milliseconds, wall clock, process times, sleeps | `src/fx/host/time.hpp` | `GetTickCount64`, `GetSystemTimeAsFileTime`, `GetProcessTimes`, `Sleep` | `CLOCK_MONOTONIC_COARSE`, `CLOCK_REALTIME` (+ 1601 epoch offset), `getrusage`/`/proc/self/stat`, `nanosleep` | cold | **done** |

### Translation cache files

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| Main-file mapping and private reads (cache main files, cold AOT files, journals) | `src/fx/cache/pe_view.cpp:178`, `:205` (`MappedFile`), `src/fx/cache/merge.cpp:181` -> `src/fx/host/file.hpp` | `CreateFileW` share modes, `CreateFileMappingW`/`MapViewOfFile`, `ReadFile` | `open`, `mmap(PROT_READ, MAP_PRIVATE)`, `read` | cold (start-up) | **done** |
| Multi-process protocol: delete-on-close lock files, journals opened without write sharing, rename-over publication, POSIX delete of mapped journals | `src/fx/cache/merge.cpp:91`, `:175`; `cache.cpp:757` (`open_journal`); `aot.cpp:268` | `FILE_FLAG_DELETE_ON_CLOSE`, share modes, `MoveFileExW(REPLACE_EXISTING)`, `FileDispositionInfoEx` POSIX semantics | `flock(LOCK_EX)` on lock files (unlinked by the holder), `fcntl` leases or `flock` for "journal still open", `rename`, `unlink` | cold | **done** (semantics kept; a Linux backend needs the "open without write sharing fails while the writer lives" rule as an explicit lock) |
| Directory listing, attributes, directory creation, temp directory, cache directory default | `src/fx/cache/cache.cpp:391` (`ensure_listing`), `merge.cpp`, `cache.cpp` (`default_directory`) | `FindFirstFileExW(LARGE_FETCH)`, `GetFileAttributesExW`, `CreateDirectoryW`, `GetTempPathW`, `%LOCALAPPDATA%` | `opendir`/`readdir`/`fnmatch`, `stat`, `mkdir`, `$TMPDIR`, `$XDG_CACHE_HOME` | cold | **done** (the `%LOCALAPPDATA%` default and `\\` separators stay in cache.cpp: path policy, not an OS call) |
| Paths and module names as `std::wstring` (UTF-16) | `GuestModule::name/path` (`include/transmute/fx/process.hpp`), the cache | UTF-16 | UTF-8 `std::string`; the seam keeps `std::wstring` and a Linux backend converts at the call | — | **left** (type change across the public API; `host::text` converts for messages) |
| Background AOT helper processes (PE images) | `src/fx/cache/background.cpp:87` -> `host::spawn_background_process` | `CreateProcessW(DETACHED_PROCESS \| IDLE_PRIORITY_CLASS \| CREATE_BREAKAWAY_FROM_JOB)`, `RegisterWaitForSingleObject` | `posix_spawn` + `setsid`, `nice`, a waiter thread or pidfd | cold | **done** |
| Weak default of `translator_family_fingerprint` | `src/fx/cache/cache.cpp:53` | MSVC `/alternatename` | `__attribute__((weak))` | — | **left** (linker, not OS) |

### Process, environment, text

| What | Where | Windows | Linux | Hot | Status |
| --- | --- | --- | --- | --- | --- |
| Environment settings (`TRANSMUTE_FX_*`, `ALCHEMY_FX_*`) | `src/fx/host/env.hpp` | `GetEnvironmentVariableA/W` | `getenv` (+ UTF-8 conversion for the wide form) | cold | **done** |
| Process id, shutdown check, immediate termination | `src/fx/host/process.hpp` | `GetCurrentProcessId`, `RtlDllShutdownInProgress`, `TerminateProcess` | `getpid`, an atexit flag, `_exit` | cold | **done** |
| UTF-16 <-> UTF-8 for messages and module paths | `src/fx/host/text.hpp` | `WideCharToMultiByte`/`MultiByteToWideChar` | a small UTF-8 codec | cold | **done** |
| `_stricmp`, `_wcsicmp` | `runtime.cpp`, `background.cpp` | CRT | `strcasecmp`, `wcscasecmp` | cold | **left** (CRT, not OS) |
| x86 intrinsics: `_InterlockedCompareExchange128`, `__cpuid`, `_umul128`/`_udiv128`, `_mm_getcsr` | `src/fx/common/interpreter.cpp:142`, `src/fx/common/cpu_features.cpp:11`, translator | `<intrin.h>`, `<immintrin.h>` | `<x86intrin.h>`, `<cpuid.h>`, `__int128` | hot | **left** (compiler, not OS; a small `fx/common` wrapper when a second compiler appears) |

### PE guest personality (left in place)

These implement Windows for Windows ARM64 PE guests (`transmute-fx`) by
binding guest imports to the host's own Win32 DLLs. They are Windows-host
only by nature: on Linux there are no host Win32 DLLs to bind (that would be
Wine). Alchemy (Mach-O guests) does not use them; it links them today only
because `alchemy_fx_engine` links every fx library
(`cmake/AlchemyFx.cmake`). A Linux build of Alchemy would link
`fx_runtime`, `fx_translator`, `fx_emit_*`, `fx_common`, `fx_cache` and
`fx_seh` and stub the loader hooks the runtime calls (`loader_thread_attach`,
`guest_activation_context`, `bind_unknown_host_target`, as the runtime tests
do in `runtime_test_stubs.cpp`).

| What | Where | Windows | Linux | Status |
| --- | --- | --- | --- | --- |
| PE loader: image mapping as kernel image sections (`SEC_IMAGE_NO_EXECUTE`: the kernel lays out the sections and applies relocations), imports, TLS, activation contexts, the PE process model (ExitProcess with guest DLL_PROCESS_DETACH, a process-detach TLS callback, the PEB to the guest entry) | `src/fx/loader/image.cpp:200`, `loader.cpp`, `search.cpp`, `tls.cpp:283`, `process.cpp:362`, `loader_internal.hpp` | `CreateFileMappingW(SEC_IMAGE_NO_EXECUTE)`, `LoadLibraryExW`, `NtCurrentTeb`, `ActivateActCtx`, `ExitProcess` | not built: a Linux host has no Win32 DLLs to bind PE imports to (that is Wine's job). If PE guests were ever wanted there, the image mapping becomes the copy path image.cpp already has (read the sections, relocate in user mode) over `host::vm`, and the process model maps to `atexit`/`_exit` | **left** (image.cpp is PE image loading, not generic file mapping) |
| `transmute-fx` command line driver: process QoS, idle priority for AOT helpers, paths | `src/fx/loader/main.cpp` -> `host::process`, `host::file::full_path` | `Get/SetProcessInformation(ProcessPowerThrottling)`, `SetPriorityClass`, `GetModuleFileNameW`, `GetFullPathNameW` | `setpriority`, `/proc/self/exe`, `realpath` | **done** (batch 2) |
| Win32 API bridging: thunks, overrides of memory, module, thread, fiber, unwind and exception APIs | `src/fx/abi/bind.cpp:192`, `overrides.cpp:994`, `overrides_crt.cpp`, `overrides_fiber.cpp`, `diagnostics.cpp` | the Win32 API it emulates | not built (no host Win32 API to bridge to); Alchemy's Mach-O guests reach the host through `src/alchemy/host/` instead | **left** |
| Guest SEH: ARM64 `.pdata/.xdata` unwinding, exception dispatch, guest function tables | `src/fx/seh/seh.hpp:6` (winnt.h `ARM64_NT_CONTEXT`, `DISPATCHER_CONTEXT_ARM64`, `IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY`, `EXCEPTION_RECORD` layouts) | guest ABI types from winnt.h; no host calls left in `seh/` (this wave moved `dispatch.cpp`'s, `guest_call.cpp`'s and `diag.cpp`'s) | the same layouts, defined by fx (`fx/seh/arm64_nt.hpp`, to write); the dispatch logic is host-independent | **left**: needs fx's own copies of those layouts (public documentation, static_asserts against winnt.h on Windows); Mach-O guests use `seh` too (the fault hook), so this is the one personality piece a Linux Alchemy needs |
| PE AOT driver and PE parsing | `src/fx/cache/aot.cpp` (its lock file now goes through `host::file`), `pe_view.cpp` (parsing is pure) | — | builds unchanged on the seams (`host::file`) | **done** for the OS calls; PE-only by purpose |

### Diagnostics (batch 2: behind seams)

| What | Where | Windows | Linux | Status |
| --- | --- | --- | --- | --- |
| Sampling profiler (`TRANSMUTE_FX_SAMPLE`): suspends guest threads, reads their registers and stack top, unwinds the copy, symbolizes host frames; report files replaced atomically | `src/fx/runtime/profiler.cpp` -> `src/fx/host/sample.hpp` (`win32/sample.cpp`), `host::AutoResetEvent`, `host::file::replace_atomically` | `OpenThread`, `SuspendThread`, `GetThreadContext`, `RtlVirtualUnwind`, dbghelp, high-resolution waitable timer, `FileRenameInfoEx` | `tgkill` + a `SIGPROF` handler copying `ucontext_t` and the stack top, frame-pointer or CFI walk, `dladdr`, `timerfd`, `renameat2` | **done** (batch 2) |
| Core statistics (`TRANSMUTE_FX_CORESTATS`): P-core/E-core share of the process's threads | `src/fx/host/win32/corestats.cpp` (moved), `src/fx/host/diagnostics.hpp`; the runtime passes guest thread ids (`runtime.cpp`) | CPU sets, special user APCs (`QueueUserAPC2`), `NtGetNextThread`, `QueryThreadCycleTime` | `/proc/self/task/*/stat`, `sched_getcpu`; or no-ops | **done** (batch 2: a backend diagnostic) |
| Translator image audit tool (`fx_translator_image_audit`) | `src/fx/translator/core_image_audit.cpp` -> `host::sample`, `host::vm`, `host::thread` | `SuspendThread`, `GetThreadContext`, dbghelp, `SetThreadAffinityMask` | as the profiler | **done** (batch 2) |

## Performance gate

Measured with interleaved runs of the before-binary (d2e8ef8, a copy of
this worktree's build before any change) and the after-binary, cloaked,
`ALCHEMY_NETWORK=loopback`, on the shared laptop (13th Gen i7-1355U)
while other groups built. See the r4 report for the numbers; the method is
`scripts/perf/startup.ps1` (Blender windowed), Blender `-b
--factory-startup` + `primitive_monkey_add` process CPU time, and
`scripts/fx_bench.ps1` (the `docs/FX_ENGINE.md` gates: compute, mem,
calls, heap, icall, recursion, interp).

Result (2026-10-18; before d2e8ef8 `alchemy-fx` 35348c9b, after 760f627
`alchemy-fx` ae8de037; load 27-100 %, mostly 100 %):

| Measure | Before | After |
| --- | --- | --- |
| Blender windowed, first 3D viewport draw, 3 interleaved runs (median) | 8.8 / 6.5 / 6.6 s (6.57 s), exit 0 ×3 | 131 (load 100 %) / 6.6 / 5.1 s (6.57 s), exit 0 ×3 |
| Blender headless process CPU, 13 interleaved pairs (median) | 7.30 s (12 runs; 1 exited at load: "requested segment address is unavailable") | 7.38 s (13 runs) |
| fx_bench, fx ns per op (2 rounds × 5 runs each) | — | every row within ±2.2 % of before; gate PASS/FAIL identical per row |

Batch 2 (samplers, image-audit tool, transmute-fx main; before 8e1154d
`alchemy-fx` ef892bf7, after 68ab1c3 `alchemy-fx` 3702e6b8; load 9-47 %):
windowed first viewport 6.48 / 6.70 / 7.42 s (median 6.70) before and
4.97 / 7.31 / 7.35 s (median 7.31) after, exit 0 ×3 each; headless CPU
median 3.21 s before and 3.18 s after (6 pairs); fx_bench rows from -14 %
to +3.4 % in both directions, gate status identical. Nothing batch 2 changed
runs on Alchemy's paths unless TRANSMUTE_FX_SAMPLE or TRANSMUTE_FX_CORESTATS
is set.

The pre-change measurement on the unchanged tree (lighter load) gave
headless 4.75-5.06 s CPU and the same four fx_bench gate failures
(CriticalSection, SRWLock, HeapAlloc, malloc), which predate this work.

## Allow-list changes

Files that no longer include an OS header (remove them from
`scripts/host_boundary_allow.txt` when `integ/refactor-host` and this
branch meet):

`src/fx/runtime/runtime_internal.hpp`, `shared_memory.cpp`;
`src/fx/translator/emit_system_timer.cpp`; `src/fx/common/interpreter.cpp`;
`src/fx/cache/aot.cpp`, `background.cpp`, `cache.cpp`, `external_aot.cpp`,
`merge.cpp`, `pe_view.cpp`; `src/fx/seh/diag.cpp`, `dispatch.cpp`,
`guest_call.cpp`, `vectored.cpp`; `src/alchemy/fx/fx_engine.cpp`,
`fx_profile_window.cpp`.

Files that still do (outside `src/fx/host/` and tests):

| File | Group |
| --- | --- |
| `src/fx/abi/bind.cpp`, `diagnostics.cpp`, `overrides.cpp`, `overrides_crt.cpp`, `overrides_fiber.cpp` | PE personality |
| `src/fx/loader/image.cpp`, `loader_internal.hpp`, `process.cpp` | PE personality |
| `src/fx/seh/seh.hpp` | guest ABI layouts (winnt.h) |

Batch 2 also removes `src/fx/runtime/profiler.cpp`, `profiler_window.hpp`,
`corestats.cpp` (moved to `src/fx/host/win32/`),
`src/fx/translator/core_image_audit.cpp` and `src/fx/loader/main.cpp` from
the allow list.

fx's unit tests live next to their sources (`src/fx/*/*_tests.cpp`,
`src/fx/cache/test_support.cpp`, `src/fx/translator/core_test_machine.cpp`,
`src/fx/testing/harness.cpp`) rather than in `tests/` directories, so the
check's `tests?/` exemption does not cover them; they stay on the allow list
or the check learns fx's `*_tests.cpp` naming.
