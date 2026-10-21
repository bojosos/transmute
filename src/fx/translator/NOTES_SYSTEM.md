# atomics/system family: design notes

Non-obvious decisions behind `emit_atomic.cpp`, `emit_system*.cpp`.

## Ordering

Mapping (ABI 11; `TRANSMUTE_FX_SYSTEM=oldorder` restores the previous one,
shown in brackets):

| ARM64 | x64 |
|---|---|
| LDAPR, LDAPUR(S) (RCpc) | `mov` |
| LDAR, LDLAR, unfused LDAXR/LDAXP (RCsc) | `cmp byte [pending],0; je 1f; lock and byte [pending],0; 1:` + `mov` [`mov`] |
| STLR, STLLR, STLUR | `mov` + `mov byte [pending],1`; `xchg` if an RCsc load follows in the same span [`xchg`] |
| DMB SY/ISH/OSH/NSH (+ reserved CRm<1:0>=00) | `lock and byte [pending],0`, or nothing when subsumed (below) [`lock or [rsp],0`] |
| DMB *LD, DMB *ST, ISB, SB, SSBB, PSSBB | nothing |
| DSB (every type but SSBB/PSSBB), DSB nXS | `lock and byte [pending],0` [`lock or [rsp],0`; DSB LD/ST: nothing] |
| LDXR/STXR loops, LSE, CAS | locked instructions (full fences) |

`[pending]` is `GuestContext::release_pending` (a byte aliasing the old
`excl_pad`, layout unchanged).

Why it is correct. x64 TSO keeps every program-order pair except an earlier
store followed by a later load (which may be satisfied first, including from
the thread's own store buffer for the same address). The ARM orderings a
thread's program order must provide (aarch64.cat `bob`/`ob`):

1. Acquire (LDAR, LDAPR, LDAXR) before every later access: R->R, R->W. TSO.
2. Every earlier access before a release (STLR): R->W, W->W. TSO, so STLR is
   a plain store.
3. RCsc: a release before a later LDAR/LDAXR/LDLAR (`[L]; po; [A]`, any
   address). This is W->R: the store must be globally visible before the
   load is satisfied. LDAPR (RCpc, `[Q]`) is exempt, so it stays a plain load.
4. DMB full: everything before everything; only W->R needs code. DMB LD
   (`[R]; po; [R|W]`) and DMB ST (`[W]; po; [W]`) are TSO already, in every
   domain (OSH/NSH/ISH/SY), so no code.
5. DSB: at least a DMB of its type, and later instructions do not execute
   until the earlier accesses complete, so even DSB LD/ST order earlier
   stores before later loads. Fenced.

Rule 3 is placed on the LDAR side because a region cannot see the LDAR that
follows an STLR in another region. Where acquire loads outnumber releases, a
fence at every LDAR would cost more than `xchg` at every STLR, so the release
is recorded instead:

- Invariant: when `release_pending == 0` at a point of thread T, no
  store-release of T before that point is still in T's store buffer.
- STLR sets it after its store. It is cleared only by a locked
  instruction (`lock and byte [pending], 0`: the fence of a DMB/DSB, or the
  one an LDAR takes), which drains the store buffer in the same instruction,
  so the invariant holds. (A plain clear after `lock or [rsp]` leaves a
  store the next fence must drain: +2 ns per DMB in a loop.)
- LDAR reads it: x64 loads always see the thread's own earlier stores, so a
  set flag is seen; set -> fence (drains the STLR) then load; clear -> the
  invariant says no release is pending. Either way rule 3 holds, also across
  regions, calls, host code, exceptions and fibers (the flag describes the
  host thread's store buffer; there is one GuestContext per host thread and
  fiber switches do not copy it).
- Locked instructions (fused loops, LSE) do not clear it (it would add a
  store to every atomic); a later LDAR then takes one redundant fence.
- STLR skips the flag store when the following words reach a full barrier
  through stores/register-only code (`stlr; dmb ish`): the barrier drains
  and clears.
- STLR stays `xchg` when an RCsc load follows in the same span before any
  fence or branch: draining two stores (value + flag) at the LDAR's fence
  costs more than `xchg` (8.7 vs 6.8 ns); the LDAR then finds the flag clear.
- The one-instruction interpreter knows nothing of the flag; the family
  never declines an allocated STLR/LDAR/LDAXR form (should-be-one fields
  included), so none reach it.

Cost (i7-1355U, bench_barrier_and_release_store_sequences, one CPU): STLR
6.8 ns (`xchg`) -> 1.0 ns; LDAR with the flag clear 0.33 -> 0.65 ns; STLR
then LDAR in another span 8.7 ns (one fence after two stores); full DMB
8.3 (`lock or [rsp]`) -> 7.7 ns (`lock and [pending]`).

DMB elision: a full DMB followed, in the same span (straight-line code of the
same region, no branch target), by only plain stores (STR/STP/STUR/ST1-4/
STLR/STLUR, any width) and register-only instructions (data processing,
SIMD/FP, hints) up to another full barrier emits nothing. For X before the
first DMB and Y after it: Y is one of the stores (X->Y is R->W or W->W, TSO)
or comes after the second barrier, which orders X before Y. There are no
loads in between. If one of the stores faults, the exception path goes
through the kernel (IRET is serializing) and the runtime's locked
instructions before any guest handler runs. Locked LSE instructions are not
used as the second barrier (CASP may become an interpret exit). Covers
`dmb ish; dmb ish` and MSVC's seq_cst store `dmb ish; str; dmb ish` (embree4:
~1200 such sites). DSB is never elided.

