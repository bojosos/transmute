"""Conformance layer: build open-source test suites as arm64 macOS guest programs, run them under
alchemy-fx, and compare the results with the expected-results baseline in baseline/<suite>.tsv.

    python -I scripts/conformance/conformance.py build  <suite|all>
    python -I scripts/conformance/conformance.py run    <suite|all> [--filter RE] [--jobs N]
    python -I scripts/conformance/conformance.py report <suite|all> [--write-baseline]
    python -I scripts/conformance/conformance.py gate   <suite|all> [--tsv FILE]   (CI: run and judge)

Suites live in suites/<name>.py. Upstream sources are untrusted data under --sources (default
C:\\dev\\conformance, see sources.json and README.md); every output goes under --out (default
C:\\dev\\conformance\\out). Nothing is built inside the repository, no GPL code is ever copied into it,
and guest programs run only through alchemy-fx, cloaked and with loopback-only networking.
"""
import argparse
import concurrent.futures
import dataclasses
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
MIN_CLANG = 17  # ld64.lld with chained fixups and arm64 ad-hoc signing, tested with 19.1.5


def llvm_hints():
    """Directories that may hold clang.exe and ld64.lld.exe, in order: $CONFORMANCE_LLVM, Visual
    Studio's LLVM (any VS version and edition), the standalone LLVM install (GitHub's Windows images
    ship one), then PATH."""
    if os.environ.get("CONFORMANCE_LLVM"):
        yield pathlib.Path(os.environ["CONFORMANCE_LLVM"])
    for edition in sorted(pathlib.Path(r"C:\Program Files\Microsoft Visual Studio").glob("*/*"), reverse=True):
        yield edition / "VC" / "Tools" / "Llvm" / "x64" / "bin"
        yield edition / "VC" / "Tools" / "Llvm" / "bin"
    yield pathlib.Path(r"C:\Program Files\LLVM\bin")
    on_path = shutil.which("clang")
    if on_path:
        yield pathlib.Path(on_path).parent


def find_llvm():
    """The first LLVM with clang and ld64.lld, after checking its version. None if there is none."""
    for d in llvm_hints():
        clang = d / ("clang.exe" if os.name == "nt" else "clang")
        lld = d / ("ld64.lld.exe" if os.name == "nt" else "ld64.lld")
        if not (clang.exists() and lld.exists()):
            continue
        out = subprocess.run([str(clang), "--version"], capture_output=True, text=True).stdout
        m = re.search(r"clang version (\d+)\.(\d+)\.(\d+)", out)
        if not m or int(m.group(1)) < MIN_CLANG:
            raise SystemExit(f"{clang}: clang {m.group(0) if m else '(unknown version)'} is older than {MIN_CLANG}")
        print(f"conformance: using {clang} (clang {'.'.join(m.groups())})", file=sys.stderr)
        return d
    return None
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
        # Building needs LLVM (find_llvm); running does not (the macOS ground-truth job only runs).
        self.llvm = find_llvm() if args.command in ("build", "repro") else None
        if not self.llvm and args.command in ("build", "repro"):
            raise SystemExit("no LLVM with clang and ld64.lld found (set CONFORMANCE_LLVM to its bin directory)")
        self.clang = self.llvm / "clang.exe" if self.llvm else None
        self.clangxx = self.llvm / "clang++.exe" if self.llvm else None
        # --native runs the programs directly: only on real macOS, for ground truth.
        self.native = args.native
        self.alchemy = pathlib.Path(args.alchemy) if args.alchemy else REPO / "build-ninja" / "Release" / "alchemy-fx.exe"
        self.llvm_source = pathlib.Path(args.llvm_source)
        self.runtime_dirs = [pathlib.Path(p) for p in (args.runtime_dir or [r"C:\dev\transmute\artifacts\alchemy-llvm-arm64"])]
        self.jobs = args.jobs
        self.gate = []  # (suite, rows, seconds) per suite the gate ran

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
        # ld64.lld 19 ignores -stack_size ("not yet implemented"), so it is applied after linking.
        stack = [f for f in flags if f.startswith("-Wl,-stack_size,")]
        flags = [f for f in flags if f not in stack]
        cmd = [str(self.clang), "--target=arm64-apple-macos11", f"-isysroot{self.sysroot}", "-fuse-ld=lld",
               "-Wl,-undefined,dynamic_lookup", "-Wl,-fixup_chains", *[str(o) for o in objs],
               *[f"-l{lib}" for lib in libs if lib != "System"], *flags, "-o", str(exe)]
        err = run_tool(cmd)
        if not err and stack:
            set_main_stack_size(exe, int(stack[-1].rsplit(",", 1)[1], 0))
        return err

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
        return Result(test.name, status or exit_status(code, text), code, round(seconds, 2), "", text[-4000:])


