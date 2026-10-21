# Blender Classroom benchmark status

The latest implementation and normal timing results are recorded in
[`PERFORMANCE_IMPLEMENTATION_RESULTS.md`](PERFORMANCE_IMPLEMENTATION_RESULTS.md).
The frozen Stage16 build passes 468 named tests plus the ARM64 child-process
check in 14.45 seconds. Its whole tiny process took 88.127 seconds on AC,
versus a fresh native median of 0.997 seconds. Pixels match exactly; the
one-second target remains unmet by about 88x. The parallel source batch
reduces memory refills, implements native REV/EXTR and shortens span guards.
Shared Q admission regressed eight-thread rendering to 210-211 seconds.
An isolated Q-exclusive control restores 58-72-second calls with identical
pixels, so Q owners now stay exclusive while retaining native translation.
Thread scaling, state transfers, memory checks and store bookkeeping remain
major follow-ups. Installer execution and the full Classroom render remain
incomplete. The detailed report contains raw records and measured limitations.

The 30 September performance reassessment is recorded in
[`PERFORMANCE_NEXT_STEPS.md`](PERFORMANCE_NEXT_STEPS.md). Fresh normal tiny
probes took 144.321 seconds for Stage9 and 130.280 seconds for a preserved
Stage8 repeat. The same Stage8 binary earlier took 94.304 seconds, so timing
drift remains material despite recorded AC power and Balanced policy. The
fresh native x64 median is 0.795 seconds across three runs. All preserved
frames match decoded native pixels exactly. No additional Blender speedup is
claimed from these runs.

Normal-path CPU samples now identify separate costs: startup dispatch/cache
lookup, indexed FMLA/FMLS software arithmetic in the render worker, and gate
transitions during saving. A diagnostic main-thread replay reports 283 million
block visits, 46 million native memory misses and three arena reclamations.
Sampling and profiling-counter runs are excluded from normal timing ratios.
The current shared-LSE checkpoint passed 405 named tests plus the child-process
test; new optimization work stopped for this assessment.

ARM64 probes now open the full Classroom scene and its linked assets. The
28 September probe synchronized the scene and entered Cycles kernel loading,
then stopped consuming CPU with all 20 host threads waiting. The run was
terminated after 3,134 seconds without an image. Captured native stacks show
guest event, condition-variable and SRW-lock waits; the cause remains under
investigation. Earlier startup paths stopped at CRT `feclearexcept` and
Blender's `RegGetValueA`, whose bridge families now pass focused tests.
A subsequent ARM64 JIT probe saved a tiny factory Cube frame successfully;
the full Classroom frame is still incomplete. The ARM64 MSI remains
administratively staged rather than installed through Transmute.

For a smaller render probe, use `scripts/probe_blender.ps1 -TinyScene` with
the same staged Blender directory. It renders the factory Cube at 16 × 16,
one sample and one CPU thread. The equivalent native x64 run produced a PNG
in 1.306 seconds. `-DiagnoseSync` reports guest wait arguments and callers,
and surfaces stored worker errors while the main thread is waiting.

On 30 September, the tiny ARM64 JIT probe passed kernel loading, built the
scene BVH and completed sample 1/1. Saving stopped at
`__stdio_common_vsscanf` called by `OpenColorIO_2_4.dll+0x8ba54`; process wall
time was 539.037 seconds. This is a completed Cycles sample without a saved
image, and cannot supply a successful-render performance ratio. A new string
scanning bridge now passes focused tests.

The next probe, logged in
`artifacts/arm64-tiny-cycles-20260930-160355.log`, saved
`artifacts/arm64-tiny-cycles-00001.png` and exited with code 0 after
718.181 seconds. Cycles reached `Finished` at 01:19.73; Blender reported
04:30.73 through saving, including 03:10.82 for saving. All 256 decoded RGBA
pixels match the native 16 × 16 factory Cube PNG exactly. The comparison is
recorded in `artifacts/tiny-cycles-pixel-comparison.json`; PNG file hashes
differ because identical pixels do not imply identical metadata. The earlier
native tiny process took 1.306 seconds. This single ARM64 run is preliminary
compatibility evidence and is much slower than native; it is not a Classroom
performance result.

The completed run executed 2,173,587,415 main-thread guest instructions
through JIT blocks and 458,087,376 through the interpreter. JIT compilation
took 4.211 seconds. Hot fallbacks include already-supported return, address
and branch instructions whose one-instruction blocks were excluded by an
admission policy. That policy is now under focused validation, alongside
further ISA lowering and thread-resource cleanup.

The 718-second result is not acceptable runtime performance. A diagnostic
replay sampled 1,486 active-thread stacks during early startup;
`execute_cached` accounted for 415 leaf samples, with repeated memory
access and locking also prominent. The raw sample report is
`artifacts/blender-startup-cpu-samples.txt`. This replay used an optimized
symbol build with CMake's former `/Ob1` policy, was instrumented, and was
stopped after reaching sample 0/1; it supplies profiling evidence rather than
a comparable end-to-end time. Future symbol builds use `/Ob2` to match
Release inlining.

