"""Shared driver for Apple's open-source darwintest suites (suites/libc_apple.py, libpthread.py).

Each tests/<file>.c is built into one program with the conformance darwintest harness
(darwintest/), and each T_DECL in it is one conformance test, run as `<program> <case>`.
T_META options are read from the source: T_META_ENABLED(false) and performance tests are skipped,
T_META_ASROOT(true) is skipped (guests run as uid 1000), T_META_ENVVAR sets the environment and
T_META_TIMEOUT the timeout.
"""
import re
import subprocess
import shutil

import conformance as c

DECL = re.compile(r"\bT_DECL\s*\(\s*(\w+)\s*,(.*?)\)\s*\{", re.S)
GLOBAL_META = re.compile(r"\bT_GLOBAL_META\s*\((.*?)\)\s*;", re.S)
# Warnings off (the tests are written for -Weverything with Apple's headers), but an undeclared
# function stays an error: an implicit declaration would link to a wrong or missing name.
FLAGS = ["-DDARWINTEST", "-std=gnu11", "-D_DARWIN_C_SOURCE", "-Wno-everything",
         "-Werror=implicit-function-declaration"]
REGISTERED = re.compile(r"_t_register\s*\(\s*\"(\w+)\"\s*,\s*_t_decl_\w+\s*,\s*0\s*\)")


def metas(text):
    skip = None
    env = {}
    timeout = None
    if re.search(r"T_META_ENABLED\s*\(\s*(false|0|FALSE)\s*\)", text):
        skip = "T_META_ENABLED(false)"
    elif re.search(r"T_META_TYPE_PERF|T_META_TAG_PERF", text):
        skip = "performance test"
    elif re.search(r"T_META_ASROOT\s*\(\s*(true|1|TRUE)\s*\)", text):
        skip = "T_META_ASROOT(true): needs root"
    elif re.search(r"T_META_BOOTARGS_SET", text):
        skip = "needs boot-args"
    for kv in re.findall(r"T_META_ENVVAR\s*\(\s*\"([^\"]*)\"\s*\)", text):
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v
    m = re.search(r"T_META_TIMEOUT\s*\(\s*(\d+)\s*\)", text)
    if m:
        timeout = int(m.group(1))
    return skip, env, timeout


def build_suite(ctx, suite, source_dir, owner_of, excluded=(), extra_flags=(), extra_includes=(),
                file_flags=None, file_link_flags=None):
    out = ctx.out / suite
    if out.exists():
        shutil.rmtree(out)
    src = out / "src"
    shutil.copytree(source_dir, src)
    flags = [*FLAGS, "-I", str(c.HERE / "darwintest"), "-I", str(src), *extra_flags]
    for inc in extra_includes:
        flags += ["-I", str(inc)]
    harness = out / "obj" / "darwintest.o"
    harness_err = ctx.compile(c.HERE / "darwintest" / "darwintest.c", harness, flags)

    def one(path):
        text = path.read_text(errors="replace")
        stem = path.stem
        exe = out / "bin" / stem
        decls = DECL.findall(text)
        if not decls:
            return []
        file_skip = None
        if "darwintest_perf.h" in text:
            file_skip = "performance test (darwintest_perf.h)"
        elif stem in excluded:
            file_skip = excluded[stem] if isinstance(excluded, dict) else "excluded upstream"
        gskip, genv, gtimeout = metas(" ".join(GLOBAL_META.findall(text)))
        err = ""
        if not file_skip:
            obj = out / "obj" / f"{stem}.o"
            err = harness_err or ctx.compile(path, obj, flags + (file_flags or {}).get(stem, [])) or \
                ctx.link([obj, harness], exe, flags=(file_link_flags or {}).get(stem, []))
        if not err and not file_skip:
            # The cases the program really has: T_DECLs in disabled #if blocks are not compiled.
            pre = subprocess.run([str(ctx.clang), *ctx.cflags(), *flags, *(file_flags or {}).get(stem, []), "-E",
                                  str(path)], capture_output=True, text=True, errors="replace").stdout
            compiled = set(REGISTERED.findall(pre))
            if compiled:
                decls = [(name, meta) for name, meta in decls if name in compiled]
        tests = []
        for name, meta in decls:
            skip, env, timeout = metas(meta)
            tests.append(c.Test(name=f"{suite}/{stem}/{name}", exe=exe, args=[name], timeout=timeout or gtimeout or 120,
                                env={**genv, **env}, cwd=out / "run" / stem, build_error=err,
                                skip=file_skip or gskip or skip or ""))
        return tests

    files = sorted(src.glob("*.c"))
    return [t for group in c.parallel(one, files) for t in group]


def classify(result, ctx):
    lines = result.output.splitlines()
    fails = [l for l in lines if l.startswith("[FAIL]")]
    skip = [l for l in lines if l.startswith("[SKIP]")]
    if skip and result.exit_code == 0:
        result.status, result.detail = "SKIP", skip[0][7:300]
    elif result.status == "PASS" and not any(l.startswith("[PASS]") for l in lines):
        result.status, result.detail = "FAIL", "exited 0 without finishing the case"
    elif fails:
        result.detail = fails[0][:300]
    return result
