"""annotate.py <profile.txt> <region#> [min_samples_window]

Host disassembly of a dumped hot region with per-instruction sample counts,
with the guest address of each fault site."""
import os, re, subprocess, sys, tempfile, bisect

LLVM = r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin"

prof, want = sys.argv[1], int(sys.argv[2])
regions = {}
cur = None
for line in open(prof + ".regions.txt"):
    m = re.match(r"region (\d+) (\S+) guest 0x([0-9a-f]+) module_base 0x([0-9a-f]+) host 0x([0-9a-f]+) size (\d+) samples (\d+)", line)
    if m:
        cur = dict(n=int(m[1]), module=m[2], guest=int(m[3], 16), base=int(m[4], 16), size=int(m[6]),
                   samples=int(m[7]), offs={}, faults=[])
        regions[cur["n"]] = cur
        continue
    parts = line.split()
    if parts and parts[0] == "sample":
        cur["offs"][int(parts[1])] = int(parts[2])
    elif parts and parts[0] == "fault":
        cur["faults"].append((int(parts[1]), int(parts[2]), int(parts[3], 16)))
r = regions[want]
data = open(prof + ".r%d.bin" % want, "rb").read()

with tempfile.TemporaryDirectory() as tmp:
    src = os.path.join(tmp, "a.s")
    with open(src, "w") as f:
        f.write(".text\nregion:\n")
        for i in range(0, len(data), 16):
            f.write("  .byte " + ",".join(str(b) for b in data[i:i + 16]) + "\n")
    obj = os.path.join(tmp, "a.obj")
    subprocess.run([os.path.join(LLVM, "clang.exe"), "--target=x86_64-pc-windows-msvc", "-c", src, "-o", obj], check=True)
    host = subprocess.run([os.path.join(LLVM, "llvm-objdump.exe"), "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", obj],
                          capture_output=True, text=True, check=True).stdout

faults = sorted(r["faults"])
fault_starts = {f[0]: f for f in faults}
total = sum(r["offs"].values())
print("region %d %s guest +0x%x size %d samples %d" % (want, r["module"], r["guest"] - r["base"], r["size"], r["samples"]))
last_gpc = None
for line in host.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
    if not m:
        continue
    off = int(m[1], 16)
    if off in fault_starts:
        gpc = fault_starts[off][2]
        if last_gpc is not None and last_gpc < gpc <= last_gpc + 64:
            pcs = range(last_gpc + 4, gpc + 4, 4)
        else:
            pcs = [gpc]
        for pc in pcs:
            print("            ; guest +%x" % (pc - r["base"]))
        last_gpc = gpc
    c = r["offs"].get(off, 0)
    pct = "%5.1f%%" % (100.0 * c / total) if c else "      "
    print("%s %5s  %5x  %s" % (pct, c if c else "", off, m[2]))
