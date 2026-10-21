# Bundled Python and native extension milestone

This milestone adds Alchemy support for ARM64 guest CPython and its Mach-O
extensions through Transmute. The original bundled executable now runs Python
commands, scripts, native extensions, compression, threaded workloads, networking,
ordinary ctypes calls and a bounded NumPy workload. The complete configured
suite passes 70 of 72 groups. The two ctypes callback groups require an engine
shared-memory alias capability and remain enabled failures.
The first workload uses Blender's bundled CPython 3.11, but the loader and
Darwin implementations are general runtime support. No Windows Python process
or Windows extension substitutes for guest execution.

## Plan and parallel ownership

1. Inspect the executable, static CPython library, standard library and native
   extensions. Record library identities, imports, resources, architecture,
   hashes and engine requirements without executing guest code.
2. Implement measured C runtime and Darwin dependencies. Preserve Darwin
   structures, errno, guest pointers, Apple ARM64 argument conventions and
   descriptor ownership. Unsupported process creation, privileges, signal
   delivery and terminal behavior return a documented error.
3. Execute real guest CPython initialization and command execution. Then run
   Python file/Unicode/environment/clock workloads and import native modules,
   including the bundled compression codecs.
4. Load an authored CPython extension that exchanges Python objects, invokes a
   Python callback and calls an authored dylib. Check missing imports and local
   versus global symbol scopes before claiming native extension support.
5. Compare interpreter and JIT at two load bases, check translated execution,
   preserve existing Alchemy tests and report separately owned engine failures.

The loader/runtime tests, real Python scripts, native-extension callbacks and
threaded embedding workload now pass. Transmute has supplied RCpc loads and
configurable stacks. The original executable prints `Python 3.11.15` and executes
commands with default `site` initialization. Compression exposed a CRC
instruction in the bundled zlib extension; Transmute has now supplied that
instruction and its standalone probe passes. Complete compression acceptance is
also passing for both the embedding launcher and original executable.

| Owner | Exclusive implementation | Gate |
| --- | --- | --- |
| Loader worker | Mach-O loader, native lookup tests, Python dependency audit | Checked bundle loading and lookup scopes |
| Darwin worker | File/stdio operations, identities, bounded signals, pthread adapters | File and synchronization contracts pass |
| Runtime worker | Checked fortified operations, strings, math, locale, time | C runtime ABI and memory checks pass |
| Integration owner | Launcher, guest extension, workloads, build registration, documentation | Real guest Python acceptance results |

Existing `ProcessContext`, `ModuleLoader`, synthetic functions and engine
callback interfaces remain the shared contracts. Workers release locks before
calling guest code. Transmute execution, memory, JIT and Windows implementation
remain separately owned.

## Stack contracts and embedding workload

The unmodified bundled `python3.11` requests 16,777,216 bytes in `LC_MAIN`.
Alchemy now passes that request to Transmute's configurable `ExecutionStart`.
The engine owns its allocation and callback bounds. Alchemy retains the
loader's validation against the actual allocated stack, without changing the
supplied executable.

An independently authored launcher calls public `Py_BytesMain` from the
bundle's `config-3.11-darwin/libpython3.11.a`. That launcher uses the supported
stack size and the original standard library and extensions. This remains a
bounded embedding workload, tested separately from the original executable.
The bundled CPython configuration also requests 16 MiB for Python-created
workers. Alchemy passes each explicit pthread stack attribute to the public
`fork_thread` API. A real Python worker test checks synchronization, GIL
transitions, join completion and isolated thread-local values.

## Provenance

The launcher, extension declarations, replacements and tests are authored
project code under MIT. CPython and third-party bundle files retain their own
licenses and stay in ignored local artifacts. Normal builds do not download or
redistribute the Blender bundle. No Apple SDK, Apple system library or copied
Apple implementation is used as a provider.

