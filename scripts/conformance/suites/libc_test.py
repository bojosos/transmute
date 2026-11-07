"""musl's libc-test (MIT): functional, regression and math tests, built against the conformance
sysroot and libtest (src/common). api/ is skipped: it only compiles declarations, so it would test
Apple's headers rather than Alchemy. musl/ tests musl internals.

A test passes when it exits 0 (t_status); failures print "file:line: message" through t_error.
libc-test targets POSIX as musl implements it, so some tests expect behaviour Darwin does not have;
those are XFAIL in the baseline with the reason.
"""
import re
import shutil

import conformance as c

DIRS = ("functional", "regression", "math")
# upstream config.mak.def CFLAGS, minus GCC-only and glibc settings. Tests that ask for BSD or GNU
# extensions (_BSD_SOURCE, _GNU_SOURCE) get Darwin's equivalent, _DARWIN_C_SOURCE.
# _FORTIFY_SOURCE=0: the suite checks libc behaviour, not the __*_chk entry points Libc's headers
# substitute when optimizing (those are checked separately; results.md).
CFLAGS = ["-std=c99", "-D_POSIX_C_SOURCE=200809L", "-D_DARWIN_C_SOURCE", "-D_FORTIFY_SOURCE=0", "-fno-builtin", "-frounding-math", "-Wno-unknown-pragmas",
          "-Wno-unused", "-Wno-overflow", "-Wno-missing-braces", "-Werror=implicit-function-declaration",
          "-Werror=implicit-int"]
# Helper libraries built from <name>.c next to the tests (the *.mk files): {dso: tests that need it}.
DSOS = {"functional/dlopen_dso": [], "functional/tls_align_dso": ["functional/tls_align"],
        "functional/tls_init_dso": [], "regression/tls_get_new-dtv_dso": []}
NOT_TESTS = set(DSOS)
# These cap the address space with setrlimit and then allocate until allocation fails. While
# Alchemy's setrlimit is ENOSYS they would allocate host memory without bound, so they are skipped.
NEEDS_RLIMIT = {"regression/malloc-oom", "regression/malloc-brk-fail", "regression/pthread_create-oom",
                "regression/setenv-oom"}
AREAS = [(r"^math/", "math"), (r"pthread|sem_|tls|cancel|mutex|cond|rwlock|barrier", "threads"),
         (r"dlopen|tls_", "loader"), (r"printf|scanf|strto|ato|wcs|mbs|mbc|iconv|wcto|swprintf|fwscanf|utf|locale",
                                       "stdio-locale"),
         (r"spawn|popen|fork|exec|wait|kill|sig|setjmp|rlimit", "process-signals"),
         (r"socket|inet|getaddr|dns|resolv", "network"), (r"time|clock|strftime|strptime", "time"),
         (r"string|mem|str", "string"), (r"stat|fcntl|fdopen|dir|path|file|ungetc|fopen|lseek|ftello", "files")]


def owner(name):
    return "fw-system"


def area(name):
    return next((a for pattern, a in AREAS if re.search(pattern, name)), name.split("/")[0])


def build(ctx):
    upstream = ctx.source("libc-test") / "src"
    out = ctx.out / "libc-test"
    src = out / "src"
    if src.exists():
        shutil.rmtree(src)
    shutil.copytree(upstream, src)
    flags = CFLAGS + ["-I", str(src / "common")]

    common_err, libtest = [], []
    for f in sorted((src / "common").glob("*.c")):
        if f.stem in ("runtest",):
            continue
        obj = out / "obj" / "common" / f"{f.stem}.o"
        err = ctx.compile(f, obj, flags)
        if err:
            common_err.append(f"{f.name}: {err}")
        libtest.append(obj)
    common_err = "\n".join(common_err)

    dso_err = {}
    for dso in DSOS:
        d, name = dso.split("/")
        obj = out / "obj" / d / f"{name}.o"
        lib = src / d / f"{name}.so"
        err = ctx.compile(src / d / f"{name}.c", obj, flags + ["-fPIC"]) or ctx.link(
            [obj], lib, flags=["-dynamiclib", f"-Wl,-install_name,@loader_path/{name}.so"])
        dso_err[dso] = err

    def one(f):
        d, stem = f.parent.name, f.stem
        name = f"{d}/{stem}"
        obj = out / "obj" / d / f"{stem}.o"
        exe = src / d / stem
        extra = [str(src / f"{dso}.so") for dso, users in DSOS.items() if name in users]
        err = common_err or next((dso_err[dso] for dso, users in DSOS.items() if name in users and dso_err[dso]), "")
        err = err or ctx.compile(f, obj, flags) or ctx.link([obj, *libtest], exe, libs=("System", "m", "pthread"),
                                                           flags=extra)
        # The math tests walk thousands of vectors in every rounding mode: minutes under translation.
        skip = "allocates until failure; needs setrlimit(RLIMIT_AS), ENOSYS today" if name in NEEDS_RLIMIT else ""
        return c.Test(name=name, exe=exe, timeout=300 if d == "math" else 120, cwd=out, build_error=err, skip=skip)

    files = [f for d in DIRS for f in sorted((src / d).glob("*.c")) if f"{d}/{f.stem}" not in NOT_TESTS]
    tests = c.parallel(one, files)
    return tests


def classify(result, ctx):
    errors = [line for line in result.output.splitlines() if re.match(r"^src/\S+:\d+: ", line) or
              re.match(r"^\S+\.c:\d+: ", line)]
    if errors:
        result.detail = errors[0][:300]
        if result.status == "PASS":
            # t_error printed but the exit status was 0: libc-test counts that as a failure.
            result.status = "FAIL"
    return result


# Interfaces and behaviour musl or Linux have and Darwin does not (BUILD or run failures that are not
# Alchemy bugs). Matched against the build error or the failure detail.
NOT_ON_DARWIN = [
    (r"'clearenv'", "clearenv is not on Darwin"),
    (r"'sem_timedwait'|'sem_init'", "unnamed and timed semaphores are not on Darwin"),
    (r"pthread_barrier", "pthread barriers are not on Darwin"),
    (r"SOCK_CLOEXEC", "SOCK_CLOEXEC is not on Darwin"),
    (r"SIGRTMIN|SIGRTMAX", "real-time signals are not on Darwin"),
    (r"'strverscmp'", "strverscmp is GNU-only"),
    (r"mntent\.h", "mntent.h is Linux-only"),
    (r"crypt\.h", "Darwin has no crypt.h"),
    (r"'pthread_condattr_setclock'", "pthread_condattr_setclock is not on Darwin"),
    (r"sys/syscall\.h", "uses Linux syscall numbers"),
    (r"mk[ds]temp\(/dev/null/fooXXXX\)",
     "musl rejects a template with fewer than six X's (EINVAL); Darwin accepts it and fails with ENOTDIR"),
    (r"st_atim'", "Darwin's struct stat has st_atimespec"),
    (r"'(drem|dremf|exp10|exp10f|exp10l|pow10|pow10f|pow10l|scalbf|sincos|sincosf|sincosl|j0f|j1f|jnf|y0f|y1f|ynf)'",
     "musl/glibc-only math function"),
]


def expected_failure(result):
    text = (result.detail or "") + "\n" + (result.output or "")[:3000]
    return next((reason for pattern, reason in NOT_ON_DARWIN if re.search(pattern, text)), None)
