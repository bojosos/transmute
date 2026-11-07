"""A small unifdef: resolves preprocessor conditionals over a fixed set of defined/undefined macros.

Header installs in xnu (makedefs DATA_UNIFDEF) and Libc (xcodescripts/headers.sh) run unifdef so
that the installed headers only keep the user-space, macOS branch. Conditions are evaluated in
three-valued logic: a condition that depends on any other macro stays in the output untouched,
while its body is still processed. Only `defined`, !, &&, ||, parentheses, integers and the known
macros are understood; anything else makes the condition unknown.
"""
import re

TOKEN = re.compile(r"\s*(defined\b|\|\||&&|!(?!=)|\(|\)|[A-Za-z_]\w*|\d+[uUlL]*|.)")
UNKNOWN = object()


def evaluate(expr, defined, undefined):
    tokens = [t for t in TOKEN.findall(re.sub(r"/\*.*?\*/|//.*", "", expr)) if t.strip()]
    pos = 0

    def peek():
        return tokens[pos] if pos < len(tokens) else None

    def take():
        nonlocal pos
        pos += 1
        return tokens[pos - 1]

    def primary():
        tok = take() if peek() is not None else None
        if tok == "!":
            v = primary()
            return UNKNOWN if v is UNKNOWN else int(not v)
        if tok == "(":
            v = disjunction()
            if peek() != ")":
                raise ValueError
            take()
            return v
        if tok == "defined":
            paren = peek() == "("
            if paren:
                take()
            name = take()
            if paren:
                if peek() != ")":
                    raise ValueError
                take()
            return 1 if name in defined else 0 if name in undefined else UNKNOWN
        if tok is not None and re.fullmatch(r"\d+[uUlL]*", tok):
            return int(re.sub(r"[uUlL]", "", tok)) != 0
        if tok is not None and re.fullmatch(r"[A-Za-z_]\w*", tok):
            if tok in defined:
                return int(defined[tok] != 0)
            if tok in undefined:
                return 0
            return UNKNOWN
        raise ValueError

    def conjunction():
        v = primary()
        while peek() == "&&":
            take()
            w = primary()
            v = 0 if (v == 0 or w == 0) else (UNKNOWN if UNKNOWN in (v, w) else 1)
        return v

    def disjunction():
        v = conjunction()
        while peek() == "||":
            take()
            w = conjunction()
            v = 1 if (v == 1 or w == 1) else (UNKNOWN if UNKNOWN in (v, w) else 0)
        return v

    try:
        value = disjunction()
        return UNKNOWN if peek() is not None else value
    except (ValueError, IndexError):
        return UNKNOWN


DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def unifdef(text, defined, undefined):
    """defined: {name: value}; undefined: set of names. Returns the processed text."""
    lines = text.split("\n")
    out = []
    # Stack of frames: (resolved, keep_current, taken, parent_keep). resolved: this chain is being
    # removed from the output; keep_current: the current branch's body is emitted.
    stack = []

    def active():
        return all(f["keep"] for f in stack)

    i = 0
    while i < len(lines):
        line = lines[i]
        # Join continuation lines of directives.
        full = line
        while full.endswith("\\") and DIRECTIVE.match(line) and i + 1 < len(lines):
            i += 1
            full = full[:-1] + " " + lines[i]
            line = line + "\n" + lines[i]
        m = DIRECTIVE.match(full)
        i += 1
        if not m:
            if active():
                out.append(line)
            continue
        kind, rest = m.group(1), m.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            name = rest.split()[0] if rest.split() else ""
            cond = (evaluate(rest, defined, undefined) if kind == "if" else
                    (1 if name in defined else 0 if name in undefined else UNKNOWN))
            if kind == "ifndef" and cond is not UNKNOWN:
                cond = int(not cond)
            frame = {"unknown_seen": cond is UNKNOWN, "taken": cond == 1, "keep": cond != 0}
            if cond is UNKNOWN:
                frame["emit"] = True
                if active():
                    out.append(line)
            else:
                frame["emit"] = False
            stack.append(frame)
        elif kind == "elif":
            frame = stack[-1]
            cond = evaluate(rest, defined, undefined)
            if frame["taken"]:
                frame["keep"] = False
                continue
            if frame["emit"]:
                # The chain is already in the output; keep it a chain.
                if cond == 0:
                    frame["keep"] = False
                elif cond == 1:
                    frame["keep"] = True
                    frame["taken"] = True
                    frame["closed"] = True  # this branch is now the chain's #else
                    stack.pop()
                    if active():
                        out.append(re.sub(r"#\s*elif\b.*", "#else", line, count=1))
                    stack.append(frame)
                else:
                    frame["keep"] = True
                    stack.pop()
                    if active():
                        out.append(line)
                    stack.append(frame)
            else:
                if cond == 0:
                    frame["keep"] = False
                elif cond == 1:
                    frame["keep"] = True
                    frame["taken"] = True
                else:
                    # First unknown branch after resolved-false ones: it becomes the #if.
                    frame["keep"] = True
                    frame["emit"] = True
                    stack.pop()
                    if active():
                        out.append(re.sub(r"#\s*elif\b", "#if", line, count=1))
                    stack.append(frame)
        elif kind == "else":
            frame = stack[-1]
            frame["keep"] = not frame["taken"]
            frame["taken"] = True
            if frame["emit"] and not frame.get("closed"):
                stack.pop()
                if active():
                    out.append(line)
                stack.append(frame)
        else:  # endif
            frame = stack.pop()
            if frame["emit"] and active():
                out.append(line)
    return "\n".join(out)


# makedefs/MakeInc.def DATA_UNIFDEF for PLATFORM=MacOSX (public /usr/include headers).
XNU_DEFINED = {"XNU_PLATFORM_MacOSX": 1, "MODULES_SUPPORTED": 1, "_OPEN_SOURCE_": 1, "__OPEN_SOURCE__": 1}
XNU_UNDEFINED = {"MACH_KERNEL_PRIVATE", "BSD_KERNEL_PRIVATE", "IOKIT_KERNEL_PRIVATE", "LIBKERN_KERNEL_PRIVATE",
                 "LIBSA_KERNEL_PRIVATE", "PEXPERT_KERNEL_PRIVATE", "XNU_KERNEL_PRIVATE", "KERNEL_PRIVATE",
                 "KERNEL", "PRIVATE", "DRIVERKIT", "EXCLAVEKIT", "EXCLAVECORE", "SCHED_TEST_HARNESS",
                 "XNU_PLATFORM_DriverKit", "XNU_PLATFORM_ExclaveKit", "XNU_PLATFORM_ExclaveCore",
                 "XNU_PLATFORM_iPhoneOS", "XNU_PLATFORM_iPhoneSimulator", "XNU_PLATFORM_tvOS",
                 "XNU_PLATFORM_AppleTVOS", "XNU_PLATFORM_tvSimulator", "XNU_PLATFORM_AppleTVSimulator",
                 "XNU_PLATFORM_iPhoneOSNano", "XNU_PLATFORM_iPhoneNanoSimulator", "XNU_PLATFORM_WatchOS",
                 "XNU_PLATFORM_WatchSimulator", "XNU_PLATFORM_BridgeOS", "XNU_PLATFORM_XROS",
                 "XNU_PLATFORM_XRSimulator"}
# Libc xcodescripts/generate_features.pl --unifdef, macosx arm64.
LIBC_DEFINED = {"UNIFDEF_TZDIR_SYMLINK": 1, "UNIFDEF_BLOCKS": 1, "UNIFDEF_LEGACY_RUNE_APIS": 1,
                "UNIFDEF_LEGACY_64_APIS": 1, "UNIFDEF_LEGACY_UTMP_APIS": 1}
LIBC_UNDEFINED = {"UNIFDEF_POSIX_ILP32_ALLOW", "UNIFDEF_DRIVERKIT", "UNIFDEF_MOVE_LOCALTIME"}
