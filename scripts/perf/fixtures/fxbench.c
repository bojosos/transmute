/* fxbench: guest microbenchmarks for the fx translator (docs/FX_PERF.md).
 *
 * One source, two builds:
 *   - arm64 Mach-O, run under alchemy-fx (translated);
 *   - x86-64 Windows, run natively (the reference time and the reference checksum).
 * scripts/perf/build_fixtures.ps1 builds both. Every kernel returns a checksum that must be the same
 * on both builds, so a translation bug that changes results shows as a checksum mismatch.
 *
 * Usage: fxbench [-scale N] [-reps N] [-only name[,name...]] [-list]
 * Output, one line per kernel:
 *   FXBENCH kernel=<name> iters=<n> reps=<r> median_ps=<ps/iter> min_ps=<ps/iter> checksum=<hex>
 * plus "FXBENCH done fails=<n>". Times are picoseconds per iteration, integers (no float formatting).
 *
 * Project-authored; freestanding (no libc headers): only the clang builtin headers. */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)
#define FXB_ARM64 1
#include <arm_neon.h>
#endif

#define NOINLINE __attribute__((noinline))
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;

/* ---- platform -------------------------------------------------------------------------------- */
int printf(const char *format, ...);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);

#if FXB_ARM64
u64 mach_absolute_time(void); /* Alchemy: nanoseconds (timebase 1/1); Apple silicon: 125/3 ns ticks */
typedef struct { u32 numer, denom; } mach_timebase_info_data_t;
int mach_timebase_info(mach_timebase_info_data_t *);
typedef void *pthread_t;
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
static u64 tb_numer = 1, tb_denom = 1;
static void clock_init(void) {
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) == 0 && tb.numer && tb.denom) { tb_numer = tb.numer; tb_denom = tb.denom; }
}
static u64 now_ns(void) { return mach_absolute_time() * tb_numer / tb_denom; }
typedef pthread_t thread_t;
static int thread_start(thread_t *t, void *(*fn)(void *), void *arg) { return pthread_create(t, 0, fn, arg); }
static void thread_join(thread_t t) { pthread_join(t, 0); }
#else
typedef void *HANDLE;
int __stdcall QueryPerformanceCounter(i64 *);
int __stdcall QueryPerformanceFrequency(i64 *);
HANDLE __stdcall CreateThread(void *, size_t, u32(__stdcall *)(void *), void *, u32, u32 *);
u32 __stdcall WaitForSingleObject(HANDLE, u32);
int __stdcall CloseHandle(HANDLE);
static i64 qpf = 1;
static void clock_init(void) { QueryPerformanceFrequency(&qpf); }
static u64 now_ns(void) {
    i64 c;
    QueryPerformanceCounter(&c);
    return (u64)(c / qpf) * 1000000000ull + (u64)(c % qpf) * 1000000000ull / (u64)qpf;
}
typedef struct { HANDLE h; void *(*fn)(void *); void *arg; } thread_t;
static u32 __stdcall thread_tramp(void *p) { thread_t *t = (thread_t *)p; t->fn(t->arg); return 0; }
static int thread_start(thread_t *t, void *(*fn)(void *), void *arg) {
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(0, 0, thread_tramp, t, 0, 0);
    return t->h ? 0 : 1;
}
static void thread_join(thread_t t) { WaitForSingleObject(t.h, 0xffffffffu); CloseHandle(t.h); }
#endif

