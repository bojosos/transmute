"""Conformance layer: build open-source test suites as arm64 macOS guest programs, run them under
alchemy-fx, and compare the results with the expected-results baseline in baseline/<suite>.tsv.

    python -I scripts/conformance/conformance.py build  <suite|all>
    python -I scripts/conformance/conformance.py run    <suite|all> [--filter RE] [--jobs N]
    python -I scripts/conformance/conformance.py report <suite|all> [--write-baseline]

Suites live in suites/<name>.py. Upstream sources are untrusted data under --sources (default
C:\\dev\\conformance, see sources.json and README.md); every output goes under --out (default
C:\\dev\\conformance\\out). Nothing is built inside the repository, no GPL code is ever copied into it,
and guest programs run only through alchemy-fx, cloaked and with loopback-only networking.
"""
import argparse
import concurrent.futures
import dataclasses
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
def llvm_hints():
    """VS 2022's LLVM (any edition: the GitHub runner has Enterprise), or $CONFORMANCE_LLVM."""
    if os.environ.get("CONFORMANCE_LLVM"):
        yield pathlib.Path(os.environ["CONFORMANCE_LLVM"])
    for edition in sorted(pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022").glob("*")):
        yield edition / "VC" / "Tools" / "Llvm" / "x64" / "bin"
        yield edition / "VC" / "Tools" / "Llvm" / "bin"


LLVM_HINTS = list(llvm_hints())
GUEST_ENV = {
    # Every guest run is cloaked and loopback-only (never a visible window, never the network).
    "ALCHEMY_GUI_CLOAK": "1", "ALCHEMY_GUI_NO_ACTIVATE": "1", "ALCHEMY_GUI_NO_CRASH_DIALOG": "1",
    "ALCHEMY_NETWORK": "loopback",
}
STATUSES = ("PASS", "FAIL", "TIMEOUT", "CRASH", "BUILD", "SKIP")


@dataclasses.dataclass
class Test:
    name: str                 # unique within the suite, e.g. "functional/strtod"
    exe: pathlib.Path         # guest program
    args: list = dataclasses.field(default_factory=list)
    timeout: float = 60.0
    env: dict = dataclasses.field(default_factory=dict)
    cwd: pathlib.Path | None = None
    build_error: str = ""     # non-empty: the test failed to build; status BUILD
    skip: str = ""            # non-empty: not run here, with the reason; status SKIP


@dataclasses.dataclass
class Result:
    name: str
    status: str
    exit_code: int | None
    seconds: float
    detail: str               # one line: why it failed (first failing assertion or error)
    output: str               # stdout+stderr tail


class Context:
    def __init__(self, args):
        self.sources = pathlib.Path(args.sources)
        self.out = pathlib.Path(args.out)
        self.sysroot = self.out / "sysroot"
        self.manifest = json.loads((HERE / "sources.json").read_text())
        # Building needs VS 2022's LLVM; running does not (the macOS ground-truth job only runs).
        self.llvm = next((p for p in LLVM_HINTS if (p / "clang.exe").exists()), None)
        if not self.llvm and args.command in ("build", "repro"):
            raise SystemExit("clang.exe not found (VS 2022 LLVM)")
        self.clang = self.llvm / "clang.exe" if self.llvm else None
        self.clangxx = self.llvm / "clang++.exe" if self.llvm else None
        # --native runs the programs directly: only on real macOS, for ground truth.
        self.native = args.native
        self.alchemy = pathlib.Path(args.alchemy) if args.alchemy else REPO / "build-ninja" / "Release" / "alchemy-fx.exe"
        self.llvm_source = pathlib.Path(args.llvm_source)
        self.runtime_dirs = [pathlib.Path(p) for p in (args.runtime_dir or [r"C:\dev\transmute\artifacts\alchemy-llvm-arm64"])]
        self.jobs = args.jobs

    def source(self, name):
        path = self.sources / self.manifest[name]["dir"] / "src"
        if not path.is_dir():
            raise SystemExit(f"{name}: {path} is missing; download it first (README.md)")
        return path

    # ---- building ----------------------------------------------------------------------------
    def cflags(self, cxx=False):
        flags = ["--target=arm64-apple-macos11", f"-isysroot{self.sysroot}", "-fblocks", "-O1", "-g0",
                 "-Wno-deprecated-declarations", "-Wno-unused-command-line-argument", "-fno-color-diagnostics"]
        if cxx:
            # The guest libc++ (scripts/build_alchemy_llvm.ps1): LLVM's headers plus the build
            # configuration headers in runtime/include.
            config = self.out / "libcxx-config"
            config.mkdir(parents=True, exist_ok=True)
            for name in ("__config_site", "__assertion_handler"):
                shutil.copyfile(REPO / "runtime" / "include" / name, config / name)
            flags += ["-std=c++17", "-nostdinc++", "-isystem", str(self.llvm_source / "libcxx" / "include"),
                      "-isystem", str(config)]
        return flags

    def cxx_link_flags(self):
        # -L lets lld find libc++'s re-exported libc++abi and libunwind.
        return [f"-L{self.runtime_dirs[0]}", str(self.runtime_dirs[0] / "libc++.1.dylib")]

    def compile(self, src, obj, flags=(), cxx=False):
        obj.parent.mkdir(parents=True, exist_ok=True)
        cmd = [str(self.clangxx if cxx else self.clang), *self.cflags(cxx), *flags, "-c", str(src), "-o", str(obj)]
        return run_tool(cmd)

    def link(self, objs, exe, libs=("System",), flags=()):
        """Links with clang's Darwin driver against the sysroot's link-only .tbd interfaces. Unresolved
        names bind at run time to Alchemy's providers (-undefined dynamic_lookup), as the repo's own
        fixtures do."""
        exe.parent.mkdir(parents=True, exist_ok=True)
        cmd = [str(self.clang), "--target=arm64-apple-macos11", f"-isysroot{self.sysroot}", "-fuse-ld=lld",
               "-Wl,-undefined,dynamic_lookup", "-Wl,-fixup_chains", *[str(o) for o in objs],
               *[f"-l{lib}" for lib in libs if lib != "System"], *flags, "-o", str(exe)]
        return run_tool(cmd)

    # ---- running -----------------------------------------------------------------------------
    def guest_command(self, test):
        if self.native:
            return [str(test.exe), *test.args]
        cmd = [str(self.alchemy)]
        for d in self.runtime_dirs:
            cmd += ["--runtime-dir", str(d)]
        cmd += ["--root", str(self.guest_root()), str(test.exe), *test.args]
        return cmd

    def guest_root(self):
        root = self.out / "root"
        marker = root / ".alchemy-root"
        if not marker.exists():
            for sub in ("tmp", "private/tmp", "var/tmp", "usr", "etc", "Users"):
                (root / sub).mkdir(parents=True, exist_ok=True)
            marker.write_text("alchemy-root\t1\n")
        # Files the suites expect on a Mac: libdispatch's I/O tests read /usr/bin/vi as their large
        # file (dispatch_test_get_large_file). 1 MiB of deterministic bytes stands in for it.
        vi = root / "usr" / "bin" / "vi"
        if not vi.exists():
            vi.parent.mkdir(parents=True, exist_ok=True)
            vi.write_bytes(bytes((i * 131 + (i >> 8)) & 0xFF for i in range(1 << 20)))
        return root

    def run_test(self, test):
        if test.skip:
            return Result(test.name, "SKIP", None, 0.0, test.skip, "")
        if test.build_error:
            return Result(test.name, "BUILD", None, 0.0, first_line(test.build_error), test.build_error[-4000:])
        cwd = test.cwd or (self.out / "run" / safe_name(test.name))
        cwd.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, **GUEST_ENV, **test.env)
        # Output goes to a file, not a pipe: a guest child process inherits the handle and can keep a
        # pipe open long after the test itself has exited or been killed.
        log = self.out / "logs" / f"{safe_name(test.name)}.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        start = time.monotonic()
        with open(log, "wb") as sink:
            proc = subprocess.Popen(self.guest_command(test), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                    stdout=sink, stderr=subprocess.STDOUT, start_new_session=os.name != "nt")
            try:
                proc.wait(timeout=test.timeout)
                status = None
            except subprocess.TimeoutExpired:
                kill_tree(proc)
                try:
                    proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    pass
                status = "TIMEOUT"
        seconds = time.monotonic() - start
        text = log.read_bytes().decode("utf-8", "replace")
        code = proc.returncode
        if status is None:
            # Crash: a signal on POSIX hosts, an NTSTATUS exception code on Windows. exit(-1) arrives
            # as 0xFFFFFFFF on Windows and is an ordinary failure.
            status = "PASS" if code == 0 else ("CRASH" if code < 0 or 0xC0000000 <= code < 0xFFFFFFFF or
                                               crashed(text) else "FAIL")
        return Result(test.name, status, code, round(seconds, 2), "", text[-4000:])


