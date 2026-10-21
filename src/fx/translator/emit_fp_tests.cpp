// fp family tests: MXCSR mapping, decoder, differential tests against the
// reference interpreter (owner: fp).
#include "fx/translator/emit_fp_test_util.hpp"
#include "fx/translator/emit_fp_tests.hpp"
#include "fx/translator/emit_fp_internal.hpp"

#include <cstdio>

using namespace transmute::fx;
using namespace transmute::fx::fptest;

// ---- MXCSR / FPCR / FPSR mapping --------------------------------------------------------

FX_TEST(fp_mxcsr_for_fpcr_maps_rounding_and_fz) {
    FX_CHECK_EQ(fp_mxcsr_for_fpcr(0), 0x1f80u);
    FX_CHECK_EQ(fp_mxcsr_for_fpcr(1u << 22), 0x1f80u | (2u << 13));   // RP -> up
    FX_CHECK_EQ(fp_mxcsr_for_fpcr(2u << 22), 0x1f80u | (1u << 13));   // RM -> down
    FX_CHECK_EQ(fp_mxcsr_for_fpcr(3u << 22), 0x1f80u | (3u << 13));   // RZ
    FX_CHECK_EQ(fp_mxcsr_for_fpcr(1u << 24), 0x1f80u | 0x8040u);       // FZ -> FTZ|DAZ
    // Trap enables, DN, AHP, FZ16 do not change MXCSR (exceptions stay masked).
    FX_CHECK_EQ(fp_mxcsr_for_fpcr((1u << 25) | (1u << 26) | (1u << 19) | 0x9f00u), 0x1f80u);
}

FX_TEST(fp_fpsr_flag_mapping_round_trips) {
    for (std::uint32_t f = 0; f < 32; ++f) {
        const auto m = fp_mxcsr_flags_from_fpsr(f);
        FX_CHECK_EQ(fp_fpsr_flags_from_mxcsr(m), f);
    }
    FX_CHECK_EQ(fp_fpsr_flags_from_mxcsr(0x01), 0x01u);   // IE -> IOC
    FX_CHECK_EQ(fp_fpsr_flags_from_mxcsr(0x04), 0x02u);   // ZE -> DZC
    FX_CHECK_EQ(fp_fpsr_flags_from_mxcsr(0x08), 0x04u);   // OE -> OFC
    FX_CHECK_EQ(fp_fpsr_flags_from_mxcsr(0x10), 0x08u);   // UE -> UFC
    FX_CHECK_EQ(fp_fpsr_flags_from_mxcsr(0x20), 0x10u);   // PE -> IXC
    FX_CHECK_EQ(fp_fpsr_flags_from_mxcsr(0x02), 0x00u);   // DE ignored
    FX_CHECK_EQ(fp_read_fpsr(0x08000000u, 0x1f80u | 0x21u), 0x08000011u);   // QC kept
    std::uint32_t fpsr = 0, mxcsr = 0;
    fp_write_fpsr(0x08000013u, 0x1f80u | 0x3fu, fpsr, mxcsr);
    FX_CHECK_EQ(fpsr, 0x08000013u);
    FX_CHECK_EQ(mxcsr, 0x1f80u | 0x01u | 0x04u | 0x20u);
    FX_CHECK_EQ(fp_mxcsr_for_state(1u << 24, 0x1fu), 0x9fc0u | 0x3du);
}

FX_TEST(fp_length_decoder_counts_assembled_sequences) {
    using namespace transmute::fx::x64;
    Assembler a;
    unsigned n = 0;
    a.mov(64, Gp::rax, ptr(Gp::r15, -128)); ++n;
    a.movdqu(Xmm::xmm3, ptr(Gp::r15, 0x400)); ++n;
    a.vex(vex::vfmadd231ps, Xmm::xmm1, Xmm::xmm2, Xmm::xmm12); ++n;
    a.sse_imm(sse::cmpps, Xmm::xmm4, Xmm::xmm9, 7); ++n;
    a.mov_imm(Gp::r9, 0x123456789ULL); ++n;
    a.mov_imm(Gp::rcx, 0x80000000ULL); ++n;
    a.cmov(Cond::p, 32, Gp::rax, Gp::r11); ++n;
    a.setcc(Cond::a, Gp::rsi); ++n;
    a.cmp(32, Gp::rdx, static_cast<std::int32_t>(0x80000000u)); ++n;
    a.cvttss2si(Gp::r10, Xmm::xmm8, 64); ++n;
    a.pextr(64, Gp::rbx, Xmm::xmm14, 1); ++n;
    a.vex_is4(vex::vblendvps, Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3); ++n;
    a.sse_shift_imm(0x72, 6, Xmm::xmm5, 31); ++n;
    a.test(64, Gp::rax, Gp::rax); ++n;
    a.bt(64, Gp::rax, 3); ++n;
    a.imul(32, Gp::rax, Gp::rax, 0x8040); ++n;
    a.stmxcsr(ptr(Gp::r15, 0x150)); ++n;
    a.finalize();
    FX_CHECK_EQ(count_x64_instructions(a.bytes()), n);
}