/* ---- shared state ---------------------------------------------------------------------------- */
static u64 rng_state = 0x9e3779b97f4a7c15ull;
static u64 rng(void) { /* splitmix64 */
    u64 z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static void rng_seed(u64 s) { rng_state = s; }

#define BUF_BYTES (1u << 16)
static u8 buf_a[BUF_BYTES] __attribute__((aligned(64)));
static u8 buf_b[BUF_BYTES] __attribute__((aligned(64)));
static u32 arr32[1u << 14] __attribute__((aligned(64)));

static void fill_random(u8 *p, size_t n, u64 seed) {
    rng_seed(seed);
    for (size_t i = 0; i < n; i += 8) { u64 v = rng(); for (int k = 0; k < 8 && i + k < n; ++k) p[i + k] = (u8)(v >> (8 * k)); }
}

static inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static inline u32 rotl32(u32 x, int r) { return (x << r) | (x >> (32 - r)); }
static inline u64 load64(const u8 *p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline u32 load32(const u8 *p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }

/* ---- integer ------------------------------------------------------------------------------- */
/* xxhash64-style over a 4 KiB block: 64-bit multiply, rotate, xor, unaligned loads. */
NOINLINE static u64 k_int_hash(u64 iters) {
    const u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;
    u64 h = 0;
    for (u64 it = 0; it < iters; ++it) {
        u64 v1 = h + P1 + P2, v2 = h + P2, v3 = h, v4 = h - P1;
        for (u32 i = 0; i < 4096; i += 32) {
            v1 = rotl64(v1 + load64(buf_a + i) * P2, 31) * P1;
            v2 = rotl64(v2 + load64(buf_a + i + 8) * P2, 31) * P1;
            v3 = rotl64(v3 + load64(buf_a + i + 16) * P2, 31) * P1;
            v4 = rotl64(v4 + load64(buf_a + i + 24) * P2, 31) * P1;
        }
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h ^= h >> 33; h *= P3; h ^= h >> 29;
    }
    return h;
}

/* Flag-heavy integer code: overflow checks, compare chains, csel, 32-bit ops, divides,
 * bit-field extract/insert, clz, byte swaps. One dependent chain so it stays scalar. */
NOINLINE static u64 k_int_mix(u64 iters) {
    u64 a = 0x0123456789abcdefull, b = 0xfedcba9876543210ull, acc = 0;
    u32 w = 12345;
    for (u64 i = 0; i < iters; ++i) {
        u64 s;
        if (__builtin_add_overflow(a, b, &s)) acc += 3; /* adds + b.cs/cset */
        i64 d;
        if (__builtin_sub_overflow((i64)a, (i64)(b >> 1), &d)) acc ^= 0x55; /* subs + b.vs */
        a = s ^ (u64)d;
        b = (b > a) ? b - a : a - b + 1;                 /* cmp + csel */
        w = w * 1103515245u + 12345u;                   /* 32-bit: zero extension */
        u32 f = (w >> 7) & 0x3ff;                       /* ubfx */
        a = (a & ~(0xffull << 16)) | ((u64)(f & 0xff) << 16); /* bfi */
        acc += (u64)__builtin_clzll(a | 1) + (u64)__builtin_ctz(w | 1u);
        if ((i & 15) == 0) acc += a / ((b & 0xffff) | 1) + (u32)w % 7u; /* udiv, urem */
        acc = rotl64(acc, 5) ^ __builtin_bswap64(b);
        if ((i32)w < 0 && (a & 1)) acc -= 17;            /* tbnz, ccmp-like chains */
        else if ((w & 3) == 2 || (a >> 63)) acc += 11;
    }
    return acc ^ a ^ b ^ w;
}

/* ---- branches -------------------------------------------------------------------------------- */
/* Unpredictable branches: binary searches with random keys over a sorted array, plus an
 * insertion pass on random data. */
NOINLINE static u64 k_branchy(u64 iters) {
    const u32 n = 1u << 14;
    for (u32 i = 0; i < n; ++i) arr32[i] = i * 3 + (i & 1);
    u64 found = 0, x = 0x1234567ull;
    for (u64 it = 0; it < iters; ++it) {
        for (u32 q = 0; q < 64; ++q) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            u32 key = (u32)(x >> 33) % (n * 3);
            u32 lo = 0, hi = n;
            while (lo < hi) {
                u32 mid = (lo + hi) >> 1;
                if (arr32[mid] < key) lo = mid + 1;
                else hi = mid;
            }
            found += lo;
            if (lo < n && arr32[lo] == key) found += 1000;
        }
        /* small insertion sort on 32 random bytes */
        u8 v[32];
        for (int i = 0; i < 32; ++i) { x = x * 6364136223846793005ull + 1; v[i] = (u8)(x >> 56); }
        for (int i = 1; i < 32; ++i) {
            u8 t = v[i];
            int j = i - 1;
            while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; --j; }
            v[j + 1] = t;
        }
        found += v[0] + v[31] * 3 + v[16];
    }
    return found;
}

