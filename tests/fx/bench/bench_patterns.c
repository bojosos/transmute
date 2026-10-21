// Classroom-inspired patterns, not extracted Blender kernels.
// Finite, nonzero inputs let min/max and compare/select do identical work.
// Build both architectures with FP contraction and auto-vectorization disabled.
#include <windows.h>
#if defined(_M_ARM64)
#include <arm_neon.h>
typedef float32x4_t V;
#define LOAD(p) vld1q_f32(p)
#define STORE(p, x) vst1q_f32(p, x)
#define ADD(a, b) vaddq_f32(a, b)
#define MUL(a, b) vmulq_f32(a, b)
#define SUB(a, b) vsubq_f32(a, b)
#define FMA(a, b, c) vfmaq_f32(c, a, b)
#define SHUFFLE2(a) vcombine_f32(vget_high_f32(a), vget_high_f32(a))
#define SHUFFLE1(a) vdupq_laneq_f32(a, 1)
#define LANE0(a) vgetq_lane_f32(a, 0)
#define SPLAT(x) vdupq_n_f32(x)
#define MIN(a, b) vminq_f32(a, b)
#define MAX(a, b) vmaxq_f32(a, b)
#define SELMIN(a, b) vbslq_f32(vcltq_f32(a, b), a, b)
#define SELMAX(a, b) vbslq_f32(vcgtq_f32(a, b), a, b)
#else
#include <immintrin.h>
typedef __m128 V;
#define LOAD(p) _mm_loadu_ps(p)
#define STORE(p, x) _mm_storeu_ps(p, x)
#define ADD(a, b) _mm_add_ps(a, b)
#define MUL(a, b) _mm_mul_ps(a, b)
#define SUB(a, b) _mm_sub_ps(a, b)
#define FMA(a, b, c) _mm_fmadd_ps(a, b, c)
#define SHUFFLE2(a) _mm_shuffle_ps(a, a, 0xee)
#define SHUFFLE1(a) _mm_shuffle_ps(a, a, 0x55)
#define LANE0(a) _mm_cvtss_f32(a)
#define SPLAT(x) _mm_set1_ps(x)
#define MIN(a, b) _mm_min_ps(a, b)
#define MAX(a, b) _mm_max_ps(a, b)
#define SELMIN(a, b) _mm_blendv_ps(b, a, _mm_cmplt_ps(a, b))
#define SELMAX(a, b) _mm_blendv_ps(b, a, _mm_cmpgt_ps(a, b))
#endif

int _fltused;
typedef unsigned long long U64;
typedef unsigned int U32;
#define N 256
static float input[N][4], output[N][4], texture[4096];
static float large_texture[2048 * 2048];
static LARGE_INTEGER frequency;
static volatile U64 sink;

static void text(const char *s) {
    DWORD n, size = 0;
    while (s[size]) ++size;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, size, &n, 0);
}
static void number(U64 x, unsigned base) {
    char s[32]; unsigned n = 31; s[n] = 0;
    do { s[--n] = "0123456789abcdef"[x % base]; x /= base; } while (x);
    text(s + n);
}
static U64 ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static U64 checksum(unsigned lanes) {
    U64 h = 1469598103934665603ULL;
    for (unsigned i = 0; i < N; ++i) {
        for (unsigned lane = 0; lane < lanes; ++lane) {
            union { float f; U32 u; } v;
            v.f = output[i][lane]; h = (h ^ v.u) * 1099511628211ULL;
        }
    }
    return h;
}

#define MINMAX_KERNEL(name, lo, hi) \
__declspec(noinline) static U64 name(U64 reps) { \
    for (U64 r = 0; r < reps; ++r) { \
        V a = SPLAT(0.2f), b = SPLAT(0.7f); \
        for (unsigned i = 0; i < N; ++i) { \
            V x = LOAD(input[i]); \
            a = lo(ADD(MUL(a, SPLAT(0.99f)), x), b); \
            b = hi(MUL(b, SPLAT(0.98f)), x); \
            STORE(output[i], ADD(a, b)); \
        } \
    } \
    return checksum(4); \
}
MINMAX_KERNEL(minmax, MIN, MAX)
MINMAX_KERNEL(compare_select, SELMIN, SELMAX)

// Similar scalar FP lifetimes to shader/noise code, with calls optionally kept.
static __forceinline float polynomial(float x, float y, float z) {
    float a = x * x * (3.0f - 2.0f * x);
    float b = y * y * (3.0f - 2.0f * y);
    float c = z * z * (3.0f - 2.0f * z);
    return (a * (1.0f - b) + b * c) * 0.5f;
}
__declspec(noinline) static float polynomial_call(float x, float y, float z) {
    return polynomial(x, y, z);
}
#define POLY_KERNEL(name, fn) \
__declspec(noinline) static U64 name(U64 reps) { \
    for (U64 r = 0; r < reps; ++r) { \
        float a = 0.3f, b = 0.4f, c = 0.5f; \
        for (unsigned i = 0; i < N; ++i) { \
            a = fn(input[i][0], b, c); \
            b = fn(input[i][1], c, a); \
            c = fn(input[i][2], a, b); \
            output[i][0] = a + b + c; \
        } \
    } \
    return checksum(1); \
}
POLY_KERNEL(scalar_inline, polynomial)
POLY_KERNEL(scalar_calls, polynomial_call)