def exit_status(code, text):
    # Crash: a signal on POSIX hosts, an NTSTATUS exception code on Windows. exit(-1) arrives as
    # 0xFFFFFFFF on Windows and is an ordinary failure.
    return "PASS" if code == 0 else ("CRASH" if code < 0 or 0xC0000000 <= code < 0xFFFFFFFF or
                                     crashed(text) else "FAIL")


def parallel(fn, items, jobs=None):
    """Runs fn over items on a thread pool (compiles and links are separate processes)."""
    with concurrent.futures.ThreadPoolExecutor(jobs or os.cpu_count()) as pool:
        return list(pool.map(fn, items))


LC_MAIN, LC_CODE_SIGNATURE = 0x80000028, 0x1D


def set_main_stack_size(path, size):
    """Writes LC_MAIN's stacksize (what ld64's -stack_size records) into an arm64 executable, then
    rehashes the page it lives on in lld's ad-hoc code signature, so the program still runs on real
    macOS. Every other page hash is verified unchanged first."""
    data = bytearray(pathlib.Path(path).read_bytes())
    if struct.unpack_from("<I", data, 0)[0] != 0xFEEDFACF:
        raise SystemExit(f"{path}: not a 64-bit Mach-O")
    ncmds, off, main, sig = struct.unpack_from("<I", data, 16)[0], 32, None, None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_MAIN:
            main = off
        elif cmd == LC_CODE_SIGNATURE:
            sig = struct.unpack_from("<I", data, off + 8)[0]
        off += cmdsize
    if main is None:
        raise SystemExit(f"{path}: no LC_MAIN")

    def code_directory():
        if sig is None:
            return None
        magic, _, count = struct.unpack_from(">III", data, sig)
        for i in range(count):
            slot, offset = struct.unpack_from(">II", data, sig + 12 + 8 * i)
            if slot == 0:  # CSSLOT_CODEDIRECTORY
                return sig + offset
        return None

    def page_hashes(cd):
        hash_offset, _, _, ncode, code_limit = struct.unpack_from(">IIIII", data, cd + 16)
        hash_size, hash_type, _, page_log2 = struct.unpack_from(">BBBB", data, cd + 36)
        if hash_type != 2:  # SHA-256
            raise SystemExit(f"{path}: unexpected code signature hash type {hash_type}")
        page = 1 << page_log2
        stored = [bytes(data[cd + hash_offset + i * hash_size: cd + hash_offset + (i + 1) * hash_size])
                  for i in range(ncode)]
        computed = [hashlib.sha256(data[i * page: min((i + 1) * page, code_limit)]).digest()[:hash_size]
                    for i in range(ncode)]
        return stored, computed, cd + hash_offset, hash_size, page

    cd = code_directory()
    if cd is not None:
        stored, computed, _, _, _ = page_hashes(cd)
        if stored != computed:
            raise SystemExit(f"{path}: code signature does not verify before patching; not changed")
    struct.pack_into("<Q", data, main + 16, size)
    if cd is not None:
        _, computed, hashes, hash_size, page = page_hashes(cd)
        index = main // page
        data[hashes + index * hash_size: hashes + (index + 1) * hash_size] = computed[index]
    pathlib.Path(path).write_bytes(data)


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
        # A note explains a non-pass; a test that passes now drops its old one.
        note = "" if r.status == "PASS" else (prev.get("note") if prev.get("expected") != "PASS" else "") or \
            reason or r.detail
        if reason and prev.get("expected") == "FAIL":
            note = f"{reason}; under Alchemy: {note}"
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
    results = run_tests(ctx, module, tests, args)
    save_results(ctx, suite, results, args)
    summarize(suite, results)


def run_one(ctx, module, test):
    r = ctx.run_test(test)
    if r.status not in ("BUILD", "SKIP") and hasattr(module, "classify"):
        r = module.classify(r, ctx)
    if not r.detail and r.status != "PASS":
        r.detail = first_line(r.output) if r.output else ""
    return r


def run_tests(ctx, module, tests, args):
    results = []
    with concurrent.futures.ThreadPoolExecutor(ctx.jobs) as pool:
        for r in pool.map(lambda t: run_one(ctx, module, t), tests):
            results.append(r)
            if args.verbose or r.status not in ("PASS", "BUILD", "SKIP"):
                print(f"{r.status:7} {r.seconds:6.1f}s {r.name}  {r.detail[:120]}", flush=True)
    return results


