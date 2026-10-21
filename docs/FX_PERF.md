# fx performance: suite, method and hot list

The fx engine runs plain translated arm64 code at about 20 % of native in Chrome: Speedometer 3 is about 6 % of
native with V8's JIT and 16–20 % jitless, and JetStream about 17 % (`ALCHEMY_CHROME_AUDIT.md`). Good translators
reach 50–80 %. This page is the shared measuring stick for making translated code faster in general: the perf suite
every change is measured with, how to profile, and the ranked list of where translated code spends its time.

## The perf suite

Everything is in `scripts/perf/`:

| File | What it does |
| --- | --- |
| `fx_ab.ps1` | Interleaved A/B runs of two alchemy-fx builds over the benchmarks below; writes `samples.csv` and `summary.md`. |
| `summarize.py` | Medians, spread, speedup, paired per-round speedup and a bootstrap 95 % interval per metric. |
| `build_fixtures.ps1` | Builds the guest fixtures: `fxbench` (arm64 Mach-O, LSE), `fxbench_llsc` (ARMv8.0, LL/SC atomics) and `fxbench_native.exe` (x86-64, the same C source). |
| `fixtures/fxbench.c` | The guest microbenchmarks (below). |
| `pages/raf_box.html` | The frame-rate page: a box moved by `requestAnimationFrame` plus canvas drawing. |
| `profile_mix.py` | Splits an fx sampling profile into host and translated-code buckets. |
| `startup.ps1` | Blender.app startup timeline (unchanged; the wave rules' windowed Blender check). |

### Running it

```powershell
. scripts/dev/build_env.ps1
scripts/perf/build_fixtures.ps1                       # once: <checkout>\build-perf-fixtures
scripts/perf/fx_ab.ps1 -A <main build dir> -B <your build dir> -LabelA main -LabelB mine -Out <scratch>\ab -Bench quick -Rounds 5
scripts/perf/fx_ab.ps1 ... -Bench cycles,plugin,qlab,blender -Rounds 3          # apps
scripts/perf/fx_ab.ps1 ... -Bench sp-jitless,js-jitless,js-jit,fps -Rounds 3   # Chrome (network: see below)
```

`-A`/`-B` take a build directory (its `Release\alchemy-fx.exe` and `fx_translator_tests.exe`) or an
`alchemy-fx.exe` (a `fx_translator_tests.exe` next to it is used too).

| `-Bench` | What | Metric |
| --- | --- | --- |
| `micro` | fxbench's kernels (below), translated, pinned to two P-cores (affinity 0x5), high priority and QoS; `fxbench_llsc` for the two atomics kernels | ns per iteration, median of `-MicroReps` in-process repetitions |
| `fxunit` | `fx_translator_tests --filter core_perf_`: hand-encoded LCG, memory and call loops in-process, and translation speed | ns per iteration; thread cycles per 12-instruction region |
| `blender` | Blender.app `-b --factory-startup` plus one operator | process CPU and wall seconds |
| `cycles` | Blender.app headless Cycles render of the factory cube, 320x180, 16 samples, 2 threads | render seconds (`time.perf_counter` around `render()`) |
| `plugin` | a Blender renderer plug-in's headless cube render on a scratch copy of its Alchemy root (`-PluginRoot`, `-PluginScript`, or `ALCHEMY_PERF_PLUGIN_ROOT`/`_SCRIPT`) | "render returned in" seconds, plug-in enable seconds |
| `qlab` | QLab 5 to its first frame on a scratch copy of `alchemy-root` (`ALCHEMY_PERF`) | "first frame presented" ms |
| `sp-jitless`, `sp-jit` | Speedometer 3.0 (browserbench.org), `-SpeedometerIterations` (1), software compositor | score |
| `js-jitless`, `js-jit` | JetStream 2.2, one fresh browser per test of `-JetStreamTests` (richards, delta-blue, crypto, navier-stokes, splay, Box2D) | score per test and their geometric mean |
| `fps`, `fps-sw` | `pages/raf_box.html`, 20 s, SwiftShader GPU process / software compositor | rAF callbacks per second |
| `quick` = micro, fxunit; `all` = everything | | |

### Method

* **Frozen binaries.** Both sides are copied into `<Out>\bin-<label>` (with the pdb and test exe) and run from
  there; the SHA-256 is logged.
* **Interleaving.** Every round runs every benchmark on both sides, A first on odd rounds and B first on even ones.
  Round 0 is a warm-up and is not counted.
* **Separate fx caches.** Each side gets `TRANSMUTE_FX_CACHE_DIR=<Out>\cache-<label>`. The persistent cache key is
  the format and ABI versions, the translate options and the family names: it does *not* change when the emitter's
  output changes, so with one shared cache a build runs the other build's cached translations. The same holds
  for any manual test of a codegen change: use your own `TRANSMUTE_FX_CACHE_DIR` or `ALCHEMY_FX_CACHE=0`.
  The integration bumps `kTranslatorAbiVersion` once per landing.
* **Quiet machine.** Before each sample: no `cl`, `link`, `lld-link`, `ninja` or MSBuild process, and CPU load under
  `-MaxLoad` (25 %) on three samples 2 s apart, waiting up to `-QuietMinutes` (30); the load is stored with the
  sample (`busy:` when it measured anyway). While a sample runs, `<LockDir>\perf.lock` (`-LockDir` or `ALCHEMY_PERF_LOCK_DIR`) holds new builds back
  (the wave's `build_locked.ps1` waits for it).
* **Measurement windows.** The shared machine never goes quiet by itself, so the lead opens windows on purpose:
  `scripts/perf/window.ps1 -Minutes 40` holds `perf.lock` and writes `<LockDir>\window.md`. While a window is
  open, sessions start no builds (`build_locked.ps1` waits by itself) and no guest app runs, and let running ones
  finish. fx_ab.ps1 inside a window leaves the window's lock alone and still waits, sample by sample, for no compiler,
  linker or other alchemy-fx process and low load. One or two 30–45 minute windows every few hours measure the
  integration rounds.
* **Hybrid CPU.** The i7-1355U has 2 P-cores (logical 0–3) and 8 E-cores. The micro kernels are pinned to logical 0
  and 2; everything runs at high priority with HighQoS (`scripts/fx_bench_control.ps1`). App benchmarks are not
  pinned.
* **Statistics.** Speedup is A/B for times and B/A for scores, so above 1 means B is faster. "Paired" is the median of
  the per-round speedups, which cancels slow drift. The verdict comes from a bootstrap 95 % interval of the speedup
  of the medians: "faster"/"slower" only when it excludes 1.
* **Correctness.** Every fxbench kernel returns a checksum, compared with the native x64 build of the same source;
  a mismatch is stored with status `checksum` and reported. A faster but wrong change is rejected. The reference
  interpreter (`fx_oracle`) and the differential and fuzz tests stay the correctness bar:
  `ctest -R "^(alchemy_|fx_)"`, which includes `core_fuzz_*` in `fx_translator_tests`.
* **Guest runs** are cloaked with `ALCHEMY_NETWORK=loopback`, except Chrome: `web_visit.ps1` sets
  `ALCHEMY_NETWORK=full` with the update, crash-upload, metrics and GCM deny list (also as `--host-resolver-rules`) and
  logs every connection; audit `network-denied.txt` in each run directory.

### fxbench kernels

One C source (`fixtures/fxbench.c`), built for arm64 macOS with `-O2 -fno-builtin -ffp-contract=off` and
`-mcpu=apple-m1`, and for x86-64 with `-march=x86-64-v3`. Native ratios are indicative: the two compilers emit
different code (clang's arm64 binary search uses `csel` where its x64 one branches, so `branchy` runs faster
translated than native).

| Kernel | What it stresses |
| --- | --- |
| `int_hash` | xxhash64-style: 64-bit multiply, rotate, unaligned loads |
| `int_mix` | flags: add/sub overflow checks, compare chains, csel, 32-bit ops, udiv, bfi/ubfx, clz/ctz, rev |
| `branchy` | unpredictable branches: binary searches, insertion sort |
| `interp_switch` | a bytecode interpreter with switch dispatch (one jump table) |
| `interp_goto` | the same with computed goto: an indirect branch per handler, as V8's Ignition dispatches |
| `indirect_calls` | blr through a table of 8 targets, and vtable calls |
| `call_ret` | recursion (fib): bl/ret, frames, the return stack |
| `mem_own` | the guest's own copy, compare, strlen and fill loops |
| `libc_calls` | imported memcpy, memcmp, memset, strlen: Alchemy's synthetic host calls |
| `ptr_chase` | a shuffled linked list: dependent loads, field updates |
| `fp_scalar` | double: sqrt, divide, fma, compares |
| `fp_float` | float: fma, fmin/fmax, conversions |
| `vec_portable` | GCC vector extensions: f32x4 saxpy/dot, i16x8 multiply-add, u8x16 min/max/average, shuffles |
| `neon_intrin` | tbl, saturating and widening arithmetic, pairwise adds, ld2/st2, cnt (native runs scalar code) |
| `atomics` | LSE (or LL/SC in `fxbench_llsc`): fetch-add, CAS loops, swap, acquire/release, fences |
| `atomics_contended` | 4 threads: a shared counter and a CAS spin lock |

## Profiling

* **fx sampler:** `TRANSMUTE_FX_SAMPLE=<file|1>[,<interval_us>][,main][,stacks][,delay=s][,duration=s]`
  (`docs/FX_ENGINE.md`, "Profiling"). `1` writes `%TEMP%\fx-profile-<pid>.txt`; for Chrome pass `TEMP` in
  `web_visit.ps1 -Env` to collect one profile per process. The sampler loads symbols first (a few seconds), so a
  run shorter than that gets no samples; give short runs `-scale`.
* **Stats:** `ALCHEMY_FX_STATS=1` prints translations, cache use and exits by reason at exit.
* **Buckets:** `python -I scripts/perf/profile_mix.py <profile.txt> [--image <guest Mach-O>]` groups host symbols
  (waits, synthetic calls into Alchemy, providers, dispatcher, translation, ...) and classifies the sampled host
  instructions of the 24 dumped hot regions: context access (r15-relative: guest registers kept in memory, spills,
  reloads), flag handling, atomics, indirect branches and exits, direct branches, guest memory operands, SIMD and FP,
  other integer. For guest fixtures `--image` names regions by symbol. Percentages are sample locations, not
  removable time. Guest apps are never disassembled: only host code is.
* The profile counts every thread; Chrome and Blender keep many threads waiting. Read the per-thread table and
  divide by the busy thread's share.

## Hot list (2026-10-20, main ebcedb1)

Profiles on main, machine loaded by other sessions' builds (shares are robust to that; times are not):
Speedometer 3 jitless (1 iteration, renderer main thread), Blender Cycles render (64 samples, 2 threads) and the
fxbench kernels. Raw data: the lead's scratchpad (`prof-*`) and the shared notes (`fx-perf/`).

**Where the busy threads are.** The Speedometer renderer's main thread: 36.5 % translated, 14.6 % host, 48.4 %
waiting. Cycles' render threads: almost all translated. fxbench: 60 % translated, the rest the synthetic calls of
`libc_calls` and thread waits.

| # | Cost | Evidence | Owner |
| --- | --- | --- | --- |
| 1 | **Guest registers in the context (r15): spills, reloads, flushes** | 27–30 % of sampled translated instructions are r15-relative in the renderer, Cycles and the fixtures (interp_goto 36 %, indirect_calls 28 %, call targets 35–48 %, fib 35 %). Hot: stores of x registers at region exits and before calls, reloads at entries. | p2 (allocation inside regions: region_alloc, region_emitter); p1 (what crosses calls, returns and links) |
| 2 | **Code density and hot footprint** | The renderer's hottest 90 % of samples are spread over 1,612 regions, 17.7 MB of host code (5.3× the guest bytes; 3.6× for the sampled parts); 50 % over 161 regions. Cycles 4–5×. A flat profile like this is front-end bound: every byte per guest instruction counts. | all groups |
| 3 | **Region entry and back-edge interrupt checks** | `cmp dword [r15+0xac],0; jne` is among the most sampled instructions in every profile (region entries r4+0, r11+0, r16+0 in the renderer; loop heads in fxbench). | p1 |
| 4 | **Synthetic host calls (libSystem, libc, Objective-C providers)** | `libc_calls`: about 90 ns per imported memcpy/memcmp/memset/strlen against 7 native (12.7×). Per call: `load_state`/`store_state` copy 31 x and 32 v registers each way unless the slot is integer-only, then a `std::function` handler, a `shared_ptr<ThreadContext>` copy and guarded copies. | proposal: p1 (stub side) with the Alchemy synthetic-call path in `src/alchemy/fx/fx_engine.cpp`, coordinated with chrome-real-web (which changed it in 81096de) |
| 5 | **Invalidation walks shared-memory aliases page by page** | `rt::shared_memory_aliases`, about 6 % of the renderer main thread: `invalidate` (regions.cpp:616) calls it for every page of a range up to 64 MB, each a shared lock and a map lookup. | fx-jit-cost |
| 6 | **Translation on the critical path** | Cold Speedometer: the renderer translated 916k guest instructions in 9.9 s; `build_plan`, `compute_tier2`, `emit_instruction` are about 20 % of the main thread's host time. | fx-jit-cost (cache, translation speed) |
| 7 | **Calls, returns and indirect branches** | `call_ret` 2.5× native, `interp_goto` 2.3×, `indirect_calls` 2.0×; call targets spend 43–50 % of their samples on the branch code. | p1 |
| 8 | **FP min/max and conversions** | `fp_float` 1.5× native: scalar FMIN/FMAX are `vcmpunordss` + `vminss` ×2 + `vandnps` …; FCVTZS is `cvttss2si` + a compare with 0x80000000 + a fixup. Cycles: SIMD/FP is 32 % of translated samples. | p3 |
| 9 | Flags | `lahf`, `setcc`, `cmc` 1–3 % of translated samples; `int_mix` 0.8× native. | p2 |
| 10 | Atomics, memory ops | LSE atomics at native speed uncontended; LL/SC 1.4× LSE; `ptr_chase` 0.9×, `mem_own` 1.35×. Low priority. | p2 |

### Anatomy of a jitless JavaScript dispatch

JetStream richards jitless (renderer main thread 91.5 % translated; 3 regions hold 53 % of its samples, 17 hold
91 %; the hottest is 12.5 KB of host code). Read from the host code only. A bytecode handler ends in `br x17`, and
the translated exit is:

| Part | Host instructions | What |
| --- | ---: | --- |
| Flush | 4–6 stores | dirty guest registers outside the cross-region convention (x2, x4–x7, x17) to the context |
| NZCV | 7 | `cmc; lahf; seto al; and eax,0xc101; imul eax,eax,0x10210000; and eax,0xf0000000; mov [r15+nzcv],rax`: flags are live across BR in the strict model |
| Target | 1 | `mov [r15+pc], rcx` |
| Inline lookup | 17 | arena base and table loads from the context, a null check, an alignment check, hash `(pc>>2 ^ pc>>22) & mask`, tag compare, `jmp rdx`; misses `jmp [r15+stub_indirect_live]` |

Natively the same dispatch is a byte load, a table load and `br`. Inside the handlers, guest registers outside the
convention (sp, x19–x22, fp, lr live in rbx, rsi, rdi, r14, r12, rbp, r13) are loaded from and stored to the
context on nearly every use: `mov [r15-0x68],rcx; mov [r15-0x60],rdx; mov [r15-0x58],r8` right after computing them.
V8 keeps its interpreter state in x19–x28 (bytecode offset, array, dispatch table, root, cage base).

fxbench on main (A/A, 2 rounds, scale 30, busy machine), translated/native: int_hash 0.71, int_mix 0.80, branchy
0.42, interp_switch 1.07, interp_goto 2.28, indirect_calls 1.96, call_ret 2.55, mem_own 1.35, libc_calls 12.7,
ptr_chase 0.90, fp_scalar 0.75, fp_float 1.47, vec_portable 1.33, atomics 1.0, atomics_contended 0.84.
