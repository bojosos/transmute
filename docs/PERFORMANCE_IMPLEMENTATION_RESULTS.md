# Performance implementation results

Measurements from 30 September and 1 October 2026. The acceptance target is
about one second for the complete tiny Blender process. It remains unmet.
The full Classroom render and running the installer through Transmute are
also incomplete.

The latest normal Stage16 tiny process takes 88.127 seconds, versus a fresh
native x64 median of 0.997 seconds. All decoded pixels match. The corrected
runtime passes 468 named tests plus the ARM64 child-process check in 14.45
seconds. The remaining complete-process gap is about 88x. A controlled Q-memory
admission comparison identifies and removes a threading regression below;
none of these results establishes the requested 100x improvement.

## Implemented and checked

The frozen Stage11 runtime passed 428 named core tests across four parallel
test processes, plus the ARM64 child-process check. Validation took 36.31
seconds. Its SHA256 is
`83FBF73F1028B322FD0EAD147B00A5CABDFC3EB46989BF7731F7162342A5BF4B`.
The executable and validation record are retained in `artifacts`.

- Indexed FMUL/FMLA/FMLS now run natively for scalar S/D and vector
  2S/4S/2D forms, including every valid indexed element and source alias.
  Tests cover precise instruction budgets, FP control, flags and exits.
- Native blocks can chain without returning to C++ at each guest branch.
  Integer spans retain up to three guest registers in host registers.
  Shifted logical operations, division, CLZ and SIMD/FP movement and
  selection fill the previously observed instruction-family gaps.
- Checked bulk guest-memory operations now serve bridge copies, fills,
  strings, file I/O and CRT scanning. File I/O releases guest-memory leases
  before blocking on the host.
- Native owners share sealed exit code instead of embedding the same
  lookup and FP finalizer repeatedly. Tests cover instruction limits,
  faults, refill, collisions, FP restoration and invalidation.
- The phase probe measures startup to readiness, repeated rendering without
  saving, repeated saving, shutdown and complete execution independently.
  Its optional verification image is saved outside the rendering interval.

All translation remains implemented in-house.

## Complete tiny process

Factory Cube, 16 x 16 pixels, one sample and one CPU render thread. These
normal probes use JIT with profiling counters disabled.

| Preserved runtime | Whole process | Render through Finished | Saving | Remaining process time |
| --- | ---: | ---: | ---: | ---: |
| Stage10 | 142.489 s | 8.31 s | 25.86 s | 108.319 s |
| Stage9 control | 108.619 s | 23.54 s | 18.70 s | 66.379 s |
| Stage10 repeat | 127.505 s | 7.00 s | 26.70 s | 93.805 s |
| Stage9 control repeat | 145.222 s | 26.54 s | 19.94 s | 98.742 s |
| Stage11, first run | 120.620 s | 4.46 s | 13.36 s | 102.800 s |
| Stage13, first run | 152.940 s | 9.01 s | 25.34 s | 118.590 s |
| Stage14, first run, battery | 103.541 s | 6.93 s | 21.98 s | 74.631 s |
| Stage14, nearby control, AC | 122.069 s | 8.78 s | 36.21 s | 77.079 s |
| Stage15, first run, AC | 105.468 s | 8.43 s | 30.22 s | 66.818 s |
| Stage16, Q-exclusive policy, AC | 88.127 s | 7.58 s | 22.46 s | 58.087 s |

Remaining process time is the difference from Blender's reported rendering
and saving intervals. It includes startup, shutdown and other work.
Stage10's rendering is consistently shorter in this alternating cohort,
but total process time does not establish an improvement. Stage11 has only
one whole-process measurement so far. Do not attribute its result to either
of its two additional changes individually.

Fresh native tiny runs took 2.196, 1.630 and 1.549 seconds. Their median is
1.630 seconds. An earlier native cohort had a 0.795-second median. These
changes in native and preserved-runtime timings show material machine
variation. The one-second target stays unchanged. The latest Stage13 run
still required about 153x improvement to reach it. Stage14's first run
requires about 104x. It is shorter than the preceding Stage13 run, but the
power state changed and each checkpoint has only one current whole-process
measurement; this is not a controlled causal speedup.

