#include "fx/translator/emit_neon_internal.hpp"
#include "fx/translator/emit_neon_test_util.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

std::array<std::uint32_t, 3> mask_sequence(unsigned d, unsigned n, unsigned m, unsigned shifts) {
    return {0x4e201c00u | (m << 16) | (n << 5) | d,
            0x6ea04400u | (shifts << 16) | (d << 5) | d,
            0x4eb1b800u | (d << 5) | d};
}

unsigned consumed(std::span<const std::uint32_t> words) {
    x64::Assembler as;
    testing::MemoryEmitContext ctx(as, 0x140001000);
    ctx.begin_instruction(0x140001000);
    return emit_neon(ctx, words);
}

struct KnobGuard {
    ~KnobGuard() { neon::neon_set_knobs_for_testing(-1); neon_set_feature_level(-1); }
};

} // namespace

FX_TEST(neon_fusion_movemask_consumes_only_safe_spans) {
    KnobGuard guard;
    neon_set_feature_level(2);
    if (!neon::features().avx2) return;
    neon::neon_set_knobs_for_testing(0);
    const auto words = mask_sequence(2, 2, 0, 1);
    FX_CHECK_EQ(consumed(words), 3u);
    FX_CHECK_EQ(consumed(std::span(words).first(2)), 1u);
    FX_CHECK_EQ(consumed(mask_sequence(2, 2, 0, 2)), 1u); // AND changes the subsequent shift counts.
    auto different_destination = words;
    different_destination[2] ^= 1u;
    FX_CHECK_EQ(consumed(different_destination), 1u); // Intermediate v2 remains guest-visible.
    neon::neon_set_knobs_for_testing(neon::neon_no_mask_fusion);
    FX_CHECK_EQ(consumed(words), 1u);
}

FX_TEST(neon_fusion_jrcxz_encoding_and_range) {
    x64::Assembler as;
    const auto start = as.bound_label(), forward = as.new_label();
    as.jrcxz_short(forward);
    as.jrcxz_short(start);
    as.bind(forward);
    as.finalize();
    const auto bytes = as.bytes();
    FX_REQUIRE(bytes.size() == 4);
    FX_CHECK_EQ(bytes[0], 0xe3);
    FX_CHECK_EQ(bytes[1], 2);
    FX_CHECK_EQ(bytes[2], 0xe3);
    FX_CHECK_EQ(bytes[3], 0xfc);
    x64::Assembler too_far;
    const auto far = too_far.new_label();
    too_far.jrcxz_short(far);
    for (unsigned i = 0; i < 128; ++i) too_far.db(0x90);
    too_far.bind(far);
    bool rejected = false;
    try { too_far.finalize(); } catch (const std::exception&) { rejected = true; }
    FX_CHECK(rejected);
}

FX_TEST(neon_fusion_movemask_fast_and_fallback) {
    KnobGuard guard;
    neon::neon_set_knobs_for_testing(0);
    // Source/destination and source/source aliases, including a mask overwritten
    // by AND. A shift-count/destination alias must use ordinary instructions.
    for (const auto regs : {std::array{2u, 2u, 0u, 1u}, std::array{2u, 0u, 2u, 1u},
                            std::array{2u, 3u, 0u, 1u}, std::array{2u, 3u, 3u, 1u},
                            std::array{2u, 2u, 0u, 0u}, std::array{2u, 2u, 0u, 2u}}) {
        const auto words = mask_sequence(regs[0], regs[1], regs[2], regs[3]);
        for (unsigned seed = 0; seed < 32; ++seed) {
            auto ctx = special_context(seed, 2);
            if (seed < 16) {
                ctx.v[regs[2]][0] = ctx.v[regs[2]][1] = 0x0000000100000001ull;
                ctx.v[regs[3]][0] = 0x0000000100000000ull;
                ctx.v[regs[3]][1] = 0x0000000300000002ull;
            }
            if (!check_sequence(emit_neon, ctx, words, "guarded movemask aliases")) return;
        }
    }
}

