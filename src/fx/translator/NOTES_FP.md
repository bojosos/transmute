# fp family notes

Files: `emit_fp.hpp` (public API), `emit_fp_internal.hpp` (helpers, decoded
`Inst`), `emit_fp.cpp` (MXCSR/FPSR helpers, `FpEmit`, decoder, `describe`,
dispatch), `emit_fp_arith.cpp`, `emit_fp_convert.cpp`, `emit_fp_estimate.cpp`,
`emit_fp_half.cpp`, `emit_fp_register.cpp` (`register_fp_emitters`, separate
object so tests link without the core). Tests: `emit_fp_tests*.cpp`.

## Runtime contract

* Thread entry: `_mm_setcsr(fp_mxcsr_for_state(ctx.fpcr, ctx.fpsr))`
  (or `fp_load_mxcsr(ctx)`).
* Architectural FPSR = `ctx.fpsr | fp_fpsr_flags_from_mxcsr(MXCSR)`. Before C++
  inspects or changes FP state (interpreter fallback, exception dispatch,
  thread context APIs) call `fp_sync_fpsr(ctx)`; after changing `ctx.fpcr` /
  `ctx.fpsr`, call `fp_load_mxcsr(ctx)` before resuming translated code.
* Translated MRS/MSR FPCR/FPSR use `ctx.fpcr` as a transient 32-bit slot for
  `stmxcsr`/`ldmxcsr` (restored before the instruction ends).
* No helpers. 16-byte constants come from the neon family's rip-relative
  pool (`FpEmit::kconst/ksplat/kscalar`; fx_emit_fp links fx_emit_neon): one
  deduplicated pool per region when the core brackets the region, else after
  the instruction behind a jmp. Relocatable; SSE fallbacks still build
  constants inline (all-ones + shifts, `mov r, imm` + `movd`/`pshufd`).

## Code shape

* AVX (VEX 3-operand) when available, SSE4.1 otherwise; both paths are tested
  (`fp_set_features_for_testing`). Fused operations require FMA3; without it
  they are declined (interpreter). F16C is required for half arithmetic.
* Scalar results need A64's zero upper bits; a VEX scalar op copies them from
  its first source. `fp_emit` reports every result's zero bits
  (`result_width` -> `EmitContext::note_vec_zero_above`) and the region
  emitter remembers them per V register until a label, helper call or exit
  (`vec_zero_above`, snapshot in `FpEmit`). The zeroing (`vinsertps 0x0e` /
  `vmovq`) is emitted only when the first source is not known zero above the
  scalar. To get there: commutative ops put a known-zero (or in-memory, loaded
  with `movss`/`movsd`, which zeroes) operand first; unary scalar ops (sqrt,
  round, cvt) take their upper lanes from a zeroed temp (`sc_u`); scalar
  by-element temps are `insertps`/`psrldq` extractions with zero upper lanes;
  FMLA/FMADD load an in-memory accumulator with `movss`/`movsd`. 64-bit
  vector FADD/FMUL/FMLA skip the `vmovq` when every source is zero above bit
  64 (0 OP 0 = +0 exactly, no flags; not FSUB/FMLS/FDIV/compares).
  64-bit vector forms otherwise compute on 128 bits and zero the upper half;
  garbage upper lanes can set spurious cumulative flags (and, with FZ off,
  could hit denormal assists). Conversions clean their upper lanes first.
* Sources come from `FpEmit::src` (`EmitContext::vsrc`): an unpinned register
  not in the core's cache is a memory operand (VEX second source, `ss`/`sd`
  forms read 4/8 bytes, elements of by-element forms at `slot + es*index`,
  vector by-element broadcasts with `vbroadcastss`/`movddup` from memory,
  FCSEL/FMOV-to-GPR/FMOV-from-GPR/SCVTF read GPR-side straight from the slot
  (`gsrc`)). Min/max need both operands in registers (both orders).
* By-element operands other than a scalar's lane 0 (`element_op`) come
  straight from the register's context slot when `EmitContext::vec_slot`
  says it is current (unpinned and clean, also when a clean copy is cached):
  `vbroadcastss`/`movddup` from memory for vectors, an `ss`/`sd` memory
  operand for scalars, instead of a register load plus `vpermilps`/
  `insertps`. `TRANSMUTE_FX_FP=noslot` (or `old`; folded into the family name
  "fp+noslot" and so the translator fingerprint) restores the register path.
* FCMP/FCMPE use the core's fusion protocol (`fcmp_swap`/`set_fcmp_flags`);
  the fallback builds NZCV from the same EFLAGS with flag-preserving code.