The ARM probes recorded AC power, Balanced policy and battery saver off.
These records do not control temperature, frequency, core placement or
background applications. The shared workspace also has another active
development task. No OS power settings were changed.

Stage14's tiny run recorded battery power, Balanced policy, battery saver
off and 51% to 48% charge. The three subsequent native runs also used
battery power at 46% and took 2.209, 2.005 and 2.215 seconds, with a
2.209-second median. The current native gap is about 46.9x; the one-second
acceptance target remains about 104x away. Earlier AC timings are not a
matched power comparison.

After AC power returned, the preserved Stage14 executable took 122.069
seconds in a fresh normal control. Its record is
`artifacts/arm64-tiny-cycles-20261001-022243.json`. The changing saving and
whole-process times reinforce the need for nearby controls and repeat runs.

Stage15's normal AC run took 105.468 seconds, about 105x from the one-second
target. It is 13.6% shorter than the nearby 122.069-second Stage14 AC control,
but each is a single process run and the source batch contains several changes.
This does not isolate a causal speedup or establish a 100x improvement. Power
was AC, Balanced and battery saver off, with charge increasing from 59% to 60%.
The raw record is `artifacts/arm64-tiny-cycles-20261001-025154.json`.

After the nearby Q-admission control, three fresh native AC tiny processes
took 1.071, 0.983 and 0.997 seconds, with a 0.997-second median. This supports
using one second as the complete-process target. The Stage15 normal tiny run
is about 105.7x slower than this native median. All three native images match
its decoded pixels. The record is `artifacts/tiny-cycles-native-stage16.json`.

The corrected Stage16 runtime completes the normal tiny process in 88.127
seconds, with exactly matching native pixels. Its SHA256 is
`3BD8875B8CEAD3BCC2F24F5F6D59129D814E61563E7905B82094043FF1D8CC53`.
This is about 88x from the one-second target and 88.4x the fresh native median.
It is a single whole-process run; the nearby isolated Q control supplies the
stronger evidence for the threading policy correction. Its record is
`artifacts/arm64-tiny-cycles-20261001-033657.json`, and the image comparison is
`artifacts/blender-stage16-image-validation.json`.

Raw records are `artifacts/tiny-cycles-stage9-stage10-alternating.json`,
`artifacts/tiny-cycles-native-stage10.json` and
`artifacts/arm64-tiny-cycles-20261001-001342.json`. The Stage13 record is
`artifacts/arm64-tiny-cycles-20261001-011810.json`.
Stage14 records are `artifacts/arm64-tiny-cycles-20261001-015719.json` and
`artifacts/tiny-cycles-native-stage14.json`.

## Separate rendering and threading

The phase workload uses a 64 x 64 factory Cube, one sample, two rendering
calls in the same process and a verification image saved afterward.

| Runtime and render threads | First render | Second render | Before ready | After finished |
| --- | ---: | ---: | ---: | ---: |
| Native x64, 1 | 0.206 s | 0.182 s | 0.759 s | 0.151 s |
| Native x64, 8 | 0.135 s | 0.116 s | 0.636 s | 0.141 s |
| ARM64 Stage11, 1 | 16.259 s | 15.466 s | 136.657 s | 16.291 s |
| ARM64 Stage11, 8 | 79.914 s | 59.168 s | 116.904 s | 17.988 s |
| Native x64, 8, fresh | 0.133 s | 0.135 s | 1.151 s | 0.163 s |
| ARM64 Stage13, 8 | 108.251 s | 81.629 s | 48.142 s | 17.971 s |
| ARM64 Stage14, 8, battery | 52.890 s | 52.208 s | 88.496 s | 19.784 s |
| ARM64 Stage14, 1, battery | 25.271 s | 21.202 s | 64.534 s | 18.421 s |
| ARM64 Stage15, 1, AC | 25.308 s | 25.018 s | 45.268 s | 16.489 s |
| ARM64 Stage15, 8, AC | 211.467 s | 210.432 s | 51.205 s | 19.284 s |
| ARM64 Stage14, 8, nearby AC control | 67.178 s | 71.899 s | 55.824 s | 24.519 s |
| Stage15, Q-exclusive diagnostic, 8, AC | 72.048 s | 58.336 s | 51.529 s | 18.437 s |
| Native x64, 1, latest AC | 0.163 s | 0.125 s | 0.546 s | 0.129 s |
| Native x64, 8, latest AC | 0.100 s | 0.067 s | 0.498 s | 0.125 s |

