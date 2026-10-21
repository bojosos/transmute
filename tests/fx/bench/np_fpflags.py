# numpy floating-point status flag checks (np.errstate raises on the FPSR /
# MXCSR cumulative flags numpy reads after each ufunc loop).
import numpy as np, warnings
cases = []
def check(name, fn, kind):
    got = "none"
    try:
        with np.errstate(all="raise"):
            fn()
    except FloatingPointError as e:
        got = str(e).split()[-1] if str(e) else "raised"
    print("NPFP %-28s expect %-10s got %s" % (name, kind, got), flush=True)
    cases.append((name, kind, got))
f32, f64 = np.float32, np.float64
a = np.array([1.0, 2.0, 3.0, 4.0] * 8, dtype=np.float32)
z = np.zeros(32, dtype=np.float32)
check("div0 scalar f64", lambda: f64(1.0) / f64(0.0), "divide")
check("div0 vec f32", lambda: a / z, "divide")
check("invalid sqrt vec", lambda: np.sqrt(-a), "invalid")
check("invalid 0/0 vec", lambda: z / z, "invalid")
check("invalid inf-inf", lambda: np.array([np.inf] * 8) - np.array([np.inf] * 8), "invalid")
check("overflow mul f32", lambda: np.full(32, 3e38, np.float32) * f32(10), "overflow")
check("overflow exp f64", lambda: np.exp(np.full(8, 1000.0)), "overflow")
check("underflow f32", lambda: np.full(32, 1e-38, np.float32) * f32(1e-10), "underflow")
check("clean add (no flag)", lambda: a + a, "none")
check("clean after div0", lambda: (np.seterr(all="ignore"), a / z, np.seterr(all="raise"), a + a), "none")
check("int cast invalid", lambda: np.array([np.nan] * 8).astype(np.int32), "invalid")
check("log0 divide", lambda: np.log(z), "divide")
check("arctanh(1) divide", lambda: np.arctanh(np.ones(8)), "divide")
check("power overflow", lambda: np.power(f64(10.0), np.full(8, 400.0)), "overflow")
bad = [c for c in cases if c[1] != c[2]]
print("NPFP summary: %d/%d match" % (len(cases) - len(bad), len(cases)))
