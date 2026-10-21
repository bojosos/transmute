"""Estimated dynamic host instructions of the hottest regions under other translator settings.

Usage:
  python tools/fx_tier2_estimate.py <profile.txt> <fx_translator_image_audit.exe> <out-dir> <base-env> <env> [<env> ...]

Each <env> is a comma-free description of environment settings separated by
';' (e.g. "TRANSMUTE_FX_CORE=tier2"), or "-" for none. <base-env> must
reproduce the profiled run's code: every profiled region whose static
translation (TRANSMUTE_FX_AUDIT_DUMP) is not byte-identical to the code
dumped by the profiler (<profile.txt>.r<N>.bin, apart from relocated
addresses) is skipped.

For each region, the profile's RIP samples are attributed to guest
instructions through the static translation's instruction map (r_<rva>.map),
summed per guest basic block (from a block's label to the next block) and
divided by the block's host instructions: an execution-frequency estimate
that assumes equal cycles per host instruction. Each setting's estimate is
then sum(frequency(block) * host instructions(block)) over the same guest
blocks, i.e. the dynamic instruction count of the sampled code if the block
frequencies stay the same. Cold-section code (edge stubs) is not counted.
"""
import bisect
import collections
import os
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fx_layout_audit import disassemble, module_path  # noqa: E402


def read_regions(profile):
    text = pathlib.Path(str(profile) + ".regions.txt").read_text(errors="replace")
    regions, cur = [], None
    for line in text.splitlines():
        m = re.match(r"region (\d+) (\S+) guest 0x([0-9a-f]+) module_base 0x([0-9a-f]+) host 0x[0-9a-f]+ size (\d+) samples (\d+)", line)
        if m:
            cur = {"n": int(m[1]), "module": m[2], "rva": int(m[3], 16) - int(m[4], 16), "size": int(m[5]),
                   "samples": collections.Counter()}
            regions.append(cur)
            continue
        m = re.match(r"\s+sample (\d+) (\d+)", line)
        if m and cur:
            cur["samples"][int(m[1])] += int(m[2])
    return regions


def translate(audit, module, rvas, out, env_text):
    env = {k: v for k, v in os.environ.items() if not k.startswith("TRANSMUTE_FX")}
    if env_text != "-":
        for kv in env_text.split(";"):
            k, v = kv.split("=", 1)
            env[k] = v
    env["TRANSMUTE_FX_AUDIT_DUMP"] = ",".join("%x" % r for r in rvas)
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([audit, str(module_path(module)), str(out)], env=env, check=True, capture_output=True)


def load(out, rva):
    code = (out / ("r_%x.bin" % rva)).read_bytes()
    starts, hot = [], len(code)
    for line in (out / ("r_%x.map" % rva)).read_text().splitlines():
        p = line.split()
        if p[0] == "base":
            hot = int(p[3])
        elif p[0] != "pin":
            pc, label, begin = int(p[0], 16), int(p[1], 16), int(p[2], 16)
            starts.append((label, pc, label != begin or (len(p) > 3 and p[3] == "1")))
    starts.sort()
    return code, hot, starts


def owners(code, hot, starts):
    """Host instruction offsets of the body -> guest pc (the instruction or block whose code it is)."""
    keys = [s[0] for s in starts]
    result = []
    for at, size, op, args in disassemble(code[:hot]):
        i = bisect.bisect_right(keys, at) - 1
        if i >= 0:
            result.append((at, starts[i][1]))
    return result


def blocks(starts):
    """Guest pc -> block id (a block starts at every leader)."""
    block, b = {}, -1
    for label, pc, leader in sorted(starts, key=lambda s: s[1]):
        if leader or b < 0:
            b = pc
        block[pc] = b
    return block


def main():
    profile, audit, out = sys.argv[1], str(pathlib.Path(sys.argv[2]).resolve()), pathlib.Path(sys.argv[3]).resolve()
    settings = sys.argv[4:]
    regions = read_regions(profile)
    by_module = collections.defaultdict(list)
    for r in regions:
        by_module[r["module"]].append(r["rva"])
    for k, s in enumerate(settings):
        for mod, rvas in by_module.items():
            translate(audit, mod, rvas, out / ("s%d" % k) / mod, s)
    totals = [0.0] * len(settings)
    used = 0
    for r in regions:
        dump = pathlib.Path("%s.r%d.bin" % (profile, r["n"]))
        base_dir = out / "s0" / r["module"]
        if not dump.exists() or not (base_dir / ("r_%x.bin" % r["rva"])).exists():
            continue
        code0, hot0, starts0 = load(base_dir, r["rva"])
        runtime = dump.read_bytes()
        # Fixups hold the run's absolute addresses: equal sizes and at most a
        # few percent of differing bytes count as the same code.
        if len(code0) != len(runtime) or sum(a != b for a, b in zip(code0, runtime)) > len(code0) // 25:
            print("region %d %s+0x%x: static translation differs from the profiled code, skipped" % (r["n"], r["module"], r["rva"]))
            continue
        block0 = blocks(starts0)
        own0 = owners(code0, hot0, starts0)
        offsets = [o for o, _ in own0]
        samples = collections.Counter()
        insts0 = collections.Counter()
        for o, pc in own0:
            insts0[block0.get(pc, pc)] += 1
        for off, n in r["samples"].items():
            i = bisect.bisect_right(offsets, off) - 1
            if 0 <= i and off < hot0:
                samples[block0.get(own0[i][1], own0[i][1])] += n
        freq = {b: samples[b] / insts0[b] for b in samples if insts0[b]}
        row = []
        for k in range(len(settings)):
            d = out / ("s%d" % k) / r["module"]
            code, hot, starts = load(d, r["rva"])
            own = owners(code, hot, starts)
            insts = collections.Counter()
            for o, pc in own:
                insts[block0.get(pc, pc)] += 1
            est = sum(f * insts[b] for b, f in freq.items())
            row.append(est)
            totals[k] += est
        used += 1
        print("region %2d %-16s +0x%-8x samples %4d  %s" % (
            r["n"], r["module"], r["rva"], sum(r["samples"].values()),
            "  ".join("%.3f" % (e / row[0]) if row[0] else "-" for e in row)))
    if not used:
        raise SystemExit("no region matched the profiled code")
    print("%d regions; estimated dynamic host instructions relative to setting 0: %s" % (
        used, "  ".join("%s=%.3f" % (s, t / totals[0]) for s, t in zip(settings, totals))))


if __name__ == "__main__":
    main()
