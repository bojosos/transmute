# Vendored open-source /usr/lib runtime libraries

macOS ships several open-source libraries in `/usr/lib` that applications link directly. GTK and Qt
applications, LibreOffice and the Python and Perl runtimes inside app bundles load them at start-up,
so a missing one stops the app before its first instruction. Alchemy builds these from pinned
upstream sources as ARM64 guest libraries under Apple's install names, as it does for libexpat.

## Pattern

| Piece | Where |
| --- | --- |
| Upstream sources, copied verbatim from the release archive (only the files the build needs), `LICENSE`, `PROVENANCE.md` (version, URL, archive SHA-256, per-file SHA-256, modifications) | `third_party/<name>/`, copied by `scripts/vendor_files.py` |
| Project-authored build configuration and the Darwin declarations the sources need beyond `runtime/include` | `runtime/<name>/` |
| Build: clang for arm64-apple-macos11 with `-nostdinc` and the project headers; a pinned libSystem import set (the build fails if it changes); the export set; ld64.lld with Apple's install name and the versions applications record; `provenance.json` | `scripts/build_alchemy_<name>.ps1` (`-OutputDirectory`, default `artifacts/alchemy-<name>-arm64`) |
| Loader allow-list: the install name is accepted only from a `--runtime-dir` whose `provenance.json` records the upstream and the file's SHA-256 | `src/alchemy/runtime_libraries.cpp` |
| Test: allow-list, doctor, refusal of a missing or foreign provenance and a modified file, and a guest fixture in alchemy-fx processes at two load bases | `cmake/AlchemyOssRuntimes.cmake` (`alchemy_oss_runtime(...)`), `tests/alchemy_oss_runtime_tests.cpp`, `tests/fixtures/macos/<name>_app.c` and `.expected` |

Runs pass one `--runtime-dir` per library: `--runtime-dir artifacts/alchemy-libffi-arm64` and so on, next to
the LLVM and Expat runtime directories. The build scripts write only to their output directory; the
`alchemy_<name>_runtime` targets rebuild the shared artifacts and are run on main only.

## libffi: /usr/lib/libffi.dylib

libffi 3.8.0 (MIT), `scripts/build_alchemy_libffi.ps1`, compatibility version 1.0.0 and current version
40.0.0. It exports exactly the names of upstream's version script (`libffi.map.in`, preprocessed with
this configuration): 46, among them `ffi_call`, `ffi_prep_cif(_var)`, the closure API and the
`ffi_type_*` objects. GLib's GObject (GIMP, Inkscape, darktable), Python's `ctypes` and LibreOffice
import 18 of them.

**Closures under fx.** libffi's arm64 Darwin port never writes executable memory. Its `__TEXT` holds a
16 KiB page of identical trampolines (`ffi_closure_trampoline_table_page`). `ffi_closure_alloc` takes two
pages with `vm_allocate`, keeps the first as a writable config page and `vm_remap`s the trampoline page
over the second (`VM_FLAGS_OVERWRITE`, shared). A trampoline loads its closure and entry point from
the config page exactly 16 KiB below itself (`adr x16, -PAGE_MAX_SIZE`), so 1024 closures share one
mapping and no page is ever writable and executable. Alchemy's `vm_remap` serves an immutable
executable source as a copy with execute permission and tells the translator the code is the image's
(`engine_code_copy`), so fx translates the trampolines at their new address and the PC-relative load
finds the config page. The fixture checks 2600 closures on three trampoline pages, called from guest
code and as a `qsort` comparator called back from the libSystem provider.

The sources carry BTI landing pads (`hint #34` and `#36`), which are no-ops on cores without BTI;
alchemy-fx, the reference interpreter and the JIT all run them as such, and the test runs the fixture in
all three (`IN_PROCESS`).

## SQLite: /usr/lib/libsqlite3.dylib

SQLite 3.54.1 (public domain), the amalgamation, `scripts/build_alchemy_sqlite.ps1`, compatibility version
9.0.0 and current version 380.0.0. It exports every global the amalgamation defines (300 names: the
C interface and data such as `sqlite3_version` and `sqlite3_temp_directory`), which covers the 107
names Krita, darktable, LibreOffice and the bundled Python `sqlite3` modules import. The feature set
(`runtime/sqlite/sqlite_cfg.h`) is close to what macOS applications expect: FTS3/4/5, R*Tree and
Geopoly, column metadata, the pre-update hook, unlock notification, `dbstat`, statement and snapshot
APIs, math functions, loadable extensions, and API argument checks. File locking is the portable
POSIX style through `fcntl` record locks; Apple's AFP and proxy locking styles are off, and memory
comes from `malloc` rather than a private malloc zone. WAL mode works: its shared-memory index is a
`MAP_SHARED` mapping of the `-shm` file, which alchemy-fx shares between Alchemy processes.

The fixture runs a file database in WAL mode with a second connection, prepared statements, FTS5,
JSON and math functions, a guest SQL function and collation, the blob and backup APIs, and
serialize/deserialize. It runs in alchemy-fx processes only: WAL's writable `MAP_SHARED` file mapping needs
the fx engine (the in-process engines have private mappings only).

## ICU: /usr/lib/libicucore.A.dylib

A subset of ICU4C 79.1 (Unicode-3.0), `scripts/build_alchemy_icu.ps1`, compatibility version 1.0.0 and current
version 79.1.0. Like Apple's libicucore it exports unversioned names (`U_DISABLE_RENAMING`). The export set is
what macOS applications import from it here (libpsl and raptor2 in darktable and LibreOffice): UTF-8
conversion, case mapping and string functions, and UTS #46 IDNA, 27 names. The build compiles the 68 sources
of ICU's common library those need (file I/O, conversion and services off) and links libc++ from the LLVM
runtime, so runs pass both runtime directories. The only data is UTS #46's normalization: upstream's prebuilt
`uts46.nrm` in a one-item common-data package (`runtime/icu/make_icudata.ps1`, 61,712 bytes); properties, case
mapping and NFC are compiled into ICU's sources. Other ICU APIs are absent, so an image importing them still
stops at load with the missing name.
