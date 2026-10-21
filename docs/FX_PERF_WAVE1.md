# fx performance, wave 1: result

The first fx performance wave made translated arm64 code faster in general, measured with the suite in
`FX_PERF.md`. Four groups worked on disjoint parts of the engine:

* **p1, control flow:** dispatch, regions, the host-call boundary.
* **p2, integer codegen:** flags and registers.
* **p3, SIMD and FP.**
* **fx-jit-cost:** self-modifying code, invalidation and the cache.

The lead integrated them on `integ/fx-perf` and gated every merge round. This page holds the landing comparison,
the app regression, the gates and the open follow-ups. `FX_PERF.md` keeps the suite, its method and the hot list.

## What landed

55 commits over main (0cc88d60), in four areas.

### Control flow (p1)

* LLVM jump tables and computed-goto label tables dispatch inside the region, by target.
  * Before, clang's switch lowering never matched (0 of 6,899 BRs in jitless Chrome).
  * Table data comes from `CodeSource::read_data`, which reads only read-only guest pages.
  * Dispatch still compares the guest's computed target, so a changed table stays exact.
* Inline indirect lookups store `ctx->pc` only on a miss.
* Leaf host calls (`register_host_leaf`) give libSystem's memcpy, memmove, memset, bzero, memcmp and strlen a path
  with no CpuState copy and no FP handoff.
* Tier 2 keeps only the last interrupt check of a loop-header chain.
* Layout knobs, all off by default: `hotalign`, `align32`.

### Integer codegen (p2)

* **Flags:**
  * Dead host flags are no longer materialised at BL, BLR and RET.
  * BR exits are flag-dead by default, except recognised intra-function tables. See "ABI assumptions" in
    `FX_ENGINE.md`; `nobrdeadflags` and strict `exactflags` restore the old behaviour.
  * Flags stay in EFLAGS around an instruction that clobbers them.
  * A CCMP fuses with its only reader (branchless).
* **Registers:**
  * The dirty-register cache crosses branches into single-predecessor blocks.
  * Pins overwritten before use are not loaded at region entries.
  * GPR results reuse the host register of the value they overwrite.
  * BR and BLR targets are not stored and reloaded at the exit.

### SIMD and FP (p3)

* **Vector state:**
  * Zero-above facts survive labels, from a fixpoint over loop back edges (fp_float's loop: 4 zeroings per
    iteration → 0).
  * Mask reductions allocate their destination before their labels. That was a bug that sent Cycles' hottest region
    to write-through.
  * LD3, LD4 and ST4 fit beside 10 pinned vector registers; ST3 is built one chunk at a time.
* **Lowerings:**
  * 16-bit register shifts use AVX2 dword variable shifts.
  * PMULL .8B uses PMULLW.
  * FCVTZS's saturation check uses an imm8 compare.
* **Tests:** a random NEON region differential fuzzer.

### JIT handling (fx-jit-cost)

* **SMC:**
  * O(1) exit-reference removal. The kill path had been quadratic: 28.5 % of the Speedometer renderer with V8's JIT.
  * JIT write windows (`pthread_jit_write_protect_np`): SMC faults inside a window are deferred and checked when it
    closes.
* **Caching:**
  * V8's builtins copy reuses the image cache.
  * A translator build fingerprint in the cache key, so a codegen change never runs stale cached translations.
* **Invalidation:** shared-memory aliases are walked once per range, not once per page.

### Tests and tools (lead)

* The perf suite: `scripts/perf/`, `FX_PERF.md`.
* Deep differential fuzzing: `TRANSMUTE_FX_FUZZ_SCALE`, `TRANSMUTE_FX_FUZZ_SEED`.
* The fuzz test now names the seed and program of a translator exception, and fails when a program failed. Before, a
  caught exception passed silently.

## Landing comparison

* **Base:** main 3e79a59 plus the suite.
* **Candidate:** r9, which is main a6c9a99 plus the four groups' ranges.
* **Method:** `scripts/perf/fx_ab.ps1` in a measurement window on a quiet machine (load under 20 %), warm-up plus 3
  interleaved rounds, a private fx cache per side.

| Benchmark | Base | r9 | Speedup | 95 % interval |
| --- | ---: | ---: | ---: | --- |
| **Speedometer 3, V8 JIT** | 1.60 | 2.53 | **1.58x** | 1.57–1.87 |
| Speedometer 3, jitless | 4.32 | 4.66 | 1.08x | 0.95–1.22 (same) |
| JetStream 2.2 richards, JIT | 179 | 246 | 1.38x | noisy (same) |
| JetStream 2.2 delta-blue, JIT | 159 | 162 | 1.02x | noisy (same) |
| Cycles render, 640x360, 64 samples | 7.81 s | 7.60 s | 1.03x | same |
| **fxbench, geometric mean of 18 kernels** | | | **1.118x** | |
| … libc_calls (imported memcpy/memcmp/memset/strlen) | 135 ns | 39 ns | 3.48x | 3.25–3.69 |
| … interp_goto (computed-goto interpreter) | 16.6 ns | 10.3 ns | 1.61x | 1.57–1.68 |
| … int_mix (flags, compare chains, csel) | 7.35 ns | 6.10 ns | 1.20x | 1.13–1.21 |
| … interp_switch | 12.9 ns | 11.3 ns | 1.14x | 1.08–1.17 |
| … fp_float | 7.51 ns | 6.99 ns | 1.075x | 1.04–1.10 |

