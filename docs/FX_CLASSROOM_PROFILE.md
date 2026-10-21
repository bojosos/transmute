# FX Classroom performance investigation

Measured on 2 October 2026 on the i7-1355U in this workspace. The main target
is steady Cycles CPU rendering of the staged ARM64 Blender 4.5 Classroom scene,
compared with the installed x64 Blender 4.5. The initial investigation measured
FX without changing engine implementation or defaults. Follow-up optimization
experiments keep Embree and the rest of the ARM64 application translated.

FX retires about twice as many host instructions as native on this workload.
Reducing that generated work is the main route forward. The clearest small
optimization is an Embree movemask idiom that measures 3.5 times slower than
native. The larger opportunity is register allocation across hot control flow,
supported by context traffic in the profile and real instruction-cache stalls.

The initial warm 192 by 108, 16-sample, one-thread render took 5.30 s in FX
and 3.29 s native. The corresponding full processes took 6.74 s and 4.02 s.
These short runs establish the symptom but are sensitive to startup and CPU
frequency. The longer comparisons below are the basis for recommendations.

## Build and measurement conditions

- Current working tree at commit `abe916e`, including the pre-existing
  uncommitted FP boundary and translator changes. Those changes were preserved.
- Separate Release build in `build-fx-investigate`, MSVC 19.44.35228,
  `TRANSMUTE_FX_ONLY=ON`. Separate translation caches for experiments.
- AC power, battery saver off, Windows Balanced plan. Single-thread runs use
  logical CPU 0, affinity mask 1. Avoid comparing absolute times between runs
  collected under different profiling or thermal conditions.
- Raw logs, images, profiles, generated assembly and benchmark JSON live in
  `artifacts/fx-investigate`. These artifacts are ignored by Git.
- The synthetic programs use the same Clang toolchain for both architectures,
  explicit 128-bit NEON or SSE operations, and disabled automatic vectorization
  and FP contraction. Explicit FMA intrinsics remain fused. This separates
  translation costs from native Blender's different compilation and dispatch.
- Final timing runs explicitly set and verify HighQoS and High priority on
  each child process. Setting these on the launcher alone did not propagate.
  Earlier `study`, `study-t8`, `patterns`, and `patterns-controlled` directories
  retain exploratory results; use the `*-final` directories for final timing.

## Repeated Classroom renders

Each configuration was warmed once and measured three times, reversing the
configuration order on alternate rounds. These are Blender's reported render
times, excluding executable startup. The eight-thread test allowed all 12
logical CPUs, affinity mask 4095; it is not restricted to P-cores.

| Workload | Native median (range), s | FX median (range), s | Median paired FX/native |
| --- | ---: | ---: | ---: |
| 192 by 108, 32 samples, 1 thread | 6.92 (5.51–8.24) | 9.67 (8.40–11.46) | 1.52 |
| 480 by 270, 16 samples, 8 threads | 5.24 (4.51–5.36) | 8.67 (8.31–9.20) | 1.76 |

The ratios of the separate medians are 1.40 and 1.65, respectively. They differ
from the median paired ratios because the laptop's timings drifted substantially.
Even the paired single-thread ratios span 1.17–1.66; eight-thread ratios span
1.62–1.84. HighQoS and affinity do not fix clock frequency or temperature.
These runs confirm a material gap, but cannot establish a precise speedup or a
small tuning improvement. The hardware counters and stable artificial patterns
below provide stronger evidence about its cause.

The one-thread study also tested the existing `carrydirty` and `noinline` flags
with separate warmed caches:

| Configuration | Render median, s | Median paired time / default | Paired range |
| --- | ---: | ---: | ---: |
| Default FX | 9.67 | 1.000 | 1.000 |
| `carrydirty` | 9.57 | 0.990 | 0.949–1.002 |
| `noinline` | 8.78 | 0.963 | 0.887–1.045 |

Neither establishes a consistent improvement under the observed drift. Do not
change the defaults based on these small samples. All six variant images were
pixel-identical to the matching baseline FX images, including alpha. Raw times,
conditions, images, and comparisons are saved in `study-final` and `study-t8-final`.

## Hardware counters

The user ran the paired VTune hardware-counter collections from an elevated
PowerShell. Both completed successfully. Results are in
`artifacts/fx-investigate/vtune-admin/20261002-225836`. The following are whole
collection metrics for 480 by 270, 64 samples, one thread on logical CPU 0.