The completed run's OCIO hot path is `basic_filebuf<char>::uflow`, including
calls to `gptr`, `egptr`, `fgetc` and stack-cookie helpers. The guest CRT
bridge exposed FILE cursor/count addresses without populating their native
read buffer state. A native differential fixture now fails exactly when
the guest cursor/count remain empty after `fgetc` fills the native buffer.
Restoring buffered reads, faster whole-width memory access and repeated
block lookup are the current performance batches. Their impact on a complete
Blender render still requires a fresh run and pixel comparison.

The combined Release batch passed the complete test suite on 30 September,
including buffered CRT native comparisons, cache collision/reentrancy and
concurrent whole-width memory checks. Its next tiny render,
`artifacts/arm64-tiny-cycles-20260930-172706.log`, exited successfully in
260.047 seconds. It used the same 16 × 16, one-sample, one-thread settings and
the same `--jit-stats --diagnose-sync` flags as the 718.181-second run.
All 256 decoded RGBA pixels still match native exactly. This is 2.76× faster
in a single end-to-end comparison, with 63.8% less process time; it remains
unacceptable production performance. Multiple fixes landed together, so this
comparison does not assign a speedup to an individual change.

| Tiny probe | Process wall time | Cycles through Finished | Saving |
| --- | ---: | ---: | ---: |
| Earlier ARM64 JIT with diagnostics | 718.181 s | 79.73 s | 190.82 s |
| Buffered reads, wide memory and front cache | 260.047 s | 46.54 s | 41.86 s |
| Fresh native x64 run | 0.972 s | 0.11 s | 0.09 s |

The native refresh is `artifacts/tiny-cycles-native-current.json`. The ARM64
result and executable hashes are in the JSON beside its probe log, and the
decoded-pixel check is `artifacts/tiny-cycles-optimized-pixel-comparison.json`.
The earlier baseline image and the tested runtime binary are retained in
`artifacts/arm64-tiny-cycles-baseline-718s.png` and
`artifacts/transmute-perf-stage1.exe`. These are individual process timings,
not medians or a full Classroom result. Normal JIT execution without
diagnostic counters is measured separately.

Normal `--jit` execution of the same runtime binary subsequently completed
in 253.060 seconds with exactly matching pixels, logged in
`artifacts/arm64-tiny-cycles-20260930-173840.log`. Cycles reported 58.02 seconds
through `Finished` and 48.72 seconds saving. The total was close to the
diagnostic run, while phase timings varied. These two individual runs do not
establish an instrumentation overhead percentage, but turning diagnostics off
does not resolve the remaining performance problem.

The next normal Release checkpoint completed in 183.059 seconds, logged in
`artifacts/arm64-tiny-cycles-20260930-182629.log`. Its integer JIT expansion,
scoped memory access, bridge bulk copies and buffered putback fixes passed the
full suite before this run. All 256 RGBA pixels again match the fresh native
image exactly. Cycles reported 32.48 seconds through `Finished` and 44.96
seconds saving. This individual process run is 1.38× faster than the prior
253.060-second normal run. Its runtime and image are preserved as
`artifacts/transmute-perf-stage2.exe` and
`artifacts/arm64-tiny-cycles-stage2-normal.png`; the decoded comparison is
`artifacts/tiny-cycles-stage2-pixel-comparison.json`. It is still far from
the requested performance target.

The first ordered read-region and native integer SIMD checkpoint passed all
tests and then completed the same normal tiny probe in 121.882 seconds. The
log is `artifacts/arm64-tiny-cycles-20260930-190059.log`; Cycles reported
26.36 seconds through `Finished`, with 22.31 seconds saving. The preserved
`artifacts/transmute-perf-stage3.exe` produced
`artifacts/arm64-tiny-cycles-stage3-normal.png`, whose 256 RGBA pixels exactly
match native. This individual run is 1.50× faster than the 183.059-second
checkpoint. The new regions retain ordered dependent loads and internal
branches, with checked span tags and precise dynamic instruction budgets.
Native stores and floating-point arithmetic are separate follow-up work.

The next checkpoint adds checked native stores with a bounded commit journal
and guarded scalar/packed floating-point arithmetic. After the full suite
passed, its normal tiny replay completed in 89.743 seconds with exactly
matching native pixels. Cycles reported 22.99 seconds through `Finished`,
with 14.13 seconds saving. The log is
`artifacts/arm64-tiny-cycles-20260930-191554.log`; the runtime, image and pixel
comparison are preserved as `artifacts/transmute-perf-stage4.exe`,
`artifacts/arm64-tiny-cycles-stage4-normal.png` and
`artifacts/tiny-cycles-stage4-pixel-comparison.json`. This individual run is
1.36× faster than the prior 121.882-second checkpoint. The 50–100× target is
still unmet. Persistent translation caching and concurrent guest execution
are not implemented by this checkpoint.

