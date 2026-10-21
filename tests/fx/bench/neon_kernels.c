// Self-checking NEON kernels (owner: neon/fp; built by build.ps1 as bench_neonk,
// ARM64 only). Video-codec style integer SIMD (an adaptive symbol decode step,
// 8-tap subpel filters, transform butterflies, a deblocking filter, a
// directional-filter constrain step, SAD and pairwise sums, structure
// loads/stores, narrowing and widening, saturating and rounding shifts) written
// with NEON intrinsics, each compared with a scalar C model of the same
// instructions in this file (the scalar code is built without vectorization).
// Written for this project from the Arm architecture's instruction definitions;
// no codec source is used. Under transmute-fx a mismatch is a mistranslation
// (or a model error, which the line names).
// Output, per kernel:
//   BENCH <name> ops=<n> ns=<total ns> check=<hex>    (the NEON kernel timed)
//   CHECK <name> ok | CHECK <name> MISMATCH at <byte> neon=<hex> ref=<hex>
// scale=<n> divides the timed iteration counts, mult=<n> multiplies them; check-only
// skips the timing.
#include <windows.h>
#include <arm_neon.h>

int _fltused = 0;
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef short s16;
typedef unsigned char u8;
typedef signed char s8;

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
static u64 g_scale = 1, g_mult = 1;
static int g_check_only = 0;
static int g_failures = 0;
static int match(const char* p, const char* w) {
    while (*w) if (*p++ != *w++) return 0;
    return 1;
}
static void parse_args(void) {
    const char* p = GetCommandLineA();
    for (; *p; ++p) {
        if (match(p, "scale=")) {
            u64 v = 0;
            for (p += 6; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (u64)(*p - '0');
            if (v) g_scale = v;
            --p;
        } else if (match(p, "mult=")) {
            u64 v = 0;
            for (p += 5; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (u64)(*p - '0');
            if (v) g_mult = v;
            --p;
        } else if (match(p, "check-only")) {
            g_check_only = 1;
        }
    }
}

static u32 lcg_state = 12345;
static u32 lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state; }

static u64 fold(const void* p, unsigned bytes) {
    const u8* b = (const u8*)p;
    u64 h = 1469598103934665603ULL;
    for (unsigned i = 0; i < bytes; i++) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}

// Random bytes with saturation edges mixed in (whole lanes of 0x00, 0x7f, 0x80, 0xff...).
static void fill(void* p, unsigned bytes) {
    u8* b = (u8*)p;
    for (unsigned i = 0; i < bytes; i++) b[i] = (u8)(lcg() >> 13);
    for (unsigned i = 0; i + 8 <= bytes; i += 8) {
        const u32 r = lcg() >> 16;
        if ((r & 7) != 0) continue;
        static const u8 edges[] = {0x00, 0x7f, 0x80, 0xff, 0x01, 0xfe, 0x81, 0x40};
        const u8 lo = edges[(r >> 3) & 7], hi = edges[(r >> 6) & 7];
        for (unsigned k = 0; k < 8; k++) b[i + k] = (k & 1) ? hi : lo;
        if (r & 0x200) for (unsigned k = 0; k < 8; k++) b[i + k] = (k == 7 || k == 3) ? hi : 0xff;
    }
}

// ---- scalar models --------------------------------------------------------------------
static s16 sat16(s64 x) { return (s16)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
static u16 satu16(s64 x) { return (u16)(x > 65535 ? 65535 : x < 0 ? 0 : x); }
static s8 sat8(s64 x) { return (s8)(x > 127 ? 127 : x < -128 ? -128 : x); }
static u8 satu8(s64 x) { return (u8)(x > 255 ? 255 : x < 0 ? 0 : x); }
static s32 sat32(s64 x) { return (s32)(x > 2147483647LL ? 2147483647LL : x < -2147483648LL ? -2147483648LL : x); }
static u32 satu32(s64 x) { return (u32)(x > 4294967295LL ? 4294967295LL : x < 0 ? 0 : x); }
static s16 m_sqdmulh16(s16 a, s16 b) { return sat16(((s64)2 * a * b) >> 16); }
static s16 m_sqrdmulh16(s16 a, s16 b) { return sat16(((s64)2 * a * b + (1 << 15)) >> 16); }
static s32 m_sqrdmulh32(s32 a, s32 b) {
    // 2ab + 2^31 can exceed 64 bits only for a = b = INT_MIN (saturates).
    if (a == (s32)0x80000000 && b == (s32)0x80000000) return 0x7fffffff;
    return (s32)(((s64)2 * a * b + (1LL << 31)) >> 32);
}
// Register shifts: signed low byte of the count; negative shifts right.
static s16 m_sshl16(s16 x, s16 c) {
    const s8 n = (s8)c;
    if (n >= 16) return 0;
    if (n >= 0) return (s16)((u16)x << n);
    if (n <= -16) return (s16)(x < 0 ? -1 : 0);
    return (s16)(x >> -n);
}
static u16 m_urshl16(u16 x, s16 c) {
    const s8 n = (s8)c;
    if (n >= 16) return 0;
    if (n >= 0) return (u16)(x << n);
    if (n < -16) return 0;
    return (u16)(((u32)x + (1u << (-n - 1))) >> -n);
}
static s16 m_sqshl16(s16 x, s16 c) {
    const s8 n = (s8)c;
    if (n >= 0) {
        if (x == 0) return 0;
        if (n >= 16) return x < 0 ? -32768 : 32767;
        return sat16((s64)x << n);
    }
    if (n <= -16) return (s16)(x < 0 ? -1 : 0);
    return (s16)(x >> -n);
}
static s32 m_sqrshl32(s32 x, s32 c) {
    const s8 n = (s8)c;
    if (n >= 0) {
        if (x == 0) return 0;
        if (n >= 32) return x < 0 ? (s32)0x80000000 : 0x7fffffff;
        return sat32((s64)x << n);
    }
    if (n < -32) return 0;
    return (s32)(((s64)x + (1LL << (-n - 1))) >> -n);
}

// ---- 1. adaptive symbol decode step (8 halfword CDF) ------------------------------------
#define SYMS 512
static u16 cdf_in[SYMS][8], cdf_neon[SYMS][8], cdf_ref[SYMS][8];
static u16 sym_rng[SYMS], sym_dif[SYMS];
static u32 sym_neon[SYMS], sym_ref[SYMS];
static const u16 kMinProb[8] = {28, 24, 20, 16, 12, 8, 4, 0};
static const u16 kBits[8] = {1, 2, 4, 8, 16, 32, 64, 128};

__declspec(noinline) static void symbol_neon(void) {
    const uint16x8_t minp = vld1q_u16(kMinProb), bits = vld1q_u16(kBits);
    for (int s = 0; s < SYMS; s++) {
        uint16x8_t cdf = vld1q_u16(cdf_in[s]);
        const uint16x8_t rng = vld1q_dup_u16(&sym_rng[s]);
        const uint16x8_t r = vandq_u16(rng, vdupq_n_u16(0x7f00));
        const uint16x8_t c = vandq_u16(cdf, vdupq_n_u16(0xffc0));
        int16x8_t v = vqdmulhq_s16(vreinterpretq_s16_u16(c), vreinterpretq_s16_u16(r));
        v = vaddq_s16(v, vreinterpretq_s16_u16(vaddq_u16(c, minp)));
        const uint16x8_t dif = vld1q_dup_u16(&sym_dif[s]);
        const uint16x8_t ge = vcgeq_u16(dif, vreinterpretq_u16_s16(v));
        const u16 m = vaddvq_u16(vandq_u16(ge, bits));
        u32 sym = 0;
        while (sym < 8 && !((m >> sym) & 1)) sym++;
        sym_neon[s] = sym | ((u32)m << 8);
        // Update: towards 32768 where ge, towards -1 elsewhere, rate 4 + (s & 3).
        const uint16x8_t target = vrhaddq_u16(vdupq_n_u16(0xffff), ge);   // 0xffff or 0x8000
        int16x8_t delta = vreinterpretq_s16_u16(vsubq_u16(target, cdf));
        delta = vshlq_s16(delta, vdupq_n_s16((s16)-(4 + (s & 3))));
        cdf = vsubq_u16(cdf, ge);
        cdf = vaddq_u16(cdf, vreinterpretq_u16_s16(delta));
        vst1q_u16(cdf_neon[s], cdf);
    }
}

static void symbol_ref(void) {
    for (int s = 0; s < SYMS; s++) {
        u16 ge[8];
        u16 m = 0;
        for (int i = 0; i < 8; i++) {
            const u16 c = cdf_in[s][i] & 0xffc0, r = sym_rng[s] & 0x7f00;
            s16 v = m_sqdmulh16((s16)c, (s16)r);
            v = (s16)(u16)((u16)v + (u16)(c + kMinProb[i]));
            ge[i] = sym_dif[s] >= (u16)v ? 0xffff : 0;
            m = (u16)(m + (ge[i] & kBits[i]));
        }
        u32 sym = 0;
        while (sym < 8 && !((m >> sym) & 1)) sym++;
        sym_ref[s] = sym | ((u32)m << 8);
        for (int i = 0; i < 8; i++) {
            const u16 target = (u16)(((u32)0xffff + ge[i] + 1) >> 1);
            const s16 delta = m_sshl16((s16)(u16)(target - cdf_in[s][i]), (s16)-(4 + (s & 3)));
            cdf_ref[s][i] = (u16)(cdf_in[s][i] - ge[i] + (u16)delta);
        }
    }
}

// ---- 2. 8-tap subpel filters -------------------------------------------------------------
#define MCW 512
static u8 mc_src[8][MCW + 16];
static u8 mc_h_neon[MCW], mc_h_ref[MCW], mc_v_neon[MCW], mc_v_ref[MCW];
static const s16 kTaps[8] = {-1, 3, -10, 72, 24, -7, 2, -19};   // sums to 64
static const u8 kTapsU[8] = {1, 3, 9, 35, 28, 7, 2, 1};          // vertical: magnitude taps, signs below
static const int kTapNeg[8] = {1, 0, 1, 0, 0, 1, 0, 1};

__declspec(noinline) static void mc_neon(void) {
    const u8* row = mc_src[0];
    for (int x = 0; x < MCW; x += 8) {
        int16x8_t acc = vdupq_n_s16(0);
        for (int t = 0; t < 8; t++)
            acc = vmlaq_n_s16(acc, vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x + t))), kTaps[t]);
        vst1_u8(mc_h_neon + x, vqrshrun_n_s16(acc, 6));
    }
    for (int x = 0; x < MCW; x += 16) {
        uint16x8_t lo = vdupq_n_u16(0), hi = vdupq_n_u16(0);
        for (int t = 0; t < 8; t++) {
            const uint8x16_t s = vld1q_u8(mc_src[t] + x);
            const uint8x8_t k = vdup_n_u8(kTapsU[t]);
            if (kTapNeg[t]) {
                lo = vmlsl_u8(lo, vget_low_u8(s), k);
                hi = vmlsl_high_u8(hi, s, vdupq_n_u8(kTapsU[t]));
            } else {
                lo = vmlal_u8(lo, vget_low_u8(s), k);
                hi = vmlal_high_u8(hi, s, vdupq_n_u8(kTapsU[t]));
            }
        }
        const uint8x8_t a = vqrshrun_n_s16(vreinterpretq_s16_u16(lo), 6);
        vst1q_u8(mc_v_neon + x, vqrshrun_high_n_s16(a, vreinterpretq_s16_u16(hi), 6));
    }
}

