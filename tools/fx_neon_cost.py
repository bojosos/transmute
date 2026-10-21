"""fx_neon_cost.py: SIMD&FP lowering cost table (owner: neon/fp).

  python tools/fx_neon_cost.py --tool build-fx-simd/Release/fx_neon_cost.exe
         [--before-env "TRANSMUTE_FX_NEON=oldshl,nozero64;TRANSMUTE_FX_FP=oldimm,oldmask,oldcvt"]
         [--out artifacts/fx-neon-cost] [--top 40]

Steps (cached in --out):
 1. supported.txt: `fx_neon_cost enumerate` (every SIMD&FP encoding with fixed
    registers that translates without an interpret exit).
 2. Form keys: llvm-objdump names every word; registers/immediates are
    normalized ("fmax .4s,.4s,.4s"). One representative per form, with
    distinct registers.
 3. Costs: `fx_neon_cost cost` translates each representative as a
    one-instruction region (unpinned: operands in GuestContext; the NOP
    region's exit sequence is subtracted) and as `L: insn; B L` (pinned:
    loop body only). Host instructions along the common path (forward
    branches followed: fix-ups sit behind them), NOPs not counted. llvm-mca
    (-mcpu=alderlake, Golden Cove) estimates uops, reciprocal throughput and
    a one-iteration latency for the pinned body.
 4. `ideal`: host instructions of a hand-written AVX2/FMA3 sequence with
    ARM-exact semantics, operands in registers (IDEAL below; estimates).
Output: table.txt sorted by the excess of the effective cost over ideal, and
forms.json with everything.
"""
import argparse, json, os, re, subprocess, sys, tempfile

LLVM = r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin"
CLANG = os.path.join(LLVM, "clang.exe")
OBJDUMP = os.path.join(LLVM, "llvm-objdump.exe")
MCA = os.path.join(LLVM, "llvm-mca.exe")
# ---- ARM64 disassembly and form keys --------------------------------------------------

def disasm_arm(words):
    """word -> llvm-objdump text (mnemonic + operands)."""
    words = list(words)
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        src, obj = os.path.join(tmp, "a.s"), os.path.join(tmp, "a.o")
        with open(src, "w") as f:
            f.write(".text\n")
            for w in words:
                f.write(".inst 0x%08x\n" % w)
        subprocess.run([CLANG, "--target=aarch64-pc-windows-msvc", "-c", src, "-o", obj], check=True)
        p = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", "--mattr=+v8.6a,+fullfp16,+fp16fml,+dotprod,"
                            "+rdm,+sha3,+sm4,+aes,+sha2,+crc,+lse,+rcpc,+frint3264,+jsconv,+i8mm,+bf16", obj],
                           capture_output=True, text=True, check=True)
        for line in p.stdout.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", line)
            if not m:
                continue
            i = int(m[1], 16) // 4
            if i < len(words):
                out[words[i]] = m[2].split("//")[0].strip()
    return out

REG = [(re.compile(r"\b0x[0-9a-f]+\s*<[^>]*>"), "lit"), (re.compile(r"\bv\d+\.(\d*[bhsdq])\b"), r".\1"), (re.compile(r"\bv\d+\b"), "v"),
       (re.compile(r"\b([bhsdq])\d+\b"), r"\1"), (re.compile(r"\b(w|x)(\d+|zr)\b"), r"\1"),
       (re.compile(r"\bwsp\b"), "w"), (re.compile(r"\bsp\b"), "x"),
       (re.compile(r"#-?0x[0-9a-f]+|#-?\d+(\.\d+)?(e[+-]?\d+)?"), "#i"), (re.compile(r"\[\d+\]"), "[i]"),
       (re.compile(r"\s+"), "")]

def form_key(text):
    if not text or text.startswith("<unknown>") or text.startswith("udf"):
        return None
    parts = text.split(None, 1)
    ops = parts[1] if len(parts) > 1 else ""
    for r, s in REG:
        ops = r.sub(s, ops)
    return parts[0] + " " + ops

def reg_numbers(text):
    return re.findall(r"\b[vbhsdqwx](\d+)\b", text)

# ---- x64 side: disassembly, common path, llvm-mca -----------------------------------------

