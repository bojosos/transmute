#!/usr/bin/env python3
"""The Linux CI job's tests (.github/workflows/ci.yml, job "linux").

    linux_ctest.py build <build dir> <list>   build what the listed tests run
    linux_ctest.py test  <build dir> <list>   run the listed tests with ctest

<list> names one ctest test per line (scripts/ci/linux_tests.txt; '#' starts a
comment). Many test targets do not build on Linux yet (Windows-only test
sources), so the job builds only the files the listed tests' commands name:
executables and the Mach-O fixtures, whatever ninja knows how to make. A
listed name that ctest does not know fails the job.
"""
import json
import os
import re
import subprocess
import sys


def listed(path):
    names = []
    with open(path, encoding="utf-8") as file:
        for line in file:
            name = line.split("#", 1)[0].strip()
            if name:
                names.append(name)
    return names


def pattern(names):
    return "^(" + "|".join(re.escape(name) for name in names) + ")$"


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("build", "test"):
        sys.exit(__doc__)
    mode, build, list_path = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]
    names = listed(list_path)
    regex = pattern(names)
    shown = json.loads(subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1", "-R", regex],
                                      check=True, capture_output=True, text=True).stdout)
    known = {test["name"] for test in shown["tests"]}
    missing = [name for name in names if name not in known]
    if missing:
        sys.exit("linux_ctest.py: not ctest tests: " + ", ".join(missing))
    if mode == "test":
        jobs = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL") or str(os.cpu_count() or 2)
        sys.exit(subprocess.run(["ctest", "--test-dir", build, "-R", regex, "--output-on-failure", "--timeout", "600",
                                 "-j", jobs]).returncode)
    # Every build-directory path a listed test's command names (arguments and
    # -DNAME=<path> values) that is an output ninja can make. The commands come
    # from the CTestTestfile.cmake files: before the build, ctest's JSON leaves
    # out the command of a test whose executable does not exist yet.
    outputs = set(line.rsplit(":", 1)[0] for line in subprocess.run(
        ["ninja", "-C", build, "-t", "targets", "all"], check=True, capture_output=True, text=True).stdout.splitlines())
    commands = {}
    for directory, _, files in os.walk(build):
        if "CTestTestfile.cmake" not in files:
            continue
        with open(os.path.join(directory, "CTestTestfile.cmake"), encoding="utf-8") as file:
            for line in file:
                if not line.startswith("add_test("):
                    continue
                # Quoted ("...") and bracket ([=[...]=]) arguments; the first is the test's name.
                tokens = [quoted or bracketed for quoted, _, bracketed in
                          re.findall(r'"((?:[^"\\]|\\.)*)"|\[(=*)\[(.*?)\]\2\]', line)]
                if tokens:
                    commands.setdefault(tokens[0], []).extend(tokens[1:])
    wanted = set()
    for name in known:
        for argument in commands.get(name, []):
            for part in re.split(r"[=;\s]", argument):
                # Custom commands' outputs (the Mach-O fixtures) are listed by absolute path.
                if part in outputs:
                    wanted.add(part)
                elif part.startswith(build + "/") and os.path.relpath(part, build) in outputs:
                    wanted.add(os.path.relpath(part, build))
    print(f"linux_ctest.py: {len(known)} tests need {len(wanted)} outputs", flush=True)
    jobs = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL")
    sys.exit(subprocess.run(["ninja", "-C", build] + (["-j", jobs] if jobs else []) + sorted(wanted)).returncode)


if __name__ == "__main__":
    main()
