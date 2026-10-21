# Translator core: non-obvious decisions

Files: `core_decode.*` (A64 decoder for core-owned encodings), `region_plan.*`
(region formation, liveness, dirty-pin dataflow, pin selection, family
registry), `region_emitter.*` (RegionEmitter = the optimized EmitContext +
exits/stubs), `emit_integer.cpp`, `emit_load_store.cpp`, `emit_branch.cpp`,
`core_translator.cpp` (translate_region + retry). Tests: `core_*tests.cpp`,
test dispatcher `core_test_machine.*` (also used by the neon family's
region tests: keep its API additive).

## Region layout
- Instructions are laid out in guest address order; each appears once.
  Fall-through is always "next in layout". Region start entry is at offset 0
  (interrupt check, pin loads, jump to its block if it is not first).
- Entries: region start, BL/BLR continuations, and pc+4 after SVC and after
  instructions planned as interpret exits (the runtime dispatches pc+4 after
  interpreting, so registering it avoids a fresh translation).
- Loop headers (targets of edges from a later or equal address) start with
  `cmp dword [r15+interrupt_pending],0; jne`. Checking at the header keeps
  host flags intact on `subs; b.ne` back edges. The start entry also checks
  (cross-region cycles have no internal back edge). Headers of small loops
  (layout range < 24 guest instructions) are aligned to 16; larger ones are
  not (measured neutral; `loopalign` aligns all, as before).
- Body, then the cold section (TranslationStats::hot_bytes marks the split):
  conditional exits, flag-materializing edges, interrupt exits, return
  trampolines, the unlinked tails of in-line direct exits (the body keeps
  only `[nop pad] jmp rel32`; the runtime records the initial rel32 at
  install, so the tail may be anywhere), the RSB-miss lookup of RET, the
  switch fallback, interpret/SVC/BRK exits (`jmp` to a stub that stores the
  pins), entry stubs not laid out in line, then the constant pool and switch
  tables. A BL/SVC/interpret continuation's entry stub is laid out in line,
  right after the code that never falls through (one jump less per return),
  unless the continuation is a loop header (it keeps its alignment).
- Branches to bound (backward) labels use rel8 when in reach. Context moves
  of vector registers use movups (one byte shorter than movdqu). BL loads
  the return pc straight into x30's pin; RET compares x30's pin with the RSB
  (rcx only on the cold miss path).
- `TRANSMUTE_FX_CORE=oldlayout` restores the previous layout byte for byte
  (checked with the JIT bench output hash), `coldentries` keeps every entry
  stub cold.
- Regions with .pdata bounds may cover the whole function, up to 16384
  instructions (`oldcap`: max_instructions, 4096): a function cut at the
  limit was translated again from every exit into its untranslated part.

## Pinning
- Score per guest register = loop-weighted block-boundary uses (a block's
  first read and first write count fully, other uses a quarter; oldpins:
  every use) - (a load at every entry:
  weight 1 for the region start, the loop weight for continuation entries)
  - (a store at every exit where it may be dirty: calls/returns weighted by
  their loop, conditional exits by 1). Pinned if score >= 1, x18 never.
- Up to 7 GPRs in host callee-saved rbx rsi rdi r14 r12 r13 rbp; SP prefers
  rbx. Regions with no family instruction (hence no helper calls) also get
  r11 and r10 (9 pins). Scratch: r8 r9 r10 r11 rcx rdx rax minus pins (>= 5).
- Vector registers (usage from family describe()) are pinned the same way in
  xmm15 downwards to xmm6 (callee-saved: survive helper and host calls);
  scratch xmm = the rest. Pins are listed in TranslatedRegion::pins with
  vector = true; dirty vector pins are flushed with movups (movdqu with
  `oldlayout`).
- Retries on scratch exhaustion: fewer vector pins (10 -> 4 -> 0), then no
  r10/r11 pins, then interpret the instruction.
