# Transmute and Alchemy

**Transmute** runs ARM64 Windows (PE) programs on x64 Windows. **Alchemy** is a
macOS application compatibility layer on top of it: it runs ARM64 macOS (Mach-O)
applications on x64 Windows, in the spirit of Wine. Alchemy is implemented
independently. It contains no Apple code, and it uses no Apple SDKs or binaries
and no reverse engineering. Its Darwin, Objective-C and framework behavior is
written from public documentation and observed program behavior.

Both run guest code on **fx**, an in-house ARM64-to-x64 translator. fx JITs
blocks into native x64 code, with guest addresses equal to host addresses and
guest threads running as host threads ([FX_ENGINE.md](docs/FX_ENGINE.md)).

## Status

This is early, experimental work. What runs today, on one x64 Windows 11 laptop:

* **Google Chrome 155 for macOS (arm64)**, windowed and multi-process with V8's
  JIT, loads real web sites. 20 public sites are covered, with WebGL on
  SwiftShader ([Chrome audit](docs/ALCHEMY_CHROME_AUDIT.md)).
* **Blender 4.5**, both macOS arm64 (windowed and headless, under Alchemy) and
  Windows ARM64 (GUI, under Transmute).
* **QLab 5** reaches its welcome window, and Blender's bundled **CPython 3.11**
  with NumPy runs scripts.

Speed is well below native. Chrome reaches about 15 % of native on JetStream's
JIT-heavy tests and about 16 % on Speedometer (jitless). Long JIT-heavy pages
still exhaust the fx code arena. Many APIs are partial; unsupported ones stop
with an explicit diagnostic instead of guessing.

## Build

You need Windows 10 or later (x64), Visual Studio 2022 with the C++ x64 tools and
its bundled Clang/LLVM, and CMake 3.24 or newer. The quickstart, the shared
compiler cache and the full option list are in
[docs/ALCHEMY_BUILD.md](docs/ALCHEMY_BUILD.md):

```powershell
. scripts/dev/build_env.ps1
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_CONFIGURATION_TYPES=Release -DTRANSMUTE_WITH_FX=ON
cmake --build build --config Release -- -j 4
ctest --test-dir build -C Release -R "^alchemy_" -j 8 --output-on-failure
```

The guest LLVM, Swift and Expat runtimes and installed application roots live in
the git-ignored `artifacts/` directory. They are built or installed locally by
the scripts in `scripts/`, and the tests that need them are skipped when they are
absent. No application binaries are part of this repository.

## Documentation

* [docs/](docs/README.md): an index of the design notes, audits and measurements.
* [ROADMAP.md](ROADMAP.md): milestones.
* [docs/TRANSMUTE.md](docs/TRANSMUTE.md) and [docs/ALCHEMY.md](docs/ALCHEMY.md):
  using the two runtimes.

## License

MIT; see [LICENSE](LICENSE). The exception is `external/m2v-capi/`, a C ABI over the
metal2vulkan translator that builds a separate, optional DLL: it is LGPL-3.0-or-later (its
own `COPYING.LESSER` and `COPYING`), and Alchemy only loads it at run time. Third-party
components and the tools the build uses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
