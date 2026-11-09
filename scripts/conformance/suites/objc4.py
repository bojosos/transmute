"""objc4's runtime tests (objc4-951.7 test/, APSL-2.0), driven the way upstream test.pl drives them.

Each test file may carry TEST_CONFIG (conditions), TEST_BUILD (build commands), TEST_ENV,
TEST_RUN_OUTPUT (a regex the output must match), TEST_CRASHES and TEST_DISABLED. Without
TEST_BUILD the test is compiled alone. A run passes when its output is exactly "OK: <name>"
(test.h's succeed()) or matches TEST_RUN_OUTPUT, with no "BAD:" line. The configuration is
test.pl's default for macOS arm64: MEM=arc unless the test allows only mrc.

Not supported, so SKIP: Swift tests, and TEST_BUILD recipes that use the shell (pipes, redirection)
or test.pl variables with no conformance equivalent (TESTLIB, XCRUN, SDK_PATH, ...).
"""
import json
import os
import re
import shlex
import shutil
import subprocess

import conformance as c

CONFIG = {"OS": "macosx", "ARCH": "arm64", "CC": "clang", "CXX": "clang++", "SDK": "macosx", "BATS": "0",
          "GUARDMALLOC": "0", "BUILD": "1"}
EXT_LANGUAGE = {".m": "objective-c", ".mm": "objective-c++", ".c": "c", ".cpp": "c++", ".swift": "swift"}
WARNINGS = ["-W", "-Wall", "-Wno-deprecated-volatile", "-Wno-undef-prefix", "-Wno-unknown-warning-option",
            "-Wno-objc-load-method", "-Wno-objc-weak-compat", "-Wno-arc-bridge-casts-disallowed-in-nonarc",
            "-Wno-missing-field-initializers", "-Qunused-arguments", "-fno-caret-diagnostics"]
AREAS = [(r"weak|arr-", "weak-arc"), (r"rr|autorelease|retain|release|bigrc|pool", "refcount"),
         (r"load|unload|image|duplicate|future|ivarSlide|weakimport|weakframework", "images"),
         (r"msgSend|forward|cache|resolve|super|methodArgs|exchangeImp|addMethod", "messaging"),
         (r"synchron|sync", "sync"), (r"tagged", "tagged-pointers"), (r"exc|unwind|AltHandler", "exceptions"),
         (r"associat", "associated-objects"), (r"initialize|fork", "initialize-fork")]
UNSUPPORTED_VARS = re.compile(r"\$C\{(TESTLIB|XCRUN|SDK_PATH|FORCE_LOAD_ARCLITE|TESTINCLUDEDIR|TESTLOCALINCLUDEDIR|ARCH)\}|\$VERBOSE")


def owner(name):
    return "fw-system"


def area(name):
    base = name.split("/", 1)[1]
    return next((a for pattern, a in AREAS if re.search(pattern, base, re.I)), "metadata")


def multiline(key, text):
    """test.pl extract_multiple_multiline: KEY on its own line, then lines up to END."""
    blocks = re.findall(rf"(?ms)^[ \t]*{key}[ \t]*\n(.*?)^[ \t]*END[ \t]*$", text)
    return ["\n".join(line.strip() for line in b.strip("\n").splitlines()) for b in blocks]


def conditions(text, language):
    m = re.search(r"\bTEST_CONFIG\b(.*)$", text, re.M)
    conds = {"MEM": ["arc"]}
    if m:
        for item in m.group(1).split():
            if "=" in item:
                k, v = item.split("=", 1)
                conds[k] = v.split(",")
    conds.setdefault("LANGUAGE", [language])
    return conds


def allowed(values, value):
    values = [{"objc": "objective-c", "objc++": "objective-c++", "!objc": "!objective-c",
               "!objc++": "!objective-c++"}.get(v, v) for v in values]
    positive = [v for v in values if not v.startswith("!")]
    negative = [v[1:] for v in values if v.startswith("!")]
    return value not in negative and (not positive or value in positive)