- Entries load all pins, except GPR pins the straight-line code from that
  entry overwrites before anything observes them (a read, an access that
  may fault, an ADD a following access folds, a family or interpreted
  instruction, any branch or block end; not at loop headers, whose interrupt
  stub stores dirty pins): pins_dead_at_entry, `TRANSMUTE_FX_CORE=
  nodeadpinloads` loads them all. V8's bytecode handlers start by
  overwriting a pinned scratch register. Exits store only possibly-dirty
  pins (forward dataflow over the region CFG, entries start clean).
- Cross-region convention (translator_register_convention; docs/FX_ENGINE.md):
  its registers are pins in fixed hosts that entries never load and exits
  never store (current_dirty() always includes them, so helpers store them).
  Pin selection gives non-convention registers the free hosts, then r11/r10,
  then evicts the convention register with the smallest uses + (entries +
  exits) cost: entry_pins() stores it, exit_pins() reloads it. Paths still
  in the entry state (region-start interrupt/tier-up stubs, return
  trampolines) skip the reload (Stub::boundary, emit_direct_exit restore).
- Switch dispatch (Plan::tables, e_table_branch): see docs/FX_ENGINE.md.
  Measured on pybench (A/B pairs on the two P-cores, with the convention):
  `notables` (every case its own region, BR = lookup exit) 2-20% slower;
  `tableexits` (table dispatch to per-case regions through linked exits)
  about equal overall, 5-10% slower on the interpreter-bound tests. Without
  the convention, in-region dispatch alone was no faster than per-case
  regions: one region-wide pin set serves the cases worse than per-case pins.
- Convention set: SP,x19,x20,x29,x30 (5) and SP,FP,LR (3) measured within
  noise of the default 7. Against `noglobals`, call-heavy pybench tests are
  10-45% faster; zlib/md5 (leaf loops whose hot registers go to r10/r11 or
  evict: REX bytes, entry/exit traffic) measured equal over 10 A/B pairs.
  Host calls pay the adapter's stores and stub_host_return's loads (~2-4 ns
  per Win32 call in fx_bench). Scaling the eviction cost by 4 or 16: no
  measurable difference.
- Non-pinned guest registers are used as `[r15 + x_disp(n)]` operands;
  their writes are stored at the end of the instruction.

## Lazy flags
- FlagsState = ctx | host_sub | host_add | host_logic. logic uses the add
  condition map (CF = OF = 0); HI/LS on add do `cmc` and switch to sub.
- A block inherits the exact flags state only if it has one predecessor
  instruction laid out before it and is not an entry; every other block
  starts with flags in ctx->nzcv, so edges carrying live host flags into it
  materialize (inline on fall-through, edge stub on taken jcc).
- Materialization (no pushfq): `[cmc] lahf; seto al; and eax,0xC101;
  imul eax,eax,0x10210000; and eax,0xF0000000; mov [nzcv],rax`.
- NZCV is dead at BL/BLR/RET exits (abi_flags): host flags are dropped
  there, not materialized (begin_instruction). Also at a BR that does not
  look intra-function (no recognised table, no ADR/ADRP in its block): tail
  calls, veneers and V8's bytecode dispatch, whose BR otherwise kept NZCV
  live back through the handler (materializations at its loop headers and
  merge points too). Measured: jitless JetStream richards 1.16x. A clang
  switch table or computed goto builds its target from a PC-relative table
  and keeps the strict model; `TRANSMUTE_FX_CORE=nobrdeadflags` keeps every
  BR live, `exactflags` / abi_flags = false everything.
- Flags that outlive an instruction that must clobber EFLAGS (EOR/ORR/AND,
  shifts without a flag-free form, REV, ...) and are read within three
  instructions stay in host form: `lahf; seto al` before it and
  `add al, 0x7f; sahf` after it (save_flags_around; not for instructions that
  may fault, branch, read or write flags, or with rax in use; an implicit
  rax user such as SMULH falls back to materializing).
  `TRANSMUTE_FX_CORE=noflagsave` always materializes.
- Conditions with flags in ctx: single-bit `test dword [nzcv],bit`, else
  `bt` against a 16-bit truth table indexed by NZCV.