static void mc_ref(void) {
    const u8* row = mc_src[0];
    for (int x = 0; x < MCW; x++) {
        s16 acc = 0;
        for (int t = 0; t < 8; t++) acc = (s16)(u16)((u16)acc + (u16)(row[x + t] * kTaps[t]));
        mc_h_ref[x] = satu8(((s32)acc + 32) >> 6);
    }
    for (int x = 0; x < MCW; x++) {
        u16 acc = 0;
        for (int t = 0; t < 8; t++) {
            const u16 p = (u16)(mc_src[t][x] * kTapsU[t]);
            acc = kTapNeg[t] ? (u16)(acc - p) : (u16)(acc + p);
        }
        mc_v_ref[x] = satu8(((s32)(s16)acc + 32) >> 6);
    }
}

// ---- 3. transform butterflies and transposes --------------------------------------------
#define TXN 128
static s16 tx_in[TXN][4][8], tx_neon[TXN][4][8], tx_ref[TXN][4][8];
static s32 tx32_in[TXN][4], tx32_neon[TXN][4], tx32_ref[TXN][4];

__declspec(noinline) static void tx_neon_k(void) {
    for (int b = 0; b < TXN; b++) {
        const int16x8_t a = vld1q_s16(tx_in[b][0]), bb = vld1q_s16(tx_in[b][1]);
        const int16x8_t c = vld1q_s16(tx_in[b][2]), d = vld1q_s16(tx_in[b][3]);
        const int16x8_t t0 = vqaddq_s16(a, c), t1 = vqsubq_s16(a, c);
        const int16x8_t t2 = vaddq_s16(vqrdmulhq_n_s16(bb, 30274), vqrdmulhq_n_s16(d, 12540));
        const int16x8_t t3 = vsubq_s16(vqrdmulhq_n_s16(bb, 12540), vqrdmulhq_n_s16(d, 30274));
        int16x8_t o0 = vrshrq_n_s16(vqaddq_s16(t0, t2), 4), o1 = vrshrq_n_s16(vqaddq_s16(t1, t3), 4);
        int16x8_t o2 = vrshrq_n_s16(vqsubq_s16(t1, t3), 4), o3 = vrshrq_n_s16(vqsubq_s16(t0, t2), 4);
        // 4x4 transposes of 16-bit lanes in each half (TRN1/TRN2 on 16 then 32 bits).
        const int16x8_t p0 = vtrn1q_s16(o0, o1), p1 = vtrn2q_s16(o0, o1);
        const int16x8_t p2 = vtrn1q_s16(o2, o3), p3 = vtrn2q_s16(o2, o3);
        o0 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(p0), vreinterpretq_s32_s16(p2)));
        o2 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(p0), vreinterpretq_s32_s16(p2)));
        o1 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(p1), vreinterpretq_s32_s16(p3)));
        o3 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(p1), vreinterpretq_s32_s16(p3)));
        vst1q_s16(tx_neon[b][0], vzip1q_s16(o0, o1));
        vst1q_s16(tx_neon[b][1], vzip2q_s16(o0, o1));
        vst1q_s16(tx_neon[b][2], vuzp1q_s16(o2, o3));
        vst1q_s16(tx_neon[b][3], vuzp2q_s16(o2, o3));
        // 32-bit rows: SQRDMULH by element, SRSHR, SQADD.
        const int32x4_t x = vld1q_s32(tx32_in[b]);
        const int32x4_t k = vld1q_s32(tx32_in[(b + 1) % TXN]);
        int32x4_t y = vqrdmulhq_laneq_s32(x, k, 2);
        y = vqaddq_s32(y, vrshrq_n_s32(x, 12));
        vst1q_s32(tx32_neon[b], y);
    }
}

static void trn1_16(const s16* a, const s16* b, s16* o) { for (int i = 0; i < 4; i++) { o[2 * i] = a[2 * i]; o[2 * i + 1] = b[2 * i]; } }
static void trn2_16(const s16* a, const s16* b, s16* o) { for (int i = 0; i < 4; i++) { o[2 * i] = a[2 * i + 1]; o[2 * i + 1] = b[2 * i + 1]; } }
static void trn1_32(const s16* a, const s16* b, s16* o) {
    for (int i = 0; i < 2; i++) { o[4 * i] = a[4 * i]; o[4 * i + 1] = a[4 * i + 1]; o[4 * i + 2] = b[4 * i]; o[4 * i + 3] = b[4 * i + 1]; }
}
static void trn2_32(const s16* a, const s16* b, s16* o) {
    for (int i = 0; i < 2; i++) { o[4 * i] = a[4 * i + 2]; o[4 * i + 1] = a[4 * i + 3]; o[4 * i + 2] = b[4 * i + 2]; o[4 * i + 3] = b[4 * i + 3]; }
}