// ---- driver ------------------------------------------------------------------------------

namespace transmute::fx::fptest {

bool accepts(std::uint32_t word) {
    x64::Assembler a;
    testing::MemoryEmitContext ctx(a, 0x140001000ULL);
    return fp_emit(ctx, std::span(&word, 1)) != 0;
}

void run_spec(const Spec& spec, Tally& tally) {
    Rng rng(spec.seed);
    unsigned failures_before = static_cast<unsigned>(testing::current_failures());
    for (unsigned w = 0; w < spec.words; ++w) {
        const std::uint32_t word = spec.gen(rng);
        if (!accepts(word)) {
            ++tally.declined;
            const auto host = fp_host_features();
            const bool full_host = host.avx && host.fma && host.f16c;   // else feature declines are expected
            if (spec.expect_accept && full_host) {
                char text[96];
                std::snprintf(text, sizeof(text), "%s: fp declined 0x%08x", spec.name, word);
                testing::report_failure(__FILE__, __LINE__, text);
            }
            continue;
        }
        for (unsigned c = 0; c < spec.contexts; ++c) {
            const std::uint64_t seed = rng.next();
            GuestContext ctx = testing::random_context(seed);
            Rng vr(seed ^ 0x5555);
            fill_vectors(ctx, vr, spec.es, spec.snan);
            if (spec.prepare) spec.prepare(ctx, vr);
            const auto& modes = fpcr_modes();
            const std::uint32_t fpcr = spec.all_modes ? 0 : modes[rng.below(static_cast<unsigned>(modes.size()))];
            const unsigned nmodes = spec.all_modes ? static_cast<unsigned>(modes.size()) : 1;
            for (unsigned m = 0; m < nmodes; ++m) {
                ctx.fpcr = spec.all_modes ? modes[m] : fpcr;
                if (spec.fpcr_extra) ctx.fpcr |= spec.fpcr_extra;
                char label[160];
                std::snprintf(label, sizeof(label), "%s 0x%08x fpcr=%08x seed=%llx", spec.name, word, ctx.fpcr,
                              static_cast<unsigned long long>(seed));
                bool missing = false;
                if (spec.reference) {
                    GuestContext expected = ctx;
                    expected.fpsr = 0;
                    if (!spec.reference(word, expected)) { ++tally.oracle_missing; continue; }
                    expected.pc += 4;
                    check_expected(word, ctx, expected, spec.opt, label);
                } else {
                    check_word(word, ctx, spec.opt, label, &missing);
                }
                if (missing) ++tally.oracle_missing;
                else ++tally.checked;
            }
            if (static_cast<unsigned>(testing::current_failures()) > failures_before + 12) return;
        }
    }
}

void run_specs(std::initializer_list<Spec> specs) {
    for (const auto& spec : specs) {
        Tally t;
        run_spec(spec, t);
        std::printf("    %-34s checked %6u  oracle-missing %5u  declined %4u\n", spec.name, t.checked,
                    t.oracle_missing, t.declined);
        if (spec.expect_oracle) FX_CHECK(t.oracle_missing == 0);
        FX_CHECK(t.checked > 0 || !spec.expect_oracle);
        // The same spec on the SSE4.1-only paths (fused and half-precision
        // forms are declined there and left to the interpreter).
        FeatureScope scope(sse_only());
        Spec sse = spec;
        sse.seed = spec.seed * 31 + 7;
        sse.expect_accept = false;
        sse.words = spec.words / 2 + 1;
        Tally ts;
        run_spec(sse, ts);
        std::printf("    %-34s checked %6u  oracle-missing %5u  declined %4u  [sse4.1]\n", spec.name, ts.checked,
                    ts.oracle_missing, ts.declined);
    }
}

} // namespace transmute::fx::fptest