* Cost audit (NOTES_NEON.md; `TRANSMUTE_FX_FP=oldimm|oldmask|oldcvt` restore
  the earlier lowerings): FMOV S/D/H #imm and FMOV .2S/.4H #imm are one
  pooled `movups` of {imm, 0} (was mov r, imm + movd: 2 uops, p5);
  FNMUL is mul + XOR with a pooled scalar sign mask (zeroing only when no
  source is known zero above the scalar), FABD/FACGE/FACGT AND with pooled
  magnitude masks as memory operands (scalar masks have zero upper lanes,
  so no zeroing); UCVTF .4S/.2S = cvt(x >> 16) * 2^16 + cvt(x & 0xffff) in
  one FMA (exact pieces, one rounding, IXC only when inexact; 8 -> 5);
  SCVTF Sd, Sn is cvtdq2ps of {n[0], 0, 0, 0} (insertps or a movss load
  unless Vn is known zero above 32), UCVTF Sd, Sn drops a redundant
  zero-extension; SCVTF/UCVTF/FCVT*.2S skip the source and result zeroing
  when the source is known zero above bit 64 (zero lanes convert to 0
  without flags).
* Rarely executed fix-ups (conversion saturation, unsigned 64-bit conversions,
  estimate special lanes) sit behind a forward branch the common path takes.
* Per instruction: at most 4 vector temps and 5 GPR temps (checked by
  `FpEmit`); constants share one GPR.
* FP16 vector code writes Vd one 64-bit (or 32-bit) group at a time. A group's
  lanes are read before they are written; pairwise and by-element forms order
  the groups so a source aliasing Vd is consumed first.

## Semantics decisions

* NaN payloads/signs: host behaviour (policy). The test comparator treats any
  NaN as equal to any NaN at the result element size.
* FMIN/FMAX/FMINNM/FMAXNM: x64 min/max return the (DAZ-flushed) second operand
  on NaN or equality; computing both operand orders t1 = op(a, b), t2 = op(b, a)
  and combining gives the A64 +-0 and NaN-versus-number rules, including FZ
  flushing. FMAX = (t1 & t2) | unord(a, b) (5), FMIN = t1 | t2 (3), FMAXNM =
  (t1 | nan(b)) & (t2 | nan(a)) (7), FMINNM = (t1 & ~(nan(b) & ~nan(a))) |
  (t2 & ~nan(a)) (8; in NaN lanes t1 = b and t2 = a). t1 is computed last, so
  the destination may alias a source without a copy (AVX; not FMINNM).
  **Gap:** FMAXNM/FMINNM treat a signalling NaN like a quiet NaN (A64 returns
  NaN for sNaN versus a number). Exactness would cost ~9 more instructions on
  the fmaxf/fminf path.