The fifth preserved Release checkpoint adds native scalar/vector memory forms,
cache entry aliases, direct native NaN representations and FMA3 lowering.
After all 366 core tests passed across four isolated process groups in 5.88
seconds, its normal tiny probe completed in 73.722 seconds. The log is
`artifacts/arm64-tiny-cycles-20260930-200451.log`; Cycles reached `Finished`
in 15.15 seconds, and saving took 11.70 seconds. All 256 decoded RGBA pixels
still match native exactly. The executable, image and comparison are
`artifacts/transmute-perf-stage5.exe`,
`artifacts/arm64-tiny-cycles-stage5-normal.png` and
`artifacts/tiny-cycles-stage5-pixel-comparison.json`. This single run is
1.22 times faster than the previous 89.743-second checkpoint. Native tiny
Blender still takes 0.972 seconds, and the requested 2.5–5-second target
remains unmet. Parallel guest execution is a separate subsequent change.

The sixth checkpoint adds concurrent native read regions, floating-point
sessions and AVX2 pairing of eligible adjacent vector operations. All 375 core
tests passed across four isolated processes in 4.39 seconds. Its normal tiny
probe took 78.664 seconds, a 6.7% regression from the fifth checkpoint, with
exactly matching pixels. Cycles reached `Finished` in 16.95 seconds and saving
took 14.50 seconds. The run is
`artifacts/arm64-tiny-cycles-20260930-203238.log`; preserved outputs are
`artifacts/transmute-perf-stage6.exe`,
`artifacts/arm64-tiny-cycles-stage6-normal.png` and
`artifacts/tiny-cycles-stage6-pixel-comparison.json`. The fifth checkpoint
remains the fastest completed Blender run at this point.

Seven alternating measurements of the native thread fixture compare the
preserved fifth and sixth cores. Eight read workers improved from a median
499.58 ms to 88.63 ms, while one read worker changed from 9.85 ms to 10.16 ms.
The eight-worker store control regressed from 666.31 ms to 841.28 ms. Raw
measurements are `artifacts/concurrent-regions-stage5-stage6-comparison.csv`.
This proves simultaneous native read execution and a reduction in contention;
it does not demonstrate near-linear CPU scaling or a Blender speedup. Guest
threads already use native Windows threads. Frequent shared-lease handoffs
and the exclusive store path remain performance work.

The user reported battery operation during this work. Earlier checkpoints
did not record AC status or the active power scheme, so cross-checkpoint
timing ratios also include a possible power-management confound. New Blender
probe records capture power state before and after the timed process. Repeat
the preserved binaries in alternating order under stable power conditions
before attributing a timing difference solely to code.

The parallel test runner names each test before execution and puts independent
groups in separate processes, preserving isolation for process-wide Windows
and CRT settings. A stalled DLL publication fixture exposed a setup race:
the paused loader could take the bridge lock before worker initialization.
Worker setup now completes before the deliberate attach pause, and fixture
waits have deadlines. This test correction does not claim a runtime speedup.

The independent dependent-load fixture in
`tests/fixtures/native_read_loop.s` executes 100 million iterations and
checks its final sum. Three normal runs per checkpoint gave medians of
1.99 seconds before read regions and 0.37 seconds afterward, roughly 5.4×.
Raw times are `artifacts/native-read-loop-stage2.json` and
`artifacts/native-read-loop-regions.json`. This synthetic gain does not
replace the measured Blender result.

The next requested tiny-process target is another 50–100× improvement from
253.060 seconds, approximately 2.5–5 seconds. This is a target, not an achieved
result. Reaching it requires larger compiled regions, retained host registers,
checked native memory access, floating-point/SIMD lowering and removal of
process-wide execution serialization. The reported translation timer excludes
fetch/decode and some allocation work, so it cannot bound the potential cold
translation benefit. AOT alone does not remove measured steady-state
dispatch, bridge and software-arithmetic costs. Keep the full Classroom
installation/render/benchmark gate separate from the tiny performance gate.

The 260.047-second checkpoint's main-thread counters show 1,978,388,625 native guest instructions,
144,898,116 interpreted instructions and 1,111,698,679 native block visits.
Translation itself took 4.878 seconds. The former hot `gptr` entries disappear
from the top ten after restoring CRT buffering, but average native block
length remains only 1.78 guest instructions. Broader integer lowering and
larger checked execution regions are still needed. AOT and persistent-cache
implementation requirements are recorded in `docs/ARCHITECTURE.md`; neither
is currently implemented.

The isolated dispatch fixture alternated seven baseline/new runs, each with
five million visits across sixteen two-instruction blocks and profiling off.
Median time fell from 0.032964 to 0.019884 seconds, 39.7% less time. Raw results
and their summary are `artifacts/jit-dispatch-comparison.json` and
`artifacts/jit-dispatch-summary.json`. This synthetic lookup measurement is
separate from Blender throughput.

On 24 September 2026, native x64 Blender 4.5.14 completed frame 1 of the
official Classroom scene with Cycles on the CPU. The render used the full
1920 × 1080 frame, 16 samples, 8 threads, and PNG output. One warm-up preceded
three measured runs on a 13th Gen Intel Core i7-1355U. The image was inspected
and contains the complete classroom scene.

