"""Static layout audit of the hottest regions of an fx profile.

Usage: python tools/fx_layout_audit.py <profile.txt> <fx_translator_image_audit.exe> <out-dir> [count] [env=value ...]

Takes the `count` hottest regions of the profile (default: those covering 90%
of the translated samples, or the top 40 listed), translates each region's
guest start statically with the given audit binary (TRANSMUTE_FX_AUDIT_DUMP),
disassembles the code and reports where the bytes go: the hot body (before
the cold section, when the translator reports it), NOP padding, unlinked exit
tails, indirect-branch lookups, RSB pushes, r15-relative operands with 32-bit
displacements and rel32 branches whose target lies within rel8 reach.
Static: regions formed at run time can differ (entries of other regions).
"""
import collections
import os
import pathlib
import re
import subprocess
import sys
import tempfile

LLVM = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin")
STAGE = pathlib.Path(r"C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5")


def module_path(name):
    for p in (STAGE / name, STAGE / "blender.shared" / name):
        if p.exists():
            return p
    hits = list(STAGE.rglob(name))
    return hits[0] if hits else None


def hot_regions(profile, count):
    regions = []
    for line in pathlib.Path(profile).read_text(errors="replace").splitlines():
        m = re.match(r"\s*([\d.]+)%\s+(\S+?)\+0x([0-9a-f]+)\s.*host 0x[0-9a-f]+ size (\d+)", line)
        if m:
            regions.append((m[2], int(m[3], 16), float(m[1]), int(m[4])))
    return regions[:count]


def disassemble(code):
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "r.s"
        with src.open("w") as s:
            s.write(".text\nr:\n")
            for i in range(0, len(code), 16):
                s.write(".byte " + ",".join(map(str, code[i:i + 16])) + "\n")
        obj = pathlib.Path(tmp) / "r.obj"
        subprocess.run([str(LLVM / "clang.exe"), "--target=x86_64-pc-windows-msvc", "-c", str(src), "-o", str(obj)], check=True)
        out = subprocess.check_output([str(LLVM / "llvm-objdump.exe"), "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", str(obj)], text=True)
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
        if m:
            rows.append((int(m[1], 16), m[2], m[3]))
    insts = []
    for k, (at, op, args) in enumerate(rows):
        end = rows[k + 1][0] if k + 1 < len(rows) else len(code)
        insts.append((at, end - at, op, args))
    return insts


def classify(code, hot_end):
    insts = disassemble(code)
    c = collections.Counter()
    n = len(insts)
    i = 0
    while i < n:
        at, size, op, args = insts[i]
        part = "hot" if at < hot_end else "cold"
        c[part] += size
        c[part + "_insts"] += 1
        if op.startswith("nop") or op == "int3":
            c[part + "_nop"] += size
        if "[r15" in args:
            m = re.search(r"\[r15 ([+-]) 0x([0-9a-f]+)\]", args)
            if m:
                disp = int(m[2], 16) * (-1 if m[1] == "-" else 1)
                if not -128 <= disp <= 127:
                    c[part + "_r15_disp32"] += 1
                else:
                    c[part + "_r15_disp8"] += 1
        if part == "hot":
            if "[r15" in args and "rip" not in args:
                kind = "ctx store " if re.match(r"\S+ ptr \[r15", args) else "ctx load/op "
                kind += "xmm" if "xmm" in args else "gpr"
            elif op.startswith("j"):
                kind = "branch " + ("rel32" if size >= 5 else "rel8" if size == 2 else "indirect")
            elif op.startswith("nop"):
                kind = "nop"
            elif "xmm" in args:
                kind = "simd/fp other"
            elif "[" in args:
                kind = "memory other"
            else:
                kind = "integer other"
            c["kind:" + kind] += size
        if (op == "jmp" or op.startswith("j")) and size >= 5 and re.match(r"0x[0-9a-f]+", args):
            target = int(args.split()[0], 16)
            if -128 <= target - (at + 2) <= 127:
                c[part + "_rel32_short_ok"] += 1
            c[part + "_rel32_branches"] += 1
        # unlinked tail: movabs rax; mov [r15+pc], rax; lea rax,[rip]; jmp [r15+stub_link]
        if op in ("mov", "movabs") and size == 10 and args.startswith("rax") and i + 3 < n and insts[i + 2][2] == "lea" and \
                insts[i + 3][2] == "jmp" and "[r15" in insts[i + 3][3]:
            tail = sum(insts[i + k][1] for k in range(4))
            c[part + "_tails"] += tail
            c[part + "_tail_count"] += 1
        i += 1
    return c


def main():
    profile, audit, out = sys.argv[1], str(pathlib.Path(sys.argv[2]).resolve()), pathlib.Path(sys.argv[3]).resolve()
    count = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4].isdigit() else 40
    env = dict(os.environ)
    for kv in sys.argv[4:]:
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v
    out.mkdir(parents=True, exist_ok=True)
    regions = hot_regions(profile, count)
    by_module = collections.defaultdict(list)
    for mod, rva, share, size in regions:
        by_module[mod].append(rva)
    info = {}
    for mod, rvas in by_module.items():
        path = module_path(mod)
        if not path:
            print("no module", mod)
            continue
        e = dict(env)
        e["TRANSMUTE_FX_AUDIT_DUMP"] = ",".join("%x" % r for r in rvas)
        text = subprocess.run([audit, str(path), str(out)], env=e, capture_output=True, text=True).stdout
        for line in text.splitlines():
            m = re.match(r"rva 0x([0-9a-f]+): (\d+) guest insns, (\d+) bytes(?:, hot (\d+))?", line)
            if m:
                info[(mod, int(m[1], 16))] = (int(m[2]), int(m[3]), int(m[4]) if m[4] else int(m[3]))
    total = collections.Counter()
    guest = 0
    for mod, rva, share, size in regions:
        if (mod, rva) not in info:
            continue
        insns, nbytes, hot = info[(mod, rva)]
        code = (out / ("r_%x.bin" % rva)).read_bytes()
        c = classify(code, hot)
        guest += insns * 4
        total.update(c)
        total["regions"] += 1
    print("regions %d, guest bytes %d" % (total["regions"], guest))
    allb = total["hot"] + total["cold"]
    print("bytes: total %d hot %d cold %d (expansion total %.2f, hot %.2f)" % (
        allb, total["hot"], total["cold"], allb / max(1, guest), total["hot"] / max(1, guest)))
    for part in ("hot", "cold"):
        print("%s: insts %d nop %d tails %d (%d) r15 disp32 %d disp8 %d rel32 branches %d (short-able %d)" % (
            part, total[part + "_insts"], total[part + "_nop"], total[part + "_tails"], total[part + "_tail_count"],
            total[part + "_r15_disp32"], total[part + "_r15_disp8"], total[part + "_rel32_branches"],
            total[part + "_rel32_short_ok"]))
    kinds = sorted(((v, k[5:]) for k, v in total.items() if k.startswith("kind:")), reverse=True)
    print("hot bytes by kind: " + ", ".join("%s %d" % (k, v) for v, k in kinds))


if __name__ == "__main__":
    main()
