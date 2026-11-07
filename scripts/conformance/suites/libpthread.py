"""Apple libpthread's own unit tests (libpthread-539.100.4 tests/, APSL-2.0) on the darwintest harness."""
import re

import _darwintest as dt

# tests/Makefile EXCLUDED_SOURCES; the JIT write-protection tests need entitlements and helper tools.
EXCLUDED = {"wq_cooperative": "excluded upstream",
            "pthread_jit_write_with_callback": "needs entitlements and a helper tool",
            "pthread_jit_write_freeze_callbacks": "needs entitlements and a helper tool",
            "pthread_jit_write_lockdown": "needs entitlements", "pthread_jit_write_protection": "needs entitlements"}
AREAS = [(r"mutex|cond|rwlock|once", "sync"), (r"wq_|workq|qos|priority", "workqueue-qos"),
         (r"stack", "stacks"), (r"cancel|exit|join|detach|create|bulk", "lifecycle"),
         (r"tsd|setspecific|key", "tsd"), (r"atfork|fork", "fork")]


def owner(name):
    return "fw-system"


def area(name):
    base = name.split("/", 1)[1]
    return next((a for pattern, a in AREAS if re.search(pattern, base)), "misc")


def build(ctx):
    src = ctx.source("libpthread")
    return dt.build_suite(ctx, "libpthread", src / "tests", owner, excluded=EXCLUDED,
                          extra_includes=[src / "private", src / "include"],
                          # tests/Makefile per-test flags
                          file_flags={"main_stack_custom": ["-DSTACKSIZE=0x124000"],
                                      "bsdthread_set_self": ["-D_DARWIN_FEATURE_CLOCK_GETTIME"]},
                          file_link_flags={"main_stack_custom": ["-Wl,-stack_size,0x124000"]})


classify = dt.classify