static void tx_ref_k(void) {
    for (int b = 0; b < TXN; b++) {
        s16 o[4][8], p[4][8], q[4][8];
        for (int i = 0; i < 8; i++) {
            const s16 a = tx_in[b][0][i], bb = tx_in[b][1][i], c = tx_in[b][2][i], d = tx_in[b][3][i];
            const s16 t0 = sat16((s32)a + c), t1 = sat16((s32)a - c);
            const s16 t2 = (s16)(u16)((u16)m_sqrdmulh16(bb, 30274) + (u16)m_sqrdmulh16(d, 12540));
            const s16 t3 = (s16)(u16)((u16)m_sqrdmulh16(bb, 12540) - (u16)m_sqrdmulh16(d, 30274));
            o[0][i] = (s16)(((s32)sat16((s32)t0 + t2) + 8) >> 4);
            o[1][i] = (s16)(((s32)sat16((s32)t1 + t3) + 8) >> 4);
            o[2][i] = (s16)(((s32)sat16((s32)t1 - t3) + 8) >> 4);
            o[3][i] = (s16)(((s32)sat16((s32)t0 - t2) + 8) >> 4);
        }
        trn1_16(o[0], o[1], p[0]); trn2_16(o[0], o[1], p[1]);
        trn1_16(o[2], o[3], p[2]); trn2_16(o[2], o[3], p[3]);
        trn1_32(p[0], p[2], q[0]); trn2_32(p[0], p[2], q[2]);
        trn1_32(p[1], p[3], q[1]); trn2_32(p[1], p[3], q[3]);
        for (int i = 0; i < 4; i++) {
            tx_ref[b][0][2 * i] = q[0][i]; tx_ref[b][0][2 * i + 1] = q[1][i];
            tx_ref[b][1][2 * i] = q[0][i + 4]; tx_ref[b][1][2 * i + 1] = q[1][i + 4];
            tx_ref[b][2][i] = q[2][2 * i]; tx_ref[b][2][i + 4] = q[3][2 * i];
            tx_ref[b][3][i] = q[2][2 * i + 1]; tx_ref[b][3][i + 4] = q[3][2 * i + 1];
        }
        const s32 k = tx32_in[(b + 1) % TXN][2];
        for (int i = 0; i < 4; i++) {
            const s32 x = tx32_in[b][i];
            tx32_ref[b][i] = sat32((s64)m_sqrdmulh32(x, k) + (((s64)x + (1 << 11)) >> 12));
        }
    }
}

// ---- 4. deblocking filter (4 taps) ------------------------------------------------------
#define LFN 1024
static u8 lf_p1[LFN], lf_p0[LFN], lf_q0[LFN], lf_q1[LFN];
static u8 lf_neon[4][LFN], lf_ref[4][LFN];

__declspec(noinline) static void lf_neon_k(void) {
    const uint8x16_t E = vdupq_n_u8(60), I = vdupq_n_u8(18), H = vdupq_n_u8(9), sign = vdupq_n_u8(0x80);
    for (int i = 0; i < LFN; i += 16) {
        const uint8x16_t p1 = vld1q_u8(lf_p1 + i), p0 = vld1q_u8(lf_p0 + i), q0 = vld1q_u8(lf_q0 + i), q1 = vld1q_u8(lf_q1 + i);
        uint8x16_t a = vabdq_u8(p0, q0);
        a = vqaddq_u8(a, a);
        a = vqaddq_u8(a, vshrq_n_u8(vabdq_u8(p1, q1), 1));
        uint8x16_t m = vcleq_u8(a, E);
        m = vandq_u8(m, vcleq_u8(vabdq_u8(p1, p0), I));
        m = vandq_u8(m, vcgeq_u8(I, vabdq_u8(q1, q0)));
        const uint8x16_t hev = vorrq_u8(vcgtq_u8(vabdq_u8(p1, p0), H), vcgtq_u8(vabdq_u8(q1, q0), H));
        const int8x16_t ps1 = vreinterpretq_s8_u8(veorq_u8(p1, sign)), ps0 = vreinterpretq_s8_u8(veorq_u8(p0, sign));
        const int8x16_t qs0 = vreinterpretq_s8_u8(veorq_u8(q0, sign)), qs1 = vreinterpretq_s8_u8(veorq_u8(q1, sign));
        int8x16_t f = vandq_s8(vqsubq_s8(ps1, qs1), vreinterpretq_s8_u8(hev));
        const int8x16_t d = vqsubq_s8(qs0, ps0);
        f = vqaddq_s8(vqaddq_s8(vqaddq_s8(f, d), d), d);
        f = vandq_s8(f, vreinterpretq_s8_u8(m));
        const int8x16_t f1 = vshrq_n_s8(vqaddq_s8(f, vdupq_n_s8(4)), 3);
        const int8x16_t f2 = vshrq_n_s8(vqaddq_s8(f, vdupq_n_s8(3)), 3);
        const uint8x16_t nq0 = veorq_u8(vreinterpretq_u8_s8(vqsubq_s8(qs0, f1)), sign);
        const uint8x16_t np0 = veorq_u8(vreinterpretq_u8_s8(vqaddq_s8(ps0, f2)), sign);
        const int8x16_t f3 = vbicq_s8(vrshrq_n_s8(f1, 1), vreinterpretq_s8_u8(hev));
        const uint8x16_t nq1 = veorq_u8(vreinterpretq_u8_s8(vqsubq_s8(qs1, f3)), sign);
        const uint8x16_t np1 = vbslq_u8(m, vrhaddq_u8(veorq_u8(vreinterpretq_u8_s8(vqaddq_s8(ps1, f3)), sign), p1), p1);
        vst1q_u8(lf_neon[0] + i, np1);
        vst1q_u8(lf_neon[1] + i, np0);
        vst1q_u8(lf_neon[2] + i, nq0);
        vst1q_u8(lf_neon[3] + i, vbslq_u8(hev, q1, nq1));
    }
}

static u8 absdiff(u8 a, u8 b) { return a > b ? (u8)(a - b) : (u8)(b - a); }
static void lf_ref_k(void) {
    for (int i = 0; i < LFN; i++) {
        const u8 p1 = lf_p1[i], p0 = lf_p0[i], q0 = lf_q0[i], q1 = lf_q1[i];
        u8 a = absdiff(p0, q0);
        a = satu8(a + a);
        a = satu8(a + (absdiff(p1, q1) >> 1));
        const u8 m = (a <= 60 && absdiff(p1, p0) <= 18 && absdiff(q1, q0) <= 18) ? 0xff : 0;
        const u8 hev = (absdiff(p1, p0) > 9 || absdiff(q1, q0) > 9) ? 0xff : 0;
        const s8 ps1 = (s8)(p1 ^ 0x80), ps0 = (s8)(p0 ^ 0x80), qs0 = (s8)(q0 ^ 0x80), qs1 = (s8)(q1 ^ 0x80);
        s8 f = (s8)(sat8(ps1 - qs1) & (s8)hev);
        const s8 d = sat8(qs0 - ps0);
        f = sat8(sat8(sat8(f + d) + d) + d);
        f = (s8)(f & (s8)m);
        const s8 f1 = (s8)(sat8(f + 4) >> 3), f2 = (s8)(sat8(f + 3) >> 3);
        const u8 nq0 = (u8)((u8)sat8(qs0 - f1) ^ 0x80), np0 = (u8)((u8)sat8(ps0 + f2) ^ 0x80);
        const s8 f3 = (s8)(((f1 + 1) >> 1) & ~(s8)hev);
        const u8 nq1 = (u8)((u8)sat8(qs1 - f3) ^ 0x80);
        const u8 t = (u8)((u8)sat8(ps1 + f3) ^ 0x80);
        const u8 np1 = m ? (u8)(((u32)t + p1 + 1) >> 1) : p1;
        lf_ref[0][i] = np1;
        lf_ref[1][i] = np0;
        lf_ref[2][i] = nq0;
        lf_ref[3][i] = hev ? q1 : nq1;
    }
}

// ---- 5. directional filter constrain step + table lookups -----------------------------
#define CDN 512
static s16 cd_px[CDN][8], cd_c[CDN][8], cd_neon[CDN][8], cd_ref[CDN][8];
static u8 tb_tab[64], tb_idx[CDN][16], tb_neon[CDN][16 * 3], tb_ref[CDN][16 * 3];

