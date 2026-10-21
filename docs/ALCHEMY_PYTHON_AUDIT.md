# Bundled CPython and native extension audit

This milestone targets a general guest Python workload: execute ARM64 CPython,
load its Python standard library, and import Mach-O native extensions through
Alchemy. The bundled Blender Python is an external test input, rather than a
special-purpose replacement interpreter. Windows Python is not an execution
backend or acceptance substitute.

## Inputs and execution boundaries

The existing extracted input is
`artifacts/alchemy-blender/extracted/Blender/Blender.app/Contents/Resources/4.5/python`.
Its `bin/python3.11` is a thin ARM64 Mach-O executable with the embedded version
string `3.11.15`. It uses conventional dyld
fixups, declares macOS 11.2 and SDK 14.0 as metadata, and depends on
`/usr/lib/libSystem.B.dylib` and CoreFoundation. Alchemy supplies independent
replacements for those identities.

The stock executable requests a 16 MiB stack through `LC_MAIN`. This was an
execution requirement at the time of the initial audit. Transmute now supplies
configurable main and worker stacks, and Alchemy passes the original requests
through that public API. The [execution milestone](ALCHEMY_PYTHON.md) records
direct execution of the unmodified stock binary; no input edits or private
engine workarounds are used.

The bundle also contains
`lib/python3.11/config-3.11-darwin/libpython3.11.a`. An independently authored
embedding launcher can link the existing ARM64 CPython archive and call the
public `Py_BytesMain` entry point, while using the engine's supported startup
stack. This tests the bundled CPython code and resources under a separate,
explicitly bounded embedding configuration. Success of that launcher does not
establish that the unmodified stock executable runs or that every Python
recursion depth is supported.

| External input | SHA-256 |
| --- | --- |
| `bin/python3.11` | `0ce92d970d2c5a7e3022e26ea0f5d0c67cc45ba843ca478927faed3dc7ebf6b9` |
| `config-3.11-darwin/libpython3.11.a` | `bd331bbe2ce404df47501329a1bd9610914153acb47180b60c4b528e6f1f9cc0` |

The bundled Makefile records `PY_ENABLE_SHARED=0` and
`SOABI=cpython-311-darwin`. The archive has 303 external symbols after removing
references satisfied by other archive members. Its additional external symbol
relative to the stock executable's import table is
`___isPlatformVersionAtLeast`. Archive-member undefined-symbol lists alone are
not missing-dependency counts.

## Native extension loading

All 75 standard-library extension images in `lib/python3.11/lib-dynload` parse
with Alchemy's existing Mach-O support. They are ARM64 `MH_BUNDLE` images with
exports such as `_PyInit__struct`. Their flat dynamic-lookup imports are resolved
against the executable's exported CPython C API. Alchemy's guest `dlsym` receives
the conventional name `PyInit__struct` and adds the Mach-O leading underscore.

An embedding launcher must expose the needed C API functions and data. Linking
the archive with `-force_load` or `-all_load` and retaining exported symbols
avoids discarding APIs that are unused by the launcher but required by an
extension. Binary API compatibility still needs real import and execution tests.

Current low-dependency extension candidates are `_struct`, `_json`, `_random`,
and `binascii`: the initial audit found no additional unresolved libc imports
beyond their CPython C API. `_pickle` additionally requires `___sprintf_chk`.
The `math` image requires several additional scalar math functions. Modules
using networking, subprocesses, database engines, crypto, NumPy, and rendering
dependencies require their own workload gates.

The bundled `zlib`, `_bz2`, and `_lzma` images contain their codec implementations
statically and declare only `libSystem` as a linked dependency. LLVM symbol
inspection confirms that the `zlib` image defines `deflate`, `inflate`, and
`zlibVersion`. A separate zlib runtime is therefore unnecessary for this
input. The initial `zlib` audit found no extra libc gaps; `_bz2` required
`___chkstk_darwin` and `___strcat_chk`, and `_lzma` required checked memory
operations. `python_compression.py` uses the actual guest extensions for
round trips, checksums, dictionaries, streaming, unused trailing bytes,
gzip framing, and malformed-input errors. Passing this script remains an
execution gate independent of static import resolution.

The authored `python_loader_*` fixtures and
`tests/alchemy_python_loader_tests.cpp` exercise the loader without pretending
to implement CPython. The fixture's `PyInit_alchemy_loader` symbol deliberately
returns an integer; it verifies symbol spelling and guest code execution, not
the Python object ABI. Actual CPython acceptance uses a real Python extension
and real bundled extensions separately.

These loader tests cover exported executable functions and data, local scopes,
global promotion, repeated handles and one-time constructors, a second bundle
calling the first, callbacks into the executable, unresolved-import rollback,
closed handles, interpreter/JIT agreement at two load bases, and nonzero JIT
execution counters. Existing loader contracts already provide these operations;
no engine changes are part of this work.

## Reproducible static audit

Run `scripts/audit_alchemy_python.ps1` from the repository or pass an explicit
`-PythonRoot`. It inspects the stock executable and every `.so` and `.dylib`
under the bundled Python library directory, including site packages. It never
executes guest code, imports modules, or runs Windows Python.

