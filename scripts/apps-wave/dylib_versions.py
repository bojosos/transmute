"""Prints the dylib dependencies a Mach-O image records, with the versions it records for each.

Reads only the Mach-O header and load commands (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB,
LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB, LC_ID_DYLIB): the same metadata `alchemy doctor` lists. A universal file
uses its arm64 slice.

    python -I dylib_versions.py <image> [<image> ...]
"""
import struct
import sys

COMMANDS = {0xC: "load", 0x80000018: "weak", 0x8000001F: "reexport", 0x20: "lazy", 0x80000023: "upward", 0xD: "id"}


def version(value):
    return f"{value >> 16}.{(value >> 8) & 0xFF}.{value & 0xFF}"


def arm64_slice(data):
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        wide = magic == 0xCAFEBABF
        count = struct.unpack_from(">I", data, 4)[0]
        for index in range(count):
            if wide:
                cpu, _, offset, size = struct.unpack_from(">iiQQ", data, 8 + index * 32)[:4]
            else:
                cpu, _, offset, size = struct.unpack_from(">iiII", data, 8 + index * 20)[:4]
            if cpu == 0x0100000C:
                return data[offset:offset + size]
        raise SystemExit("no arm64 slice")
    return data


def dependencies(path):
    data = arm64_slice(open(path, "rb").read())
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit(f"{path}: not a 64-bit Mach-O")
    offset = 32
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", data, offset)
        if command in COMMANDS:
            name_offset, _, current, compatibility = struct.unpack_from("<IIII", data, offset + 8)
            name = data[offset + name_offset:offset + size].split(b"\0", 1)[0].decode()
            yield COMMANDS[command], name, version(current), version(compatibility)
        offset += size


for image in sys.argv[1:]:
    print(image)
    for kind, name, current, compatibility in dependencies(image):
        print(f"  {kind:8} {name}  (compatibility {compatibility}, current {current})")
