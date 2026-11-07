"""Apple Libc's own unit tests (Libc-1752.120.2 tests/, APSL-2.0) on the darwintest harness."""
import re

import _darwintest as dt

# tests/Makefile EXCLUDED_SOURCES, plus tests that drive tools a guest does not have.
EXCLUDED = {"locale": "excluded upstream", "os_boot_mode": "excluded upstream", "qsort_perf": "excluded upstream",
            "fflush": "drives hdiutil (disk images)"}
AREAS = [(r"printf|scanf|strto|wcs|mbs|wchar|locale|collat|ctype|rune", "stdio-locale"),
         (r"time|strftime|strptime|tz", "time"), (r"str|mem|bcopy", "string"),
         (r"qsort|psort|heap|sort", "sort"), (r"os_|assumes|crash|abort", "os"),
         (r"fts|glob|dir|path|realpath|stat|file|fopen|fflush|fmemopen|open_memstream|stdio", "files"),
         (r"exec|spawn|popen|system|fork|signal", "process-signals"), (r"net|inet|ether|addr", "network")]


def owner(name):
    return "fw-system"


def area(name):
    base = name.split("/", 1)[1]
    return next((a for pattern, a in AREAS if re.search(pattern, base)), "misc")


def build(ctx):
    libc = ctx.source("Libc")
    # Libc's tests use a few Libc-internal headers (libc_private.h and friends) from the sources.
    return dt.build_suite(ctx, "libc_apple", libc / "tests", owner, excluded=EXCLUDED,
                          extra_includes=[libc / "darwin"])


classify = dt.classify
