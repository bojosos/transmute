# fx: the fast execution engine

fx replaces the checked-memory reference engine as Transmute's production
path. The reference interpreter (`src/a64.cpp`) stays as the differential
oracle and as a one-instruction fallback. fx code lives in `src/fx/`,
`include/transmute/fx/` and `tests/fx/`; it never includes or links
`win32_bridge.cpp`.

Goal: ARM64 Windows applications on x64 Windows at native-like speed, with
native threading and direct Win32 calls. Rosetta 2 translates whole binaries
ahead of time and caches them; Prism JITs with a per-module persistent cache
and runs system DLLs natively. fx does all three with one translator:
JIT on miss, persistent cache for warm runs, optional AOT from `.pdata`.

## Non-negotiable invariants

1. **Guest VA == host VA.** Images, stacks, heaps and mappings are host
   allocations at the address the guest sees. Loads/stores are plain x64
   `mov`s. No region maps, span tables, journals, revisions or leases.
2. **Guest threads are host threads.** No global execution lock. Locks exist
   only on cold paths (translation miss, module load, invalidation).
3. **Atomics and ordering are native.** x64 TSO keeps every program-order
   pair except store->load, so only ARM orderings of an earlier store before
   a later load need a fence: LDR/STR/LDAPR = `mov`; STLR = `mov` that sets
   `GuestContext::release_pending` (`xchg` when an LDAR follows in the same
   straight-line code); LDAR/LDAXR (RCsc, ordered after an earlier STLR) =
   `mov`, preceded by a fence only while that flag is set (the flag lives in
   memory, so this holds across regions, calls and exits); DMB
   SY/ISH/OSH/NSH and every DSB = `lock and byte [release_pending],0` (fence
   and clear), omitted when straight-line stores/register-only code lead to
   another full barrier; DMB *LD/*ST (all domains), ISB = no code. LSE =
   `lock` ops. LDXR/STXR = reservation in GuestContext + `lock cmpxchg` (ABA
   accepted, as QEMU/Dynarmic do); recognised LL/SC loops (incl. ADDS/SUBS
   bodies and the DMB after them) fuse into one locked op. Proof and knobs
   (`TRANSMUTE_FX_SYSTEM=oldorder,oldfusion`, folded into the family name and
   so the translator fingerprint): src/fx/translator/NOTES_SYSTEM.md.
4. **System DLLs run natively.** Imports that do not resolve to an ARM64 DLL
   on the guest search path bind once, at load time, to x64 thunks generated
   from the signature database. Pointers pass through unchanged: ARM64 and
   x64 Windows share LLP64 data layouts, CRITICAL_SECTION/SRWLOCK/TEB layouts,
   and `va_list` (a `char*` over 8-byte slots).
5. **Guest code is never host-executable.** Guest code pages are
   PAGE_READONLY (or PAGE_READWRITE for writable code). A host call into guest
   code raises an execute fault that the runtime turns into a callback.
   Translated pages are write-protected; a write fault invalidates only that
   page's translations.
6. **No instruction budgets in production code.** Asynchronous requests use
   `GuestContext::interrupt_pending`, checked at loop back-edges and region
   entries only.
7. **Steady state never returns to C++.** Direct branches link by patching
   rel32, indirect branches use the runtime lookup stub, RET uses the RSB,
   imports are registered host entries (thunks). Unsupported instructions
   exit to the one-instruction interpreter and resume translated code.
8. **Relocatable output.** Translated regions record every absolute guest
   address as a fixup and call helpers through `[r15 + helper_disp]`, so the
   same bytes can be cached on disk or produced by AOT.

### ABI assumptions

- **NZCV across calls and branches: exact.** Every exit keeps NZCV live by
  default (`TranslateOptions::abi_flags = false`, part of the cache options
  hash). AAPCS64 leaves the flags undefined across a public interface, but
  hand-written assembly calls its own local stubs and branches to tables it
  computed earlier, and reads the flags afterwards:
  - dav1d's NEON does `cmp x9, x10; blr x11; b.ge` (save_tmvs_neon);
  - it also does `subs w4, w4, #1; br x3` to a target that branches on the
    flags (splat_mv_neon).
  ffmpeg and libvpx do the same.
  `TranslateOptions::abi_flags = true` (and the measurement knob
  `TRANSMUTE_FX_CORE=brdeadflags`, which extends the assumption to BR) treat
  NZCV as dead at BL/BLR/RET. That is faster and only correct for programs
  that follow AAPCS64 everywhere. Making exact flags cheap is the open work
  (`FX_PERF.md`): lazy materialisation, and flags proven dead across known
  translated targets.

## Stacks

Guest stacks are host `VirtualAlloc` reservations sized per thread, with a
guard region below them; there is no fixed limit (CPython needs ~16 MiB):

- Main thread: the EXE's `SizeOfStackReserve`/`SizeOfStackCommit`.
- `CreateThread`/`_beginthreadex`: the requested size (commit size, or
  reserve with `STACK_SIZE_PARAM_IS_A_RESERVATION`); 0 = image default.
- Override: `transmute-fx --stack-size <bytes|NNk|NNm>` or the
  `TRANSMUTE_FX_STACK_SIZE` environment variable (main thread and
  default-sized threads).
- `GuestContext::guest_stack_base/limit` and the guest TEB's
  StackBase/StackLimit describe the guest stack; guard hits raise a guest
  `STATUS_STACK_OVERFLOW`.

## Shared guest memory

`map_shared_guest_memory` / `alias_guest_memory` / `unmap_shared_guest_memory`
(process.hpp, `src/fx/runtime/shared_memory.cpp`) map pagefile-backed sections
into fx-owned placeholders (VirtualAlloc2 + MapViewOfFile3), at 4 KiB
granularity, so the same pages appear at several guest addresses (Darwin
`vm_remap` of writable memory; alchemy-fx backs `vm_allocate` with it). An
alias keeps the pages after the original is unmapped; an alias replaces whole
views at its destination; unmapped space stays an fx placeholder (no other
allocation can take it) and a reservation is released once it is entirely
placeholder again. Translated code on a shared page is write-protected at every
address mapping that page, and invalidating a page invalidates its aliases
(regions.cpp asks `shared_memory_aliases`). Creating or removing an alias
invalidates both ranges. Private memory (VirtualAlloc) cannot be aliased.
`map_section_guest_memory` views a caller's section (a file mapping; fx keeps
a duplicate handle) in the same placeholders. It places the view at a chosen
address, or base 0 for anywhere. The view may end inside its last page (the
end of a file), and the rest of the range stays placeholder. A read-only
section is viewed read-only. alchemy-fx's `mmap(MAP_SHARED)` and `shm_open`
use it, so separate processes share file pages. `unmap_shared_guest_memory`
accepts placeholder gaps between whole views.
`map_file_copy_guest_memory` views a read-only file section copy-on-write
(`PAGE_WRITECOPY`) at a 4 KiB aligned file offset, the rest of its last file
page and the pages after it zero (a pagefile section). alchemy-fx maps Mach-O
segments with it, so every process shares an image's file pages until it
writes one (docs/ALCHEMY_MEMORY.md). A copy-on-write view refuses
`PAGE_READWRITE`, even on written pages, so the host's `vm::protect` turns
writable protections into their write-copy forms there. Such a view is never
aliased, duplicated or cut: its written pages exist only in it.

## Translated-code conventions

- `r15` = `GuestContext* + kContextBias` (128). `x0..x31` are at disp8
  (`x_disp(n)`); `x[31]` is SP. Use the `ctx_disp()` helpers, never raw
  offsets.
- `rsp` = host stack, 16-byte aligned, with 32 bytes of shadow space owned by
  callees. Translated code may `call [r15+helper_disp(id)]` directly. It
  never pushes or pops except balanced around calls.
- Allocatable GPRs: rax, rcx, rdx, rbx, rbp, rsi, rdi, r8-r14. Host
  callee-saved: rbx, rbp, rsi, rdi, r12-r14 (and xmm6-15) survive helper and
  host calls; place guest callee-saved registers (x19-x29, SP, d8-d15) there.
- **Region entry state** ("flushed"): all guest state in GuestContext, NZCV in
  `ctx->nzcv`, MXCSR already reflects FPCR, except the registers of the
  cross-region convention below, which are in their host registers. Every
  entry point (region start, BL continuation, RSB entry) assumes only this.
- **Cross-region register convention** (`translator_register_convention()`,
  translator.hpp): SP, x19, x20, x21, x22, FP and LR live in rbx, rsi, rdi,
  r14, r12, rbp and r13 at every boundary between pieces of translated code
  (region entries, links, RSB returns, inline lookups); their context slots
  are stale there. Calls and returns between regions therefore load and
  store none of them, and prologues/epilogues save and restore them straight
  from host registers. Inside a region each one is a pin in its host
  (`Plan::globals`; listed in `TranslatedRegion::pins`, so fault sites see
  it), or evicted (`Plan::evicted`): stored at the region's entries and
  reloaded at its exits, while another, hotter guest register is pinned in
  that host (pin selection charges the eviction a store per entry and a load
  per exit). Non-pure helpers store them first, like any dirty pin. The
  runtime keeps the rest consistent: stubs entered from translated code
  (lookup miss at arena offset 0, `stub_link`, `stub_dispatch_exit`,
  `stub_indirect_live`, return-to-host, `rsb_redispatch`) store them; `enter`,
  `stub_indirect` hits and `stub_host_return` (RSB hits of abi thunks and host
  callbacks) load them; every host entry (thunk, callback, trampoline) is
  installed behind an adapter that stores them, so thunks, overrides,
  fibers, longjmp and SEH keep working on the flushed context. The VEH and
  `exit_to_dispatcher` use the non-storing `dispatch_exit`.
  `TRANSMUTE_FX_GLOBALS=<list>` (e.g. `sp,x19,x29,x30`, `none`) picks another
  set, `TRANSMUTE_FX_CORE=noglobals` none (the previous convention); both are
  folded into the translator fingerprint.
- **Exits**: flush dirty pinned registers and flags first.
  - Direct: `jmp rel32` (rel32 field 4-byte aligned) initially pointing at an
    unlinked tail: `mov rax,imm64 target` (fixup); `mov [r15+pc],rax`;
    `lea rax,[rel32 field]`; `jmp [r15+stub_link]`. The runtime patches
    rel32 to the target region's entry and records the initial rel32 at
    install (unlink restores it). In-line exits keep their tail in the
    region's cold section (below).
  - **Code layout** (`TRANSMUTE_FX_CORE=oldlayout` restores the previous
    one; folded into the fingerprint): each region is its body, then a cold
    section (`TranslationStats::hot_bytes`, cache format 5): exit and
    interrupt stubs, unlinked exit tails, the RSB-miss lookup of RET, switch
    fallbacks, interpret/SVC/BRK exits, entry stubs that are not in line,
    constant pool, switch tables. Call continuation entry stubs are laid out
    in line after the call (`coldentries`: cold), except at loop headers;
    only small loops are aligned (`loopalign`: all; `hotalign`: tier-2 loops of
    any size; `align32`: 32 bytes instead of 16); backward branches use
    rel8 in reach; vector context moves use movups. src/fx/translator/
    NOTES_CORE.md has the details, docs/FX_CLASSROOM_PROFILE.md the
    measurements.
  - Indirect: the stub_indirect lookup inlined on the target in rcx
    (fx/runtime/runtime.hpp encoding, `host = ctx->arena_base + (u32)entry`,
    one predicted `jmp` per site); misses and misaligned targets store
    `ctx->pc` and take `jmp [r15+stub_indirect_live]` (an empty slot "hits"
    the miss stub at arena offset 0, which stores rcx as `ctx->pc`).
  - Switch dispatch: a BR through a recognised MSVC jump table (`adr;
    ldr{sw,sb,sh,b,h} [xT, wI, uxtw]; [adr]; add; br`, bound from `cmp; b.hi`,
    `uxtb`/`and`, or the table contents) stays in the region: `wI < n` and
    `xN == target_k` select host table entry k (`{target - region start,
    label - table}`), else the indirect exit. LLVM's lowering (clang,
    Mach-O: `adr xB, base; ldr{b,h,sw} [xT, xI{, lsl #k}]; add xN, xB, xO,
    lsl #2; br xN`, with the table address often set up before a loop and xI
    reused before the BR) needs a bound and dispatches on the target: `xN -
    lo` (aligned, at most `hi - lo`) indexes a dense host table over the case
    pcs, other slots take the indirect exit. The targets are internal edges
    (pins stay, the register cache is empty on them); targets outside the
    region are linkable direct exits. Regions holding a table may grow to 4x
    the instruction limit, so an interpreter loop is one region.
    `TRANSMUTE_FX_CORE=notables` disables it, `tableexits` keeps the table but
    makes every entry a direct exit; `TRANSMUTE_FX_TRACE_TABLES=1` prints BRs.
  - BL/BLR push the RSB entry `{return pc, lea of the continuation entry}`.
    RET compares x30 with the top entry and jumps directly on a hit.
  - Interpret: `mov [r15+pc],imm`; exit_reason = interpret;
    `jmp [r15+stub_dispatch_exit]`.
- **Register cache** (write-back, carried across blocks): unpinned guest
  registers read or written by an instruction may stay in a free scratch
  register (r8-r11/rcx/rdx/rax, xmm0..) and are reused by later instructions
  instead of reloading the context slot. Results stay dirty in their
  register; they are stored to GuestContext before helper calls, branches
  and other exits, interpret exits and block ends, or when the register is
  reused; a result overwritten before such a point is never stored. Fault
  sites need no stores (fault maps, below). Entries die when the register is
  handed out writable, at helper calls and direct context writes. Family
  code must therefore treat read_gpr/read_vec/vsrc/gsrc results as read-only
  and obtain operands before internal labels (EmitContext rules 4 and 5).
  - Victims: a free register without an entry, else the entry cheapest to
    give up by its next use in the following instructions (clean and not
    read again first; then reads after the block, furthest first; dirty
    ones cost a store), LRU among equals.
  - Clean entries cross block edges. Every block has an entry state (guest
    -> scratch register), decided when the block is reached: a single
    predecessor hands over its whole cache; a merge point keeps the entries
    all its forward predecessors hold in the same register (inside a loop
    body only the predecessors inside the loop count; jumps into the loop
    from before it reconcile in an edge stub), so those edges need no code.
    Loop headers keep only registers their first block reads before writing;
    back edges reload or move evicted ones before the jcc (replacing the
    header's own loads). Entry stubs load the entry state; rax is never in
    a merge point's state (edge flag materialization clobbers it).
  - **Fault maps**: every FaultSite may name a fault map
    (`TranslatedRegion::fault_maps/fault_map_regs`): the dirty cached
    registers (guest -> host GPR/XMM) at that site, applied after the pins
    by the vectored handler (`for_each_fault_register`). The site is split
    whenever a dirty entry is stored or dropped inside its host range, so
    the map stays exact.
  - Knobs (`TRANSMUTE_FX_CORE=`): `carrydirty` (dirty entries also cross
    edges inside loops: stored on the way out of the loop, at exit and
    interrupt stubs), `nocarry` (entries survive only into single-predecessor
    blocks), `nofaultmap` (store dirty entries before every fault site),
    `lru` (least recently used victims), `nowriteback` (store at the end of
    each instruction), `nocache`. `nocarry,nofaultmap,lru,oldpins` is the
    previous block-local cache.
- **Vector value facts** (`EmitContext::vec_fact`, `FamilyFacts`): a fusion
  may need a register to hold a known value. Before emission the region
  emitter asks the families what their fusions want (`want`), collects the
  register's reaching definitions over the region's edges (an entry, an
  interpret exit or a conservative instruction on a path defeats it), and
  asks every family what each definition writes (`def`). MOVI-only
  definitions with the value, or vector compares (lane masks), prove it
  (exact). Otherwise, when every definition is a 128-bit load or a MOVI, one
  16-bit flag per (register, value) in the context spill area is written
  right after each of them: an EFLAGS-free check of the loaded value
  (`check`), or a constant store. The flag then describes whichever
  definition ran last, i.e. the one the register holds. Values are never
  inferred from register numbers, addresses or memory contents seen at
  translation time. A region that relied on a flag and also calls a
  state-writing helper is retranslated without facts. Uses: the Embree
  movemask fusion (`emit_neon_fusion.cpp`: AND 1 or USHR #31, USHL by
  {0,1,2,3}, ADDV, FMOV -> MOVMSKPS; NaN/FPSR behaviour stays the compare's,
  which is not fused). `TRANSMUTE_FX_CORE=nofacts` (inline value checks),
  `TRANSMUTE_FX_NEON=nomaskfusion`; `TRANSMUTE_FX_TRACE_FACTS=1` prints
  every query.
- **Pins**: up to nine guest GPRs (rbx rsi rdi r14 r12 r13 rbp, then volatile
  r11/r10, saved and reloaded around helper calls) and ten vector registers
  (xmm15 downwards) per region, chosen by loop-weighted block-boundary
  traffic: a block's first read and first write of a register count fully,
  further uses (served by the register cache) a quarter
  (`TRANSMUTE_FX_CORE=oldpins`: every use counts fully). With the
  cross-region convention its registers keep their hosts; other registers
  get the remaining hosts, r11/r10, or evict a convention register.
- **Faults**: every x64 instruction that touches guest memory is covered by a
  `FaultSite` (host range -> guest PC, flags location, fault map). Pinned
  registers map through the region pin map, dirty cached registers through
  the site's fault map. The runtime's vectored handler rebuilds the guest
  state from these and calls `dispatch_guest_exception`. An ADD/SUB folded
  into the addressing mode of the next load/store (integer: core_isel.hpp;
  SIMD&FP: `EmitContext::address`, NOTES_NEON.md) reports the ADD's pc with
  its destination unchanged; `TRANSMUTE_FX_CORE=nofold` /
  `TRANSMUTE_FX_NEON=nofold` disable the folds.
- **FP**: MXCSR mirrors FPCR (RMode, FZ->FTZ|DAZ) while guest code runs; it
  is only rewritten on MSR FPCR and on thread entry. Host NaN propagation and
  default-NaN encodings are accepted. FPSR cumulative bits are read lazily
  from MXCSR (MRS FPSR) and written to MXCSR (MSR FPSR). Region entry moves
  the mirrored bits (IOC..IXC) from `ctx.fpsr` into MXCSR; dispatcher exits
  fold them back. While guest code runs, FPSR is `ctx.fpsr | flags(MXCSR)`,
  so C++ code reading FPSR must call `fp_sync_fpsr` first.
- **FP at host-code boundaries** (process.hpp, "FP state at host-code
  boundaries"): every piece of host code that receives a `GuestContext&`
  sees `ctx.fpsr`/`ctx.fpcr` as the architectural FPSR/FPCR, and what it
  leaves there is what the guest sees next (MRS FPSR, cleared flags, the
  rounding/FZ of the next FP instruction). Dispatcher exits already fold
  (SVC handler, raises, the fault hook and exception dispatch after a
  translated-code fault, interrupt exits), and region entry loads MXCSR from
  `ctx`. Host code reached without a dispatcher exit is bracketed
  (`src/fx/runtime/fp_boundary.hpp`): before it, `ctx.fpsr |=
  flags(MXCSR)`; after it, `MXCSR = fp_mxcsr_for_state(ctx.fpcr, ctx.fpsr)`
  with the mirrored bits leaving `ctx.fpsr`. MXCSR is not written before the
  call (one ldmxcsr per call cost 10-60 ns here after any inexact guest
  operation), so its flags are stale while the host runs and flags the
  host's own arithmetic raises are dropped; `ThreadState::fp_host` marks
  that window so that `exit_to_dispatcher` and invoke's C++-exception path
  do not fold the stale flags back. The bracket is emitted in the
  `register_host_callback` trampoline (with the marker; about +2 ns per
  callback, no ldmxcsr unless the callback changed FPCR or the flags) and
  the abi override thunks (RtlCaptureContext, RtlRestoreContext, longjmp,
  RaiseException, fenv, fibers; the fenv overrides take the host CRT's
  MXCSR result into `ctx.fpsr`), and called as C++
  (`fp_state_to_context`/`fp_state_from_context`) around interrupt requests
  run by `helper_check_interrupt`. Hand-written `register_host_entry` code
  runs in the live state and calls the same two functions around C++ that
  takes `ctx`. Tests: `src/fx/runtime/tests/runtime_fp_boundary_tests.cpp`,
  `override_thunk_fp_handoff` (abi), the seh capture/restore/longjmp and
  fault-hook tests.

## Region formation and tiers

- A region follows direct control flow from its start inside the function's
  .pdata bounds, up to `max_instructions` (4096) or, with .pdata bounds, the
  whole function up to 16384 instructions (`TRANSMUTE_FX_CORE=oldcap`: the
  4096 cut; regions started inside a cut function re-translated most of it),
  and stops at pcs that already have an entry. Code without known bounds (Mach-O images, guest
  JIT code) gets synthetic bounds: `TranslateOptions::window_forward/backward`,
  8 KiB / 2 KiB around the start in the JIT (`TRANSMUTE_FX_TIER_WINDOW=<fwd>[,<back>]`).
  Without them, every region chased tail calls and the fall-throughs of
  noreturn calls through neighbouring functions. macOS Blender `--version`
  translated 14.1M guest instructions for 0.83M distinct ones; now 0.88M.
- First translations are tier 0 (`TranslateOptions::lazy`). A tier-0 region
  that formation cut short is provisional: its first-tier budget
  (`TRANSMUTE_FX_TIER_BUDGET`, default 4096) ran out, or, with
  `TRANSMUTE_FX_TIER_SPLIT=1`, it stopped at a call continuation. The
  continuation's RSB entry is then a linkable direct exit, and the
  continuation becomes its own region on return. Such a region counts its
  region-start entries: `dec dword [rip+counter]; jz tier_up`. The counter
  is in an arena page that holds no code (a counter page of the region's
  code chunk), and the runtime patches the rel32 at install. After `TRANSMUTE_FX_TIER_THRESHOLD` entries (default 4096), it
  exits with `ExitReason::tier_up`. The runtime then installs a tier-1
  region for that pc, from the cache if it holds one, else from an eager
  JIT translation. Tier-1 formation stops only at host entries.
  `install` hands every entry of a lower tier that the new region covers to
  the new region: the map, the lookup table, the links (unlinked, then
  relinked) and matching RSB slots. The tier-0 code stays valid for threads
  inside it and for stale links, and stays registered on its pages, so SMC
  invalidation kills it too. Regions that formation did not cut are final
  (tier 1) and carry no counter.
- Defaults keep the first tier eager. Measured on Windows Blender (tiny
  render), splitting at calls translated 3.4x the regions and 16% more guest
  instructions, because continuation regions re-cover shared code. A
  512-instruction budget translated as much as eager. Only 29% of the eagerly
  translated code never runs (basic-block measurement), and cutting regions
  costs more per region than it saves. The knobs stay for experiments.
  `TRANSMUTE_FX_CORE=notier` restores the formation and return stubs used
  before tiering. It is folded into the translator fingerprint.
- **Tier 2** (optimizing tier, on by default since ABI 21;
  `TRANSMUTE_FX_CORE=notier2` turns it off, `TranslateOptions::optimize`
  overrides). Tier-up counts loops only: a first translation (`lazy`) that
  contains loops is tier 0 and every loop header with an interrupt check
  does `dec dword [rip + slot]; jz`, one counter per region (16 x
  `TRANSMUTE_FX_TIER_THRESHOLD` = 65536 iterations) in a 64-byte line of its
  own after the code, so no code shares it and the runtime need not repoint
  it. When it runs out the region exits like an interrupt with
  `ExitReason::tier_up_loop` (exit_arg = region start, pc = the header); the
  runtime retranslates the region start eagerly, which is the optimized
  translation, and re-dispatches the header (a region of its own while that
  loop still runs; later calls enter the tier-2 region). Regions without
  loops are final, so code without loops pays nothing. Classroom 10%:
  8,420 counted regions, 418 tier-ups (0.16M guest instructions, 129 ms of
  JIT, cold cache); the prototype's entry counting re-translated 2,679
  regions (0.53 s). Warm caches hold the tier-2 records (tier 1), which
  lookups prefer. Eager translations (tier-ups, AOT, `notier`) are tier-2
  code; they cost about 1.6x the plain JIT time (two passes).
  Code generation (src/fx/translator/region_alloc.cpp, NOTES_CORE.md):
  interrupt checks only where needed (a loop header keeps none when another
  checked header lies on every path from it to its back edges); no
  alignment padding where the code above falls into a loop header; pin
  weights 8^loop depth; liveness: a dirty cached value that no path
  observes again (reads, faults, exits, helpers, interrupt checks) is
  dropped instead of stored; two passes: loop headers start with the
  registers their back edges hold (tier 1's header rule as the filter), and
  the loop's carried dirty values stay in registers across its branches and
  back edges, stored on the way out; GPR copy aliases (MOV Xd, Xn emits
  nothing, like the vector aliases below); derived vector values (TBL's
  adjusted indices computed into `GuestContext::vspill` after each
  definition of the index register outside the loop, then read as a
  memory operand). Fault maps describe everything that stays in registers.
  Knobs (`TRANSMUTE_FX_CORE`, folded into the fingerprint): `t2nochecks`,
  `t2noalign`, `t2noalloc`, `t2nodead`, `t2nogalias`, `t2noderived`,
  `t2noalias`, `t2proto` (all of them and entry counting: the prototype),
  `t2entries` (entry counting), `t2loops` (one per-thread loop counter,
  tier-up at the header), opt-in `t2lend` (pins a loop never uses lend their
  host to the register cache inside it; measured slower) and `t2carry`.
  Vector copy aliases
  (`MOV Vd.16B, Vn.16B` into unpinned registers emits nothing; Vd names the
  host holding Vn, or Vn's context slot when Vn is only there; a copy back
  of the value Vd's own slot holds leaves Vd clean, so loop rotations cost
  one copy; aliases are block-local, dirty register aliases are in fault
  maps, slot aliases are copied into their own slots before instructions
  that may fault, helpers, exits and block ends, and before their source
  slot is overwritten). An alias that reaches a point needing none
  retranslates the region without aliases. `TRANSMUTE_FX_TRACE_ALIAS=1`
  prints every copy considered. Measurements: docs/FX_CLASSROOM_PROFILE.md.
- **Call inlining** (`region_plan.cpp`): a BL whose target is a small leaf
  is translated as straight-line code. The callee body is every instruction
  reachable from the target through direct control flow, inside its .pdata
  bounds (which must start at the target) or a window after it: at most
  `TRANSMUTE_FX_INLINE` (default 24) instructions, no calls, SVC/BRK/HLT,
  interpret exits or indirect branches other than RET X30, and no other read
  or write of x30. The body is copied after the BL in layout (plan segment
  k; copies are never entries). The BL only writes x30 = return address,
  which stays in the register cache (fault maps describe it; a later write
  drops the store), and each RET X30 of the copy falls through or jumps to
  the continuation. No exit, RSB push/pop, pin flush/reload or cache flush
  happens, and pins, the register cache and flags analysis span the call.
  Faults report callee pcs with the exact x30 and SP, so SEH unwinds the
  leaf as on hardware. Direct branches out of the body (tail calls such as
  `__security_check_cookie`'s jump to `__report_gsfailure`) are ordinary
  direct exits; the later RET misses the RSB and looks the return address
  up. `TranslatedRegion::guest_ranges` covers the callee (sorted, merged), so
  SMC invalidation, cache hashes and AOT see it. `TRANSMUTE_FX_CORE=noinline`
  disables it, `inlinehot` defers it to tier 1 (a region that would inline
  starts as a counted tier-0 region); both and `TRANSMUTE_FX_INLINE` are
  folded into the translator fingerprint. `TRANSMUTE_FX_TRACE_INLINE=1`
  prints every region with inlined calls.
- Cache: records carry the tier (`kRecordTier0`, format 4). Lookups and
  merges prefer tier-1 records. A merge keeps one record per start: the
  latest tier-1 one, else the latest, at the first-use position of that
  start. AOT always translates eagerly.
- JIT throughput: `fx_translator_image_audit` with
  `TRANSMUTE_FX_JIT_BENCH=<stride>[,<reps>][,plan|lazy]` translates every
  stride-th .pdata function single-threaded. It reports ns/instruction and a
  hash of the output, so speedups can be checked for identical code.
  `TRANSMUTE_FX_JIT_BENCH_SAMPLE=1` adds a profile,
  `TRANSMUTE_FX_JIT_BENCH_LINES=<function>` gives line counts for one function,
  and `TRANSMUTE_FX_JIT_BENCH_CPU=<n>` pins the run.

## Components and ownership

Each component builds its own static library and test executable so a broken
file in one component never blocks another. Edit only files you own. Shared
headers in `include/transmute/fx/` are additive-only. If you need a shared
change, add it without renaming or removing anything and record it in your
final report.

| Component | Directory | Library / tests | Owns |
|---|---|---|---|
| common | `src/fx/common` | `fx_common` | x64 assembler, shared helpers (integration) |
| testing | `src/fx/testing` | `fx_testing` | test framework, oracle, snippet runner, `MemoryEmitContext` (integration) |
| translator core | `src/fx/translator` (`core_*`, `emit_integer*`, `emit_branch*`, `emit_load_store*`, `region*`) | `fx_translator`, `fx_translator_tests` | region formation, register pinning, flags, exits, fixups, fault map, integer/branch/load-store emitters |
| fp | `src/fx/translator/emit_fp*` | `fx_emit_fp`, `fx_emit_fp_tests` | scalar and vector floating point |
| neon | `src/fx/translator/emit_neon*`, `emit_vector_memory*` | `fx_emit_neon`, `fx_emit_neon_tests` | integer SIMD, permutes, vector loads/stores |
| atomics/system | `src/fx/translator/emit_atomic*`, `emit_system*` | `fx_emit_system`, `fx_emit_system_tests` | LL/SC, LSE, barriers, MRS/MSR, cache ops, hints, CRC/crypto |
| runtime | `src/fx/runtime` | `fx_runtime`, `fx_runtime_tests` | code arena, lookup, stubs, dispatcher, linking, threads, call_guest, VEH, SMC, interpreter fallback |
| loader | `src/fx/loader` | `fx_loader`, `fx_loader_tests` | PE mapping, relocation, imports, TLS, DllMain, module APIs, `transmute-fx.exe` |
| abi | `src/fx/abi`, `tools/sigdb` | `fx_abi`, `fx_abi_tests` | signature database, host thunks, callbacks, API/CRT overrides |
| seh | `src/fx/seh` | `fx_seh`, `fx_seh_tests` | ARM64 unwinder, exception dispatch, RaiseException, longjmp |
| cache/aot | `src/fx/cache` | `fx_cache`, `fx_cache_tests` | persistent translation cache, AOT driver |
| corpus/bench | `tests/fx/corpus`, `tests/fx/bench`, `scripts/fx_*` | — | ARM64 CRT toolchain, end-to-end programs, native-vs-fx benchmarks |

Each component lists its sources in `src/fx/<component>/component.cmake`
(which it owns); `cmake/TransmuteFx.cmake` includes them.

## Building and testing

Use a private build directory per agent so parallel builds never collide:

```powershell
cmake -S . -B build-fx-<component> -G "Visual Studio 17 2022" -A x64 -DTRANSMUTE_FX_ONLY=ON
cmake --build build-fx-<component> --config Release --target <your targets>
```

`TRANSMUTE_FX_ONLY=ON` builds only the oracle library and fx targets. Tests
are plain executables built on `src/fx/testing/test.hpp`; run them directly.
Differential tests compare translated execution with
`fx::testing::Oracle` (the reference interpreter on direct host memory).

## Performance gates

`tests/fx/bench` contains the same C sources built for ARM64 (run under fx)
and x64 (run natively). Pin both to the same core, report medians of five
runs, and compare ratios. Targets for the first integration: compute and
memory loops within 2x native, a Win32 call within 10 ns, a lock pair within
20 ns, malloc/free within 50 ns, atomics within 2x native, and eight-thread
weak scaling within 20% of native.

## Profiling

`TRANSMUTE_FX_SAMPLE=<file>[,<interval_us>][,main][,stacks][,bg]` (or `=1` for
`%TEMP%\fx-profile-<pid>.txt`) starts the in-process sampling profiler
(`src/fx/runtime/profiler.cpp`). Every interval it suspends each guest thread
and records the host RIP. Translated code is mapped to the guest function
(.pdata) and guest pc (nearest fault site); host code is mapped to a module
and symbol; ntdll waits are attributed to the guest caller (x30). The report,
per-thread breakdowns, and code dumps of the 24 hottest regions
(`<file>.r<N>.bin`, `<file>.regions.txt`) are rewritten every two seconds.
Use 2000 us for many-threaded runs: suspending ~30 threads at 2 kHz slows
the guest noticeably. Flags: `main` samples only the loader's main thread
(also before it runs guest code); `stacks` copies the top 32 KiB of the host
stack of each non-translated sample (a memcpy while suspended), unwinds the
copy after resuming and adds inclusive, first-non-system-frame, innermost
runtime-frame and folded-stack reports; `bg` also samples the runtime's
preloader threads.

Measurement window: `delay=<seconds>` starts sampling that long after
`runtime_initialize` (leave out startup), `duration=<seconds>` stops it that
long after it started (the report then keeps exactly that window), `paused`
starts with sampling off. Example:
`TRANSMUTE_FX_SAMPLE=C:\p.txt,1000,main,delay=20,duration=30`. At run time,
`fx::profiler_set_enabled(bool)` pauses/resumes and `fx::profiler_reset()`
drops the samples so far (process.hpp); alchemy-fx binds them to presented
frames with `ALCHEMY_FX_PROFILE_WINDOW=first-frame[,<s>]|period=<s>`. A paused
profiler suspends no thread. Every report file is written to
`<file>.tmp<pid>` and renamed over the old one, so a killed process leaves
the last complete report, never an empty one.

- `tools/fx_symbolize.py <file>`: adds guest symbol names (DbgHelp + the
  guest PDB; blender.exe from `artifacts/blender-symbols` by default).
- `tools/fx_profile_annotate.py <file> <N>`: host disassembly of hot region N
  with per-instruction sample counts and the guest address of each fault
  site.
- `tools/fx_profile_mix.py <file> <count>`: sample-weighted host instruction
  mix (context traffic, flag materialization, zeroing, ...) of hot regions.
- The report ends with the hot code footprint (regions covering 50/90/99% of
  translated samples: host bytes and pages, body bytes and pages, guest
  bytes, expansion) and the guest code those regions translate more than
  once, by function.
- `tools/fx_layout_audit.py <profile> <fx_translator_image_audit.exe> <dir>
  [count] [VAR=value]`: translates the hottest regions' starts statically and
  splits their bytes into body and cold section, NOP padding, exit tails,
  context operands (disp8/disp32), rel32 branches in rel8 reach and byte
  categories. `fx_translator_image_audit` prints body and cold bytes of a
  whole image (`TRANSMUTE_FX_AUDIT_THREADS=<n>` limits its workers).
- `tools/fx_loop_count.py <audit.exe> <image> <region rva> <guest path>
  <dir> [VAR=value]`: host instructions and context loads/stores of one
  iteration of a guest loop path in the static translation (edge stubs
  included; the audit writes `r_<rva>.map`, every guest instruction's host
  offset). `tools/fx_tier2_estimate.py <profile> <audit.exe> <dir> <base>
  <settings...>`: the profile's samples per guest block over host
  instructions as block frequencies, and the dynamic instruction count of
  the same blocks under other settings (e.g. `TRANSMUTE_FX_CORE=notier2`).
- `scripts/fx_classroom.ps1`: Classroom render, native vs fx (wall time,
  Blender render time, CPU seconds); compare images with
  `tools/fx_imgdiff.py`. The test machine is a 15 W i7-1355U: runs longer than
  a few seconds leave turbo, so compare CPU seconds and short runs.

Startup timing:

- `transmute-fx --verbose`: loader phases (map, bind, protect, DllMain/TLS
  init, time since process creation) after the static closure is loaded.
- `TRANSMUTE_FX_STATS=1`: at exit, runtime and cache counters plus time spent
  in `translate_and_install` on guest threads (cache lookup, JIT, install),
  regions installed by the preloader and the exiting thread's CPU time.
  The `aot:` line adds AOT-file use and the JIT cost per guest instruction.
  The `startup:` line gives contended region-map lock waits on guest threads,
  preloader busy time and when the preload queue drained; the `exit:` lines
  time the exit path (cache flush, ExitProcess, guest DLL_PROCESS_DETACH).
  Note: host OPENGL32.dll (imported by usd_ms.dll) sleeps ~60 ms in its
  DLL_PROCESS_DETACH, natively too.
- Switches for A/B runs: `TRANSMUTE_FX_PRELOAD=<threads>` (0 = no cached-region
  preloading; Alchemy sets 0 in its child processes unless it is already set), `TRANSMUTE_FX_IMAGE_VIEWS=0` (copy images instead of mapping
  them as image sections), `TRANSMUTE_FX_CACHE=0`,
  `TRANSMUTE_FX_BACKGROUND_AOT=0|<threads>` and
  `TRANSMUTE_FX_BACKGROUND_AOT_DELAY=<seconds>` (see below).

## Translation cache files

Per module key (`<cache dir>\<module>.<key16>`):

- `.fxc`, the main file. It holds the records the guest used, in first-use
  order, and the preloader installs them at startup.
- `.fxj`, one journal per process. It collects JIT regions and AOT records the
  guest used; `cache_flush` folds journals into the main file once they reach
  max(1 MiB, main / 8) or 8 journals (checked from the directory listing, so a
  warm exit costs ~0.5 ms). Regions first used by guest DLL_PROCESS_DETACH
  (after `cache_flush`) are appended by `cache_flush_journals`; journals left
  by earlier processes are folded into main files of at most 8 MiB at the next
  exit regardless of size, so the preloader installs them (Blender translated
  ~6k detach regions, ~45 ms, on every warm exit before).
- `.fxa`, the cold AOT file. It covers every function of the image. Lookups
  fall back to it after the main file and the journals, and the preloader
  never walks it.

Background AOT: a module with neither a main nor an AOT file is queued once
the JIT has stored 200 of its regions. Once the guest process has run 20 s,
`transmute-fx --aot-background` translates the queued images in a detached,
idle-priority helper process with up to hardware threads / 4 workers (1..4),
writing `.fxa` atomically under `<stem>.aot.lock`. When the helper exits,
the guest process maps the new files. The 20 s delay exists because a short
render's JIT costs about 1 s, which a helper competing for the same 15 W
power budget outweighs. AOT of all of blender.exe takes ~10 CPU-seconds and
writes ~700 MiB. `transmute-fx --aot <image>` (explicit) still writes the
main file.

External modules (Mach-O images registered with `register_external_module`)
cannot be AOT-translated from a file by a helper process. `aot_external_module`
(cache.hpp, `src/fx/cache/external_aot.cpp`) translates them in-process from
their mapped code with 1..3 background workers: flow-ahead from the start (the
direct targets and the next two functions of every region the guest starts
using, translated into an in-memory pool that `cache_lookup` consults after the
main file and journals; used records go to the journal, unused ones are dropped
at exit), and after `TRANSMUTE_FX_BACKGROUND_AOT_DELAY` every function start of
images with at most `TRANSMUTE_FX_EXTERNAL_AOT_STATIC` (2) MiB of text, written
as their `.fxa`. Nothing runs for a module that has a main file. Knobs:
`TRANSMUTE_FX_EXTERNAL_AOT=0|<threads>`, `_DEPTH`, `_STATIC`, `_WAIT`,
`_BOUNDS` (external_aot.cpp). Measured on macOS Blender's headless cold start
(`scripts/fx_alchemy_cold_start.ps1 -Mode fresh`, 2026-10-11, load 21-39%):
JIT translations 21.9k -> 10.9k (11k pool hits, 51%), time in
`translate_and_install` 0.86-1.18 s -> 0.78-1.00 s, 2.3-3.0 s of worker CPU;
time to the startup script 6.7-8.7 s (baseline) vs 7.8-8.2 s, i.e. within
run-to-run noise. Expect gains only where the JIT, not shader compiles or
loading, is the critical path. With a 0 s delay the static phase wrote 25
`.fxa` files (41 MB) during one start.

## Process exit

Every guest exit path goes through `loader_process_exit`: CRT exit/abort,
ExitProcess/RtlExitUserProcess, and the return of the entry point. Guest
TerminateProcess/NtTerminateProcess of the current process goes through
`loader_process_terminate`, which skips DLL_PROCESS_DETACH, as native
Windows does. Both first call `runtime_prepare_exit`, which stops background
AOT requests and parks the preloaders outside every lock. Only then do they
run the cache flush, the profile flush, and the host ExitProcess, which kills
other threads wherever they are. The loader's process-detach hook runs guest
DLL_PROCESS_DETACH only if `runtime_exit_safe()` finds no runtime or cache
lock left held by a terminated thread. It never waits, and skips guest detach
with a message otherwise. The same check covers host code that calls
ExitProcess directly. Fatal runtime errors (`fatal`, `abort`) and unhandled
guest exceptions (`TerminateProcess`) run no detach code.
- The test machine runs an EDR that hooks process creation and DLL loads:
  any process spends ~60 ms before `main`, and the first runs of a freshly
  built binary are slower while it is scanned. Run new builds once before
  timing them.

## Scheduling on hybrid CPUs

Windows gives a new process launch QoS for about 3 s. After that, a process
without a foreground window (e.g. `blender -b` from a hidden console) drops to
low QoS, and its busy threads move to E-cores. Native short renders finish
inside that window; translated ones mostly do not. `transmute-fx` therefore
sets HighQoS on the guest process:

- `TRANSMUTE_FX_QOS=high|default|eco` (default `high`). An explicit state set
  by a parent wins, and guest `SetProcessInformation` / `SetThreadInformation`
  calls still override it.
- `TRANSMUTE_FX_BACKGROUND_QOS`: QoS for runtime background threads
  (preloaders, journal writer, AOT scheduler) and the `--aot-background`
  helper process. Default: EcoQoS, so they leave P-cores to guest work.
- `TRANSMUTE_FX_CORESTATS`: per-thread P-core/E-core residency diagnostics
  (`src/fx/host/win32/corestats.cpp`, also buildable as a standalone DLL to
  measure native processes).

Compare native and fx at equal QoS. Pinning both to logical processor 0
(`scripts/fx_classroom.ps1 -Affinity 1`) is the most stable comparison on the
test machine.