| Run | Process wall time | Blender render time |
| --- | ---: | ---: |
| Warm-up | 86.312 s | 01:25.37 |
| Measured 1 | 73.998 s | 01:13.16 |
| Measured 2 | 87.466 s | 01:26.74 |
| Measured 3 | 91.707 s | 01:30.96 |
| **Median measured** | **87.466 s** | **01:26.74** |

The native executable SHA-256 is
`57fa1d294ea76448c3bec84ca758caf4629611330abba7dcf55bb0c56a0a15ab`.
The `.blend` file SHA-256 is
`5c526ea3f280566e80253673c9955640527cd0f247ea41b1742620b5bc39f7a4`.
The local machine has the image, log, and JSON report under
`artifacts/native-benchmark-idle/`. The benchmark command is captured in
`scripts/benchmark_classroom.ps1`; it sets Cycles samples and resolution
explicitly, so this is a full scene at 16 samples rather than the scene's
original 300-sample setting. Run-to-run variation on this laptop is material;
report individual times alongside the median.

The ARM64 MSI has been verified and administratively staged for runtime
diagnosis, but it has not been installed through Transmute. The staged ARM64
Blender process now gets through guest PE static TLS initialization,
`OpenImageIO.dll` startup, and the earlier `VCRUNTIME140.dll` vector-load fault.
The latter required page-sized heap mappings because the packaged CRT's
vectorized `memchr` reads beyond a small allocation while remaining within its
mapped page. Recent startup probes exercised vector structure loads, CRT time,
string and formatted-output calls, processor topology, and path APIs. The
latest probe ran the reference interpreter with an eighty-million-instruction
limit. It passed the earlier `usd_ms.dll` budget stop, a 384 MiB sparse
`VirtualAlloc` reservation, process-handle and CRT descriptor calls, and an
`LDPSW` stop in `oslexec.dll`. It reached the packaged `embree4.dll` and
passed a scalar `FMADD` stop there. Focused tests now cover indexed-lane
floating-point arithmetic, vector comparisons, widening integer arithmetic,
modified immediates, shifts, reciprocal estimates, saturating narrows,
permutations, and conditional floating-point comparison. Recent CRT and path
bridge additions passed `_set_app_type`, `_set_fmode`, locale setup,
`SetUnhandledExceptionFilter`, `_set_new_mode`, and `GetSystemDirectoryA`.
Subsequent probes passed guest thread-local exit callback registration,
guest-owned `argc`/`argv` accessors, `SHELL32!CommandLineToArgvW`, CRT integer
parsing and file-status calls, and `KERNEL32!SetThreadAffinityMask`. The
latest GUI probe passed `USER32.dll!RegisterClassW`, guest-memory CRT `qsort`
with an ARM64 comparator callback, CRT `_wsopen_dispatch`, and `_waccess`.
An earlier run hit a non-code return fault at Blender `0x1406b5f8c`; that
interpreter fault is now fixed; the latest GUI run reaches
`ole32.dll!CoCreateInstance` at step 205,994,613. The run also
passed `LoadIconA` for Blender's named `APPICON` group resource, stock cursor
loading, and GDI stock-object setup.
Other
recent probes passed CRT `rand_s`, scalar fixed-point `UCVTF`, file
enumeration, process DPI setup, keyboard layout, OLE initialization, and
common-controls setup. Raw-input registration has focused tests, but the
latest probe stopped before exercising its new bridge. The call path differs
between builds and needs continued validation.
A separate headless run with the Classroom scene and intended render arguments
printed the Blender 4.5.14 banner, passed an OpenImageIO worker `YIELD` hint,
guest-memory CRT `qsort`, `SHELL32.dll!SHGetSpecialFolderPathW`, and CRT
`_control87`. A targeted trace found that `AND SP, x9, #...c0` did not update
SP because the logical-immediate decoder treated register 31 as XZR for all
operations. A later ordinary store overwrote saved `x29`/`x30`, causing the
non-code return. The decoder now writes SP for non-flag AND/ORR/EOR immediate,
with a regression using Blender's instruction sequence. The latest headless
probe passed that site and stopped on `XPACLRI` (`0xd50320ff`) in
`VCRUNTIME140.dll` at run step 147,364,236. The PAuth-disabled guest now
handles that hint explicitly; the latest headless run stops at UCRT `_mktemp_s`
at run step 192,415,823. After the temporary-name bridge, it stops at
`KERNEL32.dll!CreateDirectoryW` at run step 192,419,327. A directory/path
bridge passes focused tests; the next headless probe stopped at
`KERNEL32.dll!GetLogicalDrives` at run step 192,453,483. The drive bridge
passed focused tests; the latest headless probe stops at
`SHELL32.dll!SHGetDesktopFolder` at run step 192,453,548. The caller then
uses `IShellFolder::ParseDisplayName` and `GetDisplayNameOf`; a typed guest
COM proxy for these calls passed focused tests. A subsequent staged run passed
that path and stopped at `ole32.dll!CoCreateInstance` for the Shell automation
`IShellDispatch` interface at run step 192,270,323. A typed Shell automation
proxy now passes that call, Quick Access `Folder.Items`, item count, indexed
items, `IsFolder`, and `Path` in a staged run. The next headless stop is UCRT
`_wputenv_s` at run step 193,298,340. Separate guest-owned Win32 and CRT
environment views now pass that path; a later staged run stopped at
`bcrypt.dll!BCryptGenRandom` at run step 193,309,590. A typed CNG
random/provider bridge passed that path; the latest staged headless run stops
at CRT `__fpe_flt_rounds` at run step 198,157,168. A bridge maps the guest
FPCR rounding mode to the CRT result; the latest staged run passed it and
stopped at CRT `_wcsicmp` at run step 201,151,834. A case-insensitive CRT
string comparison batch passed that import; the latest staged run stopped at
`PathCchSkipRoot` at run step 201,191,484. A five-function path batch passed
that point; the following run stopped at `GetTimeZoneInformation` at run step
204,321,740. A typed Win32 time and time-zone batch passed that path; the
next staged run stopped at `KERNEL32.dll!FormatMessageW` at run step
204,695,216. A typed formatting bridge passed that import; the latest run
stopped at `KERNEL32.dll!GetFileInformationByHandle` at run step 204,697,079.
A file-metadata bridge passed it. The following stop was CRT `_dclass` at
204,799,370; an FP classification bridge passed that too. The next observed
import was `KERNEL32.dll!GetFullPathNameW` at 206,592,088, and its A/W path
bridge now passes focused tests. A later headless run printed Blender's banner
and started about twenty guest worker threads, then the host terminated with
Windows fail-fast status `0xc0000409` before guest main returned. A debugger
located the host CRT invalid-parameter call inside the `_wopen` bridge: it
forwarded an unused guest mode register to secure `_wsopen_s`. With that
corrected, one run reached UCRT `__sys_nerr` at run step 206,623,154.
A tested CRT error-table bridge passes that accessor in isolation. The next
staged run stopped at UCRT `_set_thread_local_invalid_parameter_handler` at
206,618,391. Tested bridges for the CRT error and invalid-parameter families
passed those calls. The next run reached `KERNEL32.dll!MoveFileExW` at
219,539,850. These startup paths are not a monotonic progress measure. No
run produced a frame. A typed file move/copy/delete/link batch then passed
`MoveFileExW`; the next staged stop was `KERNEL32.dll!GetVersionExW` at
239,295,695 guest instructions. A version-information bridge and `DC ZVA`
instruction support passed subsequent runs. A corrected ninth-argument ABI
for `LCMapStringEx` and `CompareStringEx` cleared a Python site-module import
failure. Narrow and wide CRT formatted-output bridges then allowed a staged
run to reach its 500-million-instruction limit inside `python311.dll` after
94.6 seconds. The process printed a `sitecustomize` WinError 127 warning,
which a targeted trace identified as a failed dynamic lookup of
`AddDllDirectory` through the synthetic KERNEL32 handle. A typed dynamic
lookup and directory-cookie bridge cleared that warning in subsequent
260-million- and 500-million-instruction replays. The latter reached only its
instruction cap in `python311.dll` after about 81 seconds. No frame was
produced. A subsequent one-billion-instruction interpreter probe reached
`python311.dll+0x1d4ec0` after 173.97 seconds, again without a new API or
instruction failure and without a frame. A two-billion-instruction probe
reached `python311.dll+0x2102ec` after 325.55 seconds with the same outcome.
With a four-billion limit, Blender reached `KERNEL32.dll!CreatePipe` at
3,141,395,641 guest instructions after 528.21 seconds. The bridge did not yet
support that import, so this run stopped before scene loading or a frame.
After the typed pipe and handle bridge passed focused tests, a second
four-billion-limit replay followed a different startup path and stopped at
`KERNEL32.dll!InitializeProcThreadAttributeList` after 1,218,442,988 guest
instructions and 245.58 seconds. This replay did not reach `CreatePipe`, so
its staged behavior is still unverified. Neither replay produced a frame.
A later replay with the typed process-attribute bridge took another startup
path and stopped at `WS2_32.dll!#115` (`WSAStartup`) after 1,154,106,259 guest
instructions and 219.41 seconds. A typed Winsock bridge subsequently passed
focused startup, socket I/O, and resolver tests.
With typed Winsock startup, socket I/O, computer-name queries, and ARM64 child
launch integrated, the next replay stopped at `ADVAPI32.dll!RegOpenKeyExW`,
called from `python311.dll+0x2072e0`, after 1,168,263,110 guest instructions
and 195.81 seconds. The typed registry bridge now passes focused tests for
all 18 registry imports in `python311.dll`.
The next four-billion-instruction replay opened `classroom.blend`, read its
linked asset libraries, and advanced into render setup. It then stopped at
`api-ms-win-crt-runtime-l1-1-0.dll!feclearexcept`, called from
`blender.exe+0x17a51fc`, after 1,887,823,912 guest instructions and 338.99
seconds. This was an observed compatibility gap. The printed `Read blend`
and linked-library messages establish scene loading, not a completed render.
After the CRT floating-environment bridge passed focused tests, a replay took
a different path and stopped at `ADVAPI32.dll!RegGetValueA`, called from
`blender.exe+0x2498290`, after 1,670,315,068 main-run guest instructions and
900.27 seconds. The source for a typed `RegGetValueA/W` bridge was already
prepared in parallel but was not in that executable. This replay does not
prove that `feclearexcept` passed dynamically; it may not have reached it.
These are startup and scene-loading probes, not render times. The guest signal,
worker-thread, and mapped-file paths have focused tests, but the full Cycles
frame has not completed.
There is no ARM64 render or translation throughput result yet, so no
x64-versus-ARM64 ratio is reported.
An isolated 600,002-instruction integer-loop fixture took 0.0410 seconds in
the reference interpreter and 0.0031 seconds with the revised opt-in x64 JIT
(about 13 times faster on this run). The JIT executed 600,001 of those
instructions natively. A staged headless Blender startup using `--jit` reached
the same `_control87` boundary in 52.65 seconds, while the prior interpreter
invocation took about 35.5 seconds; these are single, uncontrolled startup
measurements, not render times. The narrow JIT is not yet a Blender speedup.
One staged headless `--jit --jit-stats` startup reached the same `_mktemp_s`
boundary at about 192.4 million guest instructions. It compiled 36,137
blocks and counted 8,044,642 native guest instructions and 211,911,494
interpreter fallback instructions across the main engine's lifetime. Native
execution is about 3.7% of those cumulative instruction counters. The engine
also runs DLL initialization and nested guest callbacks, so its counters exceed
the local 192.4-million main-run step count; guest workers have separate
counters. This is an engine diagnostic, not full application coverage. Frequent
load/store and conditional
operations still split short native blocks; more lowering and fast guest
memory access are needed before a render performance comparison.
After W/X unsigned-offset load/store lowering, a 100-million-main-instruction
Blender startup diagnostic counted 71.80 million native and 55.65 million
fallback instructions over the engine lifetime, versus 52.26 million and
75.19 million at the same stop before that batch. Native share rose from
41.0% to 56.3%. The two single-run wall times were 51.51 and 45.18 seconds;
they do not establish a controlled speedup, and neither run reached a render.
The mapped-memory JIT fast path passed permission, remapping, cross-region,
and executable-write tests. In a short 200,000-iteration byte-memory loop,
JIT load time fell from 3.48–3.96 to 2.82–2.90 milliseconds and store time
from 5.14–5.61 to 4.29–4.57 milliseconds. These diagnostic runs do not
measure Blender throughput; the executable has not yet been replayed with
this memory change.
A later diagnostic after flag-setting arithmetic and conditional-branch lowering
counted 59,103 compiled blocks, 52,697,705 native instructions and 167,295,872
fallback instructions across the main engine's lifetime, or about 24.0% native
by these counters. This has not been timed as a complete render.
A subsequent headless `--jit --jit-stats` run with conditional-select and
logical-immediate lowering reached the same Shell automation import. It took
75.039 seconds and counted 83,064 compiled blocks, 76,681,956 native guest
instructions, and 143,128,464 fallback instructions over the main engine's
lifetime (about 34.9% native). The corresponding interpreter run reached that
import in roughly 42 seconds, without a controlled performance protocol.
The JIT's most frequent blocks still contain only two or three guest
instructions; startup speed has not improved despite broader native coverage.
Bounded chaining of already-compiled blocks improved the isolated 600,002-step
loop to 0.000987 seconds in the JIT versus 0.041597 seconds in the interpreter
(about 42 times faster in that one run). A staged Blender startup comparison
after chaining remains pending; the small-loop result is not a render speedup.
A staged headless run after chaining still took 73.834 seconds with `--jit`
to reach `BCryptGenRandom`, versus 48.762 seconds in one interpreter run.
These are uncontrolled startup timings. The JIT counted 84,551 compiled
blocks, 81,015,765 native guest instructions, and 139,831,403 fallback
instructions across the main engine's lifetime (about 36.7% native). Cold
code allocation and short block returns remain performance concerns.
Instrumentation of a later staged JIT run through `_wcsicmp` measured 88,801
compiled blocks containing 7,143,661 emitted code bytes but consuming
363,728,896 committed bytes because each block then occupied at least
one 4 KiB page. Cumulative compilation and W^X protection time was only
0.389 seconds, so code pooling primarily addressed memory footprint;
it had not been shown to resolve the startup slowdown. This run counted
83.88 million native versus 144.82 million fallback guest instructions over
the main engine's lifetime, about 36.7% native.
A subsequent 64 KiB chunked W^X code arena reached `PathCchSkipRoot` after
88,839 compiled blocks. It emitted 7,147,645 code bytes and committed
7,864,320 bytes in 120 chunks, approximately 98% less committed code memory
than the earlier page-per-block measurement. Compilation plus page-protection
time was 0.367 seconds. This uncontrolled startup run took 78.20 seconds,
while the earlier measurement stopped at a different import, so it does not
show a speedup or slowdown. The synthetic loop result and these code-cache
figures are not Blender render benchmarks.
A bounded cache of PCs that repeatedly fall back to the interpreter reached
`GetFileInformationByHandle` in a staged JIT run after 90,037 compiled blocks.
It recorded 22,963,891 negative-cache hits and used about 8.0 MiB of live
generated-code storage for 7.26 million emitted bytes. The run took 84.21
seconds; a separate interpreter run reached that same import in 51.93 seconds.
Both are single uncontrolled startup measurements, and the JIT remains slower
in this path. Skipping repeated JIT decode attempts is verified, but its
performance benefit is unproven. Sampling a later staged run at a
100-million main-instruction limit found a hot Blender string loop repeatedly
falling back on shifted and extended register ADD forms. The in-house JIT now
has differential fixtures for that ADD/SUB family; no end-to-end Blender
speedup has been measured from it.