- Liveness (backward, exits live) decides `keep_flags()`: emitters then
  pick flag-free forms (lea add/sub, shlx/shrx/sarx/rorx, mulx, not+lea
  negate, cmov/setcc) instead of materializing.
- CCMP/CCMN: branchy; false path loads the immediate NZCV into EFLAGS with
  `mov eax,img; add al,0x7f (OF); sahf`. When the next instruction is its
  only reader (B.cond or CSEL family, same block, NZCV dead after it, at
  most three flag-free non-faulting instructions in between) it is
  branchless instead: with K = the reader's condition on the immediate
  NZCV, `set<c1|!c1> t1; cmp; set<c2> t2; test|or t1, t2` and the reader
  uses NE (e_ccmp, bool_reader_; `TRANSMUTE_FX_CORE=noccmpfusion`). An
  instruction in between that must clobber EFLAGS parks the answer first
  (park_bool: setnz into spill slot 61; the reader tests it there). The
  dead NZCV is then stale in the context, as at merge points and calls.

## Register cache across blocks
- s_.cache_slot[leader] -> entry state (CacheSnapshot). Single-predecessor
  leaders take the predecessor's state at the edge. Forward edges into
  merge points are *pending* (pending_edge: snapshot + a fresh label); the
  state is decided when the block is reached (merge_entry: entries every
  relevant predecessor holds in the same register; relevant = inside the
  innermost loop for loop-body blocks, never the region-start edge when
  others exist). start_block binds each pending label at the block (state
  fits) or emits an edge stub that reconciles (and materializes flags).
- Loop = layout range [header, last back-edge source]; innermost wins.
  Headers keep only registers read in their first block before written.
- reconcile(current -> entry): stores of dirty entries the target does not
  keep dirty, then moves ordered so no source is overwritten first (cycles:
  one value goes through the context), then loads. Edge code runs on one
  path, so it bypasses the "store inside internal control flow" check.
- Back-edge jcc: reconcile inline before the jcc (not-taken path keeps the
  untouched entries, now clean). Conditional exits / interrupt checks: the
  stub stores the snapshot's dirty entries before materializing flags (rax).
- Default carry_clean: dirty entries are stored before every branch and
  block end, so entry states are clean (exit paths stay short; carrying
  dirty values, knob carrydirty, measured no faster and grows code).
  Exception (dirty_crosses_branch; `TRANSMUTE_FX_CORE=nobranchdirty`
  restores the stores): a B.cond/CBZ/TBZ whose fall-through continues the
  block and whose taken target has it as its only predecessor (or is an
  exit, whose stub spills the dirty snapshot) stores nothing: both paths
  take the cache over as it is and fault maps describe the dirty values.
  V8's JIT code stored temporaries before nearly every type-check branch.
  Opt-in `TRANSMUTE_FX_CORE=mergedirty` (dirty_into_merge): forward jumps,
  fall-throughs and taken edges into merge points outside loops carry their
  dirty entries too; the merge keeps those every predecessor holds in the
  same host (dirty if dirty in any) and the edges store the rest.
- rax never appears in a merge point's entry state: flag materialization on
  edges and edge stubs clobber it.
- GPR destinations (reusable_gpr): a core instruction that overwrites Xd
  without reading it, cannot fault and does not branch writes the result
  into the host (r8-r11) that caches Xd's old value, so a guest stays in one
  host and merge points agree more often (a max(x, 3) diamond reloaded x
  at the merge). `TRANSMUTE_FX_CORE=nogprreuse`: a fresh register.
- Victims (alloc/alloc_vec): evict_cost = store (dirty) + reload (read again
  in the block, or after it within 64 instructions), then next-use distance,
  then LRU. Vector LDP / LD1 multi-register loads write unpinned destinations
  directly (EmitContext::vec_write_deferred) instead of temp + move.

## Faults
- One FaultSite per access group (from mark_may_fault to the next mark or
  the instruction end) with the flags location at that point. Loads write a
  pinned destination only from the last faulting access (LDP loads the first
  value into a scratch).