The audited bundle contains CPython 3.11.15. Python's public
[embedding interface](https://docs.python.org/3.11/extending/embedding.html)
and [initialization contracts](https://docs.python.org/3.11/c-api/init.html)
describe guest runtime entry points. Native modules use CPython's guest object
ABI and import machinery. [CPython licensing](https://docs.python.org/3.11/license.html)
remains distinct from the project's MIT license.
The authored extension's declarations are checked against the
[3.11.15 public module ABI](https://github.com/python/cpython/blob/v3.11.15/Include/moduleobject.h).

## Reproduction

The external input is optional and must already exist locally. To build the
embedding workload and enable its acceptance tests:

```powershell
pwsh -NoProfile -File scripts/build_alchemy_python.ps1 `
  -PythonDirectory artifacts/alchemy-blender/extracted/Blender/Blender.app/Contents/Resources/4.5/python
cmake -S . -B build/alchemy-expansion `
  -DALCHEMY_PYTHON_HOME=C:/dev/transmute/artifacts/alchemy-blender/extracted/Blender/Blender.app/Contents/Resources/4.5/python `
  -DALCHEMY_PYTHON_RUNTIME_DIRECTORY=C:/dev/transmute/artifacts/alchemy-python/runtime
cmake --build build/alchemy-expansion --config Release
ctest --test-dir build/alchemy-expansion -C Release -R alchemy_python --output-on-failure
```

The script generates declaration-only linker inputs from the CPython archive's
unresolved symbol names, using LLVM tools. Alchemy supplies the implementations
at runtime. The script preserves the CPython license and records input/output
hashes in `provenance.json`; these local artifacts are not redistributed.

## Engine execution gate

Real CPython preinitialization exposed the archive's RCpc instruction use.
The engine stopped in `_PyEval_FiniGIL` on an unsupported instruction:
`ldapr w8, [x8]`, opcode `0xb8bfc108`, at `0x100122a7c` for the default
embedding image base. Transmute now implements it, clearing that gate.
The project-authored dependency-free reproducer isolates that exact word:

```powershell
cmake --build build/alchemy-expansion --config Release --target alchemy_python_engine_probe
build/alchemy-expansion/Release/alchemy.exe build/alchemy-expansion/macos/python_rcpc_probe
build/alchemy-expansion/Release/alchemy.exe --jit build/alchemy-expansion/macos/python_rcpc_probe
```

Both modes now return 0. Alchemy does not replace the instruction with a plain
load or edit the external archive. The next dependency-free reproducer is
`python_crc_probe`, opcode `0x9acd4d08` (`crc32x w8, w8, x13`). It initially
failed alongside the real compression workload. Both probe modes now return 0;
JIT executes 11 translated instructions and one fallback instruction.
See [the engine requirements](ALCHEMY_ENGINE_REQUIREMENTS.md#current-python-execution-gate)
for the exact locations and reproduction commands.

## Run the original bundled executable

The original ARM64 `bin/python3.11` runs directly through Alchemy. It discovers
the bundle's Python resources from its executable path. No embedding launcher,
host Python interpreter or replacement Windows extension participates in this
invocation:

```powershell
& 'C:/dev/transmute/build/alchemy-expansion/Release/alchemy.exe' `
  --jit --instruction-limit 300000000 `
  'C:/dev/transmute/artifacts/alchemy-blender/extracted/Blender/Blender.app/Contents/Resources/4.5/python/bin/python3.11' `
  -B -c 'import sys, math; assert sys.platform == "darwin"; assert math.sqrt(729) == 27; print("Bundled macOS Python: 27")'
```

The verified direct run printed `Bundled macOS Python: 27` with exit 0.
Default `site` initialization remained enabled. Its JIT counters recorded
34,149,433 native guest instructions, 316,691 native blocks and 3,240 fallback
instructions. The standalone version command printed `Python 3.11.15`, exit 0,
with 8,567 native guest instructions. These counts establish guest execution;
they are not performance comparisons.

The 300-million instruction budget is explicit because Python initialization
exceeds Alchemy's default ten-million budget. The NumPy script has a separate
one-billion budget; its successful direct JIT run executes about 580 million
translated guest instructions. `-B` avoids bytecode writes to the
external bundle. Python's inherited `PYTHONHOME`/`PYTHONPATH` settings still have
their normal precedence; the acceptance tests provide an explicit guest home.

Script opening exposed CPython's `FIOCLEX` request. Alchemy now implements
`FIOCLEX`/`FIONCLEX` over guest descriptor close-on-exec flags, sharing `fcntl`
state without reading an unused variadic argument. Invalid descriptors return
`EBADF`; inapplicable requests return `ENOTTY`. The script test checks Python's
inheritable-descriptor operations and duplicate isolation as well as file data.

Additional measured helpers provide `%G/%g/%V/%C` time formatting and
`setlocale(..., "")` environment selection. Locale precedence is `LC_ALL`, the
requested category, then `LANG`; unsupported names leave locale state unchanged.
This profile still accepts C/POSIX locales only.

Native-extension acceptance now explicitly selects Darwin local loading,
reimports and reloads the same module, rejects a missing `PyInit` export,
recovers the original import, and propagates a Python callback exception. The
loaded authored extension invokes both a guest dylib and a Python callback.

CTest registers version, commands, default-site startup, nonzero exit status,
scripts, extensions, compression and workers for both the embedding launcher
and the unmodified executable. Every execution gate compares exact stdout,
empty stderr, guest exit status, interpreter/JIT and both load bases. JIT gates
require nonzero translated-instruction counters. All tests remain enabled;
no expected-failure flags or output bypasses are used.

## Current verified result

Three workers implemented networking, ctypes and NumPy concurrently. The
networking worker extended the shared descriptor namespace with Darwin sockets,
resolver and readiness services. The ctypes worker added self-task Mach VM
services and an SDK-free guest helper dylib. The numeric worker authored bounded
LP64 Accelerate replacements and C math helpers. The integration owner connected
providers, added native C helpers, registered tests and audited dependencies.
Transmute execution and memory internals remain separately owned.

The October 1, 2026 Release build succeeded with production `/W4 /WX` checks.
The complete configured suite ran 72 groups in 361.01 seconds with four parallel
test jobs: 70 passed, and both ctypes callback groups stopped at `vm_remap`'s
missing shared-backing alias operation. After the final numeric rounding and
exception-flag changes, all six affected numeric, NumPy and doctor groups passed
again in 238.07 seconds. The earlier 58-group baseline remains passing.

| Workload | Embedding launcher | Unmodified bundled executable |
| --- | --- | --- |
| `Python 3.11.15` version | Passed | Passed |
| Python command and real standard native imports | Passed | Passed |
| Default `site` initialization | Passed | Passed |
| Exact output with `sys.exit(27)` | Passed | Passed |
| Unicode, environment, clocks, files and descriptor inheritance | Passed | Passed |
| Native extension, guest dylib, callbacks and callback exceptions | Passed | Passed |
| zlib, bz2, lzma and gzip, malformed inputs and streaming | Passed | Passed |
| Workers, synchronization, GIL transitions and isolated TLS | Passed | Passed |
| TCP/UDP, socketpair, resolver, timeout and kqueue readiness | Passed | Passed |
| ctypes scalar, structure, array, data and dylib calls | Passed | Passed |
| ctypes callback allocation and invocation | Blocked: engine aliases | Blocked: engine aliases |
| NumPy 1.26.4 arrays, ufuncs, BLAS, LU, Cholesky and least squares | Passed | Passed |

Every passed cell covers interpreter/JIT and the default/second load base. The
callback tests stop before their callback assertions; callback execution is not
certified. The suite also passed all four Windows shards, guest LLVM
C++/exception workloads, Objective-C/CoreFoundation/Foundation, dispatch, loader
and malformed-input regressions. The original Python executable and archive
SHA-256 values still match the audit; external inputs were not modified.

The standalone NumPy JIT gate executed over 573 million translated guest
instructions at each load base. A separate direct run printed exactly
`Alchemy NumPy: arrays BLAS LU Cholesky`, exited 0, and recorded 580,408,328
translated instructions. These counters establish translated execution and are
not performance comparisons.

## Supported subset and remaining requirements

Networking uses real Windows sockets behind guest descriptor identities. The
workload covers local TCP with threaded file wrappers, UDP/IPv6, anonymous stream
and datagram socketpairs, resolver and interface queries, timeouts, nonblocking
operations, descriptor duplication and Python's default kqueue selector.
Read readiness reports available bytes; write readiness reports a conservative
lower bound of one byte. Exact Darwin send-space accounting is not implemented.
Pathname Unix sockets, raw sockets, ancillary messages/descriptor passing,
multicast options, positive linger and unsupported kqueue filters/flags fail
explicitly. Resolver waits are bounded and retain native ownership after timeout.

The ctypes workload imports the real guest `_ctypes`/libffi extension and calls
an authored ARM64 Mach-O dylib. It checks LP64 integers, doubles, mixed structures
by value, arrays, strings, checked memory functions, mutable exported data and
`CDLL(None)` lookup. Mach VM replacements provide bounded private allocations,
deallocation and protection. Bundled libffi's callback trampolines need shared
aliases with destination overwrite and subrange splitting. Alchemy reports the
[engine requirement](ALCHEMY_ENGINE_REQUIREMENTS.md#current-python-execution-gate)
without copying a trampoline or changing guest binaries.

The numeric provider implements real/complex single/double BLAS, LU factorization
and solves, Cholesky and bounded least squares. Least squares uses an authored
one-sided Jacobi SVD with workspace, rank, singular-value and minimum-norm
contracts. Guest rounding and floating-point exception flags are preserved across
scalar math and numeric calls. Generic matrix dimensions are bounded at 4096 with
an operation budget; least squares is further bounded at 512 rows/columns,
128 singular values and 256 right-hand sides. This is LP64 `$NEWLAPACK` support;
ILP64 and 15 general SVD/eigen/QR drivers remain explicit unsupported operations.
Guest `_longjmp` still requires engine context restoration. The named NumPy
workload does not call that path or the deferred drivers.

`doctor --deps --json` reports these requirements independently of execution.
The focused audit parsed 24 selected images with no missing dependency identities;
20 static import graphs were resolvable and four reported remaining operations.
See the [audit](ALCHEMY_PYTHON_AUDIT.md) for the inventory and reproduction.

This result certifies the named workloads. Full Python standard-library and
site-package compatibility, interactive terminals, subprocesses and Blender
startup remain outside the verified profile. Subsequent native-module work should
add explicit execution gates while preserving the passing tests.

The implementations use public interface references, including XNU's
[socket declarations](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/socket.h)
and [kqueue declarations](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/event.h),
the [Apple ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms),
and [NumPy's native linear algebra call sites](https://github.com/numpy/numpy/blob/v1.26.4/numpy/linalg/umath_linalg.cpp).
The Accelerate provider contains authored algorithms and does not load an Apple
framework or SDK payload. External NumPy and libffi code remains in the local
bundle under its own licenses.

## Initial integration result

On October 1, 2026, the final Visual Studio Release build succeeded with the
production `/W4 /WX` checks. The complete configured suite ran all 42 groups:
39 passed and three optional Python workloads failed at the documented RCpc
instruction. Total test time was 52.23 seconds. None of these failures was
disabled, marked as an expected failure, or hidden by a fixed hash seed.

| Gate | Result |
| --- | --- |
| Existing Windows, Alchemy, Objective-C/CF and LLVM C++ regressions | All passed |
| Native-extension loader | Passed interpreter/JIT and two bases; functions, data, constructors, scopes, callbacks and failure cleanup |
| Darwin/runtime helper tests | Passed, including real cryptographic entropy, descriptor behavior, ARM64 floating-point ABI and variadic padding |
| Real CPython archive `--version` | Printed `Python 3.11.15` with exit 0 and empty guest stderr in all four execution configurations |
| Python script, actual extension imports, compression | Blocked at `ldapr w8, [x8]`; their Python assertions have not executed |
| Original standalone Python executable | Still rejected for its 16 MiB main stack requirement |
| Python-created workers | Need the bundle's configured 16 MiB worker stacks |

A direct JIT version run executed 8,146 native guest instructions in 539 blocks,
with 74 fallback instructions. The passing version path does not initialize a
Python interpreter or validate extension object behavior. The real script gate
advances through entropy initialization and then stops at the engine instruction
requirement; further runtime gaps may appear after that requirement is supplied.

The final static audit parsed all 163 bundled native images without errors.
Seventy-five dependency graphs have resolvable static imports, compared with
48 before this wave. Remaining imports include C++ runtime symbols, framework
providers and terminal libraries; see [the audit](ALCHEMY_PYTHON_AUDIT.md).

The original Python executable and archive hashes still match the audit. The
embedding build retains the local CPython license, SPDX license and available
bundle copyright inventory beside its outputs. All compatibility replacements
are authored code; external CPython and codec binaries are not redistributed.