On 30 September, the in-house JIT gained signed byte loads to W/X and the
16-bit load/store family, with shared decoding for valid integer imm12,
unscaled imm9, pre/post writeback and register-offset accesses. Differential
tests cover 156 size/access/address combinations plus aliases, SP/ZR,
permission and boundary faults, partial-write fallback and executable STRH
invalidation. Whole-width stores now use the mapped-memory contract directly,
avoiding a second region lookup. The exact sampled Blender LDRSB/CBNZ scan
sequence completed 400,001 steps in a median 57.783 ms in the interpreter and
6.972 ms in the JIT over three runs, about 8.29 times faster in this synthetic
case. Raw measurements are in `artifacts/jit-memory-family-benchmark.log`.

The LSLV/LSRV/ASRV/RORV W/X family now lowers to x64 shifts and rotations.
Tests cover 3,328 dynamic operand/count/register cases, all NZCV combinations,
ZR and source/destination aliases, W upper clearing, preserved vector/FP
state and cached/runtime instruction budgets. A loop containing the exact
sampled `0x9ac82128` and `0x9ad82128` words completed 800,001 steps in a median
62.061 ms in the interpreter and 1.462 ms in the JIT over three runs. Individual
JIT times ranged from 1.319 to 4.040 ms; the ratio of medians is about 42.44.
Raw measurements are in `artifacts/jit-variable-shift-benchmark.log`.
These timings preceded the later policy change admitting supported single
instructions. Both batches pass the full Release suite. They establish loop
correctness and isolated throughput; they do not measure Blender startup,
saving or Cycles performance.

