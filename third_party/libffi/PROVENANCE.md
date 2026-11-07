# libffi (vendored as /usr/lib/libffi.dylib)

| Field | Value |
| --- | --- |
| Project | libffi: a portable foreign-function interface library |
| Upstream | <https://github.com/libffi/libffi> |
| Version | 3.8.0 (release of 2026-08-08) |
| Archive | `https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz`, 1,581,626 bytes, SHA-256 `7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4` (matches the GitHub release asset digest) |
| License | MIT (`LICENSE`, copied verbatim) |
| Files | `LICENSE`, `README.md`, `libffi.map.in` (the export list), `include/` (`ffi.h.in` and three internal headers), the common sources `src/{prep_cif,types,closures,raw_api,java_raw_api,tramp}.c` and the aarch64 port `src/aarch64/`. Other ports, the build system, tests and documentation are not vendored |
| Modifications | None. Copied by `python -I scripts/vendor_files.py <archive> third_party/libffi --strip libffi-3.8.0/ <files>`. The build configuration (`fficonfig.h`) and the Mach VM declarations the trampoline table uses are the project's own, in `runtime/libffi/`; `ffi.h` is generated from `ffi.h.in` by the build script with the values configure substitutes for aarch64-apple-darwin |

Use: built by `scripts/build_alchemy_libffi.ps1` as `/usr/lib/libffi.dylib` (compatibility version 1.0.0, current
version 40.0.0, as macOS applications record it). See `docs/ALCHEMY_OSS_RUNTIMES.md`.

To update: download the new release, check the digest and the licence, rerun the copy command with the same file list,
build, run the `alchemy_libffi_runtime` test, and update this file, the build script's pins and
`THIRD_PARTY_NOTICES.md`.

SHA-256 of the vendored files, as the copy script prints them; the tree hash is the SHA-256 of this list (one
LF-terminated line per file):

```
17b64dc60f3b6897a60f971e288b973f655c2edcdf08b25f3c3dd5549857881c  LICENSE
a1876e27771be3b550a2e72a3dccc72d05382bb50354c8698d92eeb197fe40a1  README.md
decd0f2f39008aedf424f54a5f15f32328cd9a5c4384c7d4a347a244f5da4705  include/ffi.h.in
13ed26eeafad029ec04b802bea0d86c68a1a09c0a71b90ee69992c4cfbc8d310  include/ffi_cfi.h
1d8090956e934992f1505a21b11de69ff6cc43e0e1e4f31dc2e20b156fc5f297  include/ffi_common.h
ef20dace99925672167a5f3b3898299d462ace170f0b90ca47fd36b30cfc7fbf  include/tramp.h
d1d86ad37eca1dad64cd2b02dda8307268f2fe8d7c3fbc3675d7788a1b4c43ec  libffi.map.in
6cb999516a85c94de18f7dbc0c746e8c15d89bc8974553c7270d0e20b674e4dc  src/aarch64/ffi.c
ca1c2f5b66fac019373a031b4c47acd67f508ac43a0f18fe620bb5890a26d50e  src/aarch64/ffitarget.h
6d335424d7a7364e884dad2e9af24f58086d32e716677f2f36b092f31cf43afb  src/aarch64/internal.h
5bcd3da436bcbb69974c9da20b870232e3517b7e509fcbe47827283b88d91576  src/aarch64/sysv.S
0c7d404a66e62154077a08966f21016793a75ad1fcd0be2b3fe814d93c3efcb6  src/closures.c
e8efc8c19d002c8df36c63c0f1b1b010cfd607f2de28442d4c1318a91ac9f34c  src/java_raw_api.c
37431a8b5f848007c5a80465b76a06eb6ce0d6485bd5c5480d7609dde852a230  src/prep_cif.c
9da2f31d073dba9451714082f2c14d45de0512b7b9d945865c78d2b8f033a9fe  src/raw_api.c
803ae4e289003cbd38659dad39bf15cfdfc06a18406cf9f8f7d190b703d942d0  src/tramp.c
74d640cf0c9668a08b148e22dd1ee180364ac8a4f5be0c2d2675c03568586277  src/types.c
```

Tree hash: `5bee9e6c53e1fc229152572fdc4f5a3e662257d4c795193974d984f28d82f06d` (17 files).
