// SIMD micro-benchmarks (owner: neon/fp; built by build.ps1 as bench_simd).
// The same kernels written with NEON intrinsics (ARM64, run under
// transmute-fx) and SSE4.1/AVX2 intrinsics (x64, run natively), matched work
// per iteration. No CRT; output as in bench.c:
//   BENCH <name> ops=<n> ns=<total ns> check=<hex>
// Kernels: ray/box slab test (FMIN/FMAX + movemask), float4 dot/cross/
// normalize, int16 saturating image filter, float<->int conversions, byte
// table lookup/shuffles. scale=<n> divides the iteration counts.
#include <windows.h>
#if defined(_M_ARM64)
#include <arm_neon.h>
#else
#include <immintrin.h>
#endif

int _fltused = 0;
typedef unsigned long long u64;
typedef unsigned int u32;

static void out(const char* s) {
    DWORD n = 0, len = 0;
    while (s[len]) ++len;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, len, &n, 0);
}
static void out_u64(u64 v) {
    char buf[32];
    int i = 31;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(buf + i);
}
static void out_hex(u64 v) {
    char buf[32];
    int i = 31;
    buf[i] = 0;
    do { buf[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    out(buf + i);
}
static LARGE_INTEGER freq;
static u64 now_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (u64)t.QuadPart; }
static u64 ticks_to_ns(u64 t) {
    u64 f = (u64)freq.QuadPart;
    return (t / f) * 1000000000ULL + (t % f) * 1000000000ULL / f;
}
static void report(const char* name, u64 ops, u64 ticks, u64 check) {
    out("BENCH "); out(name); out(" ops="); out_u64(ops); out(" ns="); out_u64(ticks_to_ns(ticks));
    out(" check="); out_hex(check); out("\r\n");
}
static u64 g_scale = 1;
static void parse_args(void) {
    const char* p = GetCommandLineA();
    for (; *p; ++p)
        if (p[0] == 's' && p[1] == 'c' && p[2] == 'a' && p[3] == 'l' && p[4] == 'e' && p[5] == '=') {
            u64 v = 0;
            for (p += 6; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (u64)(*p - '0');
            if (v) g_scale = v;
            break;
        }
}
static u64 scaled(u64 n) { u64 v = n / g_scale; return v ? v : 1; }

static u32 lcg_state = 12345;
static u32 lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state; }
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(lcg() >> 8) * (1.0f / 16777216.0f); }

static u64 fold(const void* p, unsigned bytes) {
    const u32* w = (const u32*)p;
    u64 h = 1469598103934665603ULL;
    for (unsigned i = 0; i < bytes / 4; i++) h = (h ^ w[i]) * 1099511628211ULL;
    return h;
}

// ---- 1. ray / 4-box slab test (BVH4 node, SoA) -------------------------------------
#define NODES 256
typedef struct { float lo[3][4], hi[3][4]; } Node;
static Node nodes[NODES];