/* A bytecode interpreter: switch dispatch (a jump table, one indirect branch). Ignition-like. */
enum { OP_LDI, OP_ADD, OP_SUB, OP_MUL, OP_XOR, OP_SHR, OP_JNZ, OP_DEC, OP_MOV, OP_AND, OP_HALT };
typedef struct { u8 op, a, b, c; i32 imm; } insn_t;
static insn_t prog[16];
static void build_prog(void) {
    int k = 0;
    prog[k++] = (insn_t){OP_LDI, 0, 0, 0, 0};      /* r0 = n (patched) */
    prog[k++] = (insn_t){OP_LDI, 1, 0, 0, 7};      /* r1 = 7 */
    prog[k++] = (insn_t){OP_LDI, 2, 0, 0, 3};      /* r2 = 3 */
    /* loop @3 */
    prog[k++] = (insn_t){OP_MUL, 1, 1, 2, 0};      /* r1 = r1 * r2 */
    prog[k++] = (insn_t){OP_ADD, 1, 1, 0, 0};      /* r1 += r0 */
    prog[k++] = (insn_t){OP_SHR, 3, 1, 0, 7};      /* r3 = r1 >> 7 */
    prog[k++] = (insn_t){OP_XOR, 1, 1, 3, 0};      /* r1 ^= r3 */
    prog[k++] = (insn_t){OP_AND, 4, 1, 0, 0xff};   /* r4 = r1 & 0xff */
    prog[k++] = (insn_t){OP_ADD, 5, 5, 4, 0};      /* r5 += r4 */
    prog[k++] = (insn_t){OP_SUB, 2, 2, 4, 0};      /* r2 -= r4 */
    prog[k++] = (insn_t){OP_MOV, 6, 2, 0, 0};
    prog[k++] = (insn_t){OP_DEC, 0, 0, 0, 0};      /* r0-- */
    prog[k++] = (insn_t){OP_JNZ, 0, 0, 0, 3};      /* if r0 goto 3 */
    prog[k++] = (insn_t){OP_HALT, 0, 0, 0, 0};
}
NOINLINE static u64 k_interp_switch(u64 iters) {
    build_prog();
    u64 r[8] = {0};
    prog[0].imm = (i32)iters;
    const insn_t *pc = prog;
    for (;;) {
        const insn_t i = *pc++;
        switch (i.op) {
        case OP_LDI: r[i.a] = (u64)(i64)i.imm; break;
        case OP_ADD: r[i.a] = r[i.b] + r[i.c]; break;
        case OP_SUB: r[i.a] = r[i.b] - r[i.c]; break;
        case OP_MUL: r[i.a] = r[i.b] * r[i.c]; break;
        case OP_XOR: r[i.a] = r[i.b] ^ r[i.c]; break;
        case OP_SHR: r[i.a] = r[i.b] >> i.imm; break;
        case OP_AND: r[i.a] = r[i.b] & (u64)i.imm; break;
        case OP_MOV: r[i.a] = r[i.b]; break;
        case OP_DEC: r[i.a] -= 1; break;
        case OP_JNZ: if (r[i.a]) pc = prog + i.imm; break;
        case OP_HALT: return r[1] ^ (r[5] << 1) ^ r[6];
        default: return 0;
        }
    }
}

/* The same program with computed goto: an indirect branch at the end of every handler
 * (threaded code, the way V8's Ignition handlers and most interpreters dispatch). */
NOINLINE static u64 k_interp_goto(u64 iters) {
    build_prog();
    static const void *labels[] = {&&l_ldi, &&l_add, &&l_sub, &&l_mul, &&l_xor, &&l_shr, &&l_jnz,
                                   &&l_dec, &&l_mov, &&l_and, &&l_halt};
    u64 r[8] = {0};
    prog[0].imm = (i32)iters;
    const insn_t *pc = prog;
    insn_t i;
#define NEXT() do { i = *pc++; goto *labels[i.op]; } while (0)
    NEXT();
l_ldi: r[i.a] = (u64)(i64)i.imm; NEXT();
l_add: r[i.a] = r[i.b] + r[i.c]; NEXT();
l_sub: r[i.a] = r[i.b] - r[i.c]; NEXT();
l_mul: r[i.a] = r[i.b] * r[i.c]; NEXT();
l_xor: r[i.a] = r[i.b] ^ r[i.c]; NEXT();
l_shr: r[i.a] = r[i.b] >> i.imm; NEXT();
l_and: r[i.a] = r[i.b] & (u64)i.imm; NEXT();
l_mov: r[i.a] = r[i.b]; NEXT();
l_dec: r[i.a] -= 1; NEXT();
l_jnz: if (r[i.a]) pc = prog + i.imm; NEXT();
l_halt: return r[1] ^ (r[5] << 1) ^ r[6];
#undef NEXT
}

