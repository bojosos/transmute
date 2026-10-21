# Blender ARM64 compatibility audit

This is a static inventory of the staged Blender 4.5.14 Windows ARM64 MSI
payload, compared with Transmute's source on 27 September 2026. Import coverage
counts were refreshed after the virtual-memory, process-handle, low-level CRT,
file-mapping, waitable-object, locale, path, allocation-handler, GUI,
guest-memory sort, special-folder, and SIMD implementation batches. It identifies
work to batch into subsystems before trying another startup run. It does not
claim that every imported API runs in the Classroom
render. The current runtime has not completed the ARM64 installation, launch,
or render.

## Reproduce the inventory

`scripts/audit_arm64.ps1` reads PE headers and import tables from
`llvm-objdump -p`. It finds
Visual Studio's x64 LLVM tool if `llvm-objdump` is absent from `PATH`; pass
`-Objdump` to override. Run from the repository root:

```powershell
$base = 'artifacts\arm64-stage\Blender Foundation\Blender 4.5'
.\scripts\audit_arm64.ps1 `
  -Directory $base,(Join-Path $base 'blender.crt'),(Join-Path $base 'blender.shared') `
  -Top 10000 | Set-Content artifacts\arm64-import-audit-full.json

.\scripts\probe_embree.ps1 -Stage $base -Phase triangle -RayQuery `
  -BlenderGeometry -Instance -PacketWidth 16 -SingleThread
