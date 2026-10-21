// NEON intrinsics (arm_neon.h) on ARM64. The x64 build implements the same
// intrinsics in portable C with ARM semantics (saturation, FMAX/FMIN zero
// and NaN rules, FCVTZS saturation, pairwise reduction order), so both builds
// print identical results.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_M_ARM64)
#include <arm_neon.h>
#else
typedef struct { float v[4]; } float32x4_t;
typedef struct { double v[2]; } float64x2_t;
typedef struct { int32_t v[4]; } int32x4_t;
typedef struct { uint32_t v[4]; } uint32x4_t;
typedef struct { int16_t v[8]; } int16x8_t;
typedef struct { int16_t v[4]; } int16x4_t;
typedef struct { uint16_t v[8]; } uint16x8_t;
typedef struct { uint8_t v[16]; } uint8x16_t;

#define LANES(n) for (int i = 0; i < (n); i++)
static float32x4_t vld1q_f32(const float* p) { float32x4_t r; memcpy(r.v, p, 16); return r; }
static void vst1q_f32(float* p, float32x4_t a) { memcpy(p, a.v, 16); }
static float32x4_t vdupq_n_f32(float x) { float32x4_t r; LANES(4) r.v[i] = x; return r; }
static float32x4_t vaddq_f32(float32x4_t a, float32x4_t b) { LANES(4) a.v[i] += b.v[i]; return a; }
static float32x4_t vsubq_f32(float32x4_t a, float32x4_t b) { LANES(4) a.v[i] -= b.v[i]; return a; }
static float32x4_t vmulq_f32(float32x4_t a, float32x4_t b) { LANES(4) a.v[i] *= b.v[i]; return a; }
static float32x4_t vdivq_f32(float32x4_t a, float32x4_t b) { LANES(4) a.v[i] /= b.v[i]; return a; }
static float32x4_t vfmaq_f32(float32x4_t a, float32x4_t b, float32x4_t c) {
    LANES(4) a.v[i] = fmaf(b.v[i], c.v[i], a.v[i]);
    return a;
}
static float32x4_t vsqrtq_f32(float32x4_t a) { LANES(4) a.v[i] = sqrtf(a.v[i]); return a; }
static float32x4_t vabsq_f32(float32x4_t a) { LANES(4) a.v[i] = fabsf(a.v[i]); return a; }
static float32x4_t vnegq_f32(float32x4_t a) { LANES(4) a.v[i] = -a.v[i]; return a; }
// FMAX/FMIN: NaN if either operand is NaN; max(-0,+0)=+0, min(-0,+0)=-0.
static float arm_fmax(float a, float b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a == 0 && b == 0) return signbit(a) && signbit(b) ? -0.0f : 0.0f;
    return a > b ? a : b;
}
static float arm_fmin(float a, float b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a == 0 && b == 0) return signbit(a) || signbit(b) ? -0.0f : 0.0f;
    return a < b ? a : b;
}
static float32x4_t vmaxq_f32(float32x4_t a, float32x4_t b) { LANES(4) a.v[i] = arm_fmax(a.v[i], b.v[i]); return a; }
static float32x4_t vminq_f32(float32x4_t a, float32x4_t b) { LANES(4) a.v[i] = arm_fmin(a.v[i], b.v[i]); return a; }
static float vaddvq_f32(float32x4_t a) { return (a.v[0] + a.v[1]) + (a.v[2] + a.v[3]); }
static float vmaxvq_f32(float32x4_t a) { return arm_fmax(arm_fmax(a.v[0], a.v[1]), arm_fmax(a.v[2], a.v[3])); }
static int32_t arm_fcvtzs(float x) {
    if (isnan(x)) return 0;
    if (x >= 2147483648.0f) return INT32_MAX;
    if (x < -2147483648.0f) return INT32_MIN;
    return (int32_t)x;
}
static int32x4_t vcvtq_s32_f32(float32x4_t a) { int32x4_t r; LANES(4) r.v[i] = arm_fcvtzs(a.v[i]); return r; }
static float32x4_t vcvtq_f32_s32(int32x4_t a) { float32x4_t r; LANES(4) r.v[i] = (float)a.v[i]; return r; }
static float32x4_t vrndnq_f32(float32x4_t a) { LANES(4) a.v[i] = nearbyintf(a.v[i]); return a; }
static uint32x4_t vcgtq_f32(float32x4_t a, float32x4_t b) {
    uint32x4_t r;
    LANES(4) r.v[i] = a.v[i] > b.v[i] ? 0xffffffffu : 0;
    return r;
}
static float32x4_t vbslq_f32(uint32x4_t m, float32x4_t a, float32x4_t b) {
    float32x4_t r;
    LANES(4) {
        uint32_t x, y;
        memcpy(&x, &a.v[i], 4);
        memcpy(&y, &b.v[i], 4);
        x = (m.v[i] & x) | (~m.v[i] & y);
        memcpy(&r.v[i], &x, 4);
    }
    return r;
}
static uint32x4_t vreinterpretq_u32_f32(float32x4_t a) { uint32x4_t r; memcpy(r.v, a.v, 16); return r; }
static void vst1q_u32(uint32_t* p, uint32x4_t a) { memcpy(p, a.v, 16); }

