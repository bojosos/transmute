# Documentation

## Start here

* [ALCHEMY_BUILD.md](ALCHEMY_BUILD.md): building with Ninja and the shared compiler cache.
* [TRANSMUTE.md](TRANSMUTE.md): the ARM64 Windows (PE) runtime, its command line and probes.
* [ALCHEMY.md](ALCHEMY.md): the macOS runtime's command line, supported profile and provenance.
* [ARCHITECTURE.md](ARCHITECTURE.md): translator and operating-system boundaries.
* [ALCHEMY_PROVIDERS.md](ALCHEMY_PROVIDERS.md): how the macOS layer is organized: one directory per
  framework under `src/alchemy/darwin/`, each with a README saying what is real and what is unavailable.
* [../ROADMAP.md](../ROADMAP.md) and [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## The fx engine

* [FX_ENGINE.md](FX_ENGINE.md): design and invariants.
* [FX_HOST_SEAMS.md](FX_HOST_SEAMS.md), [FX_FOLLOWUPS.md](FX_FOLLOWUPS.md).
* [ALCHEMY_FX.md](ALCHEMY_FX.md): Alchemy on fx.
* [ALCHEMY_MEMORY.md](ALCHEMY_MEMORY.md): shared image pages and the translation cache.
* Performance: [FX_CLASSROOM_PROFILE.md](FX_CLASSROOM_PROFILE.md), [ALCHEMY_PERF.md](ALCHEMY_PERF.md),
  [PERFORMANCE_IMPLEMENTATION_RESULTS.md](PERFORMANCE_IMPLEMENTATION_RESULTS.md),
  [PERFORMANCE_NEXT_STEPS.md](PERFORMANCE_NEXT_STEPS.md).

## Alchemy subsystems

* [ALCHEMY_OBJC_CF.md](ALCHEMY_OBJC_CF.md): Objective-C and CoreFoundation.
* [ALCHEMY_QUARTZ2D.md](ALCHEMY_QUARTZ2D.md): Quartz 2D, CoreText and ImageIO.
* [../src/alchemy/darwin/msl/README.md](../src/alchemy/darwin/msl/README.md): the MSL-to-SPIR-V compiler.
* [ALCHEMY_NETSEC.md](ALCHEMY_NETSEC.md): network and security.
* [ALCHEMY_PROCESS.md](ALCHEMY_PROCESS.md): child processes.
* [ALCHEMY_SWIFT.md](ALCHEMY_SWIFT.md) and [../runtime/README.md](../runtime/README.md): guest Swift and LLVM runtimes.
* [ALCHEMY_EXPANSION.md](ALCHEMY_EXPANSION.md), [ALCHEMY_ENGINE_REQUIREMENTS.md](ALCHEMY_ENGINE_REQUIREMENTS.md).
* Plans: [ALCHEMY_HOST_LINUX.md](ALCHEMY_HOST_LINUX.md), [ALCHEMY_TOOLCHAIN_PLAN.md](ALCHEMY_TOOLCHAIN_PLAN.md).

## Applications

* Chrome: [ALCHEMY_CHROME_AUDIT.md](ALCHEMY_CHROME_AUDIT.md) (status, real web, performance),
  [ALCHEMY_CHROME_GAPS.md](ALCHEMY_CHROME_GAPS.md), [ALCHEMY_CHROME_GAPS_SYMBOLS.md](ALCHEMY_CHROME_GAPS_SYMBOLS.md).
* Blender: [BLENDER_TARGET.md](BLENDER_TARGET.md), [BLENDER_GUI.md](BLENDER_GUI.md),
  [BENCHMARK_STATUS.md](BENCHMARK_STATUS.md), [COMPATIBILITY_AUDIT.md](COMPATIBILITY_AUDIT.md),
  [ALCHEMY_BLENDER_AUDIT.md](ALCHEMY_BLENDER_AUDIT.md).
* Python: [ALCHEMY_PYTHON.md](ALCHEMY_PYTHON.md), [ALCHEMY_PYTHON_AUDIT.md](ALCHEMY_PYTHON_AUDIT.md),
  [ALCHEMY_PYTHON_FOLLOWUP.md](ALCHEMY_PYTHON_FOLLOWUP.md).
* QLab: [ALCHEMY_QLAB_AUDIT.md](ALCHEMY_QLAB_AUDIT.md), [ALCHEMY_QLAB_GAPS.md](ALCHEMY_QLAB_GAPS.md).
* Visual Studio Code (Electron): [ALCHEMY_VSCODE.md](ALCHEMY_VSCODE.md) (folder and editor, ripgrep search,
  integrated terminals over node-pty).

## How the work is organized

[ALCHEMY_WAVE_RULES.md](ALCHEMY_WAVE_RULES.md) and the `ALCHEMY_BRIEF_*.md` briefs record how parallel work
was split and landed. [ALCHEMY_REFACTOR_FOLLOWUPS.md](ALCHEMY_REFACTOR_FOLLOWUPS.md) lists items left for later.

Commit hashes quoted in these documents and in older commit messages (such as `245dedd`) are from the
development history, which is not published: the public history starts from a single squashed commit.