__declspec(noinline) static void cd_neon_k(void) {
    for (int b = 0; b < CDN; b++) {
        const int16x8_t c = vld1q_s16(cd_c[b]);
        int16x8_t sum = vdupq_n_s16(0), mn = c, mx = c;
        for (int t = 0; t < 2; t++) {
            const int16x8_t px = vld1q_s16(cd_px[(b + t) % CDN]);
            const int16x8_t diff = vsubq_s16(px, c);
            const uint16x8_t ad = vreinterpretq_u16_s16(vabsq_s16(diff));
            const int shift = 1 + ((b + t) & 3);
            const uint16x8_t sh = vreinterpretq_u16_s16(vshlq_s16(vreinterpretq_s16_u16(ad), vdupq_n_s16((s16)-shift)));
            const uint16x8_t lim = vqsubq_u16(vdupq_n_u16(40), sh);
            const int16x8_t m = vreinterpretq_s16_u16(vminq_u16(ad, lim));
            const int16x8_t v = vbslq_s16(vcltzq_s16(diff), vnegq_s16(m), m);
            sum = vmlaq_n_s16(sum, v, (s16)(t ? 2 : 4));
            mn = vminq_s16(mn, px);
            mx = vmaxq_s16(mx, px);
        }
        sum = vaddq_s16(sum, vreinterpretq_s16_u16(vcltzq_s16(sum)));
        int16x8_t o = vsraq_n_s16(c, vaddq_s16(sum, vdupq_n_s16(8)), 4);
        o = vmaxq_s16(vminq_s16(o, mx), mn);
        vst1q_s16(cd_neon[b], o);
        // Table lookups: 1, 2 and 4 registers, TBX keeping out-of-range lanes.
        const uint8x16_t idx = vld1q_u8(tb_idx[b]);
        const uint8x16x4_t t4 = vld1q_u8_x4(tb_tab);
        const uint8x16x2_t t2 = {{t4.val[1], t4.val[2]}};
        vst1q_u8(tb_neon[b], vqtbl1q_u8(t4.val[0], idx));
        vst1q_u8(tb_neon[b] + 16, vqtbx2q_u8(idx, t2, vandq_u8(idx, vdupq_n_u8(0x3f))));
        vst1q_u8(tb_neon[b] + 32, vqtbl4q_u8(t4, idx));
    }
}

static void cd_ref_k(void) {
    for (int b = 0; b < CDN; b++) {
        s16 sum[8] = {0}, mn[8], mx[8];
        for (int i = 0; i < 8; i++) mn[i] = mx[i] = cd_c[b][i];
        for (int t = 0; t < 2; t++) {
            const s16* px = cd_px[(b + t) % CDN];
            const int shift = 1 + ((b + t) & 3);
            for (int i = 0; i < 8; i++) {
                const s16 diff = (s16)(u16)((u16)px[i] - (u16)cd_c[b][i]);
                const u16 ad = (u16)(diff < 0 ? (u16)(-(s32)diff) : (u16)diff);   // ABS wraps -32768
                const u16 sh = (u16)m_sshl16((s16)ad, (s16)-shift);
                const u16 lim = satu16((s32)40 - sh);
                const u16 m = ad < lim ? ad : lim;
                const s16 v = diff < 0 ? (s16)(u16)(0u - m) : (s16)m;
                sum[i] = (s16)(u16)((u16)sum[i] + (u16)(v * (t ? 2 : 4)));
                if (px[i] < mn[i]) mn[i] = px[i];
                if (px[i] > mx[i]) mx[i] = px[i];
            }
        }
        for (int i = 0; i < 8; i++) {
            const s16 s = (s16)(u16)((u16)sum[i] + (sum[i] < 0 ? 0xffffu : 0u));
            const s16 a = (s16)(u16)((u16)s + 8u);
            s16 o = (s16)(u16)((u16)cd_c[b][i] + (u16)(a >> 4));
            if (o > mx[i]) o = mx[i];
            if (o < mn[i]) o = mn[i];
            cd_ref[b][i] = o;
        }
        for (int i = 0; i < 16; i++) {
            const u8 x = tb_idx[b][i];
            tb_ref[b][i] = x < 16 ? tb_tab[x] : 0;
            const u8 y = x & 0x3f;
            tb_ref[b][16 + i] = y < 32 ? tb_tab[16 + y] : x;
            tb_ref[b][32 + i] = x < 64 ? tb_tab[x] : 0;
        }
    }
}

// ---- 6. SAD and pairwise sums -------------------------------------------------------------
#define SADN 256
static u8 sad_a[SADN][32], sad_b[SADN][32];
static u32 sad_neon[SADN][8], sad_ref[SADN][8];

__declspec(noinline) static void sad_neon_k(void) {
    for (int b = 0; b < SADN; b++) {
        const uint8x16_t a0 = vld1q_u8(sad_a[b]), a1 = vld1q_u8(sad_a[b] + 16);
        const uint8x16_t b0 = vld1q_u8(sad_b[b]), b1 = vld1q_u8(sad_b[b] + 16);
        uint16x8_t acc = vabdl_u8(vget_low_u8(a0), vget_low_u8(b0));
        acc = vabal_high_u8(acc, a0, b0);
        acc = vabal_u8(acc, vget_low_u8(a1), vget_low_u8(b1));
        acc = vabal_high_u8(acc, a1, b1);
        uint32x4_t acc32 = vpaddlq_u16(acc);
        acc32 = vpadalq_u16(acc32, vpaddlq_u8(a1));
        sad_neon[b][0] = vaddvq_u32(acc32);
        sad_neon[b][1] = vaddlvq_u8(a0);
        sad_neon[b][2] = vaddlv_u16(vget_low_u16(acc));
        sad_neon[b][3] = (u32)(s32)vaddv_s8(vreinterpret_s8_u8(vget_low_u8(b1)));
        sad_neon[b][4] = (u32)vmaxvq_u8(a0) | ((u32)vminvq_u8(b0) << 8) | ((u32)(u8)vmaxvq_s8(vreinterpretq_s8_u8(a1)) << 16) |
                         ((u32)(u8)vminvq_s8(vreinterpretq_s8_u8(b1)) << 24);
        const int16x8_t sa = vreinterpretq_s16_u8(a0), sb = vreinterpretq_s16_u8(b0);
        sad_neon[b][5] = (u32)(s32)vaddlvq_s16(sa);
        const int32x4_t p = vpaddq_s32(vpaddlq_s16(sa), vpaddlq_s16(sb));
        sad_neon[b][6] = (u32)vgetq_lane_s32(p, 1) ^ (u32)vgetq_lane_s32(p, 3);
        const uint8x16_t mx = vpmaxq_u8(a0, b0), mn = vpminq_u8(a1, b1);
        sad_neon[b][7] = vaddlvq_u8(veorq_u8(mx, mn));
    }
}

