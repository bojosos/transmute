// Differential tests for lowerings from the SIMD&FP cost audit (owner: neon):
// 64-bit vector results that skip the upper-half zeroing when every source
// is known zero above bit 64, and USHL/SSHL by register (vpsignd negation on
// S lanes, the AVX2 vpsllvq/vpsrlvq sequence on D lanes), and 16-bit lanes of
// every register shift through the AVX2 dword variable shifts, PMULL .8B by PMULLW. Each runs with the
// new lowering and with TRANSMUTE_FX_NEON=nozero64,oldshl,oldvarshift,oldpmull8 (knob override), at
// every host feature level and in every harness configuration, including the
// known-zero-upper context (claims derived from the start state).
#include "fx/translator/emit_neon_internal.hpp"
#include "fx/translator/emit_neon_test_util.hpp"

#include <cstdio>
#include <tuple>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

constexpr int kOld = neon::neon_no_zero64 | neon::neon_old_shl | neon::neon_old_var_shift | neon::neon_old_pmull8 |
                     neon::neon_old_mull_elem | neon::neon_old_widen_hi | neon::neon_old_rshl |
                     neon::neon_old_ushl8;

template <class F> void for_both_lowerings(F&& body) {
    for (const int bits : {0, kOld}) {
        neon::neon_set_knobs_for_testing(bits);
        body();
    }
    neon::neon_set_knobs_for_testing(-1);
}

// Boundary-biased lanes with the upper halves of most registers zero (as
// after LDR D / 64-bit vector results), so the known-zero claims apply.
GuestContext dform_context(std::uint64_t seed, unsigned size) {
    GuestContext ctx = special_context(seed, size);
    std::uint64_t state = seed ^ 0x5555;
    for (unsigned r = 0; r < 32; ++r)
        if (rnd(state) % 4 != 0) ctx.v[r][1] = 0;
    return ctx;
}

bool run_word(std::uint32_t word, const GuestContext& ctx, const char* label) {
    const auto outcome = check(emit_neon, ctx, word, label);
    if (outcome == Outcome::mismatch) return false;
    return true;
}

} // namespace

FX_TEST(neon_lowering_dform_known_zero_upper) {
    unsigned words = 0;
    for_both_lowerings([&] {
        std::uint64_t state = 77;
        // Three same (integer) with Q = 0: every opcode and size.
        for (unsigned opc = 0; opc < 0x18; ++opc)
            for (unsigned u = 0; u < 2; ++u)
                for (unsigned size = 0; size < 4; ++size)
                    for (unsigned s = 0; s < 3; ++s) {
                        const unsigned rd = rnd(state) % 8, rn = rnd(state) % 8, rm = rnd(state) % 8;
                        const std::uint32_t w = 0x0e200400u | (u << 29) | (size << 22) | (rm << 16) | (opc << 11) |
                                                (rn << 5) | rd;
                        if (!run_word(w, dform_context(rnd(state), size < 3 ? size : 2), "three_same 64-bit")) return;
                        ++words;
                    }
        // Shifts by immediate with Q = 0 (non-narrowing, non-widening).
        for (unsigned opc : {0x00u, 0x02u, 0x04u, 0x06u, 0x08u, 0x0au, 0x0cu, 0x0eu})
            for (unsigned u = 0; u < 2; ++u)
                for (unsigned immh = 1; immh < 8; ++immh)
                    for (unsigned s = 0; s < 3; ++s) {
                        const unsigned immb = rnd(state) % 8, rd = rnd(state) % 8, rn = rnd(state) % 8;
                        const std::uint32_t w = 0x0f000400u | (u << 29) | (immh << 19) | (immb << 16) | (opc << 11) |
                                                (rn << 5) | rd;
                        const unsigned size = immh >= 4 ? 2 : immh >= 2 ? 1 : 0;
                        if (!run_word(w, dform_context(rnd(state), size), "shift_imm 64-bit")) return;
                        ++words;
                    }
    });
    std::printf("    %u words\n", words);
    print_stats("after dform_known_zero_upper");
}

FX_TEST(neon_lowering_register_shift_all_counts) {
    unsigned words = 0;
    for_both_lowerings([&] {
        for (unsigned u = 0; u < 2; ++u) {
            // USHL/SSHL .2S/.4S/.2D and scalar D: every count byte (-128..127)
            // appears in some lane, on boundary-biased values.
            struct Form { std::uint32_t base; unsigned size, lanes; };
            const Form forms[] = {{0x0ea04400u, 2, 2}, {0x4ea04400u, 2, 4}, {0x4ee04400u, 3, 2}, {0x5ee04400u, 3, 1}};
            for (const auto& f : forms) {
                std::uint64_t state = f.base + u;
                for (unsigned c0 = 0; c0 < 256; c0 += f.lanes) {
                    for (const auto [rd, rn, rm] : {std::tuple{0u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u}}) {
                        auto ctx = special_context(rnd(state), f.size);
                        auto* bytes = reinterpret_cast<std::uint8_t*>(ctx.v[rm]);
                        for (unsigned l = 0; l < 16u >> f.size; ++l)
                            bytes[l << f.size] = static_cast<std::uint8_t>(c0 + l % f.lanes);
                        if (rnd(state) % 2) ctx.v[rn][1] = 0;
                        const std::uint32_t w = f.base | (u << 29) | (rm << 16) | (rn << 5) | rd;
                        if (!run_word(w, ctx, "ushl/sshl counts")) return;
                        ++words;
                    }
                }
            }
        }
    });
    std::printf("    %u words\n", words);
    print_stats("after register_shift_all_counts");
}

