"""Where translated code spends its samples: an fx sampling profile (TRANSMUTE_FX_SAMPLE) split into
buckets (docs/FX_PERF.md, "Profiling").

usage: python -I profile_mix.py <profile.txt> [--image <guest Mach-O> ...] [--regions N] [--top 25]

* Host buckets: the profile's host symbols grouped by subsystem (dispatcher and lookup, synthetic calls into
  Alchemy, translation, waits, ...).
* Translated-code buckets: every sampled host instruction of the hottest dumped regions (<profile>.r<N>.bin, so
  N <= 24) classified as context access (r15-relative: guest registers kept in the context, spills and reloads),
  flag handling, atomics (lock-prefixed or xchg), indirect branches and exits, other branches, guest memory
  operands, SIMD/FP, or other integer work.
* With --image (a guest Mach-O), each region's guest start is named by the nearest preceding symbol (llvm-nm), and
  the instruction mix is also given per symbol.
Percentages are RIP sample locations, not removable time: a stalled instruction collects the samples of its
producers' latency.
"""
import argparse
import bisect
import collections
import pathlib
import re
import subprocess
import sys
import tempfile

LLVM = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin")

HOST_BUCKETS = [
    ("waits (ntdll waits, sleeps)", r"ntdll\.dll!(Nt|Zw)(WaitFor|Delay|RemoveIoCompletion|WaitForAlert)|NtWaitForSingleObject|NtWaitForMultipleObjects"),
    ("synthetic calls into Alchemy (invoke_synthetic, state copies, handler dispatch)",
     r"invoke_synthetic|store_state|load_state|host_callback|_Func_impl|Ref_count_obj2<alchemy::ThreadContext>|svc_hook|GuestCallHandler"),
    ("Alchemy providers (the called library code itself)", r"alchemy::|`alchemy::|services_detail|DarwinServices|ObjcRuntime"),
    ("guest memory helpers (guarded copies, string reads)", r"guarded_|read_bytes|copy_bytes|fill_bytes|string_length|write_bytes"),
    ("fx dispatcher, lookup, links (rt::)", r"fx::rt::|dispatch|lookup|redispatch|link_exit|unlink"),
    ("fx translation (translate_region, plan, emit, install)", r"translate|build_plan|RegionEmitter|emit_|install_region|isel|tier"),
    ("fx cache", r"cache"),
    ("C runtime (memmove, memset, heap)", r"VCRUNTIME|ucrtbase|memmove|memset|memcpy|malloc|free|Heap"),
    ("kernel calls (other ntdll)", r"ntdll\.dll"),
]


def host_bucket(sym):
    for name, pattern in HOST_BUCKETS:
        if re.search(pattern, sym):
            return name
    return "other host code"


def classify(op, args):
    if op.startswith("lock") or (op == "xchg" and "[" in args) or op in ("cmpxchg", "cmpxchg16b", "xadd"):
        return "atomics (lock, xchg)"
    if "[r15" in args:
        return "context access (guest regs in memory, spills, reloads)"
    if op in ("jmp", "call") and not re.match(r"0x[0-9a-f]+\s*(<|$)", args):
        return "indirect branches, calls and exits"
    if op.startswith("j") or op in ("call", "ret"):
        return "direct branches"
    if op.startswith(("set", "cmov")) or op in ("lahf", "sahf", "cmc", "pushfq", "popfq", "adc", "sbb", "rcl", "rcr"):
        return "flag handling"
    if re.search(r"\b[xyz]mm\d+\b", args) or op == "vzeroupper":
        return "SIMD and FP"
    if "[" in args and "[rip" not in args and op != "lea":
        return "guest memory operands"
    return "other integer"


def load_symbols(images):
    syms = []  # (address, name)
    for image in images:
        out = subprocess.run([str(LLVM / "llvm-nm.exe"), "--defined-only", "-n", image], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r"([0-9a-f]+) [tT] (\S+)", line)
            if m:
                syms.append((int(m[1], 16), m[2].lstrip("_")))
    syms.sort()
    return syms