__declspec(noinline) static u64 slab(u64 reps) {
    u64 hits = 0;
    const float org[3] = {0.1f, 0.2f, 0.3f}, inv[3] = {1.7f, -2.3f, 0.9f};
#if defined(_M_ARM64)
    const uint32x4_t bits = {1, 2, 4, 8};
    const float32x4_t ox = vdupq_n_f32(org[0]), oy = vdupq_n_f32(org[1]), oz = vdupq_n_f32(org[2]);
    const float32x4_t ix = vdupq_n_f32(inv[0]), iy = vdupq_n_f32(inv[1]), iz = vdupq_n_f32(inv[2]);
    float32x4_t tfar0 = vdupq_n_f32(100.0f), tnear0 = vdupq_n_f32(0.0f);
    for (u64 r = 0; r < reps; r++)
        for (int i = 0; i < NODES; i++) {
            const Node* n = &nodes[i];
            float32x4_t ax = vmulq_f32(vsubq_f32(vld1q_f32(n->lo[0]), ox), ix);
            float32x4_t bx = vmulq_f32(vsubq_f32(vld1q_f32(n->hi[0]), ox), ix);
            float32x4_t ay = vmulq_f32(vsubq_f32(vld1q_f32(n->lo[1]), oy), iy);
            float32x4_t by = vmulq_f32(vsubq_f32(vld1q_f32(n->hi[1]), oy), iy);
            float32x4_t az = vmulq_f32(vsubq_f32(vld1q_f32(n->lo[2]), oz), iz);
            float32x4_t bz = vmulq_f32(vsubq_f32(vld1q_f32(n->hi[2]), oz), iz);
            float32x4_t tn = vmaxq_f32(vmaxq_f32(vminq_f32(ax, bx), vminq_f32(ay, by)), vmaxq_f32(vminq_f32(az, bz), tnear0));
            float32x4_t tf = vminq_f32(vminq_f32(vmaxq_f32(ax, bx), vmaxq_f32(ay, by)), vminq_f32(vmaxq_f32(az, bz), tfar0));
            uint32x4_t m = vandq_u32(vcleq_f32(tn, tf), bits);
            hits += vaddvq_u32(m);
        }
#else
    const __m128 ox = _mm_set1_ps(org[0]), oy = _mm_set1_ps(org[1]), oz = _mm_set1_ps(org[2]);
    const __m128 ix = _mm_set1_ps(inv[0]), iy = _mm_set1_ps(inv[1]), iz = _mm_set1_ps(inv[2]);
    const __m128 tfar0 = _mm_set1_ps(100.0f), tnear0 = _mm_setzero_ps();
    for (u64 r = 0; r < reps; r++)
        for (int i = 0; i < NODES; i++) {
            const Node* n = &nodes[i];
            __m128 ax = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n->lo[0]), ox), ix);
            __m128 bx = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n->hi[0]), ox), ix);
            __m128 ay = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n->lo[1]), oy), iy);
            __m128 by = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n->hi[1]), oy), iy);
            __m128 az = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n->lo[2]), oz), iz);
            __m128 bz = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n->hi[2]), oz), iz);
            __m128 tn = _mm_max_ps(_mm_max_ps(_mm_min_ps(ax, bx), _mm_min_ps(ay, by)), _mm_max_ps(_mm_min_ps(az, bz), tnear0));
            __m128 tf = _mm_min_ps(_mm_min_ps(_mm_max_ps(ax, bx), _mm_max_ps(ay, by)), _mm_min_ps(_mm_max_ps(az, bz), tfar0));
            hits += (u64)_mm_movemask_ps(_mm_cmple_ps(tn, tf));
        }
#endif
    return hits;
}

// ---- 2. float4 dot / cross / normalize ------------------------------------------------
#define VECS 512
static float va[VECS][4], vb[VECS][4], vout[VECS][4];

__declspec(noinline) static u64 vec3(u64 reps) {
    for (u64 r = 0; r < reps; r++)
        for (int i = 0; i < VECS; i++) {
#if defined(_M_ARM64)
            float32x4_t a = vld1q_f32(va[i]), b = vld1q_f32(vb[i]);
            // cross(a, b) = a.yzx * b.zxy - a.zxy * b.yzx (w lane: 0)
            float32x4_t a_yzx = vextq_f32(vextq_f32(a, a, 3), a, 2), b_yzx = vextq_f32(vextq_f32(b, b, 3), b, 2);
            float32x4_t c = vsubq_f32(vmulq_f32(a, b_yzx), vmulq_f32(a_yzx, b));
            c = vextq_f32(vextq_f32(c, c, 3), c, 2);
            float d = vaddvq_f32(vmulq_f32(c, c)) + vaddvq_f32(vmulq_f32(a, b));
            float32x4_t s = vdivq_f32(vdupq_n_f32(1.0f), vsqrtq_f32(vdupq_n_f32(d + 1.0f)));
            vst1q_f32(vout[i], vmulq_f32(c, s));
#else
            __m128 a = _mm_loadu_ps(va[i]), b = _mm_loadu_ps(vb[i]);
            __m128 a_yzx = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1)), b_yzx = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
            __m128 c = _mm_sub_ps(_mm_mul_ps(a, b_yzx), _mm_mul_ps(a_yzx, b));
            c = _mm_shuffle_ps(c, c, _MM_SHUFFLE(3, 0, 2, 1));
            __m128 cc = _mm_mul_ps(c, c), ab = _mm_mul_ps(a, b);
            cc = _mm_hadd_ps(cc, cc); cc = _mm_hadd_ps(cc, cc);
            ab = _mm_hadd_ps(ab, ab); ab = _mm_hadd_ps(ab, ab);
            float d = _mm_cvtss_f32(cc) + _mm_cvtss_f32(ab);
            __m128 s = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(_mm_set1_ps(d + 1.0f)));
            _mm_storeu_ps(vout[i], _mm_mul_ps(c, s));
