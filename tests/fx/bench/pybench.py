# Scalar / branchy workloads that native x64 cannot vectorize away:
# CPython interpreter loops, dicts, strings, json, regex, zlib, sorting,
# hashing, big integers. Run with blender -b --python <this file> (native
# x64 Blender vs ARM64 Blender under transmute-fx). Prints seconds per test.
import time, json, re, zlib, hashlib, random, struct, math

def t(name, fn, *a):
    fn(*a)                      # warm (JIT/translation, caches)
    best = 1e9
    for _ in range(3):
        s = time.perf_counter(); fn(*a); best = min(best, time.perf_counter() - s)
    print("PYBENCH %-14s %.4f" % (name, best), flush=True)

def loops(n):
    acc = 0
    for i in range(n):
        acc = (acc * 31 + i) & 0xFFFFFFFF
        if acc & 1: acc ^= i << 3
    return acc

def fib(n): return n if n < 2 else fib(n - 1) + fib(n - 2)

def dicts(n):
    d = {}
    for i in range(n): d["k%d" % (i % 5000)] = d.get("k%d" % (i % 3000), 0) + i
    return len(d)

def strings(n):
    s = []
    for i in range(n): s.append(("%d-%s" % (i, hex(i))).upper().replace("X", "_"))
    return len("|".join(s).split("_"))

random.seed(1)
DOC = [{"id": i, "name": "item%d" % i, "tags": ["a", "b", str(i)], "v": i * 0.5, "ok": i % 3 == 0} for i in range(20000)]
TEXT = " ".join("word%d foo@bar%d.com 2026-10-%02d" % (i, i, i % 28 + 1) for i in range(40000))
BLOB = (TEXT * 3).encode()
NUMS = [random.random() for _ in range(300000)]

def js(): return len(json.loads(json.dumps(DOC)))
def regex(): return len(re.findall(r"[a-z]+@[a-z]+\d+\.com|\d{4}-\d{2}-\d{2}", TEXT))
def zl(): return len(zlib.decompress(zlib.compress(BLOB, 6)))
def sorting(): return sorted(NUMS)[1000]
def sha(): return hashlib.sha256(BLOB).hexdigest()
def md5(): return hashlib.md5(BLOB).hexdigest()
def bigint(): return len(str(math.factorial(6000)))
def structs(): return sum(struct.unpack_from("<d", struct.pack("<d", x))[0] for x in NUMS[:100000])

t("loops", loops, 3000000)
t("fib", fib, 27)
t("dicts", dicts, 600000)
t("strings", strings, 300000)
t("json", js)
t("regex", regex)
t("zlib", zl)
t("sort", sorting)
t("sha256", sha)
t("md5", md5)
t("bigint", bigint)
t("struct", structs)