The ARM64 verification saves took 11.312 and 22.425 seconds for one and eight
threads, excluded from the render columns. Each row is one process with two
render calls, not an independent
two-process cohort. The single-thread rendering gap is about 79-85x.
Startup and shutdown remain substantial costs even after reducing rendering.
Eight ARM64 render threads were 3.8-4.9x slower than one in this cohort,
while native benefited from eight threads. Active workers consumed CPU
throughout the ARM render. This establishes a scaling regression to diagnose,
not its cause. Reader admission, exclusive operations, memory bookkeeping
and actual arithmetic all remain candidates until sampled separately.

The Stage13 eight-thread process took 272.232 seconds including its
16.110-second verification save. Its setup was shorter than the earlier
Stage11 process, but both render intervals were longer. Reader parking has
not established a rendering improvement. These separate process runs do
not control machine variation or isolate each source change. Raw records
are `artifacts/phase-native-render-20261001-011012.json` and
`artifacts/phase-arm64-render-20261001-011014.json`.
Stage14's battery-powered eight-thread process took 234.437 seconds,
including a 20.938-second verification save. The subsequent one-thread
process took 148.940 seconds, including a 19.428-second verification save.
Eight threads remain about 2.1-2.5x slower in these render intervals. Both
processes ran on battery, with charge falling from 46% to 40% and from 35%
to 31%, respectively; these are separate runs rather than a matched cohort.
Their raw records are `artifacts/phase-arm64-render-20261001-020010.json`
and `artifacts/phase-arm64-render-20261001-020721.json`.

Stage15's one-thread AC process took 136.529 seconds, including a separate
24.355-second verification save. Its two rendering intervals do not establish
an improvement over Stage14. The record is
`artifacts/phase-arm64-render-20261001-025622.json`.

Stage15's eight-thread AC process took 519.505 seconds, including a separate
27.011-second verification save. Both render calls are about 8.4x slower than
Stage15's one-thread calls. This is a substantial regression relative to the
preceding checkpoint and must be investigated before claiming a threading
improvement. A nearby preserved Stage14 AC control is needed to separate the
source batch from machine drift; no individual change is blamed from this run.
The record is `artifacts/phase-arm64-render-20261001-025839.json`.

The subsequent preserved Stage14 AC control took 260.667 seconds, with render
calls of 67.178 and 71.899 seconds. Stage15's calls are about 2.9-3.1x slower.
Both used AC, Balanced policy and battery saver off. This strengthens the
source-batch regression evidence, though separate runs still do not isolate
its cause. The record is `artifacts/phase-arm64-render-20261001-030945.json`.

An isolated diagnostic changes only Q admission from `element_width <= 16`
to `<= 8`. The modified backend object links ahead of the frozen Stage15
static library and original main object; all other source changes, including
split 64-bit Q operations and selective invalidation, are retained. Its two
renders take 72.048 and 58.336 seconds, about 2.9x and 3.6x faster than the
shared-Q calls. Complete execution takes 224.552 seconds, including a
24.060-second verification save, and all native pixels match. This implicates
the shared Q path in the regression. It does not distinguish journal mutex
contention from alignment promotion, both of which the policy changes.

