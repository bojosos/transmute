"""Adds disassembly to fx_coverage --words output and groups it by mnemonic.

usage: python tools/fx_coverage_names.py <words.txt> [top]
"""
import collections, os, re, subprocess, sys, tempfile

LLVM = r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin"

def main():
    rows = []
    with open(sys.argv[1]) as f:
        for line in f:
            w, n = line.split()
            rows.append((int(w, 16), int(n)))
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "w.s")
        with open(src, "w") as f:
            f.write(".text\n")
            for w, _ in rows:
                f.write(f"  .inst 0x{w:08x}\n")
        obj = os.path.join(tmp, "w.obj")
        subprocess.run([os.path.join(LLVM, "clang.exe"), "--target=aarch64-pc-windows-msvc", "-c", src, "-o", obj], check=True)
        dis = subprocess.run([os.path.join(LLVM, "llvm-objdump.exe"), "-d", "--mattr=+all", obj],
                             capture_output=True, text=True).stdout
    text = []
    for line in dis.splitlines():
        m = re.match(r"^\s+[0-9a-f]+:\s+[0-9a-f]{8}\s+(.*)$", line)
        if m: text.append(m.group(1).strip())
    by_mnemonic = collections.Counter()
    for (w, n), t in zip(rows, text):
        by_mnemonic[t.split()[0] if t else "?"] += n
    total = sum(n for _, n in rows)
    print(f"{len(rows)} distinct unsupported encodings, {total} occurrences")
    print("by mnemonic:")
    for mnemonic, n in by_mnemonic.most_common(top):
        print(f"  {mnemonic:<14} {n}")
    print("top encodings:")
    for (w, n), t in list(zip(rows, text))[:top]:
        print(f"  {w:08x} {n:>9}  {t}")

main()
