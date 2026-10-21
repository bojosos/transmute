// Translator core + the real fp/neon/system families: differential fuzzing of
// programs that mix family instructions (vector ops and memory, FCMP feeding
// the core's lazy flags, MRS/MSR NZCV, barriers, CRC) with core code
// (owner: translator core). Built only when all three family libraries exist.
#include "fx/testing/test.hpp"
#include "fx/testing/harness.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_emitter.hpp"
#include "fx/translator/translator.hpp"

#include <windows.h>
#include <immintrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

namespace {

// Encodings assembled with clang (aarch64-pc-windows-msvc).
constexpr std::uint32_t kFamilyWords[] = {
    0x4ee28420u,   // add v0.2d, v1.2d, v2.2d
    0x6ea58483u,   // sub v3.4s, v4.4s, v5.4s
    0x4e281ce6u,   // and v6.16b, v7.16b, v8.16b
    0x4eab1d49u,   // orr v9.16b, v10.16b, v11.16b
    0x6e2e1dacu,   // eor v12.16b, v13.16b, v14.16b
    0x0e205a0fu,   // cnt v15.8b, v16.8b
    0x4e31ba51u,   // addv b17, v18.16b
    0x4e080c33u,   // dup v19.2d, x1
    0x4e040c54u,   // dup v20.4s, w2
    0x4e083ea3u,   // umov x3, v21.d[0]
    0x0e0c3ec4u,   // umov w4, v22.s[1]
    0x4e181cb7u,   // mov v23.d[1], x5
    0x9e6700d8u,   // fmov d24, x6
    0x9e660327u,   // fmov x7, d25
    0x1e27011au,   // fmov s26, w8
    0x1e7c2360u,   // fcmp d27, d28
    0x1e2023a8u,   // fcmp s29, #0.0
    0x1e611404u,   // fccmp d0, d1, #4, ne
    0x1e64bc62u,   // fcsel d2, d3, d4, lt
    0xd53b4209u,   // mrs x9, nzcv
    0xd51b420au,   // msr nzcv, x10
    0xd5033bbfu,   // dmb ish
    0xd50339bfu,   // dmb ishld
    0xd503201fu,   // nop
    0xd5033fdfu,   // isb
    0x3dc00a00u,   // ldr q0, [x16, #32]
    0x3c9f0e21u,   // str q1, [x17, #-16]!
    0xad410fe2u,   // ldp q2, q3, [sp, #32]
    0x6c811604u,   // stp d4, d5, [x16], #16
    0xbc5fc226u,   // ldur s6, [x17, #-4]
    0x4c407207u,   // ld1 {v7.16b}, [x16]
    0x4c00ae28u,   // st1 {v8.2d, v9.2d}, [x17]
    0x4d40ca0au,   // ld1r {v10.4s}, [x16]
    0x9acd5d8bu,   // crc32cx w11, w12, x13
    0x1ac24020u,   // crc32b w0, w1, w2
    0x4e20098bu,   // rev64 v11.16b, v12.16b
    0x4e8f39cdu,   // zip1 v13.4s, v14.4s, v15.4s
    0x6e121a30u,   // ext v16.16b, v17.16b, v18.16b, #3
    0x6eb58e93u,   // cmeq v19.4s, v20.4s, v21.4s
    0x6f7906f6u,   // ushr v22.2d, v23.2d, #7
    0x4f235738u,   // shl v24.4s, v25.4s, #3
    0x0e212b7au,   // xtn v26.8b, v27.8h
    0x6e602bbcu,   // uaddlp v28.4s, v29.8h
    0x6f05e55eu,   // movi v30.2d, #0xff00ff00ff00ff00
    0x6f00065fu,   // mvni v31.4s, #0x12
    0x9e620020u,   // scvtf d0, x1
    0x1e230062u,   // ucvtf s2, w3
    0x9e7800a4u,   // fcvtzs x4, d5
    0x1e6140e6u,   // fneg d6, d7
    0x1e20c128u,   // fabs s8, s9
};

void register_all() {
    static bool done = false;
    if (done) return;
    done = true;
    register_fp_emitters();
    register_neon_emitters();
    register_system_emitters();
}

std::vector<std::uint32_t> generate(std::uint64_t seed, unsigned& family_words) {
    Rng rng(seed);
    InstGen g(rng);
    std::vector<std::uint32_t> w;
    const unsigned len = 8 + rng.below(32);
    const auto family = [&] { return kFamilyWords[rng.below(static_cast<unsigned>(std::size(kFamilyWords)))]; };
    family_words = 0;
    bool loop = false;
    std::size_t loop_begin = 0, loop_end = 0;   // [mov w12 .. b.ne]
    while (w.size() < len) {
        const auto c = rng.below(100);
        if (c < 40) {
            w.push_back(family());
            ++family_words;
        } else if (c < 48) {   // consume flags (possibly set by FCMP/FCCMP/MSR)
            w.push_back(a64::csel(true, false, rng.pick(kSrc), rng.below(16), rng.below(2), rng.pick(kSrc), rng.pick(kDest)));
        } else if (c < 54) {   // forward conditional branch over 1..3 instructions
            w.push_back(a64::bcond(rng.below(16), 2 + static_cast<std::int32_t>(rng.below(3))));
        } else if (c < 58 && !loop) {   // counted loop around family code
            loop = true;
            loop_begin = w.size();
            w.push_back(a64::movw(false, 2, 0, 1 + rng.below(4), 12));
            const auto start = w.size();
            for (unsigned i = 0, n = 1 + rng.below(5); i < n; ++i) w.push_back(rng.chance(50) ? family() : g.data_or_memory());
            w.push_back(a64::add_imm(true, true, true, 12, 12, 1));
            w.push_back(a64::bcond(1, static_cast<std::int32_t>(start) - static_cast<std::int32_t>(w.size())));
            loop_end = w.size() - 1;
        } else {
            w.push_back(g.data_or_memory());
        }
    }
    for (std::size_t i = 0; i < w.size(); ++i) {
        if ((w[i] & 0xff000010u) != 0x54000000u || (loop && i == loop_end)) continue;
        const auto off = static_cast<std::int32_t>(w[i] << 8) >> 13;
        const auto target = i + static_cast<std::size_t>(off);
        const bool past_end = target > w.size();
        const bool into_loop = loop && (i < loop_begin || i > loop_end) && target > loop_begin && target <= loop_end;
        if (past_end || into_loop) w[i] = a64::nop();
    }
    return w;
}

} // namespace