The diagnostic SHA256 is
`10C6DC829416B2826941E0379C2D50A6330E82B815F9527BD5D8B23E6491EE4A`.
Its build record and exact response files are retained in
`artifacts/stage15-q-serialized-diagnostic.json`. Timing is recorded in
`artifacts/phase-arm64-render-20261001-031837.json`.

The latest native AC phase controls use the same 64 x 64 render/verification
workload. One-thread render calls take 0.163/0.125 seconds; eight-thread calls
take 0.100/0.067 seconds. Their complete processes take 1.072/0.913 seconds.
Raw records are `artifacts/phase-native-render-20261001-032311.json` and
`artifacts/phase-native-render-20261001-032312.json`. Even after the Q policy
correction, translated rendering and thread scaling remain far from native.

All six preserved Stage10/Stage11 and alternating control tiny images match
native decoded RGBA pixels exactly. The completed one-thread ARM64 64 x 64
images at one and eight threads also match all 4,096 native pixels exactly.
Validation is recorded
in `artifacts/blender-stage10-stage11-image-validation.json`.
The Stage13 eight-thread image matches all 4,096 native RGBA pixels and its
tiny image matches all 256. The separate record is
`artifacts/blender-stage13-image-validation.json`.
Stage14's tiny, one-thread and eight-thread images also match all native pixels exactly;
the record is `artifacts/blender-stage14-image-validation.json`.
Stage15's normal tiny, one-thread and eight-thread images also match every
native decoded RGBA pixel exactly. Its record is
`artifacts/blender-stage15-image-validation.json`.

## Diagnostic counters and next decisions

The separate Stage10 main-thread diagnostic records 47.918 million C++
native-frame visits versus 283.055 million in the older Stage9 diagnostic,
about 5.9x fewer. It counts 218.693 million direct native chain edges and
2.113 billion native guest instructions. These are instrumented counters,
not a whole-process speedup or worker-thread profile.

Stage10 also emitted 410.2 MB of cumulative code with four arena
reclamations, 12.713 seconds in its compilation timer and 2.411 seconds in
its decode timer. This motivated the shared exit code in Stage11. Stage11's
actual cumulative emitted bytes have not yet been remeasured. Persistent
AOT can address compilation and reclamation, but those timers alone cannot
explain the remaining whole-process gap.

The diagnostic reports 32.334 million read misses and 34.163 million store
misses. Those include rejected refills, not just cache collisions. The next
memory experiment first classifies misses before changing table capacity
or invalidation policy. Actual multicore rendering and CPU/wait evidence
will determine the threading priority. Further hot-code work must reduce
register/vector state traffic and per-operation guards while preserving
precise observable exits.

The separate Stage13 main-thread diagnostic now classifies those misses.
Of 32.772 million read refills, 24.736 million are collisions and 8.024
million follow topology invalidation; only 12,955 are rejected. Of 34.166
million store refills, 28.768 million are rejected image writes, while
5.384 million succeed. A rejected image store returns to checked execution
under exclusive ownership, making published writable PE data a more direct
priority than a new guest-heap arena. These are main-thread refill counts,
not all memory-access counts or worker-thread profiles.

Stage13 emitted 365.817 MB cumulatively, with three arena reclamations,
9.054 seconds in compilation and 2.212 seconds in decoding. These counters
come from `artifacts/arm64-tiny-cycles-20261001-012119.log`; its 136.391-second
process time is instrumented and excluded from the normal timing tables.
The new GP conversions no longer appear among its hottest sampled fallback
PCs. Compilation alone still cannot explain the remaining gap.

The Stage14 main-thread diagnostic records 13.146 million read misses and
4.368 million store misses. Published image-store rejection falls to 112
attempts, from 28.768 million in Stage13. Read collisions fall to 2.196
million and store collisions to 0.277 million. Topology invalidation now
accounts for 10.936 million read refills and 4.076 million store refills,
about 85.7% of all refill attempts. A bounded range-change history is the
next implementation experiment: retain unaffected spans and conservatively
flush if an engine falls behind the recorded history.

