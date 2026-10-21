# Alchemy toolchain plan (2026-10-14): a macOS toolchain under Alchemy, then self-hosting

The user's direction:
1. First, run the normal macOS LLVM/clang through Transmute/Alchemy, compile small C++ programs with it, and run those programs again through Alchemy.
2. Then ship a clang and a basic set of tools, the way macOS ships them.
3. Only after that, compile full LLVM with the shipped clang.
4. Later, possibly build bigger things such as Blender from source.

Building for macOS (not just cross-compiling to Windows) is the goal. It forces a real "Alchemy SDK". All work stays project-authored with recorded provenance: no Apple SDK, Xcode or Apple binaries.

## T0: Inputs (needs user approval before any download)
- **Official LLVM 19.1.5 macOS ARM64 release tarball** (`LLVM-19.1.5-macOS-ARM64.tar.xz`, github.com/llvm/llvm-project/releases, Apache-2.0 with the LLVM exception). Pin it and record its SHA-256 in `artifacts/alchemy-llvm-toolchain/provenance.json`.
- **Optional quick track:** the Windows ARM64 release (`LLVM-19.1.5-woa64.exe`), extracted rather than installed. Run it under transmute-fx as a Windows-personality compile benchmark.

## T1: The macOS clang runs under Alchemy and builds small programs
- **Get the tools running:** `alchemy-fx clang --version`, `ld64.lld --version`, `llvm-ar`, `llvm-nm`, `llvm-objdump`. Fix the libSystem, process, file and mmap gaps they hit; run in strict mode, not probe mode.
- **Compile:** use `--target=arm64-apple-macos11 -isysroot <Alchemy SDK>`, where the SDK is `runtime/include` plus link stubs. Compile clang inside the guest and link with the guest `ld64.lld`. The clang driver spawns `ld` through our posix_spawn path.
- **Corpus:** hello world (C and C++); STL-heavy code; exceptions across dylibs; threads and atomics; `std::filesystem`; iostreams/locale; ObjC + Foundation; the existing `tests/fixtures/macos/*` sources; a couple of small open-source CLIs (e.g. `zlib` examples, `lua`, `sqlite3` shell).
- **Run** every output under alchemy-fx and alchemy (interpreter/JIT, two load bases).
- **Differential check:** compile the same sources with VS's native clang 19.1.5 and with the translated macOS clang 19.1.5, using deterministic flags (`-fno-ident`, `-ffile-prefix-map`, no timestamps). The **objects must be byte-identical**. Any diff is a translator, libSystem or runtime bug, localized to a function.
- **Optional Transmute variant:** the Windows ARM64 clang-cl under transmute-fx builds the same corpus for x64 Windows, with the same byte-identical check.
- **Measure:** compile time versus native clang, per file and per corpus.

## T2: The Alchemy SDK
- **Headers:** grow `runtime/include` into a coherent macOS SDK profile: POSIX, BSD, `mach/*`, `sys/*`, `dispatch`, `os/*`, `CoreFoundation`, `Foundation`, `objc`, plus the AppKit/Metal/QuartzCore declarations we already have. Write everything from public documentation and man pages.
- **Header tests** compare struct sizes, offsets and constants against what the providers implement. The 2026-10-13 `wint_t` mismatch is the kind of bug these catch.
- **Link stubs:** generate `.tbd` text stubs automatically from Alchemy's provider export tables (`ProcessContext` exports, ObjC classes) and from the runtime-library identities. The SDK then always matches exactly what Alchemy implements, and a missing symbol fails at link time instead of at load time.
- **Layout and wrappers:** `SDKs/Alchemy.sdk` with `SDKSettings.json`, an `xcrun`/`xcode-select`-style shim, and `alchemy cc` / `alchemy c++` wrappers. Building against the SDK works both from Windows (VS clang) and inside the guest.

## T3: Ship a base system with the runtime ("what macOS ships")
- **Toolchain:** `/usr/bin/clang`, `clang++`, `cc`, `c++`, `ld`, `ar`, `ranlib`, `nm`, `strip`, `otool`-equivalent (`llvm-objdump`), `lipo`-equivalent (`llvm-lipo`), `install_name_tool`-equivalent (`llvm-install-name-tool`), `dsymutil`. These are the official LLVM release binaries (redistributable under Apache-2.0); later we can rebuild them ourselves (T4).
- **Base tools:** `/bin/sh` (graduate from the built-in shell subset to a real permissively licensed shell built with our clang, e.g. a BSD-licensed sh), `make` (a permissively licensed implementation), and core utilities (`cp`, `mv`, `rm`, `mkdir`, `cat`, `sed`, `grep`, `find`, `xargs`, `tar` from permissively licensed sources). Check every license before shipping. Prefer BSD, MIT or Apache; avoid GPL in the shipped base unless it's clearly separated.
- **Packaging:** installed into the Alchemy root (`/usr/bin`, `/usr/lib`, `/Library/Developer/...`), with provenance per binary and an `alchemy toolchain install` command.
- **Process-spawn performance gate:** builds launch thousands of short processes. Every guest child is currently a new `alchemy-fx.exe` with a full loader start.
  - Measure `posix_spawn` → exit round-trip latency.
  - Add the fast paths needed: a warm translation cache for the toolchain images; preloaded runtime libraries; possibly a zygote/process-reuse server for hot tools like clang and ld.