- FaultSite::map: the dirty cache entries at mark_may_fault (deduplicated
  against the previous map). Storing or dropping a dirty entry while a site
  is open splits the site (fault_reopen) so the map stays exact.
- Loads with writeback where Rt == Rn keep the writeback (reference
  interpreter behaviour; architecturally CONSTRAINED UNPREDICTABLE).

## Families
- Lookahead stops before the next leader (branch target or entry). Words
  planned as interpret exits make pc+4 an entry, so families must describe()
  every word of a fused sequence.
- A family accepting a word that was planned as an interpret exit (no
  describe) is entered correctly but its fall-through goes through the
  continuation entry stub (flush + reload), since the dataflow did not see
  that edge.
- Emitter limitations (scratch exhaustion, family writes outside its
  describe()) throw RetryTranslation; translate_region retries without extra
  pins, then with conservative usage / an interpret exit for that pc.

## Fusions
- MOVZ/MOVN + following MOVKs of the same register: one mov_imm.
- ADRP xd + ADD xd, xd, #lo12: one relocatable mov_imm64 (fixup value =
  page + lo12).

## Integration hooks
- EmitContext::guest_address() records a guest_address_imm64 fixup.
  temp_gpr_prefer/load_gpr/store_gpr avoid scratch for fixed registers.
- neon_begin_region()/neon_end_region() (per-region constant pool) are
  called around each attempt; they are weak references (/alternatename) so
  fx_translator links without fx_emit_neon (core::neon_region_hooks_linked()).
- CodeSource::has_entry(pc) stops region growth at pcs another region
  already covers (AOT); the region start is never cut.
- translator_family_fingerprint() hashes the registered family names (for
  cache keys).
- TRANSMUTE_FX_CORE=nopins,novecpins,noextrapins disables pinning features
  (debugging; read once).
- Instruction selection (core_isel.hpp; guest-level block analyses, exact):
  - Address folding: ADD/SUB #imm, ADD LSL #0..3, ADD UXTW/SXTW/UXTX/SXTX
    #0..3 feeding only the adjacent LDR/STR/LDP/STP base become one access
    when Xd is rewritten by the load or overwritten later in the block before
    any read, fault or exit. The folded access's fault site reports the ADD's
    pc with Xd unchanged: resuming there re-executes both (exact).
  - Extension facts: UXTW/SXTW register offsets, extended ADD/SUB and
    SMADDL/UMADDL operands skip the mov r32/movsxd when the block's last
    writer already zero/sign-extended the value (W ops, narrow loads, LDRSW,
    SXT*/UBFX/LSR, AND masks, constants); MUL drops the self mov r32.
  - MOVK of a known constant (MOVZ/MOVN/ORR earlier in the block) is one mov.
  - Cached guest registers serve as direct sources of lea/movsx/rorx forms;
    LSL #1..3 into another register is lea; UBFX/SBFX of 8/16/32 bits is
    rorx + movzx/movsx; CMN #imm is CMP #-imm; CSINV/CSNEG of XZR are
    constants; LDP with a pinned first destination loads the second word first.
  - Knobs: TRANSMUTE_FX_CORE=nofold, nozext, nosrcreg, nomovk, or oldisel
    (all four) restore the previous lowering (codegen_knob_bits() << 16).

