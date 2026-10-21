# Guest LLVM runtimes

`scripts/build_alchemy_llvm.ps1 -Fetch -Runtime all` builds LLVM 19.1.5 runtime
sources for ARM64 Mach-O using Clang and Mach-O LLD. The default outputs and
source checkout live under ignored `artifacts/` directories. The script verifies
commit `ab4b5a2db582958af1ee308a790cfdb42bd24720` before building.

The build includes libc++, libc++abi, libunwind and the compiler-rt integer
builtins required by libc++. These are guest ARM64 libraries executed by
Transmute. The ABI library reexports the unwinder, and libc++ reexports the ABI
library and loads compiler builtins. Configure Alchemy's `--runtime-dir` with the output
directory. The runtime loader accepts the approved independently built library
identities; it does not search Apple installations or SDKs.

The authored builds target generic ARMv8-A to keep their instruction requirements
explicit. Stock macOS applications can use newer instructions and need separate
Transmute coverage. The script writes `provenance.json` with the pinned commit,
target flags, compiler/linker versions, authored header hashes, output SHA-256
hashes, and license-file hashes for each completed invocation.

The ARM64 unwinder uses LLVM's five applicable source files. Platform sources
that preprocess to empty translation units are excluded because LLD 19 otherwise
emits duplicate zero entries at their compact-unwind boundaries. Cross-dylib
exception fixtures verify handler discovery, stack cleanup, and guest context
restoration. For diagnosis, `-UnwindTrace` enables LLVM's upstream tracing hooks;
guest environment variables select the traces.

`runtime/include` contains independently authored interface declarations and
build configuration. The headers describe the supported Darwin LP64 profile,
including 64-byte mutexes, 48-byte conditions, 200-byte rwlocks, 128-byte
multibyte state, 32-bit `wchar_t`, and 144-byte `stat`. File streams are opaque
guest objects. These declarations supply no Apple implementations.

The C++ ABI is LLVM's stable version 1 in namespace `__1`. Matching these names
does not prove compatibility with Apple's libc++ variants. Stock applications
still require symbol and object-layout validation against their dependencies.
Unavailable strong imports fail before guest startup. Exceptions must stay in
guest code; unwinding through host service callbacks is unsupported.

The LLVM libraries retain Apache-2.0 with LLVM-exception licensing and other
notices included by their source distributions. The build copies the root and
component license files and `THIRD_PARTY_NOTICES.md` beside its outputs. The
additional Ryu attribution and Boost notice are preserved in that document.
Project-authored declarations,
adapters and fixtures retain the project's MIT license. Do not redistribute
the externally supplied sample or Apple application binaries as fixtures.

## Guest libexpat

`scripts/build_alchemy_expat.ps1 -Fetch` builds Expat 2.8.5 (MIT, tag
`R_2_8_5`, commit `4b3f0b06f39fb5529cead381694f8929901bc273`, verified before
building) into `artifacts/alchemy-expat-arm64/libexpat.1.dylib`. The library
uses the install name `/usr/lib/libexpat.1.dylib`, with compatibility version
7.0.0 and current version 8.0.0. These are the versions macOS applications
record for libexpat.1; a Blender add-on installer requires
compatibility version 7.0.0. The library exports exactly Expat's public API,
the 72 names in upstream's `libexpat.def`, without `XML_GetAttributeInfo`.
That is upstream's default profile: DTD, general entities and namespaces are
on, and attribute offsets are off. Releases of macOS that ship an older Expat
export a subset of these names. The script fails if the export set differs
from the definition file. It also fails if the objects import any libSystem
symbol outside its reviewed list of 18 names. Those names bind two-level to
`/usr/lib/libSystem.B.dylib` through a generated link-only interface, and
Alchemy's libSystem provider implements them. Hash-salt entropy comes from
`arc4random_buf`. `runtime/expat/expat_config.h` replaces the generated
configuration header, and no Apple SDK is used.

Pass the output directory with an additional `--runtime-dir`. For example:
`alchemy-fx --runtime-dir artifacts\alchemy-llvm-arm64 --runtime-dir artifacts\alchemy-expat-arm64 app`.

## Runtime identity allow-list

A Mach-O whose install name lies under `/usr/lib` or `/System/Library` is
normally rejected, because those identities belong to Alchemy's host
providers. `include/alchemy/runtime_libraries.hpp` lists the exceptions:
`libc++.1`, `libc++abi` and `libunwind` from LLVM, and `libexpat.1` from
Expat. Each entry names its upstream repository and build script. The loader
and `doctor` accept an allow-listed identity only when all of these hold:

- The file is found through `--runtime-dir`. `--dylib-dir` and `@rpath` never
  serve Apple identities.
- The file name matches the install name.
- A `provenance.json` beside the file records the entry's upstream `source`.
- That `provenance.json` lists the file under `outputs` with its exact SHA-256.

Any other case fails with the requirement that was not met. The LLVM
identities stay declaration-only provider identities when no runtime
directory supplies them. A missing libexpat is a missing dependency.

Reference specifications: [LLVM vendor build instructions](https://libcxx.llvm.org/VendorDocumentation.html),
[LLVM ABI guarantees](https://libcxx.llvm.org/ABIGuarantees.html),
[LLVM license](https://llvm.org/LICENSE.txt).
