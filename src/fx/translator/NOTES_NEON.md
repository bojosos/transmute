# NEON family notes (owner: neon)

Non-obvious decisions in `emit_neon*` / `emit_vector_memory*`.

## Structure

- `emit_neon_dispatch.cpp`: group decode (mask/value per A64 encoding group)
  and `describe_*`. `describe()` runs the emitter against a recording
  EmitContext, so register usage can never disagree with what `emit()` does.
- `emit_neon_common.cpp`: feature selection, the constant pool and `V`, the
  VEX/legacy helper. With AVX every op is emitted as 3-operand VEX.128 (no
  256-bit ops, so the upper YMM state stays clean and legacy-SSE code from
  other families never pays transition penalties). Without AVX, `V::op`
  resolves destination/source aliasing (d == y, non-commutative) through a
  private temp.
- Every group emitter validates the encoding before emitting anything: the
  core does not roll back bytes when a family declines.

## Constant pool

16-byte constants (pshufb controls, masks, LUTs) are rip-relative.
- Default (inline): constants of one instruction are emitted after its code
  behind one `jmp` (rel8 when short).
- Region pool: when the core brackets a translation attempt with
  `neon_begin_region(as)` ... `neon_end_region(as)` (after the last
  unconditional jump, before `finalize()`), constants are deduplicated and
  emitted once per region; no jumps on the hot path. Call `neon_begin_region`
  again on every retry (a RetryTranslation unwind leaves the old pool).
- Memory operands of legacy SSE ops need 16-byte alignment: the island and the
  pool are aligned relative to the buffer start, which is 16-byte aligned in
  code memory.

## Fault discipline

- `mark_may_fault()` immediately precedes every guest access (copies into
  the destination, e.g. legacy `movaps` before `pinsr`, are emitted before
  the mark).
- No guest register is written before the last access that can fault. For
  multi-register loads (LD1 x2-4, LDP, LDnR, LDn single lane) the last chunk
  is loaded first into a temp (or GPR) and the first chunk next; spans are at
  most 64 bytes, so once both succeeded every byte in between is on one of
  those two pages and the remaining loads go straight into their destinations
  (saves one move per register). Only concurrent unmapping between two
  instructions could break this, which is a guest race anyway.
- Structure loads (LD2-LD4) load all chunks into temps, then shuffle into the
  destinations. Stores may be partially performed (allowed).
- Writeback uses `lea` (no EFLAGS clobber); no NEON emitter touches EFLAGS.

## Upper-bit tracking and memory operands

- `emit_neon` notes the zero upper bits of each result (`result_width` in
  emit_neon_dispatch.cpp mirrors the group decode: Q=0 vectors 64, scalar
  groups 32/64, narrowing "2" forms, long/wide/INS 128, UMOV/SMOV none);
  vector-memory scalar loads note theirs. MOV (ORR v, v, v) propagates the
  source's knowledge. The test harness checks every note against the
  reference state.
