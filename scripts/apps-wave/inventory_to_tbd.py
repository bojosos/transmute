"""Turns an Alchemy registration inventory into text link stubs (.tbd) for the toolchain checks.

The inventory comes from a guest run with ALCHEMY_REGISTRATION_AUDIT=<file>.tsv. Its "export" rows are
"export<TAB><library> <symbol><TAB><owner>...". Each library that Alchemy provides gets one .tbd listing every name
Alchemy registers for it, so a guest linker (ld64.lld) can link programs against what Alchemy actually provides.
The stubs are our own data; no SDK material is involved.

    python -I inventory_to_tbd.py <inventory.tsv> <output dir>

Library file names: /usr/lib/libSystem.B.dylib -> libSystem.tbd (plus libc, libm, libpthread, libdl aliases, which
the Darwin libSystem also serves); /usr/lib/libfoo.N.dylib -> libfoo.tbd;
/System/Library/Frameworks/X.framework/... -> X.framework/X.tbd.
"""

import collections
import os
import re
import sys

LIBSYSTEM = "/usr/lib/libSystem.B.dylib"
UNWIND = ["__Unwind_Resume", "__Unwind_RaiseException", "__Unwind_DeleteException", "__Unwind_GetIP",
          "__Unwind_GetLanguageSpecificData", "__Unwind_GetRegionStart", "__Unwind_SetGR", "__Unwind_SetIP",
          "__Unwind_Backtrace", "__Unwind_GetCFA", "__Unwind_FindEnclosingFunction", "___register_frame",
          "___deregister_frame", "__Unwind_GetIPInfo", "__Unwind_ForcedUnwind", "__Unwind_GetDataRelBase",
          "__Unwind_GetTextRelBase", "__Unwind_GetGR"]
# dyld's image API is served by the loader core, outside the registration audit.
DYLD = ["__dyld_image_count", "__dyld_get_image_header", "__dyld_get_image_name", "__dyld_get_image_vmaddr_slide",
        "__dyld_register_func_for_add_image", "__dyld_register_func_for_remove_image", "__NSGetExecutablePath",
        "__NSGetArgc", "__NSGetArgv", "__NSGetEnviron", "__NSGetProgname", "__NSGetMachExecuteHeader"]
LIBSYSTEM_ALIASES = ["libc", "libm", "libpthread", "libdl", "libdyld"]


def tbd(install_name, symbols):
    lines = [
        "--- !tapi-tbd",
        "tbd-version:     4",
        "targets:         [ arm64-macos ]",
        f"install-name:    '{install_name}'",
        "exports:",
        "  - targets:     [ arm64-macos ]",
        "    symbols:     [ " + ",\n                   ".join(f"'{name}'" for name in sorted(symbols)) + " ]",
        "...",
        "",
    ]
    return "\n".join(lines)


def stub_path(out_dir, install_name):
    framework = re.match(r".*/([^/]+)\.framework/", install_name)
    if framework:
        name = framework.group(1)
        return os.path.join(out_dir, f"{name}.framework", f"{name}.tbd")
    base = os.path.basename(install_name)
    stem = re.sub(r"(\.[A-Z0-9]+)?\.dylib$", "", base)
    return os.path.join(out_dir, stem + ".tbd")


def main(inventory, out_dir):
    exports = collections.defaultdict(set)
    with open(inventory, encoding="utf-8") as handle:
        for line in handle:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 2 or fields[0] != "export":
                continue
            library, _, symbol = fields[1].partition(" ")
            if library.startswith("/") and symbol:
                exports[library].add(symbol)
    if LIBSYSTEM in exports:
        # The loader binds this itself, so it is never registered; classic lazy binding links against it.
        exports[LIBSYSTEM].add("dyld_stub_binder")
        # libSystem re-exports libunwind; Alchemy loads the runtime's libunwind for it (load_system_unwinder).
        exports[LIBSYSTEM].update(UNWIND)
        exports[LIBSYSTEM].update(DYLD)
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for library, symbols in sorted(exports.items()):
        path = stub_path(out_dir, library)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(tbd(library, symbols))
        written.append((path, len(symbols)))
    if LIBSYSTEM in exports:
        for alias in LIBSYSTEM_ALIASES:
            with open(os.path.join(out_dir, alias + ".tbd"), "w", encoding="utf-8", newline="\n") as handle:
                handle.write(tbd(LIBSYSTEM, exports[LIBSYSTEM]))
    for path, count in written:
        print(f"{count:6d} {os.path.relpath(path, out_dir)}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