def save_results(ctx, suite, results, args):
    previous = {}
    path = results_path(ctx, suite)
    if args.filter and path.exists():
        previous = {r["name"]: r for r in json.loads(path.read_text())}
    previous.update({r.name: dataclasses.asdict(r) for r in results})
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(sorted(previous.values(), key=lambda r: r["name"]), indent=1))


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


# ---- CI gate ----------------------------------------------------------------------------------
TRIES = 3  # a failing test that decides the gate runs up to three times in all


def ci_rules(suite):
    """ci.tsv: tests a hosted CI runner treats specially. `flaky`: known to fail intermittently; it runs
    once, with the rule's timeout if it has one, and never decides the job. `skip`: needs what the runner
    does not have (this machine's GPU, the network, or timing a shared runner cannot keep); not run,
    reported as skipped."""
    rules = []
    for line in (HERE / "ci.tsv").read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.startswith("#"):
            name, pattern, kind, reason, *timeout = line.split("\t")
            if name == suite:
                rules.append((re.compile(pattern), kind, reason, float(timeout[0]) if timeout else None))
    return rules


def ci_rule(rules, name):
    """(kind, reason, timeout) of the first rule that matches the whole test name."""
    return next((rule[1:] for rule in rules if rule[0].fullmatch(name)), (None, "", None))


def cmd_gate(ctx, suite, module, args):
    """Runs a suite the way CI does and judges it against the baseline: a test the baseline expects to
    pass (or to build) that fails TRIES times in a row is a regression. Everything else is reported.
    --quick leaves out the tests the baseline expects to fail: they cannot regress, so the verdict is the
    same, but a quick run does not see them start passing (the full run does)."""
    start = time.monotonic()
    rules, baseline = ci_rules(suite), load_baseline(suite)
    tests = load_tests(ctx, suite)
    if args.filter:
        tests = [t for t in tests if re.search(args.filter, t.name)]
    not_run = []
    if args.quick:
        not_run = [t for t in tests if baseline.get(t.name, {}).get("expected") in ("FAIL", "XFAIL", "BUILD", "SKIP")
                   and not t.build_error and ci_rule(rules, t.name)[0] != "flaky"]
        tests = [t for t in tests if t not in not_run]
    for t in tests:
        kind, reason, timeout = ci_rule(rules, t.name)
        if kind == "skip" and not t.skip:
            t.skip = f"not on a CI runner: {reason}"
        if kind == "flaky" and timeout:
            t.timeout = timeout
    results = {r.name: r for r in run_tests(ctx, module, tests, args)}
    for r in results.values():
        # The guest libc++ comes from artifacts/ (scripts/build_alchemy_llvm.ps1), which a CI runner
        # does not have; a C++ test there cannot build.
        if r.status == "BUILD" and re.search(r"libc\+\+\.1\.dylib", r.output):
            r.status, r.detail = "SKIP", "not on a CI runner: needs the guest libc++ from artifacts/"

    def decides(r):
        expected = baseline.get(r.name, {}).get("expected")
        return r.status != "PASS" and (expected == "PASS" or (expected == "FAIL" and r.status == "BUILD"))

    tries = {name: 1 for name in results}
    first = dict(results)  # the first try's result, which an unstable test's report names
    by_name = {t.name: t for t in tests}
    for _ in range(TRIES - 1):
        again = [by_name[n] for n, r in results.items() if r.status not in ("PASS", "SKIP", "BUILD") and
                 decides(r) and ci_rule(rules, n)[0] != "flaky"]
        for r in run_tests(ctx, module, again, args):
            tries[r.name] += 1
            results[r.name] = r
    rows = []
    for name, r in sorted(results.items()):
        expected = baseline.get(name, {}).get("expected")
        kind, reason, _ = ci_rule(rules, name)
        retried = tries[name] > 1
        if r.status == "SKIP":
            verdict = "skipped"
        elif kind == "flaky":
            verdict = "flaky"
        elif expected is None:
            verdict = "new"
        elif r.status == "PASS" and expected == "PASS":
            verdict = "unstable" if retried else "pass"
        elif r.status == "PASS":
            verdict = "newly passing"
        elif decides(r):
            verdict = "regression"
        else:
            verdict = "expected failure"
        detail = r.detail or reason
        if retried and r.status == "PASS":
            detail = f"first try {first[name].status} in {first[name].seconds:.0f}s: {first[name].detail}"
        rows.append({"suite": suite, "test": name, "status": r.status, "expected": expected or "", "verdict": verdict,
                     "tries": tries[name], "seconds": r.seconds, "detail": detail})
    rows += [{"suite": suite, "test": t.name, "status": "", "expected": baseline[t.name]["expected"],
              "verdict": "not run", "tries": 0, "seconds": 0, "detail": "quick run: the baseline expects it to fail"}
             for t in not_run]
    save_results(ctx, suite, list(results.values()), args)
    ctx.gate.append((suite, rows, time.monotonic() - start))