#endif
        }
    return fold(vout, sizeof(vout));
}

// ---- 3. int16 saturating 3-tap image filter -> u8 -------------------------------------
#define PIX 4096
static short img[PIX + 16];
static unsigned char dst8[PIX];

__declspec(noinline) static u64 satfilter(u64 reps) {
    for (u64 r = 0; r < reps; r++)
        for (int i = 0; i < PIX; i += 8) {
#if defined(_M_ARM64)
            int16x8_t a = vld1q_s16(img + i), b = vld1q_s16(img + i + 1), c = vld1q_s16(img + i + 2);
            int16x8_t s = vqaddq_s16(vqaddq_s16(vmulq_n_s16(a, 3), vmulq_n_s16(b, 10)), vmulq_n_s16(c, 3));
            s = vqsubq_s16(s, vdupq_n_s16(200));
            s = vshrq_n_s16(s, 4);
            vst1_u8(dst8 + i, vqmovun_s16(s));
#else
            __m128i a = _mm_loadu_si128((const __m128i*)(img + i)), b = _mm_loadu_si128((const __m128i*)(img + i + 1));
            __m128i c = _mm_loadu_si128((const __m128i*)(img + i + 2));
            __m128i s = _mm_adds_epi16(_mm_adds_epi16(_mm_mullo_epi16(a, _mm_set1_epi16(3)), _mm_mullo_epi16(b, _mm_set1_epi16(10))),
                                       _mm_mullo_epi16(c, _mm_set1_epi16(3)));
            s = _mm_subs_epi16(s, _mm_set1_epi16(200));
            s = _mm_srai_epi16(s, 4);
            _mm_storel_epi64((__m128i*)(dst8 + i), _mm_packus_epi16(s, s));
#endif
        }
    return fold(dst8, sizeof(dst8));
}

// ---- 4. float <-> int conversions -----------------------------------------------------
#define CNV 2048
static float fin[CNV];
static int iout[CNV];
static float fout[CNV];

__declspec(noinline) static u64 convert(u64 reps) {
    for (u64 r = 0; r < reps; r++)
        for (int i = 0; i < CNV; i += 4) {
#if defined(_M_ARM64)
            float32x4_t x = vld1q_f32(fin + i);
            int32x4_t t = vcvtq_s32_f32(x);                          // FCVTZS (saturating)
            int32x4_t n = vcvtnq_s32_f32(x);                         // FCVTNS
            vst1q_s32(iout + i, vaddq_s32(t, n));
            uint32x4_t u = vreinterpretq_u32_s32(vaddq_s32(t, vdupq_n_s32(0x40000000)));
            vst1q_f32(fout + i, vaddq_f32(vcvtq_f32_u32(u), vcvtq_f32_s32(n)));   // UCVTF + SCVTF
#else
            __m128 x = _mm_loadu_ps(fin + i);
            __m128i t = _mm_cvttps_epi32(x);
            __m128i n = _mm_cvtps_epi32(x);
            _mm_storeu_si128((__m128i*)(iout + i), _mm_add_epi32(t, n));
            __m128i u = _mm_add_epi32(t, _mm_set1_epi32(0x40000000));
            // u32 -> float (no native instruction below AVX-512)
            __m128 hi = _mm_cvtepi32_ps(_mm_srli_epi32(u, 16));
            __m128 lo = _mm_cvtepi32_ps(_mm_and_si128(u, _mm_set1_epi32(0xffff)));
            __m128 uf = _mm_fmadd_ps(hi, _mm_set1_ps(65536.0f), lo);
            _mm_storeu_ps(fout + i, _mm_add_ps(uf, _mm_cvtepi32_ps(n)));
#endif
        }
    return fold(iout, sizeof(iout)) ^ fold(fout, sizeof(fout));
}