| Metric | Native x64 | FX | Interpretation |
| --- | ---: | ---: | --- |
| Retired instructions | 384.53 billion | 820.16 billion | FX executes 2.13 times as many |
| CPU cycles | 272.18 billion | 390.68 billion | FX needs 1.44 times as many |
| Cycles per instruction | 0.708 | 0.476 | Individual instruction throughput is not the main problem |
| Front-end bound | 22.4% | 27.2% | More instruction-delivery pressure in FX |
| Instruction-cache miss stalls | 3.9% | 7.1% | Larger code footprint has a measured cost |
| Instruction TLB miss stalls | 4.0% | 4.5% | Smaller increase than instruction-cache stalls |
| Bad speculation | 9.0% | 8.9% | Similar fraction, not proof of equal absolute cost |
| Memory bound | 22.4% | 14.4% | A lower fraction amid more generated work |
| Average CPU frequency | 2.118 GHz | 2.845 GHz | Profiled wall times are not comparable |
| Multiplexing reliability | 0.999 | 0.998 | VTune's reported reliability |

Front-end, speculation and memory-bound percentages use pipeline slots.
Instruction-cache and TLB stall percentages use cycles. Do not add these
overlapping metrics or interpret them as directly recoverable elapsed time.
The hardware samples also include kernel work attributed to the target.
Different native and ARM64 compiler output, SIMD widths and dispatched code
paths contribute to this comparison; the 2.13 multiplier cannot all be assigned
to translator inefficiency. Memory latency also makes cycle counts sensitive
to frequency. Treat the 1.44 cycle ratio as this collection's observation.

VTune attributed 127.80 CPU seconds to FX code outside any image module, which
includes the translation arena. The runtime executable itself accounted for
0.256 CPU seconds. Native `blender.exe` and `embree4.dll` accounted for 77.51
and 35.30 CPU seconds respectively. This agrees with the in-process profile:
generated rendering code is the main target, not dispatcher C++.

The first version of the collection script asked for a time-filtered summary.
VTune explicitly ignored that filter. Files named `*-render-summary.txt` in
this collection are therefore whole-run reports too. The saved script now
requests a time-filtered hotspots counter report instead. No conclusions here
depend on the ignored filter.

## Where Classroom spends its time

The built-in sampler recorded 141,973 thread samples at a requested 2 ms
interval during a 480 by 270, 64-sample, one-thread FX render. The render worker
was in translated code for 95.7% of its samples, runtime stubs or unmapped arena
code for 1.0%, ntdll for 0.3%, and other host code for 3.0%.

The aggregate 75.8% host figure is misleading for CPU attribution. This sampler
suspends and inspects sleeping threads too. Two idle threads and the waiting
main thread dominate the aggregate. It is not evidence of a global runtime lock.

| Guest function or location | Share of translated samples |
| --- | ---: |
| Embree `+0x33e974` | 16.71% |
| `noise_fbm` | 9.29% |
| `svm_eval_nodes<8389563,0,...>` | 8.99% |
| `kernel_tex_image_interp` | 8.52% |
| Embree `+0x33e200` | 7.95% |
| `integrate_surface<955>` | 6.80% |
| `integrator_megakernel` | 5.26% |
| `svm_node_tex_image` | 3.43% |
| Embree `+0x3595c4` | 3.25% |

The profile's 54 regions covering 90% of translated samples occupy 668,904 host
bytes on 180 pages. Their ARM64 instruction ranges occupy 159,204 distinct bytes,
an expansion of 4.20 times. This is a footprint measurement, not proof of an
instruction-cache bottleneck. It includes region stubs and constant pools.

The hottest 24 regions contain 26,275 RIP samples. The host-only classifier
decoded all of them:

| Sampled instruction category | Share within those regions |
| --- | ---: |
| Other SIMD and FP | 31.09% |
| Other integer and control | 21.72% |
| Guest context reads and checks | 18.55% |
| Other memory operands | 14.50% |
| Guest context stores | 7.87% |
| Branches | 3.06% |
| INSERTPS | 2.41% |
| Flag handling | 0.81% |

These are mutually exclusive categories of sampled instruction addresses.
They are not retired instruction counts, cycle attribution, or removable time.
For example, context operands in FP instructions belong to the context category.
Other SIMD/FP includes operations with RIP-relative constants. INSERTPS includes
lane extraction as well as upper-lane clearing.

### Concrete code patterns

In `noise_fbm`, host region 2 offsets `0x8c4` through `0x949` contain a sequence
of scalar loads, full-vector loads, full-vector stores, and reloads around a
loop back edge. The default translator flushes dirty unpinned values at branch
boundaries. `carrydirty` already provides an experimental alternative.

Texture region 3 has 1,671 samples at `+0x3e89`, a `movsxd` immediately after
a texture load at `+0x3e84`. Another 366 samples land immediately after another
texture load. Likewise, Embree region 1 has 525 samples at a context load
immediately following a BVH vector load. Optimizing the named integer instruction
alone would be an unjustified conclusion. Data latency and instruction scheduling
must be separated from translation overhead.

The two largest Embree functions alone account for 24.66% of translated samples.
Their hot paths include vector box tests, byte-table shuffles, lane reductions,
and pointer-dependent loads. Noise contains shuffle/subtract/FMLA chains followed
by scalar accumulation. These informed the artificial benchmarks.

### Costs that do not explain the main gap