* Every fxbench kernel's checksum matched the native x64 build of the same source on both sides.
* The other 13 kernels were within noise, with mem_own at 0.965 and ptr_chase at 0.957. Both are layout, see the
  follow-ups.
* Single-knob measurements, all from one binary (`window4`/`window5` in the wave's notes):

| Knob | Effect |
| --- | --- |
| BR flag-dead | jitless JetStream richards 1.16x |
| p1's label tables | interp_goto 1.57x |
| p3's zero-above join | fp_float 1.105x |
| p2's flag work | int_mix 1.22x |

## App regression

The wave rules' check, base vs r9, interleaved, cloaked, loopback network, a private fx cache per binary:

| Check | Base | r9 | |
| --- | --- | --- | --- |
| Chrome strict headless `--dump-dom` probe/test.html, jitless, 3 runs | exit 0, `data-js=ran`, 3/3 | exit 0, `data-js=ran`, 3/3 | equal |
| Chrome windowed (software compositor, `run_windowed.ps1 -Frames 2 -Settle 20`), 3 runs | Browser.close, exit 0, 3/3 | Browser.close, exit 0, 3/3 | equal |
| Blender windowed startup, first 3D viewport draw, 3 runs | 2741 / 2804 / 2784 ms | 2808 / 2748 / 2772 ms | equal |
| Blender headless `-b --factory-startup` + operator, 4 rounds | CPU 2.07 s, wall 2.22 s | CPU 1.99 s, wall 2.40 s | same |
| QLab 5 to its welcome window, 6 rounds | 1.26 s | 1.27 s | same |
| QLab's welcome window capture | `backing-5.bmp`, SHA-256 3B0D89F748905800… | byte-identical | equal |
| A renderer plug-in's headless cube in Blender, 2 rounds | renders; 6.5 s | renders; 7.2 s | same (interval 0.82–1.0) |

Network denials were only the known ones: Chrome's `[2001:4860:4860::8888]:443` and QLab's account server.

## Gates

* **Every round:**
  * a full build;
  * `ctest -R "^(alchemy_|fx_)" -j12`, cloaked, with a private fx cache;
  * the fx translator, family, NEON, FP, system and interpreter tests again with the differential fuzz at 50x on a
    new seed range per round.
* **Results:**

  | Rounds | Result |
  | --- | --- |
  | 0–3, 5–9 | passed with known failures only |
  | 4 | failed: the deep fuzz found the CCMP-fusion register bug |

  * Round 4's bug was fixed with a regression test, and round 5 re-gated it.
* **The known failures:**
  * `alchemy_gui_network` and `alchemy_gui_bonjour_fixture`: this machine's mDNS.
  * `alchemy_gui_windowserver_win32`: foreground checks.
  * Under `-j12` load only: `fx_abi_tests`' thunk timing assert (main fails it the same) and the `fx_runtime_tests`
    preloader test. Both pass alone.
* **The landing tree** (r9 + main 0cc88d60), round 10: full build exit 0, ctest **246/249** (the two mDNS tests and
  `alchemy_gui_windowserver_win32`), deep fuzz 8/8.

## Follow-ups (open)

* **Loop alignment and layout.**
  * The two layout regressions seen during the wave were both code layout, not codegen:
    * interp_switch: fixed by tier 2's header-chain change;
    * mem_own: an unaligned loop whose interrupt-check `cmp` straddles a 32/64-byte boundary.
  * Tier 2's `t2align` rule skips padding for small loops that are fallen into.
  * Next: an A/B of `TRANSMUTE_FX_CORE=t2noalign` (always pad small loops) on the quick suite and Cycles. If it's
    neutral or better, p2 flips the rule.
  * `hotalign`/`align32`: mixed (interp_switch up to 1.15x, call_ret down to 0.82x). They stay off until a broader
    measurement.
* **fx-jit-cost's first-tier JIT code knobs** (30b957c: `TRANSMUTE_FX_JIT_CODE_BUDGET`, `_WINDOW`): an A/B with
  Speedometer and JetStream with the JIT, not measured yet.
* **`mergedirty`** (dirty values into merge points): 0.97 overall. It stays off.
* **Jitless Chrome:** 1.08x only. Before the wave, the Ignition dispatch was about 30 host instructions where native
  uses 3 (`FX_PERF.md`, "Anatomy of a jitless JavaScript dispatch"). The wave removed its NZCV materialisation, the
  target reload and part of the lookup. Dirty-register flushes and the lookup remain; a fresh profile should recount
  them.
* **Code density:** the jitless renderer's 90 % hot set was 17.7 MB of host code over 1,612 regions (5.3x expansion),
  so a smaller expansion pays everywhere.
* **The differential fuzz runs strict flags,** so the BR flag-dead relaxation is covered by its unit test and the A/B,
  not by the fuzz. An ABI-mode fuzz variant would need programs that don't read flags after BL, BLR, RET or BR.