static float64x2_t vld1q_f64(const double* p) { float64x2_t r; memcpy(r.v, p, 16); return r; }
static void vst1q_f64(double* p, float64x2_t a) { memcpy(p, a.v, 16); }
static float64x2_t vfmaq_f64(float64x2_t a, float64x2_t b, float64x2_t c) {
    LANES(2) a.v[i] = fma(b.v[i], c.v[i], a.v[i]);
    return a;
}
static float64x2_t vmulq_f64(float64x2_t a, float64x2_t b) { LANES(2) a.v[i] *= b.v[i]; return a; }
static double vaddvq_f64(float64x2_t a) { return a.v[0] + a.v[1]; }

static int32x4_t vld1q_s32(const int32_t* p) { int32x4_t r; memcpy(r.v, p, 16); return r; }
static void vst1q_s32(int32_t* p, int32x4_t a) { memcpy(p, a.v, 16); }
static int32x4_t vaddq_s32(int32x4_t a, int32x4_t b) { LANES(4) a.v[i] = (int32_t)((uint32_t)a.v[i] + (uint32_t)b.v[i]); return a; }
static int32x4_t vmulq_s32(int32x4_t a, int32x4_t b) { LANES(4) a.v[i] = (int32_t)((uint32_t)a.v[i] * (uint32_t)b.v[i]); return a; }
static int32x4_t vqaddq_s32(int32x4_t a, int32x4_t b) {
    LANES(4) {
        int64_t s = (int64_t)a.v[i] + b.v[i];
        a.v[i] = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
    }
    return a;
}
static int32x4_t vabsq_s32(int32x4_t a) { LANES(4) a.v[i] = a.v[i] < 0 ? (int32_t)(0u - (uint32_t)a.v[i]) : a.v[i]; return a; }
static int32x4_t vshrq_n_s32(int32x4_t a, int n) { LANES(4) a.v[i] = a.v[i] >> n; return a; }
static int32x4_t vshlq_n_s32(int32x4_t a, int n) { LANES(4) a.v[i] = (int32_t)((uint32_t)a.v[i] << n); return a; }
static int32x4_t vmaxq_s32(int32x4_t a, int32x4_t b) { LANES(4) a.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return a; }
static int32_t vaddvq_s32(int32x4_t a) { uint32_t s = 0; LANES(4) s += (uint32_t)a.v[i]; return (int32_t)s; }
static int32_t vminvq_s32(int32x4_t a) { int32_t m = a.v[0]; LANES(4) if (a.v[i] < m) m = a.v[i]; return m; }

static int16x8_t vld1q_s16(const int16_t* p) { int16x8_t r; memcpy(r.v, p, 16); return r; }
static int16x4_t vget_low_s16(int16x8_t a) { int16x4_t r; memcpy(r.v, a.v, 8); return r; }
static int16x4_t vget_high_s16(int16x8_t a) { int16x4_t r; memcpy(r.v, a.v + 4, 8); return r; }
static int32x4_t vmull_s16(int16x4_t a, int16x4_t b) { int32x4_t r; LANES(4) r.v[i] = (int32_t)a.v[i] * b.v[i]; return r; }
static int16x8_t vqdmulhq_s16(int16x8_t a, int16x8_t b) {
    int16x8_t r;
    LANES(8) {
        int64_t p = 2 * (int64_t)a.v[i] * b.v[i];
        p >>= 16;
        r.v[i] = p > INT16_MAX ? INT16_MAX : (int16_t)p;
    }
    return r;
}
static void vst1q_s16(int16_t* p, int16x8_t a) { memcpy(p, a.v, 16); }

