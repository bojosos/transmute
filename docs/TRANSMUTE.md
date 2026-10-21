# Transmute: the ARM64 Windows (PE) runtime

Transmute is an early ARM64-to-x64 Windows application runtime. The eventual goal
is to run ARM64 Windows installers and installed applications on x64 Windows.
Alchemy ([ALCHEMY.md](ALCHEMY.md)) uses the same execution engine for macOS
applications.

The `transmute` executable loads plain ARM64 PE32+ files, applies 64-bit base
relocations, and loads ARM64 DLLs with named and ordinal function and data
exports and guest-DLL forwarded exports. It sets up initial-thread static TLS,
invokes TLS and process-attach callbacks, and executes ARM64 code through an
in-house reference interpreter, or through the fx engine ([FX_ENGINE.md](FX_ENGINE.md)).
Its Win32/CRT bridge covers selected file, environment, heap, locale,
synchronization, and process APIs. It is not yet an installer or a general
Windows application compatibility layer. Unsupported instructions and APIs stop
with a diagnostic instead of silently continuing.

## Build and test with the Visual Studio generator

[ALCHEMY_BUILD.md](ALCHEMY_BUILD.md) describes the Ninja build with the shared
compiler cache. The plain Visual Studio build also works:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --parallel 4 --output-on-failure
```

When Visual Studio's ARM64 clang-cl and lld-link are installed, the build also
compiles genuine ARM64 Windows executables and DLLs, including one executable
that calls a native Win32 API and another that calls through two guest DLLs,
then runs them in the test suite.

The core suite runs in four separate processes so tests that change Windows or
CRT process state cannot interfere with one another. Every test reports its name
before running, and each group has a two-minute timeout. Configure
`-DTRANSMUTE_TEST_SHARDS=8` for eight groups, or `=1` for one group.
`transmute_tests --list-tests` lists available tests; `--test <name>` runs one,
and `--shard <index> <count>` runs one group. Pass the usual compiled guest
fixture paths after these options to include loader integration tests. Keep
performance measurements separate from parallel correctness tests.

## Run and inspect

* `build\Release\transmute.exe doctor program_arm64.exe` inspects a PE image.
* `build\Release\transmute.exe program_arm64.exe` executes a supported program
  and returns the guest's 32-bit exit code.
* `transmute doctor --load-base 0x180000000 program_arm64.exe` checks whether
  the image can run at a different address and displays the selected base.
* The execution command accepts the same `--load-base` option, and
  `--dll-dir <directory>` for additional ARM64 DLL search directories.
* `--instruction-limit <count>` raises the interpreter's per-call instruction
  budget above its ten-million default during long startup probes.
* `transmute doctor --deps --dll-dir <directory> program_arm64.exe` prints the
  guest address range and source path of every loaded PE module. This helps map
  a guest PC or return address to its DLL.
* `--jit` enables the first in-house x64 block translator for supported integer
  blocks. It falls back to the reference interpreter and is experimental; the
  default is interpreter execution. The production engine is fx
  ([FX_ENGINE.md](FX_ENGINE.md)).

## Embree probe

With the official ARM64 Blender files staged locally,
`scripts\probe_embree.ps1 -Stage <Blender 4.5 directory> -Phase triangle -RayQuery -BlenderGeometry -Instance`
runs the packaged ARM64 `embree4.dll` through Transmute. It is a focused library
test and does not substitute for a Cycles render.

* The probe creates a triangle scene with Blender's geometry-buffer API and checks
  scalar and 4/8/16-lane ray hit/miss, occlusion, and clear-path results,
  including the traversable calls Blender imports.
* `-Instance` also verifies a translated instance scene and its hit ID.
* `-PacketWidth 1`, `4`, `8`, or `16` narrows the ray-query surface when
  diagnosing a failure.
* `-GridSize 32 -Callbacks` builds a larger triangle mesh and verifies a
  scene-build progress callback.
* `-Phase triangle -RayQuery -Instance -ExtendedApis` checks the remaining Embree
  functions Blender imports: device callbacks, shared geometry storage, curve
  tessellation, and quaternion instance transforms. This mode omits
  `-BlenderGeometry` because it tests a distinct shared-buffer path.
* `-DynamicUpdate`, with either triangle ray-query mode, moves an attached mesh
  after its first commit, rebuilds the scene, and verifies the new hit and
  occlusion results. It also checks that disabled packet lanes stay intact.
* `-ArgumentFilter` checks a guest filter callback that rejects an intersection
  and an occlusion through Embree 4's ray-query arguments.
* `-Native -Stage <native x64 Blender 4.5 directory>` with the same options runs
  the equivalent fixture against the installed native Embree DLL.

## Blender startup probe

`scripts\probe_blender.ps1 -Stage <Blender 4.5 directory> -Scene <classroom.blend>`
runs the staged Classroom startup. It reports the actual Transmute exit code and
wall time even when startup fails, and saves the console output under `artifacts/`.

* `-TinyScene` instead of `-Scene` renders the factory Cube at 16 x 16, one sample
  and one CPU thread. This isolates Cycles startup from Classroom's linked assets
  and object synchronization.
* `-JitStats -DiagnoseSync` reports translation statistics and pending guest
  waits. The wait monitor also reports stored guest worker errors while the main
  thread is blocked.
* Successful probes verify a fresh PNG and save its dimensions, hashes, settings
  and process wall time beside the console log.

## Related documents

* [Blender target](BLENDER_TARGET.md): the first full installer and render workload.
* [Benchmark status](BENCHMARK_STATUS.md): the native x64 result and the ARM64
  scene-loading boundary.
* [Compatibility audit](COMPATIBILITY_AUDIT.md): the staged ARM64 DLLs and
  prioritized gaps.
* [Architecture](ARCHITECTURE.md): the in-house translator boundary and the macOS
  personality.
* [Roadmap](../ROADMAP.md): milestones. The reference interpreter remains an
  independent correctness oracle as faster engines are added.