Stage14 emitted 374.140 MB cumulatively, with three arena reclamations,
8.984 seconds in compilation and 0.401 seconds in decoding. Profiling now
propagates to workers; process counters report 25 profiled worker caches,
522,721 inherited native owners and a peak of six overlapping shared leases.
Main-thread fallback samples still contain LL/SC, REV and EXTR. Native
translation of the complete legal REV/EXTR families and shared execution of
aligned Q memory operations were the next Stage15 implementation targets.
These measurements are from
`artifacts/arm64-tiny-cycles-20261001-020951.log`; its 132.835-second elapsed
time is instrumented and excluded from normal performance ratios. The
laptop changed from battery to AC power during this diagnostic.

The Stage15 main-thread diagnostic reduces read/store refill attempts to
3.764/0.920 million, versus Stage14's 13.146/4.368 million. Stale classifications
fall to 0.377/0.312 million; collisions are now 3.374/0.593 million. Selective
invalidation reports 804,506 refreshes, 818,248 replayed changes, 496.483 million
retained live-entry observations and three full flushes. Retention reduces
refills but scanning the tables is a separate cost requiring measurement.

Serialized-access exits rise from 192,107 to 1,053,828, about 5.5x, while native
guest instructions stay near 2.097 billion. Frequent misaligned Q accesses can
now attempt shared admission, exit and retry under exclusive ownership. This
is a candidate for the eight-thread regression, alongside journal contention
and topology scans; main-thread counters alone do not establish its cause.
REV/EXTR no longer appear in the hottest sampled fallbacks. LL/SC, literal
loads and SIMD operations remain. Cumulative code emission is 366.633 MB,
compilation is 11.999 seconds and decoding 0.528 seconds, with three arena
reclamations. The 128.947-second diagnostic is excluded from normal timing
ratios. Raw counters are
`artifacts/arm64-tiny-cycles-20261001-030719.log`.

The earlier CPU evidence and implementation acceptance gates are recorded
in [the performance assessment](PERFORMANCE_NEXT_STEPS.md).

## Follow-up implementation and the larger memory change

The subsequent source batch replaces reader admission's repeated yields with
an atomic wait notified when the pending writer queue drains. It retains
reader/writer ownership, nested exclusive calls and the existing backing
lifetime and invalidation policy. A concurrent wakeup fixture exercises
multiple writers and parked readers, including queue-drain races.

CPU-filtered samples from the separate eight-thread diagnostic frequently
capture that yield path. Of 2,284 render-status samples, 1,129 contain
`SwitchToThread` and 1,590 contain reader admission. These are overlapping
inclusive counts. The sampler checks that CPU time advanced before capturing
a stack; the thread may already be waiting at capture. These counts establish
a frequent path, not exact CPU-time or blocked-wall percentages. Raw evidence
is `artifacts/blender-stage12-eight-profile-summary.json`.

The same source batch compacts integer-register address encodings and omits
unreachable fallthrough tails after unconditional terminal instructions.
It also implements the complete GP SCVTF/UCVTF W/X-to-S/D family, including
every legal fixed-point scale. An OCIO loop in the measured fallback PCs
contains three of these conversions per iteration. Independent ARM64
assembly confirms the opcode forms; differential fixtures cover rounding,
unsigned high-bit values, FP status, ZR, precise budgets and session exits.
The frozen Stage13 runtime passed 438 named tests across four processes,
plus the ARM64 child-process gate, in 16.68 seconds. Its SHA256 is
`7B94662815DBD56BD061422AE1A56B1A2283AA27D6DD34614BD2F3EA203EBBBD`.
Normal Blender timing of this batch is recorded above. The validation
record is `artifacts/transmute-perf-stage13-validation.json`. A separate
counter-enabled replay supplies the refill classifications above; its
timing is not used as normal performance evidence.

The first three simultaneous focused suites produced one failed LSE overlap
assertion. Atomic results and revision checks had passed before that assertion.
The isolated retry, the same three-suite concurrent retry and both full test
runs passed. This scheduling-sensitive overlap observation is retained in the
validation record; it has not been weakened or removed from the test.