- Locked RMW vs `mfence`: `lock or dword [rsp], 0` (legacy fence) 9.5 ns vs
  14.5 ns. [rsp] is the callee-owned shadow area and [pending] a
  GuestContext byte, both thread private. Neither orders non-temporal
  stores; no emitter in fx produces them.
- ISB emits nothing. The family cannot end a region; the core should end
  regions at ISB if it wants code changed by the same thread (IC IVAU + ISB)
  to take effect before the region's own stale translation of later code.
  IC IVAU calls the invalidation helper; DC CVAU and the DSBs around it are
  fences or no-ops and never elided before a system instruction.
- Litmus tests (`litmus_store_buffering_and_message_passing`, real core,
  2 threads in lockstep, 10^6 iterations each, `FX_LITMUS_ITERATIONS`):
  SB with STLR;LDAR (same region, across a BR, legacy), STLR;LDAXR,
  STLR/STR;DMB;LDR, the elided `dmb; str; dmb`, DSB ISHST, and MP with
  DMB ISHST/ISHLD, STLR/LDAR, STLR/LDAPR, DMB ISH: 0 forbidden outcomes. The
  allowed shapes show the harness sees reordering: STR;LDR ~40%,
  STLR;LDAPR ~17%, STLR;DMB ISHLD;LDR ~6%.

## Exclusive monitor

- LDXR records (address, value, size) in `GuestContext::excl_*`. STXR succeeds
  iff address and size match and `lock cmpxchg` against the recorded value
  succeeds; the reservation is cleared either way. ABA is accepted.
