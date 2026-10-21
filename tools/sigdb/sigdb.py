#!/usr/bin/env python3
"""fx signature database generator (owner: abi).

Runs the local clang over translation units that include the Windows SDK
headers and every UCRT header, reads the JSON AST
(`-Xclang -ast-dump=json -fsyntax-only`), classifies every function's return
and parameter types and writes a sorted name -> signature table to
src/fx/abi/generated/sigdb_table.inc. The signature grammar is defined in
src/fx/abi/NOTES.md.

Record sizes and alignments come from a second clang pass that evaluates
sizeof/_Alignof for every structure passed or returned by value, so layouts
(packing, alignment attributes, bitfields) are exactly the compiler's.

Usage:
    python tools/sigdb/sigdb.py [--clang PATH] [--out FILE] [--keep DIR] [-v]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from collections import Counter

DEFAULT_CLANG = r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin\clang.exe"
TARGET = "x86_64-pc-windows-msvc"
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_OUT = os.path.join(REPO, "src", "fx", "abi", "generated", "sigdb_table.inc")

PRELUDE = """\
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_DECLARE_NONSTDC_NAMES 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#define _USE_MATH_DEFINES 1
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#define STRICT 1
"""

# Translation units. Headers that cannot coexist (wininet/winhttp) get their
# own unit. Each header is syntax-checked on its own first and dropped (with a
# warning) if it does not compile in C mode.
SDK_BASE = ["winsock2.h", "ws2tcpip.h", "windows.h"]
SDK_HEADERS = [
    "mswsock.h", "winternl.h", "shlobj.h", "shellapi.h", "commctrl.h", "commdlg.h",
    "ole2.h", "oleauto.h", "objbase.h", "dbghelp.h", "psapi.h", "bcrypt.h", "ncrypt.h",
    "dwmapi.h", "imm.h", "shellscalingapi.h", "shcore.h", "pathcch.h", "shlwapi.h",
    "winver.h", "userenv.h", "setupapi.h", "GL/gl.h", "GL/glu.h", "tlhelp32.h",
    "uxtheme.h", "mmsystem.h", "xinput.h", "wtsapi32.h", "winhttp.h", "processthreadsapi.h",
    "synchapi.h", "threadpoolapiset.h", "realtimeapiset.h", "powrprof.h", "hidsdi.h",
    "wincrypt.h", "dpapi.h", "winsvc.h", "winnetwk.h", "iphlpapi.h", "versionhelpers.h",
    "timeapi.h", "dxgi.h", "d3d11.h", "d3d12.h", "dcomp.h", "avrt.h", "mmdeviceapi.h",
    "propsys.h", "propvarutil.h", "knownfolders.h", "wtypes.h", "objidl.h", "oleidl.h",
    "sddl.h", "aclapi.h", "lmcons.h", "winioctl.h", "fileapifromapp.h", "appmodel.h",
    "memoryapi.h", "jobapi2.h", "winevt.h", "evntprov.h", "wow64apiset.h", "werapi.h",
]
INET_BASE = ["windows.h"]
INET_HEADERS = ["wininet.h", "urlmon.h"]
CRT_EXTRA = ["vcruntime.h", "vcruntime_string.h", "vcruntime_startup.h", "setjmp.h", "excpt.h",
             "stdarg.h", "intrin.h"]
# stdnoreturn.h/stdalign.h define keyword macros (noreturn, alignas) that break
# later MSVC headers in the same unit.
CRT_SKIP = {"tgmath.h", "safeint.h", "safeint_internal.h", "new.h", "stdnoreturn.h", "stdalign.h"}

# ---------------------------------------------------------------------------
# Type strings (clang TypePrinter output) -> type trees
# ---------------------------------------------------------------------------

QUALIFIERS = {
    "const", "volatile", "restrict", "__restrict", "__unaligned", "__ptr64", "__ptr32",
    "_Nonnull", "_Nullable", "_Null_unspecified", "__sptr", "__uptr", "_Atomic",
    "__w64", "__capability",
}
BUILTIN_WORDS = {
    "unsigned", "signed", "char", "short", "int", "long", "float", "double", "void",
    "_Bool", "bool", "wchar_t", "__wchar_t", "__int8", "__int16", "__int32", "__int64",
    "__int128", "char8_t", "char16_t", "char32_t", "_Float16", "__bf16", "_Complex",
}
TAG_WORDS = {"struct", "union", "enum", "class"}

TOKEN_RE = re.compile(r"\.\.\.|[A-Za-z_][A-Za-z_0-9]*|\d+|[*()\[\],&:<>]")
ANON_RE = re.compile(r"\((?:unnamed|anonymous)[^()]*? at .*?:\d+:\d+\)")


def strip_attributes(text: str) -> str:
    """Removes __attribute__((...)) groups (balanced)."""
    out = []
    i = 0
    while True:
        j = text.find("__attribute__", i)
        if j < 0:
            out.append(text[i:])
            break
        out.append(text[i:j])
        k = text.find("(", j)
        if k < 0:
            break
        depth = 0
        m = k
        while m < len(text):
            if text[m] == "(":
                depth += 1
            elif text[m] == ")":
                depth -= 1
                if depth == 0:
                    break
            m += 1
        i = m + 1
    return "".join(out)


def tokenize(text: str) -> list[str]:
    text = ANON_RE.sub(" __anon__ ", text)
    text = strip_attributes(text)
    text = text.replace("__cdecl", " ").replace("__stdcall", " ").replace("__fastcall", " ")
    return TOKEN_RE.findall(text)


class ParseError(Exception):
    pass


class TypeParser:
    """Recursive-descent parser for C abstract declarators."""

    def __init__(self, tokens: list[str]):
        self.toks = tokens
        self.pos = 0

    def peek(self, offset: int = 0):
        i = self.pos + offset
        return self.toks[i] if i < len(self.toks) else None

    def take(self):
        tok = self.peek()
        if tok is None:
            raise ParseError("unexpected end")
        self.pos += 1
        return tok

    def expect(self, tok: str):
        got = self.take()
        if got != tok:
            raise ParseError(f"expected {tok!r}, got {got!r}")

    def parse_full(self):
        node = self.parse_type()
        if self.peek() is not None:
            raise ParseError(f"trailing tokens {self.toks[self.pos:]}")
        return node

    def parse_specifiers(self):
        words = []
        named = None
        tag = None
        while True:
            tok = self.peek()
            if tok is None:
                break
            if tok in QUALIFIERS or tok in ("__vectorcall", "__thiscall", "__clrcall", "__regcall"):
                self.pos += 1
                continue
            if tok in TAG_WORDS:
                self.pos += 1
                name = self.take()
                tag = ("tag", "struct" if tok == "class" else tok, name)
                continue
            if tok in BUILTIN_WORDS:
                words.append(tok)
                self.pos += 1
                continue
            if re.match(r"[A-Za-z_]", tok) and named is None and tag is None and not words:
                named = ("named", tok)
                self.pos += 1
                continue
            break
        if tag is not None:
            return tag
        if named is not None:
            return named
        if words:
            return ("builtin", " ".join(words))
        raise ParseError(f"no type specifier at {self.toks[self.pos:]}")

    def parse_type(self):
        base = self.parse_specifiers()
        return self.parse_declarator(base)

    def is_nested_declarator(self) -> bool:
        if self.peek() != "(":
            return False
        nxt = self.peek(1)
        return nxt in ("*", "(", "[", "&", "^")

    def parse_declarator(self, base):
        while self.peek() in ("*", "&"):
            self.pos += 1
            base = ("ptr", base)
            while self.peek() in QUALIFIERS:
                self.pos += 1
        if self.is_nested_declarator():
            self.pos += 1
            depth = 1
            start = self.pos
            while depth:
                tok = self.take()
                if tok == "(":
                    depth += 1
                elif tok == ")":
                    depth -= 1
            inner = self.toks[start:self.pos - 1]
            base = self.parse_suffixes(base)
            sub = TypeParser(inner)
            result = sub.parse_declarator(base)
            if sub.peek() is not None:
                raise ParseError(f"bad nested declarator {inner}")
            return result
        return self.parse_suffixes(base)

    def parse_suffixes(self, base):
        ops = []
        while self.peek() in ("[", "("):
            if self.peek() == "[":
                self.pos += 1
                size = None
                while self.peek() != "]":
                    tok = self.take()
                    if tok.isdigit():
                        size = int(tok)
                self.pos += 1
                ops.append(("array", size))
            else:
                self.pos += 1
                params = []
                variadic = False
                proto = True
                if self.peek() == ")":
                    proto = False   # K&R "()" (unprototyped)
                while self.peek() != ")":
                    if self.peek() == "...":
                        self.pos += 1
                        variadic = True
                    else:
                        params.append(self.parse_type())
                    if self.peek() == ",":
                        self.pos += 1
                self.pos += 1
                if len(params) == 1 and params[0] == ("builtin", "void"):
                    params = []
                ops.append(("func", params, variadic, proto))
        for op in reversed(ops):
            if op[0] == "array":
                base = ("array", base, op[1])
            else:
                base = ("func", base, op[1], op[2], op[3])
        return base


def parse_type_string(text: str):
    return TypeParser(tokenize(text)).parse_full()


# ---------------------------------------------------------------------------
# Database of typedefs and records collected from the AST
# ---------------------------------------------------------------------------

BUILTIN_CODES = {
    "void": "v",
    "_Bool": "C", "bool": "C",
    "char": "c", "signed char": "c", "unsigned char": "C", "char8_t": "C",
    "__int8": "c", "unsigned __int8": "C", "signed __int8": "c",
    "short": "s", "short int": "s", "signed short": "s", "signed short int": "s",
    "unsigned short": "S", "unsigned short int": "S", "wchar_t": "S", "__wchar_t": "S",
    "char16_t": "S", "__int16": "s", "unsigned __int16": "S",
    "int": "i", "signed": "i", "signed int": "i", "long": "i", "long int": "i",
    "signed long": "i", "__int32": "i",
    "unsigned": "I", "unsigned int": "I", "unsigned long": "I", "unsigned long int": "I",
    "char32_t": "I", "unsigned __int32": "I",
    "long long": "l", "long long int": "l", "signed long long": "l", "unsigned long long": "l",
    "unsigned long long int": "l", "__int64": "l", "unsigned __int64": "l", "signed __int64": "l",
    "float": "f", "double": "d", "long double": "d",
    "_Float16": "S", "__bf16": "S",
}


class Record:
    __slots__ = ("id", "tag", "name", "complete", "fields")

    def __init__(self, rid, tag, name):
        self.id = rid
        self.tag = tag
        self.name = name
        self.complete = False
        self.fields = []   # (type_string, is_bitfield, nested_record_id or None)


class Database:
    def __init__(self):
        self.typedefs: dict[str, str] = {}          # name -> underlying type string
        self.typedef_record: dict[str, str] = {}    # typedef name -> owned anonymous record id
        self.records: dict[str, Record] = {}        # id -> record
        self.tags: dict[str, str] = {}              # tag name -> complete record id
        self.functions: dict[str, tuple] = {}       # name -> (type string, variadic)
        self.function_conflicts = 0

    # ---- AST ingestion -----------------------------------------------------
    def add_typedef(self, node):
        name = node.get("name")
        qual = node.get("type", {}).get("qualType")
        if not name or qual is None or name in self.typedefs:
            return
        self.typedefs[name] = qual
        for child in node.get("inner", []):
            owned = child.get("ownedTagDecl")
            if owned and owned.get("kind") == "RecordDecl" and not owned.get("name"):
                self.typedef_record[name] = owned["id"]

    def add_record(self, node):
        rid = node["id"]
        rec = Record(rid, node.get("tagUsed", "struct"), node.get("name", ""))
        rec.complete = bool(node.get("completeDefinition"))
        last_nested = None
        for child in node.get("inner", []):
            kind = child.get("kind")
            if kind in ("RecordDecl", "CXXRecordDecl"):
                self.add_record(child)
                last_nested = child["id"]
            elif kind == "FieldDecl":
                qual = child.get("type", {}).get("qualType", "")
                nested = last_nested if ("(unnamed" in qual or "(anonymous" in qual) else None
                rec.fields.append((qual, bool(child.get("isBitfield")), nested))
        if rid not in self.records or rec.complete:
            self.records[rid] = rec
        if rec.name and rec.complete:
            self.tags.setdefault(rec.name, rid)

    def add_function(self, name, qual, variadic):
        if name.startswith("__builtin") or name.startswith("__sigdb"):
            return
        old = self.functions.get(name)
        if old is None:
            self.functions[name] = (qual, variadic)

    # ---- resolution ---------------------------------------------------------
    def resolve(self, node, depth=0):
        """Strips typedef names until a non-typedef node is reached."""
        while node[0] == "named":
            if depth > 64:
                raise ParseError("typedef loop")
            under = self.typedefs.get(node[1])
            if under is None:
                return node
            if node[1] in self.typedef_record:
                return ("record_id", self.typedef_record[node[1]], node[1])
            node = parse_cached(under)
            depth += 1
        if node[0] == "tag" and node[1] in ("struct", "union"):
            rid = self.tags.get(node[2])
            if rid is None and node[2] in self.typedef_record:
                rid = self.typedef_record[node[2]]
            if rid is not None:
                return ("record_id", rid, f"{node[1]} {node[2]}" if node[2] in self.tags else node[2])
        return node


_parse_cache: dict[str, tuple] = {}


def parse_cached(text: str):
    node = _parse_cache.get(text)
    if node is None:
        node = parse_type_string(text)
        _parse_cache[text] = node
    return node


# ---------------------------------------------------------------------------
# Classification into the signature grammar
# ---------------------------------------------------------------------------

class RecordUse:
    """A record passed or returned by value; rendered after the sizeof pass."""

    def __init__(self, rid, spelling):
        self.rid = rid
        self.spelling = spelling
        self.hfa = None


class Classifier:
    def __init__(self, db: Database):
        self.db = db
        self.record_uses: dict[str, RecordUse] = {}   # rid -> use
        self.unknown_names = Counter()
        self.hfa_cache: dict[str, object] = {}

    def code(self, node, spelling_hint=None):
        """Returns a list of grammar pieces (str or RecordUse)."""
        db = self.db
        if node[0] == "named":
            resolved = db.resolve(node)
            if resolved[0] == "named":
                self.unknown_names[node[1]] += 1
                return ["l"]
            if resolved[0] == "record_id":
                return [self.use_record(resolved[1], node[1])]
            return self.code(resolved, spelling_hint)
        kind = node[0]
        if kind == "builtin":
            code = BUILTIN_CODES.get(node[1])
            if code is None:
                if "__int128" in node[1]:
                    return ["{16:16}"]
                self.unknown_names[node[1]] += 1
                return ["l"]
            return [code]
        if kind == "tag":
            if node[1] == "enum":
                return ["i"]
            resolved = db.resolve(node)
            if resolved[0] == "record_id":
                return [self.use_record(resolved[1], resolved[2])]
            self.unknown_names[f"{node[1]} {node[2]}"] += 1
            return ["l"]
        if kind == "record_id":
            return [self.use_record(node[1], node[2])]
        if kind in ("ptr", "array"):
            target = node[1]
            if kind == "ptr":
                target = self.strip_named(target)
                if target[0] == "func":
                    return self.function_pointer(target)
            return ["p"]
        if kind == "func":
            return self.function_pointer(node)
        raise ParseError(f"cannot classify {node}")

    def strip_named(self, node):
        if node[0] == "named":
            resolved = self.db.resolve(node)
            return resolved
        return node

    def function_pointer(self, func):
        if not func[4]:      # unprototyped: plain pointer
            return ["p"]
        try:
            return ["@"] + self.signature(func)
        except ParseError:
            return ["p"]

    def signature(self, func):
        _, ret, params, variadic, _proto = func
        pieces = []
        ret_res = self.strip_named(ret) if ret[0] == "named" else ret
        if ret_res[0] in ("array",):
            pieces += ["p"]
        else:
            pieces += self.code(ret)
        pieces.append("(")
        for p in params:
            pres = self.strip_named(p) if p[0] == "named" else p
            if pres[0] == "builtin" and pres[1] == "void":
                raise ParseError("void parameter")
            pieces += self.code(p)
        if variadic:
            pieces.append(".")
        pieces.append(")")
        return pieces

    def use_record(self, rid, spelling):
        use = self.record_uses.get(rid)
        if use is None:
            use = RecordUse(rid, spelling)
            self.record_uses[rid] = use
        return use

    # ---- homogeneous floating-point aggregates (AAPCS64) --------------------
    def hfa(self, rid, depth=0):
        if rid in self.hfa_cache:
            return self.hfa_cache[rid]
        self.hfa_cache[rid] = None
        rec = self.db.records.get(rid)
        result = None
        if rec is not None and rec.fields and depth < 16:
            base = None
            count = 0
            ok = True
            for qual, bitfield, nested in rec.fields:
                if bitfield:
                    ok = False
                    break
                if nested is not None:
                    member = self.hfa(nested, depth + 1)
                else:
                    try:
                        member = self.member_hfa(parse_cached(qual), depth)
                    except ParseError:
                        member = None
                if member is None:
                    ok = False
                    break
                mbase, mcount = member
                if base is None:
                    base = mbase
                elif base != mbase:
                    ok = False
                    break
                count = max(count, mcount) if rec.tag == "union" else count + mcount
            if ok and base is not None and 1 <= count <= 4:
                result = (base, count)
        self.hfa_cache[rid] = result
        return result

    def member_hfa(self, node, depth):
        node = self.db.resolve(node) if node[0] in ("named", "tag") else node
        if node[0] == "builtin":
            code = BUILTIN_CODES.get(node[1])
            if code in ("f", "d"):
                return (code, 1)
            return None
        if node[0] == "array":
            if node[2] is None or node[2] == 0:
                return None
            inner = self.member_hfa(node[1], depth)
            if inner is None:
                return None
            return (inner[0], inner[1] * node[2])
        if node[0] == "record_id":
            return self.hfa(node[1], depth + 1)
        return None


def render(pieces, sizes) -> str:
    out = []
    for piece in pieces:
        if isinstance(piece, str):
            out.append(piece)
            continue
        size, align = sizes[piece.rid]
        hfa = piece.hfa
        if hfa is not None and size == hfa[1] * (4 if hfa[0] == "f" else 8):
            out.append("{%s%d}" % hfa)
        elif align >= 16:
            out.append("{%d:16}" % size)
        else:
            out.append("{%d}" % size)
    return "".join(out)


# ---------------------------------------------------------------------------
# clang driving
# ---------------------------------------------------------------------------

def clang_base(clang):
    return [clang, f"--target={TARGET}", "-x", "c", "-fsyntax-only", "-ferror-limit=0",
            "-Wno-everything", "-fms-extensions"]


def include_dirs(clang):
    proc = subprocess.run([clang, f"--target={TARGET}", "-x", "c", "-E", "-v", "-"],
                          input="", capture_output=True, text=True)
    dirs = []
    active = False
    for line in proc.stderr.splitlines():
        if "search starts here" in line:
            active = True
            continue
        if line.startswith("End of search list"):
            break
        if active and line.startswith(" "):
            dirs.append(line.strip())
    return dirs


def header_compiles(clang, workdir, base, header):
    text = PRELUDE + "".join(f"#include <{h}>\n" for h in base + [header])
    path = os.path.join(workdir, "probe.c")
    with open(path, "w") as f:
        f.write(text)
    proc = subprocess.run(clang_base(clang) + [path], capture_output=True, text=True)
    return proc.returncode == 0, proc.stderr


def build_units(clang, workdir, verbose):
    dirs = include_dirs(clang)
    ucrt = next((d for d in dirs if d.lower().endswith("\\ucrt")), None)
    if ucrt is None:
        sys.exit("sigdb: cannot find the UCRT include directory")
    crt_headers = []
    for root, _dirs, files in os.walk(ucrt):
        for name in sorted(files):
            if not name.endswith(".h") or name in CRT_SKIP:
                continue
            rel = os.path.relpath(os.path.join(root, name), ucrt).replace("\\", "/")
            crt_headers.append(rel)
    crt_headers = CRT_EXTRA + crt_headers

    units = []
    for unit_name, base, headers in (("sdk", SDK_BASE, SDK_HEADERS),
                                     ("inet", INET_BASE, INET_HEADERS),
                                     ("crt", [], crt_headers)):
        accepted = []
        for header in headers:
            ok, err = header_compiles(clang, workdir, base, header)
            if ok:
                accepted.append(header)
            else:
                first = next((l for l in err.splitlines() if "error" in l), err[:200])
                print(f"sigdb: skipping {header} ({unit_name}): {first.strip()}", file=sys.stderr)
        units.append((unit_name, base + accepted))
        if verbose:
            print(f"sigdb: unit {unit_name}: {len(base) + len(accepted)} headers", file=sys.stderr)
    return units, ucrt


FUNC_NAME_RE = re.compile(r'^      "name": "((?:[^"\\]|\\.)*)",?$', re.M)
FUNC_TYPE_RE = re.compile(r'^      "type": \{\n(?:        "desugaredQualType": "(?:[^"\\]|\\.)*",\n)?        "qualType": "((?:[^"\\]|\\.)*)"', re.M)
FUNC_VARIADIC_RE = re.compile(r'^      "variadic": true', re.M)
FUNC_IMPLICIT_RE = re.compile(r'^      "isImplicit": true', re.M)
FUNC_STATIC_RE = re.compile(r'^      "storageClass": "static"', re.M)


def ingest_chunk(db: Database, chunk: str):
    head = chunk[:200]
    m = re.search(r'"kind": "(\w+)"', head)
    if not m:
        return
    kind = m.group(1)
    if kind == "FunctionDecl":
        head = chunk[:6000]
        # Implicit builtins and static (header-only, e.g. clang intrinsics)
        # functions are never exported.
        if FUNC_IMPLICIT_RE.search(head) or FUNC_STATIC_RE.search(head):
            return
        nm = FUNC_NAME_RE.search(chunk)
        tm = FUNC_TYPE_RE.search(chunk)
        if not nm or not tm:
            return
        name = json.loads('"' + nm.group(1) + '"')
        qual = json.loads('"' + tm.group(1) + '"')
        db.add_function(name, qual, bool(FUNC_VARIADIC_RE.search(chunk[:4000])))
    elif kind == "TypedefDecl":
        db.add_typedef(json.loads(chunk))
    elif kind == "RecordDecl":
        db.add_record(json.loads(chunk))


def dump_unit(clang, workdir, unit_name, headers, db: Database, verbose, keep):
    path = os.path.join(workdir, f"{unit_name}.c")
    with open(path, "w") as f:
        f.write(PRELUDE + "".join(f"#include <{h}>\n" for h in headers))
    cmd = clang_base(clang) + ["-Xclang", "-ast-dump=json", path]
    start = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            encoding="utf-8", errors="replace", bufsize=1 << 20)
    chunk_lines = []
    in_chunk = False
    count = 0
    raw = open(os.path.join(keep, f"{unit_name}.json"), "w", encoding="utf-8") if keep else None
    for line in proc.stdout:
        if raw:
            raw.write(line)
        if not in_chunk:
            if line == "    {\n":
                in_chunk = True
                chunk_lines = [line]
            continue
        chunk_lines.append(line)
        if line in ("    },\n", "    }\n"):
            text = "".join(chunk_lines).rstrip().rstrip(",")
            ingest_chunk(db, text)
            count += 1
            in_chunk = False
    err = proc.stderr.read()
    proc.wait()
    if raw:
        raw.close()
    if proc.returncode != 0:
        print(err[:4000], file=sys.stderr)
        sys.exit(f"sigdb: clang failed on unit {unit_name}")
    if verbose:
        print(f"sigdb: unit {unit_name}: {count} top-level decls in {time.time() - start:.1f}s",
              file=sys.stderr)


def measure_records(clang, workdir, units, uses):
    """Second pass: sizeof/_Alignof for every by-value record."""
    sizes = {}
    pending = list(uses.values())
    for unit_name, headers in units:
        if not pending:
            break
        lines = [PRELUDE] + [f"#include <{h}>\n" for h in headers]
        for index, use in enumerate(pending):
            lines.append(f"typedef char __sigdb_sz_{index}[sizeof({use.spelling})];\n")
            lines.append(f"typedef char __sigdb_al_{index}[_Alignof({use.spelling})];\n")
        path = os.path.join(workdir, f"{unit_name}_sizes.c")
        with open(path, "w") as f:
            f.write("".join(lines))
        cmd = clang_base(clang) + ["-Xclang", "-ast-dump", "-Xclang", "-ast-dump-filter=__sigdb_", path]
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        found = {}
        for m in re.finditer(r"__sigdb_(sz|al)_(\d+) 'char\[(\d+)\]'", proc.stdout):
            found.setdefault(int(m.group(2)), {})[m.group(1)] = int(m.group(3))
        rest = []
        for index, use in enumerate(pending):
            entry = found.get(index, {})
            if "sz" in entry and "al" in entry:
                sizes[use.rid] = (entry["sz"], entry["al"])
            else:
                rest.append(use)
        pending = rest
    for use in pending:
        print(f"sigdb: cannot measure record {use.spelling}", file=sys.stderr)
    return sizes


def collect_pieces(pieces, acc):
    for piece in pieces:
        if isinstance(piece, RecordUse):
            acc.add(piece.rid)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang", default=os.environ.get("FX_SIGDB_CLANG", DEFAULT_CLANG))
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--keep", help="directory to keep intermediate files (JSON dumps)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.clang):
        sys.exit(f"sigdb: clang not found at {args.clang}")
    workdir = args.keep or tempfile.mkdtemp(prefix="fx_sigdb_")
    os.makedirs(workdir, exist_ok=True)

    units, ucrt = build_units(args.clang, workdir, args.verbose)
    db = Database()
    for unit_name, headers in units:
        dump_unit(args.clang, workdir, unit_name, headers, db, args.verbose, args.keep)
    if args.verbose:
        print(f"sigdb: {len(db.functions)} functions, {len(db.typedefs)} typedefs, "
              f"{len(db.records)} records", file=sys.stderr)

    classifier = Classifier(db)
    signatures = {}
    failures = 0
    for name, (qual, variadic) in db.functions.items():
        try:
            node = parse_cached(qual)
            if node[0] != "func":
                continue
            if not node[4] and not node[2]:
                continue        # unprototyped: leave to the unknown default
            pieces = classifier.signature(node)
            if variadic and "." not in [p for p in pieces if isinstance(p, str)][-2:]:
                pieces.insert(len(pieces) - 1, ".")
            signatures[name] = pieces
        except (ParseError, RecursionError) as error:
            failures += 1
            if args.verbose:
                print(f"sigdb: cannot classify {name}: {qual}: {error}", file=sys.stderr)

    for use in classifier.record_uses.values():
        use.hfa = classifier.hfa(use.rid)
    sizes = measure_records(args.clang, workdir, units, classifier.record_uses)

    rows = []
    for name in sorted(signatures):
        pieces = signatures[name]
        rids = set()
        collect_pieces(pieces, rids)
        if any(rid not in sizes for rid in rids):
            continue
        rows.append((name, render(pieces, sizes)))

    sdk_version = os.path.basename(os.path.dirname(ucrt))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", newline="\n") as f:
        f.write("// Generated by tools/sigdb/sigdb.py; do not edit. Grammar: src/fx/abi/NOTES.md.\n")
        f.write(f"// Sources: Windows SDK {sdk_version} (um/shared/ucrt), MSVC vcruntime, target {TARGET}.\n")
        f.write(f"// {len(rows)} functions, sorted by name (strcmp order).\n")
        for name, sig in rows:
            f.write(f'{{"{name}", "{sig}"}},\n')
    print(f"sigdb: wrote {len(rows)} signatures to {args.out} "
          f"({failures} unclassified, {len(classifier.record_uses)} by-value records)")
    if args.verbose and classifier.unknown_names:
        common = ", ".join(f"{n}({c})" for n, c in classifier.unknown_names.most_common(20))
        print(f"sigdb: unresolved type names: {common}", file=sys.stderr)


if __name__ == "__main__":
    main()