The successful tiny render's fallback profile included supported lone RET,
ADRP, ADD and CBZ instructions. The runtime previously admitted a decoded
single instruction only for selected memory operations. It now admits every
nonempty supported block, preserving the unsupported-instruction fallback and
instruction budgets. Focused tests use the exact sampled words, MSR/BL
separators, repeated helper returns and ten budget boundaries.
`tests/fixtures/jit_singleton_loop_arm64.s` provides a separate 3,200,007-step
process probe. On the same fixture, native steps rose from 8 to 1,600,007,
while interpreter steps fell from 3,199,999 to 1,600,000. Three process times
before the policy change were 0.3341752, 0.2940740 and 0.3016765 seconds;
afterward they were 0.2404029, 0.1801278 and 0.1850013 seconds. The medians
are 0.3016765 and 0.1850013 seconds, about 38.7% less time in this synthetic
probe. Raw timings and counters are in `artifacts/jit-singleton-*`.
This verifies the policy cost for supported prefixes. Later process replays
combine this policy with additional translator and runtime changes.

The subsequent integer batch lowers MOVN W/X, signed and merging bitfields,
CCMP/CCMN register and immediate forms, MADD/MSUB, signed/unsigned widening
multiply-add/subtract, high multiplies and direct BL. It also admits the exact
PAC/AUT hints already treated as no-ops by the reference engine's disabled
pointer-authentication policy. The differential matrix covers 28,851 operand,
encoding and condition cases, including every valid bitfield immediate pair,
register aliases, zero registers, NZCV, link-register behavior and budgets.
Adjacent hints, barriers and authenticated branches remain outside that policy.

