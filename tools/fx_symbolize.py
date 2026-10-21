"""fx_symbolize.py <profile.txt> [module=image[;pdb_dir] ...]

Appends guest symbol names (DbgHelp, from the guest image's PDB) to every
`<module>+0xRVA` location in an fx sampling profile. By default blender.exe
is symbolized from artifacts/blender-symbols (blender_private.pdb)."""
import ctypes, ctypes.wintypes as wt, os, re, sys

STAGE = r"C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5"
DEFAULT = {"blender.exe": (os.path.join(STAGE, "blender.exe"), r"C:\dev\transmute\artifacts\blender-symbols")}

dbghelp = ctypes.WinDLL("dbghelp.dll")
SYMOPT_UNDNAME, SYMOPT_DEFERRED_LOADS, SYMOPT_LOAD_ANYTHING = 0x2, 0x4, 0x40
dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_ANYTHING)
process = wt.HANDLE(0x7f1234)


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", wt.WCHAR * 1024)]


modules = dict(DEFAULT)
for arg in sys.argv[2:]:
    name, _, rest = arg.partition("=")
    image, _, pdbdir = rest.partition(";")
    modules[name.lower()] = (image, pdbdir or os.path.dirname(image))
search = ";".join(sorted({d for _, d in modules.values()}))
if not dbghelp.SymInitializeW(process, ctypes.c_wchar_p(search), False):
    sys.exit("SymInitialize failed")
bases = {}
next_base = 0x10000000000
for name, (image, _) in modules.items():
    dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
    dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint64,
                                         wt.DWORD, ctypes.c_void_p, wt.DWORD]
    base = dbghelp.SymLoadModuleExW(process, None, image, None, next_base, 0x10000000, None, 0)
    if base:
        bases[name] = base
        next_base += 0x100000000

dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
cache = {}


def lookup(module, rva):
    key = (module, rva)
    if key in cache:
        return cache[key]
    info = SYMBOL_INFOW()
    info.SizeOfStruct = 88   # sizeof(SYMBOL_INFOW)
    info.MaxNameLen = 1023
    disp = ctypes.c_uint64(0)
    out = None
    if dbghelp.SymFromAddrW(process, bases[module] + rva, ctypes.byref(disp), ctypes.byref(info)):
        n = info.Name
        n = re.sub(r"\bccl::", "", n)
        if len(n) > 110:
            n = n[:110] + "..."
        out = "%s+0x%x" % (n, disp.value)
    cache[key] = out
    return out


pat = re.compile(r"([\w.-]+\.(?:exe|dll))\+0x([0-9a-f]+)", re.I)
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    m = pat.search(line)
    if m and m[1].lower() in bases:
        s = lookup(m[1].lower(), int(m[2], 16))
        if s:
            line += "   [" + s + "]"
    print(line)