def disasm_x64(blobs):
    """blobs: {name: bytes} -> {name: [(offset, mnemonic, operands)]}"""
    out = {}
    names = list(blobs)
    with tempfile.TemporaryDirectory() as tmp:
        src, obj = os.path.join(tmp, "a.s"), os.path.join(tmp, "a.o")
        with open(src, "w") as f:
            for i, n in enumerate(names):
                f.write(".section .text$b%d,\"xr\"\n.globl B%d\nB%d:\n" % (i, i, i))
                b = blobs[n]
                for j in range(0, len(b), 32):
                    f.write("  .byte " + ",".join(str(x) for x in b[j:j + 32]) + "\n")
        subprocess.run([CLANG, "--target=x86_64-pc-windows-msvc", "-c", src, "-o", obj], check=True)
        p = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", obj],
                           capture_output=True, text=True, check=True)
        cur = None
        for line in p.stdout.splitlines():
            m = re.match(r"^[0-9a-f]+ <B(\d+)>:", line)
            if m:
                cur = out.setdefault(names[int(m[1])], [])
                continue
            m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
            if m and cur is not None:
                cur.append((int(m[1], 16), m[2], m[3].split("#")[0].strip()))
    return out

def walk(insns, start):
    """Common path from `start`: follows forward jumps, stops at a backward
    jmp (loop edge), an indirect jmp or ret. Returns (instructions, back_target)."""
    at = {o: i for i, (o, _, _) in enumerate(insns)}
    i = at.get(start)
    path, seen = [], set()
    while i is not None and i < len(insns) and i not in seen and len(path) < 2000:
        seen.add(i)
        off, mn, ops = insns[i]
        if mn == "ret" or (mn == "jmp" and "ptr" in ops):
            path.append(insns[i])
            return path, None
        if mn.startswith("j"):
            m = re.match(r"(0x[0-9a-f]+)", ops)
            tgt = int(m[1], 16) if m else None
            path.append(insns[i])
            if tgt is not None and tgt < off and mn == "jmp":
                return path, tgt
            if tgt is not None and tgt > off:
                i = at.get(tgt)
                continue
        else:
            path.append(insns[i])
        i += 1
    return path, None

def is_nop(mn):
    return mn.startswith("nop") or mn == "int3"

