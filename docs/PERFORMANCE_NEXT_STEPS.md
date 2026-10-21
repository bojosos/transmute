# Performance assessment and next steps

Measured on 30 September 2026. New optimization work stopped for this audit.
The subsequent implemented batch and 1 October measurements are recorded in
[performance implementation results](PERFORMANCE_IMPLEMENTATION_RESULTS.md).
The subsequent implementation request sets the tiny whole-process acceptance
target to about one second. That needs approximately 130-144x improvement
from the fresh normal ARM timings. The native comparison gap is 164-182x.
The already-started shared-LSE work passed 405 named tests across four
processes plus the ARM64 child-process test in 17.79 seconds. The frozen
Stage9 executable and its validation record are retained in `artifacts`.
This validates that checkpoint, not subsequent changes from other tasks.

## What the current measurements say

The workload is Blender 4.5.14 LTS, factory Cube, 16 x 16 pixels, one sample,
one CPU rendering thread. All preserved outputs from these runs have the
same decoded RGBA pixels as the fresh native x64 output.

| Normal execution | Whole process | Render through Finished | Saving | Other process time |
| --- | ---: | ---: | ---: | ---: |
| Stage8, earlier | 94.304 s | 19.66 s | 15.12 s | 59.37 s |
| Stage9, fresh | 144.321 s | 36.12 s | 29.66 s | 78.20 s |
| Stage8, fresh repeat after Stage9 | 130.280 s | 24.27 s | 21.04 s | 84.76 s |
| Native x64, fresh three-run median | 0.795 s | See native logs | See native logs | See native logs |

Other time is the difference between process wall time and Blender's reported
render/save interval. It includes startup, shutdown and unclassified work.
It must not all be labelled startup. Small unclassified gaps inside Blender's
render/save interval are excluded from this column.

The fresh Stage9 run is 10.8% slower than the fresh Stage8 repeat. One run
per binary cannot establish a code regression. The same Stage8 binary also
changed from 94.304 to 130.280 seconds, so timings vary materially even without
a code change.
Every run recorded AC power, Balanced policy and battery saver off. Those
fields do not control CPU frequency, thermal throttling, core placement or
other applications. No OS power settings were changed.

Earlier alternating Stage5/Stage7 runs averaged 86.899/86.250 seconds.
That is no useful Blender gain, despite large synthetic improvements. The
earlier native median was 0.688 seconds. The tiny workload remains roughly
164-182 times slower than the latest native median in the two fresh ARM runs.

Raw records and pixel checks are in
`artifacts/blender-performance-stepback.json`,
`artifacts/tiny-cycles-stage5-stage7-ac-alternating.json`, and
`artifacts/tiny-cycles-native-current2.json`.

## Where CPU time goes

A normal JIT run with profiling counters disabled was sampled in short
windows using an optimized symbol build with Release's `/Ob2` inlining.
Each window selects one recently busiest thread, then samples that thread.
Suspension and stack walking perturb timing. The 160.696-second sampled run
is diagnostic evidence and is excluded from the normal timing table.

| Sampled phase | Evidence | Consequence |
| --- | --- | --- |
| Startup windows 0-5, 2,197 samples | 34.1% leaf CPU in dispatch/cache lookup; 11.3% in refill/commit; 23.6% in anonymous code | Returning to C++ and rebuilding execution context costs more than the generated guest code in these windows |
| Render worker windows 8 and 10, 644 samples | 48.8% leaf CPU in software fused arithmetic; 3.6% in anonymous code | A currently interpreted arithmetic family breaks the native render path |
| Saving windows 12-15, 1,507 samples | About 20% in EX gate acquisition/release and constructor; 15.7% anonymous code; 34.7% inclusive bridge CPU | Bulk guest-memory access and fewer transitions are concrete saving candidates |

Leaf percentages count disjoint instruction locations. Inclusive bridge
percentages overlap other rows and must not be added to them. Compiler
inlining can move lease/mutex work into its caller; broader grouping gives
about 24% saving gate CPU. These are sampled CPU shares, not process wall
shares or measurements of blocked time.

A second targeted diagnostic retained source-line buckets. The material
software-FMA caller is `ReferenceEngine::execute+0x183b`, `src/a64.cpp:3741`,
with 65 inclusive samples. This is indexed FMLA/FMLS. No material scalar
register, ordinary vector register or reciprocal-step caller appeared in
that window. Host FMA and AVX2 availability were verified independently.

The separate main-thread stats run recorded:

- 2.101 billion native guest instructions in 283.055 million block visits,
  about 7.42 instructions per visit.
- 32.368 million native read misses and 13.744 million store misses.
- 312,938 compiled blocks, 347.6 MB cumulative emitted code and three arena
  reclamations, with 54.9 MB live emitted code at completion.
- 23.067 million interpreted instructions, about 1.1% of main-thread guest
  instructions. This does not quantify the render worker's fallback cost.
- 6.652 seconds in the compilation timer. Fetch/decode and some allocations
  happen outside that timer. It is not the total cost removable by AOT.

Main-thread stats omit workers and add per-visit counting overhead. Their
179.750-second process time is also diagnostic, not a speed benchmark.
Front-cache hit counters omit predicate lookups such as `peek_hot`.