A virtual-memory reentry regression demonstrated that invalidating all code
inside a pre-call memory callback could free the currently executing block.
The cache now retains code only for linked active invocations and retires it
after the final caller returns. An invalidation epoch also prevents an outer
call from replacing a newer nested code address. All/range invalidation,
colliding replacement compilation and direct/cached entry tests pass. Normal
hits allocate nothing and require no atomic reference count.

The first ordered native-read region batch removes the first-memory split
within dependent loads and read loops. Native code checks a lock-owned span
table before host dereferences, exits before a failing read, refills metadata
only after returning, and resumes at the current instruction without replaying
the completed prefix. Dynamic counts preserve instruction limits and bound
each invocation to 256 steps. Integer SIMD fragments use baseline SSE2 without
helper calls. The Release suite passed in 6.84 seconds, including 198 integer
load forms, dependent reads, every tested budget boundary, mapped lifetime and
pair-fault commit checks. A 400-million-step dependent-read process fixture
fell from a median 1.987 to 0.368 seconds, about 5.4 times faster. This isolated
result is separate from the actual tiny-render measurements above and does
not establish performance for a complete demo scene.

Native regions now include ordered writes to owned non-executable memory.
Each successful element appends a bounded journal record, and the runtime
commits that journal immediately after every native return, before refill,
checked access or instruction-limit reporting. Commit advances dirty identity
and invalidates all touched exclusive granules. Image, external, executable
and incomplete spans retain checked fallback and its partial-fault behavior.
The vector memory batch covers scalar B/H/S/D/Q accesses and D/Q pairs with
immediate, literal, register-offset and writeback addressing. Whole-span
guards precede both pair effects. A Q store deliberately uses one 16-byte
dirty record per destination, and a Q pair uses two, normalizing the reference
engine's differing byte/width call counts under its monotonic dirty-identity
contract. Integer store record-count semantics remain unchanged.