* Against a constant (clamps), bit-exact: when one operand holds a value known
  at translation time on every path (EmitContext::vec_fp_const: FMOV
  immediates, MOVI/MVNI, FMOV S/D from a MOVZ/MOVK register) that is finite
  and normal in every element, and the other operand x can hold no signalling
  NaN (EmitContext::vec_fp_quiet: an FP arithmetic result, which A64 never
  makes signalling, or a constant without one), FMAXNM/FMINNM are one max/min
  with the constant second (a quiet NaN x yields it, as A64 does) and FMAX/FMIN
  one max/min with x second (a quiet NaN x is returned unchanged, which equals
  A64's quieted x with FPCR.DN = 0). RegionEmitter's forward dataflow tracks
  both, joined across loop headers, with quiet bits per element size (32-bit
  quiet lanes can read as a signalling double). A normal constant is never
  zero, so there is no +-0 tie, and never denormal, so FZ changes nothing.
  fxbench fp_float's clamps (8 + 7 instructions) become 1 + 1.
  `TRANSMUTE_FX_FP=noconstminmax`. Tests: `fp_region_minmax_const_bit_exact`
  (x = FADD(raw, -0.0) over quiet and signalling NaN payloads, +-0, denormals,
  infinities and ties; every op, shape and constant side; the result compared
  bit for bit with the oracle) and `fp_region_minmax_const_random_differential`
  (random programs: the constant forms against the general lowering and, without
  signalling NaNs, against the oracle).
* FZ: x64 detects tininess after rounding, A64 before. With FZ = 1 a result in
  [min_normal * (1 - 2^-p), min_normal) is flushed by A64 but becomes
  min_normal on x64 (comparator tolerance `fz_tininess`). x64 sets UE|PE on an
  FTZ flush where A64 sets only UFC.
* FCVTZS/FCVTZU/FCVT[NPMA]*: `roundss/sd` with an explicit mode, then `cvtt*`
  with saturation and NaN -> 0 fixed up behind a branch. FCVTA*/FRINTA use an
  exact trunc / doubled-remainder sequence. FRINTA(+-inf) sets a spurious IOC.
* UCVTF from 64 bits: halve with a sticky bit (round to odd), convert, double.
* FRECPE/FRSQRTE are bit exact (no tables): the 8-bit estimate is
  `round(512 / m)` / `round(512 / sqrt(m))` in single precision, which matches
  A64 RecipEstimate/RecipSqrtEstimate for every input in every rounding mode
  (`fp_estimate_formulas_match_arm_tables`); exponents come from integer lane
  arithmetic; zero/denormal/inf/NaN/negative lanes take an inline slow path
  (denormals pre-scaled, specials via x64 1/x and 1/sqrt x, which produce the
  A64 special results). Spurious IXC (the division is inexact).
* FRECPS is one FMA. **Gap:** FRSQRTS is `fma(-a, b, 3) * 0.5`; when
  |a*b| >= 2^127 the fused intermediate overflows before the halving (A64
  halves before rounding).
* FCVTXN: round-to-odd narrowing (`cvtpd2ps`, back-convert, step toward zero
  if rounded away, set the low bit if inexact), then `* 1.0` so FZ flushes a
  tiny result exactly like A64. FCVT H<-D uses the same narrowing then
  `vcvtps2ph`; normal doubles below 2^-126 are replaced by +-2^-100 first (FTZ
  would flush the single intermediate, but FZ does not apply to a half result).
* FJCVTZS: 64-bit truncation fast path; |x| >= 2^63, inf, NaN via an exact
  `x - 2^32 * trunc(x / 2^32)`; Z = in range and `(double)result` has the bits
  of x (excludes -0, inexact and FZ-flushed denormals).
* FP16 (F16C): widen to single exactly, compute, narrow with MXCSR rounding
  (double rounding innocuous for + - * / sqrt; min/max/compare/round exact).
  Fused half operations go through double (the product is exact; the sum is
  exact unless the addend dominates, where rounding to 53 bits cannot create a
  false half midpoint) plus round-to-odd to single, i.e. one correct rounding.
  FNMUL/FABD negate/abs after narrowing (directed modes). Not modelled:
  FPCR.FZ16, FPCR.AHP. Declined (interpreter): FMULX, FRECPE, FRSQRTE, FRECPS,
  FRSQRTS for H; SCVTF/UCVTF H from X registers with fbits != 0.
* FRINT32Z/X, FRINT64Z/X (FEAT_FRINTTS; FPRoundIntN): roundps/pd (X: MXCSR
  mode, Z: truncate; IXC for an inexact in-range result in both forms), then
  32-bit range: cvtt + cvt back (INT_MIN with IE for NaN, infinity and out of
  range -> -2^31; exact otherwise) OR the rounded sign (keeps -0); 64-bit
  range: compares against +-2^63 select -2^63 and cvtt of the all-ones mask
  raises IE. **Gap:** a double that rounds out of int32 range (|x| just below
  2^31) also sets IXC (A64: IOC only).
* FMLAL/FMLSL/FMLAL2/FMLSL2 (FEAT_FHM; FPMulAddH): vcvtph2ps of both halves
  (exact; ignores DAZ, so halves are never flushed: FZ16 is not modelled) and
  one fp32 FMA (FZ via DAZ/FTZ on the addend and result). 2S forms compute
  with zeroed upper lanes (no spurious flags); FMLSL re-zeroes them (+0 - 0*0
  is -0 when rounding toward -inf).
* The reference interpreter implements FRINT32/64 and FMLAL/FMLSL(2) for the
  oracle and the interpreter fallback (a64.cpp: floating_round_int_n_bits,
  floating_fhm_widen_bits).
* Not implemented (declined): FCMLA/FCADD (FEAT_FCMA), BFloat16, FEAT_AFP FPCR
  bits, FEAT_RPRES.

## FPSR

* MXCSR IE->IOC, ZE->DZC, OE->OFC, UE->UFC, PE->IXC. DE is not mapped to IDC:
  x64 sets DE only when DAZ is clear (FZ = 0), exactly when A64 never sets IDC,
  and never sets it under DAZ. IDC therefore only appears via MSR FPSR.
* Known flag divergences (`fp_zz_fpsr_divergence_report`): x64 max/min raise IE
  on quiet NaNs; unsigned/out-of-range conversions may add PE (A64 reports IOC
  only); vector FCVTZU does not raise IOC for negative lanes; 2S/4H garbage
  upper lanes; FTZ flushing; FRINTA(inf); FRECPE/FRSQRTE IXC.

## Tests

* `check_word`/`check_batch` also run the aliasing context with memory sources
  (`vsrc`/`gsrc` hand out slots), with known-zero claims on contexts sanitized
  to match (`alias_variants`), and check every `note_vec_zero_above` against
  the reference state. The aliasing contexts store every register they handed
  out, read-only ones included, so an emitter writing into a source register
  (a pinned or cached guest register in regions) fails.
* `fp_hot_form_instruction_counts` also counts with zero-upper sources.
* Region-level checks (translator core, caching, tracking across a loop
  header): `fp_region_*` in emit_neon_region_tests.cpp.

## Reference interpreter issues found (for integration)

* Scalar single-precision FMAX/FMIN/FMAXNM/FMINNM do not mask the 64-bit
  source to 32 bits: bits 63:32 leak into the result and the +-0 rule fails
  when they are non-zero. Tests use sanitized inputs (`ref_scalar_minmax`).
* Missing: FMULX (all forms), vector FMAXP/FMINP/FMAXNMP/FMINNMP, scalar
  register FCMEQ/FCMGE/FCMGT, FRECPX, FCVTXN, FJCVTZS, FMOV Xd,Vn.D[1] /
  Vd.D[1],Xn, FMOV H<->W/X, FP16 arithmetic (scalar FP16 is decoded as double).
  Tests use references built from oracle primitives or written by hand
  (`emit_fp_tests_ref.cpp`, `emit_fp_tests_half.cpp`).