static uint8x16_t vld1q_u8(const uint8_t* p) { uint8x16_t r; memcpy(r.v, p, 16); return r; }
static void vst1q_u8(uint8_t* p, uint8x16_t a) { memcpy(p, a.v, 16); }
static uint8x16_t vaddq_u8(uint8x16_t a, uint8x16_t b) { LANES(16) a.v[i] = (uint8_t)(a.v[i] + b.v[i]); return a; }
static uint8x16_t vqaddq_u8(uint8x16_t a, uint8x16_t b) { LANES(16) { int s = a.v[i] + b.v[i]; a.v[i] = (uint8_t)(s > 255 ? 255 : s); } return a; }
static uint8x16_t vqsubq_u8(uint8x16_t a, uint8x16_t b) { LANES(16) a.v[i] = (uint8_t)(a.v[i] > b.v[i] ? a.v[i] - b.v[i] : 0); return a; }
static uint8x16_t vrhaddq_u8(uint8x16_t a, uint8x16_t b) { LANES(16) a.v[i] = (uint8_t)((a.v[i] + b.v[i] + 1) >> 1); return a; }
static uint8x16_t vcntq_u8(uint8x16_t a) {
    LANES(16) {
        int c = 0;
        for (int b = 0; b < 8; b++) c += (a.v[i] >> b) & 1;
        a.v[i] = (uint8_t)c;
    }
    return a;
}
static uint8x16_t vclzq_u8(uint8x16_t a) {
    LANES(16) {
        int c = 0;
        while (c < 8 && !(a.v[i] & (0x80 >> c))) c++;
        a.v[i] = (uint8_t)c;
    }
    return a;
}
static uint8x16_t vextq_u8(uint8x16_t a, uint8x16_t b, int n) {
    uint8x16_t r;
    LANES(16) r.v[i] = i + n < 16 ? a.v[i + n] : b.v[i + n - 16];
    return r;
}
static uint8x16_t vqtbl1q_u8(uint8x16_t t, uint8x16_t idx) {
    uint8x16_t r;
    LANES(16) r.v[i] = idx.v[i] < 16 ? t.v[idx.v[i]] : 0;
    return r;
}
static uint8x16_t vrev64q_u8(uint8x16_t a) {
    uint8x16_t r;
    LANES(16) r.v[i] = a.v[(i & ~7) + (7 - (i & 7))];
    return r;
}
static uint8x16_t vzip1q_u8(uint8x16_t a, uint8x16_t b) {
    uint8x16_t r;
    LANES(8) {
        r.v[2 * i] = a.v[i];
        r.v[2 * i + 1] = b.v[i];
    }
    return r;
}
static uint8x16_t vuzp1q_u8(uint8x16_t a, uint8x16_t b) {
    uint8x16_t r;
    LANES(8) {
        r.v[i] = a.v[2 * i];
        r.v[8 + i] = b.v[2 * i];
    }
    return r;
}
static uint8x16_t vceqq_u8(uint8x16_t a, uint8x16_t b) { LANES(16) a.v[i] = a.v[i] == b.v[i] ? 0xff : 0; return a; }
static uint8x16_t veorq_u8(uint8x16_t a, uint8x16_t b) { LANES(16) a.v[i] ^= b.v[i]; return a; }
static uint8x16_t vmvnq_u8(uint8x16_t a) { LANES(16) a.v[i] = (uint8_t)~a.v[i]; return a; }
static uint8_t vmaxvq_u8(uint8x16_t a) { uint8_t m = 0; LANES(16) if (a.v[i] > m) m = a.v[i]; return m; }
static uint16_t vaddlvq_u8(uint8x16_t a) { uint16_t s = 0; LANES(16) s = (uint16_t)(s + a.v[i]); return s; }
static uint16x8_t vpaddlq_u8(uint8x16_t a) { uint16x8_t r; LANES(8) r.v[i] = (uint16_t)(a.v[2 * i] + a.v[2 * i + 1]); return r; }
static void vst1q_u16(uint16_t* p, uint16x8_t a) { memcpy(p, a.v, 16); }
#endif