// Lists encodings the reference interpreter does not implement (diagnostic).
FX_TEST(fp_zz_oracle_coverage_report) {
    using namespace transmute::fx::fptest;
    struct Item { const char* name; std::uint32_t word; };
    const Item items[] = {
        {"FMULX v.4s", enc::v3(1, 0, 0, 0x1b, 0, 1, 2)}, {"FMULX v.2d", enc::v3(1, 0, 1, 0x1b, 0, 1, 2)},
        {"FMUL v.4s", enc::v3(1, 1, 0, 0x1b, 0, 1, 2)},  {"FABD v.4s", enc::v3(1, 1, 2, 0x1a, 0, 1, 2)},
        {"FDIV v.4s", enc::v3(1, 1, 0, 0x1f, 0, 1, 2)},  {"FMAX v.4s", enc::v3(1, 0, 0, 0x1e, 0, 1, 2)},
        {"FMIN v.4s", enc::v3(1, 0, 2, 0x1e, 0, 1, 2)},  {"FMAXP v.4s", enc::v3(1, 1, 0, 0x1e, 0, 1, 2)},
        {"FMINP v.4s", enc::v3(1, 1, 2, 0x1e, 0, 1, 2)}, {"FMAXNM v.4s", enc::v3(1, 0, 0, 0x18, 0, 1, 2)},
        {"FMINNM v.4s", enc::v3(1, 0, 2, 0x18, 0, 1, 2)}, {"FMAXNMP v.4s", enc::v3(1, 1, 0, 0x18, 0, 1, 2)},
        {"FMINNMP v.4s", enc::v3(1, 1, 2, 0x18, 0, 1, 2)}, {"FMAXP v.2s", enc::v3(0, 1, 0, 0x1e, 0, 1, 2)},
        {"FMULX s", enc::s3(0, 0, 0x1b, 0, 1, 2)},        {"FABD s", enc::s3(1, 2, 0x1a, 0, 1, 2)},
        {"FCMEQ s", enc::s3(0, 0, 0x1c, 0, 1, 2)},        {"FCMGE s", enc::s3(1, 0, 0x1c, 0, 1, 2)},
        {"FCMGT s", enc::s3(1, 2, 0x1c, 0, 1, 2)},        {"FACGE s", enc::s3(1, 0, 0x1d, 0, 1, 2)},
        {"FACGT s", enc::s3(1, 2, 0x1d, 0, 1, 2)},        {"FRECPS s", enc::s3(0, 0, 0x1f, 0, 1, 2)},
        {"FRECPE v.4s", enc::v2(1, 0, 2, 0x1d, 0, 1)},    {"FRSQRTE v.4s", enc::v2(1, 1, 2, 0x1d, 0, 1)},
        {"FRECPE s", enc::s2(0, 2, 0x1d, 0, 1)},          {"FRSQRTE s", enc::s2(1, 2, 0x1d, 0, 1)},
        {"FRECPX s", enc::s2(0, 2, 0x1f, 0, 1)},          {"FJCVTZS", 0x1e7e0000u},
        {"FMOV h<-w", enc::fpint(0, 3, 0, 7, 0, 1)},      {"FADD h", enc::fp2(2, 3, 0, 1, 2)},
        {"FCVT h<-s", enc::fp1(7, 0, 0, 1)},              {"FCVTL 4s<-4h", enc::v2(0, 0, 0, 0x17, 0, 1)},
        {"FCVTN 4h<-4s", enc::v2(0, 0, 0, 0x16, 0, 1)},   {"FADD v.8h", 0x4e401400u},
        {"SCVTF h<-w", enc::fpint(0, 3, 0, 2, 0, 1)},     {"FRINT32Z s", enc::fp1(16, 0, 0, 1)},
        {"FRINT64X v.2d", enc::v2(1, 1, 1, 0x1f, 0, 1)},  {"FMLAL2 v.4s", enc::v3(1, 1, 0, 0x19, 0, 1, 2)},
        {"MRS fpsr", 0xd53b4420u}, {"MSR fpsr", 0xd51b4420u}, {"MRS fpcr", 0xd53b4400u}, {"MSR fpcr", 0xd51b4400u},
    };
    for (const auto& item : items) {
        GuestContext ctx = transmute::fx::testing::random_context(1);
        const bool ok = transmute::fx::testing::oracle_step(ctx, item.word);
        std::printf("    oracle %-14s 0x%08x %s   fp %s\n", item.name, item.word, ok ? "yes" : "MISSING",
                    accepts(item.word) ? "yes" : "no");
    }
}