## T4: Self-hosting LLVM
1. Build LLVM (clang + lld + llvm tools) **for arm64-apple-macos** with the shipped clang under Alchemy, using the shipped `cmake`/`ninja`. Either the official macOS cmake/ninja binaries under Alchemy, or native Windows cmake/ninja with a guest path bridge; decide in T3.
2. **Bootstrap check:** the stage-1 compiler is the official release, translated. The stage-2 compiler is built by stage 1. Stage 3 is built by stage 2 under Alchemy. **Stage 2 and stage 3 objects must be identical.**
3. Rebuild the runtime libraries (libc++, libc++abi, libunwind, compiler-rt) with our own stage-2 clang, and swap them in through the provenance-gated runtime identities.
4. Benchmark: full-build wall time versus native Windows clang building the same tree.

## T5: Build real applications from source
- **Candidates:** small, then medium, then large. CLIs first (sqlite, lua, zstd, cmake itself), then a GUI ObjC/AppKit sample app, then **Blender for macOS from source**.
- **What Blender needs:** its macOS dependency libraries (built from source with our toolchain, or Blender's prebuilt macOS libs where licensing allows), plus an SDK covering the AppKit, Metal, QuartzCore and CoreVideo headers it compiles against. Our headers only declare what Alchemy implements, so gaps surface at compile and link time.
- **Gate:** the source-built Blender runs under Alchemy like the official binary (M4 scenarios, the headless Cycles regression).

## Risks and notes
- **Header ABI fidelity:** our headers must match real macOS layouts exactly; otherwise binaries would work under Alchemy but not on a real Mac. Test layouts against public ABI documentation and the official binaries' expectations, e.g. imported symbol signatures and struct sizes used by the official LLVM build.
- **Licensing:** what we ship (LLVM binaries, shells, tools) must be redistributable. Track provenance and licenses per file, like the expat, LLVM runtime and robin-map entries.
- **Ordering:** T1 → T2 → T3 → T4 → T5. Each phase gates the next. The optional Windows-ARM64 Transmute compile benchmark can run in parallel with T1.

## Cross-cutting requirement (user, 2026-10-14): outputs must also run on real macOS

Everything our toolchain and SDK produce must be valid ARM64 macOS binaries that run on a real Mac, not only under Alchemy. Consequences, which apply to T1-T5:
- **Exact ABI headers.** SDK headers must match Apple's public ABI exactly: struct layouts, constants, macro values, inline-function behavior, symbol names (including any `$DARWIN_EXTSN`-style variants), and the `TargetConditionals`/`Availability` semantics. "Consistent with our providers" is not enough. Sources remain public documentation and man pages; nothing is copied from Apple's SDK or APSL headers. A header/ABI test suite compiles probes for `sizeof`/`offsetof`/constants. Its expected values come from public specs and are confirmed on a real Mac (see validation below).
- **Link stubs.** `.tbd` stubs generated from our providers must use Apple's real install names and current/compatibility versions. They must only list symbols that actually exist in the macOS release we target. Otherwise a program could link and run under Alchemy but fail in dyld on a real Mac. So generated `.tbd` files are intersected with a verified per-OS-version export list. That list is built by checking symbol presence on a real Mac (`dlsym`/`nm` probes). Apple binaries are never copied into the repo.
- **libc++ policy.** System `/usr/lib/libc++.1.dylib` on a real Mac is Apple's build. Programs compiled against libc++ 19 headers can reference symbols an older macOS libc++ lacks. Default mode: link libc++/libc++abi statically, or bundle our own libc++ under a non-Apple install name via `@rpath`; both work on Mac and under Alchemy. Optional "system libc++" mode: enable libc++ availability markup for the deployment target and validate on a real Mac.
- **Load commands and signing.** Correct `LC_BUILD_VERSION` (platform macOS, minos/sdk), and chained fixups for macOS ≥ 12 targets (classic binds for 11). Mandatory ad-hoc code signing for arm64: LLD's `-adhoc_codesign`, verified by `codesign --verify` on a real Mac. Developer ID signing and notarization for distribution is a later, separate step that needs the user's certificates.
- **Frameworks.** Our AppKit/Metal/Foundation declaration headers may stay subsets. Everything they declare must match Apple's real selectors, signatures, types and enum values.
- **Real-Mac validation gate.** Every phase's test corpus (T1 programs, T2 SDK probes, T3 tools, T4 stage-2/3 compilers, T5 apps) is run both under Alchemy and on a real ARM64 Mac (CI runner or the user's machine over SSH). A phase is done only when both pass and outputs agree. **Open question for the user: which Mac (and macOS version) is available for this.**
