# fx performance, wave 2

Wave 2 made NZCV exact by default (correctness first), then won the speed back. Wave 1's landing note is
[FX_PERF_WAVE1.md](FX_PERF_WAVE1.md); the suite and method are in [FX_PERF.md](FX_PERF.md).

## What landed (rounds 11-16, main 79654111 .. dec12aa7)

| Round | Change | Why |
| --- | --- | --- |
| 11-12 | BR exits keep NZCV live again (wave 1's BR flag-dead default reverted); one RBIT->TZCNT fusion at a time | dav1d's hand-written NEON (splat_mv, cfl_ac) broke |
| 13 | Exact NZCV at every exit by default (`abi_flags = false`) | dav1d's save_tmvs (`cmp; blr; b.ge`) broke under the AAPCS64 flags model: YouTube AV1 crashed in Chrome |
| 14 | Raw host flags at exits, converted only when read (`norawflags` turns it off); NEON: PMADDWD for by-element multiplies, rounding shifts, byte USHL, one-instruction high-half widening | Win back exact flags; dav1d |
| 15 | Table shadows for BRs through heap dispatch tables (`noshadows`); `___chkstk_darwin` as a leaf (`ALCHEMY_FX_STACK_PROBE_LEAF=0`); the `abiflags` measurement knob | Interpreter dispatch; Cycles' 38.9M stack probes |
| 16 | Direct exits skip their flag store when the target writes NZCV before reading it: a scan of the target's image code (`nodeadtargets`) and a link-time skip (`noflagskip`); cache format 6; NEON element broadcasts precomputed (`noelemderived`) | Win back exact flags |

Every round passed the gate: full build, ctest, deep differential fuzz (50x, a new seed each round), dav1d's checkasm
(all 1039 at the default), the AV1 WebCodecs page in Chrome, and a functional app regression (QLab capture
byte-identical, Blender, the renderer plug-in, Chrome probes).

## Results (overnight 2026-10-26, quiet machine, interleaved A/B, bootstrap 95 % intervals)

Speedup above 1 means the second side is faster. "Same" means the interval includes 1. Windows opened while
builds were finishing, so some samples ran under load; spreads are in the scratch summaries.

### The key comparison: main dec12aa7 against wave 1's landing fc30d43b (5 rounds)

| Benchmark | wave 1 | main | speedup | 95 % interval | verdict |
| --- | ---: | ---: | ---: | --- | --- |
| Cycles render (640x360, 64 samples, 2 threads), s | 8.18 | 5.29 | **1.545** | 1.47-1.58 | faster |
| Cycles CPU, s | 17.61 | 12.20 | **1.443** | 1.41-1.50 | faster |
| Speedometer 3, V8 JIT | 2.85 | 2.58 | 0.905 | 0.85-1.04 | same |
| JetStream 2.2 (richards, delta-blue, navier-stokes), JIT | 201 | 194 | 0.963 | 0.90-1.03 | same |
| Speedometer 3, jitless | 5.26 | 5.29 | 1.006 | 0.94-1.07 | same |
| fxbench micro, geomean of 18 | | | 0.988 | | same (17 of 18) |
| fxbench call_ret, ns | 14,494 | 14,814 | 0.978 | 0.84-1.23 | same |

So exact flags cost nothing measurable against wave 1 now. When round 13 turned them on, the indicative cost was
call_ret 0.805 and micro geomean 0.977; both are back to wave 1. Cycles is 1.5x faster, all of it from the chkstk
leaf (below).

### Exact flags against the old AAPCS64 model, on main (5 rounds)

The same binary, `TRANSMUTE_FX_CORE=abiflags` against the default. Wider spreads than the window above (builds were
finishing).

| Benchmark | abi | exact | speedup | 95 % interval | verdict |
| --- | ---: | ---: | ---: | --- | --- |
| Speedometer 3, JIT | 2.63 | 2.61 | 0.992 | 0.70-1.23 | same |
| JetStream (3 tests), JIT | 187 | 168 | 0.902 | 0.68-1.08 | same |
| Speedometer 3, jitless | 5.32 | 5.09 | 0.956 | 0.90-1.11 | same |
| fxbench micro, geomean | | | 0.988 | | same (18 of 18) |
| fxbench call_ret | | | 0.909 | 0.81-1.25 | same |
| Cycles | | | 0.987 | | same |

No benchmark separates the exact model from the unsound one any more.

### Where fx stands against native (fxbench, same window)

Translated time over the native x64 build of the same kernels, so lower is better: geomean **1.11x** over the 16 kernels
with a native build (wave 1: 1.10x). The furthest from native: libc_calls 4.2x (host calls), call_ret 2.4x,
indirect_calls 2.0x, mem_own 1.7x, fp_float 1.4x. Some kernels beat their native build (branchy 0.36x, neon_intrin
0.30x, int_hash 0.57x), because the native build's code generation differs.

### One-binary knob A/Bs on main (3 rounds)

| Knob (the feature off) | Benchmark | speedup with the feature | verdict |
| --- | --- | ---: | --- |
| `ALCHEMY_FX_STACK_PROBE_LEAF=0` (chkstk leaf) | Cycles render / CPU | **1.53 / 1.47** | faster |
| `noshadows` (table shadows) | JetStream jitless geomean / Speedometer jitless | 1.006 / 0.965 | same |
| `norawflags` (raw flags at exits) | micro geomean / Speedometer JIT | 0.997 / 0.929 | same |
| `nodeadtargets` (dead-target scan) | micro | - | unusable: spreads 100-200 %, rerun |
| `noflagskip` (link-time skip) | micro | - | not run (window time), rerun |
| `t2noalign` (tier-2 loop alignment off) | Cycles / micro geomean | 0.988 / 1.002 | same |

fx-jit-cost's JIT code budget (their knob binary, sp-jit / js-jit): B1 (512 KiB, window 2048/512) 0.915 / 0.997 and B2
(256 KiB, 1024/256) 1.014 / 1.024, all "same". No default changes: no A/B justifies one.

### dav1d's NEON on fx (checkasm --bench, p3's ranges 5-6 against their knobs, 5 pairs)

| Functions | geomean of medians |
| --- | ---: |
| Wiener (2) | 1.31x |
| inverse DCT (`*dct_dct*`, 73) | 1.22x |
| 8-tap motion compensation (104) | 1.13x |
| self-guided restoration (3) | 1.21x |

### Correction to wave 1's table

Wave 1's jitless Speedometer row (4.32 -> 4.66, 1.08x, same) included the BR flag-dead default that round 11
reverted. Measured again without it (wave 1's base against the hotfix binary, 5 rounds): 3.72 -> 4.25, 1.14x,
interval 0.81-1.57, still "same". The base side was noisy (68 % spread), so the row stays "same".

## Open

* Rerun `nodeadtargets` and `noflagskip` in a quiet window. They are the two halves of round 16's flag work, and the
  overnight runs could not separate them. Found since: the dead-target scan never fires under alchemy-fx (its image
  code check excludes registered code memory, and the engine registers every executable mapping, image text
  included), so only the link-time skip was active. Fix first, then measure.
* RET still stores NZCV in raw form (p1's next phase), which is most of call_ret's remaining gap to native.
* Host calls (libc_calls 4.2x native) and indirect calls (2.0x) are the largest gaps.
* vm_remap's guest-space placeholders (fx-jit-cost) need their own A/B (`ALCHEMY_FX_GUEST_SPACE=0`).
* The Chrome gate's AV1 page and SwiftShader runs are slow to start on a loaded machine (not a deadlock):
  `ALCHEMY_HANG_DUMP` tells the two apart.
