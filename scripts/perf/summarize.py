"""Summarize an fx perf suite run (scripts/perf/fx_ab.ps1; docs/FX_PERF.md).

usage: python -I summarize.py <samples.csv> --a <label> --b <label> [--out summary.md]

Per benchmark metric: the median of each side, its spread ((max - min) / median), the speedup of B over A
(A/B for lower-is-better metrics, B/A for higher-is-better ones, so above 1 always means B is faster), the
median of the per-round paired speedups, and a bootstrap 95 % interval of the speedup of the medians.
A metric is called faster or slower only when that interval excludes 1. Rows with a non-ok status are left
out and counted. For the micro benchmark the native x64 reference (side "native", every round; median) gives
translated/native for each side.
"""
import argparse
import csv
import math
import random
import statistics
import sys
from collections import defaultdict


def median(v):
    return statistics.median(v) if v else float("nan")


def speedup(a, b, better):
    if a <= 0 or b <= 0:
        return float("nan")
    return a / b if better == "lower" else b / a


def bootstrap(av, bv, better, n=2000, seed=1):
    rng = random.Random(seed)
    out = []
    for _ in range(n):
        ra = [rng.choice(av) for _ in av]
        rb = [rng.choice(bv) for _ in bv]
        out.append(speedup(median(ra), median(rb), better))
    out = sorted(x for x in out if not math.isnan(x))
    if not out:
        return float("nan"), float("nan")
    return out[int(0.025 * (len(out) - 1))], out[int(0.975 * (len(out) - 1))]


def fmt(x, digits=3):
    if isinstance(x, float) and math.isnan(x):
        return "–"
    if abs(x) >= 1000:
        return f"{x:,.0f}"
    if abs(x) >= 100:
        return f"{x:.1f}"
    return f"{x:.{digits}g}" if abs(x) < 1 else f"{x:.{digits}f}".rstrip("0").rstrip(".")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--out")
    args = ap.parse_args()

    values = defaultdict(lambda: defaultdict(dict))  # (bench, metric) -> side -> round -> value
    meta = {}
    native = {}
    bad = defaultdict(int)
    loads = defaultdict(list)
    with open(args.csv, newline="") as f:
        for row in csv.DictReader(f):
            key = (row["bench"], row["metric"])
            if row["side"] == "native":
                native.setdefault(key, []).append(float(row["value"]))
                continue
            if row["status"] != "ok":
                bad[(key, row["side"])] += 1
                continue
            values[key][row["side"]][int(row["round"])] = float(row["value"])
            meta[key] = (row["unit"], row["better"])
            if row["load"]:
                loads[row["side"]].append(row["load"])

    lines = [f"# fx perf: {args.b} vs {args.a}", "",
             "Speedup above 1: B is faster. Spread: (max - min) / median. Paired: median of the per-round speedups.",
             "Verdict from the bootstrap 95 % interval of the speedup of the medians.", ""]
    by_bench = defaultdict(list)
    for key in sorted(values):
        by_bench[key[0]].append(key)
    for bench, keys in by_bench.items():
        has_native = any((bench, k[1]) in native for k in keys)
        head = f"| {bench} | unit | {args.a} median | spread | {args.b} median | spread | speedup | paired | 95 % interval | verdict |"
        sep = "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |"
        if has_native:
            head += f" native | {args.a}/native | {args.b}/native |"
            sep += " ---: | ---: | ---: |"
        lines += [f"## {bench}", "", head, sep]
        logs = []
        for key in keys:
            unit, better = meta[key]
            av = list(values[key].get(args.a, {}).values())
            bv = list(values[key].get(args.b, {}).values())
            ma, mb = median(av), median(bv)
            sa = (max(av) - min(av)) / ma if av and ma else float("nan")
            sb = (max(bv) - min(bv)) / mb if bv and mb else float("nan")
            sp = speedup(ma, mb, better) if av and bv else float("nan")
            rounds = sorted(set(values[key].get(args.a, {})) & set(values[key].get(args.b, {})))
            paired = median([speedup(values[key][args.a][r], values[key][args.b][r], better) for r in rounds]) if rounds else float("nan")
            lo, hi = bootstrap(av, bv, better) if len(av) >= 2 and len(bv) >= 2 else (float("nan"), float("nan"))
            verdict = "–"
            if not math.isnan(lo):
                verdict = "faster" if lo > 1 else "slower" if hi < 1 else "same"
                if not math.isnan(sp):
                    logs.append(math.log(sp))
            notes = []
            for side in (args.a, args.b):
                if bad[(key, side)]:
                    notes.append(f"{bad[(key, side)]} failed on {side}")
            if notes:
                verdict += " (" + ", ".join(notes) + ")"
            row = (f"| {key[1]} | {unit} | {fmt(ma)} | {fmt(100 * sa, 2)} % | {fmt(mb)} | {fmt(100 * sb, 2)} % | "
                   f"{fmt(sp)} | {fmt(paired)} | {fmt(lo)}–{fmt(hi)} | {verdict} |")
            if has_native:
                n = median(native[key]) if key in native else float("nan")
                row += f" {fmt(n)} | {fmt(ma / n if n == n and n else float('nan'))} | {fmt(mb / n if n == n and n else float('nan'))} |"
            lines.append(row)
        for (key, side), count in sorted(bad.items()):
            if key[0] == bench and key not in values:
                lines.append(f"| {key[1]} | | | | | | | | | {count} failed on {side} |")
        if logs:
            lines += ["", f"Geometric mean speedup over {len(logs)} metrics: **{fmt(math.exp(sum(logs) / len(logs)))}**"]
        lines.append("")
    for side, l in loads.items():
        lines.append(f"Load before samples, {side}: {', '.join(l[:40])}{' ...' if len(l) > 40 else ''}")
    text = "\n".join(lines) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