FX_TEST(neon_fusion_movemask_signed_low_byte_counts) {
    KnobGuard guard;
    neon::neon_set_knobs_for_testing(0);
    const auto words = mask_sequence(2, 2, 0, 1);
    for (unsigned count = 0; count < 256; ++count) {
        auto ctx = special_context(count + 1000, 2);
        // USHL uses a signed low byte, regardless of the upper 24 bits.
        ctx.v[1][0] = (0x87654300ull | count) | ((0x12345600ull | ((count + 1) & 255)) << 32);
        ctx.v[1][1] = (0xabcdef00ull | count) | ((0xffffff00ull | ((count + 127) & 255)) << 32);
        if (!check_sequence(emit_neon, ctx, words, "guarded movemask shift fallback")) return;
    }
}

namespace {
std::array<std::uint32_t, 4> ushr_sequence(unsigned d, unsigned n, unsigned shifts, unsigned wt) {
    return {0x6f210400u | (n << 5) | d,
            0x6ea04400u | (shifts << 16) | (d << 5) | d,
            0x4eb1b800u | (d << 5) | d,
            0x1e260000u | (d << 5) | wt};
}
} // namespace

FX_TEST(neon_fusion_ushr_movemask_spans) {
    KnobGuard guard;
    neon_set_feature_level(2);
    if (!neon::features().avx2) return;
    neon::neon_set_knobs_for_testing(0);
    const auto full = ushr_sequence(2, 3, 1, 9);
    FX_CHECK_EQ(consumed(full), 4u);
    FX_CHECK_EQ(consumed(std::span(full).first(3)), 3u);
    FX_CHECK_EQ(consumed(ushr_sequence(2, 3, 2, 9)), 1u);   // USHR writes the shift counts
    auto other_shift = ushr_sequence(2, 3, 1, 9);
    other_shift[0] = 0x6f220400u | (3u << 5) | 2u;            // USHR #30: not a sign-bit extraction
    FX_CHECK_EQ(consumed(other_shift), 1u);
    neon::neon_set_knobs_for_testing(neon::neon_no_mask_fusion);
    FX_CHECK_EQ(consumed(ushr_sequence(2, 3, 1, 9)), 1u);
}

// Every 4-bit lane mask (lanes all ones or zeros, as compares produce) and
// arbitrary lanes, through the inline-guarded short form and the general
// path, for both forms.
FX_TEST(neon_fusion_movemask_all_lane_masks) {
    KnobGuard guard;
    neon::neon_set_knobs_for_testing(0);
    for (const bool ushr : {false, true})
        for (unsigned mask = 0; mask < 16; ++mask)
            for (unsigned variant = 0; variant < 4; ++variant) {
                auto ctx = special_context(mask * 4 + variant + 7000, 2);
                for (unsigned lane = 0; lane < 4; ++lane) {
                    std::uint32_t value = ((mask >> lane) & 1u) ? 0xffffffffu : 0u;
                    if (variant == 3) value ^= 0x5a5a5a5au;   // arbitrary bits: sign and bit 0 differ
                    auto& half = ctx.v[3][lane / 2];
                    half = (half & ~(0xffffffffull << (32 * (lane & 1)))) | (std::uint64_t{value} << (32 * (lane & 1)));
                }
                ctx.v[0][0] = ctx.v[0][1] = variant == 1 ? 0x0000000300000001ull : 0x0000000100000001ull;
                ctx.v[1][0] = variant == 2 ? 0x0000000100000001ull : 0x0000000100000000ull;
                ctx.v[1][1] = 0x0000000300000002ull;
                // Without FMOV (an fp-family word: declined on the levels
                // without the fusion); the region tests cover the extraction.
                if (ushr) {
                    const auto words = ushr_sequence(2, 3, 1, 9);
                    if (!check_sequence(emit_neon, ctx, std::span(words).first(3), "ushr movemask lanes")) return;
                } else {
                    const auto words = mask_sequence(2, 3, 0, 1);
                    if (!check_sequence(emit_neon, ctx, words, "and movemask lanes")) return;
                }
            }
}