/* ---- calls ----------------------------------------------------------------------------------- */
typedef u64 (*fn_t)(u64, u64);
NOINLINE static u64 f0(u64 a, u64 b) { return a + b; }
NOINLINE static u64 f1(u64 a, u64 b) { return a ^ (b << 1); }
NOINLINE static u64 f2(u64 a, u64 b) { return a * 3 + b; }
NOINLINE static u64 f3(u64 a, u64 b) { return (a >> 1) + b * 5; }
NOINLINE static u64 f4(u64 a, u64 b) { return a - (b ^ 0x55); }
NOINLINE static u64 f5(u64 a, u64 b) { return rotl64(a, 7) + b; }
NOINLINE static u64 f6(u64 a, u64 b) { return (a | b) + 9; }
NOINLINE static u64 f7(u64 a, u64 b) { return (a & ~b) + (b >> 3); }
static fn_t volatile fns[8] = {f0, f1, f2, f3, f4, f5, f6, f7};

typedef struct obj obj_t;
typedef struct { u64 (*get)(const obj_t *); void (*bump)(obj_t *, u64); } vtable_t;
struct obj { const vtable_t *vt; u64 v; };
NOINLINE static u64 get_a(const obj_t *o) { return o->v; }
NOINLINE static u64 get_b(const obj_t *o) { return o->v * 2 + 1; }
NOINLINE static void bump_a(obj_t *o, u64 x) { o->v += x; }
NOINLINE static void bump_b(obj_t *o, u64 x) { o->v ^= x; }
static const vtable_t vt_a = {get_a, bump_a}, vt_b = {get_b, bump_b};

/* Indirect calls (blr through a table, polymorphic: 8 targets in a pseudo-random pattern) and
 * C++-style virtual calls through a vtable (two classes, alternating). */
NOINLINE static u64 k_indirect_calls(u64 iters) {
    fn_t local[8];
    for (int k = 0; k < 8; ++k) local[k] = fns[k];
    obj_t objs[4] = {{&vt_a, 1}, {&vt_b, 2}, {&vt_a, 3}, {&vt_b, 4}};
    u64 acc = 1, x = 99;
    for (u64 i = 0; i < iters; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        acc = local[(x >> 61)](acc, i);
        obj_t *o = &objs[i & 3];
        o->vt->bump(o, acc & 0xff);
        acc += o->vt->get(o);
    }
    return acc ^ objs[0].v ^ objs[1].v ^ objs[2].v ^ objs[3].v;
}

/* Direct calls and returns: recursion (bl/ret, frame push/pop, deep return stack). */
NOINLINE static u64 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
NOINLINE static u64 k_call_ret(u64 iters) {
    u64 acc = 0;
    for (u64 i = 0; i < iters; ++i) acc += fib(18 + (u32)(i & 1));
    return acc;
}

/* ---- memory ---------------------------------------------------------------------------------- */
/* The guest's own copy, fill, compare and scan loops (compiled with -fno-builtin, so they stay
 * in translated code): 16-byte pair loads and stores, byte loops, early exits. */