- LDXP X reads two 64-bit halves (validated by STXP's `cmpxchg16b`); LDXP W is
  one 64-bit load.
- Should-be-one fields: Rs of LDXR/LDAXR/LDAR/LDLAR/LDXP/LDAXP/LDAPR and Rt2
  of single-register forms (LDXR, STXR, STLR, ...) with other values are
  CONSTRAINED UNPREDICTABLE; they execute as the canonical encoding.
  Unallocated encodings (odd CASP registers, LSE o3=1 opcodes other than
  SWP/LDAPR, invalid LDAPUR size/opc) stay undefined.
- The reference interpreter keeps its own monitor and performs atomics
  byte by byte. An exclusive or LSE instruction that falls back to it is
  wrong (no shared reservation, not atomic). The family never declines them;
  see the register budget below for the one case that can still overflow.

## LL/SC fusion

Shapes (any size, any acquire/release mix, cbnz W or X on the status):

| shape | x64 |
|---|---|
| `ldxr; add/sub(imm, reg, shifted); stxr; cbnz` | `lock xadd` |
| `ldxr; and/orr/eor/bic/orn/eon(reg, imm, shifted); stxr; cbnz` | `cmpxchg` loop |
| `ldxr; stxr Rv; cbnz` | `xchg` (Rv == Rt: `lock xadd 0`) |
| `ldxr; cmp; b.ne <after cbnz>; stxr; cbnz` (MSVC) | `lock cmpxchg` |
| `ldxr; cbnz Rt, <after>; stxr; cbnz` | `lock cmpxchg` with 0 |
| `ldxp X; stxp same regs; cbnz` (128-bit atomic load) | `lock cmpxchg16b` with 0 |
| `ldxp W; stxp; cbnz` | `lock xadd 0` / `xchg` 64-bit |
| `ldxr; adds/subs(imm, reg, shifted); stxr; cbnz` (ABI 11) | `lock xadd` + x64 `add`/`sub` producing NZCV |

Up to two `mov` (ORR from ZR) and one DMB/DSB after the loop are consumed: a
locked instruction is a full barrier, the movs are register-only. Since ABI
11 up to three register-only ops are consumed: MOV, CMP/CMN #imm, CMP Rn, Rm,
AND #imm (bitmask fitting imm32) and ADD/SUB #imm. The same applies after
an LSE instruction (LDADD/LDCLR/.../SWP/CAS): clang-cl emits `ldaddal;
[add;] dmb ish` for `_InterlockedIncrement`, which cost two locked
instructions (bench threads_atomic_private [lse]: 13.6 -> 7.5 ns, 2.1x ->
1.2x native). They read the span's final values (or
registers the span does not write, read before any guest write); the last
flag setter is emitted as the span's final x64 instruction, after the
EFLAGS-clobbering AND and the reservation check. An AND after a flag-setting
body is consumed only if a later CMP sets the flags again; a CMP of a value
the span sets to zero (the status register) is not consumed.

blender.exe (static): 14200 `subs` loops (MSVC `_InterlockedDecrement(...) ==
0`, refcount release), previously unfused (LDAXR + `lock cmpxchg` + the DMB's
`lock or`: three locked or reservation operations), and ~6000 loops followed
by `cmp`/`and`/`cmn` before their `dmb ish`, whose DMB was a second locked
instruction. `TRANSMUTE_FX_SYSTEM=oldfusion` restores the previous shapes.

ADDS/SUBS equivalence: the stored value is old +- operand as before; NZCV is
the x64 `add`/`sub` of the same width on (old, operand): N=SF, Z=ZF, V=OF,
C=CF (add) / !CF (sub), i.e. FlagsProducer::add/sub. ADD flags are symmetric,
so ADDS with Rt as either operand fuses; SUBS only with Rt first.

Equivalence: the fused op produces the state of an execution where the first
STXR succeeds, which is always architecturally possible because nothing
between LDXR and STXR can clear the monitor. Every register the loop writes
(old value, result, status 0) and the flags of the CMP are produced. On the
compare-exchange failure path the status register keeps its value and the
reservation stays live (a later STXR to that address succeeds, as on
hardware). Compare values with bits above the access size can never match
(the loop compares 32/64 bits); that case only loads.

Not fused: clang's compare-exchange (b.ne goes to a CLREX), flag-setting ALU
ops other than ADDS/SUBS (and SUBS Rd, Rm, Rt), operands that use the loaded
value twice, 32-bit ops on 64-bit exclusives,
mismatched base/size, 128-bit exchange (register budget). These run through
the unfused LDXR/STXR path, which is correct but about 2-4x slower under
contention.

The DMB that MSVC places at the `b.ne` target of `_InterlockedCompareExchange`
is a branch target, so it is not in the fused span and costs a second locked
op. Dropping it is safe only if no other branch in the region reaches it; the
core knows that, the family does not. Proposed core-side follow-up: let a
family report "emission ends with a full barrier" and let the core skip a DMB
reached only by fallthrough from it.

## Register budget and the EmitContext hooks

The core's RegionEmitter has 7 scratch GPRs (r8 r9 r10 r11 rcx rdx rax, rax
last) and unpinned operands consume them. emit_context.hpp promises 5 temps
in addition to operands, so the family is written to stay within 7 in the
worst case: nothing pinned, hooks not overridden. Where an optional part
(trailing movs) would not fit, it is left to the core.