The warm long profile translated only 1,324 guest instructions, with 1.4 ms of
JIT time and five interpreter fallbacks in the whole render. Total foreground
translation/cache/install accounting was 63.7 ms. Further JIT compile-time
optimization cannot materially accelerate that render's steady state.

The cold small render JIT took 810.8 ms and compiled about 2.56 million guest
instructions. Startup is a separate worthwhile target for short jobs, but it
does not explain the steady rendering hot spots.

## Artificial instruction pattern benchmarks

All nine kernels compiled and ran natively and through FX. All output checksums
matched, including the SIMD output lanes. Final measurements use three paired
runs after a complete warm-up, matching explicit HighQoS and High priority,
and affinity mask 1. Results are in `artifacts/fx-investigate/patterns-final`.

| Pattern | Median elapsed FX/native | Median thread-cycle FX/native | Thread-cycle ratio range |
| --- | ---: | ---: | ---: |
| Vector min/max chain | 1.25 | 1.25 | 1.23 to 1.25 |
| Compare/select on the same finite inputs | 1.51 | 1.50 | 1.50 to 1.51 |
| Scalar polynomial, source inlined | 1.09 | 1.08 | 1.08 to 1.08 |
| Scalar polynomial, source calls | 1.02 | 1.02 | 1.01 to 1.02 |
| Four texture taps, 16 KiB working set | 1.52 | 1.52 | 1.52 to 1.52 |
| Four texture taps, 16 MiB working set | 1.27 | 1.28 | 1.23 to 1.32 |
| Noise shuffle/subtract/FMLA chain | 1.15 | 1.17 | 1.17 to 1.21 |
| Embree-style comparison to bit mask | 3.51 | 3.51 | 3.49 to 3.57 |
| Branches with live scalar values | 1.16 | 1.17 | 1.08 to 1.20 |

The mask conversion is a consistent large deficit. Its ARM64 source uses
`FCMxx`, `AND 1`, `USHL {0,1,2,3}`, `ADDV`, and scalar extraction. The native
version uses a compare and `MOVMSKPS`. The same pattern occurs at Embree guest
RVA `0x33ebc0` through `0x33ebd0`; the staged DLL's constant at RVA `0x4146f0`
contains the four shift values `{0,1,2,3}`. FX's output uses separate operations
for the shift and horizontal reduction, including intermediate vector moves.
This is a concrete fusion target with a matching native comparison.
The native compiler also unrolled the mask loop by two. These are equivalent
source-level operations, not identical machine-code loops with a single
instruction substituted.

The larger texture benchmark has a smaller relative translation penalty despite
costing much more per operation. Data latency hides some added work, so a small,
cache-resident gather cannot stand in for real textures. Likewise, the small
noise interpolation chain costs about 17% more thread cycles than native,
while the real `noise_fbm` function also spills values. Register pressure and
control flow around that arithmetic need attention.

Compare/select was slower than the current min/max implementation in these
finite-input examples. Do not replace ARM min/max semantics globally on the
assumption that exceptional-value handling must be the largest cost.

