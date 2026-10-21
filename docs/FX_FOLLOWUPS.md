# fx follow-ups

## In flight (3 October 2026)

- FPSR at host-code boundaries (macOS numpy: 4 failing tests). The engine
  agent hit the usage limit mid-way, at "rebuild, then run the numpy suite".
  Fix contract: ctx.fpsr/ctx.fpcr must be architectural whenever host code
  receives a GuestContext&. Paths: svc exits, register_host_callback, host
  entries, the fault hook, interrupt requests, and the seh context capture.
  Fold the MXCSR flags into ctx.fpsr before each call and reload MXCSR after
  it. Windows check: `tests/fx/bench/np_fpflags.py` already matches native.
- Codex investigation: docs/FX_CLASSROOM_PROFILE.md. VTune measured 2.13x
  retired instructions and 1.44x cycles against native. Its plan:
  1. Embree movemask fusion: done (ABI 19). The shift/mask vectors are
     proven by vector value facts (MOVI, compare lane masks, or a flag
     checked once after each defining load); the USHR #31 form is fused too.
     `mask_reduce` 3.41 -> 1.89-2.51 x native (thread cycles, noisy laptop;
     paired 0.59-0.85 of Codex's inline-guard build, which gave 2.71). The rest of that loop (interrupt check, Q-index address
     arithmetic, the ADDV scalar result write) keeps it near 2x: native is
     unrolled by two. Results: docs/FX_CLASSROOM_PROFILE.md.
  2. Optimizing tier: done, on by default (ABI 21; `notier2` turns it
     off). Tier-up counts loop iterations per region (regions without
     loops are final; Classroom 10%: 418 tier-ups, 129 ms JIT). Tier-2
     code: covered interrupt checks dropped, two-pass loop-header states
     with carried dirty values, liveness-based dead stores, GPR copy
     aliases, TBL indices precomputed outside loops, 8^depth pin weights.
     noise_fbm loop 257 -> 218 host instructions (context 34+18 -> 19+10),
     Embree node loops 86 -> 82 and 85 -> 82; profile-weighted estimate
     over Classroom's hot regions: plain needs 4.1% more instructions.
     Pattern kernels: texture 0.91-0.92x, the rest within placement noise.
     Remaining: edge frequencies (a tier-0 edge profile) would let entry
     states be chosen at every merge point (static weights made it worse);
     Embree's loop-invariant offsets need registers the region does not
     have (pin lending, `t2lend`, measured slower); exact FMAX/FMIN cost 24
     of Embree's 82 instructions; disp32 V-register operands (no free GPR
     for a second base in the hot regions). Timing under a quiet machine
     (pattern bench same-binary A/B: `-BaselineCore notier2`; pybench).
     Results: docs/FX_CLASSROOM_PROFILE.md section 4.
  3. A smaller hot-code footprint: done (ABI 20, cache format 5). Rare paths
     (exit tails, RSB-miss lookups, switch fallbacks, interpret/trap exits)
     in a cold section per region, continuation entry stubs in line, rel8
     back edges, movups context moves, fewer alignment NOPs, and regions
     that cover whole .pdata functions (up to 16384 instructions): 17-21% of
     the hot guest code was translated twice. Classroom 10%, regions at 90%:
     expansion 4.57 -> 3.77 (body 3.15), duplicated guest code 58 KB -> 7 KB;
     blender.exe static code -3.7%, embree4.dll -4.0%. Knobs `oldlayout`,
     `oldcap`, `coldentries`, `loopalign`. Results and what was not done:
     docs/FX_CLASSROOM_PROFILE.md. Remaining: forward branch relaxation
     (~0.9% of hot bodies), disp32 vector context operands (~8%: needs a
     second base register or a context layout change), hot-region placement
     once tier 2 is on (the only hotness signal at install time).
- Measuring: other agents' and Codex's renders share this laptop. Use
  `scripts/fx_classroom_pair.ps1` / `fx_pybench_pair.ps1` (simultaneous
  pairs) or `fx_classroom_study.ps1` / `fx_pattern_bench.ps1` (Codex). Never
  trust a single unpaired wall time.

Queued work that is not owned by a running agent yet. Newest requests at the
bottom of each section.

## Correctness

- Intel OpenGL driver crash: `igxelpicd64.dll` null+0x20 on a driver worker
  thread, OpenGL + viewport Cycles only, about 1 in 7 runs. fx-specific: native
  is 10/10 clean. No guest frames on the faulting thread. Vulkan is clean and is
  the GUI default.
- Host code called through a plain import thunk runs on the guest MXCSR. If
  it calls back into guest code (host_call_bridge, e.g. a qsort comparator),
  the nested region entry loads MXCSR from `ctx.fpsr`, which lacks the flags
  raised since the last region entry: the callback's MRS FPSR misses them and
  its feclearexcept cannot clear them (invoke restores the old MXCSR after).
  Folding there needs to know whether MXCSR is the guest's (thunk) or the
  host's (host thread), e.g. from the bridge's caller.