static void sad_ref_k(void) {
    for (int b = 0; b < SADN; b++) {
        const u8 *a = sad_a[b], *bb = sad_b[b];
        u16 acc[8];
        for (int i = 0; i < 8; i++)
            acc[i] = (u16)(absdiff(a[i], bb[i]) + absdiff(a[8 + i], bb[8 + i]) + absdiff(a[16 + i], bb[16 + i]) +
                           absdiff(a[24 + i], bb[24 + i]));
        u32 acc32[4];
        for (int i = 0; i < 4; i++) acc32[i] = (u32)acc[2 * i] + acc[2 * i + 1] + (u32)(u16)(a[16 + 4 * i] + a[17 + 4 * i]) +
                                               (u32)(u16)(a[18 + 4 * i] + a[19 + 4 * i]);
        sad_ref[b][0] = acc32[0] + acc32[1] + acc32[2] + acc32[3];
        u32 s = 0;
        for (int i = 0; i < 16; i++) s += a[i];
        sad_ref[b][1] = (u16)s;
        sad_ref[b][2] = (u32)acc[0] + acc[1] + acc[2] + acc[3];
        s8 t = 0;
        for (int i = 0; i < 8; i++) t = (s8)(t + (s8)bb[16 + i]);
        sad_ref[b][3] = (u32)(s32)t;
        u8 umx = 0, umn = 255;
        s8 smx = -128, smn = 127;
        for (int i = 0; i < 16; i++) {
            if (a[i] > umx) umx = a[i];
            if (bb[i] < umn) umn = bb[i];
            if ((s8)a[16 + i] > smx) smx = (s8)a[16 + i];
            if ((s8)bb[16 + i] < smn) smn = (s8)bb[16 + i];
        }
        sad_ref[b][4] = (u32)umx | ((u32)umn << 8) | ((u32)(u8)smx << 16) | ((u32)(u8)smn << 24);
        s32 sl = 0;
        s16 ha[8], hb[8];
        for (int i = 0; i < 8; i++) {
            ha[i] = (s16)(a[2 * i] | (a[2 * i + 1] << 8));
            hb[i] = (s16)(bb[2 * i] | (bb[2 * i + 1] << 8));
            sl += ha[i];
        }
        sad_ref[b][5] = (u32)sl;
        const s32 pa1 = (s32)ha[2] + ha[3] + ha[0] + ha[1], pa3 = (s32)ha[4] + ha[5] + ha[6] + ha[7];
        const s32 pb1 = (s32)hb[2] + hb[3] + hb[0] + hb[1], pb3 = (s32)hb[4] + hb[5] + hb[6] + hb[7];
        (void)pa1; (void)pb1;
        // vpaddq_s32(x, y) = {x0+x1, x2+x3, y0+y1, y2+y3}; x = pairwise s16 sums of a, y of b.
        const s32 x2 = (s32)ha[4] + ha[5], x3 = (s32)ha[6] + ha[7], y2 = (s32)hb[4] + hb[5], y3 = (s32)hb[6] + hb[7];
        (void)pa3; (void)pb3;
        sad_ref[b][6] = (u32)(x2 + x3) ^ (u32)(y2 + y3);
        u8 mx[16], mn[16];
        for (int i = 0; i < 8; i++) {
            mx[i] = a[2 * i] > a[2 * i + 1] ? a[2 * i] : a[2 * i + 1];
            mx[8 + i] = bb[2 * i] > bb[2 * i + 1] ? bb[2 * i] : bb[2 * i + 1];
            mn[i] = a[16 + 2 * i] < a[17 + 2 * i] ? a[16 + 2 * i] : a[17 + 2 * i];
            mn[8 + i] = bb[16 + 2 * i] < bb[17 + 2 * i] ? bb[16 + 2 * i] : bb[17 + 2 * i];
        }
        u32 e = 0;
        for (int i = 0; i < 16; i++) e += (u8)(mx[i] ^ mn[i]);
        sad_ref[b][7] = (u16)e;
    }
}

// ---- 7. structure loads and stores ----------------------------------------------------------
#define STN 64
static u8 st_in[STN][256];
static u8 st_neon[STN][512], st_ref[STN][512];

__declspec(noinline) static void st_neon_k(void) {
    for (int b = 0; b < STN; b++) {
        const u8* in = st_in[b];
        u8* o = st_neon[b];
        const uint8x16x2_t l2 = vld2q_u8(in);
        const uint16x8x3_t l3 = vld3q_u16((const u16*)(in + 32));
        const uint8x16x4_t l4 = vld4q_u8(in + 80);
        const uint16x4x4_t l4h = vld4_u16((const u16*)(in + 144));
        vst1q_u8(o, veorq_u8(l2.val[0], l2.val[1]));
        vst1q_u16((u16*)(o + 16), vaddq_u16(vaddq_u16(l3.val[0], vshlq_n_u16(l3.val[1], 1)), vshlq_n_u16(l3.val[2], 2)));
        vst4q_u8(o + 32, l4);   // round trip
        vst1q_u8(o + 96, vsubq_u8(l4.val[3], l4.val[1]));
        vst2q_u16((u16*)(o + 112), ((uint16x8x2_t){{l3.val[2], l3.val[0]}}));
        vst3q_u8(o + 144, ((uint8x16x3_t){{l4.val[2], l2.val[1], l4.val[0]}}));
        vst4_u16((u16*)(o + 192), l4h);
        uint16x8_t lane = vld1q_dup_u16((const u16*)(in + 200));
        lane = vld1q_lane_u16((const u16*)(in + 202), lane, 5);
        vst1q_u16((u16*)(o + 224), lane);
        uint32x4_t l32 = vld1q_dup_u32((const u32*)(in + 204));
        l32 = vld1q_lane_u32((const u32*)(in + 208), l32, 2);
        vst1q_u32((u32*)(o + 240), l32);
        uint16x8x2_t lp = vld2q_dup_u16((const u16*)(in + 212));
        lp = vld2q_lane_u16((const u16*)(in + 216), lp, 3);
        vst1q_u16((u16*)(o + 256), lp.val[0]);
        vst1q_u16((u16*)(o + 272), lp.val[1]);
        const uint8x8x4_t r4 = vld4_dup_u8(in + 220);
        vst1_u8(o + 288, r4.val[0]); vst1_u8(o + 296, r4.val[1]); vst1_u8(o + 304, r4.val[2]); vst1_u8(o + 312, r4.val[3]);
        const uint8x16x3_t x3 = vld1q_u8_x3(in + 160);
        vst1q_u16_x2((u16*)(o + 320), ((uint16x8x2_t){{vreinterpretq_u16_u8(x3.val[2]), vreinterpretq_u16_u8(x3.val[0])}}));
        vst1q_lane_u32((u32*)(o + 352), vreinterpretq_u32_u8(x3.val[1]), 3);
        vst1q_lane_u8(o + 357, x3.val[1], 9);
        vst3q_lane_u16((u16*)(o + 360), ((uint16x8x3_t){{l3.val[1], l3.val[2], l3.val[0]}}), 6);
        const uint32x4x4_t w4 = {{vreinterpretq_u32_u8(l4.val[0]), vreinterpretq_u32_u8(l4.val[1]),
                                  vreinterpretq_u32_u8(l4.val[2]), vreinterpretq_u32_u8(l4.val[3])}};
        vst4q_lane_u32((u32*)(o + 368), w4, 1);
    }
}

static u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static void wr16(u8* p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void st_ref_k(void) {
    for (int b = 0; b < STN; b++) {
        const u8* in = st_in[b];
        u8* o = st_ref[b];
        for (int i = 0; i < 16; i++) o[i] = (u8)(in[2 * i] ^ in[2 * i + 1]);
        for (int i = 0; i < 8; i++) {
            const u16 e0 = rd16(in + 32 + 6 * i), e1 = rd16(in + 34 + 6 * i), e2 = rd16(in + 36 + 6 * i);
            wr16(o + 16 + 2 * i, (u16)(e0 + (u16)(e1 << 1) + (u16)(e2 << 2)));
            wr16(o + 112 + 4 * i, e2);
            wr16(o + 114 + 4 * i, e0);
        }
        for (int i = 0; i < 64; i++) o[32 + i] = in[80 + i];
        for (int i = 0; i < 16; i++) {
            o[96 + i] = (u8)(in[80 + 4 * i + 3] - in[80 + 4 * i + 1]);
            o[144 + 3 * i] = in[80 + 4 * i + 2];
            o[145 + 3 * i] = in[2 * i + 1];
            o[146 + 3 * i] = in[80 + 4 * i];
        }
        for (int i = 0; i < 32; i++) o[192 + i] = in[144 + i];
        for (int i = 0; i < 8; i++) wr16(o + 224 + 2 * i, i == 5 ? rd16(in + 202) : rd16(in + 200));
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) o[240 + 4 * i + k] = i == 2 ? in[208 + k] : in[204 + k];
        for (int i = 0; i < 8; i++) {
            wr16(o + 256 + 2 * i, i == 3 ? rd16(in + 216) : rd16(in + 212));
            wr16(o + 272 + 2 * i, i == 3 ? rd16(in + 218) : rd16(in + 214));
        }
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 8; i++) o[288 + 8 * k + i] = in[220 + k];
        for (int i = 0; i < 16; i++) { o[320 + i] = in[192 + i]; o[336 + i] = in[160 + i]; }
        for (int k = 0; k < 4; k++) o[352 + k] = in[176 + 12 + k];
        o[357] = in[176 + 9];
        // l3.val[j] lane 6 = halfword j of structure 6 (in + 32 + 36).
        wr16(o + 360, rd16(in + 32 + 36 + 2));
        wr16(o + 362, rd16(in + 32 + 36 + 4));
        wr16(o + 364, rd16(in + 32 + 36 + 0));
        // l4.val[j] as u32 lane 1 = bytes 4..7 of val[j]; val[j] byte i = in[80 + 4i + j].
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) o[368 + 4 * j + k] = in[80 + 4 * (4 + k) + j];
    }
}

