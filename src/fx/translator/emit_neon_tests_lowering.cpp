// Differential tests for lowerings from the SIMD&FP cost audit (owner: neon):
// 64-bit vector results that skip the upper-half zeroing when every source
// is known zero above bit 64, and USHL/SSHL by register (vpsignd negation on
// S lanes, the AVX2 vpsllvq/vpsrlvq sequence on D lanes). Each runs with the
// new lowering and with TRANSMUTE_FX_NEON=nozero64,oldshl (knob override), at
// every host feature level and in every harness configuration, including the
// known-zero-upper context (claims derived from the start state).
#include "fx/translator/emit_neon_internal.hpp"
#include "fx/translator/emit_neon_test_util.hpp"

#include <cstdio>
#include <tuple>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

constexpr int kOld = neon::neon_no_zero64 | neon::neon_old_shl;

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
