# Brief: refactor wave group r4 fx (engine host seams, performance gate)

This brief is the integrator's, for the r4 session of the refactor wave
(docs/ALCHEMY_BRIEF_REFACTOR.md).

You are group **r4 fx** of the provider refactor wave in C:\dev\transmute.
The integrator is the "Refactor integrator" session (the refactor wave's
integrator). The user authorized this wave and is away.

## Read first, completely

1. `docs/ALCHEMY_BRIEF_REFACTOR.md`, especially "fx host seams", "Phases
   and groups" (r4) and the last section, "Integrator's working rules". The
   working rules are binding for builds, locks and reports.
2. `docs/ALCHEMY_WAVE_RULES.md`.
3. The shared notes file
   `refactor-groups.md` in the wave's notes folder (outside the repository):
   setup, build commands, the status log, and the rule never to build the
   `*_runtime` targets.
4. For context only, the provider-side host layer (not yours): the branch
   `integ/refactor-host`, with `include/alchemy/host/`.

## Area

* `src/fx/` only (abi, cache, common, loader, runtime, seh, translator),
  and the fx side of `src/alchemy/fx/` (the engine adapter).
* About 50 non-test fx files include `windows.h`.

## Worktree

```text
git -C C:\dev\transmute worktree add .claude/worktrees/refactor-r4 -b mac/refactor-r4 integ/refactor
```

integ/refactor is at 60354e0 or later.

## Task

1. **Inventory.** List every host dependency of fx. Write it to
   `docs/FX_HOST_SEAMS.md`, one row per seam: what it is, where it lives
   (file:line), the Windows API, the Linux counterpart, and whether it is on
   a hot path. At least:
   * VEH fault handling;
   * the Win64 calling convention of generated code
     (`jit_x64_windows.cpp`);
   * VirtualAlloc2 and MapViewOfFile3 placeholders and views;
   * thread context and TLS;
   * the code-cache file mapping;
   * unwind and SEH registration;
   * the PE-specific pieces;
   * anything else (timers, threads, events, file I/O).
2. **Seams.** Move each dependency behind an fx-internal seam in
   `src/fx/host/`, for example headers in `src/fx/host/` and
   `src/fx/host/win32/*.cpp`.
   * Select at compile time: inline or free functions, no virtual calls,
     zero cost on hot paths.
   * The goal: outside `src/fx/host/` and the tests, no fx file includes OS
     headers.
   * Where a seam can't be zero-cost in this wave, or moving it is too
     risky (the JIT calling convention, for example), document it precisely
     in FX_HOST_SEAMS.md and leave the code. Correctness and speed come
     before the port.
3. **Performance gate.** fx performance must be unchanged.
   * **Before any change,** measure on integ/refactor:
     * the `ALCHEMY_PERF=1` start-up numbers: Blender windowed through
       `scripts/perf/startup.ps1`, cloaked;
     * Blender headless CPU time: `-b --factory-startup` plus a monkey add,
       5 runs interleaved;
     * the FX benchmark that `docs/BENCHMARK_STATUS.md` names as the gate.
   * **After the change,** measure the same, interleaved with a copy of the
     before-binary.
   * The machine is shared and loaded: interleave the runs and report
     medians.
4. **Tests.**
   * The fx test suites.
   * `ctest -R "^alchemy_"`, cloaked, without `ALCHEMY_NETWORK`. The
     integ baseline is 152/152.
   * Two known failures are not regressions:
     `alchemy_gui_windowserver_win32` fails when the workstation is
     locked, and `alchemy_process_tree_fx` "dev (rebased)" is
     intermittent under load.

## Rules

* **Commits:** small and logical, ending with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Moves of whole
  files use `git mv`, in their own commits.
* **Builds:** build `alchemy-fx` and the fx test targets explicitly, at
  `/m:2`, under the build-lock protocol. Never build the `*_runtime`
  targets.
* **Branches:** never push. Never merge into main or integ/refactor.

## Report

Use the wave rules' format. Add the before and after performance numbers
and FX_HOST_SEAMS.md's summary: which seams are done, and which are
documented and left as they are.