Raw evidence is `artifacts/blender-stage9-profile-summary.json`,
`artifacts/blender-stage9-normal-cpu.json`,
`artifacts/blender-stage9-fma-callers-02.txt`, and
`artifacts/arm64-tiny-cycles-20260930-222642.log`.

## Recommended implementation sequence

1. Recover the measured native render path. Implement the complete indexed
   FMUL/FMLA/FMLS S/D family using the existing FP session and side-exit
   contract. Include scalar S/D and vector 2S/4S/2D arrangements and every
   valid indexed element. Preserve aliased source snapshots, inactive upper
   lanes, selected-element FZ checks, rounding and FPSR behavior. Native FMA
   is required only for fused forms. Half-precision and reserved encodings
   remain outside this batch. Then remove the surrounding region breaks.
   Acceptance: software-FMA leaf share below 5% on the same worker path and
   at least 25% lower normal render time, with unchanged precise exits and
   output. These are engineering thresholds, not promised gains.

2. Replace frequent C++ returns with bounded native chaining over canonical
   hot blocks. Retain execution/memory/FP context across linked blocks and
   refill side exits. Cover hot pure-control loops as well as memory regions.
   Keep instruction budgets, writer fairness, invalidation and exception
   state precise at exits. Remove duplicate lookup and repeated temporary
   decoding. Complete coherent families that currently break main-thread
   regions: W/X shifted logical operations, SDIV/UDIV and CLZ/CLS, followed by
   SIMD/FP movement and selection such as DUP, MOVI/MVNI, FMOV and FCSEL.
   The fresh top ten fallback PCs include shifted AND/ORR, signed/unsigned
   division, CLZ, DUP, MOVI and FCSEL. Counts identify frequent instructions,
   not their CPU share. Preserve ARM division-by-zero and signed-overflow
   results instead of allowing host DIV traps. Movement and selection need
   no FP environment change. Add register/vector/flags allocation within this hot tier,
   committing dirty guest state at observable exits. This avoids building
   a second optimizer around the current per-instruction memory traffic.
   Acceptance: at least four times fewer C++ visits per guest instruction,
   cache lookup below 5% of sampled startup CPU, at least 20% lower normal
   startup time, and less emitted code for the same hot trace.

3. Give bridge helpers bulk guest-memory snapshot/copy operations. Validate
   and pin each extent once rather than reacquiring execution ownership for
   individual bytes or eight-byte chunks. Preserve partial-fault behavior,
   mapping identity, dirty revisions, streaming cursor semantics and callback
   safety. Release memory leases before blocking host I/O. Focus first on
   the actual PNG/OCIO/CRT callers in the saving profile.
   Acceptance: ten times fewer gate acquisitions per transferred MiB,
   saving gate CPU below 10%, at least 20% lower normal saving time, and exact
   image output.

These three tracks can be developed in parallel after their interfaces and
file ownership are agreed. Heavy timing runs must be serialized. Run narrow
correctness fixtures during development, the isolated full test shards at
integration, then three alternating baseline/candidate process runs. A
synthetic gain without a phase or whole-process gain does not pass the
performance gate.

## Threading, AOT and the one-second target

Guest workers already are native Windows threads. The one-thread probe
cannot establish multicore scaling. Eight-worker synthetic stores still
take about 205 ms against about 12 ms for one worker. Shared journal metadata
and the conservative EX policy for every pair/vector-memory owner remain
real scaling candidates. Journal CPU is only about 0.2% in the sampled render
worker and 2.3% during saving, so journal grouping is a secondary tiny-scene
priority. Measure an actual eight-thread Cycles phase and wait stacks before
ranking it against native arithmetic and dispatch work.

Add separate workload gates for startup-only, repeated renders in one
process, repeated saves without rendering, and multi-thread rendering.
Keep the whole tiny process as an integration gate. Use a reduced Classroom
scene for frequent scaling checks, followed by the unchanged full Classroom
acceptance run. The full scene and installer are still incomplete.

Persistent AOT should share canonical hot code, relocations, versioning and
invalidation with the JIT. It can reduce cold translation and cache churn.
It does not remove software arithmetic, C++ returns or byte-wise bridge
access. Build that code representation first, then measure cold and warm
launches separately. A larger arena by itself only postpones reclamation.

For the earlier 94.304-second reference, eliminating rendering entirely
would yield only about 1.26x overall; leaving the 15.12-second saving cost
unchanged caps overall speedup below 6.24x. A 10-20x whole-process result
therefore requires broad reductions across all phases. Even making 95% of
work ten times faster yields only about 6.9x overall:

`speedup = 1 / ((1 - affected_fraction) + affected_fraction / local_speedup)`

The final tiny acceptance target is about one second for the whole process.
Compare new runs with contemporaneous preserved-binary controls. The fresh
130-144-second cohort needs its own stable reference. The measurements
justify the sequence above; they do not yet support a 130-144x forecast.

Keep the optimizer and generated-code format independent of PE/Win32.
Platform loaders, import adapters, thread identity and exception translation
belong to the Windows or future Darwin personality. This preserves the
future ARM64 macOS-on-x64-Windows path without delaying the current measured
Windows costs.