__declspec(noinline) static U64 texture_gather(U64 reps) {
    U32 state = 12345;
    for (U64 r = 0; r < reps; ++r)
        for (unsigned i = 0; i < N; ++i) {
            state = state * 1664525U + 1013904223U;
            unsigned x = (state >> 16) & 63, y = (state >> 24) & 63;
            float u = input[i][0], v = input[i][1];
            float a = texture[y * 64 + x], b = texture[y * 64 + ((x + 1) & 63)];
            float c = texture[((y + 1) & 63) * 64 + x];
            float d = texture[((y + 1) & 63) * 64 + ((x + 1) & 63)];
            output[i][0] = (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
        }
    return checksum(1);
}

// Same four-tap pattern with a 16 MiB texture, so data stalls can hide translation cost.
__declspec(noinline) static U64 texture_large(U64 reps) {
    U32 state = 12345;
    for (U64 r = 0; r < reps; ++r)
        for (unsigned i = 0; i < N; ++i) {
            state = state * 1664525U + 1013904223U;
            unsigned x = (state >> 5) & 2047, y = (state >> 16) & 2047;
            float u = input[i][0], v = input[i][1];
            float a = large_texture[y * 2048 + x], b = large_texture[y * 2048 + ((x + 1) & 2047)];
            float c = large_texture[((y + 1) & 2047) * 2048 + x];
            float d = large_texture[((y + 1) & 2047) * 2048 + ((x + 1) & 2047)];
            output[i][0] = (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
        }
    return checksum(1);
}

// Mirrors noise_fbm's shuffle/subtract/FMLA chain followed by scalar accumulation.
__declspec(noinline) static U64 noise_lerp(U64 reps) {
    for (U64 r = 0; r < reps; ++r)
        for (unsigned i = 0; i < N; ++i) {
            float sum = 0, amplitude = 0.5f;
            for (unsigned octave = 0; octave < 6; ++octave) {
                V a = LOAD(input[(i + octave * 13) & (N - 1)]);
                V b = LOAD(input[(i + octave * 13 + 7) & (N - 1)]);
                V x = FMA(SUB(b, a), SPLAT(input[i][0]), a);
                x = FMA(SUB(SHUFFLE2(x), x), SPLAT(input[i][1]), x);
                x = FMA(SUB(SHUFFLE1(x), x), SPLAT(input[i][2]), x);
                sum += LANE0(x) * amplitude;
                amplitude *= 0.5f;
            }
            output[i][0] = sum;
        }
    return checksum(1);
}

// Embree's FCMxx / AND 1 / USHL {0,1,2,3} / ADDV / FMOV movemask idiom.
__declspec(noinline) static U64 mask_reduce(U64 reps) {
    U64 hits = 0;
    for (U64 r = 0; r < reps; ++r)
        for (unsigned i = 0; i < N; ++i) {
            V a = LOAD(input[i]), b = LOAD(input[(i + (unsigned)r + 37) & (N - 1)]);
#if defined(_M_ARM64)
            const int32x4_t shift = {0, 1, 2, 3};
            uint32x4_t bits = vandq_u32(vcltq_f32(a, b), vdupq_n_u32(1));
            hits += vaddvq_u32(vshlq_u32(bits, shift));
#else
            hits += (unsigned)_mm_movemask_ps(_mm_cmplt_ps(a, b));
#endif
        }
    return hits;
}

// Dirty scalar values survive data-dependent branch joins and a loop back edge.
__declspec(noinline) static U64 branch_live(U64 reps) {
    U32 state = 12345;
    float a = .1f, b = .2f, c = .3f, d = .4f, e = .5f, f = .6f;
    for (U64 r = 0; r < reps; ++r)
        for (unsigned i = 0; i < N; ++i) {
            state = state * 1664525U + 1013904223U;
            float x = input[i][0];
            if (state & 0x80000000U) {
                a = a * .99f + x * .01f; b = b * .98f + a * .02f; c = c * .97f + b * .03f;
            } else {
                d = d * .99f + x * .01f; e = e * .98f + d * .02f; f = f * .97f + e * .03f;
            }
            output[i][0] = a + b + c + d + e + f;
        }
    return checksum(1);
}

static void run(const char *name, U64 (*fn)(U64), U64 reps) {
    sink = fn(100);
    U64 cycles_start, cycles_end;
    if (!QueryThreadCycleTime((HANDLE)(LONG_PTR)-2, &cycles_start)) ExitProcess(90);
    U64 start = ticks(), check = fn(reps), elapsed = ticks() - start;
    if (!QueryThreadCycleTime((HANDLE)(LONG_PTR)-2, &cycles_end)) ExitProcess(91);
    U64 ns = elapsed * 1000000000ULL / (U64)frequency.QuadPart;
    text("BENCH "); text(name); text(" ops="); number(reps * N, 10);
    text(" ns="); number(ns, 10); text(" check="); number(check, 16);
    text(" cycles="); number(cycles_end - cycles_start, 10); text("\r\n");
}
void guest_entry(void) {
    QueryPerformanceFrequency(&frequency);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < 4; ++j) input[i][j] = .01f + (float)((i * 17 + j * 13) % 97) / 100.0f;
    for (unsigned i = 0; i < 4096; ++i) texture[i] = (float)((i * 37) % 251) / 256.0f;
    for (unsigned i = 0; i < 2048 * 2048; ++i) large_texture[i] = (float)((i * 37) % 251) / 256.0f;
    run("minmax", minmax, 300000);
    run("compare_select", compare_select, 300000);
    run("scalar_inline", scalar_inline, 300000);
    run("scalar_calls", scalar_calls, 300000);
    run("texture_gather", texture_gather, 300000);
    run("texture_large", texture_large, 300000);
    run("noise_lerp", noise_lerp, 30000);
    run("mask_reduce", mask_reduce, 300000);
    run("branch_live", branch_live, 300000);
    ExitProcess(0);
}