GATE_COLUMNS = ("pass", "expected failure", "not run", "skipped", "flaky", "unstable", "newly passing", "new",
                "regression")


def gate_report(ctx, args):
    """Prints the gate's summary table (and adds it to the GitHub job summary), writes the results TSV,
    and returns whether any regression failed the gate."""
    rows = [row for _, suite_rows, _ in ctx.gate for row in suite_rows]
    lines = ["| suite | tests | " + " | ".join(GATE_COLUMNS) + " | minutes |",
             "|---" * (len(GATE_COLUMNS) + 3) + "|"]
    for suite, suite_rows, seconds in ctx.gate + [("**all**", rows, sum(s for _, _, s in ctx.gate))]:
        counts = [sum(1 for row in suite_rows if row["verdict"] == c) for c in GATE_COLUMNS]
        lines.append(f"| {suite} | {len(suite_rows)} | " + " | ".join(map(str, counts)) + f" | {seconds / 60:.1f} |")
    notes = []
    for verdict, title in (("regression", "Regressions: the baseline expects these to pass; they failed "
                                          f"{TRIES} times in a row"),
                           ("unstable", "Unstable: failed, then passed on a rerun (fix it, or list it as flaky in "
                                        "scripts/conformance/ci.tsv)"),
                           ("flaky", "Known flaky (scripts/conformance/ci.tsv): run once, never decide the job"),
                           ("newly passing", "Newly passing: update the baseline (`report <suite> --write-baseline`)"),
                           ("new", "Not in the baseline yet"),
                           ("skipped", "Skipped on the CI runner")):
        picked = [row for row in rows if row["verdict"] == verdict]
        if verdict == "skipped":
            picked = [row for row in picked if row["detail"].startswith("not on a CI runner")]
        if picked:
            notes += ["", f"{title} ({len(picked)}):"]
            notes += [f"- {row['suite']} {row['test']}: {row['status']} after {row['tries']} "
                      f"{'try' if row['tries'] == 1 else 'tries'}  {row['detail'][:160]}" for row in picked]
    text = "\n".join(lines + notes)
    print("\n" + text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write("## Conformance under alchemy-fx\n\n" + text + "\n")
    if os.environ.get("GITHUB_ACTIONS"):
        for row in rows:
            if row["verdict"] in ("regression", "unstable"):
                level = "error" if row["verdict"] == "regression" else "warning"
                print(f"::{level} title=conformance {row['verdict']}::{row['suite']} {row['test']}: "
                      f"{row['status']}  {row['detail'][:200]}")
    if args.tsv:
        path = pathlib.Path(args.tsv)
        path.parent.mkdir(parents=True, exist_ok=True)
        keys = ("suite", "test", "status", "expected", "verdict", "tries", "seconds", "detail")
        path.write_text("\t".join(keys) + "\n" + "".join(
            "\t".join(str(row[k]).replace("\t", " ").replace("\n", " ") for k in keys) + "\n" for row in rows),
            encoding="utf-8")
    return any(row["verdict"] == "regression" for row in rows)


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
    mac = {r["name"]: reclassify(r, module, ctx) for r in json.loads(mac_path.read_text())}
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


def reclassify(r, module, ctx):
    """Classifies a stored result again from its exit code and output with the current harness, so a
    macOS run made before a harness fix is read the way a new run would be."""
    if r["status"] in ("TIMEOUT", "SKIP", "BUILD") or r["exit_code"] is None or not hasattr(module, "classify"):
        return r
    res = module.classify(Result(**dict(r, status=exit_status(r["exit_code"], r["output"]), detail="")), ctx)
    if not res.detail and res.status != "PASS":
        res.detail = first_line(res.output) if res.output else ""
    return dataclasses.asdict(res)


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
    ap.add_argument("command", choices=("build", "run", "report", "repro", "compare", "gate"))
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
    ap.add_argument("--tsv", help="gate: write every test's status and verdict to this TSV file")
    ap.add_argument("--quick", action="store_true",
                    help="gate: leave out the tests the baseline expects to fail (they cannot regress)")
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
            {"build": cmd_build, "run": cmd_run, "report": cmd_report, "compare": cmd_compare,
             "gate": cmd_gate}[args.command](ctx, name, module, args)
        except SystemExit as e:
            if e.code not in (0, None):
                failed = True
                if isinstance(e.code, str):
                    print(e.code, file=sys.stderr)
    if args.command == "gate" and gate_report(ctx, args):
        failed = True
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