def parallel(fn, items, jobs=None):
    """Runs fn over items on a thread pool (compiles and links are separate processes)."""
    with concurrent.futures.ThreadPoolExecutor(jobs or os.cpu_count()) as pool:
        return list(pool.map(fn, items))


def run_tool(cmd):
    proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True, errors="replace")
    return "" if proc.returncode == 0 else (proc.stderr or proc.stdout or f"exit {proc.returncode}").strip()


def kill_tree(proc):
    # The PID is the process this script started; /T (or its process group) also ends the child
    # processes it spawned.
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
    else:
        try:
            os.killpg(proc.pid, 9)
        except OSError:
            proc.kill()


def crashed(text):
    return bool(re.search(r"(?m)^(alchemy(-fx)?: (fatal|unhandled|guest fault)|.*guest crashed)", text, re.I))


def first_line(text):
    for line in text.splitlines():
        if "error" in line.lower():
            return line.strip()[:300]
    return (text.strip().splitlines() or [""])[0][:300]


def safe_name(name):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


# ---- baseline ---------------------------------------------------------------------------------
def load_baseline(suite):
    path = HERE / "baseline" / f"{suite}.tsv"
    rows = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            name, expected, owner, *note = line.split("\t")
            rows[name] = {"expected": expected, "owner": owner, "note": note[0] if note else ""}
    return rows