namespace {

// SMULL (acc = false) / SMLAL Vd.4S, Vn.4H, Vm.H[index] (q: SMULL2/SMLAL2 from .8H).
std::uint32_t smlal_elem(bool acc, bool q, unsigned d, unsigned n, unsigned m, unsigned index) {
    return (acc ? 0x0f402000u : 0x0f40a000u) | (q ? 1u << 30 : 0u) | (((index >> 1) & 1u) << 21) | ((index & 1u) << 20) |
           (m << 16) | ((index >> 2) << 11) | (n << 5) | d;
}

} // namespace

FX_TEST(neon_fusion_mull_pair_consumes_only_safe_pairs) {
    KnobGuard guard;
    neon::neon_set_knobs_for_testing(0);
    const std::uint32_t pair[] = {smlal_elem(false, false, 4, 1, 0, 0), smlal_elem(true, false, 4, 2, 0, 1)};
    FX_CHECK_EQ(consumed(pair), 2u);
    FX_CHECK_EQ(consumed(std::span(pair).first(1)), 1u);
    const std::uint32_t acc_pair[] = {smlal_elem(true, true, 4, 4, 0, 7), smlal_elem(true, true, 4, 1, 0, 3)};
    FX_CHECK_EQ(consumed(acc_pair), 2u);   // the first may read Vd
    const std::uint32_t reads_vd[] = {pair[0], smlal_elem(true, false, 4, 4, 0, 1)};
    FX_CHECK_EQ(consumed(reads_vd), 1u);   // the second reads the first's result
    const std::uint32_t elem_vd[] = {smlal_elem(false, false, 4, 1, 4, 0), smlal_elem(true, false, 4, 2, 4, 1)};
    FX_CHECK_EQ(consumed(elem_vd), 1u);
    const std::uint32_t mixed_q[] = {pair[0], smlal_elem(true, true, 4, 2, 0, 1)};
    FX_CHECK_EQ(consumed(mixed_q), 1u);
    const std::uint32_t other_d[] = {pair[0], smlal_elem(true, false, 5, 2, 0, 1)};
    FX_CHECK_EQ(consumed(other_d), 1u);
    const std::uint32_t other_m[] = {pair[0], smlal_elem(true, false, 4, 2, 3, 1)};
    FX_CHECK_EQ(consumed(other_m), 1u);
    const std::uint32_t umlal[] = {pair[0], pair[1] | 1u << 29};
    FX_CHECK_EQ(consumed(umlal), 1u);
    neon::neon_set_knobs_for_testing(neon::neon_no_mull_pair);
    FX_CHECK_EQ(consumed(pair), 1u);
}

FX_TEST(neon_fusion_mull_pair_matches_oracle) {
    KnobGuard guard;
    unsigned checked = 0;
    for (const int knobs : {0, static_cast<int>(neon::neon_no_mull_pair | neon::neon_old_mull_elem)}) {
        neon::neon_set_knobs_for_testing(knobs);
        std::uint64_t state = 0x5a11;
        for (unsigned k = 0; k < 400; ++k) {
            // An 8-tap chain (SMULL then seven SMLAL, the dav1d vertical filter
            // shape) with random registers, Q and element indexes; aliasing
            // the accumulator with sources keeps some pairs separate.
            const bool q = rnd(state) & 1;
            const unsigned d = rnd(state) % 6, m = rnd(state) % 6;
            std::vector<std::uint32_t> words;
            for (unsigned tap = 0; tap < 8; ++tap)
                words.push_back(smlal_elem(tap != 0, q, d, rnd(state) % 6, m, rnd(state) % 8));
            auto ctx = special_context(rnd(state), 1);
            if (k % 8 == 0)   // the PMADDWD wrap: every word 0x8000
                for (unsigned r = 0; r < 6; ++r) ctx.v[r][0] = ctx.v[r][1] = 0x8000800080008000ull;
            if (!check_sequence(emit_neon, ctx, words, "mull pair chain")) return;
            ++checked;
        }
    }
    std::printf("    %u chains\n", checked);
}