// ---- 5. byte table lookup and shuffles -------------------------------------------------
#define TBYTES 8192
static unsigned char tin[TBYTES + 32], tout[TBYTES];
static unsigned char lut[16];

__declspec(noinline) static u64 table(u64 reps) {
    for (u64 r = 0; r < reps; r++)
        for (int i = 0; i < TBYTES; i += 16) {
#if defined(_M_ARM64)
            uint8x16_t t = vld1q_u8(lut);
            uint8x16_t a = vld1q_u8(tin + i), b = vld1q_u8(tin + i + 16);
            uint8x16_t ia = vandq_u8(a, vdupq_n_u8(15));
            uint8x16_t x = vqtbl1q_u8(t, ia);                         // TBL
            uint8x16_t y = vextq_u8(a, b, 5);                         // EXT
            uint8x16_t z = vzip1q_u8(x, y);                           // ZIP1
            uint8x16_t w = vuzp2q_u8(x, y);                           // UZP2
            vst1q_u8(tout + i, veorq_u8(vrev64q_u8(z), w));           // REV64
#else
            __m128i t = _mm_loadu_si128((const __m128i*)lut);
            __m128i a = _mm_loadu_si128((const __m128i*)(tin + i)), b = _mm_loadu_si128((const __m128i*)(tin + i + 16));
            __m128i ia = _mm_and_si128(a, _mm_set1_epi8(15));
            __m128i x = _mm_shuffle_epi8(t, ia);
            __m128i y = _mm_alignr_epi8(b, a, 5);
            __m128i z = _mm_unpacklo_epi8(x, y);
            const __m128i odd = _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1);
            __m128i w = _mm_unpacklo_epi64(_mm_shuffle_epi8(x, odd), _mm_shuffle_epi8(y, odd));
            const __m128i rev = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
            _mm_storeu_si128((__m128i*)(tout + i), _mm_xor_si128(_mm_shuffle_epi8(z, rev), w));
#endif
        }
    return fold(tout, sizeof(tout));
}

static void init(void) {
    for (int i = 0; i < NODES; i++)
        for (int a = 0; a < 3; a++)
            for (int l = 0; l < 4; l++) {
                float c = frand(-2.0f, 2.0f), e = frand(0.05f, 1.0f);
                nodes[i].lo[a][l] = c - e;
                nodes[i].hi[a][l] = c + e;
            }
    for (int i = 0; i < VECS; i++)
        for (int l = 0; l < 4; l++) {
            va[i][l] = l == 3 ? 0.0f : frand(-3.0f, 3.0f);
            vb[i][l] = l == 3 ? 0.0f : frand(-3.0f, 3.0f);
        }
    for (int i = 0; i < PIX + 16; i++) img[i] = (short)(lcg() % 1200);
    for (int i = 0; i < CNV; i++) fin[i] = frand(-3.0e6f, 3.0e6f);
    for (int i = 0; i < TBYTES + 32; i++) tin[i] = (unsigned char)lcg();
    for (int i = 0; i < 16; i++) lut[i] = (unsigned char)(lcg() >> 3);
}

typedef u64 (*Kernel)(u64);
static void run(const char* name, Kernel k, u64 reps, u64 ops_per_rep) {
    volatile u64 sink = k(reps / 20 + 1);   // warm up (translation, caches)
    (void)sink;
    u64 t0 = now_ticks();
    u64 check = k(reps);
    report(name, reps * ops_per_rep, now_ticks() - t0, check);
}

void guest_entry(void) {
    QueryPerformanceFrequency(&freq);
    parse_args();
    init();
    run("simd_slab_bvh4", slab, scaled(200000), NODES);
    run("simd_vec3_dot_cross_norm", vec3, scaled(100000), VECS);
    run("simd_sat16_filter", satfilter, scaled(100000), PIX / 8);
    run("simd_f2i_i2f", convert, scaled(100000), CNV / 4);
    run("simd_tbl_shuffle", table, scaled(100000), TBYTES / 16);
    ExitProcess(0);
}
