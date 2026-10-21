"""Classify sampled x64 instructions without disassembling guest PE files.

Usage: python tools/fx_profile_hostmix.py profile.txt [number_of_regions]
Percentages describe RIP sample locations, not removable execution time.
"""
import collections
import pathlib
import re
import subprocess
import sys
import tempfile

profile = pathlib.Path(sys.argv[1])
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 24
llvm = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin")
regions = []
for line in pathlib.Path(str(profile) + ".regions.txt").read_text().splitlines():
    m = re.match(r"region (\d+) (\S+).* size (\d+) samples (\d+)", line)
    if m:
        region = dict(n=int(m[1]), module=m[2], size=int(m[3]), samples=int(m[4]), offsets={})
        if region["n"] <= limit:
            regions.append(region)
    elif line.startswith("  sample "):
        _, offset, count = line.split()
        region["offsets"][int(offset)] = int(count)

offsets = {}
with tempfile.TemporaryDirectory() as tmp:
    source = pathlib.Path(tmp) / "hot.s"
    cursor = 0
    with source.open("w") as stream:
        stream.write(".text\n")
        for region in regions:
            data = pathlib.Path(str(profile) + f'.r{region["n"]}.bin').read_bytes()
            region["base"] = cursor
            for offset, count in region["offsets"].items():
                offsets[cursor + offset] = count
            stream.write(f'\nregion{region["n"]}:\n')
            for start in range(0, len(data), 16):
                stream.write(".byte " + ",".join(map(str, data[start:start + 16])) + "\n")
            cursor += len(data)
    obj = pathlib.Path(tmp) / "hot.obj"
    subprocess.run([str(llvm / "clang.exe"), "--target=x86_64-pc-windows-msvc", "-c", str(source), "-o", str(obj)], check=True)
    asm = subprocess.check_output([str(llvm / "llvm-objdump.exe"), "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", str(obj)], text=True)

pathlib.Path(str(profile) + ".host.asm.txt").write_text(asm)
counts = collections.Counter()
matched = 0
hot = []
for line in asm.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
    if not m:
        continue
    at, op, args = int(m[1], 16), m[2], m[3]
    n = offsets.get(at, 0)
    if not n:
        continue
    matched += n
    if "[r15" in args:
        category = "context stores" if re.match(r"(?:xmmword|qword|dword|word|byte) ptr \[r15", args) and op.startswith(("mov", "vmov")) else "context reads and checks"
    elif op.startswith("j") or op in ("call", "ret"):
        category = "branches"
    elif op in ("vinsertps", "insertps"):
        category = "insertps"
    elif op.startswith(("set", "cmov")) or op in ("lahf", "sahf", "cmc"):
        category = "flag handling"
    elif "[" in args and "[rip" not in args and op != "lea":
        category = "other memory operands"
    elif re.search(r"\b[xyz]mm\d+\b", args) or op == "vzeroupper":
        category = "other SIMD and FP"
    else:
        category = "other integer and control"
    counts[category] += n
    hot.append((n, at, op, args))
total = sum(offsets.values())
print(f"Regions: {len(regions)}; samples in regions: {total}; decoded samples: {matched}")
for category, n in counts.most_common():
    print(f"{category:28} {n:8d} {100 * n / total:6.2f}%")
print("\nMost sampled instructions in concatenated host disassembly:")
for n, at, op, args in sorted(hot, reverse=True)[:30]:
    region = next(r for r in reversed(regions) if r["base"] <= at)
    print(f'{n:6d} r{region["n"]:2d}+0x{at-region["base"]:x} {op} {args}')