## Tier 2 (optimizing tier; region_alloc.cpp; on by default)
- Plan::optimize (eager translations with tier 2 on: tier-ups, AOT, notier).
  `TRANSMUTE_FX_CORE=notier2` turns the tier off; `t2no<x>` one feature;
  `t2proto` all of them (the prototype's code generation and tier-up).
  `codegen_tier2_bits()` folds them into the fingerprint.
- Tier-up: a first-tier (lazy) region with loops is tier 0; every loop
  header with an interrupt check does `dec dword [rip + slot]; jz`, one
  counter per region (16 x TranslateOptions::tier_threshold iterations) in a
  64-byte line of its own after the code (no code shares it, so its stores
  never invalidate fetched code; the runtime does not repoint it). Its stub
  exits like an interrupt with ExitReason::tier_up_loop, exit_arg = region
  start (fixup), pc = header; the runtime retranslates the region start and
  re-dispatches the header (a region of its own if the loop is still
  running). Regions without loops are final. `t2entries` / optimize 3:
  entry counting (prototype); `t2loops` / optimize 2: one per-thread counter.
- compute_tier2(): loop depth (layout ranges), dropped interrupt checks (a
  header H keeps none when another header H2, which then keeps its check, is
  on every path from H to each back edge into H: every cycle through H passes
  H2), and liveness to a fixed point: `rlive` (read before written, exits
  read everything) and `olive` (observed: also every may-fault instruction,
  conservative/interpret instruction, kept interrupt check and an ADD/SUB
  right before a faulting access, which a folded fault reports with the
  ADD's state). A dirty cache entry whose guest is not observed from the
  current point (olive_in inside an instruction, olive_out after it) is
  dropped, not stored: evictions, reconcile (dead at the target, and with
  both_paths at the fall-through too), flush_dirty_except.
- Two passes (Plan::t2_alloc): pass 1 records the cache at every internal
  edge into a block (record_edges, before stores); build_presets() gives
  each loop header (and blocks reached only by back edges) the entry state
  of its incoming edges weighted 8^depth (majority per register and host),
  filtered like tier 1's header rule (read in the header's block before
  written), at most all but two cache hosts, dirty where dirty on most of
  that weight. Pass 2 starts those blocks with it; dirty values stay in
  registers only where the innermost loop's header state keeps them dirty
  in the same host (carried_dirty: back edges and branches inside the
  loop), are stored on edges out of the loop (edge stubs for jcc, the
  fall-through path) and everywhere else as with clean carrying
  (flush_dirty_except). Forward jcc into a decided state reconcile in an
  edge stub. Presets for every merge point, header states holding every
  live register, and carrying all dirty values all measured worse
  (docs/FX_CLASSROOM_PROFILE.md).
- Pin weights: 8^level (tier 1: 1 + 8 * level) for uses, entries and exits:
  an inner loop's registers outrank the enclosing loop's and evict
  convention registers it never uses.
- GPR copy aliases (galias_*; t2nogalias): MOV Xd, Xn between unpinned
  registers with Xn cached emits nothing; resolved before control flow,
  block ends, families, interpreted instructions and anything writing Xd,
  Xn's owner (also in the next three words: core fusions consume them);
  eviction stores dirty aliases; fault maps list them.
- Derived vector values (t2noderived; TRANSMUTE_FX_NEON=notblderived):
  families ask for a pure transform of a vector register (FamilyFactWant::
  K::derived; TBL's adjusted indices). When every reaching definition is
  known and less deeply nested than the use, the transform is computed into
  GuestContext::vspill right after each definition (emit_fact_checks,
  FamilyFacts::derive) and the use reads it as a memory operand
  (EmitContext::vec_derived). Not guest state (fault maps need nothing); a
  state-writing helper in the region retranslates without facts.
- Pin lending (opt-in `t2lend`, measured slower): in a loop that never
  touches a pinned register (innermost first, no loop crossing the range),
  that pin's host serves the cache; lend_enter/lend_leave on every edge
  whose lent sets differ (edge stubs for jcc), lent convention registers go
  to the context like evicted ones (entry_pins/exit_pins), fault maps name
  lent registers kFaultMapContext, helpers skip them.
- Tools: core_image_audit writes r_<rva>.map (instruction_map_sink: pc,
  block label, own code, leader); tools/fx_loop_count.py (host instructions
  and context operands per iteration of a guest loop path) and
  tools/fx_tier2_estimate.py (profile-weighted dynamic instruction estimate
  of other settings). Traces: TRANSMUTE_FX_TRACE_PRESETS, _TRACE_LEND,
  _TRACE_PINS=<region start>.
- Tests: core_tier2_tests.cpp (random loop nests vs the interpreter in four
  configurations, guest state at faults in the inner loop, tier-up from
  loops, interrupt checks).