static void print_f32(const char* name, float32x4_t v) {
    float f[4];
    uint32_t u[4];
    vst1q_f32(f, v);
    memcpy(u, f, 16);
    printf("%-10s", name);
    for (int i = 0; i < 4; i++) {
        if (isnan(f[i])) printf(" nan");
        else printf(" %.9g[%08x]", f[i], u[i]);
    }
    printf("\n");
}

static void print_s32(const char* name, int32x4_t v) {
    int32_t x[4];
    vst1q_s32(x, v);
    printf("%-10s %d %d %d %d\n", name, x[0], x[1], x[2], x[3]);
}

static void print_u8(const char* name, uint8x16_t v) {
    uint8_t x[16];
    vst1q_u8(x, v);
    printf("%-10s", name);
    for (int i = 0; i < 16; i++) printf(" %02x", x[i]);
    printf("\n");
}

static volatile float zero_f = 0.0f;

__declspec(noinline) static float dot_fma(const float* a, const float* b, int n) {
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f);
    for (int i = 0; i < n; i += 8) {
        acc0 = vfmaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
        acc1 = vfmaq_f32(acc1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }
    return vaddvq_f32(vaddq_f32(acc0, acc1));
}

int main(void) {
    float nanv = zero_f / zero_f;
    float infv = 1.0f / zero_f;
    float fa[4] = {1.5f, -2.25f, 0.0f, 1e30f};
    float fb[4] = {0.5f, 4.0f, -0.0f, 1e10f};
    float fc[4] = {0.1f, 0.2f, 0.3f, -1e30f};
    float32x4_t a = vld1q_f32(fa), b = vld1q_f32(fb), c = vld1q_f32(fc);
    print_f32("add", vaddq_f32(a, b));
    print_f32("sub", vsubq_f32(a, b));
    print_f32("mul", vmulq_f32(a, b));
    print_f32("div", vdivq_f32(a, vaddq_f32(b, vdupq_n_f32(1.0f))));
    print_f32("fma", vfmaq_f32(c, a, b));
    print_f32("sqrt", vsqrtq_f32(vabsq_f32(a)));
    print_f32("neg", vnegq_f32(a));
    print_f32("max", vmaxq_f32(a, b));
    print_f32("min", vminq_f32(a, b));
    float zs[4] = {-0.0f, 0.0f, -0.0f, 3.0f};
    float zt[4] = {0.0f, -0.0f, -0.0f, 0};
    zt[3] = nanv;
    float32x4_t z1 = vld1q_f32(zs), z2 = vld1q_f32(zt);
    print_f32("max0", vmaxq_f32(z1, z2));
    print_f32("min0", vminq_f32(z1, z2));
    float rn[4] = {2.5f, 3.5f, -2.5f, 0.49999997f};
    print_f32("rndn", vrndnq_f32(vld1q_f32(rn)));
    float cv[4] = {3e9f, -3e9f, -7.9f, 0};
    cv[3] = nanv;
    print_s32("cvt_s32", vcvtq_s32_f32(vld1q_f32(cv)));
    float cv2[4] = {2147483520.0f, -2147483648.0f, 123.999f, 0};
    cv2[3] = infv;
    print_s32("cvt_s32b", vcvtq_s32_f32(vld1q_f32(cv2)));
    int32_t iv[4] = {16777217, -16777219, 2147483647, -5};
    print_f32("cvt_f32", vcvtq_f32_s32(vld1q_s32(iv)));
    uint32x4_t gt = vcgtq_f32(a, b);
    uint32_t mask[4];
    vst1q_u32(mask, gt);
    printf("cgt        %08x %08x %08x %08x\n", mask[0], mask[1], mask[2], mask[3]);
    print_f32("bsl", vbslq_f32(gt, a, b));
    printf("addv       %.9g\n", vaddvq_f32(vld1q_f32(fc)));
    printf("maxv       %.9g\n", vmaxvq_f32(a));

    double da[2] = {1.0 / 3.0, -2.5}, db[2] = {3.0, 1e-300}, dc[2] = {-1.0, 1e-310};
    float64x2_t fd = vfmaq_f64(vld1q_f64(dc), vld1q_f64(da), vld1q_f64(db));
    double out[2];
    vst1q_f64(out, fd);
    printf("fma_f64    %.17g %.17g\n", out[0], out[1]);
    printf("mul_addv   %.17g\n", vaddvq_f64(vmulq_f64(vld1q_f64(da), vld1q_f64(db))));

    int32_t sa[4] = {2147483647, -2147483647 - 1, 100000, -7};
    int32_t sb[4] = {1, -1, 100000, 3};
    int32x4_t ia = vld1q_s32(sa), ib = vld1q_s32(sb);
    print_s32("add_s32", vaddq_s32(ia, ib));
    print_s32("qadd_s32", vqaddq_s32(ia, ib));
    print_s32("mul_s32", vmulq_s32(ia, ib));
    print_s32("abs_s32", vabsq_s32(ia));
    print_s32("shr_s32", vshrq_n_s32(ia, 3));
    print_s32("shl_s32", vshlq_n_s32(ib, 30));
    print_s32("max_s32", vmaxq_s32(ia, ib));
    printf("addv_s32   %d minv_s32 %d\n", vaddvq_s32(ia), vminvq_s32(ia));

    int16_t ha[8] = {-32768, 32767, 1000, -1000, 3, -3, 16384, -32768};
    int16_t hb[8] = {-32768, 2, 1000, 1000, -4, -5, 16384, 32767};
    int16x8_t h1 = vld1q_s16(ha), h2 = vld1q_s16(hb);
    print_s32("mull_lo", vmull_s16(vget_low_s16(h1), vget_low_s16(h2)));
    print_s32("mull_hi", vmull_s16(vget_high_s16(h1), vget_high_s16(h2)));
    int16_t q[8];
    vst1q_s16(q, vqdmulhq_s16(h1, h2));
    printf("qdmulh    ");
    for (int i = 0; i < 8; i++) printf(" %d", q[i]);
    printf("\n");

    uint8_t ba[16], bb[16], idx[16];
    for (int i = 0; i < 16; i++) {
        ba[i] = (uint8_t)(i * 37 + 200);
        bb[i] = (uint8_t)(i * 11);
        idx[i] = (uint8_t)((i * 5) ^ (i & 1 ? 0x10 : 0));
    }
    uint8x16_t u1 = vld1q_u8(ba), u2 = vld1q_u8(bb), ui = vld1q_u8(idx);
    print_u8("add_u8", vaddq_u8(u1, u2));
    print_u8("qadd_u8", vqaddq_u8(u1, u2));
    print_u8("qsub_u8", vqsubq_u8(u2, u1));
    print_u8("rhadd_u8", vrhaddq_u8(u1, u2));
    print_u8("cnt", vcntq_u8(u1));
    print_u8("clz", vclzq_u8(u2));
    print_u8("ext3", vextq_u8(u1, u2, 3));
    print_u8("ext13", vextq_u8(u1, u2, 13));
    print_u8("tbl", vqtbl1q_u8(u1, ui));
    print_u8("rev64", vrev64q_u8(u1));
    print_u8("zip1", vzip1q_u8(u1, u2));
    print_u8("uzp1", vuzp1q_u8(u1, u2));
    print_u8("ceq", vceqq_u8(vaddq_u8(u2, u2), vrhaddq_u8(u2, u2)));
    print_u8("eor_mvn", vmvnq_u8(veorq_u8(u1, u2)));
    uint16_t pl[8];
    vst1q_u16(pl, vpaddlq_u8(u1));
    printf("paddl     ");
    for (int i = 0; i < 8; i++) printf(" %u", pl[i]);
    printf("\nmaxv_u8    %u addlv_u8 %u\n", vmaxvq_u8(u1), vaddlvq_u8(u1));

    static float va[1024], vb[1024];
    for (int i = 0; i < 1024; i++) {
        va[i] = (float)(i % 17) * 0.125f - 1.0f;
        vb[i] = (float)(i % 13) * 0.0625f + 0.5f;
    }
    float dot = 0;
    for (int r = 0; r < 200; r++) dot = dot_fma(va, vb, 1024);
    uint32_t dot_bits;
    memcpy(&dot_bits, &dot, 4);
    printf("dot_fma    %.9g[%08x]\n", dot, dot_bits);
    fflush(stdout);
    return 0;
}