FX_TEST(core_integration_neon_hooks_linked) { FX_CHECK(core::neon_region_hooks_linked()); }

FX_TEST(core_integration_cached_vector_destinations) {
    register_all();
    DiffRunner r;
    std::vector<std::uint32_t> words;
    for (unsigned pass = 0; pass < 4; ++pass)
        for (unsigned v = 0; v < 16; ++v)
            words.push_back(a64::neon_add2d(v, v, 31));
    words.push_back(a64::brk(0xdead));
    std::memcpy(reinterpret_cast<void*>(r.program_base()), words.data(), words.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + words.size() * 4);
    const auto initial = r.context(19835);
    GuestContext results[2];
    std::size_t sizes[2]{};
    for (unsigned mode = 0; mode < 2; ++mode) {
        core::Plan plan;
        core::PlanOverrides overrides;
        overrides.vector_pin_limit = 0;
        overrides.no_reuse_dest = mode == 0;
        core::build_plan(r.program_base(), code, {}, overrides, plan);
        TranslatedRegion region;
        core::RegionEmitter(plan, {}, region).run();
        sizes[mode] = region.code.size();
        void* host = VirtualAlloc(nullptr, region.code.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        FX_REQUIRE(host);
        std::memcpy(host, region.code.data(), region.code.size());
        FlushInstructionCache(GetCurrentProcess(), host, region.code.size());
        results[mode] = initial;
        r.machine.prepare(results[mode]);
        r.machine.enter(results[mode], static_cast<std::uint8_t*>(host) + region.entries[0].offset);
        VirtualFree(host, 0, MEM_RELEASE);
        for (unsigned v = 0; v < 16; ++v)
            for (unsigned lane = 0; lane < 2; ++lane)
                FX_CHECK_EQ(results[mode].v[v][lane], initial.v[v][lane] + 4 * initial.v[31][lane]);
    }
    std::printf("    Real NEON cached vector destinations: %zu -> %zu host bytes\n", sizes[0], sizes[1]);
    FX_CHECK(sizes[1] < sizes[0]);
    testing::compare_contexts(results[0], results[1], "real cached destinations");
}

FX_TEST(core_integration_families_fuzz) {
    register_all();
    static DiffRunner r;
    int failures = 0;
    unsigned translated_family = 0, total_family = 0, vector_pins = 0;
    for (std::uint64_t seed = 1; seed <= 2500 && failures < 8; ++seed) {
        unsigned fw = 0;
        const auto prog = generate(seed, fw);
        total_family += fw;
        auto ctx = r.context(seed * 13);
        // Keep the FP control state at its default (round to nearest, no flush).
        ctx.fpcr = 0;
        ctx.fpsr = 0;
        if (!r.run(prog, ctx, "integration seed " + std::to_string(seed))) {
            ++failures;
            std::string hex;
            char buf[16];
            for (auto x : prog) { std::snprintf(buf, sizeof buf, "%08x ", x); hex += buf; }
            std::fprintf(stderr, "    program: %s\n", hex.c_str());
        }
        translated_family += fw > r.last.interprets ? fw - r.last.interprets : 0;
        for (const auto& region : r.machine.regions)
            for (const auto& pin : region.pins) vector_pins += pin.vector ? 1u : 0u;
    }
    std::printf("    family instructions translated (not interpreted): ~%u of %u; vector pins: %u\n", translated_family,
                total_family, vector_pins);
    if (core::codegen_knob_bits() & 4u) FX_CHECK_EQ(vector_pins, 0u);   // novecpins
    else FX_CHECK(vector_pins > 0);
}

namespace {
double seconds() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
__declspec(noinline) void native_fma(float* acc, const float* in, std::uint64_t n) {
    __m128 a0 = _mm_loadu_ps(acc), a1 = _mm_loadu_ps(acc + 4), a2 = _mm_loadu_ps(acc + 8), a3 = _mm_loadu_ps(acc + 12);
    __m128 x = _mm_loadu_ps(in);
    const __m128 b0 = _mm_loadu_ps(in + 4), b1 = _mm_loadu_ps(in + 8), b2 = _mm_loadu_ps(in + 12);
    const __m128 b3 = _mm_loadu_ps(in + 16), d = _mm_loadu_ps(in + 20);
    for (std::uint64_t i = 0; i != n; ++i) {
        a0 = _mm_fmadd_ps(x, b0, a0);
        a1 = _mm_fmadd_ps(x, b1, a1);
        a2 = _mm_fmadd_ps(x, b2, a2);
        a3 = _mm_fmadd_ps(x, b3, a3);
        x = _mm_add_ps(x, d);
    }
    _mm_storeu_ps(acc, a0);
    _mm_storeu_ps(acc + 4, a1);
    _mm_storeu_ps(acc + 8, a2);
    _mm_storeu_ps(acc + 12, a3);
}
} // namespace

// NEON FMA loop (vector pins in xmm6..15) vs the same loop with x64 FMA intrinsics.
FX_TEST(core_integration_perf_neon_fma_loop) {
    register_all();
    static DiffRunner r;
    const DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), 1);
    constexpr std::uint64_t n = 50'000'000;
    const std::vector<std::uint32_t> prog = {
        0x4e25cc80u,   // loop: fmla v0.4s, v4.4s, v5.4s
        0x4e26cc81u,   // fmla v1.4s, v4.4s, v6.4s
        0x4e27cc82u,   // fmla v2.4s, v4.4s, v7.4s
        0x4e30cc83u,   // fmla v3.4s, v4.4s, v16.4s
        0x4e31d484u,   // fadd v4.4s, v4.4s, v17.4s
        0xf100058cu,   // subs x12, x12, #1
        0x54ffff41u,   // b.ne loop
    };
    float in[24];
    for (int i = 0; i < 20; ++i) in[i] = 1.0f + 0.001f * static_cast<float>(i);
    for (int i = 20; i < 24; ++i) in[i] = 1e-7f;
    std::vector<double> tt, tn;
    for (int rep = 0; rep < 5; ++rep) {
        auto ctx = r.context(1);
        ctx.fpcr = 0;
        ctx.x[12] = n;
        for (unsigned v = 0; v < 4; ++v) ctx.v[v][0] = ctx.v[v][1] = 0;
        const unsigned src_regs[] = {4, 5, 6, 7, 16, 17};
        for (unsigned k = 0; k < 6; ++k) std::memcpy(ctx.v[src_regs[k]], in + 4 * k, 16);
        const auto t0 = seconds();
        FX_CHECK(r.run_translated(prog, ctx));
        tt.push_back(seconds() - t0);
        float acc[16] = {};
        const auto t1 = seconds();
        native_fma(acc, in, n);
        tn.push_back(seconds() - t1);
        if (rep == 0) {
            float got[16];
            for (unsigned v = 0; v < 4; ++v) std::memcpy(got + 4 * v, r.actual.v[v], 16);
            FX_CHECK(std::memcmp(got, acc, sizeof got) == 0);
        }
    }
    SetThreadAffinityMask(GetCurrentThread(), old);
    std::sort(tt.begin(), tt.end());
    std::sort(tn.begin(), tn.end());
    unsigned vpins = 0;
    for (const auto& region : r.machine.regions)
        for (const auto& pin : region.pins) vpins += pin.vector ? 1u : 0u;
    std::printf("    NEON FMA loop: native %.3f ns/iter, translated %.3f ns/iter, ratio %.2fx (%u vector pins)\n",
                tn[2] * 1e9 / n, tt[2] * 1e9 / n, tt[2] / tn[2], vpins);
}