- Guest-visible ARM64 identity covers the environment (`PROCESSOR_*`),
  GetSystemInfo/GetNativeSystemInfo, IsWow64Process2 and
  GetMachineTypeAttributes. The registry (`CentralProcessor\0`), WMI and
  `NtQuerySystemInformation(SystemProcessorInformation)` still describe the
  x64 host. x64 children started through CreateProcessW/A get the host's
  `PROCESSOR_*` values back. Children started by host code, such as the UCRT's
  `system()`/`_spawn*` or ShellExecute, or through CreateProcessAsUser, inherit
  `PROCESSOR_ARCHITECTURE=ARM64`.
- IsProcessorFeaturePresent leaves JSCVT, FP16/FHM, I8MM, SHA3 and SHA512
  unreported. The translator has emitters for them, but the interpreter has no
  fallback, and FP16 needs host F16C. Report them once the translator guarantees
  them: numpy and OIDN choose code paths from these bits.

- The inlining agent tried skipping NZCV materialization at BL/RET when the
  flags are dead (the `abi_flags` model in FX_ENGINE.md). In 3 of 6 runs,
  8-thread Classroom then differed by one pixel. It reverted that, so the code
  still materializes there. Native Cycles also shows a one-pixel
  nondeterminism about once in 29 runs, so this may be noise. Check before
  re-enabling: same-exe A/B over ~30 runs each, and confirm what
  `abi_flags` currently elides.

## Performance

- Cold-cache throughput (measured 2 October 2026, 15 W i7-1355U).
  - Headless EEVEE (64x64, 1 sample): the first run took 43 s, but JIT was
    only 0.8 s of it; the GL driver's own shader compile is the rest (native
    pays it too). The 161 s GUI case has not been reproduced headless. It
    probably adds Blender's shader-compilation subprocesses, each a cold fx
    process.
  - The JIT cost about 350 ns per guest instruction (`TRANSMUTE_FX_STATS`
    `aot:` line). It now costs about 250 ns: getenv was called on every
    translation, emission was byte-wise, and plan dataflow passes were
    repeated. The generated code is unchanged (hash from
    `TRANSMUTE_FX_JIT_BENCH`, docs/FX_ENGINE.md). A Blender render translates
    2–2.5M guest instructions, about 0.65 s.
  - Remaining hot spots (`TRANSMUTE_FX_JIT_BENCH_SAMPLE`): `core::build_plan`
    (~20%, mostly discovery), `RegionEmitter::evict_cost` (~6%, the Belady
    lookahead), `modrm_mem`/`decode`. There are also about 6 us of fixed
    cost per region (install, cache store, dispatcher round trip) for about
    40k regions per render.
  - Tier 0 does not shrink Windows JIT work (docs/FX_ENGINE.md, Region
    formation and tiers). A cut that does not re-cover shared code would need
    entries at join points, or side exits translated only up to the next join.
  - Background AOT (cache `background.cpp`) is in place but helps little for
    renders: Classroom after a tiny render JITs 1023 instead of 4056 regions,
    within run-to-run noise. It is delayed by 20 s so short runs never pay for
    it. A lookup that hits a cold record costs about 21 µs, mostly page faults
    on the mapped `.fxa`.
- Done: tier-0 region size on huge cold binaries (Alchemy request). Code
  without .pdata now gets synthetic function bounds. macOS Blender `--version`
  translates 10952 regions holding 0.88M guest instructions (was 14516 regions
  and 14.1M): translate 0.45 s instead of 12.7 s, arena 16 MB instead of 225 MB,
  wall 5.5 s instead of 17.7 s. Tier-up infrastructure (entry counters,
  replacement, tier-1 cache records) is in place, behind
  `TRANSMUTE_FX_TIER_*` knobs.
- Alchemy JIT: `LiveCodeSource` called VirtualQuery for every new code page
  outside PE text. In alchemy-fx that took ~150 us per call (large
  reservations), ~2 s per start. It is now cached per thread and invalidation
  epoch.
- First Vulkan start compiles shaders through the translated
  `shaderc_shared.dll` and takes tens of seconds.
- CPU-bound GUI workload for profiling: sculpting a 31k-face mesh on Vulkan
  runs at 5.6 fps, against 7.8 fps native.
- "Dead upper lanes" query for vector reductions (ADDV etc.), so they can drop
  the remaining `insertps`.
- `transmute-fx --aot <image>` still writes the main file, which the preloader
  walks in full at every start: for blender.exe that is 254k regions (~450
  MiB of code into the 1 GiB arena). `--aot-background` writes the cold
  `.fxa` instead; consider making that the default for `--aot`.

## Alchemy / macOS personality (requests from the Alchemy session)

- `alias_guest_memory(dst, src, size, protect)` / `unalias_guest_memory`:
  shared-memory aliasing for `vm_remap` with writable sources. It needs
  section-backed guest regions (CreateFileMapping + MapViewOfFile3 into
  placeholders). Darwin uses 16 KiB pages and Windows views use 64 KiB. Read-only
  trampoline pages (libffi) are handled in the Alchemy shim by copy +
  read-only + `register_guest_code_memory`.