Thread-cycle counts use `QueryThreadCycleTime` on the same host CPU. They
exclude much of the scheduling noise in elapsed time but are not the VTune
PMU cycle measurements above. Windows documents processor-dependent timer
behavior; these counts are not converted into seconds. See
[Microsoft's API documentation](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime).
The large-texture and branch kernels still vary more than the others. Differences
of a few percent require more runs. Earlier timings without verified child
controls are retained but are not used for recommendations.

The command below reproduces the current performance failure. Correctness
checks pass; the optional 1.25 ratio performance gate fails, prominently on
the mask conversion:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/fx_pattern_bench.ps1 -Runs 3 -MaxRatio 1.25
```

## Recommended implementation order

### 1. Fuse the Embree mask idiom

This is the most concrete first change: a repeatable 3.51 cycle ratio and the
same instruction sequence in a major real hot spot. Recognize the vector
comparison, mask-to-one, shift-by-lane-index, horizontal sum, and scalar extract
as one operation. For the proven `{0,1,2,3}` shift vector, the essential host
work can be a vector compare plus `MOVMSKPS`.

Start at `src/fx/translator/emit_neon_dispatch.cpp:176` and the sequence handling
in `region_emitter.cpp`; NEON currently dispatches only `words[0]`. Propagate
constant vectors or guard their values when proof is unavailable. Do not infer
the shift values merely from register names or an observed address. Preserve
the ARM comparison's NaN/exception behavior, every live guest result, and legal
intermediate entry points. In particular, `ADDV` writes a scalar vector result;
it must still be materialized if live after the scalar extraction. Update the
consumed-instruction, use/def and fault-state accounting together.

Validate against the existing movemask sequence tests, all 16 comparison masks,
aliasing, exceptional FP inputs, and full guest state. Then require fewer
generated instructions and a lower `mask_reduce` ratio before measuring
Classroom. The local 3.51 ratio is not a prediction of overall render speedup:
the profile does not isolate time spent in this idiom.

### 2. Optimize live ranges across hot blocks

This is the strongest broader candidate, but needs a measured prototype. The
engine already has register caching, next-use eviction, region pinning, direct
links and a cross-region register convention. Adding those features again is
not the answer. The remaining issue is how values survive branches, joins and
register-pressure peaks.

`RegionEmitter::flush_dirty` at `region_emitter.cpp:255`, branch/fallthrough
handling around line 1249, and pin selection in `region_plan.cpp` are the places
to inspect. The 64-instruction eviction lookahead at `region_emitter.cpp:91`
is a local heuristic. The real noise loop visibly stores and reloads values
around its back edge, whereas the small arithmetic-only noise benchmark is
only 1.17 times native in thread cycles. Simply enabling `carrydirty` did not
establish a win.

Build an optimizing tier for the hottest regions with explicit value identities,
block liveness, split live ranges and register assignment at joins. Remove dead
guest-context stores, coalesce copies, and keep loop-carried values in registers
when cheaper than pinning them across the whole region. Preserve precise guest
state through the existing fault maps and helper boundaries. Start with
`noise_fbm` and one Embree traversal region, leaving cold translation fast.

The first acceptance criteria should be fewer context loads/stores and retired
host instructions for those regions, bit-identical FX output, and lower cycles.
The 26.42% of sampled addresses on context operations is evidence to investigate,
not a promised 26.42% recoverable execution time.

### 3. Reduce the hot code footprint and specialize vector operations

The 4.20-fold footprint expansion and 7.1% versus 3.9% instruction-cache stall
measurement support this work. Keep hot successors adjacent, place rare exits
and fixup paths away from the hot path, and budget inlining by generated host
size and measured call frequency. The global `noinline` switch is too coarse
and did not establish a consistent gain here.

Extend constant/range and dead-lane knowledge through shuffles and reductions
to avoid general variable-shift sequences and unnecessary temporaries. Upper
lane tracking already exists in the FP and NEON emitters; improve its scope
rather than removing required ARM zeroing. INSERTPS accounts for only 2.41% of
sample locations and is not always zeroing. Likewise, replacing min/max with
compare/select made the finite-input synthetic example slower.

Judge this work using emitted bytes and dynamic instruction counts as well as
cycles. Smaller code can help both the front end and register pressure, but
the measured stall percentages are not independent speedup budgets.

### What to defer

Warm-cache JIT speed, interpreter coverage and C++ dispatch are too small in
this render to close the steady-state gap. AOT can help startup without making
the same emitted hot loop execute faster. Broad fast-math changes would trade
correctness for an unproven benefit and are unnecessary for the first targets.

As a scale check, moving the observed 1.44 cycle ratio to 1.10 would require
roughly a 23–24% reduction in FX's total cycles under comparable conditions. No
single measured flag supplies that. Idiom fusion is the first bounded change;
better live-range handling and generated-code size are the broader program.
All gains in this section remain proposals until implemented and re-profiled.

## Implementation results (3 October 2026)

Release build `build-fx-tier2`, ABI 19. The laptop was shared with other
agents' builds during these runs: CPU load reached 100% for long stretches,
and the same binary's `mask_reduce` ratio ranged 1.89–2.51 across sessions.
Paired ratios between two FX builds in the same run (`-BaselineFx`) are the
reliable comparison; ratios to native are indicative. Results are in
`artifacts/fx-tier2/patterns-*`.

### 1. Movemask fusion (default on)

`emit_neon_fusion.cpp` fuses AND with {1,1,1,1} or USHR #31, USHL by
{0,1,2,3}, ADDV and FMOV into MOVMSKPS. Codex's version checked both vectors
inline on every execution (six instructions plus two context loads in
Embree). Now the shift and mask vectors are proven by **vector value facts**
(docs/FX_ENGINE.md): MOVI definitions prove the mask; Embree loads the shift
vector once at function entry and again after each filter callback, so a
16-bit flag is checked after each of those loads and the fusion tests that
flag (`movzx ecx, word [flag]; jrcxz`). A compare definition proves lane
masks, so MOVMSKPS reads the compare result directly. The USHR #31 form, four
of Embree's five sites in `+0x33e974`, is fused too. The compare is not part of
the span, so its NaN and FPSR behaviour is unchanged. Every other case keeps an
exact general path.

| Code | Unfused | Codex (inline guard) | Now |
| --- | ---: | ---: | ---: |
| Embree `+0x33ebc0` site, hot path, x64 instructions | 15 | 11 (2 context loads) | 4 |
| `mask_reduce` inner loop per iteration | 27 | 24 | 18 |
| `mask_reduce` thread cycles / native | 3.41 | 2.71 | 1.89–2.51 |
| `mask_reduce` paired vs Codex build | | 1.00 | 0.59–0.85 |

Native is about 8.5 instructions per iteration: it is unrolled by two and folds
the loads. What remains in FX is the interrupt check (2), Q-index address
arithmetic (`lea` x2), post-index updates, and the ADDV scalar result's write
to its vector register. This loop's 1.0–1.3 target needs loop-level work, not
more fusion. The Classroom 10% one-thread render is pixel-identical to
`artifacts/fx-baseline/classroom_p10_s16_t1_fx.png` (0 of 82,944 channels
differ, run with tier 2 on).

### 2. Optimizing tier (prototype, `TRANSMUTE_FX_CORE=tier2`, off by default; completed in section 4)

Implemented: tier-up counters on every first translation; the eager
re-translation is tier 2. Within a block it adds **vector copy aliases**: a
MOV emits nothing, the destination names the source's host register or its
context slot, and a copy back of a slot's own value leaves it clean. These
are explicit value identities, and the dead stores of copied values go away.
`t2carry` adds dirty values across loop edges; `t2loops` tiers up hot loops
at their header (OSR). Both are opt-in. Fault maps describe dirty register
aliases; slot aliases are copied into their own slots before any instruction
that may fault.

| Target | Plain | Tier 2 |
| --- | ---: | ---: |
| `noise_fbm` hot loop, x64 instructions / iteration | 257 | 245 |
| same, context loads + stores / iteration | 34 + 18 | 26 + 12 |
| `noise_fbm` region, static instructions | 937 | 925 |
| Embree `+0x33e974` region, static instructions | 1754 | 1751 |

The noise loop's MOV rotation (`v8..v31` and back) shrinks from 16 context
operations to one copy. Embree's traversal has no hot copies, so it is
unchanged; its context traffic is GPR pressure and data latency. The pattern
kernels tiered up (`-TierThreshold 1`) run at 0.97–1.07 of plain FX, within
noise. `t2carry` measured 1.02–1.17 slower. `t2loops` lost the mask facts:
the header region has no preheader definitions, so the fusion falls back to
inline checks, and `mask_reduce` ran 1.19 slower. Classroom with tier 2
re-translated 2,679 regions (0.67M guest instructions, 0.53 s JIT).

Not yet done: live ranges split across joins and register assignment at
them (the existing merge-point cache still decides), GPR copy aliases,
spilling chosen per loop for the 16 xmm registers, and a dynamic instruction
counter. The loop counts above are static counts of single-block loops,
which are also their per-iteration dynamic counts.

| Pattern (5 paired runs) | Before (doc) | After, thread cycles / native | Tier 2 / plain |
| --- | ---: | ---: | ---: |
| minmax | 1.25 | 1.31 | 1.02 |
| compare_select | 1.50 | 1.48 | 1.02 |
| scalar_inline | 1.08 | 1.08 | 0.97 |
| scalar_calls | 1.02 | 1.21 | 1.00 |
| texture_gather | 1.52 | 1.51 | 1.03 |
| texture_large | 1.28 | 1.38 | 1.07 |
| noise_lerp | 1.17 | 1.20 | 0.99 |
| mask_reduce | 3.51 | 2.51 | 1.03 |
| branch_live | 1.17 | 1.21 | 0.99 |

Only `mask_reduce` changed beyond the run-to-run noise. The other "after"
values moved in both directions, with the same code as before.

### 3. Hot-code footprint (ABI 20, cache format 5)

Release build `build-fx-footprint`; "before" is the ABI 19 binary of the same
tree (`build-fx-tier2`, saved in `artifacts/fx-footprint/baseline-bin`).
`TRANSMUTE_FX_CORE=oldlayout,oldcap` reproduces it byte for byte (identical
JIT-bench output hash over embree4.dll). Results: `artifacts/fx-footprint`.

What changed (docs/FX_ENGINE.md, Code layout; NOTES_CORE.md):

- **Cold section.** Unlinked exit tails (about 31 bytes each, dead once
  linked), RET's RSB-miss lookup (about 70 bytes), switch fallbacks and
  interpret/SVC/BRK exits leave the body; it keeps `jmp rel32`, a 6-byte
  `jcc` or a `jmp` to the stub. The runtime already restored the rel32 it
  read at install, so tails can be anywhere.
- **Continuation entry stubs in line** after the call's exit (one jump fewer
  per return, the stub next to its block); not at loop headers.
- **Alignment.** Only loops shorter than 24 guest instructions keep their
  16-byte header alignment. Every-loop alignment (`loopalign`) measured
  0.996-1.009 of this in the pattern kernels, i.e. no effect.
- **Encodings.** Backward branches in rel8 reach are short (a typical inner
  loop back edge: 6 to 2 bytes); vector context moves use `movups` (8 bytes,
  not 9); BL writes the return pc straight into x30's pin and RET compares
  that pin with the RSB (rcx only on the miss path): one instruction fewer
  per call and per return.
- **Formation.** A region may cover its whole .pdata function, up to 16384
  instructions (`oldcap`: 4096). Cut at 4096, Classroom's large kernels were
  translated again from every exit into their untranslated part, and the
  depth-first walk of those regions re-covered the first one:
  `integrate_surface` had three hot regions (58, 60 and 41 KB; the first two
  shared 93% of their instructions). It now has one of 99 KB.

Hot footprint (profiler, Classroom 10%, 16 samples, 1 thread on CPU 0, cold
private cache, 2 ms; one run each; the "before" run was slowed by a parallel
build, so its sample distribution is comparable but not identical):

| Regions covering 90% of samples | Before | After |
| --- | ---: | ---: |
| Regions | 149 | 157 |
| Host bytes (pages) | 1,255,987 (354) | 1,218,876 (373) |
| Body bytes (pages) | - | 1,016,993 (346) |
| Guest bytes, sum over regions | 332,980 | 329,660 |
| Guest bytes, distinct | 274,748 | 323,104 |
| Guest bytes translated more than once | 58,232 | 6,556 |
| Expansion, host / distinct guest | 4.57 | 3.77 |
| Expansion, body / distinct guest | - | 3.15 |

At 50% (13-16 regions): 315,028 to 281,700 host bytes, 84 to 78 pages,
expansion 3.89 to 3.14 (body 2.77). Overlap was not about 1%: 17-21% of the
hot guest code was translated twice, mostly the big Cycles kernels. What
remains is switch cases of `svm_eval_nodes` reached through indirect jumps
(2 KB) and a 6-instruction leaf inlined 46 times. Whole-function regions
also bring the cold parts of hot functions into hot regions, hence more
distinct guest bytes and pages for slightly fewer host bytes.

Static size (`fx_translator_image_audit`, every .pdata function start):

| Image | Before: bytes (body) | After: bytes (body) | Bytes / guest instruction |
| --- | ---: | ---: | ---: |
| blender.exe | 296,964,239 (254,600,674) | 285,997,102 (221,549,092) | 20.1 to 19.1 |
| embree4.dll | 18,378,002 (16,038,618) | 17,633,821 (14,810,627) | 18.0 to 17.3 |

blender.exe's static regions now cover 1.6% more guest instructions (the
cap); with `oldcap` it is 282,563,831 bytes (-4.8%). A cold-cache Blender
start (`-b --python-expr "import bpy"`, `TRANSMUTE_FX_STATS=1`, one run
each) JIT-translated 33,618 regions and 1.740M guest instructions (before:
33,680 and 1.746M) into 34.7 MB of arena (before: 36.6 MB, -5.2%); JIT time
517 vs 481 ms, within this machine's run-to-run spread. In the 40 hottest
functions (`tools/fx_layout_audit.py`, same starts, `oldcap` for like with
like): 699,306 to 675,337 bytes, NOP padding 7,911 to 2,981 bytes, exit tails
in the body 14,818 to 0 bytes. The body itself is 0.8% larger (564,199 to
568,715): the continuation entry stubs (33 KB, mostly pin reloads) moved in.
They run on every return, so the old cold section was not cold; with
`coldentries` the body is 536 KB.

Pattern kernels (`fx_pattern_bench.ps1 -Runs 3`, thread cycles vs native,
paired medians; the laptop was busy, ranges in brackets):

| Pattern | Before | After | After / before |
| --- | ---: | ---: | ---: |
| minmax | 1.28 | 1.38 | 1.06 (0.99-1.08) |
| compare_select | 1.55 | 1.61 | 1.04 (0.90-1.20) |
| scalar_inline | 1.10 | 1.10 | 1.00 (0.85-1.14) |
| scalar_calls | 1.09 | 1.01 | 1.00 (0.76-1.20) |
| texture_gather | 1.54 | 1.50 | 1.00 (0.88-1.21) |
| texture_large | 1.37 | 1.27 | 1.00 (0.85-1.36) |
| noise_lerp | 1.18 | 1.18 | 1.00 (0.95-1.26) |
| mask_reduce | 2.37 | 2.25 | 0.98 (0.86-1.34) |
| branch_live | 1.17 | 1.18 | 1.01 (0.88-1.22) |

The minmax and compare_select inner loops are byte-identical between the
two builds apart from a 2-byte back edge (checked statically); their
cross-binary differences are placement and noise. Same-binary A/B runs
(5 paired runs) are steadier: `oldlayout` / new 0.996-1.022, mask_reduce
1.079; `coldentries` / new 0.985-1.030; `oldcap` / new 0.979-1.012.
`fx_bench.ps1` is unchanged within noise; seven alternating runs of the
call-heavy groups: recursion_fib 3.40 ns new vs 3.27 `oldlayout` (3.24
`coldentries`), TlsGetValue 5.41 vs 5.70, SetLastError 11.1 vs 11.7, the
others within 1%. The HeapAlloc and malloc gates (50 ns) fail in both
builds (native itself takes 52-58 ns); CriticalSection_pair sits at its
20 ns gate in both (18.0-20.2 ns across runs).

The Classroom 10% one-thread render is pixel-identical to
`artifacts/fx-baseline/classroom_p10_s16_t1_fx.png` (0 of 82,944 channels).

Considered and not done:

- **Inlining budget by host size.** Inlining shrinks the hot code: the 40
  hottest functions are 3% larger with `noinline`, since a call costs an RSB
  push, pin stores, an entry stub and a RET. Budgeting by host size would
  inline more, not less; call frequency is not known at first translation.
- **Forward branch relaxation.** 1,983 forward rel32 branches of the hot
  functions' bodies are in rel8 reach: about 7 KB (0.9%). It needs offset
  remapping of fault sites, exits and fixups after emission.
- **Context displacements.** 21,290 body operands use `[r15 + disp32]`
  (vector slots lie outside the disp8 window that x0-x31 fill): 64 KB, about
  8% of the hot functions' bodies. Fixing that needs a second base register
  or a GuestContext layout change.
- **Arena placement.** The 90% regions' bodies fill 346 pages at 72% density;
  hot regions are already clustered by first use. Without hotness at
  install time (tier 1 is the first translation) a separate hot area has
  nothing to sort by; it would follow tier 2 (`TRANSMUTE_FX_CORE=tier2`).

### 4. Optimizing tier completed (ABI 21, tier 2 on by default)

Release build `build-fx-tier3`; "plain" is the same binary with
`TRANSMUTE_FX_CORE=notier2` (byte-identical to the ABI 20 code: equal JIT
bench output hash over embree4.dll); "prototype" is `t2proto`. Results:
`artifacts/fx-tier3`. The laptop was shared: from mid-session its CPU stayed
at 100% (other applications), so only the earlier pattern run below is a
usable timing; the rest are instruction counts.

What changed (docs/FX_ENGINE.md "Tier 2", NOTES_CORE.md):

- Tier-up from loops only: one counter per region at its loop headers,
  `ExitReason::tier_up_loop`; regions without loops are final. Classroom
  10%: 418 tier-ups, 161K guest instructions, 129 ms of tier-up JIT (cold
  cache) where the prototype's entry counting made 2,679 (0.67M, 0.53 s).
- Covered interrupt checks dropped; no alignment padding on fall-through
  loop headers; pin weights 8^loop depth (an inner loop evicts convention
  registers it never uses).
- Region-wide liveness: dirty cached values no path observes again (reads,
  fault sites, exits, helpers, interrupt checks) are dropped, not stored.
- Two passes: loop headers take the registers their back edges hold; a
  loop's carried dirty values stay in registers across its branches and
  are stored on the way out.
- GPR copy aliases; TBL's adjusted indices precomputed after the index
  register's definitions (`GuestContext::vspill`), read as memory operands.

Hot loops (`tools/fx_loop_count.py`, x64 instructions per iteration of the
guest path, edge stubs included; context = guest-register operands of
GuestContext):

| Loop | Plain | Prototype | Now |
| --- | ---: | ---: | ---: |
| `noise_fbm` (2a4d280..2a4d4d0) | 257 (34 loads + 18 stores) | 245 (26 + 12) | 218 (19 + 10, + 10 TBL index operands) |
| Embree `+0x33e974` node loop (33eb4c..33ebfc) | 86 (16 + 5) | 86 (16 + 5) | 82 (16 + 2) |
| Embree `+0x33e200` node loop (33e440..33e4e4) | 85 (14 + 2) | 85 (14 + 2) | 82 (14 + 1) |

Embree's node loops lost one interrupt check (two headers on one cycle),
loop-alignment NOPs and the dead stores of the hit-mask temporaries. Their
remaining context loads are the loop-invariant offsets (x4-x7, x12-x17):
there are no free host registers for them (`t2lend`, below). Most of their
remaining overhead is exact FMAX/FMIN NaN and signed-zero handling (24 of 82
instructions), not allocation.

Profile-weighted estimate (`tools/fx_tier2_estimate.py`, the hottest
regions of a Classroom 10% one-thread run, 1 ms sampling, ~1,700 samples
in 20 regions whose static translation matches the run): plain needs 4.1%
more host instructions than tier 2 (prototype: +3.2%, i.e. the prototype
saved 0.9%). `noise_fbm` 0.86x of plain, Embree `+0x33e974` 0.98x,
`+0x33e200` 0.97x, OIIO `+0x39dd08` 0.88x, the rest 0.95-1.00x.

Pattern kernels, same binary, tier 2 / plain thread cycles (3 paired runs,
before the machine got busy; `patterns-1`): texture_gather 0.912
(0.902-0.919), texture_large 0.924 (0.865-0.932), scalar_calls 0.985,
noise_lerp 0.987, compare_select 0.993, branch_live 0.994, scalar_inline
1.001, minmax 1.021 (1.015-1.026), mask_reduce 1.026 (1.005-1.037). The
minmax and mask_reduce loops are instruction-for-instruction identical in
both modes (only register names and offsets differ: placement). Two later
runs (`patterns-2`, `patterns-3`) had paired ranges of 0.33-2.9 under the
foreign load and are not used.

Classroom 10% one-thread with tier 2: pixel-identical to
`artifacts/fx-baseline/classroom_p10_s16_t1_fx.png` (0 of 82,944 channels),
twice (profiled run and the final identity run). Its render time is not
comparable (the identity run shared the CPU at 100%).

Tried and not kept (estimate relative to plain, on the earlier `profile-final`
run of section 3, where the kept design reaches 0.978 without the TBL
indices and 0.970 with them):

- Entry states from every incoming edge for every merge point (2-pass
  vote): 1.017 (worse: static weights cannot tell the hot edge; carrying
  every dirty value moved stores into hotter blocks). Header states holding
  everything live at the header or everything the loop reads: 0.992-0.997
  (pressure). Kept: headers only, tier 1's header filter, carried values
  only.
- Pin lending (`t2lend`, opt-in): hosts of pins a loop never uses serve the
  register cache inside it. 0.984 against 0.980 without: the lend/return
  traffic on every loop entry and exit outweighs the extra registers for
  these loops (Embree's inner loops run a few iterations per entry).
- Context displacement compaction: 39 of the 218 `noise_fbm` loop
  instructions carry a 32-bit `[r15 + disp]` (the V registers; Embree's
  loops 8-12). A global GuestContext reorder cannot help: x0-x31 fill the
  whole disp8 window, any V register moved in pushes X registers out. A
  second base register needs a free GPR in tier-2 regions, and the hot
  ones have none (noise_fbm, Embree: all nine pin hosts used). It changes
  bytes, not instructions (the loops fit the �op cache). Not done.
- Vector pin weights by reload distance: 0.977 vs 0.978, not kept.

Not measured: pybench (`fx_pybench_pair.ps1`), because of the foreign load;
the GPR aliases apply to scalar code but did not change the estimate.

## Reproduction tools

Build the measured configuration and run the comparisons from the repository
root. Use new output directories when preserving an earlier run:

```powershell
cmake -S . -B build-fx-investigate -G "Visual Studio 17 2022" -A x64 -DTRANSMUTE_FX_ONLY=ON -DTRANSMUTE_FX_CORPUS_TESTS=OFF
cmake --build build-fx-investigate --config Release --target transmute-fx -j4
./scripts/fx_classroom_study.ps1 -Runs 3 -Percent 10 -Samples 32 -OutDir artifacts/fx-investigate/repeat-single
./scripts/fx_classroom_study.ps1 -Configs native,default -Runs 3 -Percent 25 -Samples 16 -Threads 8 -Affinity 4095 -OutDir artifacts/fx-investigate/repeat-eight
./scripts/fx_pattern_bench.ps1 -Runs 3 -OutDir artifacts/fx-investigate/repeat-patterns
```

For another hardware-counter collection, run the following in an Administrator
PowerShell. A timestamped result directory is created automatically:

```powershell
Set-Location C:\dev\transmute
./scripts/fx_vtune_classroom.ps1
```

`scripts/fx_classroom_study.ps1` warms each configuration, alternates execution
order, records full Blender render time and the sample-zero to last-sample
interval, and saves every render log and output image. It rejects failed renders.

`scripts/fx_pattern_bench.ps1` builds `tests/fx/bench/bench_patterns.c` for ARM64
and x64, pins them to the same CPU, alternates their order, validates matching
checksums, and saves every timed observation. `-MaxRatio 1.25` is an optional
performance gate; a failure means at least one median paired ratio exceeds 1.25.
It is a performance experiment, not a stable machine-independent CI assertion.

`tools/fx_profile_hostmix.py` disassembles all dumped host regions in one pass,
instead of one tool run per region. The generated `.host.asm.txt` and the original fault maps remain available for
inspection with `tools/fx_profile_annotate.py`.

`scripts/fx_vtune_classroom.ps1` runs matching native and FX hardware-counter
collections. Windows requires the VTune sampling driver and an elevated shell.
See [Intel's driver instructions](https://www.intel.com/content/www/us/en/docs/vtune-profiler/user-guide/2025-4/install-sampling-drivers-for-windows-targets.html).
Use [Intel's top-down method](https://www.intel.com/content/www/us/en/docs/vtune-profiler/cookbook/2025-4/top-down-microarchitecture-analysis-method.html)
to distinguish instruction delivery, speculation, and execution or memory stalls.

## Correctness limits

Native and ARM64 Blender images are different builds with different CPU code
paths. The initial pair differed in 1,369 of 20,736 pixels, with mean absolute
channel difference 0.1194 and maximum 111 on the 8-bit output. A native image is
therefore not a bitwise oracle for a translator optimization. Compare an FX
experiment with baseline FX and use the existing differential instruction tests
before changing production semantics. The synthetic checksums test their finite
input sets, not NaNs, signed zero, exceptions, or arbitrary programs.