// ---- 8. narrowing and widening ---------------------------------------------------------------
#define NWN 256
static s32 nw_in[NWN][8];
static u8 nw_neon[NWN][160], nw_ref[NWN][160];

__declspec(noinline) static void nw_neon_k(void) {
    for (int b = 0; b < NWN; b++) {
        const int32x4_t a = vld1q_s32(nw_in[b]), c = vld1q_s32(nw_in[b] + 4);
        u8* o = nw_neon[b];
        vst1q_s16((s16*)o, vqrshrn_high_n_s32(vqrshrn_n_s32(a, 7), c, 13));
        vst1q_u16((u16*)(o + 16), vqshrun_high_n_s32(vqshrun_n_s32(a, 3), c, 16));
        vst1q_u16((u16*)(o + 32), vmovn_high_u32(vmovn_u32(vreinterpretq_u32_s32(a)), vreinterpretq_u32_s32(c)));
        vst1q_u16((u16*)(o + 48), vqmovn_high_u32(vqmovn_u32(vreinterpretq_u32_s32(a)), vreinterpretq_u32_s32(c)));
        vst1q_s16((s16*)(o + 64), vqmovn_high_s32(vqmovn_s32(a), c));
        vst1q_u16((u16*)(o + 80), vqmovun_high_s32(vqmovun_s32(a), c));
        const uint16x8_t h = vreinterpretq_u16_s32(a);
        vst1q_u8(o + 96, vrshrn_high_n_u16(vrshrn_n_u16(h, 3), vreinterpretq_u16_s32(c), 8));
        vst1_u16((u16*)(o + 112), vshrn_n_u32(vreinterpretq_u32_s32(c), 9));
        vst1_u16((u16*)(o + 120), vqrshrn_n_u32(vreinterpretq_u32_s32(c), 5));
        vst1_u16((u16*)(o + 128), vaddhn_u32(vreinterpretq_u32_s32(a), vreinterpretq_u32_s32(c)));
        vst1_u16((u16*)(o + 136), vraddhn_u32(vreinterpretq_u32_s32(a), vreinterpretq_u32_s32(c)));
        vst1_s16((s16*)(o + 144), vrsubhn_s32(a, c));
        const uint16x8_t w = vshll_n_u8(vreinterpret_u8_s32(vget_low_s32(c)), 3);
        const int32x4_t s = vmovl_s16(vreinterpret_s16_s32(vget_high_s32(a)));
        vst1_u16((u16*)(o + 152), vget_high_u16(vaddq_u16(w, vreinterpretq_u16_s32(s))));
    }
}

static s64 rsh(s64 x, int n) { return (x + (1LL << (n - 1))) >> n; }
static void nw_ref_k(void) {
    for (int b = 0; b < NWN; b++) {
        const s32* a = nw_in[b];
        const s32* c = nw_in[b] + 4;
        u8* o = nw_ref[b];
        for (int i = 0; i < 4; i++) {
            ((s16*)o)[i] = sat16(rsh(a[i], 7));
            ((s16*)o)[4 + i] = sat16(rsh(c[i], 13));
            ((u16*)(o + 16))[i] = satu16((s64)a[i] >> 3);
            ((u16*)(o + 16))[4 + i] = satu16((s64)c[i] >> 16);
            ((u16*)(o + 32))[i] = (u16)a[i];
            ((u16*)(o + 32))[4 + i] = (u16)c[i];
            ((u16*)(o + 48))[i] = satu16((u32)a[i]);
            ((u16*)(o + 48))[4 + i] = satu16((u32)c[i]);
            ((s16*)(o + 64))[i] = sat16(a[i]);
            ((s16*)(o + 64))[4 + i] = sat16(c[i]);
            ((u16*)(o + 80))[i] = satu16(a[i]);
            ((u16*)(o + 80))[4 + i] = satu16(c[i]);
            ((u16*)(o + 112))[i] = (u16)((u32)c[i] >> 9);
            ((u16*)(o + 120))[i] = satu16(((s64)(u32)c[i] + 16) >> 5);
            ((u16*)(o + 128))[i] = (u16)(((u32)a[i] + (u32)c[i]) >> 16);
            ((u16*)(o + 136))[i] = (u16)(((u64)(u32)a[i] + (u32)c[i] + 0x8000u) >> 16);
            ((s16*)(o + 144))[i] = (s16)(u16)((u32)((u32)a[i] - (u32)c[i] + 0x8000u) >> 16);
        }
        for (int i = 0; i < 8; i++) {
            const u16 ha = (u16)((u32)a[i / 2] >> (16 * (i & 1)));
            const u16 hc = (u16)((u32)c[i / 2] >> (16 * (i & 1)));
            o[96 + i] = (u8)((ha + 4u) >> 3);
            o[104 + i] = (u8)((hc + 128u) >> 8);
        }
        // vshll_n_u8(low bytes of c, 3) + the halfwords of vmovl_s16(high halfwords of a), high 4 lanes.
        for (int i = 4; i < 8; i++) {
            const u8 byte = (u8)((u32)c[i / 4] >> (8 * (i & 3)));
            const s32 s = (s16)((u32)a[2 + (i / 2) / 2] >> (16 * ((i / 2) & 1)));
            const u16 s_half = (u16)((u32)s >> (16 * (i & 1)));
            ((u16*)(o + 152))[i - 4] = (u16)((u16)(byte << 3) + s_half);
        }
    }
}

// ---- 9. widening and by-element multiplies --------------------------------------------------
#define MLN 256
static s16 ml_a[MLN][8], ml_b[MLN][8];
static u8 ml_neon[MLN][176], ml_ref[MLN][176];

__declspec(noinline) static void ml_neon_k(void) {
    for (int b = 0; b < MLN; b++) {
        const int16x8_t a = vld1q_s16(ml_a[b]), c = vld1q_s16(ml_b[b]);
        u8* o = ml_neon[b];
        vst1q_s32((s32*)o, vmull_s16(vget_low_s16(a), vget_low_s16(c)));
        vst1q_u32((u32*)(o + 16), vmull_high_u16(vreinterpretq_u16_s16(a), vreinterpretq_u16_s16(c)));
        vst1q_s32((s32*)(o + 32), vmlal_lane_s16(vmovl_s16(vget_low_s16(c)), vget_high_s16(a), vget_low_s16(c), 3));
        vst1q_s32((s32*)(o + 48), vmlsl_n_s16(vmovl_s16(vget_high_s16(c)), vget_low_s16(a), ml_a[b][0]));
        vst1q_s32((s32*)(o + 64), vqdmull_s16(vget_low_s16(a), vget_high_s16(c)));
        vst1q_s32((s32*)(o + 80), vqdmlal_high_s16(vmovl_s16(vget_low_s16(a)), a, c));
        vst1q_s16((s16*)(o + 96), vqdmulhq_laneq_s16(a, c, 7));
        vst1q_s16((s16*)(o + 112), vqrdmulhq_s16(a, c));
        vst1q_s16((s16*)(o + 128), vmulq_lane_s16(a, vget_high_s16(c), 2));
        vst1q_u16((u16*)(o + 144), vmlaq_laneq_u16(vreinterpretq_u16_s16(c), vreinterpretq_u16_s16(a), vreinterpretq_u16_s16(c), 5));
        vst1q_u16((u16*)(o + 160), vmull_p8(vreinterpret_p8_s16(vget_low_s16(a)), vreinterpret_p8_s16(vget_high_s16(c))));
    }
}