def name_of(syms, address):
    if not syms:
        return None
    i = bisect.bisect_right([a for a, _ in syms], address) - 1
    return syms[i][1] if i >= 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("--image", action="append", default=[])
    ap.add_argument("--regions", type=int, default=24)
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    profile = pathlib.Path(args.profile)
    text = profile.read_text(errors="replace")
    total = int(re.search(r"fx sampling profile: (\d+) samples", text)[1])
    head = re.search(r"translated ([0-9.]+)%\s+stubs ([0-9.]+)%\s+arena-unknown ([0-9.]+)%\s+host ([0-9.]+)%", text)
    print(f"# {profile.name}: {total} samples; translated {head[1]} %, stubs {head[2]} %, arena-unknown {head[3]} %, host {head[4]} %\n")

    # Host buckets.
    section = re.search(r"== host symbols.*?\n(.*?)\n\n", text, re.S)
    buckets = collections.Counter()
    examples = collections.defaultdict(list)
    for line in (section[1].splitlines() if section else []):
        m = re.match(r"\s*([0-9.]+)%\s+(\d+)\s+(.*)", line)
        if m:
            b = host_bucket(m[3])
            buckets[b] += int(m[2])
            if len(examples[b]) < 3:
                examples[b].append(re.sub(r"<.*", "", m[3].split("!")[-1])[:60])
    print("| Host bucket (of all samples) | % | top symbols |\n| --- | ---: | --- |")
    for b, n in buckets.most_common():
        print(f"| {b} | {100 * n / total:.1f} | {', '.join(examples[b])} |")

    # Regions and their sampled host instructions.
    syms = load_symbols(args.image)
    regions = []
    rt = pathlib.Path(str(profile) + ".regions.txt")
    for line in rt.read_text().splitlines():
        m = re.match(r"region (\d+) (.+?) guest (0x[0-9a-f]+) module_base (0x[0-9a-f]+) .* size (\d+) samples (\d+)", line)
        if m:
            region = dict(n=int(m[1]), module=m[2], guest=int(m[3], 16), base=int(m[4], 16), size=int(m[5]), samples=int(m[6]), offsets={})
            regions.append(region)
        elif line.startswith("  sample ") and regions:
            _, off, cnt = line.split()
            regions[-1]["offsets"][int(off)] = int(cnt)
    dumped = [r for r in regions if r["n"] <= args.regions and pathlib.Path(str(profile) + f".r{r['n']}.bin").exists()]
    mix = collections.Counter()
    per_sym = collections.defaultdict(collections.Counter)
    hot = []
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "hot.s"
        cursor = 0
        offsets = {}
        with src.open("w") as s:
            s.write(".text\n")
            for r in dumped:
                data = pathlib.Path(str(profile) + f".r{r['n']}.bin").read_bytes()
                r["at"] = cursor
                for off, cnt in r["offsets"].items():
                    offsets[cursor + off] = (cnt, r)
                s.write(f"region{r['n']}:\n")
                for i in range(0, len(data), 16):
                    s.write(".byte " + ",".join(map(str, data[i:i + 16])) + "\n")
                cursor += len(data)
        obj = pathlib.Path(tmp) / "hot.obj"
        subprocess.run([str(LLVM / "clang.exe"), "--target=x86_64-pc-windows-msvc", "-c", str(src), "-o", str(obj)], check=True)
        asm = subprocess.check_output([str(LLVM / "llvm-objdump.exe"), "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", str(obj)], text=True)
    decoded = 0
    for line in asm.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
        if not m:
            continue
        at, op, a = int(m[1], 16), m[2], m[3]
        if op in ("lock", "rep", "repne") and a:
            op, a = op + " " + a.split()[0], " ".join(a.split()[1:])
        hit = offsets.get(at)
        if not hit:
            continue
        cnt, r = hit
        cat = classify(op, a)
        mix[cat] += cnt
        decoded += cnt
        sym = name_of(syms, r["guest"] - r["base"] + (0x100000000 if r["base"] else 0)) if syms else None
        per_sym[sym or r["module"]][cat] += cnt
        hot.append((cnt, r["n"], at - r["at"], op, a))
    in_regions = sum(r["samples"] for r in dumped)
    print(f"\n{len(dumped)} dumped regions hold {in_regions} samples ({100 * in_regions / total:.1f} % of all); {decoded} decoded.\n")
    print("| Translated-code bucket (of decoded region samples) | % |\n| --- | ---: |")
    for c, n in mix.most_common():
        print(f"| {c} | {100 * n / max(decoded, 1):.1f} |")
    if syms:
        cats = [c for c, _ in mix.most_common()]
        print("\n| Guest symbol | samples | " + " | ".join(cats) + " |\n| --- | ---: | " + " | ".join("---:" for _ in cats) + " |")
        for sym, c in sorted(per_sym.items(), key=lambda kv: -sum(kv[1].values())):
            n = sum(c.values())
            print(f"| {sym} | {n} | " + " | ".join(f"{100 * c[k] / n:.0f}" for k in cats) + " |")
    print(f"\nMost sampled host instructions (region+offset):")
    for cnt, n, off, op, a in sorted(hot, reverse=True)[:args.top]:
        print(f"  {cnt:6d} r{n}+0x{off:x}  {op} {a}")


if __name__ == "__main__":
    main()