def build(ctx):
    upstream = ctx.source("objc4")
    out = ctx.out / "objc4"
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(upstream / "test", out / "test")
    shutil.copytree(upstream / "runtime", out / "runtime")  # test.h includes ../runtime/isa.h
    inc = out / "inc" / "objc"
    inc.mkdir(parents=True)
    for h in (upstream / "runtime").glob("*.h"):
        shutil.copyfile(h, inc / h.name)  # objc4's private headers (objc-internal.h, objc-abi.h)
    # The tests use objc's pointer-authentication qualifiers, which a newer <ptrauth.h> than xnu's
    # defines. arm64 (not arm64e) has no pointer authentication, so they are empty.
    (out / "inc" / "ptrauth.h").write_text(
        "#include_next <ptrauth.h>\n" + "".join(
            f"#ifndef {m}\n#define {m}\n#endif\n" for m in (
                "__ptrauth_objc_isa_pointer", "__ptrauth_objc_super_pointer", "__ptrauth_objc_class_ro",
                "__ptrauth_objc_method_list_imp", "__ptrauth_objc_method_list_pointer")))
    testdir = out / "test"
    # Recipes name /dev/null as an empty input (rawisa's -sectcreate); a Windows linker would open
    # \dev\null on the current drive instead.
    empty = out / "empty"
    empty.write_bytes(b"")

    def host_path(arg):
        return str(empty) if arg == "/dev/null" else arg

    def compiler(language, mem, link=True):
        cxx = language in ("objective-c++", "c++")
        cmd = [str(ctx.clangxx if cxx else ctx.clang), *ctx.cflags(cxx=cxx), "-I", str(out / "inc"),
               "-I", str(testdir), *WARNINGS, "-Os", "-fuse-ld=lld", "-Wl,-undefined,dynamic_lookup",
               # succeed() prints basename(__FILE__); a Windows path has no '/' for the guest to split.
               f"-fmacro-prefix-map={testdir}{os.sep}=", f"-fmacro-prefix-map={testdir.as_posix()}/="]
        cmd[cmd.index("-O1")] = "-Os"
        cmd += {"c": ["-x", "c", "-std=gnu17"], "c++": ["-x", "c++", "-std=gnu++20"],
                "objective-c": ["-x", "objective-c", "-std=gnu17"],
                "objective-c++": ["-x", "objective-c++", "-std=gnu++20"]}[language]
        if language.startswith("objective"):
            cmd += ["-fobjc-arc"] if mem == "arc" else []
        if link:
            # Libraries go before -x, which would otherwise make them source files.
            libs = (["-lobjc"] if language.startswith("objective") else []) + (ctx.cxx_link_flags() if cxx else [])
            i = cmd.index("-x")
            cmd[i:i] = libs
        else:
            cmd += ["-c"]
        return [a for a in cmd if a != "-std=c++17"]

    def one(path):
        name = path.stem
        text = path.read_text(errors="replace")
        language = EXT_LANGUAGE[path.suffix]
        test = c.Test(name=f"objc4/{name}", exe=out / "build" / name / f"{name}.exe", timeout=120,
                      cwd=out / "build" / name)
        disabled = re.search(r"\bTEST_DISABLED\b(.*)$", text, re.M)
        if disabled:
            return test, "SKIP", f"TEST_DISABLED{disabled.group(1)}"
        conds = conditions(text, language)
        for key, values in conds.items():
            if key == "RUN" and values == ["0"]:
                return test, "SKIP", "RUN=0 (build-only test)"
            if key == "MEM":
                continue
            value = CONFIG.get(key, language if key == "LANGUAGE" else None)
            if value is not None and not allowed(values, value):
                return test, "SKIP", f"TEST_CONFIG {key}={','.join(values)}"
        if language == "swift":
            return test, "SKIP", "Swift test"
        mem = "arc" if allowed(conds["MEM"], "arc") else "mrc"
        env = {}
        m = re.search(r"\bTEST_ENV\b(.*)$", text, re.M)
        if m:
            env = dict(kv.split("=", 1) for kv in m.group(1).split() if "=" in kv)
        test.env = env
        builddir = test.cwd
        builddir.mkdir(parents=True, exist_ok=True)
        recipe = multiline("TEST_BUILD", text)
        if recipe:
            script = recipe[0]
            if UNSUPPORTED_VARS.search(script) or re.search(r"[|;>`]|&&", script.replace("2>&1", "")):
                return test, "SKIP", "TEST_BUILD recipe needs the shell or test.pl internals"
            commands = []
            for line in script.splitlines():
                line = line.strip()
                if not line:
                    continue
                line = line.replace("\\\\", "\\").replace("$DIR", str(testdir).replace("\\", "/"))
                line = line.replace("$T{DYLIBDIR}", "@loader_path").replace("$T{OBJDIR}", ".")
                for var, lang, link in (("COMPILE_NOLINK_NOMEM", language, False), ("COMPILE_NOLINK", language, False),
                                        ("COMPILE_C", "c", True), ("COMPILE_CXX", "c++", True),
                                        ("COMPILE_MM", "objective-c++", True), ("COMPILE_M", "objective-c", True),
                                        ("COMPILE", language, True)):
                    line = line.replace(f"$C{{{var}}}", f"@@{var}@@")
                args = []
                for tok in shlex.split(line, posix=True):
                    m = re.fullmatch(r"@@(\w+)@@", tok)
                    if m:
                        var = m.group(1)
                        lang = {"COMPILE_C": "c", "COMPILE_CXX": "c++", "COMPILE_MM": "objective-c++",
                                "COMPILE_M": "objective-c"}.get(var, language)
                        args += compiler(lang, "mrc" if var.endswith("NOMEM") else mem, link="NOLINK" not in var)
                    else:
                        args.append(host_path(tok))
                commands.append(args)
        else:
            m = re.search(r"\bTEST_CFLAGS\b(.*)$", text, re.M)
            cflags = [host_path(a) for a in shlex.split(m.group(1))] if m else []
            commands = [compiler(language, mem) + cflags + [str(path), "-o", f"{name}.exe"]]
        for cmd in commands:
            err = run_in(cmd, builddir)
            if err:
                test.build_error = err
                break
        # A TEST_RUN_OUTPUT block may list alternatives separated by a line holding only OR.
        expected = [alt for block in multiline("TEST_RUN_OUTPUT", text) for alt in re.split(r"\nOR\n", block)]
        crashes = bool(re.search(r"\bTEST_CRASHES\b", text))
        # test.pl's run environment: the runtime prints "objc[pid]: HALTED" and exits instead of
        # crashing, and freed memory is scribbled.
        if crashes:
            test.env["OBJC_DEBUG_DONT_CRASH"] = "YES"
        if not re.search(r"\bTEST_NO_MALLOC_SCRIBBLE\b", text):
            test.env["MallocScribble"] = "1"
        return test, None, {"expected": expected, "crashes": crashes}

    # test.pl gettests(): a file is a test when a line starts with TEST_ (after comment characters);
    # the other sources are helpers that TEST_BUILD recipes compile.
    files = sorted(p for p in testdir.iterdir() if p.suffix in EXT_LANGUAGE and
                   re.search(r"(?m)^[/*\s]*TEST_", p.read_text(errors="replace")))
    tests = []
    meta = {}
    for test, skip, info in c.parallel(one, files):
        if skip:
            test.build_error, test.skip = "", info
        else:
            meta[test.name] = info
        tests.append(test)
    (out / "expectations.json").write_text(json.dumps(meta, indent=1))
    return tests