def mca(regions):
    """regions: {name: [asm lines]} -> {name: (uops, rthroughput, latency)}"""
    res = {}
    names = [n for n in regions if regions[n]]
    if not names:
        return res
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "a.s")
        with open(src, "w") as f:
            f.write(".intel_syntax noprefix\n")
            for i, n in enumerate(names):
                f.write("# LLVM-MCA-BEGIN R%d\n" % i)
                for l in regions[n]:
                    f.write(l + "\n")
                f.write("# LLVM-MCA-END\n")
        tp = subprocess.run([MCA, "-mtriple=x86_64-pc-windows-msvc", "-mcpu=alderlake", "-iterations=100",
                             "-skip-unsupported-instructions=any", src], capture_output=True, text=True)
        lt = subprocess.run([MCA, "-mtriple=x86_64-pc-windows-msvc", "-mcpu=alderlake", "-iterations=1",
                             "-timeline", "-timeline-max-cycles=400", "-skip-unsupported-instructions=any",
                             src], capture_output=True, text=True)
    def per_region(text):
        blocks = re.split(r"\[(\d+)\] Code Region - R(\d+)", text)
        d = {}
        for k in range(1, len(blocks) - 2, 3):
            d[int(blocks[k + 1])] = blocks[k + 2]
        return d
    t1, t2 = per_region(tp.stdout), per_region(lt.stdout)
    for i, n in enumerate(names):
        b = t1.get(i, "")
        uops = re.search(r"Total uOps:\s+(\d+)", b)
        rt = re.search(r"Block RThroughput:\s+([\d.]+)", b)
        lat = None
        tl = t2.get(i, "")
        first_d, last_e = None, None
        for line in tl.splitlines():
            m = re.match(r"\[0,\d+\]\s+(\S+)", line)
            if m:
                s = line[line.index("]") + 1:]
                s = s.lstrip(" ")
                col = line.index(s) if s else 0
                row = line[col:col + 400]
                cells = re.match(r"([.=DeER\-]+)", row.replace(" ", "."))
                if not cells:
                    continue
                c = cells[1]
                if "D" in c:
                    first_d = c.index("D") if first_d is None else min(first_d, c.index("D"))
                if "E" in c:
                    last_e = max(last_e or 0, c.index("E"))
        if first_d is not None and last_e is not None:
            lat = last_e - first_d
        res[n] = (int(uops[1]) // 100 if uops else None, float(rt[1]) if rt else None, lat)
    return res

def mca_line(mn, ops):
    if mn.startswith("j") or is_nop(mn):
        return None
    ops = ops.replace("xmmword ptr", "xmmword ptr").replace("rip + ", "rip+")
    return (mn + " " + ops).strip()

# ---- ideal hand-written x64 (AVX2/FMA3/BMI2, ARM-exact, operands in registers) ----------

def ideal(key):
    """Host instructions of a hand-written AVX2/FMA3 sequence with ARM-exact
    semantics (FPCR rounding/FZ, NaN rules of NOTES_FP.md, saturation), operands
    in registers, perfect knowledge of zero upper bits, constants in memory,
    rarely taken fix-ups behind a branch (common path counted). Estimates."""
    mn, ops = key.split(" ", 1) if " " in key else (key, "")
    arr = re.findall(r"\.(\d*[bhsdq])", ops)
    a = arr[0] if arr else ""
    first = ops.split(",")[0] if ops else ""
    scalar = not arr and not ops.startswith("{")
    q = a in ("16b", "8h", "4s", "2d")
    d = a in ("2d", "1d") or (scalar and ops.startswith("d"))
    b = a in ("8b", "16b")
    elem = "[i]" in ops
    vec_elem = elem and first.startswith(".") and mn not in ("mov", "ins", "dup", "umov", "smov", "ld1", "st1",
                                                               "ld2", "st2", "ld3", "st3", "ld4", "st4", "ld1r")
    mem = "[x" in ops or "lit" in ops
    wb = mem and ("]!" in ops or re.search(r"\],(#i|x)", ops) is not None)
    nregs = ops.count(".") if ops.startswith("{") else 1
    if mem:
        if mn in ("ldp", "stp", "ldnp", "stnp"):
            base = 2
        elif mn in ("ld1", "st1") and elem:
            base = 1
        elif mn in ("ld1", "st1", "ld1r"):
            base = 1 if mn == "ld1r" else nregs
        elif mn in ("ld2", "st2"):
            base = 4
        elif mn in ("ld3", "st3", "ld4", "st4", "ld2r", "ld3r", "ld4r"):
            base = 8
        else:
            base = 1
            if re.search(r"w,sxtw|w,uxtw", ops):
                base = 2
        return base + (1 if wb else 0)
    if mn in ("fmax", "fmaxp"):
        base = 5
    elif mn in ("fmin", "fminp"):
        base = 3
    elif mn in ("fmaxnm", "fminnm", "fmaxnmp", "fminnmp"):
        base = 7
    elif mn in ("fmaxv", "fmaxnmv", "fminnmv"):
        base = 12
    elif mn == "fminv":
        base = 8
    elif mn in ("fcvtzs", "fcvtns", "fcvtms", "fcvtps"):
        base = 3 if first[:1] in ("w", "x") else 4      # cvt(t) + range check + jcc
        if mn != "fcvtzs" and first[:1] not in ("w", "x"):
            base += 1                                   # vroundps
    elif mn == "fcvtas":
        base = 9
    elif mn in ("fcvtzu", "fcvtnu", "fcvtmu", "fcvtpu", "fcvtau"):
        base = 6 if first[:1] in ("w", "x") else 8
    elif mn == "ucvtf":
        base = 5 if a else 3                            # vector: srl/and/cvt/cvt/fma; scalar: zero-ext + xor + cvt
    elif mn == "scvtf":
        base = 1 if a else 2
    elif mn in ("frecpe", "frsqrte"):
        base = 12
    elif mn == "frecps":
        base = 1
    elif mn == "frsqrts":
        base = 2
    elif mn == "frinta":
        base = 7
    elif mn in ("bsl", "bit", "bif"):
        base = 3
    elif mn == "orn":
        base = 2
    elif mn in ("cmge", "cmhs") and "#" not in ops:
        base = 2
    elif mn == "cmhi":
        base = 3
    elif mn == "cmtst":
        base = 3
    elif mn in ("cmle", "cmlt", "cmge", "cmgt", "cmeq") and "#" in ops:
        base = 1 if (mn == "cmlt" and a in ("4s", "2s", "8h", "4h")) else 2
    elif mn == "neg":
        base = 2
    elif mn in ("not", "mvn"):
        base = 1
    elif mn in ("uzp1", "uzp2", "trn1", "trn2"):
        base = 1 if a == "2d" else 2
    elif mn in ("zip1", "zip2"):
        base = 1 if q else 2
    elif mn in ("xtn2", "uxtl2", "sxtl2", "ushll2", "sshll2", "fcvtl2", "fcvtn2", "shll", "shll2"):
        base = 2
    elif mn in ("addhn", "subhn", "raddhn", "rsubhn", "addhn2", "subhn2"):
        base = 3
    elif mn == "addp":
        base = 1 if (a in ("8h", "4s") or scalar) else 2
    elif mn == "faddp":
        base = 1 if q else 2
    elif mn == "addv":
        base = {"16b": 4, "8b": 3, "8h": 4, "4h": 3, "4s": 4}.get(a, 3)
    elif mn in ("umaxv", "uminv", "smaxv", "sminv"):
        base = 4
    elif mn in ("uaddlv", "saddlv"):
        base = 3
    elif mn in ("smull", "umull", "smull2", "umull2", "smlal", "umlal", "smlal2", "umlal2", "smlsl", "umlsl"):
        base = 3 + (1 if ("mlal" in mn or "mlsl" in mn) else 0)
    elif mn in ("sqdmulh", "sqrdmulh"):
        base = 3 if (a in ("8h", "4h") or first.startswith("h")) else 7
    elif mn in ("ushl", "sshl"):
        base = 6 if a in ("4s", "2s") else 10
    elif mn == "tbl":
        base = 2 + 3 * max(0, nregs_tbl(ops) - 1)
    elif mn == "tbx":
        base = 4 + 3 * max(0, nregs_tbl(ops) - 1)
    elif mn == "cnt":
        base = 6
    elif mn in ("sqxtn", "uqxtn", "sqxtun"):
        base = 2 if not a.endswith("s") else 4
    elif mn in ("sqxtn2", "uqxtn2", "sqxtun2"):
        base = 3 if not a.endswith("s") else 5
    elif mn == "fcsel":
        base = 4                                        # movd, movd, cmov, movd
    elif mn in ("fccmp", "fccmpe"):
        base = 6
    elif mn in ("fcmp", "fcmpe"):
        base = 2 if "#" in ops else 1
    elif mn == "dup" and ops.split(",")[-1][:1] in ("w", "x") and not elem:
        base = 2                                        # movd + vpbroadcast
    elif mn in ("uqadd", "uqsub") and (a in ("4s", "2s", "2d") or scalar):
        base = 3
    elif mn in ("sqadd", "sqsub") and (a in ("4s", "2s", "2d") or scalar):
        base = 6
    elif mn == "mul" and b:
        base = 5
    elif mn in ("abs", "smax", "smin", "umax", "umin") and d:
        base = 3
    elif mn in ("shl", "ushr", "sshr") and b:
        base = 2
    elif mn in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
        base = 2                                        # x64 FMA is destructive: copy of the addend into Vd
    elif mn in ("mla", "mls", "fabd", "fnmul", "usra", "ssra"):
        base = 2
    elif mn in ("facge", "facgt"):
        base = 3
    else:
        base = 1
    return base + (1 if vec_elem else 0)

def nregs_tbl(ops):
    m = re.match(r"[^{]*\{([^}]*)\}", ops)
    return m[1].count(".") if m else 1

# ---- main ---------------------------------------------------------------------------------

def run_cost(tool, words, env_extra, path):
    if os.path.exists(path):
        return path
    wf = path + ".words"
    with open(wf, "w") as f:
        for w in words:
            f.write("%08x\n" % w)
    env = dict(os.environ)
    for kv in env_extra:
        k, v = kv.split("=", 1)
        env[k] = v
    with open(path, "w") as f:
        subprocess.run([tool, "cost", wf], stdout=f, env=env, check=True)
    return path

def parse_costs(path):
    rows = {}
    for line in open(path):
        p = line.split()
        w, mode = int(p[0], 16), p[1]
        if mode in ("baseline", "unpinned"):
            rows[(w, mode)] = dict(entry=int(p[2]), interp=int(p[3]), code=bytes.fromhex(p[4]) if len(p) > 4 else b"")
            if mode == "unpinned" and int(p[3]) != 0:
                rows.pop((w, mode))   # interpreted: not supported
        else:
            rows[(w, mode)] = dict(entry=int(p[2]), interp=int(p[3]), vpins=int(p[4]), gpins=int(p[5]),
                                   code=bytes.fromhex(p[6]) if len(p) > 6 else b"")
    return rows

def measure(rows):
    blobs = {k: v["code"] for k, v in rows.items()}
    dis = disasm_x64(blobs)
    base_path, _ = walk(dis[(0xd503201f, "baseline")], rows[(0xd503201f, "baseline")]["entry"])
    base_n = sum(1 for _, mn, _ in base_path if not is_nop(mn))
    out, mca_in = {}, {}
    for (w, mode), r in rows.items():
        if mode == "baseline":
            continue
        ins = dis.get((w, mode), [])
        path, back = walk(ins, r["entry"])
        if mode in ("pinned", "pinz", "pinzbase"):
            if back is None:
                continue
            seq = [x for x in path if x[0] >= back][:-1]   # loop body without the back edge
            n = sum(1 for _, mn, _ in seq if not is_nop(mn))
            if mode == "pinned":
                mca_in[w] = [l for l in (mca_line(mn, ops) for _, mn, ops in seq) if l]
            out[(w, mode)] = dict(n=n, asm=["%s %s" % (mn, ops) for _, mn, ops in seq if not is_nop(mn)])
        else:
            n = sum(1 for _, mn, _ in path if not is_nop(mn)) - base_n
            out[(w, mode)] = dict(n=n, asm=["%s %s" % (mn, o) for _, mn, o in path if not is_nop(mn)][:max(0, n)])
    m = mca(mca_in)
    for w, v in m.items():
        out[(w, "pinned")].update(uops=v[0], rthru=v[1], lat=v[2])
    # Known-zero-upper sources: loop with the word minus the same loop without it.
    for (w, mode) in list(out):
        if mode == "pinz" and (w, "pinzbase") in out:
            out[(w, "pinz")]["n"] -= out[(w, "pinzbase")]["n"]
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--out", default=r"C:\dev\transmute\artifacts\fx-neon-cost")
    ap.add_argument("--before-env", default="TRANSMUTE_FX_NEON=oldshl,nozero64;TRANSMUTE_FX_FP=oldimm,oldmask,oldcvt")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--fresh", action="store_true", help="recompute costs (keep supported/forms)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    sup = os.path.join(args.out, "supported.txt")
    if not os.path.exists(sup):
        with open(sup, "w") as f:
            subprocess.run([args.tool, "enumerate"], stdout=f, check=True)
    supported = [int(l, 16) for l in open(sup, encoding="utf-8-sig") if l.strip()]

    # Names and form keys.
    keys_path = os.path.join(args.out, "keys.json")
    if os.path.exists(keys_path):
        names = {int(k): v for k, v in json.load(open(keys_path)).items()}
    else:
        names = {}
    todo = [w for w in supported if w not in names]
    for i in range(0, len(todo), 400000):
        names.update(disasm_arm(todo[i:i + 400000]))
    json.dump({str(k): v for k, v in names.items()}, open(keys_path, "w"))

    forms = {}
    for w in supported:
        k = form_key(names.get(w))
        if not k:
            continue
        f = forms.setdefault(k, dict(words=[]))
        f["words"].append(w)
    # Representative: distinct registers, Rd = 0, Rm = 2 preferred.
    for k, f in forms.items():
        def score(w):
            t = names[w]
            regs = reg_numbers(t)
            return (len(set(regs)) == len(regs), (w & 31) == 0, ((w >> 16) & 31) == 2, -w)
        f["rep"] = max(f["words"], key=score)
        f["text"] = names[f["rep"]]
        del f["words"]

    reps = sorted(f["rep"] for f in forms.values())
    after_path = os.path.join(args.out, "cost_after.txt")
    before_path = os.path.join(args.out, "cost_before.txt")
    if args.fresh:
        for p in (after_path, before_path):
            if os.path.exists(p):
                os.remove(p)
    run_cost(args.tool, reps, [], after_path)
    run_cost(args.tool, reps, [kv for kv in args.before_env.split(";") if kv], before_path)
    after = measure(parse_costs(after_path))
    before = measure(parse_costs(before_path))

    table = []
    for k, f in forms.items():
        w = f["rep"]
        a_p, a_u = after.get((w, "pinned")), after.get((w, "unpinned"))
        b_p, b_u = before.get((w, "pinned")), before.get((w, "unpinned"))
        if not a_p or not b_p or not a_u:
            continue
        a_z, b_z = after.get((w, "pinz")), before.get((w, "pinz"))
        f.update(after_pinz=a_z["n"] if a_z else a_p["n"], before_pinz=b_z["n"] if b_z else b_p["n"])
        idl = ideal(k)
        f.update(ideal=idl, after_pinned=a_p["n"], after_unpinned=a_u["n"] if a_u else None,
                 before_pinned=b_p["n"], before_unpinned=b_u["n"] if b_u else None,
                 uops=a_p.get("uops"), rthru=a_p.get("rthru"), lat=a_p.get("lat"),
                 before_uops=b_p.get("uops"), before_lat=b_p.get("lat"),
                 asm=a_p["asm"], before_asm=b_p["asm"])
        # Effective cost: mean of unknown-upper (loop header) and known-zero sources.
        f["before_eff"] = (f["before_pinned"] + f["before_pinz"]) / 2.0
        f["after_eff"] = (f["after_pinned"] + f["after_pinz"]) / 2.0
        f["score_before"] = max(0.0, f["before_eff"] - idl)
        f["score_after"] = max(0.0, f["after_eff"] - idl)
        table.append((k, f))
    table.sort(key=lambda kf: -kf[1]["score_before"])
    json.dump(dict(forms=forms),
              open(os.path.join(args.out, "forms.json"), "w"), indent=1)
    n = max(1, len(table))
    hb = sum(f["before_eff"] for _, f in table)
    ha = sum(f["after_eff"] for _, f in table)
    hi = sum(f["ideal"] for _, f in table)
    sb = sum(f["score_before"] for _, f in table)
    sa = sum(f["score_after"] for _, f in table)
    lines = []
    lines.append("forms %d (supported encodings %d)" % (len(table), len(supported)))
    lines.append("mean host instrs per form (eff = mean of pinned / pinned+known-zero): "
                 "before %.3f after %.3f ideal %.3f; excess over ideal before %.3f after %.3f" %
                 (hb / n, ha / n, hi / n, sb / n, sa / n))
    lines.append("%-30s %5s %9s %9s %9s %4s %4s %4s %6s %6s %6s" % (
        "form", "ideal", "pin b->a", "pinZ b->a", "unp b->a", "uops", "rthr", "lat",
        "ratio", "scoreB", "scoreA"))
    for k, f in table[:args.top]:
        lines.append("%-30s %5d %4d->%-4d %4d->%-4d %4s->%-4s %4s %4s %4s %6.2f %6.3f %6.3f" % (
            k[:30], f["ideal"], f["before_pinned"], f["after_pinned"],
            f["before_pinz"], f["after_pinz"], f["before_unpinned"], f["after_unpinned"], f["uops"],
            f["rthru"], f["lat"], f["before_eff"] / max(1, f["ideal"]), f["score_before"], f["score_after"]))
    txt = "\n".join(lines)
    open(os.path.join(args.out, "table.txt"), "w").write(txt + "\n")
    print(txt)

if __name__ == "__main__":
    main()