- `Src`/`source()` wrap `EmitContext::vsrc`: simple three-same ops, BSL/BIT/
  BIF, ZIP/UZP/TRN (Q), EXT, INS element (S), scalar DUP, UMOV/SMOV (element
  loads), DUP element (broadcast from the slot, `dup_source()` first) and the
  by-element broadcast take in-memory sources directly; DUP/INS (general) read
  an in-memory GPR with `vpbroadcast*`/`vpinsr*`. VEX immediate shifts have
  no memory form (#UD; EVEX only). In-memory operands that must become
  registers are loaded (`in_reg`) before `write_vec`.
- Test modes: aliasing/pool with memory sources (vector and GPR); the
  aliasing context stores every register it handed out, read-only ones too.

## Semantics notes

- FPSR.QC (cumulative saturation) is not modelled; tests do not compare FPSR.
- Register shifts (SSHL/USHL/SRSHL/URSHL/SQSHL/UQSHL/SQRSHL/UQRSHL) use the
  signed low byte of each count lane. AVX2: vpsllv/vpsrlv/vpsrav (S/D; D
  arithmetic via xor-sign). Otherwise and for B/H: binary decomposition of the
  clamped count (1, 2, 4, ... esize) with blends. Rounding right shifts use
  t = x >> (k-1); r = t - (t >> 1), which never overflows.
- Saturating narrowing shifts round with (x >> n) + ((x >> (n-1)) & 1).
- TBL/TBX: idx +sat 0x70 sets bit 7 for idx >= 16 (pshufb then zeroes); table
  k uses (idx - 16k) +sat 0x70. TBX keeps Vd where idx >= 16 * tables.
- UZP/TRN/ZIP of 64-bit arrangements: shuffle then zero the upper half; all
  Q=0 results zero bits 127:64.

## Oracle gaps (C++ references in emit_neon_test_reference.cpp)

The reference interpreter lacks: SQDMULL/SQDMLAL/SQDMLSL (all forms),
scalar SSHL/USHL/SRSHL/URSHL, scalar SUB, SUQADD/USQADD, scalar SQNEG,
scalar compare-with-zero/ABS/NEG (D), scalar SQXTN/UQXTN/SQXTUN, vector RBIT,
PMUL, PMULL (8- and 64-bit), DUP (general) with don't-care imm5 bits.
It gets UQRSHRN wrong for a 64-bit all-ones lane shifted by 1 (int64
overflow: returns 0 instead of saturating); the shift tests use a C++
reference whenever the two disagree.
It also accepts several reserved encodings (scalar CMGT/CMHI/... with
size != D, DUP general .1D, store forms of LDnR); the family declines those.

## Test harness note

`testing::Snippet`'s trampoline does not preserve xmm6-xmm15 (Win64
callee-saved). Sequences using more than six XMM registers corrupt the C++
caller; the NEON tests use their own prologue/epilogue that saves them.

## Address folding (EmitContext::address)

- Accesses without base writeback build their operand with
  `EmitContext::address(GuestAddress)` (`base_offset`/`register_offset` in
  emit_vector_memory.cpp); the integer loads/stores use the same entry point
  in the region emitter. The default reads the registers as before.
- `FamilyEmitter::address_base` (`vector_memory_address_base`) tells the core
  which words read their base only through `address()`: LDR/STR/LDUR/STUR,
  register offset (index != base), LDP/STP/LDNP/STNP offset, LD1-LD4/ST1-ST4
  and single-structure forms without post-index. The core then folds an
  `ADD/SUB Xd, Xn|SP, #imm`, `ADD Xd, Xn, Xm, LSL #0..3` or `ADD Xd, Xn|SP,
  Wm/Xm, {U,S}XT{W,X} #0..3` right before the access into its x64 operand when
  Xd is dead after the access (`core::dead_after`: overwritten before any
  read, fault or exit; never when both the ADD and the access have an index).
  The folded access is emitted inside the ADD's instruction: its fault sites
  report the ADD's pc with Xd unchanged, as for the integer fold, so a fault
  rebuilds exact guest state through the usual pins + fault maps. A family
  that reads Xd any other way gets the value materialized (`lea`), so the
  fold can never be observed.
- Byte/halfword lane-0 stores with a disp32 operand go through a GPR (`movd`
  + `mov`): `pextrb/pextrw [disp32], x, 0` leaves a disp32 followed by a zero
  imm8, which the persistent cache's relocation audit reads as an image
  address (the region would be pinned to its base).
- Measured (Classroom, top-24 regions): of 58 ADD/SUB + SIMD&FP access pairs
  on the same base, 12 fold; the other 46 reuse Xd (several accesses off one
  base, or Xd overwritten by a load that may fault first). Static -0.06%.

## Broadcasts from current context slots (EmitContext::vec_slot)

- DUP (element) and integer by-element (S/D elements, AVX) use
  `element_source()`: the slot of a register that is unpinned and clean
  (also when a clean copy is cached) is broadcast straight from memory
  (vbroadcastss / vmovddup: one load uop, no shuffle on p5) instead of
  load + vpermilps/vpshufd. The fp family does the same for its by-element
  operands (NOTES_FP.md).
- Single-table TBL (AVX) computes `idx +sat 0x70` into the destination
  unless the destination is the table: no scratch xmm.