def run_in(cmd, cwd):
    proc = subprocess.run(cmd, cwd=cwd, stdin=subprocess.DEVNULL, capture_output=True, text=True, errors="replace")
    return "" if proc.returncode == 0 else (proc.stderr or proc.stdout or f"exit {proc.returncode}").strip()


def expectation(ctx, name):
    return json.loads((ctx.out / "objc4" / "expectations.json").read_text()).get(name, {})


def classify(result, ctx):
    info = expectation(ctx, result.name)
    # test.pl filter_warn: WARN: lines are the test's own notes and do not fail it.
    lines = [l.rstrip() for l in result.output.splitlines()
             if l.strip() and not l.startswith("WARN:") and not re.match(r"^alchemy(-fx)?: (warning|note|info)", l)]
    text = "\n".join(lines) + "\n"
    name = result.name.split("/", 1)[1]
    bad = [l for l in lines if l.startswith("BAD:")]
    patterns = [re.sub(r"CRASHED: SIG\w+\n?", "", p) for p in info.get("expected", [])]
    if result.status == "TIMEOUT":
        result.detail = "timed out"
        return result
    if info.get("crashes"):
        # A run without OBJC_DEBUG_DONT_CRASH (older test lists) dies by a signal and prints no HALTED line.
        if result.exit_code is not None and result.exit_code < 0:
            patterns = [re.sub(r"\n?objc\\\[\\d\+\\\]: HALTED", "", p) for p in patterns]
        ok = result.exit_code not in (0, None) and (not patterns or any(re.search(p, text) for p in patterns))
    elif patterns:
        ok = any(re.search(p, text) for p in patterns) and not bad
    else:
        ok = not bad and len(lines) >= 1 and lines[-1].startswith(f"OK: {name}") and result.exit_code == 0 and \
            all(l.startswith(f"OK: {name}") for l in lines)
    if ok:
        result.status, result.detail = "PASS", ""
    else:
        if result.status == "PASS":
            result.status = "FAIL"
        result.detail = (bad[0] if bad else (lines[-1] if lines else "no output"))[:300]
    return result