```

The Embree probe builds its own small ARM64 PE from the local fixture; use
`-Phase load`, `device`, or `newscene` to isolate earlier API boundaries.

The first run covers 58 PE files: 50 reported as `coff-arm64`, seven
`coff-arm64x`, and one `coff-arm64ec`. There are **15,260 static imports**:
10,424 resolve to packaged guest DLLs, 4,836 to Windows host bridge candidates,
and none to a missing named DLL in the supplied search directories. The bridge
source does not name 1,137 of the host import occurrences, representing 626
distinct `DLL!function` pairs. Among those distinct pairs, 151 are from
`KERNEL32.dll`, 137 from `USER32.dll`, and 17 from the UCRT stdio API set.
`blender.exe` alone imports 3,554 symbols; 573 resolve to host libraries and
221 of those names are absent from the audit's bridge-source snapshot.
`embree4.dll` has one such host import: `_seh_filter_dll`. The `CreateThread`
bridge has guest-worker lifecycle tests. The direct ARM64 Embree fixture now
commits a triangle and verifies scalar and 4/8/16-lane intersections and
occlusion through the packaged DLL, both with a single worker and with its
default worker pool. The fixture also exercises the host/device geometry-buffer,
explicit geometry-ID, traversable, and transformed instance APIs imported by
Blender. A 32-by-32 triangle grid also passes on the ARM64 interpreter with
the default worker pool, progress monitor callback, and instancing. Its
equivalent native x64 probe passes with normal C runtime startup; 20 repeated
native runs passed. An earlier empty-scene commit raised a TBB C++
exception; the populated triangle path avoids that unsupported teardown case.
The 64-by-64 grid with packets, callback, and instancing passed through the
in-house JIT and the native x64 DLL as well.
An extended fixture also calls the five remaining Embree functions imported
by `blender.exe`: device error and memory-monitor registration, shared
geometry buffer setup, curve tessellation rate, and quaternion instance
transform. It checks an actual memory callback, buffer pointer identity,
curve bounds, and an instanced ray hit. The native x64 fixture, ARM64
interpreter, and ARM64 JIT with Embree's default worker pool pass. Error
callback invocation is not asserted. This covers the imported function names,
not every Embree behavior or Cycles scene. A direct import-table comparison
found all 32 `embree4.dll` functions imported by `blender.exe` across the two
executed ARM64 fixtures, which together import 44 Embree functions.
Among the 58 staged PE files, only `blender.exe` imports `embree4.dll`
directly. Blender's [Cycles Embree integration](https://github.com/blender/blender/blob/main/intern/cycles/bvh/embree.cpp)
shows the relevant build paths: triangle and curve buffers, argument filters,
transformed instances, motion time steps, progress callbacks, and vertex-buffer
updates during refit. That source describes current Cycles code; the staged
binary's import table confirms function names but does not prove which branch
the packaged build executes. The fixture's `-DynamicUpdate` mode checks more
than imported names:
after releasing its creator reference, it moves every vertex of an attached
mesh, calls `rtcUpdateGeometryBuffer`, recommits the geometry and scene, and
checks changed bounds, old-position misses, new-position hits and occlusion,
and inactive packet lanes. Application-shared, Embree-owned, and host/device
vertex buffers pass in native x64 and the ARM64 interpreter. The shared-buffer variant
also passes with Embree's default worker pool and Transmute's JIT. This follows
Embree's [buffer-update example](https://github.com/RenderKit/embree/blob/master/tutorials/multiscene_geometry/multiscene_geometry_device.cpp).
A 64-by-64 triangle grid with the progress callback and instancing also passes
the dynamic-update checks on native x64 and ARM64 JIT with the default worker
pool. These are compatibility results, not render-time or speed measurements.
The opt-in `-ArgumentFilter` mode supplies Embree 4 ray-query argument
structures and a guest callback that rejects a candidate hit. It asserts that
the callback receives the expected geometry user pointer, that both
intersection and occlusion become clear, and that the callback runs for both
queries. The packaged native x64 DLL and translated ARM64 DLL pass this mode
with Blender-style host/device buffers and with the 64-by-64 shared-buffer
grid; the latter passes under the in-house JIT and default Embree worker pool.
The filter-argument layout follows Embree's published
[scene header](https://github.com/RenderKit/embree/blob/master/include/embree4/rtcore_scene.h)
and [common header](https://github.com/RenderKit/embree/blob/master/include/embree4/rtcore_common.h).
These results cover a small geometry and ray-query workload, not all Embree
features or Blender's Cycles workload.

The audit includes imports by ordinal. It previously omitted 43 such entries;
the refreshed counts above include them. Blender imports `COMCTL32.dll!#17`
(`InitCommonControls`) and `COMCTL32.dll!#345` (`TaskDialogIndirect` in the
host's common-controls v6 DLL). The latter requires guest structure and
callback translation if it is reached; it cannot be forwarded as a raw pointer.

These counts are a **coverage inventory, not a failure rate**. Imports repeat
across DLLs and many are never called in a headless render. A name appearing
in `win32_bridge.cpp` does not prove correct argument, callback, lifetime,
error, or threading behavior. An absent name is a likely bridge gap under the
current explicit-dispatch design. The script classifies a DLL as guest when it
exists in a supplied guest directory, ahead of the host system directory, as
`GuestProcess` does. Supply both packaged DLL directories when auditing a
single binary so the result matches runtime search order.

## Guest thread lifecycle checks

A focused native Windows DLL fixture and its synthetic ARM64 counterpart now
agree on thread notifications: TLS callbacks run in array order before
`DllMain` on both attach and detach. Guest workers receive the DLL notifications
in process attachment order (reversed on detach), followed by executable TLS
callbacks. Notifications are serialized with process attach and dynamic DLL
initialization. Static TLS, dynamic TLS values, and CRT errno remain available
through detach callbacks; `DisableThreadLibraryCalls` rejects static-TLS DLLs.
The ordering was measured on this Windows host and follows the notification
and serialization rules documented for
[DllMain](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain).

`transmute_tests --test-thread-lifecycle` passes the native/guest comparison,
TLS template, TEB thread identity, and detach-state checks.
`transmute_tests --test-worker-failure` passes a separate regression where a
worker throws, its cleanup blocks and then throws again: the original failure
is visible before cleanup completes and remains the reported error afterward.
Both tests first failed on the missing behavior and passed after implementation.

`transmute_tests --test-thread-resources` now passes 1,050 successive real guest
workers. Finished workers reclaim their TEB, TLS pointer array, module TLS
blocks, errno, locale, and Winsock buffers. Slots and CRT error buffers are
reused after cleanup; dynamic TLS allocation ownership is tracked so rollback
cannot clear a replacement worker's slot. Per-thread bridge maps are purged,
and completed successful worker records release retained native handles. The
regression also checks static and dynamically loaded DLL TLS templates, empty
TLS/FLS values on replacement workers, initialization rollback, and a cleanup
callback that blocks while another worker runs. The slot remains mapped until
that callback finishes. The existing limit is now 1,023 concurrent workers,
instead of 1,023 worker creations over the process lifetime.
The combined Release CTest suite also passes both registered tests, including
the lifecycle, JIT, CRT, and instruction coverage changes in this batch.

These focused checks do not establish that the observed Cycles stall is fixed.
Normal CLI execution still needs process-wide failure cancellation when its
main thread is blocked. Whole-process DLL detach and Windows exception
semantics also remain incomplete.

## Wide guest-memory access checks

Reference 32/64-bit reads and writes now resolve a complete mapped or PE image
access under one address-space lock. Image-aware contiguous helpers also serve
the translated scalar memory families; the existing mapped-only API retains
its contract. Failed complete-access checks leave memory untouched and fall
back to the checked bytewise path, preserving writes made before a boundary or
permission fault. No raw guest pointer escapes the locked operation.

`transmute_tests --test-fast-memory` passes unaligned 1/2/4/8-byte accesses,
mapped and image boundaries, writable-section faults, adjacent mappings,
protection changes, remapping, external-memory coherency, executable image
invalidation, cross-engine exclusive granules, and translated image accesses.
`--test-width-atomicity` forces a callback pause during the checked fallback
across adjacent valid mappings and tests 20,000 concurrent load/write samples
per contiguous mapped/image width and interpreter/JIT configuration. The
earlier callback test's byte hook was bypassed by successful contiguous access;
it now diagnoses an unreached hook separately from a mixed value. The combined
Release suite passes 2/2 tests in 7.35 seconds, including the cache and buffered
stdio changes.
Seven alternating CPU-idle runs of `--bench-wide-memory`, each with 500,000
accesses per case, measured median speedups of 1.59/2.30 times for mapped
32-bit reads/writes, 3.38/3.70 times for mapped 64-bit reads/writes,
2.90/2.85 times for image 32-bit reads/writes, and 5.74/4.89 times for image
64-bit reads/writes. These method timings do not measure Blender render time.
The process-wide instruction/chain lock and existing bytewise bridge bulk
copies remain separate performance work.

## Prioritized implementation batches

1. **Guest process and Windows APIs.** Build guest-aware
   file/path/environment, process/thread/synchronization, memory mapping,
   console/stdio, locale, and GUI/message subsystems. A runtime probe during
   this audit stopped at `KERNEL32.dll!GetEnvironmentVariableW`; the main EXE's
   static imports also include `CreateFileW`, `ReadFile`, `WriteFile`,
   `GetCommandLineW`, `CreateProcessW`, `VirtualAlloc`, `WaitForSingleObject`,
   and many `USER32` APIs. Native Windows calls cannot receive guest pointers
   directly: each bridge needs typed structure conversion, guest handles,
   callback dispatch, last-error behavior, and ownership rules. Implement and
   test these by API family rather than adding one import at each crash. A
   focused ARM64 parent-to-child fixture now passes through `CreateProcessW`:
   it checks quoted arguments, Unicode environment, working directory,
   inherited pipe output, wait, and exit code. This does not yet establish
   installer subprocess compatibility. The
   staged `python311.dll` has a named shared-memory path that tries
   `OpenFileMappingW`, falls back to `CreateFileMappingW`, then calls
   `MapViewOfFile` and `UnmapViewOfFile`. Views need coherent shared backing,
   named-object lookup, correct offsets, and lifetime tracking. The current
   virtual-memory bridge covers private reserve/commit/decommit/release,
   protection, and queries. Named page-file mappings and coherent guest views
   pass focused tests; a staged Blender probe has not yet exercised this path.
   Repeated CRT startup names such as `_cexit`, argv/environment setup,
   `_callnewh`, and allocation-mode calls now have focused bridge tests. CRT
   exception setup (`_seh_filter_dll` in 51 binaries) remains an import gap.
   Repetitions indicate shared runtime prerequisites; they are not call-frequency
   measurements. A staged probe found and fixed the pointer-first argument
   order of `GetSystemDirectoryA/W` and `GetWindowsDirectoryA/W`; API names
   alone would not have revealed that behavior bug.

2. **PE lifecycle and exception semantics.** The 58-file scan finds 23 PE files
   with TLS directories, 57 with exception directories, and 56 with load-config
   directories; none has a delay-import directory. Since this audit snapshot,
   `PeImage` parses static TLS, the initial guest thread receives per-module
   TLS templates, and `ReferenceEngine` invokes process-attach TLS callbacks.
   These paths have a synthetic PE fixture, and the staged Blender probe now
   passes the prior `shaderc_shared.dll` TLS fault. Guest workers now receive
   separate stacks, TEBs, and static TLS pointer arrays. Worker TLS/DllMain
   thread attach/detach, notification suppression and resource reclamation now
   pass lifecycle fixtures, including a native Windows ordering comparison.
   Broader DLL unload/shutdown, Windows unwind/exception handling,
   and load-config policy remain open. Verify these against packaged TLS/CRT
   cases and dependency ordering before claiming installed Blender works.

   The GUI probe reaches `ole32.dll!CoCreateInstance` after about 206 million
   guest instructions. Its static callsite copies CLSID
   `{bcde0395-e52f-467c-8e3d-c4579291692e}` and IID
   `{a95664d2-9614-4f35-a746-de8db63617e6}` to the guest stack. These name
   the [MMDeviceEnumerator and IMMDeviceEnumerator Windows audio
   interfaces](https://learn.microsoft.com/en-us/windows/win32/coreaudio/mmdevice-api).
   A typed guest COM vtable, interface identity, reference counting, device
   methods, and callback conversion are needed before that GUI path can pass;
   returning a raw host COM interface pointer would be invalid.

   The headless Classroom path now passes a typed desktop `IShellFolder`
   proxy (`ParseDisplayName`, `GetDisplayNameOf`, `StrRetToStrW`, and ownership)
   and reaches `CoCreateInstance` for CLSID
   `{13709620-c279-11ce-a49e-444553540000}` and IID
   `{d8f015c0-c278-11ce-a49e-444553540000}`. This is Shell automation;
   subsequent calls request the Quick Access namespace `Folder`, its items,
   and their count.
   Those guest `IDispatch`/`VARIANT`/`BSTR` boundaries now have a typed bridge
   for the observed methods and pass the staged path. The next headless stop
   was CRT `_wputenv_s`. Separate guest-owned Win32 and CRT environment views
   now pass it. A typed CNG bridge for `BCryptGenRandom`, provider open, and
   close also passes the staged path; the next stop is CRT
   `PathCchSkipRoot` in Python startup, after passing `__fpe_flt_rounds`
   via guest FPCR rounding-mode mapping and a case-insensitive CRT string
   compare batch. A five-function PathCch batch and typed Win32 time-zone
   family passed subsequent staged runs. A later headless stop was
   `KERNEL32.dll!GetFileInformationByHandle` at about 204.70 million guest
   instructions, after a typed `FormatMessageW/A` bridge passed the preceding
   stop. File-metadata and CRT `_dclass` bridges passed subsequent stops;
   `GetFullPathNameW/A` has focused tests. A later headless run prints Blender's
   banner and starts guest workers. A debugger located a host CRT
   invalid-parameter fail-fast in the `_wopen` bridge; the corrected mode
   handling advances to UCRT `__sys_nerr` and
   `_set_thread_local_invalid_parameter_handler` on separate runs at about
   206.62 million guest instructions. Their focused bridges passed the staged
   path. A typed file operation batch passed `KERNEL32.dll!MoveFileExW` at
   about 219.54 million; the next stop is `KERNEL32.dll!GetVersionExW` at
   about 239.30 million. Version queries, `DC ZVA`, corrected NLS ninth
   arguments, and CRT formatted output passed later staged runs. The latest
   headless run hit its 500-million-instruction limit in `python311.dll` and
   printed a `sitecustomize` WinError 127 warning. A typed dynamic
   `AddDllDirectory` bridge cleared that warning in later 260-million- and
   500-million-instruction replays; neither produced a frame. Guest
   `LoadLibraryEx` now searches registered user directories when Windows flags
   request them. A synthetic runtime DLL suite now passes transitive ARM64
   dependency binding, dependency-before-`DllMain` attach, TLS setup on the
   loading and pre-existing worker threads, and rollback after failed attach.
   Reentrant loading during attach remains unsupported; this suite does not
   establish compatibility with every Blender plugin. A deeper Classroom probe reached
   `CreatePipe` at 3.14 billion instructions; its typed pipe bridge now passes
   focused tests. A replay followed a different startup path and stopped at
   `InitializeProcThreadAttributeList` after 1.22 billion instructions, before
   reaching the pipe call. A later replay stopped at `WS2_32.dll!#115`, which
   the host export table identifies as `WSAStartup`.
   An isolated ARM64 fixture reaches the packaged Embree DLL and verifies
   triangle scene commit, scalar and 4/8/16-lane ray hit/miss, and occlusion,
   including Blender's host/device geometry-buffer, traversable, and instance
   API paths.
   A later staged Blender run opened the full Classroom scene and its linked
   asset libraries, then stopped at CRT `feclearexcept` during render setup
   after 1,887,823,912 guest instructions. No frame has been produced.

3. **SIMD, floating point, and atomics for Cycles.** Cycles and Embree need
   AdvSIMD floating point and table lookups, exclusive and ordered accesses,
   and LSE atomics. The reference engine now has tested scalar/vector S/D
   indexed-lane FMUL/FMLA/FMLS forms, plus scalar fused multiply-add, vector
   FP comparisons, ZIP/EXT/XTN rearrangement, SSHLL/USHLL and SHLL/SHLL2
   widening shifts, vector CLZ/CLS and BIC, and
   FADDP/FMINV/FMAXV reductions, saturating narrow operations, scalar
   conditional FP comparisons, FCVT rounding/fixed-point forms, vector
   integer multiply and widening multiply, SHL/SLI/SRI, signed and unsigned
   narrowing shifts, and TRN/UZP/REV64. The reference decoder also covers
   single-lane and interleaved structure loads/stores across element sizes.
   Scalar SIMD&FP-register conversions (`FCVTZS`/`SCVTF` with S/D integer and
   fixed-point operands) have signed and unsigned tests, and `DUP` into a
   64-bit destination may read any lane of the full 128-bit source register.
   These tests exercise selected encodings and edge cases; they do not
   demonstrate that every use in Blender executes correctly. Reciprocal
   estimates and widening pairwise arithmetic have baseline tests; complete
   FPCR-controlled rounding and FPSR exception/trap behavior remain open.
   FP16 conversion forms, SVE/SME state and memory semantics, and full FP
   exception traps remain separate gaps.
   The reservation and bytewise ordered-access model uses a shared lock and
   16-byte-granule reservation epochs for correctness across the first
   guest-worker threads. The 1-, 2-, 4-, and 8-byte
   `LDXR`/`LDAXR`/`STXR`/`STLXR` family uses exact-width accesses, with
   alignment, zero-extension, and reservation tests.
   Pair-exclusive `LDXP`/`LDAXP`/`STXP`/`STLXP` W/X forms have CRT
   opcode fixtures, total-access alignment and all-or-nothing store tests.
   Indexed-element AdvSIMD integer `MUL`/`MLA`/`MLS` H/S forms have
   opcode fixtures and lane edge cases. Non-temporal `LDNP`/`STNP` and
   AdvSIMD `SDOT`/`UDOT` families have architecture-level tests.
   This serializes interpreted instructions and cannot represent parallel
   Cycles throughput. CAS/CASP, SWP, and the LSE add/clear/xor/set/min/max
   families have instruction fixtures and shared-memory reservation tests;
   more concurrency stress and differential validation are still needed.
   Build decode coverage and differential instruction fixtures by family.

   The present 128-bit V-register state cannot represent SVE Z/P/FFR or
   negotiated vector length. Add feature-gated per-thread SVE state, V/Z
   alias rules, predication, and first-fault memory behavior as one subsystem
   before implementing isolated SVE opcodes.

4. **In-house translation and performance.** `ReferenceEngine` interprets one
   instruction at a time with bytewise guest memory access and a default
   ten-million-instruction run limit. That is a correctness oracle, not a route
   to exceptional Blender performance. The first opt-in in-house decoder, IR,
   and x64 block generator now lowers a narrow integer subset, falls back to
   the interpreter, and invalidates after executable-memory changes. Broaden
   instruction lowering, fast memory translation, block caching, and safe
   deoptimization. Establish semantic parity on
   the instruction fixtures and a working render before using the native x64
   Classroom median of 87.466 seconds as a performance comparison. This engine
   seam should not encode Windows ABI assumptions so a future Mach-O/dyld and
   Darwin user-space personality can reuse it on x64 Windows.

5. **Installer and persistent launch.** The official ARM64 Blender MSI is a
   Windows Installer database whose summary template is `x64;1033`, despite
   carrying ARM64 Blender binaries. It has two launch conditions and six custom
   actions. `RegisterBlender` and `UnRegisterBlender` invoke the packaged
   ARM64 `blender-launcher.exe` for file associations. Administrative staging
   bypasses those actions and is not an end-to-end installation. A complete
   Transmute workflow must run or mediate MSI custom actions, register a
   persistent translated launch path, and verify install, repair, uninstall,
   and installed GUI launch without bypassing the ARM64 launcher. A generic
   ARM64 `.exe` installer additionally needs guest child-process creation and
   inherited environment/command-line behavior.

## CRT formatted input

The narrow and UTF-16 `__stdio_common_vsscanf` / `__stdio_common_vswscanf`
bridges marshal ARM64 argument slots into host-owned output buffers. Tests
cover Windows integer widths, floating-point input, scansets, suppression,
literal matching, bounded input, total `%n` counts, secure string capacities,
legacy wide string specifiers, unchanged outputs on mismatch and EOF, and
guest errno. The string bridge limits input, format and field widths to one
million code units and argument consumption to 256 slots. Guest addresses
are checked before result writes and never become host scanf output pointers.
File scanning through `__stdio_common_vfscanf` is still unimplemented.
Passing this family in isolation does not establish successful image output;
the runtime checkpoint remains in `BENCHMARK_STATUS.md`.

## Hybrid CRT interpretation

Seven packaged CRT DLLs are labeled `coff-arm64x` by LLVM and contain
`.a64xrm` plus `.hexpthk` sections, but their **raw COFF Machine is 0xAA64**.
`PeImage::parse` accepts that ARM64 view, and the runtime has executed guest
code from packaged CRT DLLs. The separate `vcruntime140_1.dll` is labeled
`coff-arm64ec` and has raw Machine **0x8664**, which the loader rejects if
loaded; no scanned PE statically imports this file. The hybrid sections and
load-config data deserve focused validation, especially for future dynamic
loads and unwind behavior, but LLVM's `coff-arm64x` label alone does not prove
the currently used ARM64 view is unusable. No ARM64EC support is claimed.

## Acceptance evidence still required

The tiny factory Cube Cycles probe now saves an ARM64 PNG and exits with code
0. All 256 decoded RGBA pixels match its native x64 equivalent exactly.
This is a 16 × 16, one-sample, one-thread compatibility result; it does not
establish full-scene support or acceptable performance.

The audit's next checkpoints are: MSI installation through the intended
Transmute entry point; an installed ARM64 Blender GUI launch; complete
Classroom frame 1 output at the exact native settings; and repeated wall-time
and render-time measurements with hashes, image dimensions, and logs. The
native x64 and tiny ARM64 results are in `BENCHMARK_STATUS.md`. The full
Classroom ARM64 render and its performance comparison remain incomplete.