Implicit registers (rax for cmpxchg, rax/rbx/rcx/rdx for cmpxchg16b, rax/rdx
for rdtsc+mul, xmm0 for sha256rnds2) are claimed through
`EmitContext::temp_gpr_prefer` (added, additive). Without the hook a context
gets one probing `temp_gpr()`. An implicit register that cannot be claimed
may hold a pinned guest value: it is saved to an xmm temp and restored, and a
`lock or [addr], 0` write probe runs first so a fault still sees every pinned
register intact (rule 2). The probe is a second locked op. Implementing the
hooks in RegionEmitter removes it:

```cpp
Gp RegionEmitter::temp_gpr_prefer(Gp r) {
    if (!((kScratchMask >> x64::id(r)) & 1u) || !is_free(r)) return Gp::none;
    take(r);
    return r;
}
void RegionEmitter::load_gpr(Gp dst, unsigned index, bool sp) { load(64, dst, val(index, sp)); }
void RegionEmitter::store_gpr(unsigned index, Gp src, bool sp) {
    if (index == 31 && !sp) return;
    note_write(index);
    if (pinned(index)) { if (pin(index) != src) a_.mov(64, pin(index), src); return; }
    a_.mov(64, gmem(index), src);   // callers store only after their last faulting access
}
```

CASP needs 5 operands plus rax and two results; without the hooks and with
nothing pinned it needs 9-10 scratch GPRs and the core retries it as an
interpret exit, which is not atomic. With the hooks it needs 4-5.

`CoreLikeContext` (tests) reproduces these constraints; the differential
suites run under it with three pin layouts, with and without hooks.

## Counter

CNTFRQ_EL0 = 19.2 MHz (a common Windows-on-ARM value). With an invariant TSC,
CNTVCT/CNTPCT(SS) = (rdtsc * mult) >> 32 inline. `mult` is read through
`helpers[helper_system_timer_data]`, a data pointer, so translated bytes stay
process-independent (the cache component must still key on host features).
The TSC rate comes from CPUID 0x15, or the best of three 2 ms QPC windows.
Without an invariant TSC, a helper scales QueryPerformanceCounter.

## CRC and crypto

- CRC32C*: SSE4.2 `crc32`, which uses the same reflected Castagnoli CRC without
  inversion. CRC32* (ISO): PCLMUL Barrett reduction in the reflected domain
  (P' = 0x1DB710641, mu' = 0x1F7011641; X form = two 32-bit steps); table
  helper without PCLMUL. Validated against the literal BitReverse/Poly32Mod2
  pseudocode and the oracle (incl. zlib's `crc32x w8, w8, x13`).
- AESE = aesenclast(d^n, 0), AESD = aesdeclast(d^n, 0), AESMC =
  aesenc(aesdeclast(x, 0), 0), AESIMC = aesimc; AESE+AESMC and AESD+AESIMC on
  the same register fuse into aesenc/aesdec. FIPS-197 known answer.
- SHA1C/P/M: sha1rnds4 on lane-reversed state with W-K (+E in the top lane),
  because x64 adds K itself. SHA256H/H2: two sha256rnds2 on shufps-built
  ABEF/CDGH; SHA256SU0 = sha256msg1; SHA256SU1 = sha256msg2(d + (m:n >> 32),
  m). SHA1H/SU0/SU1 are plain SSE. SHA-1/SHA-256 of "abc" through the
  instruction sequences.
- SHA512*: portable helper (no x64 equivalent below SHA512 AVX); validated
  with SHA-512("abc") using the Linux sha512-ce round structure.
- EOR3/BCAX/RAX1/XAR: SSE. SM3/SM4: not implemented (declined).

## Helpers

`install_system_helpers(ctx)` fills `helpers[48..63]` (the runtime must call
it for every context): 48 IC IVAU (invalidate_guest_code of the page), 49 IC
IALLU, 50 QPC counter, 51 CRC32 fallback, 52 portable vector crypto (reads and
writes ctx->v), 63 the timer data pointer. `emit_system_helpers.cpp` is the
only object that references `fx::invalidate_guest_code`, so linking the
family without the runtime works until `system_helper()` is used.