def write_baseline(suite, results, module, old):
    path = HERE / "baseline" / f"{suite}.tsv"
    path.parent.mkdir(exist_ok=True)
    lines = [f"# {suite}: expected results under alchemy-fx (scripts/conformance/README.md).",
             "# test\texpected\towner\tnote. expected: PASS, FAIL (owner bug), XFAIL (honest refusal or not",
             "# on Darwin; the note says which), BUILD (does not build yet), SKIP."]
    suite_xfail = getattr(module, "expected_failure", None)
    on_macos = macos_status(suite)
    for r in sorted(results, key=lambda r: r.name):
        prev = old.get(r.name, {})
        reason = None
        if r.status not in ("PASS", "SKIP"):
            reason = policy_refusal(r) or (suite_xfail(r) if suite_xfail else None)
            if not reason and on_macos.get(r.name) not in (None, "PASS", "SKIP", "BUILD"):
                reason = f"fails on real macOS too ({on_macos[r.name]})"
        if r.status in ("PASS", "SKIP"):
            expected = r.status
        elif prev.get("expected") == "XFAIL" or reason:
            expected = "XFAIL"
        else:
            expected = "BUILD" if r.status == "BUILD" else "FAIL"
        owner = prev.get("owner") or module.owner(r.name)
        note = prev.get("note") or reason or (r.detail if r.status != "PASS" else "")
        lines.append("\t".join([r.name, expected, owner, note.replace("\t", " ")[:400]]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# Features Alchemy refuses by policy: the sandbox (an honest ENOTSUP) and launchd/XPC services today.
POLICY_REFUSALS = re.compile(r"sandbox_init|sandbox_check|\bsandbox\b.*(ENOTSUP|not supported|refus)|"
                             r"bootstrap_look_up|xpc_connection|\blaunchd\b", re.I)


def policy_refusal(result):
    if POLICY_REFUSALS.search(result.detail or "") or POLICY_REFUSALS.search(result.output[-2000:]):
        return "refused by policy (sandbox, or launchd/XPC services)"
    return None


def compare(results, baseline):
    """Regressions: tests that were expected to pass (or to build) and no longer do."""
    regressions, fixed, new = [], [], []
    for r in results:
        b = baseline.get(r.name)
        if b is None:
            new.append(r)
        elif b["expected"] == "PASS" and r.status != "PASS":
            regressions.append(r)
        elif b["expected"] in ("FAIL", "XFAIL", "BUILD") and r.status == "PASS":
            fixed.append(r)
        elif b["expected"] == "FAIL" and r.status == "BUILD":
            regressions.append(r)
    return regressions, fixed, new


# ---- driver -----------------------------------------------------------------------------------
def load_suite(name):
    spec = importlib.util.spec_from_file_location(f"suite_{name}", HERE / "suites" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def suite_names():
    return sorted(p.stem for p in (HERE / "suites").glob("*.py") if not p.stem.startswith("_"))


def results_path(ctx, suite):
    return ctx.out / "results" / f"{suite}.json"


def cmd_build(ctx, suite, module, args):
    tests = module.build(ctx)
    broken = sum(1 for t in tests if t.build_error)
    (ctx.out / "tests").mkdir(parents=True, exist_ok=True)
    # Paths relative to --out, so a built tree runs anywhere (the macOS ground-truth job, README).
    def rel(p):
        return pathlib.Path(p).relative_to(ctx.out).as_posix() if p else None
    (ctx.out / "tests" / f"{suite}.json").write_text(json.dumps(
        [dict(dataclasses.asdict(t), exe=rel(t.exe), cwd=rel(t.cwd)) for t in tests], indent=1))
    print(f"{suite}: {len(tests)} tests, {broken} do not build")


def load_tests(ctx, suite):
    path = ctx.out / "tests" / f"{suite}.json"
    if not path.exists():
        raise SystemExit(f"{suite}: not built; run `build {suite}` first")
    tests = []
    for t in json.loads(path.read_text()):
        t["exe"] = ctx.out / t["exe"]
        t["cwd"] = ctx.out / t["cwd"] if t["cwd"] else None
        tests.append(Test(**t))
    return tests


def cmd_run(ctx, suite, module, args):
    if not ctx.native and not ctx.alchemy.exists():
        raise SystemExit(f"alchemy-fx not found: {ctx.alchemy} (pass --alchemy)")
    tests = load_tests(ctx, suite)
    if args.filter:
        tests = [t for t in tests if re.search(args.filter, t.name)]
    results = []
    with concurrent.futures.ThreadPoolExecutor(ctx.jobs) as pool:
        for r in pool.map(ctx.run_test, tests):
            if r.status not in ("BUILD",):
                r = module.classify(r, ctx) if hasattr(module, "classify") and r.status != "SKIP" else r
            if not r.detail and r.status != "PASS":
                r.detail = first_line(r.output) if r.output else ""
            results.append(r)
            if args.verbose or r.status not in ("PASS", "BUILD", "SKIP"):
                print(f"{r.status:7} {r.seconds:6.1f}s {r.name}  {r.detail[:120]}", flush=True)
    previous = {}
    path = results_path(ctx, suite)
    if args.filter and path.exists():
        previous = {r["name"]: r for r in json.loads(path.read_text())}
    previous.update({r.name: dataclasses.asdict(r) for r in results})
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(sorted(previous.values(), key=lambda r: r["name"]), indent=1))
    summarize(suite, results)


def summarize(suite, results):
    counts = {s: sum(1 for r in results if r.status == s) for s in STATUSES}
    print(f"{suite}: " + ", ".join(f"{s} {n}" for s, n in counts.items() if n))


def cmd_report(ctx, suite, module, args):
    path = results_path(ctx, suite)
    if not path.exists():
        raise SystemExit(f"{suite}: no results; run `run {suite}` first")
    results = [Result(**r) for r in json.loads(path.read_text())]
    baseline = load_baseline(suite)
    if args.write_baseline:
        write_baseline(suite, results, module, baseline)
        baseline = load_baseline(suite)
    summarize(suite, results)
    regressions, fixed, new = compare(results, baseline)
    by_owner = {}
    for r in results:
        owner = baseline.get(r.name, {}).get("owner") or module.owner(r.name)
        area = module.area(r.name) if hasattr(module, "area") else r.name.split("/")[0]
        row = by_owner.setdefault((owner, area), {s: 0 for s in STATUSES})
        expected = baseline.get(r.name, {}).get("expected")
        row["XFAIL" if expected == "XFAIL" and r.status != "PASS" else r.status] = \
            row.get("XFAIL" if expected == "XFAIL" and r.status != "PASS" else r.status, 0) + 1
    print("\n| owner | area | pass | fail | xfail | timeout | crash | build |")
    print("|---|---|---|---|---|---|---|---|")
    for (owner, area), row in sorted(by_owner.items()):
        print(f"| {owner} | {area} | {row['PASS']} | {row['FAIL']} | {row.get('XFAIL', 0)} | {row['TIMEOUT']} | "
              f"{row['CRASH']} | {row['BUILD']} |")
    for title, rows in (("regressions", regressions), ("newly passing", fixed), ("not in baseline", new)):
        if rows:
            print(f"\n{title} ({len(rows)}):")
            for r in rows:
                print(f"  {r.status:7} {r.name}  {r.detail[:150]}")
    if regressions:
        sys.exit(1)


def cmd_compare(ctx, suite, module, args):
    """Alchemy's results against a --native run of the same programs on real macOS. A test that fails
    under Alchemy but passes on macOS is an Alchemy bug; one that fails on both is a test expectation
    macOS does not meet either (recorded in baseline/macos/<suite>.tsv, and XFAIL in the baseline)."""
    if not args.macos_results:
        raise SystemExit("compare needs --macos-results <dir>")
    mine = {r["name"]: r for r in json.loads(results_path(ctx, suite).read_text())}
    mac_path = pathlib.Path(args.macos_results) / f"{suite}.json"
    if not mac_path.exists():
        raise SystemExit(f"{suite}: no macOS results at {mac_path}")
    mac = {r["name"]: r for r in json.loads(mac_path.read_text())}
    groups = {"Alchemy-only failure": [], "fails on macOS too": [], "passes only under Alchemy": []}
    for name, r in sorted(mine.items()):
        m = mac.get(name)
        if not m or "SKIP" in (r["status"], m["status"]) or "BUILD" in (r["status"], m["status"]):
            continue
        if r["status"] != "PASS" and m["status"] == "PASS":
            groups["Alchemy-only failure"].append(r)
        elif r["status"] != "PASS":
            groups["fails on macOS too"].append(r)
        elif m["status"] != "PASS":
            groups["passes only under Alchemy"].append(m)
    out = HERE / "baseline" / "macos" / f"{suite}.tsv"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(f"# {suite}: status of each program on real macOS (conformance-macos workflow).\n" +
                   "".join(f"{n}\t{m['status']}\t{(m['detail'] or '').replace(chr(9), ' ')[:200]}\n"
                           for n, m in sorted(mac.items())), encoding="utf-8")
    for title, rows in groups.items():
        print(f"{suite}: {title}: {len(rows)}")
        for r in rows:
            print(f"  {r['status']:7} {r['name']}  {(r['detail'] or '')[:150]}")


def macos_status(suite):
    """Ground truth from baseline/macos/<suite>.tsv: {test: status on real macOS}."""
    path = HERE / "baseline" / "macos" / f"{suite}.tsv"
    if not path.exists():
        return {}
    return {line.split("\t")[0]: line.split("\t")[1] for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")}


def cmd_repro(ctx, path, args):
    """Builds one C/ObjC source against the sysroot and runs it the way the suites do."""
    src = pathlib.Path(path)
    exe = ctx.out / "repro" / src.stem
    objc = src.suffix == ".m"
    err = ctx.compile(src, exe.with_suffix(".o"), ["-fobjc-arc"] if objc else []) or \
        ctx.link([exe.with_suffix(".o")], exe, libs=("System", "objc") if objc else ("System",))
    if err:
        raise SystemExit(f"build failed:\n{err}")
    r = ctx.run_test(Test(name=f"repro/{src.stem}", exe=exe, timeout=60))
    print(r.output, end="" if r.output.endswith("\n") else "\n")
    print(f"[{r.status}] exit {r.exit_code} in {r.seconds}s")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("build", "run", "report", "repro", "compare"))
    ap.add_argument("suite", help="a suite name, all, or (repro) a source file")
    ap.add_argument("--sources", default=r"C:\dev\conformance")
    ap.add_argument("--out", default=r"C:\dev\conformance\out")
    ap.add_argument("--alchemy", help="alchemy-fx.exe (default: <repo>/build-ninja/Release/alchemy-fx.exe)")
    ap.add_argument("--runtime-dir", action="append", help="guest runtime dirs (default: the LLVM runtimes)")
    ap.add_argument("--llvm-source", default=r"C:\dev\transmute\artifacts\alchemy-llvm-source",
                    help="LLVM checkout of the guest libc++ (its headers)")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--filter")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--native", action="store_true",
                    help="run the programs directly instead of under alchemy-fx (real macOS only)")
    ap.add_argument("--macos-results", help="compare: the results directory of a --native run on real macOS")
    args = ap.parse_args()
    # Guest output can hold any bytes; the console code page must not end the run.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ctx = Context(args)
    # Suites import this module by name; make that the running instance.
    sys.modules.setdefault("conformance", sys.modules[__name__])
    sys.path.insert(0, str(HERE))
    sys.path.insert(0, str(HERE / "suites"))  # shared suite helpers (suites/_*.py)
    if args.command == "repro":
        cmd_repro(ctx, args.suite, args)
        return
    names = suite_names() if args.suite == "all" else [args.suite]
    failed = False
    for name in names:
        module = load_suite(name)
        try:
            {"build": cmd_build, "run": cmd_run, "report": cmd_report,
             "compare": cmd_compare}[args.command](ctx, name, module, args)
        except SystemExit as e:
            if e.code not in (0, None):
                failed = True
                if isinstance(e.code, str):
                    print(e.code, file=sys.stderr)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
