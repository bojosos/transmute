# Alchemy expansion

Alchemy implements macOS user-space services on Windows and delegates ARM64
execution to Transmute. This expansion edits Alchemy sources, authored guest
declarations and fixtures, build registration, and documentation. Transmute's
interpreter, translator, memory internals, Windows bridge, and engine tests are
owned separately.

## Contracts and ownership

`ProcessContext` owns providers, guest allocations, paths, environment, module
notifications, thread records, output serialization, and process termination.
`ThreadContext` belongs to one existing engine worker and carries errno,
`dlerror`, pthread identity, TLS blocks, keys, and cleanup registrations.
`ModuleLoader` owns persistent modules, dependencies, scopes, handles, and
constructor state. Export records distinguish code, data, and TLS descriptors.
Relocated TLS templates and unwind sections are published before constructors.

Service metadata locks are released before waiting or calling guest code.
The engine adapter calls the existing `fork_thread` and callback APIs. Missing
engine behavior fails with a named requirement instead of a private execution
implementation. `run_program` and the original command-line defaults remain.

Loader, Darwin-service, and thread/C++ workers own separate implementation and
test files. The integration owner registers targets and fixtures. Engine work
is communicated to the separately owned Transmute task.

## Reproducible builds

The host minimum is x64 Windows 10 or Windows Server 2016. Precise Windows
interrupt and unbiased-interrupt clocks implement Darwin clock profiles that
include or exclude suspension.

The normal CMake build creates authored C and assembly Mach-O fixtures without
an Apple SDK. Their declaration-only link inputs are never runtime providers.
The fixtures execute the real loader and synthetic service calls.

```powershell
cmake -S . -B build/alchemy-expansion -G "Visual Studio 17 2022" -A x64
cmake --build build/alchemy-expansion --config Release
ctest --test-dir build/alchemy-expansion -C Release --output-on-failure
```

LLVM runtimes require an explicit build. Source retrieval is not part of a
normal build or configuration.

```powershell
pwsh -NoProfile -File scripts/build_alchemy_llvm.ps1 -Fetch -Runtime all
cmake -S . -B build/alchemy-expansion
cmake --build build/alchemy-expansion --config Release --target alchemy_cpp_tests
ctest --test-dir build/alchemy-expansion -C Release -R alchemy_cpp --output-on-failure
```

The script pins LLVM 19.1.5 commit
`ab4b5a2db582958af1ee308a790cfdb42bd24720`. It builds ARM64 Mach-O libc++,
libc++abi, libunwind, and required compiler-rt integer builtins against
independently authored declarations. Runtime outputs and source checkouts stay
under ignored `artifacts/` directories. Root and component license files are
copied beside the output libraries. See [the runtime profile](../runtime/README.md).
Supplemental [Ryu notices](../runtime/THIRD_PARTY_NOTICES.md) accompany the
runtime outputs. `provenance.json` records source, flags, tools, authored
header hashes, runtime hashes and notice hashes.

## Acceptance workloads

| Workload | What it checks |
| --- | --- |
| Original sample and starter fixtures | Exact bytes, guest status, startup arguments, strong/weak imports, direct syscalls, relocation, and real translated execution |
| Nested loader/plugin fixture | Two authored dylibs, reexports, executable symbols, callbacks, runtime bundle loading, constructor lifetime, lookup scopes, and resident close/reopen |
| C services fixture | Zero-filled allocation, realloc, environment mutation, formatting, binary file write/read/seek, clocks, entropy, and finalization |
| Darwin threads | Worker return and pointer-valued exit, synchronization, once, keys, guards, errno/TLS ownership, and cleanup |
| C++ exceptions fixture | Real guest LLVM exception allocation, unwinding across a guest dylib, destructor cleanup, RTTI, and dynamic cast |
| C++ runtime fixture | C++17 containers, strings, streams, filesystem, std::thread, mutexes, atomics, and cross-dylib std::exception |
| Bundle/doctor fixtures | XML and binary Info.plist, executable discovery, checked paths, recursive dependency JSON, and malformed-input rejection |

