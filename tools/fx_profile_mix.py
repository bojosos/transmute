"""mix.py <profile.txt> <count>: sample-weighted and static host instruction mix of the hottest regions."""
import os, re, subprocess, sys, collections
here = os.path.dirname(os.path.abspath(__file__))
prof, count = sys.argv[1], int(sys.argv[2])
cats = collections.Counter()
static = collections.Counter()
total = 0
nstatic = 0
for n in range(1, count + 1):
    out = subprocess.run([sys.executable, os.path.join(here, "fx_profile_annotate.py"), prof, str(n)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"^\s*(?:(\d+\.\d)%\s+(\d+))?\s+([0-9a-f]+)\s+(\S+)\s*(.*)$", line)
        if not m or line.lstrip().startswith(";"):
            continue
        c = int(m[2]) if m[2] else 0
        op, args = m[4], m[5]
        if "r15" in args and ("ptr [r15" in args):
            k = "ctx-vec" if op.startswith(("movdqu", "vmovdqu", "movups", "vmovups")) else "ctx-gpr" if op == "mov" else "ctx-other"
            if op == "cmp" and "0xac" in args:
                k = "interrupt-check"
        elif op in ("vinsertps", "insertps"):
            k = "insertps(zero-upper)"
        elif op in ("ucomiss", "ucomisd", "comiss", "comisd"):
            k = "fcmp"
        elif op.startswith("set") or op.startswith("cmov") or op in ("lahf", "sahf", "cmc", "seto"):
            k = "flags-materialize"
        elif op.startswith("nop"):
            k = "nop"
        elif op.startswith("j"):
            k = "branch"
        elif op.startswith("v") or op.startswith("p") or op.startswith("movd") or op.startswith("movq"):
            k = "vector/fp op"
        else:
            k = "gpr op"
        cats[k] += c
        total += c
        static[k] += 1
        nstatic += 1
print("%-24s %8s %8s" % ("category", "samples%", "static%"))
for k, v in sorted(cats.items(), key=lambda kv: -kv[1]):
    print("%-24s %7.1f%% %7.1f%%" % (k, 100.0 * v / total, 100.0 * static[k] / nstatic))