static u16 pmul8(u8 a, u8 b) {
    u16 r = 0;
    for (int i = 0; i < 8; i++) if ((b >> i) & 1) r ^= (u16)(a << i);
    return r;
}
static void ml_ref_k(void) {
    for (int b = 0; b < MLN; b++) {
        const s16 *a = ml_a[b], *c = ml_b[b];
        u8* o = ml_ref[b];
        for (int i = 0; i < 4; i++) {
            ((s32*)o)[i] = (s32)a[i] * c[i];
            ((u32*)(o + 16))[i] = (u32)(u16)a[4 + i] * (u16)c[4 + i];
            ((s32*)(o + 32))[i] = (s32)c[i] + (s32)a[4 + i] * c[3];
            ((s32*)(o + 48))[i] = (s32)c[4 + i] - (s32)a[i] * a[0];
            ((s32*)(o + 64))[i] = sat32((s64)2 * a[i] * c[4 + i]);
            ((s32*)(o + 80))[i] = sat32((s64)a[i] + sat32((s64)2 * a[4 + i] * c[4 + i]));
        }
        for (int i = 0; i < 8; i++) {
            ((s16*)(o + 96))[i] = m_sqdmulh16(a[i], c[7]);
            ((s16*)(o + 112))[i] = m_sqrdmulh16(a[i], c[i]);
            ((s16*)(o + 128))[i] = (s16)(u16)((u32)(u16)a[i] * (u16)c[6]);
            ((u16*)(o + 144))[i] = (u16)((u16)c[i] + (u32)(u16)a[i] * (u16)c[5]);
            const u8 x = (u8)((u16)a[i / 2] >> (8 * (i & 1))), y = (u8)((u16)c[4 + i / 2] >> (8 * (i & 1)));
            ((u16*)(o + 160))[i] = pmul8(x, y);
        }
    }
}

// ---- 10. shifts, bit ops, pairwise and misc ---------------------------------------------------
#define MSN 256
static u8 ms_a[MSN][16], ms_b[MSN][16];
static u8 ms_neon[MSN][16 * 24], ms_ref[MSN][16 * 24];

__declspec(noinline) static void ms_neon_k(void) {
    for (int b = 0; b < MSN; b++) {
        const uint8x16_t a = vld1q_u8(ms_a[b]), c = vld1q_u8(ms_b[b]);
        const int16x8_t ah = vreinterpretq_s16_u8(a), ch = vreinterpretq_s16_u8(c);
        uint8x16_t* o = (uint8x16_t*)ms_neon[b];
        vst1q_u8((u8*)&o[0], vreinterpretq_u8_s16(vclzq_s16(ah)));
        vst1q_u8((u8*)&o[1], veorq_u8(vcntq_u8(a), vrbitq_u8(c)));
        vst1q_u8((u8*)&o[2], vaddq_u8(vrev16q_u8(a), vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(c)))));
        vst1q_u8((u8*)&o[3], vreinterpretq_u8_s32(vrev64q_s32(vreinterpretq_s32_u8(a))));
        vst1q_u8((u8*)&o[4], vreinterpretq_u8_s8(vabsq_s8(vreinterpretq_s8_u8(a))));
        vst1q_u8((u8*)&o[5], vreinterpretq_u8_s16(vqabsq_s16(ah)));
        vst1q_u8((u8*)&o[6], vreinterpretq_u8_s16(vqnegq_s16(ch)));
        vst1q_u8((u8*)&o[7], vreinterpretq_u8_u32(vsliq_n_u32(vreinterpretq_u32_u8(a), vreinterpretq_u32_u8(c), 7)));
        vst1q_u8((u8*)&o[8], vreinterpretq_u8_u16(vsriq_n_u16(vreinterpretq_u16_u8(a), vreinterpretq_u16_u8(c), 5)));
        vst1q_u8((u8*)&o[9], vreinterpretq_u8_s16(vsraq_n_s16(ah, ch, 3)));
        vst1q_u8((u8*)&o[10], vreinterpretq_u8_u16(vrsraq_n_u16(vreinterpretq_u16_u8(a), vreinterpretq_u16_u8(c), 11)));
        vst1q_u8((u8*)&o[11], vreinterpretq_u8_u16(vqshluq_n_s16(ah, 4)));
        vst1q_u8((u8*)&o[12], vreinterpretq_u8_s16(vqshlq_s16(ah, ch)));
        vst1q_u8((u8*)&o[13], vreinterpretq_u8_u16(vrshlq_u16(vreinterpretq_u16_u8(a), ch)));
        vst1q_u8((u8*)&o[14], vreinterpretq_u8_s32(vqrshlq_s32(vreinterpretq_s32_u8(a), vreinterpretq_s32_u8(c))));
        vst1q_u8((u8*)&o[15], vreinterpretq_u8_s16(vhaddq_s16(ah, ch)));
        vst1q_u8((u8*)&o[16], vhsubq_u8(a, c));
        vst1q_u8((u8*)&o[17], vreinterpretq_u8_s8(vrhaddq_s8(vreinterpretq_s8_u8(a), vreinterpretq_s8_u8(c))));
        vst1q_u8((u8*)&o[18], vreinterpretq_u8_u16(vtstq_u16(vreinterpretq_u16_u8(a), vreinterpretq_u16_u8(c))));
        vst1q_u8((u8*)&o[19], vornq_u8(vbicq_u8(a, c), vmvnq_u8(vextq_u8(a, c, 11))));
        vst1q_u8((u8*)&o[20], vreinterpretq_u8_u64(vqaddq_u64(vreinterpretq_u64_u8(a), vreinterpretq_u64_u8(c))));
        vst1q_u8((u8*)&o[21], vreinterpretq_u8_s64(vqsubq_s64(vshrq_n_s64(vreinterpretq_s64_u8(a), 3), vreinterpretq_s64_u8(c))));
        vst1q_u8((u8*)&o[22], vreinterpretq_u8_s16(vuqaddq_s16(ah, vreinterpretq_u16_u8(c))));
        vst1q_u8((u8*)&o[23], vsqaddq_u8(a, vreinterpretq_s8_u8(c)));
    }
}

static u16 h16(const u8* p, int i) { return (u16)(p[2 * i] | (p[2 * i + 1] << 8)); }
static u32 w32(const u8* p, int i) { return (u32)p[4 * i] | ((u32)p[4 * i + 1] << 8) | ((u32)p[4 * i + 2] << 16) | ((u32)p[4 * i + 3] << 24); }
static u64 d64(const u8* p, int i) { return (u64)w32(p, 2 * i) | ((u64)w32(p, 2 * i + 1) << 32); }
static void put16(u8* p, int i, u16 v) { p[2 * i] = (u8)v; p[2 * i + 1] = (u8)(v >> 8); }
static void put32(u8* p, int i, u32 v) { for (int k = 0; k < 4; k++) p[4 * i + k] = (u8)(v >> (8 * k)); }
static void put64(u8* p, int i, u64 v) { put32(p, 2 * i, (u32)v); put32(p, 2 * i + 1, (u32)(v >> 32)); }
static u8 rbit8(u8 x) { u8 r = 0; for (int i = 0; i < 8; i++) r |= (u8)(((x >> i) & 1) << (7 - i)); return r; }
static u8 popc8(u8 x) { u8 n = 0; while (x) { n += x & 1; x >>= 1; } return n; }

