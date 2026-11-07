"""libdispatch's own unit tests (swift-corelibs-libdispatch swift-6.2-RELEASE tests/, Apache-2.0).

The tests are copied out of the pinned release (sources.json; the same tag as the Swift runtime
build), patched (PATCHES below) and built against the conformance sysroot, with Apple's
libdispatch-1542 private headers and config.h. Each test is run directly: bsdtests.c prints
[PASS]/[FAIL] per check and exits 0 or 0xff. (bsdtestharness only adds leak checks and timeouts.)
"""
import pathlib
import re
import shutil

import conformance as c

# DISPATCH_C_TESTS, the EXTENDED_TEST_SUITE additions and the Darwin-only tests from tests/CMakeLists.txt.
TESTS = ["apply", "api", "debug", "queue_finalizer", "overcommit", "context_for_key", "after", "timer",
         "timer_short", "timer_timeout", "sema", "timer_bit31", "timer_bit63", "timer_set_time", "data",
         "io_muxed", "io_net", "io_pipe", "io_pipe_close", "select",
         "priority", "concur", "group", "read", "read2", "starfish", "suspend_timer", "pingpong", "drift",
         "readsync", "cascade", "io",
         "deadname", "proc", "vm", "vnode", "c99"]
TIMEOUTS = {"io_pipe": 15, "io_pipe_close": 5}
# (file, old, new): portability fixes to the test sources, applied to the copy.
PATCHES = [
    # A fallback prototype for platforms without <sys/sysctl.h>; its last parameter (size_t *) contradicts
    # Darwin's real declaration (size_t), so it cannot compile next to it.
    ("dispatch_test.h", "#if __APPLE__\nint sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp,\n\t\tsize_t *newpl);\n#endif\n", ""),
]
AREAS = {"io": "io", "read": "io", "select": "io", "vnode": "sources", "proc": "sources", "deadname": "mach",
         "timer": "timers", "after": "timers", "drift": "timers", "suspend": "timers", "vm": "sources"}


def owner(name):
    return "fw-system"


def area(name):
    base = name.split("/", 1)[1]
    return next((a for k, a in AREAS.items() if base.startswith(k)), "queues")


def build(ctx):
    upstream = ctx.source("swift-corelibs-libdispatch")
    apple = ctx.source("libdispatch")
    out = ctx.out / "dispatch"
    src = out / "src"
    if src.exists():
        shutil.rmtree(src)
    shutil.copytree(upstream / "tests", src)
    for name, old, new in PATCHES:
        path = src / name
        text = path.read_text()
        if old not in text:
            raise SystemExit(f"dispatch: patch for {name} no longer applies")
        path.write_text(text.replace(old, new))
    inc = out / "inc"
    if inc.exists():
        shutil.rmtree(inc)
    shutil.copytree(apple / "private", inc / "dispatch")
    shutil.copytree(apple / "config", inc / "config")
    shutil.copytree(c.HERE / "shims" / "CoreFoundation", inc / "CoreFoundation")
    flags = ["-I", str(inc), "-I", str(src), "-DNOLEAKS=1"]

    harness, errors = [], []
    for name in ("bsdtests", "dispatch_test"):
        obj = out / "obj" / f"{name}.o"
        err = ctx.compile(src / f"{name}.c", obj, flags)
        harness.append(obj)
        if err:
            errors.append(f"{name}.c: {err}")
    tests = []
    for t in TESTS + ["plusplus"]:
        cxx = t == "plusplus"
        source = src / (f"dispatch_{t}.cpp" if cxx else f"dispatch_{t}.c")
        obj, exe = out / "obj" / f"dispatch_{t}.o", out / "bin" / f"dispatch_{t}"
        err = "\n".join(errors) or ctx.compile(source, obj, flags, cxx=cxx)
        if not err:
            err = ctx.link([obj, *harness], exe, flags=ctx.cxx_link_flags() if cxx else [])
        tests.append(c.Test(name=f"dispatch/{t}", exe=exe, timeout=TIMEOUTS.get(t, 120), build_error=err,
                            env={"NOLEAKS": "1"}))
    return tests


def classify(result, ctx):
    fails = re.findall(r"(?m)^\[FAIL\].*$", result.output)
    if fails:
        result.detail = fails[0][:300]
    return result