NOINLINE static void own_memcpy(u8 *d, const u8 *s, size_t n) {
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        u64 a = load64(s + i), b = load64(s + i + 8), c = load64(s + i + 16), e = load64(s + i + 24);
        __builtin_memcpy(d + i, &a, 8); __builtin_memcpy(d + i + 8, &b, 8);
        __builtin_memcpy(d + i + 16, &c, 8); __builtin_memcpy(d + i + 24, &e, 8);
    }
    for (; i < n; ++i) d[i] = s[i];
}
NOINLINE static size_t own_strlen(const char *s) { const char *p = s; while (*p) ++p; return (size_t)(p - s); }
NOINLINE static int own_memcmp(const u8 *a, const u8 *b, size_t n) {
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
NOINLINE static u64 k_mem_own(u64 iters) {
    u64 acc = 0;
    for (u64 it = 0; it < iters; ++it) {
        size_t off = (size_t)(it * 40) & 1023, n = 512 + (size_t)(it & 255);
        own_memcpy(buf_b + off, buf_a + off + 3, n);
        acc += own_memcmp(buf_a + off + 3, buf_b + off, n) + buf_b[off + n - 1];
        buf_b[off + 100 + (it & 63)] = 0;
        acc += own_strlen((const char *)buf_b + off + 1 + (it & 31));
        for (size_t i = 0; i < 256; ++i) buf_b[2048 + i] = (u8)(i + it); /* fill (may vectorize) */
        acc += buf_b[2048 + (it & 255)];
    }
    return acc;
}

/* Imported libc string calls (on Alchemy these are host functions; the call boundary is
 * what's measured): small memcpy, memset, memcmp, strlen. */
NOINLINE static u64 k_libc_calls(u64 iters) {
    u64 acc = 0;
    for (u64 it = 0; it < iters; ++it) {
        size_t off = (size_t)(it * 24) & 4095, n = 16 + (size_t)(it & 63);
        memcpy(buf_b + off, buf_a + off, n);
        acc += (u64)(memcmp(buf_b + off, buf_a + off, n) == 0);
        memset(buf_b + off + 8192, (int)(it & 0xff), 24);
        buf_b[off + 8192 + 24] = 0;
        acc += strlen((const char *)buf_b + off + 8192 + (it & 7)) + buf_b[off];
    }
    return acc;
}

/* Pointer chasing and struct fields: a shuffled linked list in a 256 KiB pool, field loads and
 * stores with pre/post-index and register-offset addressing. */
typedef struct node { struct node *next; u32 key, count; u64 payload[2]; } node_t;
#define NODES 8192
static node_t pool[NODES];
NOINLINE static u64 k_ptr_chase(u64 iters) {
    u32 order[NODES];
    for (u32 i = 0; i < NODES; ++i) order[i] = i;
    rng_seed(7);
    for (u32 i = NODES - 1; i > 0; --i) { u32 j = (u32)(rng() % (i + 1)); u32 t = order[i]; order[i] = order[j]; order[j] = t; }
    for (u32 i = 0; i < NODES; ++i) {
        node_t *n = &pool[order[i]];
        n->next = &pool[order[(i + 1) % NODES]];
        n->key = order[i] * 2654435761u;
        n->count = 0;
        n->payload[0] = i; n->payload[1] = ~(u64)i;
    }
    node_t *p = &pool[order[0]];
    u64 acc = 0;
    for (u64 it = 0; it < iters; ++it) {
        for (u32 s = 0; s < 64; ++s) {
            acc += p->key ^ p->payload[s & 1];
            p->count += 1;
            p = p->next;
        }
    }
    u64 counts = 0;
    for (u32 i = 0; i < NODES; i += 97) counts += pool[i].count * (u64)i;
    return acc ^ counts;
}

/* ---- floating point -------------------------------------------------------------------------- */
/* Scalar double: n-body-style step with sqrt, divide, fused multiply-add, compares, conversions. */
NOINLINE static u64 k_fp_scalar(u64 iters) {
    double px[8], py[8], vx[8], vy[8], m[8];
    for (int i = 0; i < 8; ++i) { px[i] = i * 1.5 - 4; py[i] = (i * 7 % 5) - 2.5; vx[i] = 0.01 * i; vy[i] = -0.02 * i; m[i] = 1.0 + i * 0.25; }
    i64 hits = 0;
    for (u64 it = 0; it < iters; ++it) {
        for (int i = 0; i < 8; ++i) {
            double ax = 0, ay = 0;
            for (int j = 0; j < 8; ++j) {
                if (j == i) continue;
                double dx = px[j] - px[i], dy = py[j] - py[i];
                double d2 = __builtin_fma(dx, dx, __builtin_fma(dy, dy, 0.01));
                double inv = m[j] / (d2 * __builtin_sqrt(d2));
                ax = __builtin_fma(dx, inv, ax);
                ay = __builtin_fma(dy, inv, ay);
            }
            vx[i] = __builtin_fma(ax, 1e-3, vx[i]);
            vy[i] = __builtin_fma(ay, 1e-3, vy[i]);
        }
        for (int i = 0; i < 8; ++i) {
            px[i] += vx[i] * 1e-3; py[i] += vy[i] * 1e-3;
            if (px[i] > 10.0 || px[i] < -10.0) { vx[i] = -vx[i]; ++hits; }
            hits += (i64)(py[i] * 4.0) & 1;
        }
    }
    u64 bits = 0;
    for (int i = 0; i < 8; ++i) { u64 b; double v = px[i] + py[i] * 3; __builtin_memcpy(&b, &v, 8); bits = rotl64(bits, 9) ^ b; }
    return bits ^ (u64)hits;
}

/* Scalar float: polynomial and conversions (fcvt, scvtf, fcvtzs, fmin/fmax, fabs). */
NOINLINE static u64 k_fp_float(u64 iters) {
    float acc = 0.5f;
    i32 iacc = 0;
    for (u64 i = 0; i < iters; ++i) {
        float x = (float)(i32)(i & 1023) * (1.0f / 1024.0f);
        float p = __builtin_fmaf(__builtin_fmaf(__builtin_fmaf(x, 0.25f, -0.5f), x, 1.0f), x, 0.125f);
        acc = __builtin_fmaxf(__builtin_fminf(acc * 0.999f + p, 1000.0f), -1000.0f);
        iacc += (i32)(acc * 16.0f) + (i32)__builtin_fabsf(p - acc);
        double d = (double)acc;
        if (d > 3.0) acc = (float)(d * 0.5);
    }
    u32 b;
    __builtin_memcpy(&b, &acc, 4);
    return ((u64)b << 32) ^ (u32)iacc;
}

/* ---- vectors --------------------------------------------------------------------------------- */
typedef float f32x4 __attribute__((vector_size(16)));
typedef i32 i32x4 __attribute__((vector_size(16)));
typedef i16 i16x8 __attribute__((vector_size(16)));
typedef u8 u8x16 __attribute__((vector_size(16)));

/* Portable vector code (GCC vector extensions: NEON on arm64, SSE/AVX natively): saxpy and dot
 * products in f32x4, i16x8 multiply-add, u8x16 min/max/average, lane shuffles. */
NOINLINE static u64 k_vec_portable(u64 iters) {
    f32x4 *fa = (f32x4 *)buf_a, *fb = (f32x4 *)buf_b;
    for (int i = 0; i < 256; ++i) for (int l = 0; l < 4; ++l) { fa[i][l] = (float)((i * 4 + l) % 97) * 0.125f; fb[i][l] = 1.0f; }
    i16x8 *ha = (i16x8 *)(buf_a + 8192), *hb = (i16x8 *)(buf_b + 8192);
    for (int i = 0; i < 128; ++i) for (int l = 0; l < 8; ++l) { ha[i][l] = (i16)((i * 8 + l) * 37 % 512 - 256); hb[i][l] = (i16)(l - 4); }
    u8x16 *ba = (u8x16 *)(buf_a + 16384), *bb = (u8x16 *)(buf_b + 16384);
    for (int i = 0; i < 128; ++i) for (int l = 0; l < 16; ++l) { ba[i][l] = (u8)(i * 16 + l); bb[i][l] = (u8)(255 - l * 7); }
    u64 acc = 0;
    for (u64 it = 0; it < iters; ++it) {
        const f32x4 a = {1.0f + (float)(it & 7) * 0.0625f, 0.5f, 0.25f, 2.0f};
        f32x4 dot = {0, 0, 0, 0};
        for (int i = 0; i < 256; ++i) { fb[i] = a * fa[i] + fb[i] * 0.5f; dot += fa[i] * fb[i]; }
        f32x4 sh = __builtin_shufflevector(dot, dot, 2, 3, 0, 1);
        dot += sh;
        i32x4 isum = {0, 0, 0, 0};
        for (int i = 0; i < 128; ++i) {
            i16x8 p = ha[i] * hb[i] + (ha[i] >> 2);
            hb[i] = p & (i16)0x7f;
            isum += __builtin_convertvector(__builtin_shufflevector(p, p, 0, 2, 4, 6), i32x4);
        }
        u8x16 mx, mn;
        for (int i = 0; i < 128; ++i) {
            u8x16 x = ba[i], y = bb[i];
            const u8x16 m = (u8x16)(x > y);
            mx = (x & m) | (y & ~m); mn = (y & m) | (x & ~m);
            bb[i] = mn + ((mx - mn) >> 1);
        }
        u32 fbits;
        float d0 = dot[0];
        __builtin_memcpy(&fbits, &d0, 4);
        acc = rotl64(acc, 3) ^ fbits ^ (u32)(isum[0] + isum[1] + isum[2] + isum[3]) ^ ((u64)bb[it & 127][it & 15] << 40);
    }
    return acc;
}

/* NEON intrinsics with no portable form (table lookups, saturating and widening arithmetic,
 * pairwise adds, structure loads, popcount). The native build runs a scalar version of each so
 * the checksum is comparable; its time is not a like-for-like reference. */
NOINLINE static u64 k_neon_intrin(u64 iters) {
    u8 *src = buf_a, *dst = buf_b + 32768;
    u8 lut[64];
    for (int i = 0; i < 64; ++i) lut[i] = (u8)(i * 13 + 5);
    u64 acc = 0;
    for (u64 it = 0; it < iters; ++it) {
        u32 sad = 0, pop = 0;
#if FXB_ARM64
        const uint8x16x4_t t = vld1q_u8_x4(lut);
        uint32x4_t sadv = vdupq_n_u32(0);
        uint16x8_t popv = vdupq_n_u16(0);
        for (int i = 0; i < 2048; i += 32) {
            uint8x16x2_t v = vld2q_u8(src + i);                      /* deinterleave */
            uint8x16_t idx = vandq_u8(v.val[0], vdupq_n_u8(63));
            uint8x16_t look = vqtbl4q_u8(t, idx);                    /* tbl, 4 registers */
            uint8x16_t sat = vqaddq_u8(look, v.val[1]);              /* uqadd */
            uint8x16_t ad = vabdq_u8(sat, v.val[0]);                 /* uabd */
            sadv = vpadalq_u16(sadv, vpaddlq_u8(ad));                /* pairwise widen */
            int16x8_t w = vreinterpretq_s16_u16(vmull_u8(vget_low_u8(v.val[0]), vget_low_u8(v.val[1]))); /* umull */
            w = vqrshrn_high_n_s32(vqrshrn_n_s32(vmovl_s16(vget_low_s16(w)), 3), vmovl_s16(vget_high_s16(w)), 3);
            popv = vaddq_u16(popv, vpaddlq_u8(vcntq_u8(vreinterpretq_u8_s16(w)))); /* cnt */
            const uint8x16x2_t pair = {{sat, ad}};
            vst2q_u8(dst + i, pair);                                 /* interleave store */
        }
        sad = vaddvq_u32(sadv);
        pop = vaddvq_u16(popv);
#else
        for (int i = 0; i < 2048; i += 32) {
            u8 sat[16], ad[16];
            i16 w[16];
            for (int l = 0; l < 16; ++l) {
                u8 e = src[i + 2 * l], o = src[i + 2 * l + 1];
                u8 look = lut[e & 63];
                u32 s = (u32)look + o;
                sat[l] = s > 255 ? 255 : (u8)s;
                ad[l] = sat[l] > e ? (u8)(sat[l] - e) : (u8)(e - sat[l]);
                sad += ad[l];
                if (l < 8) {
                    i32 prod = (i16)(u16)((u32)e * o);
                    i32 r = (prod + 4) >> 3;
                    w[l] = (i16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
                }
            }
            for (int l = 0; l < 8; ++l) { pop += (u32)__builtin_popcount((u16)w[l]); }
            for (int l = 0; l < 16; ++l) { dst[i + 2 * l] = sat[l]; dst[i + 2 * l + 1] = ad[l]; }
        }
#endif
        acc = rotl64(acc, 11) ^ sad ^ ((u64)pop << 32) ^ dst[(it * 7) & 2047];
        src[(it * 31) & 2047] ^= (u8)it;
    }
    return acc;
}

/* ---- atomics --------------------------------------------------------------------------------- */
/* Uncontended: fetch-add, compare-exchange loops, exchange, acquire loads, release stores and a
 * fence (LSE on the default build, LL/SC loops on the -llsc build). */
static _Atomic u64 at_counter, at_flag, at_value;
static _Atomic u32 at_small;
NOINLINE static u64 k_atomics(u64 iters) {
    u64 acc = 0;
    for (u64 i = 0; i < iters; ++i) {
        acc += atomic_fetch_add_explicit(&at_counter, i & 7, memory_order_relaxed);
        u64 expect = atomic_load_explicit(&at_value, memory_order_acquire);
        while (!atomic_compare_exchange_weak_explicit(&at_value, &expect, expect * 3 + 1, memory_order_acq_rel, memory_order_acquire)) {}
        atomic_store_explicit(&at_flag, i, memory_order_release);
        acc ^= atomic_exchange_explicit(&at_small, (u32)i, memory_order_seq_cst);
        if ((i & 63) == 0) atomic_thread_fence(memory_order_seq_cst);
        acc += atomic_fetch_or_explicit(&at_small, 1u << (i & 7), memory_order_relaxed) & 0xff;
    }
    return acc ^ atomic_load(&at_counter) ^ atomic_load(&at_value) ^ atomic_load(&at_flag);
}

/* Contended: 4 threads share a counter and a spin lock guarding a plain counter. */
static _Atomic u32 spin;
static u64 guarded;
static _Atomic u64 shared_counter;
typedef struct { u64 iters; } worker_arg;
static void *contend_worker(void *p) {
    const u64 n = ((worker_arg *)p)->iters;
    for (u64 i = 0; i < n; ++i) {
        atomic_fetch_add_explicit(&shared_counter, 1, memory_order_relaxed);
        if ((i & 7) == 0) {
            u32 z = 0;
            while (!atomic_compare_exchange_weak_explicit(&spin, &z, 1, memory_order_acquire, memory_order_relaxed)) z = 0;
            guarded += i & 3;
            atomic_store_explicit(&spin, 0, memory_order_release);
        }
    }
    return 0;
}
NOINLINE static u64 k_atomics_contended(u64 iters) {
    enum { T = 4 };
    thread_t t[T];
    worker_arg arg = {iters / T};
    atomic_store(&shared_counter, 0);
    guarded = 0;
    for (int i = 0; i < T; ++i) if (thread_start(&t[i], contend_worker, &arg)) return 0xdead;
    for (int i = 0; i < T; ++i) thread_join(t[i]);
    return atomic_load(&shared_counter) * 1000003u + guarded;
}

/* ---- driver ---------------------------------------------------------------------------------- */
typedef struct { const char *name; u64 (*fn)(u64); u64 iters; } kernel_t;
static const kernel_t kernels[] = {
    {"int_hash", k_int_hash, 20000},
    {"int_mix", k_int_mix, 4000000},
    {"branchy", k_branchy, 20000},
    {"interp_switch", k_interp_switch, 1500000},
    {"interp_goto", k_interp_goto, 1500000},
    {"indirect_calls", k_indirect_calls, 3000000},
    {"call_ret", k_call_ret, 300},
    {"mem_own", k_mem_own, 60000},
    {"libc_calls", k_libc_calls, 1000000},
    {"ptr_chase", k_ptr_chase, 50000},
    {"fp_scalar", k_fp_scalar, 60000},
    {"fp_float", k_fp_float, 6000000},
    {"vec_portable", k_vec_portable, 20000},
    {"neon_intrin", k_neon_intrin, 6000},
    {"atomics", k_atomics, 3000000},
    {"atomics_contended", k_atomics_contended, 4000000},
};
#define NKERNELS (sizeof(kernels) / sizeof(kernels[0]))

static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static u64 parse_u64(const char *s) { u64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0'); return v; }
static int in_list(const char *list, const char *name) {
    /* comma-separated */
    const char *p = list;
    while (*p) {
        const char *n = name;
        while (*p && *p != ',' && *p == *n) { ++p; ++n; }
        if ((*p == ',' || !*p) && !*n) return 1;
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
    }
    return 0;
}
static void sort_u64(u64 *v, int n) {
    for (int i = 1; i < n; ++i) { u64 t = v[i]; int j = i - 1; while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; --j; } v[j + 1] = t; }
}

int main(int argc, char **argv) {
    u64 scale_pct = 100, reps = 5;
    const char *only = 0;
    for (int i = 1; i < argc; ++i) {
        if (str_eq(argv[i], "-scale") && i + 1 < argc) scale_pct = parse_u64(argv[++i]);
        else if (str_eq(argv[i], "-reps") && i + 1 < argc) reps = parse_u64(argv[++i]);
        else if (str_eq(argv[i], "-only") && i + 1 < argc) only = argv[++i];
        else if (str_eq(argv[i], "-list")) { for (unsigned k = 0; k < NKERNELS; ++k) printf("%s\n", kernels[k].name); return 0; }
    }
    if (reps < 1) reps = 1;
    if (reps > 31) reps = 31;
    clock_init();
    fill_random(buf_a, BUF_BYTES, 1);
    int fails = 0;
    for (unsigned k = 0; k < NKERNELS; ++k) {
        const kernel_t *K = &kernels[k];
        if (only && !in_list(only, K->name)) continue;
        u64 iters = K->iters * scale_pct / 100;
        if (iters < 1) iters = 1;
        /* Warm-up (translation, caches) with the same inputs: every rep restores the global state. */
        u64 times[32], checksum = 0;
        for (u64 r = 0; r <= reps; ++r) {
            fill_random(buf_a, BUF_BYTES, 1);
            memset(buf_b, 0, BUF_BYTES);
            atomic_store(&at_counter, 0); atomic_store(&at_flag, 0); atomic_store(&at_value, 1); atomic_store(&at_small, 0);
            const u64 t0 = now_ns();
            const u64 c = K->fn(iters);
            const u64 t1 = now_ns();
            if (r == 0) checksum = c;
            else {
                times[r - 1] = t1 - t0;
                if (c != checksum) { ++fails; printf("FXBENCH kernel=%s unstable checksum %llx vs %llx\n", K->name, (unsigned long long)c, (unsigned long long)checksum); }
            }
        }
        sort_u64(times, (int)reps);
        const u64 med = times[reps / 2], mn = times[0];
        printf("FXBENCH kernel=%s iters=%llu reps=%llu median_ps=%llu min_ps=%llu checksum=%llx\n", K->name,
               (unsigned long long)iters, (unsigned long long)reps, (unsigned long long)(med * 1000 / iters),
               (unsigned long long)(mn * 1000 / iters), (unsigned long long)checksum);
    }
    printf("FXBENCH done fails=%d\n", fails);
    return fails ? 1 : 0;
}