Execution acceptance tests compare interpreter and JIT results at the normal
and a second load base, and require nonzero translated counters when JIT is
selected. Doctor inspects metadata and symbol availability without executing
guest code. Its `supported` field does not certify runtime compatibility.

Workers created before a runtime bundle is loaded receive that bundle's TLS
template on first access. The authored integration test checks independent
values in two workers and the main thread. The nested-library test allocates
in the leaf library, modifies through the bridge, and frees in the executable.

Guest paths use POSIX separators. Windows drives appear as `/Volumes/C/...`,
`/tmp` maps to the host temporary directory, and `/` uses the initial working
directory's drive root. Relative paths use the process working directory.
Native Windows drive paths remain accepted for command-line convenience.
Startup and dyld path queries return guest paths; doctor and diagnostics retain
native source paths. This namespace exposes host files and is not a sandbox.

## Verified result

On 2026-10-01, a clean Visual Studio Release build passed all 20 configured
CTest groups in 36.36 seconds. This includes the four existing Windows test
shards, the external sample, expanded C services, nested dylibs, runtime
plugins, late-loaded TLS, and both real guest LLVM C++ gates. Execution tests
check exact output, exit status and empty guest stderr in interpreter/JIT modes
at both load bases. JIT tests require translated guest instructions.

The full C++17 fixture printed and wrote `Guest C++ runtime: 69\n`. A direct JIT
run executed 23,221 native guest instructions in 1,775 blocks, with 364 fallback
instructions. The exception/RTTI fixture printed
`Guest C++ exception and RTTI: 27 42\n`. After the clean rebuild, 25 additional
trace-free thread-unit runs passed.

The supplied `test_arm` still printed exactly `Hello World from MacOS: 27\n`
and returned 0. Its JIT run executed 22 native guest instructions in three
blocks with no fallback. Its SHA-256 remained
`63097a1ee99e51840230c98692bade514ed5bbdfe4ef5005f0b4a4f9ad8d6602`.
The binary remains an optional external input, not a redistributed fixture.
The final notice/provenance packaging preserved all four tested LLVM runtime
binaries byte for byte.

## Limits and follow-up gates

Darwin services cover an explicit subset. Unsupported conversions, path forms,
relocations, traps, signals, locales, and resource policies fail explicitly.
Guest file descriptors and stdio objects are distinct from Windows descriptors
and pointers. No guest variadic list is forwarded to a Windows formatter.

Modules stay resident until shutdown. Physical unload, signals, full locale
behavior, shared VM, arbitrary partial VM operations, and full pthread
attributes are later work. Guest exceptions must not unwind through host
service frames. Apple's libc++ symbol names alone do not establish object-layout
compatibility; vendor build options affect ABI. [LLVM ABI guarantees](https://libcxx.llvm.org/ABIGuarantees.html)

Stdio is unbuffered in this profile. Windows file identity and timestamps are
translated into Darwin structures, but Unix ownership and BSD flags currently
read as zero and permission support is limited. Native symlink checks are
conditional on host permission; this host denied creation, and the tests verify
that its privilege error becomes Darwin `EPERM`.

The next application gates are bundled CPython and extension loading,
audit-selected framework services, Blender `--version`, background scene
creation and saving, then a small CPU render. Windowing and GPU translation
remain deferred. The [Blender audit](ALCHEMY_BLENDER_AUDIT.md) records real bundle
requirements rather than assigning framework work from assumptions.

## Provenance

Authored replacements, adapters, declarations, fixtures, and tooling retain
the project's MIT license. LLVM source and built runtimes retain their own
licenses and exceptions; they are not relicensed as MIT. The build does not
read Apple SDKs, libraries, frameworks, shared caches, or implementation code.
External binaries are local audit inputs and are not redistributed fixtures.
This is a specification-driven implementation, without a formal clean-room
claim or a blanket legal conclusion. [LLVM licensing policy](https://llvm.org/docs/DeveloperPolicy.html#copyright-license-and-patents)
