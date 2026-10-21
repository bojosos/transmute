"""Host instructions per iteration of a guest loop path in a statically translated region.

Usage:
  python tools/fx_loop_count.py <fx_translator_image_audit.exe> <image> <region rva> <path> <out-dir> [VAR=value ...]

<path> lists the guest instructions of one iteration in execution order, as
comma-separated hex RVAs or inclusive ranges (`a-b`, step 4), ending with the
first one again (the loop header), e.g. `33eb4c-33eb54,33eb58-33ebfc,33eb4c`.

The audit binary translates the region (TRANSMUTE_FX_AUDIT_DUMP, with the
given environment, e.g. TRANSMUTE_FX_CORE=tier2) and writes its code and
instruction map (r_<rva>.bin / .map). This script walks the host code from
the header's block label: straight-line code is counted, a branch is taken
when its target (directly, or through a cold-section edge stub, which is then
counted too) is the next guest instruction of the path, and branches inside
one guest instruction's code are assumed not taken. It prints the host
instructions of one iteration and the GuestContext ([r15 + ...]) loads and
stores among them. This is exact for the path's code, including edge code,
as long as the path's internal branches go the assumed way.
"""
import os
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fx_layout_audit import disassemble  # noqa: E402


def parse_path(spec):
    pcs = []
    for item in spec.split(","):
        if "-" in item:
            a, b = (int(x, 16) for x in item.split("-"))
            pcs.extend(range(a, b + 4, 4))
        else:
            pcs.append(int(item, 16))
    return pcs


def read_map(path):
    entries, hot, retries, pins = {}, None, 0, []
    for line in pathlib.Path(path).read_text().splitlines():
        parts = line.split()
        if parts[0] == "base":
            hot, retries = int(parts[3]), int(parts[5])
        elif parts[0] == "pin":
            pins.append(parts[1] + "->" + parts[2])
        else:
            entries.setdefault(int(parts[0], 16), (int(parts[1], 16), int(parts[2], 16)))
    return entries, hot, retries, pins


def ctx_kind(op, args):
    if "[r15" not in args or "rip" in args:
        return None
    store = re.match(r"\S+ ptr \[r15", args) is not None
    if op.startswith(("cmp", "test")) or op in ("bt",):
        store = False
    return "store" if store else "load"


def count(code, entries, hot, path):
    insts = {at: (size, op, args) for at, size, op, args in disassemble(code)}
    order = sorted(insts)
    nxt = {a: order[i + 1] if i + 1 < len(order) else None for i, a in enumerate(order)}
    path = [pc for pc in path if pc in entries]
    if len(path) < 2 or path[0] != path[-1]:
        raise SystemExit("path must start and end at the header, with translated instructions")
    start = entries[path[0]][0]
    begins = {}
    for pc, (label, begin) in entries.items():
        begins.setdefault(begin, pc)
    stats = {"insts": 0, "load": 0, "store": 0, "stub": 0}
    listing = []

    def at_target(t, k):
        label, begin = entries[path[k + 1]]
        return label <= t <= begin

    def follow_stub(t, k):
        """Instructions of a cold stub at t if it leads to path[k+1], else None."""
        seen = []
        h = t
        while h is not None and len(seen) < 64:
            size, op, args = insts[h]
            seen.append(h)
            if op == "jmp":
                m = re.match(r"0x([0-9a-f]+)", args)
                if m and at_target(int(m[1], 16), k):
                    return seen
                return None
            h = nxt[h]
        return None

    h, k = start, 0
    while True:
        size, op, args = insts[h]
        stats["insts"] += 1
        kind = ctx_kind(op, args)
        if kind:
            stats[kind] += 1
        listing.append("%6x  %-10s %s" % (h, op, args))
        target = None
        m = re.match(r"0x([0-9a-f]+)", args) if op.startswith("j") else None
        if m:
            t = int(m[1], 16)
            if at_target(t, k):
                target = t
            elif t >= hot:
                stub = follow_stub(t, k)
                if stub:
                    for s in stub:
                        _, sop, sargs = insts[s]
                        stats["insts"] += 1
                        stats["stub"] += 1
                        kind = ctx_kind(sop, sargs)
                        if kind:
                            stats[kind] += 1
                        listing.append("%6x  %-10s %s   (edge stub)" % (s, sop, sargs))
                    _, _, sargs = insts[stub[-1]]
                    target = int(re.match(r"0x([0-9a-f]+)", sargs)[1], 16)
            elif (op == "jmp" or op == "jrcxz") and t > h and t < entries[path[k + 1]][0]:
                # Inside one guest instruction's code: a join (jmp), or a fusion's
                # fact-flag check (jrcxz: the flag is zero in steady state).
                h = t
                continue
            elif op == "jmp":
                raise SystemExit("unconditional jump at %x off the path (k=%d, pc %x)" % (h, k, path[k]))
        if target is None and op == "jmp":
            raise SystemExit("indirect jump at %x off the path" % h)
        if target is not None:
            h = target
            k += 1
        else:
            h = nxt[h]
            if h is None:
                raise SystemExit("ran off the end of the code")
            # Falling through into the next path instruction's code (several when
            # they emit nothing, e.g. inlined RETs or copies).
            while k + 1 < len(path) and h in (entries[path[k + 1]][0], entries[path[k + 1]][1]):
                if h == entries[path[k + 1]][0] and entries[path[k + 1]][0] != entries[path[k + 1]][1]:
                    break   # at its block label: the instruction's own code starts after the block code
                k += 1
            if h in begins and entries[path[k]][1] != h:
                raise SystemExit("fell into pc %x at %x (path expects %x)" % (begins[h], h, path[min(k + 1, len(path) - 1)]))
        if k >= len(path) - 1:
            break
    return stats, listing


def main():
    audit, image, rva, spec, out = sys.argv[1:6]
    env = dict(os.environ)
    tag = []
    for kv in sys.argv[6:]:
        k, v = kv.split("=", 1)
        env[k] = v
        tag.append(v)
    rva = int(rva, 16)
    out = pathlib.Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env["TRANSMUTE_FX_AUDIT_DUMP"] = "%x" % rva
    subprocess.run([str(pathlib.Path(audit).resolve()), image, str(out)], env=env, check=True, capture_output=True)
    code = (out / ("r_%x.bin" % rva)).read_bytes()
    entries, hot, retries, pins = read_map(out / ("r_%x.map" % rva))
    stats, listing = count(code, entries, hot, parse_path(spec))
    (out / ("loop_%x.txt" % rva)).write_text("\n".join(listing) + "\n")
    print("region %x [%s]: %d host instructions/iteration, context loads %d, stores %d (edge stubs %d); "
          "region %d bytes (body %d), retries %d, pins %s" % (
              rva, ",".join(tag) or "default", stats["insts"], stats["load"], stats["store"], stats["stub"],
              len(code), hot, retries, " ".join(pins)))


if __name__ == "__main__":
    main()
