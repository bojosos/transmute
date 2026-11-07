"""The Open POSIX Test Suite (LTP testcases/open_posix_testsuite, GPL-2.0), conformance/interfaces.

GPL-2.0: nothing of it is copied into this repository. It is built from the extracted release under
--sources into --out, and this script only names those paths.

Each <interface>/<assertion>-<n>.c is one program; *-buildonly.c files only have to compile.
Exit codes follow include/posixtest.h: 0 PASS, 1 FAIL, 2 UNRESOLVED, 4 UNSUPPORTED (an optional
POSIX feature the system does not offer), 5 UNTESTED. UNRESOLVED counts as a failure;
UNSUPPORTED and UNTESTED are SKIP with the reason.
"""
import re

import conformance as c

FLAGS = ["-std=gnu99", "-D_POSIX_C_SOURCE=200809L", "-D_XOPEN_SOURCE=700", "-D_DARWIN_C_SOURCE",
         "-D_FORTIFY_SOURCE=0", "-Wno-everything"]
AREAS = [(r"^pthread_(mutex|cond|rwlock|spin|barrier|once)", "sync"), (r"^pthread_", "threads"),
         (r"^sem_", "semaphores"), (r"^(mq_)", "message-queues"), (r"^(sig|kill|raise|alarm|pause|killpg)", "signals"),
         (r"^(timer_|clock|nanosleep|time)", "timers-clocks"), (r"^(sched_)", "scheduling"),
         (r"^(mmap|munmap|mprotect|mlock|munlock|msync|shm_|posix_madvise|mlockall|munlockall)", "memory"),
         (r"^(aio_|lio_)", "async-io"), (r"^(fork|exec|posix_spawn|wait)", "process"),
         (r"^(posix_fadvise|posix_fallocate|fsync|fdatasync|open|close|read|write|dup)", "files")]
# Optional POSIX features Darwin does not provide (expected failures, not owner bugs).
NOT_ON_DARWIN = r"^(mq_|timer_|clock_nanosleep|clock_getcpuclockid|pthread_barrier|pthread_spin|sem_init|sem_destroy|" \
                r"sem_timedwait|pthread_mutex_timedlock|pthread_rwlock_timed|pthread_condattr_setclock|" \
                r"pthread_condattr_getclock|posix_fallocate|sched_rr_get_interval|pthread_getcpuclockid|" \
                r"aio_fsync|lio_listio|sigqueue|sigwaitinfo|sigtimedwait|mlockall)"


def owner(name):
    return "fw-system"


def area(name):
    iface = name.split("/")[1]
    return next((a for pattern, a in AREAS if re.search(pattern, iface)), "misc")


def not_on_darwin(name):
    return bool(re.search(NOT_ON_DARWIN, name.split("/")[1]))


def build(ctx):
    root = ctx.source("ltp") / "testcases" / "open_posix_testsuite"
    interfaces = root / "conformance" / "interfaces"
    out = ctx.out / "posix"
    flags = [*FLAGS, "-I", str(root / "include")]
    # lib/common.c is the suite's main(), which calls each test's test_main().
    common = out / "obj" / "lib" / "common.o"
    common_err = ctx.compile(root / "lib" / "common.c", common, flags)

    def one(path):
        rel = path.relative_to(interfaces)
        iface = rel.parts[0]
        stem = "/".join(rel.with_suffix("").parts[1:])
        name = f"posix/{iface}/{stem}"
        obj = out / "obj" / rel.with_suffix(".o")
        exe = out / "bin" / rel.with_suffix("")
        err = ctx.compile(path, obj, flags)
        if name.endswith("-buildonly"):
            return c.Test(name=name, exe=exe, build_error=err, skip="" if err else "build-only test (it compiles)")
        if not err:
            err = common_err or ctx.link([obj, common], exe, libs=("System", "pthread", "m"))
        return c.Test(name=name, exe=exe, timeout=60, cwd=out / "run" / iface, build_error=err)

    files = sorted(p for p in interfaces.rglob("*.c")
                   if "testfrmw" not in p.parts and re.match(r"^\d+-\d+(-buildonly)?$", p.stem))
    return c.parallel(one, files)


def classify(result, ctx):
    code = result.exit_code
    reason = next((l for l in result.output.splitlines() if l.strip()), "")[:300]
    if result.status in ("FAIL", "CRASH") and code == 2:
        result.status, result.detail = "FAIL", f"UNRESOLVED: {reason}"
    elif code == 4:
        result.status, result.detail = "SKIP", f"UNSUPPORTED: {reason}"
    elif code == 5:
        result.status, result.detail = "SKIP", f"UNTESTED: {reason}"
    elif result.status != "PASS":
        result.detail = reason
    return result


BUILD_NOT_ON_DARWIN = r"mqueue\.h|'timer_t'|SIGRTMIN|SIGRTMAX|SIGPOLL|pthread_spinlock_t|pthread_barrier|" \
                      r"struct itimerspec|TIMER_ABSTIME|PTHREAD_BARRIER_SERIAL_THREAD|_NP'"


# Interfaces the suite calls that macOS's headers do not declare at all (checked against the sysroot).
# The suite compiles without prototypes, so these show up as unresolved imports at load time.
NOT_DECLARED_ON_DARWIN = {"clock_nanosleep", "pthread_condattr_getclock", "pthread_condattr_setclock",
                          "pthread_getattr_np", "pthread_getcpuclockid", "pthread_mutex_timedlock",
                          "pthread_rwlock_timedrdlock", "pthread_rwlock_timedwrlock", "sched_getparam",
                          "sched_getscheduler", "sched_rr_get_interval", "sched_setparam", "sched_setscheduler",
                          "sem_timedwait", "sigqueue", "sigtimedwait", "sigwaitinfo"}


def expected_failure(result):
    if not_on_darwin(result.name):
        return "optional POSIX feature macOS does not provide"
    m = re.search(r"unresolved strong import _(\w+)", result.detail or "")
    if m and m.group(1) in NOT_DECLARED_ON_DARWIN:
        return f"{m.group(1)} is not in macOS's headers"
    if result.status == "BUILD" and re.search(BUILD_NOT_ON_DARWIN, result.output or result.detail):
        return "uses a POSIX option or extension macOS does not provide"
    return None