// Random words over the data-processing (FP and SIMD) encoding space: every
// word the family accepts must emit without throwing on both feature sets,
// describe() must agree, and where the reference interpreter implements the
// word the results must match.
FX_TEST(fp_encoding_space_fuzz) {
    using namespace transmute::fx::fptest;
    Rng rng(2024);
    unsigned accepted = 0, compared = 0, mismatched_words = 0;
    for (unsigned i = 0; i < 400000; ++i) {
        std::uint32_t w = static_cast<std::uint32_t>(rng.next());
        w = (w & ~0x0e000000u) | 0x0e000000u;   // op0 bits 27:25 = 111 (FP / SIMD data processing)
        if (i & 1) w = (w & ~0x10000000u) | ((i & 2) ? 0x10000000u : 0);   // half scalar-FP / AdvSIMD scalar
        InstructionUse use;
        const bool described = fp_describe(w, use);
        bool ok_host = false;
        {
            x64::Assembler a;
            testing::MemoryEmitContext ctx(a, 0x140001000ULL);
            ok_host = fp_emit(ctx, std::span(&w, 1)) != 0;
        }
        {
            FeatureScope sse(sse_only());
            x64::Assembler a;
            testing::MemoryEmitContext ctx(a, 0x140001000ULL);
            const bool ok_sse = fp_emit(ctx, std::span(&w, 1)) != 0;
            if (ok_sse && !described) FX_CHECK(described);
        }
        if (ok_host && !described) {
            char text[64];
            std::snprintf(text, sizeof(text), "emitted but not described: 0x%08x", w);
            testing::report_failure(__FILE__, __LINE__, text);
        }
        if (!ok_host) continue;
        ++accepted;
        InstructionUse actual;
        if (emitted_uses(w, actual)) {
            const bool covered = (actual.gpr_read & ~use.gpr_read) == 0 && (actual.gpr_write & ~use.gpr_write) == 0 &&
                                 (actual.vec_read & ~use.vec_read) == 0 && (actual.vec_write & ~use.vec_write) == 0 &&
                                 (!actual.reads_flags || use.reads_flags) && (!actual.writes_flags || use.writes_flags) &&
                                 actual.vec_write == use.vec_write && actual.gpr_write == use.gpr_write &&
                                 actual.writes_flags == use.writes_flags;
            if (!covered) {
                char text[160];
                std::snprintf(text, sizeof(text),
                              "describe mismatch 0x%08x: vr %08x/%08x vw %08x/%08x gr %08x/%08x gw %08x/%08x",
                              w, actual.vec_read, use.vec_read, actual.vec_write, use.vec_write, actual.gpr_read,
                              use.gpr_read, actual.gpr_write, use.gpr_write);
                testing::report_failure(__FILE__, __LINE__, text);
            }
        }
        fpx::Inst pre;
        fpx::decode(w, pre);
        const bool nm = pre.k == fpx::K::fmaxnm || pre.k == fpx::K::fminnm || pre.k == fpx::K::fmaxnmp ||
                        pre.k == fpx::K::fminnmp || pre.k == fpx::K::fmaxnmv || pre.k == fpx::K::fminnmv;
        GuestContext ctx = testing::random_context(rng.next());
        Rng vr(i);
        fill_vectors(ctx, vr, rng.below(2) ? 32 : 64, !nm);   // NM + sNaN: documented gap
        ctx.fpcr = fpcr_modes()[rng.below(8)];
        GuestContext probe = ctx;
        if (!testing::oracle_step(probe, w)) continue;
        // Skip encodings with known oracle defects (scalar single FMAX family, FP16 decoded as double).
        if ((w & 0xff200c00u) == 0x1e200800u && ((w >> 12) & 15) >= 4 && ((w >> 12) & 15) <= 7) continue;
        if ((w & 0x5f000000u) == 0x1e000000u && ((w >> 22) & 3) == 3) continue;
        if ((w & 0x5f000000u) == 0x1f000000u && ((w >> 22) & 3) == 3) continue;
        // FRSQRTS overflow gap (|a*b| >= 2^127) is covered by its own test.
        if ((w & 0x9fa0fc00u) == 0x0ea0fc00u || (w & 0xdfa0fc00u) == 0x5ea0fc00u) continue;
        fpx::Inst inst;
        fpx::decode(w, inst);
        if (inst.sh.es == 16 || inst.dst_es == 16) continue;   // FP16: oracle has no FP16 arithmetic
        ++compared;
        const int before = testing::current_failures();
        CheckOptions opt;
        switch (inst.k) {
        case fpx::K::cvt_f2i: case fpx::K::fcmeq: case fpx::K::fcmge: case fpx::K::fcmgt: case fpx::K::facge:
        case fpx::K::facgt: case fpx::K::fcmeq0: case fpx::K::fcmge0: case fpx::K::fcmgt0: case fpx::K::fcmle0:
        case fpx::K::fcmlt0: case fpx::K::fmov_to_gpr: case fpx::K::fmov_from_gpr: case fpx::K::fmov_to_gpr_hi:
        case fpx::K::fmov_from_gpr_hi: case fpx::K::fcsel: case fpx::K::fmov_imm: case fpx::K::fmov_vimm:
            break;   // integer or bit-pattern results: exact
        default:
            opt.nan_es = inst.dst_es ? inst.dst_es : inst.sh.es;
        }
        opt.both_contexts = false;
        char label[64];
        std::snprintf(label, sizeof(label), "fuzz 0x%08x", w);
        check_word(w, ctx, opt, label);
        if (testing::current_failures() != before && ++mismatched_words > 10) break;
    }
    std::printf("    %u accepted, %u compared with the oracle\n", accepted, compared);
}