Every instruction inside a native region now has a cached entry alias, so
yielding or returning into an already compiled suffix reuses its owning code.
Invalidation checks the owner's complete guest-byte extent and clears its
aliases, including aliases whose own PC lies outside the changed prefix.
Focused tests cover prefix writes and unmap/remap replacement. The live code
arena remains bounded at 64 MiB; increasing that limit has not yet been tested.

Native FP uses the explicit host NaN-representation policy accepted for this
runtime. Ordinary arithmetic retains guest rounding and sticky exception
semantics while NaN sign, payload and DN representation follow the host.
Fused arithmetic uses FMA3 only when CPU and OS state support it. Trap-enable
and rare FZ/tininess cases retain checked fallback, and data-dependent exits
do not permanently mark a structurally supported PC as unsupported. Stage5
passed all 366 Release tests across four isolated shards in 5.88 seconds,
including vector memory, alias ownership, FP/FMA and warm fallback transitions.

FP regions now retain the guest MXCSR session across accepted arithmetic and
comparison instructions, restoring the exact caller state at every budget,
memory, branch and fallback exit. Only flags from committed instructions reach
FPSR; a failing instruction cannot publish its exception flags. Independent
adjacent Q1 integer SIMD pairs can use feature-gated AVX2, while their second
instruction retains its own alias and one-step budget behavior.

Stage7 groups at most eight consecutive nonfaulting register operations under
one suffix budget check. Memory, guarded FP, control transfers and callbacks
remain outside these spans. Every alias and internal branch checks its own
remaining suffix before effects, and short budgets resume through the checked
path. Exhaustive budgets for a 71-step mixed loop and a fault after its pure
prefix pass. All 385 Release tests passed in 10.33 seconds. Process timings
across different power states are provisional; the battery-power report
prevents treating earlier cross-stage ratios as controlled measurements.

Stage8 added all 160 single-element LSE operation, width and ordering forms to
native regions. The adapter validates readable and writable owned spans,
identical resolved backing and natural guest/host alignment before the locked
operation. CAS mismatch returns the old value without a dirty record; successful
equal-value writes still publish dirty identity and invalidate reservations.
Aligned scalar native loads and stores can now retain shared execution leases,
with short journal-publication locks. Misaligned guest or host spans return
before the current effect and retry exclusively. The 402-test Release checkpoint
passed in 14.23 seconds; shared LSE admission is a subsequent checkpoint.

The first guest-thread implementation serializes interpreted instructions
through the shared address-space lock; it is a correctness step, not a
performance baseline for Cycles.
Worker DLL and TLS attach/detach notifications now pass focused ordering
tests. A 1,050-worker churn test checks resource cleanup and slot reuse, and
worker translation failures are published before potentially blocking guest
cleanup. These tests do not establish complete multithreaded Blender behavior
or process-wide cancellation of failed workers.
An isolated ARM64 fixture calls the packaged `embree4.dll` directly. It now
creates and commits a triangle scene, verifies scalar and 4/8/16-lane ray hits
and misses, and verifies occlusion and clear-path queries. A Blender-style path
also uses host/device geometry buffers, explicit geometry IDs, traversable
queries, and a transformed instance scene. Interpreter runs with one thread
and Embree's default worker pool,
plus a single-thread JIT run, exited successfully. The same fixture passes
against native x64 Embree.
The larger 32-by-32 triangle grid also passes on the ARM64 interpreter with
the default worker pool, including a progress callback and an instanced
scene. The native comparison fixture now uses normal C runtime startup;
with that entry point, its equivalent grid and callback checks passed 20
consecutive runs. The earlier no-CRT native fixture sometimes crashed in
`sycl8.dll`, so its results were not a reliable baseline.
A 64-by-64 grid with the same packet, instance, and callback checks also
passed through the in-house JIT and the native x64 fixture.
An extended imported-API fixture passes native x64, ARM64 interpreter, and
ARM64 JIT runs. It exercises shared triangle storage, a flat Bezier curve,
quaternion instancing, and device callback registration. The memory callback
is observed; the error callback is registered but not deliberately triggered.
This is narrower than Blender's Cycles workload. A fixed bridge registration
race prevents the earlier host access violation during worker teardown;
orderly guest DLL detach and TBB pool shutdown remain incomplete.
The ARM64 benchmark must use the same Blender version, scene file, frame,
samples, resolution, CPU device, and thread count after the Windows runtime
can launch Blender reliably.
