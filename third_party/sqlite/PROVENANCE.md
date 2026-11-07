# SQLite (vendored as /usr/lib/libsqlite3.dylib)

| Field | Value |
| --- | --- |
| Project | SQLite: an embedded SQL database engine |
| Upstream | <https://www.sqlite.org> |
| Version | 3.54.1 (amalgamation 3540100) |
| Archive | `https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip`, 2,990,966 bytes, SHA-256 `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`, SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` (matches the SHA3-256 on sqlite.org's download page) |
| License | Public domain: SQLite's authors dedicate the code to the public domain (the blessing at the top of `sqlite3.c` and `sqlite3.h`); the amalgamation has no separate licence file |
| Files | `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h`. The shell (`shell.c`) is not vendored |
| Modifications | None. Copied by `python -I scripts/vendor_files.py <archive> third_party/sqlite --strip sqlite-amalgamation-3540100/ sqlite3.c sqlite3.h sqlite3ext.h`. The feature set is the project's `runtime/sqlite/sqlite_cfg.h` (read through `_HAVE_SQLITE_CONFIG_H`); the Darwin declarations the unix VFS needs beyond `runtime/include` are the project's own, in `runtime/sqlite/include` |

Use: built by `scripts/build_alchemy_sqlite.ps1` as `/usr/lib/libsqlite3.dylib` (compatibility version 9.0.0,
current version 380.0.0, as macOS applications record it). See `docs/ALCHEMY_OSS_RUNTIMES.md`.

To update: download the new amalgamation, check its SHA3-256 against the download page, rerun the copy command,
build, run the `alchemy_sqlite_runtime` test, and update this file, the build script's pins and
`THIRD_PARTY_NOTICES.md`.

SHA-256 of the vendored files, as the copy script prints them; the tree hash is the SHA-256 of this list (one
LF-terminated line per file):

```
0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52  sqlite3.c
d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c  sqlite3.h
48963abfefb41ac329ef9d706acb5eadba952a99a748285c9ded6b5e0942721b  sqlite3ext.h
```

Tree hash: `fd1520c398367b506e28f1a0f6d46e049ed9b5a8296ff0e9c5ba65d43ae9cc9a` (3 files).