## Measured-refill implementation, Stage14

The subsequent frozen runtime passed 455 named tests across four processes
plus the ARM64 child-process check in 34.10 seconds. Its SHA256 is
`F85B435EAB735C75F6C92CB2C39EC30EED206EEB35FC6F4B700CE607F498B474`.
The record is `artifacts/transmute-perf-stage14-validation.json`.

- Native writes now accept complete spans in published writable,
  non-executable PE sections. Tagged journal targets retain the generated
  entry layout and publish every written byte to the exclusive-monitor
  history. PE data keeps its existing untracked write-revision contract.
  Staged modules, protection failures and boundary crossings stay checked.
- Naturally aligned W/X pairs and B/H/S/D SIMD memory owners can run under
  shared execution leases. Each host access is at most eight bytes; full
  span checks and journal capacity precede effects. Misalignment promotes
  only the faulting instruction to exclusive execution.
- Read and write span caches grow from 64 to 1,024 slots and live on the
  heap. Topology changes advance their epochs without clearing all entries;
  both tables are cleared before an epoch value is reused on wraparound.
- Workers can inherit current immutable native PE owners from the parent.
  Exact words and published executable/nonwritable provenance are checked
  while the address space is quiescent. Shared RX chunks are permanently
  sealed; worker dispatch, FP and memory contexts remain private. Parent
  append, invalidation and destruction retain independent worker ownership.
  This is in-process code reuse; persistent AOT is not implemented.
- Critical-section mirroring submits its original six typed field writes
  under one gate admission. The 40-byte extent, six write revisions,
  virtual overrides and successful prefix before a fault are preserved.
  Explicit profiling now propagates to workers and reports process-wide
  shared-lease and inherited-owner counts; normal runs disable it.

Focused fixtures check budget/resume, FP restoration, code-sharing lifetime
and exclusions, protection/partial faults, aligned element contention,
publication and identical/net-zero monitor history. Independent LLVM
assembly confirms the new pair/SIMD witness encodings. Initial fixture
setup failures and the corrected GUI registration path are retained in the
validation record. No production checks were weakened.

## Selective invalidation and native families, Stage15

The frozen runtime passed 467 named tests across four processes plus the
ARM64 child-process check in 19.64 seconds. Focused native-chain and concurrent
region suites also passed. Its SHA256 is
`86D141B1CB45DC32382FD1B79CB8E6E319D2B72057CFAA122D6EBF4959D1A400`.
The validation record is `artifacts/transmute-perf-stage15-validation.json`.

- A bounded 512-record topology history retains native read/write spans whose
  complete extents do not overlap backing or permission changes. Metadata-only
  changes retain spans without scanning them. Missing history, sequence/era
  rollover and unusable history take conservative full-flush paths; tables are
  cleared before an epoch value is reused. Generated entry layouts are unchanged.
- Private staged PE memory now exposes only contiguous intervals not shadowed
  by shared mappings. Checked whole-width and bulk paths preserve permissions,
  fault prefixes and mapping selection across both shadow boundaries. Review
  found this existing correctness gap while checking selective invalidation;
  dedicated tests cover replacement, partial shadowing, strings and LL/SC history.
- Naturally eight-byte-aligned Q loads/stores and Q pairs can execute under
  shared leases. Each Q uses two explicit 64-bit host accesses; complete span
  and journal-capacity checks precede effects. This preserves the baseline ARM
  single-copy atomicity of each aligned 64-bit element without assuming atomic
  128-bit accesses. Misalignment retains precise checked execution.
- Every legal REV/REV16/REV32 W/X form and EXTR W/X shift now lowers directly to
  x64 instructions. Reserved encodings remain rejected. Independent assembly
  and byte/bit oracles cover aliases, ZR, width, precise budgets and fault resume.
- Native span guards use an unsigned offset and remaining-size proof, removing
  redundant loads, instructions and one branch. Differential cases exercise all
  supported access widths, undersized spans and addresses near UINT64_MAX.

