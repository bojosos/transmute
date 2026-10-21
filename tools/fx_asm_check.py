"""Checks the fx x64 assembler: disassembles the bytes produced by
fx_assembler_dump with llvm-objdump and compares them with the intended text.

usage: python tools/fx_asm_check.py <fx_assembler_dump.exe>
"""
import os, re, subprocess, sys, tempfile

LLVM = r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin"

def norm(text):
    text = text.lower().strip()
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"\s*([,\[\]+\-*])\s*", r"\1", text)
    text = re.sub(r"0x0*([0-9a-f])", r"0x\1", text)
    return text

def main():
    out = subprocess.run([sys.argv[1]], capture_output=True, text=True, check=True).stdout
    cases = [line.split("|", 1) for line in out.splitlines() if "|" in line]
    failures = 0
    asm_cases = []
    for text, hexbytes in cases:
        if text.startswith("HEX:"):
            if text[4:] != hexbytes:
                print(f"FAIL hex: expected {text[4:]} got {hexbytes}"); failures += 1
        else:
            asm_cases.append((text, hexbytes))
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "a.s")
        with open(src, "w") as f:
            f.write(".intel_syntax noprefix\n.text\n")
            for i, (text, hexbytes) in enumerate(asm_cases):
                f.write(f"case_{i}:\n")
                f.write("  .byte " + ",".join("0x" + hexbytes[j:j+2] for j in range(0, len(hexbytes), 2)) + "\n")
            f.write("case_end: nop\n")
        obj = os.path.join(tmp, "a.obj")
        subprocess.run([os.path.join(LLVM, "clang.exe"), "--target=x86_64-pc-windows-msvc", "-c", src, "-o", obj], check=True)
        dis = subprocess.run([os.path.join(LLVM, "llvm-objdump.exe"), "-d", "--no-show-raw-insn",
                              "--x86-asm-syntax=intel", obj], capture_output=True, text=True, check=True).stdout
    blocks = {}
    current = None
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <case_(\d+)>:", line)
        if m: current = int(m.group(1)); blocks[current] = []; continue
        if re.match(r"^[0-9a-f]+ <case_end>:", line): current = None; continue
        m = re.match(r"^\s+[0-9a-f]+:\s+(.*)$", line)
        if m and current is not None:
            text = m.group(1).split("#")[0].strip()
            if blocks[current] and blocks[current][-1] == "lock":
                blocks[current][-1] = "lock " + text
            else:
                blocks[current].append(text)
    for i, (text, hexbytes) in enumerate(asm_cases):
        got = blocks.get(i, [])
        if len(got) != 1 or norm(got[0]) != norm(text):
            print(f"FAIL: expected '{text}' got {got} bytes={hexbytes}"); failures += 1
    print(f"{len(cases) - failures}/{len(cases)} encodings ok")
    return 1 if failures else 0

sys.exit(main())