The script copies the current Alchemy inspector into the ignored report
directory so concurrent builds cannot change the inspected compatibility
profile during a run. Reports default to `artifacts/alchemy-python/audit`:

- `manifest.json`: original relative paths, binary hashes, architectures,
  initializer exports, dependencies, fixup formats, requirements, and per-module
  unresolved strong imports.
- `images/*.json`: each image's complete recursive doctor report.
- `summary.json`: missing libraries and unresolved symbols grouped by provider.
- `python-exports.txt`: the stock executable's C API export namespace.
- `libpython-undefined.txt` and `libpython-external-symbols.txt`: raw archive
  references and symbols still external after subtracting archive definitions.

`-ReuseReports` avoids repeating unchanged inspections only when the Alchemy
binary hash, input image hash, search directories, and reported root path match.
Changing the compatibility profile invalidates the cached reports.

`-ImagePattern` accepts wildcard patterns against original relative paths for
a focused re-audit. The manifest records the patterns, and its image counts
then describe that selected subset. Omit it for the entire bundle. Supply
`-RuntimeDirectory artifacts/alchemy-llvm-arm64` to inspect dependencies against
the previously built guest LLVM runtimes. The initial reports used no runtime
directory. All 376 C++ names flagged there are exported by those local guest
runtimes, though matching names alone does not verify ABI compatibility.

The initial October 1 audit inspected 163 images: one executable, 75
standard-library extensions, and 87 site-package extensions. None failed Mach-O
parsing. Forty-eight graphs had all their static imports resolvable under the
pre-Python-expansion compatibility profile. This is baseline inventory evidence,
rather than an execution result; later service additions can change that count.

The earlier audit after the basic Python service additions inspected the same 163
images, with zero Mach-O parsing failures and 75 statically import-resolvable
graphs. Its reports are in `artifacts/alchemy-python/audit-final`. The increase
from 48 to 75 records import coverage only; it does not establish successful
Python initialization or extension execution.

Those reports predate the bounded Accelerate provider. Site-package graphs also
reference SystemConfiguration, AppKit, CoreGraphics, IOKit, Metal, OpenGL and
other missing framework providers. Large rendering extensions need separate
dependency and ABI work. `libedit`, `libncurses` and `libpanel` are additional
missing dependencies for terminal-oriented modules.

The networking/ctypes/NumPy wave has a focused audit in
`artifacts/alchemy-python/audit-native-wave`. It selects the executable, four
standard-library extensions and 19 NumPy native extensions. All 24 images parse,
and no dependency identities are missing. Twenty static import graphs are
resolvable; four report specific remaining operations:

| Image | Remaining requirements |
| --- | --- |
| `_ctypes` | `_vm_remap`: checked shared-backing aliases supplied by Transmute |
| NumPy `_multiarray_umath` | `_longjmp`: guest execution-context restoration |
| NumPy `_umath_linalg` | 15 general SVD/eigen/QR LAPACK drivers |
| NumPy `lapack_lite` | Four of those deferred QR drivers |

Unsupported numeric entry points bind to explicit diagnostics, and doctor marks
them unsupported rather than treating the registered names as full support.
The summary's `unsupported_images=0` records parsing success; it does not assert
that every requested operation is implemented. Actual networking, ordinary ctypes
and named NumPy workloads pass separately in the
[execution milestone](ALCHEMY_PYTHON.md#current-verified-result).

Reproduce the selected audit with an array-valued `-ImagePattern` argument:

```powershell
& ./scripts/audit_alchemy_python.ps1 `
  -OutputDirectory artifacts/alchemy-python/audit-native-wave `
  -RuntimeDirectory artifacts/alchemy-llvm-arm64 `
  -ImagePattern @('bin/python3.11', 'lib/python3.11/lib-dynload/_socket*', `
    'lib/python3.11/lib-dynload/select.*', 'lib/python3.11/lib-dynload/_ctypes*', `
    'lib/python3.11/site-packages/numpy/*')
```

For native extension graphs, the derived manifest matches flat imports against
the stock executable's exports. Standalone doctor output lacks that Python
executable namespace and can correctly report those same names as unresolved.
`static_imports_resolvable` only assesses parsed imports and dependencies; the
manifest always leaves `execution_verified` false. It does not assert that
signals, instructions, stack requirements, ABI layouts, or runtime behavior
have passed.

## Provenance

Authored loader tests, declarations, scripts, and compatibility services retain
the project's MIT license. Existing CPython binaries and archives remain under
ignored `artifacts`; they are not redistributed as project fixtures. The input
bundle includes the CPython `lib/python3.11/LICENSE.txt`,
`Contents/Resources/text/license/spdx/Python-2.0.txt`, and its third-party
copyright inventory. CPython and each native dependency keep their
own license terms and notices. No Apple SDK headers, Apple system binaries,
shared caches, frameworks, or copied Apple implementations are used.

Public references:

- [CPython 3.11.15 extension loading](https://github.com/python/cpython/blob/v3.11.15/Python/dynload_shlib.c)
- [CPython embedding](https://docs.python.org/3.11/extending/embedding.html)
- [Py_BytesMain](https://docs.python.org/3.11/c-api/init.html#c.Py_BytesMain)
- [Python license and dependency notices](https://docs.python.org/3.11/license.html)