Tests preserve full guest state and warmed native-body host FP control/status.
Cold compilation is tested separately because its C++ timing arithmetic can
set the host inexact flag. Fixture setup corrections are retained in the
validation record; production checks were not relaxed.

## Measured Q policy correction, Stage16

Q memory owners now remain native under exclusive admission, retaining both
explicit 64-bit accesses per Q, complete span proofs, operand snapshots and
one 16-byte journal record per Q. W/X pairs and B/H/S/D shared admission remain
unchanged. The isolated comparison above establishes this as a conservative
regression correction; it does not establish which Q-path cost dominates.

All Q differential cases remain: aligned and misaligned guest/host addresses,
state, lanes, precise budgets, partial faults, contention, equal-value writes
and exclusive-monitor history. Admission checks now explicitly require Q
exclusion. A shared register-only source chained toward a Q target must stop
before Q effects, promote, and preserve its exact five-step state/history.

The frozen Stage16 runtime passes 468 named tests across four processes and
the ARM64 child-process check in 14.45 seconds. Both focused native-chain and
concurrent-region selectors also pass. Its validation and full test log are
`artifacts/transmute-perf-stage16-validation.json` and
`artifacts/transmute-perf-stage16-ctest.log`.

The additional named test is a concurrently added OpenGL draw fixture. Its
initial indirect-draw failure is retained in the first test log. A dedicated
command buffer and native differential checks now verify binding, extent,
mapping state, uploaded words, native drawing and forwarded drawing, each
with an independent pixel assertion. No error checks were suppressed. The
shared workspace's temporary missing Alchemy source files required building
the existing generated compiler/link targets directly; archive and executable
timestamps were checked before validation.

## Remaining execution overhead

The Stage14 diagnostic counts 223.135 million native chain edges for
2.097 billion native guest instructions, about 9.4 instructions per edge.
There are also 26.354 million C++ native-frame visits. Repeated state
materialization, memory guards and dispatch remain substantial candidates
after compilation and refill improvements.

The isolated scalar-loop control runs two million iterations per worker,
three rounds, with profiling disabled and disjoint worker pages. Read-only
wall-time medians are 35.544 ms for one worker and 77.891 ms for eight.
Store-loop medians are 60.714 ms and 808.872 ms, respectively. Eight workers
perform eight times the work, so reads gain aggregate throughput but stores
lose it. All worker pages share one owned mapping and its revision metadata.
The raw record is `artifacts/concurrent-regions-stage14-2m-3rounds.csv`.
This establishes a store-scaling problem in that loop, not the fraction of
Blender time spent there or a predicted whole-process speedup.

The same Stage15 scalar benchmark, with profiling disabled, has median read
times of 22.241 ms for one worker and 43.227 ms for eight, and store times of
31.749 ms and 614.889 ms. Eight store workers perform eight times the work but
take about 19.4 times as long, yielding about 0.41x single-worker aggregate
throughput. Store publication and shared revision metadata remain a concrete
scaling problem. Separate noisy source checkpoints cannot assign this change
to one optimization. Raw data is
`artifacts/concurrent-regions-stage15-2m-3rounds.csv`.

Three bounded follow-ups address repeated execution work:

- Aggregate journal records with one token and one monitor bucket before
  publication. Preserve one revision ordinal per owned store, PE's untracked
  revision policy, every byte epoch, reservation invalidation and exhaustion.
  Longer shared native frames need the existing 256-step pending-writer
  checkpoint, rather than merely extending the fairness bound.
- Reuse one complete bounds proof for two to eight constant-offset accesses
  through an unchanged base. Failed proofs and short budgets must select
  existing per-instruction paths; preserve original effects and journal order.
- Retain SIMD values across bounded integer arithmetic spans, flushing dirty
  guest registers at exits. Memory, FP policy, flags and callbacks initially
  remain barriers. Verify aliases, upper-lane clearing and every budget suffix.

These follow-ups are designs, not implemented performance claims. Persistent
AOT remains incomplete and cannot by itself remove these execution costs.

