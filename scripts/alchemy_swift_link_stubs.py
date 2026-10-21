"""Writes link-only interface stubs for a guest Swift library.

Usage: python -I alchemy_swift_link_stubs.py <llvm-nm> <output-dir> <runtime dylib...> -- <objects...>

Collects the undefined symbols of the objects that neither the objects nor the
given runtime dylibs (the guest libc++/libc++abi, libswiftCore, ...) define,
and writes one assembly source per Apple system identity naming them:
  objc.s       -> /usr/lib/libobjc.A.dylib (Objective-C runtime entry points,
                  NSObject)
  foundation.s -> Foundation.framework (its Objective-C classes and NS* names)
  system.s -> /usr/lib/libSystem.B.dylib (everything else)
The stubs are linked into dylibs that only give the linker two-level
namespace bindings; they are never loaded (Alchemy's host providers serve
these identities), like the project's tests/fixtures/macos/link_*.c inputs.
"""
import re
import subprocess
import sys

OBJC = re.compile(r"^_(objc_|_objc_|class_|object_|sel_|protocol_|method_|ivar_|imp_|OBJC_CLASS_\$_NSObject$|OBJC_METACLASS_\$_NSObject$|OBJC_EHTYPE_)")
# Other Objective-C classes (NSString, NSData, ...) are Foundation's.
FOUNDATION = re.compile(r"^_(OBJC_CLASS_\$_|OBJC_METACLASS_\$_|NS[A-Z])")


def symbols(nm: str, paths: list[str], flags: list[str]) -> set[str]:
    result: set[str] = set()
    for start in range(0, len(paths), 200):
        output = subprocess.run([nm, *flags, *paths[start:start + 200]], check=True, capture_output=True, text=True).stdout
        for line in output.splitlines():
            parts = line.split()
            if parts and not line.endswith(":"):
                result.add(parts[-1])
    return result


def main() -> None:
    separator = sys.argv.index("--")
    nm, out = sys.argv[1], sys.argv[2]
    runtimes, objects = sys.argv[3:separator], sys.argv[separator + 1:]
    undefined = symbols(nm, objects, ["-u", "-j"])
    defined = symbols(nm, objects, ["-g", "--defined-only", "-j"]) | symbols(nm, runtimes, ["-g", "--defined-only", "-j"])
    needed = sorted(undefined - defined)
    groups = {
        "objc": [s for s in needed if OBJC.match(s)],
        "foundation": [s for s in needed if not OBJC.match(s) and FOUNDATION.match(s)],
        "system": [s for s in needed if not OBJC.match(s) and not FOUNDATION.match(s)],
    }
    for name, names in groups.items():
        with open(f"{out}/{name}.s", "w", encoding="ascii", newline="\n") as stream:
            stream.write("// Generated link-only interface; never executed.\n.text\n.p2align 2\n")
            for symbol in names:
                stream.write(f'.globl "{symbol}"\n"{symbol}":\n')
            stream.write("ret\n")
        print(f"{name}: {len(names)} symbols")


if __name__ == "__main__":
    main()