## Knobs

`TRANSMUTE_FX_NEON=<comma-separated>` (folded into the family names "neon" /
"vector_memory" and so into `translator_family_fingerprint()`): `nofold`
(no ADD/SUB folding into SIMD&FP accesses, old byte-store encoding),
`noslot` (no slot broadcasts), `notbltemp` (TBL index temp as before),
`oldshl` (USHL/SSHL as before the cost audit), `nozero64` (always zero
64-bit vector results), `old` (all). `TRANSMUTE_FX_FP=noslot|oldimm|oldmask|
oldcvt|old` likewise for the fp family. Tests flip them in-process with
`neon_set_knobs_for_testing` / `fp_set_knobs_for_testing`.

## Cost audit (tools/fx_neon_cost.cpp + .py, artifacts/fx-neon-cost)

- `fx_neon_cost enumerate` translates the SIMD&FP encoding space (fixed
  registers) as one-instruction regions: ~497k supported encodings, ~2.7k
  forms after llvm-objdump names them. `cost` measures each representative
  unpinned (one-instruction region minus the NOP region's exit), pinned
  (`L: insn; B L`, loop body) and pinned with sources known zero above
  (LDR S of every register field first, subtracted). The script ranks forms
  by their excess over a hand-written ARM-exact AVX2 estimate (`ideal()`)
  and runs llvm-mca (alderlake).
- The remaining gap to the estimate: scalar FP upper-lane zeroing at block starts
  (vinsertps; tracked otherwise), FMADD's accumulator copy (x64 FMA is
  destructive), the LDP temp + move (fault discipline), FCSEL/FCCMP flag
  glue, the A64 FMAX (5) / FMINNM (7) NaN and +-0 rules.
- Changes: USHL/SSHL S lanes negate the count with `vpsignd` (7 -> 6);
  D lanes (AVX2) gather the count bytes with pshufb + pmovsxbq and use
  vpsllvq/vpsrlvq directly (USHL 15 -> 7, SSHL 10); three-same simple ops,
  BSL/BIT/BIF and shift-by-immediate skip the `vmovq` of a 64-bit result
  when every source is known zero above bit 64 (all map zero lanes to zero;
  CMEQ excluded).
- Micro-benchmarks: tests/fx/bench/bench_simd.c (NEON vs SSE/AVX2
  intrinsics; build.ps1 variant `simd`). The ray/box slab kernel stays at
  ~2.6x native: native x64 MINPS/MAXPS are one instruction with x64 NaN/
  +-0 rules, ARM-exact FMIN/FMAX need 3/5.

## Spill-cost experiments (vector-register pressure; Raptor Lake i7-1355U)

- Upper YMM halves as spill space: a microbenchmark of 8 reloads + 2 spills
  per iteration measured memory operands 1.8 ns/iter, vextracti128 /
  vinserti128 into the upper halves 2.5 ns/iter (+40%, p5 contention), and
  423-446 ns/iter as soon as any legacy-SSE instruction runs with the upper
  state dirty (the core and several families emit legacy movdqu/movd/pinsr;
  host calls clobber the upper halves of xmm6-15 too). Not adopted.
- Pairing two 128-bit ops into one 256-bit op (Embree node test): only the
  six FSUB and six FMUL pair (the FMAX/FMIN trees pair different ops); the
  pairs need vinsertf128 packing of both inputs, packed per-lane broadcast
  factors and vextractf128 for the FMAX/FMIN halves, about 27 uops for the
  24 they replace, plus the dirty-upper penalty above. Not implemented.
- Within the top-24 regions vector context traffic is ~8% of samples (4.4%
  loads, 3.8% stores), spill/reload pairs inside a block 0.6%; zeroing
  vinsertps 1.5% (mostly scalar FP results and the Cycles dot-product
  idiom FMUL + INS wzr + FADDP + FADDP, already minimal per instruction).
  The same-exe A/B of all of the above (TRANSMUTE_FX_NEON=old,
  TRANSMUTE_FX_FP=old) is within noise (B/A geomean 0.999 over 10 pairs).