FX_TEST(neon_lowering_register_shift_h_lanes) {
    unsigned words = 0;
    for_both_lowerings([&] {
        // SSHL/USHL, SQSHL/UQSHL, SRSHL/URSHL, SQRSHL/UQRSHL on .4H, .8H and
        // scalar H (saturating forms): every count byte (-128..127) in some
        // lane, garbage in the count's high bytes, boundary-biased values.
        for (unsigned opc = 0x08; opc <= 0x0b; ++opc)
            for (unsigned u = 0; u < 2; ++u)
                for (const std::uint32_t base : {0x0e600400u, 0x4e600400u, 0x5e600400u}) {
                    if (base == 0x5e600400u && (opc & 1) == 0) continue;   // scalar H: saturating forms only
                    const unsigned lanes = base == 0x0e600400u ? 4 : 8;
                    std::uint64_t state = base + opc * 2 + u;
                    for (unsigned c0 = 0; c0 < 256; c0 += lanes) {
                        for (const auto [rd, rn, rm] : {std::tuple{0u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u},
                                                        std::tuple{3u, 2u, 2u}}) {
                            auto ctx = special_context(rnd(state), 1);
                            auto* bytes = reinterpret_cast<std::uint8_t*>(ctx.v[rm]);
                            for (unsigned l = 0; l < 8; ++l) bytes[2 * l] = static_cast<std::uint8_t>(c0 + l % lanes);
                            if (rnd(state) % 2) ctx.v[rn][1] = 0;
                            const std::uint32_t w = base | (u << 29) | (rm << 16) | (opc << 11) | (rn << 5) | rd;
                            if (!run_word(w, ctx, "h-lane register shift")) return;
                            ++words;
                        }
                    }
                }
    });
    std::printf("    %u words\n", words);
    print_stats("after register_shift_h_lanes");
}

FX_TEST(neon_lowering_register_shift_every_form_all_counts) {
    unsigned words = 0;
    for_both_lowerings([&] {
        // SSHL/USHL, SQSHL/UQSHL, SRSHL/URSHL, SQRSHL/UQRSHL on every arrangement
        // and the scalar forms: every count byte (-128..127) in some lane, garbage
        // in the count's other bytes, boundary-biased values.
        for (unsigned opc = 0x08; opc <= 0x0b; ++opc)
            for (unsigned u = 0; u < 2; ++u)
                for (unsigned size = 0; size < 4; ++size)
                    for (unsigned form = 0; form < 3; ++form) {   // Q = 0, Q = 1, scalar
                        if (size == 3 && form == 0) continue;     // .1D is reserved
                        if (form == 2 && size != 3 && (opc & 1) == 0) continue;   // scalar B/H/S: saturating forms only
                        const std::uint32_t base = form == 2 ? 0x5e200400u : form == 1 ? 0x4e200400u : 0x0e200400u;
                        const unsigned lanes = form == 2 ? 1 : (form == 1 ? 16u : 8u) >> size;
                        std::uint64_t state = base + (opc << 4) + (size << 1) + u;
                        for (unsigned c0 = 0; c0 < 256; c0 += lanes) {
                            static constexpr std::tuple<unsigned, unsigned, unsigned> kRegs[] = {{0, 1, 2}, {1, 1, 2}, {2, 1, 2}};
                            {   // one register assignment per count group, rotating (keeps the test short)
                                const auto [rd, rn, rm] = kRegs[(c0 / lanes) % 3];
                                auto ctx = special_context(rnd(state), size);
                                auto* bytes = reinterpret_cast<std::uint8_t*>(ctx.v[rm]);
                                for (unsigned l = 0; l < 16u >> size; ++l)
                                    bytes[l << size] = static_cast<std::uint8_t>(c0 + l % lanes);
                                if (rnd(state) % 2) ctx.v[rn][1] = 0;
                                const std::uint32_t w =
                                    base | (u << 29) | (size << 22) | (rm << 16) | (opc << 11) | (rn << 5) | rd;
                                if (!run_word(w, ctx, "register shift, every form")) return;
                                ++words;
                            }
                        }
                    }
    });
    std::printf("    %u words\n", words);
    print_stats("after register_shift_every_form_all_counts");
}

