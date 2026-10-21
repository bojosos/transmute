# Brief: Chrome stage 3, V8 with its JIT on

The rules for running this wave are in
[ALCHEMY_WAVE_RULES.md](ALCHEMY_WAVE_RULES.md). For background, see
[ALCHEMY_CHROME_AUDIT.md](ALCHEMY_CHROME_AUDIT.md) ("JIT analysis") and
[ALCHEMY_FX.md](ALCHEMY_FX.md).

* **When:** after the provider refactor. This wave works almost entirely
  in the fx engine and libSystem VM, so it can run **in parallel with
  stage 2**.
* **Coordination with stage 2:** it shares only `darwin/libsystem/vm/`,
  so give that area one owner.
* **fx seams:** keep the refactor's fx host seams (`src/fx/host/`), and
  add new host-dependent fx code behind them.

## Goal

Chrome strict headless `--dump-dom` without `--js-flags=--jitless` exits
0 on two pages:

* the wave 1 probe page;
* a JavaScript-heavy local page: a loop-heavy benchmark in the page, plus
  WebAssembly instantiation from an inline module.

The wave also measures:

* wall time and SMC fault counts against `--jitless`;
* a small set of numbers from a local JS benchmark page, with the
  results printed into the DOM.

## What is known

* **Memory works.**
  * Wave 1's `DarwinVmSpace` gives V8 what its code range and cage need:
    advisory hints, `MAP_FIXED` inside reservations, partial
    munmap/mprotect, reservations over 1 TiB, and `MAP_JIT` accepted.
  * Rewriting code is coherent through fx's SMC write-protect faults
    (`smc_handle_write_fault`).
  * `pthread_jit_write_protect_np` changes nothing, which is correct here.
  * RWX is allowed.
* **Open items:**
  * `sys_icache_invalidate` calls `invalidate_engine_code`, which does
    nothing, because FxEngine has no `invalidate_code(address, size)`
    (`docs/ALCHEMY_FX.md` around line 149).
  * `register_guest_code_memory` adds an entry on every `mprotect` to
    executable and never merges them (c1's note).
  * Cost: every patch to a translated code page costs a VEH fault and a
    page invalidation. That covers inline-cache updates, code-space
    compaction and lazy deoptimization.
  * Unmeasured: the SMC fault rate under V8, and whether V8 makes
    write-through-alias mappings that fx doesn't know about. V8 on macOS
    may dual-map its code space with `mach_vm_remap` *(design, check it)*.

## Groups (suggested)

* **j1 fx invalidation:**
  * `FxEngine::invalidate_code(address, size)`, called from
    `sys_icache_invalidate`;
  * finer than page granularity, if the fault rate demands it;
  * merging guest-code registrations;
  * fault-rate and translation-cache counters through `ALCHEMY_PERF`.
  * Owns `src/fx/` invalidation and SMC code.
* **j2 VM under V8:**
  * Check the V8 code range and cage under the JIT.
  * Check `mach_vm_remap` / dual mapping of code and its alias
    protection.
  * Check W^X toggling patterns, and `MAP_JIT` with
    `pthread_jit_write_protect_np` if Chrome takes that path. It does
    that when it holds the `allow-jit` entitlement; Alchemy reports no
    entitlements today.
  * Owns `darwin/libsystem/vm/`.
* **j3 measurement and Wasm:**
  * Build the local benchmark and WebAssembly pages.
  * Run the matrix (jitless vs JIT, with the fault counts).
  * Profile hot spots and chase the stops in the Chrome run.

## Done when

* The goal's two pages dump correctly, and exit 0 in 3 of 3 runs.
* A performance table is in `ALCHEMY_CHROME_AUDIT.md`: jitless vs JIT
  wall time, SMC faults per second and translation invalidations.
* Unit tests cover `invalidate_code` and the dual-mapping cases, using
  guest fixtures like `fx_vm_reserve`.
* The regression check before landing passes, including Blender's
  performance numbers, which must be unchanged.

## Not in scope

* SwiftShader. It uses the same path, but it belongs to stage 4.
