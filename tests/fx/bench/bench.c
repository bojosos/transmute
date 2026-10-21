// fx performance benchmarks (owner: corpus/bench). One source, compiled per
// group (-DB_<GROUP>) for ARM64 (run under transmute-fx and the old engine)
// and for x64 (run natively). See scripts/fx_bench.ps1 and build.ps1.
//
// No CRT: entry point guest_entry, kernel32 only, except the heap group
// (malloc/free from the UCRT) and the memcpy group (memcpy from vcruntime140).
//
// Output, one line per measurement:
//   BENCH <name> ops=<n> ns=<total ns> check=<hex> [bytes=<bytes per op>]
//   INFO <key>=<value>
// Thread-scaling names carry "@<threads>". The optional argument scale=<n>
// divides every iteration count (used for slow engines; results are per op).
#include <windows.h>
#if defined(_M_ARM64)
#include <arm_neon.h>
#else
#include <emmintrin.h>
#endif

int _fltused = 0;

typedef unsigned long long u64;

#if defined(B_HEAP)
void* __cdecl malloc(size_t size);
void __cdecl free(void* block);
#endif

// ---- output -----------------------------------------------------------------
static void out(const char* s) {
    DWORD n = 0, len = 0;
    while (s[len]) ++len;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, len, &n, 0);
}
static void out_u64(u64 v) {
    char buf[32];
    int i = 31;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    out(buf + i);
}
static void out_hex(u64 v) {
    char buf[32];
    int i = 31;
    buf[i] = 0;
    do {
        buf[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    out(buf + i);
}

static LARGE_INTEGER freq;
static u64 now_ticks(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}
static u64 ticks_to_ns(u64 t) {
    u64 f = (u64)freq.QuadPart;
    return (t / f) * 1000000000ULL + (t % f) * 1000000000ULL / f;
}

static void report_bytes(const char* name, const char* suffix, u64 threads, u64 ops, u64 ticks, u64 check,
                         u64 bytes) {
    out("BENCH ");
    out(name);
    if (suffix) out(suffix);
    if (threads) {
        out("@");
        out_u64(threads);
    }
    out(" ops=");
    out_u64(ops);
    out(" ns=");
    out_u64(ticks_to_ns(ticks));
    out(" check=");
    out_hex(check);
    if (bytes) {
        out(" bytes=");
        out_u64(bytes);
    }
    out("\r\n");
}
static void report(const char* name, u64 ops, u64 ticks, u64 check) {
    report_bytes(name, 0, 0, ops, ticks, check, 0);
}

static u64 g_scale = 1;
static void parse_args(void) {
    const char* p = GetCommandLineA();
    for (; *p; ++p) {
        if (p[0] == 's' && p[1] == 'c' && p[2] == 'a' && p[3] == 'l' && p[4] == 'e' && p[5] == '=') {
            u64 v = 0;
            for (p += 6; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (u64)(*p - '0');
            if (v) g_scale = v;
            break;
        }
    }
}
static u64 scaled(u64 n) {
    u64 v = n / g_scale;
    return v ? v : 1;
}
static u64 warmup(u64 n) {
    u64 v = n / 100;
    return v ? v : 1;
}

static volatile u64 sink;

// ---- integer compute loop ---------------------------------------------------
__declspec(noinline) static u64 compute(u64 n, u64 seed) {
    u64 x = seed | 1, y = 0x9abcdef;
    for (u64 i = 0; i < n; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        y ^= x >> 17;
        y = (y << 5) | (y >> 59);
        y += i;
    }
    return x ^ y;
}

// ---- dependent loads + stores ------------------------------------------------
__declspec(noinline) static u64 memwork(unsigned* a, unsigned mask, u64 n) {
    u64 sum = 0;
    unsigned idx = 1;
    for (u64 i = 0; i < n; i++) {
        unsigned v = a[idx];
        a[(idx * 7) & mask] = v * 3 + (unsigned)i;
        sum += v;
        idx = (idx + v + 1) & mask;
    }
    return sum;
}

// ---- vectorized float FMA (ARM64: FMLA) -------------------------------------
__declspec(noinline) static float fpwork(float* a, const float* b, const float* c, int len, u64 reps) {
    for (u64 r = 0; r < reps; r++) {
#pragma clang loop vectorize(enable) interleave_count(2)
        for (int i = 0; i < len; i++) a[i] = a[i] * b[i] + c[i];
    }
    float s = 0;
    for (int i = 0; i < len; i++) s += a[i];
    return s;
}

// ---- threads -----------------------------------------------------------------
typedef struct {
    u64 n;
    u64 result;
    unsigned* mem;
    volatile LONG* shared;
    int mode;
} Work;

static DWORD WINAPI worker(LPVOID p) {
    Work* w = (Work*)p;
    if (w->mode == 0) {
        w->result = compute(w->n, (u64)(ULONG_PTR)p);
    } else if (w->mode == 1) {
        w->result = memwork(w->mem, (1u << 16) - 1, w->n);
    } else if (w->mode == 2) {
        for (u64 i = 0; i < w->n; i++) InterlockedIncrement(w->shared);
    } else {
        volatile LONG local = 0;
        for (u64 i = 0; i < w->n; i++) InterlockedIncrement(&local);
        w->result = (u64)local;
    }
    return 0;
}

static void threads(const char* name, int mode, u64 n_per_thread) {
    static volatile LONG shared;
    for (int t = 1; t <= 8; t *= 2) {
        Work work[8];
        HANDLE h[8];
        for (int i = 0; i < t; i++) {
            work[i].n = n_per_thread;
            work[i].result = 0;
            work[i].mode = mode;
            work[i].shared = &shared;
            work[i].mem = (unsigned*)VirtualAlloc(0, (1u << 16) * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        u64 start = now_ticks();
        for (int i = 0; i < t; i++) h[i] = CreateThread(0, 0, worker, &work[i], 0, 0);
        WaitForMultipleObjects((DWORD)t, h, TRUE, INFINITE);
        u64 ticks = now_ticks() - start;
        u64 chk = 0;
        for (int i = 0; i < t; i++) {
            CloseHandle(h[i]);
            chk += work[i].result;
            VirtualFree(work[i].mem, 0, MEM_RELEASE);
        }
        report_bytes(name, 0, (u64)t, n_per_thread, ticks, chk, 0);
    }
}

// ---- indirect calls / virtual dispatch --------------------------------------
typedef u64 (*OpFn)(u64, u64);
__declspec(noinline) static u64 op0(u64 a, u64 b) { return a + b; }
__declspec(noinline) static u64 op1(u64 a, u64 b) { return a ^ (b << 1); }
__declspec(noinline) static u64 op2(u64 a, u64 b) { return a * 3 + b; }
__declspec(noinline) static u64 op3(u64 a, u64 b) { return (a >> 1) + b; }
__declspec(noinline) static u64 op4(u64 a, u64 b) { return a - b; }
__declspec(noinline) static u64 op5(u64 a, u64 b) { return a ^ b; }
__declspec(noinline) static u64 op6(u64 a, u64 b) { return (a << 2) ^ b; }
__declspec(noinline) static u64 op7(u64 a, u64 b) { return a + (b >> 2); }
static OpFn volatile op_table[8] = {op0, op1, op2, op3, op4, op5, op6, op7};

// Direct calls of small leaf functions (BL + RET; the fx translator inlines them).
__declspec(noinline) static u64 dcall_loop(u64 n) {
    u64 acc = 1;
    for (u64 i = 0; i < n; i++) acc = op2(op1(acc, i), i);
    return acc;
}

__declspec(noinline) static u64 icall_loop(OpFn* table, u64 n) {
    u64 acc = 1;
    for (u64 i = 0; i < n; i++) acc = table[i & 7](acc, i);
    return acc;
}

typedef struct Shape Shape;
typedef struct {
    u64 (*area)(const Shape*);
    u64 (*grow)(Shape*, u64);
} VTable;
struct Shape {
    const VTable* vt;
    u64 a, b;
};
__declspec(noinline) static u64 rect_area(const Shape* s) { return s->a * s->b; }
__declspec(noinline) static u64 tri_area(const Shape* s) { return (s->a * s->b) >> 1; }
__declspec(noinline) static u64 sq_area(const Shape* s) { return s->a * s->a; }
__declspec(noinline) static u64 line_area(const Shape* s) { return s->a + s->b; }
__declspec(noinline) static u64 grow_a(Shape* s, u64 v) { return s->a += v & 1; }
__declspec(noinline) static u64 grow_b(Shape* s, u64 v) { return s->b += v & 1; }
static const VTable vt_rect = {rect_area, grow_a}, vt_tri = {tri_area, grow_b}, vt_sq = {sq_area, grow_a},
                    vt_line = {line_area, grow_b};

__declspec(noinline) static u64 vcall_loop(Shape* shapes, u64 n) {
    u64 acc = 0;
    for (u64 i = 0; i < n; i++) {
        Shape* s = &shapes[i & 63];
        acc += s->vt->area(s);
        if ((i & 15) == 0) acc += s->vt->grow(s, i);
    }
    return acc;
}

// ---- recursion (call/return) -------------------------------------------------
// The combining expressions are deliberately not "call op invariant", so
// LLVM's tail-recursion elimination cannot turn either function into a loop.
__declspec(noinline) static u64 fib(unsigned n) {
    if (n < 2) return n;
    u64 a = fib(n - 1);
    u64 b = fib(n - 2);
    return a + (b | (b >> 40)); // == a + b for these sizes
}
__declspec(noinline) static u64 deep(unsigned n, u64 acc) {
    if (n == 0) return acc;
    u64 r = deep(n - 1, acc * 31 + n);
    return (r * 31) ^ n;
}

// ---- memcpy ------------------------------------------------------------------
#if defined(B_MEMCPY)
void* __cdecl memcpy(void* dst, const void* src, size_t n);

__declspec(noinline) static void simd_copy(unsigned char* dst, const unsigned char* src, size_t n) {
    for (size_t i = 0; i < n; i += 64) {
#if defined(_M_ARM64)
        uint8x16_t a = vld1q_u8(src + i), b = vld1q_u8(src + i + 16), c = vld1q_u8(src + i + 32),
                   d = vld1q_u8(src + i + 48);
        vst1q_u8(dst + i, a);
        vst1q_u8(dst + i + 16, b);
        vst1q_u8(dst + i + 32, c);
        vst1q_u8(dst + i + 48, d);
#else
        __m128i a = _mm_loadu_si128((const __m128i*)(src + i)), b = _mm_loadu_si128((const __m128i*)(src + i + 16)),
                c = _mm_loadu_si128((const __m128i*)(src + i + 32)), d = _mm_loadu_si128((const __m128i*)(src + i + 48));
        _mm_storeu_si128((__m128i*)(dst + i), a);
        _mm_storeu_si128((__m128i*)(dst + i + 16), b);
        _mm_storeu_si128((__m128i*)(dst + i + 32), c);
        _mm_storeu_si128((__m128i*)(dst + i + 48), d);
#endif
    }
}

static void bench_copy(const char* name, int use_crt, unsigned char* dst, unsigned char* src, size_t size, u64 n) {
    for (u64 i = 0; i < warmup(n); i++) {
        if (use_crt) memcpy(dst, src, size);
        else simd_copy(dst, src, size);
    }
    u64 t0 = now_ticks();
    for (u64 i = 0; i < n; i++) {
        src[i & 63] = (unsigned char)i; // keep every copy observable
        if (use_crt) memcpy(dst, src, size);
        else simd_copy(dst, src, size);
    }
    u64 ticks = now_ticks() - t0;
    report_bytes(name, 0, 0, n, ticks, (u64)dst[0] + dst[size - 1] + dst[size / 2], (u64)size);
}
#endif

// ---- switch-heavy interpreter ------------------------------------------------
enum { OP_PUSHI, OP_LOAD, OP_STORE, OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_XOR, OP_SHR, OP_SHL, OP_DUP, OP_POP, OP_JNZ,
       OP_NOP, OP_HALT };

__declspec(noinline) static u64 interpret(const unsigned char* code, const int* args, u64 iterations) {
    u64 stack[16];
    u64 locals[4];
    int sp = 0, pc = 0;
    locals[0] = iterations;
    locals[1] = 0x12345;
    locals[2] = 0;
    locals[3] = 0;
    for (;;) {
        unsigned char op = code[pc];
        int arg = args[pc];
        pc++;
        switch (op) {
        case OP_PUSHI: stack[sp++] = (u64)(unsigned)arg; break;
        case OP_LOAD: stack[sp++] = locals[arg]; break;
        case OP_STORE: locals[arg] = stack[--sp]; break;
        case OP_ADD: sp--; stack[sp - 1] += stack[sp]; break;
        case OP_SUB: sp--; stack[sp - 1] -= stack[sp]; break;
        case OP_MUL: sp--; stack[sp - 1] *= stack[sp]; break;
        case OP_AND: sp--; stack[sp - 1] &= stack[sp]; break;
        case OP_XOR: sp--; stack[sp - 1] ^= stack[sp]; break;
        case OP_SHR: stack[sp - 1] >>= arg; break;
        case OP_SHL: stack[sp - 1] <<= arg; break;
        case OP_DUP: stack[sp] = stack[sp - 1]; sp++; break;
        case OP_POP: sp--; break;
        case OP_JNZ: if (stack[--sp]) pc = arg; break;
        case OP_NOP: break;
        case OP_HALT: return locals[1] ^ (locals[2] << 32);
        default: return 0;
        }
    }
}

static const unsigned char vm_code[] = {
    OP_LOAD, OP_PUSHI, OP_MUL, OP_LOAD, OP_XOR, OP_DUP, OP_SHR, OP_XOR, OP_STORE,           // h = (h*31 ^ i) ^ >>13
    OP_LOAD, OP_LOAD, OP_PUSHI, OP_AND, OP_ADD, OP_STORE,                                // acc += h & 255
    OP_LOAD, OP_PUSHI, OP_SUB, OP_DUP, OP_STORE, OP_JNZ,                                 // if (--i) loop
    OP_HALT};
static const int vm_args[] = {1, 31, 0, 0, 0, 0, 13, 0, 1, 2, 1, 255, 0, 0, 2, 0, 1, 0, 0, 0, 0, 0};
#define VM_LOOP_INSNS 21

// ---- entry ------------------------------------------------------------------
void guest_entry(void) {
    QueryPerformanceFrequency(&freq);
    parse_args();
    DWORD_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    out("INFO affinity=0x");
    out_hex((u64)process_mask);
    out(" scale=");
    out_u64(g_scale);
    out("\r\n");
    u64 t0, r, n;
    (void)t0;
    (void)r;
    (void)n;

#if defined(B_COMPUTE)
    n = scaled(100000000ULL);
    sink = compute(warmup(n), 3);
    t0 = now_ticks();
    r = compute(n, 7);
    report("compute", n, now_ticks() - t0, r);
#elif defined(B_MEM)
    unsigned* a = (unsigned*)VirtualAlloc(0, (1u << 20) * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    n = scaled(50000000ULL);
    sink = memwork(a, (1u << 20) - 1, warmup(n));
    t0 = now_ticks();
    r = memwork(a, (1u << 20) - 1, n);
    report("mem", n, now_ticks() - t0, r);
#elif defined(B_FMA)
    float* a = (float*)VirtualAlloc(0, 3 * 4096 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    float* b = a + 4096;
    float* c = b + 4096;
    for (int i = 0; i < 4096; i++) {
        a[i] = 1.0f;
        b[i] = 0.999f;
        c[i] = 0.001f * (float)(i & 7);
    }
    u64 reps = scaled(50000);
    sink = (u64)fpwork(a, b, c, 4096, warmup(reps));
    t0 = now_ticks();
    float s = fpwork(a, b, c, 4096, reps);
    report("fma", 4096ULL * reps, now_ticks() - t0, (u64)s);
#elif defined(B_CALLS)
    u64 acc = 0;
    n = scaled(10000000ULL);
    for (u64 i = 0; i < warmup(n); i++) acc += GetCurrentThreadId();
    t0 = now_ticks();
    for (u64 i = 0; i < n; i++) acc += GetCurrentThreadId();
    report("GetCurrentThreadId", n, now_ticks() - t0, acc);

    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    for (u64 i = 0; i < warmup(n); i++) {
        EnterCriticalSection(&cs);
        LeaveCriticalSection(&cs);
    }
    t0 = now_ticks();
    for (u64 i = 0; i < n; i++) {
        EnterCriticalSection(&cs);
        LeaveCriticalSection(&cs);
    }
    report("CriticalSection_pair", n, now_ticks() - t0, 0);

    SRWLOCK srw = SRWLOCK_INIT;
    for (u64 i = 0; i < warmup(n); i++) {
        AcquireSRWLockExclusive(&srw);
        ReleaseSRWLockExclusive(&srw);
    }
    t0 = now_ticks();
    for (u64 i = 0; i < n; i++) {
        AcquireSRWLockExclusive(&srw);
        ReleaseSRWLockExclusive(&srw);
    }
    report("SRWLock_pair", n, now_ticks() - t0, 0);

    DWORD tls = TlsAlloc();
    TlsSetValue(tls, (LPVOID)5);
    for (u64 i = 0; i < warmup(n); i++) acc += (ULONG_PTR)TlsGetValue(tls);
    t0 = now_ticks();
    for (u64 i = 0; i < n; i++) acc += (ULONG_PTR)TlsGetValue(tls);
    report("TlsGetValue", n, now_ticks() - t0, acc);

    for (u64 i = 0; i < warmup(n); i++) {
        SetLastError((DWORD)i);
        acc += GetLastError();
    }
    t0 = now_ticks();
    for (u64 i = 0; i < n; i++) {
        SetLastError((DWORD)i);
        acc += GetLastError();
    }
    report("SetLastError_GetLastError", n, now_ticks() - t0, acc);
#elif defined(B_HEAP)
    HANDLE heap = GetProcessHeap();
    static void* keep[64];
    n = scaled(2000000ULL);
    for (int pass = 0; pass < 2; pass++) {
        u64 count = pass ? n : warmup(n);
        t0 = now_ticks();
        for (u64 i = 0; i < count; i++) {
            unsigned slot = (unsigned)(i & 63);
            if (keep[slot]) HeapFree(heap, 0, keep[slot]);
            keep[slot] = HeapAlloc(heap, 0, 16 + (i & 255));
            ((char*)keep[slot])[0] = (char)i;
        }
        if (pass) report("HeapAlloc_HeapFree", n, now_ticks() - t0, 0);
    }
    for (int i = 0; i < 64; i++) {
        HeapFree(heap, 0, keep[i]);
        keep[i] = 0;
    }
    for (int pass = 0; pass < 2; pass++) {
        u64 count = pass ? n : warmup(n);
        t0 = now_ticks();
        for (u64 i = 0; i < count; i++) {
            unsigned slot = (unsigned)(i & 63);
            free(keep[slot]);
            keep[slot] = malloc(16 + (i & 255));
            ((char*)keep[slot])[0] = (char)i;
        }
        if (pass) report("malloc_free", n, now_ticks() - t0, 0);
    }
    for (int i = 0; i < 64; i++) free(keep[i]);
#elif defined(B_THREADS)
    threads("threads_compute", 0, scaled(20000000ULL));
    threads("threads_mem", 1, scaled(10000000ULL));
    threads("threads_atomic_shared", 2, scaled(2000000ULL));
    threads("threads_atomic_private", 3, scaled(2000000ULL));
#elif defined(B_ICALL)
    n = scaled(100000000ULL);
    sink = dcall_loop(warmup(n));
    t0 = now_ticks();
    r = dcall_loop(n);
    report("direct_call_pair", n, now_ticks() - t0, r);
    sink = icall_loop((OpFn*)op_table, warmup(n));
    t0 = now_ticks();
    r = icall_loop((OpFn*)op_table, n);
    report("indirect_call", n, now_ticks() - t0, r);
    static Shape shapes[64];
    const VTable* vts[4] = {&vt_rect, &vt_tri, &vt_sq, &vt_line};
    for (int i = 0; i < 64; i++) {
        shapes[i].vt = vts[(i * 5) & 3];
        shapes[i].a = (u64)i + 1;
        shapes[i].b = (u64)(i * 3) + 2;
    }
    sink = vcall_loop(shapes, warmup(n));
    t0 = now_ticks();
    r = vcall_loop(shapes, n);
    report("virtual_call", n, now_ticks() - t0, r);
#elif defined(B_RECURSION)
    unsigned fib_n = g_scale >= 100 ? 24 : g_scale >= 10 ? 28 : 34;
    sink = fib(20);
    t0 = now_ticks();
    r = fib(fib_n);
    // calls(n) = 2 * fib(n + 1) - 1
    report("recursion_fib", 2 * fib(fib_n + 1) - 1, now_ticks() - t0, r);
    n = scaled(2000);
    sink = deep(5000, 1);
    t0 = now_ticks();
    r = 0;
    for (u64 i = 0; i < n; i++) r += deep(5000, i);
    report("recursion_deep", n * 5001, now_ticks() - t0, r);
#elif defined(B_MEMCPY)
    unsigned char* src = (unsigned char*)VirtualAlloc(0, (16u << 20) + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char* dst = (unsigned char*)VirtualAlloc(0, (16u << 20) + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (u64 i = 0; i < (16u << 20); i += 64) src[i] = (unsigned char)(i >> 6);
    bench_copy("memcpy_crt_64", 1, dst, src, 64, scaled(20000000ULL));
    bench_copy("memcpy_crt_4k", 1, dst, src, 4096, scaled(2000000ULL));
    bench_copy("memcpy_crt_256k", 1, dst, src, 256 << 10, scaled(20000ULL));
    bench_copy("memcpy_crt_16m", 1, dst, src, 16u << 20, scaled(200ULL));
    bench_copy("memcpy_simd_64", 0, dst, src, 64, scaled(20000000ULL));
    bench_copy("memcpy_simd_4k", 0, dst, src, 4096, scaled(2000000ULL));
    bench_copy("memcpy_simd_256k", 0, dst, src, 256 << 10, scaled(20000ULL));
    bench_copy("memcpy_simd_16m", 0, dst, src, 16u << 20, scaled(200ULL));
#elif defined(B_INTERP)
    n = scaled(5000000ULL);
    sink = interpret(vm_code, vm_args, warmup(n));
    t0 = now_ticks();
    r = interpret(vm_code, vm_args, n);
    report("interpreter", n * VM_LOOP_INSNS + 1, now_ticks() - t0, r);
#else
#error define one B_<GROUP>
#endif
    ExitProcess(0);
}