FX_TEST(neon_lowering_long_multiply_by_element_h) {
    unsigned words = 0;
    for_both_lowerings([&] {
        // [SU]MULL/[SU]MLAL/[SU]MLSL and SQDMULL/SQDMLAL/SQDMLSL by element from
        // .4H/.8H: every index, the element register aliasing the source and
        // the destination, boundary-biased values (0x8000 * 0x8000 included).
        std::uint64_t state = 0xe1e;
        for (const unsigned opc : {0x2u, 0x3u, 0x6u, 0x7u, 0xau, 0xbu})
            for (unsigned u = 0; u < 2; ++u) {
                if (u && (opc & 1)) continue;   // SQDMULL forms are signed only
                for (unsigned q = 0; q < 2; ++q)
                    for (unsigned index = 0; index < 8; ++index)
                        for (const auto [rd, rn, rm] : {std::tuple{0u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u},
                                                        std::tuple{3u, 2u, 2u}}) {
                            const std::uint32_t w = 0x0f400000u | (q << 30) | (u << 29) | (((index >> 1) & 1u) << 21) |
                                                    ((index & 1u) << 20) | (rm << 16) | (opc << 12) | ((index >> 2) << 11) |
                                                    (rn << 5) | rd;
                            for (unsigned s = 0; s < 4; ++s) {
                                auto ctx = special_context(rnd(state), 1);
                                if (s == 0) {   // the PMADDWD edge: every word 0x8000
                                    ctx.v[rn][0] = ctx.v[rn][1] = ctx.v[rm][0] = ctx.v[rm][1] = 0x8000800080008000ull;
                                }
                                if (!run_word(w, ctx, "long multiply by element")) return;
                                ++words;
                            }
                        }
            }
    });
    std::printf("    %u words\n", words);
    print_stats("after long_multiply_by_element_h");
}

FX_TEST(neon_lowering_unsigned_widen_high) {
    unsigned words = 0;
    for_both_lowerings([&] {
        // The unsigned "2" forms widen the source's high half: UADDL2/UADDW2/
        // USUBL2/USUBW2/UABAL2/UABDL2/UMLAL2/UMLSL2/UMULL2 for every size, and
        // USHLL2 (UXTL2) with every shift; aliasing destination and sources.
        std::uint64_t state = 0x71d3;
        for (const unsigned opc : {0x0u, 0x1u, 0x2u, 0x3u, 0x5u, 0x7u, 0x8u, 0xau, 0xcu})
            for (unsigned size = 0; size < 3; ++size)
                for (const auto [rd, rn, rm] : {std::tuple{0u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u},
                                                std::tuple{3u, 4u, 4u}})
                    for (unsigned s = 0; s < 6; ++s) {
                        const std::uint32_t w = 0x6e200000u | (size << 22) | (rm << 16) | (opc << 12) | (rn << 5) | rd;
                        if (!run_word(w, special_context(rnd(state), size), "unsigned widen high")) return;
                        ++words;
                    }
        for (unsigned size = 0; size < 3; ++size)
            for (unsigned shift = 0; shift < (8u << size); ++shift)
                for (const auto [rd, rn] : {std::pair{0u, 1u}, std::pair{1u, 1u}}) {
                    const std::uint32_t immhb = (8u << size) | shift;
                    const std::uint32_t w = 0x6f00a400u | (immhb << 16) | (rn << 5) | rd;
                    if (!run_word(w, special_context(rnd(state), size), "ushll2")) return;
                    ++words;
                }
    });
    std::printf("    %u words\n", words);
    print_stats("after unsigned_widen_high");
}

FX_TEST(neon_lowering_pmull8) {
    unsigned words = 0;
    for_both_lowerings([&] {
        // PMULL and PMULL2 .8H from .8B/.16B: random and boundary bytes, aliasing.
        std::uint64_t state = 0x9e11;
        for (const std::uint32_t base : {0x0e20e000u, 0x4e20e000u})
            for (unsigned s = 0; s < 64; ++s)
                for (const auto [rd, rn, rm] : {std::tuple{0u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u},
                                                std::tuple{3u, 4u, 4u}}) {
                    auto ctx = special_context(rnd(state), 0);
                    if (s < 16) {   // every byte value against every single bit and 0xff
                        auto* bn = reinterpret_cast<std::uint8_t*>(ctx.v[rn]);
                        auto* bm = reinterpret_cast<std::uint8_t*>(ctx.v[rm]);
                        for (unsigned l = 0; l < 16; ++l) {
                            bn[l] = static_cast<std::uint8_t>(s * 16 + l);
                            bm[l] = static_cast<std::uint8_t>(l < 8 ? 1u << l : 0xffu - (l & 3));
                        }
                    }
                    const std::uint32_t w = base | (rm << 16) | (rn << 5) | rd;
                    if (!run_word(w, ctx, "pmull .8b")) return;
                    ++words;
                }
    });
    std::printf("    %u words\n", words);
    print_stats("after pmull8");
}