static void ms_ref_k(void) {
    for (int b = 0; b < MSN; b++) {
        const u8 *a = ms_a[b], *c = ms_b[b];
        u8* o = ms_ref[b];
        for (int i = 0; i < 8; i++) {
            u16 x = h16(a, i), n = 0;
            while (n < 16 && !(x & 0x8000)) { x = (u16)(x << 1); n++; }
            put16(o, i, n);
        }
        for (int i = 0; i < 16; i++) {
            o[16 + i] = (u8)(popc8(a[i]) ^ rbit8(c[i]));
            o[32 + i] = (u8)(a[i ^ 1] + c[(i & ~3) + (2 + (i & 3)) % 4]);
            o[48 + i] = a[(i & ~7) + ((i & 7) + 4) % 8];
            const s8 sa = (s8)a[i];
            o[64 + i] = (u8)(sa < 0 ? (u8)(-(s32)sa) : (u8)sa);
        }
        for (int i = 0; i < 8; i++) {
            const s16 x = (s16)h16(a, i), y = (s16)h16(c, i);
            const u16 ux = h16(a, i), uy = h16(c, i);
            put16(o + 80, i, (u16)sat16(x < 0 ? -(s32)x : x));
            put16(o + 96, i, (u16)sat16(-(s32)y));
            put16(o + 128, i, (u16)((ux & ~(0xffffu >> 5)) | (uy >> 5)));
            put16(o + 144, i, (u16)(x + (y >> 3)));
            put16(o + 160, i, (u16)(ux + ((uy + (1u << 10)) >> 11)));
            put16(o + 176, i, x < 0 ? 0 : satu16((s32)x << 4));
            put16(o + 192, i, (u16)m_sqshl16(x, y));
            put16(o + 208, i, m_urshl16(ux, y));
            put16(o + 240, i, (u16)(s16)(((s32)x + y) >> 1));
            put16(o + 288, i, (ux & uy) ? 0xffff : 0);
            put16(o + 352, i, (u16)sat16((s32)x + (s32)uy));
        }
        for (int i = 0; i < 4; i++) {
            const u32 x = w32(a, i), y = w32(c, i);
            put32(o + 112, i, (x & 0x7f) | (y << 7));
            put32(o + 224, i, (u32)m_sqrshl32((s32)x, (s32)y));
        }
        for (int i = 0; i < 16; i++) {
            o[256 + i] = (u8)(((s32)a[i] - (s32)c[i]) >> 1);
            o[272 + i] = (u8)(((s32)(s8)a[i] + (s8)c[i] + 1) >> 1);
            const u8 e = i + 11 < 16 ? a[i + 11] : c[i - 5];
            o[304 + i] = (u8)((a[i] & ~c[i]) | (u8)~(u8)~e);
            o[368 + i] = satu8((s32)a[i] + (s8)c[i]);
        }
        for (int i = 0; i < 2; i++) {
            const u64 x = d64(a, i), y = d64(c, i);
            put64(o + 320, i, x + y < x ? ~0ull : x + y);
            const s64 sx = (s64)x >> 3, sy = (s64)y;
            const s64 d = (s64)((u64)sx - (u64)sy);
            const int ovf = ((sx ^ sy) & (sx ^ d)) < 0;
            put64(o + 336, i, (u64)(ovf ? (sx < 0 ? (s64)0x8000000000000000ull : 0x7fffffffffffffffll) : d));
        }
    }
}

// ---- driver ---------------------------------------------------------------------------------
static void init(void) {
    fill(cdf_in, sizeof(cdf_in));
    for (int s = 0; s < SYMS; s++) {
        for (int i = 0; i < 8; i++) cdf_in[s][i] = (u16)(32768u - (u32)(8 - i) * 4096u + (cdf_in[s][i] & 0x7ff));
        if (s & 7) for (int i = 0; i < 8; i++) cdf_in[s][i] &= 0x7fff;
        sym_rng[s] = (u16)(0x8000u | (lcg() >> 16));
        sym_dif[s] = (u16)(lcg() >> 16);
    }
    fill(mc_src, sizeof(mc_src));
    fill(tx_in, sizeof(tx_in));
    fill(tx32_in, sizeof(tx32_in));
    fill(lf_p1, sizeof(lf_p1)); fill(lf_p0, sizeof(lf_p0)); fill(lf_q0, sizeof(lf_q0)); fill(lf_q1, sizeof(lf_q1));
    for (int i = 0; i < LFN; i += 3) { lf_p1[i] = (u8)(lf_p0[i] + (lf_p1[i] & 15)); lf_q0[i] = (u8)(lf_p0[i] + (lf_q0[i] & 31)); lf_q1[i] = (u8)(lf_q0[i] - (lf_q1[i] & 7)); }
    fill(cd_px, sizeof(cd_px)); fill(cd_c, sizeof(cd_c));
    for (int b = 0; b < CDN; b += 2)
        for (int i = 0; i < 8; i++) cd_px[b][i] = (s16)(cd_c[b][i] + (s16)(cd_px[b][i] % 97));
    fill(tb_tab, sizeof(tb_tab)); fill(tb_idx, sizeof(tb_idx));
    for (int b = 0; b < CDN; b += 2)
        for (int i = 0; i < 16; i++) tb_idx[b][i] &= 0x4f;
    fill(sad_a, sizeof(sad_a)); fill(sad_b, sizeof(sad_b));
    fill(st_in, sizeof(st_in));
    fill(nw_in, sizeof(nw_in));
    for (int b = 0; b < NWN; b += 2)
        for (int i = 0; i < 8; i++) nw_in[b][i] >>= (lcg() >> 16) % 24;
    fill(ml_a, sizeof(ml_a)); fill(ml_b, sizeof(ml_b));
    fill(ms_a, sizeof(ms_a)); fill(ms_b, sizeof(ms_b));
    for (int b = 0; b < MSN; b += 2)
        for (int i = 0; i < 16; i++) ms_b[b][i] = (u8)((s8)(ms_b[b][i] % 40) - 20);   // in-range shift counts
}

typedef void (*Kernel)(void);
static void check(const char* name, const void* got, const void* want, unsigned bytes) {
    const u8 *g = (const u8*)got, *w = (const u8*)want;
    for (unsigned i = 0; i < bytes; i++)
        if (g[i] != w[i]) {
            out("CHECK "); out(name); out(" MISMATCH at "); out_u64(i); out(" neon="); out_hex(g[i]); out(" ref="); out_hex(w[i]);
            out("\r\n");
            g_failures++;
            return;
        }
    out("CHECK "); out(name); out(" ok\r\n");
}
static void run(const char* name, Kernel neon, Kernel ref, const void* got, const void* want, unsigned bytes, u64 reps, u64 ops) {
    neon();
    ref();
    check(name, got, want, bytes);
    if (g_check_only) return;
    reps = reps * g_mult / g_scale ? reps * g_mult / g_scale : 1;
    const u64 t0 = now_ticks();
    for (u64 r = 0; r < reps; r++) neon();
    const u64 t = now_ticks() - t0;
    out("BENCH "); out(name); out(" ops="); out_u64(reps * ops); out(" ns="); out_u64(ticks_to_ns(t));
    out(" check="); out_hex(fold(got, bytes)); out("\r\n");
}

void guest_entry(void) {
    QueryPerformanceFrequency(&freq);
    parse_args();
    init();
    run("neonk_symbol", symbol_neon, symbol_ref, cdf_neon, cdf_ref, sizeof(cdf_neon), 4000, SYMS);
    check("neonk_symbol_sym", sym_neon, sym_ref, sizeof(sym_neon));
    run("neonk_mc8tap", mc_neon, mc_ref, mc_h_neon, mc_h_ref, sizeof(mc_h_neon), 8000, MCW);
    check("neonk_mc8tap_v", mc_v_neon, mc_v_ref, sizeof(mc_v_neon));
    run("neonk_txfm", tx_neon_k, tx_ref_k, tx_neon, tx_ref, sizeof(tx_neon), 8000, TXN);
    check("neonk_txfm32", tx32_neon, tx32_ref, sizeof(tx32_neon));
    run("neonk_loopfilter", lf_neon_k, lf_ref_k, lf_neon, lf_ref, sizeof(lf_neon), 8000, LFN / 16);
    run("neonk_cdef", cd_neon_k, cd_ref_k, cd_neon, cd_ref, sizeof(cd_neon), 4000, CDN);
    check("neonk_tbl", tb_neon, tb_ref, sizeof(tb_neon));
    run("neonk_sad", sad_neon_k, sad_ref_k, sad_neon, sad_ref, sizeof(sad_neon), 8000, SADN);
    run("neonk_struct", st_neon_k, st_ref_k, st_neon, st_ref, sizeof(st_neon), 8000, STN);
    run("neonk_narrow", nw_neon_k, nw_ref_k, nw_neon, nw_ref, sizeof(nw_neon), 8000, NWN);
    run("neonk_mul", ml_neon_k, ml_ref_k, ml_neon, ml_ref, sizeof(ml_neon), 8000, MLN);
    run("neonk_misc", ms_neon_k, ms_ref_k, ms_neon, ms_ref, sizeof(ms_neon), 4000, MSN);
    out("NEONK failures="); out_u64((u64)g_failures); out("\r\n");
    ExitProcess(g_failures ? 1 : 0);
}