The parallel audit narrows the next experiments:

- Replace repeated full-table invalidation scans with candidate slots selected
  from changed guest pages, followed by exact overlap checks. PE slots require
  a separate exception bitmap because their cached spans can extend beyond the
  fill page. Publication currently needs a further overlap audit: a privately
  staged image can encounter another worker's mapping before publication.
  Conservatively include all PE read/write slots until that invariant is repaired.
  Large ranges and missing history retain bounded full-scan/epoch fallback.
- Keep Q admission exclusive after its controlled regression. Failed-alignment
  memoization is a separate experiment: restrict the precise failing owner,
  clear previously published shared-chain flags, keep worker metadata private,
  and test chained suffixes and aligned access after restriction. The Q policy
  control does not prove this alone removes journal contention.
- Start vector retention with two guest vectors in XMM4/5 across two to eight
  pure integer-SIMD instructions. Existing binary SIMD fragments transfer 48
  bytes of CpuState data; Q FMA transfers 64 bytes. Typed operand metadata,
  independent suffix-entry reloads and exact dirty-state exits are required.
  Measure both state accesses and code size; dense alias thunks can erase gains.
- Fill native integer literal LDR W/X and LDRSW for every signed displacement,
  preserving runtime reads, Rd31 faults and precise boundaries. Legacy decoding
  needs a separate literal-addressing fix in its checked memory helper.
- Implement complete signed/unsigned widening MULL/MLAL/MLSL lower/upper register
  families and XTN/XTN2. Independent LLVM identifies the remaining sampled UMLAL
  and XTN forms. Include wrapping, aliases, upper-half selection and unchanged
  FP status; saturating and indexed forms need explicit separate scope.
- Add native LDXR/LDAXR/STXR/STLXR across all four widths and CLREX using the
  existing monitor identity and byte-epoch contract. Commit pending journals
  before reservation capture/check, preserve equal-value/remap invalidation,
  and clear the reservation on either STXR outcome. Ordinary CAS is insufficient.

These are family and execution-path changes, not PC-specific substitutions or
forecasts of a 100x speedup. Normal Blender timings and exact output remain the
acceptance evidence. Cached/AOT startup translation must also avoid retaining
raw host pointers, and key code by guest image/words, translator ABI, lowering
policy and required host ISA before reconstructing per-process dispatch state.

A larger direct-memory tier is a concrete next experiment. Start with a
private reserved host arena and a fixed offset for the first four GiB of
the guest heap range. Allocation is monotonic and can exceed this prefix.
Give eligible whole-page owned heap mappings canonical
VM backing used by every checked helper and native lease. Freed and
ineligible pages stay inaccessible. Do not keep a copied mirror or assume
that all guest addresses can also be host addresses.

After backing and lifecycle tests pass, add naturally aligned scalar loads
of at most eight bytes. The generated path proves the guest heap window,
then loads from the arena directly. A narrow fault handler recognizes only
registered translated memory sites in the active frame and its owned arena.
It records the exact guest fault instruction and exits to checked execution,
which performs that instruction once. The handler must not allocate, acquire
locks or consume unrelated host exceptions. Normal valid accesses avoid the
current table selection, epoch, bounds and pointer reconstruction checks.

Add stores only after load fault recovery passes. Check journal capacity
before the write and publish the same mapping token afterward. Identical
writes and net-zero writes must still update the exclusive-monitor history.
Page protection alone cannot replace that bookkeeping. Keep current gates
through protect, unmap, remap and backing changes, retaining mapping identity
where required and invalidating spans through their epochs.

The first tier excludes external and PE/staged image memory, partial pages,
write-only or executable mappings, unaligned accesses, pair/vector operations
and atomics. Separate address spaces require separate arenas even when their
guest addresses match. This experiment can reduce memory guards; it does not
remove register-state traffic, branch dispatch, FP policy or compilation.
Measure the fraction of actual Blender accesses it covers before committing
to a wider redesign or predicting a speedup.
