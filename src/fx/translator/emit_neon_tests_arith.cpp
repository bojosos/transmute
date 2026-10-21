// Differential tests: integer SIMD arithmetic groups (owner: neon).
#include "fx/translator/emit_neon_test_util.hpp"

#include "fx/common/cpu_features.hpp"

#include <cstdio>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

struct Regs {
    unsigned d, n, m;
};

// Random registers with frequent aliasing (rd == rn, rd == rm, rn == rm).
Regs pick_regs(std::uint64_t& state) {
    Regs r{static_cast<unsigned>(rnd(state) % 32), static_cast<unsigned>(rnd(state) % 32),
           static_cast<unsigned>(rnd(state) % 32)};
    switch (rnd(state) % 8) {
    case 0: r.n = r.d; break;
    case 1: r.m = r.d; break;
    case 2: r.m = r.n; break;
    case 3: r.n = r.m = r.d; break;
    default: break;
    }
    return r;
}

enum Fields : unsigned { kD = 1, kN = 2, kM = 4, kDNM = 7, kDN = 3, kM4 = 8 };

std::uint32_t with_regs(std::uint32_t base, const Regs& r, unsigned fields) {
    std::uint32_t w = base;
    if (fields & kD) w |= r.d;
    if (fields & kN) w |= r.n << 5;
    if (fields & kM) w |= r.m << 16;
    if (fields & kM4) w |= (r.m & 15) << 16;
    return w;
}

// Checks `base` (with random registers) for `seeds` random states. A decline
// is a failure when `expect_valid` (the architecture defines the encoding).
void sweep(std::uint32_t base, unsigned size, unsigned seeds, const char* label, std::uint64_t salt,
           unsigned fields = kDNM, bool expect_valid = true) {
    std::uint64_t state = base * 0x9e3779b97f4a7c15ULL + salt;
    for (unsigned s = 0; s < seeds; ++s) {
        const Regs r = pick_regs(state);
        const std::uint32_t word = with_regs(base, r, fields);
        const auto ctx = special_context(rnd(state), size);
        const auto outcome = check(emit_neon, ctx, word, label);
        if (outcome == Outcome::declined || outcome == Outcome::both_declined) {
            if (expect_valid) {
                char buffer[112];
                std::snprintf(buffer, sizeof(buffer), "%s 0x%08x: valid encoding declined (oracle %s)", label,
                              word, outcome == Outcome::declined ? "implements it" : "declines too");
                testing::report_failure(__FILE__, __LINE__, buffer);
            }
            return;
        }
        if (outcome != Outcome::ok) return;   // mismatch reported; oracle-missing: one probe is enough
    }
}

bool d64_ok(unsigned size, unsigned q) { return size != 3 || q; }

bool three_same_valid(unsigned opc, unsigned u, unsigned size, unsigned q) {
    switch (opc) {
    case 0x00: case 0x02: case 0x04: case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x12:
    case 0x14: case 0x15:
        return size != 3;
    case 0x03: return true;
    case 0x13: return u ? size == 0 : size != 3;
    case 0x16: return size == 1 || size == 2;
    case 0x17: return !u && d64_ok(size, q);
    default: return d64_ok(size, q);
    }
}

bool three_different_valid(unsigned opc, unsigned u, unsigned size) {
    if (size == 3) return false;   // PMULL 1Q (crypto) is checked separately
    if (opc == 0xe) return !u && size == 0;
    if (opc == 0x9 || opc == 0xb || opc == 0xd) return !u && size != 0;
    return true;
}

bool two_reg_misc_valid(unsigned opc, unsigned u, unsigned size, unsigned q) {
    switch (opc) {
    case 0x00: return u ? size <= 1 : size <= 2;
    case 0x01: return !u && size == 0;
    case 0x02: case 0x04: case 0x06: case 0x12: case 0x14: return size != 3;
    case 0x05: return u ? size <= 1 : size == 0;
    case 0x0a: return !u && d64_ok(size, q);
    case 0x13: return u && size != 3;
    case 0x1c: return size == 2;   // URECPE/URSQRTE; other sizes are FP
    default: return d64_ok(size, q);
    }
}

bool scalar_three_same_valid(unsigned opc, unsigned size) {
    switch (opc) {
    case 0x01: case 0x05: case 0x09: case 0x0b: return true;
    case 0x16: return size == 1 || size == 2;
    default: return size == 3;
    }
}

bool scalar_two_reg_misc_valid(unsigned opc, unsigned u, unsigned size) {
    switch (opc) {
    case 0x03: case 0x07: return true;
    case 0x0a: return !u && size == 3;
    case 0x12: return u && size != 3;
    case 0x14: return size != 3;
    default: return size == 3;
    }
}

} // namespace

FX_TEST(neon_three_same) {
    for (unsigned opc = 0; opc < 0x18; ++opc) {
        if (opc >= 0x08 && opc <= 0x0b) continue;   // register shifts: emit_neon_tests_shift.cpp
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned q = 0; q < 2; ++q)
                    sweep(0x0e200400u | (q << 30) | (u << 29) | (size << 22) | (opc << 11), size, 12,
                          "three_same", opc, kDNM, three_same_valid(opc, u, size, q));
    }
    print_stats("after three_same");
}

FX_TEST(neon_three_different) {
    for (unsigned opc = 0; opc < 0xf; ++opc)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned q = 0; q < 2; ++q)
                    sweep(0x0e200000u | (q << 30) | (u << 29) | (size << 22) | (opc << 12), size, 12,
                          "three_different", opc, kDNM, three_different_valid(opc, u, size));
    print_stats("after three_different");
}

FX_TEST(neon_two_reg_misc) {
    static constexpr unsigned opcodes[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                           0x09, 0x0a, 0x0b, 0x12, 0x13, 0x14, 0x1c};
    for (const unsigned opc : opcodes)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned q = 0; q < 2; ++q)
                    sweep(0x0e200800u | (q << 30) | (u << 29) | (size << 22) | (opc << 12), size, 12,
                          "two_reg_misc", opc, kDN, two_reg_misc_valid(opc, u, size, q));
    print_stats("after two_reg_misc");
}

FX_TEST(neon_across_lanes) {
    static constexpr unsigned opcodes[] = {0x03, 0x0a, 0x1a, 0x1b};
    for (const unsigned opc : opcodes)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned q = 0; q < 2; ++q) {
                    const bool valid = size != 3 && (size != 2 || q) && (opc != 0x1b || !u);
                    sweep(0x0e300800u | (q << 30) | (u << 29) | (size << 22) | (opc << 12), size, 16,
                          "across_lanes", opc, kDN, valid);
                }
    print_stats("after across_lanes");
}

FX_TEST(neon_indexed) {
    // Integer by-element encodings (U, opcode); FP and RDM forms are excluded.
    struct Form { unsigned u, opc; };
    static constexpr Form forms[] = {
        {1, 0x0}, {1, 0x4}, {0, 0x8},                                 // MLA MLS MUL
        {0, 0x2}, {1, 0x2}, {0, 0x6}, {1, 0x6}, {0, 0xa}, {1, 0xa},   // [SU]MLAL [SU]MLSL [SU]MULL
        {0, 0x3}, {0, 0x7}, {0, 0xb}, {0, 0xc}, {0, 0xd},             // SQDMLAL SQDMLSL SQDMULL SQ(R)DMULH
        {1, 0xd}, {1, 0xf},                                           // SQRDMLAH SQRDMLSH (RDM)
    };
    for (const auto f : forms)
        for (unsigned size = 1; size < 3; ++size)
            for (unsigned q = 0; q < 2; ++q)
                for (unsigned hlm = 0; hlm < 8; ++hlm) {
                    const unsigned h = hlm >> 2, l = (hlm >> 1) & 1, m = hlm & 1;
                    if (size == 2 && m) continue;   // M is part of Rm for S
                    sweep(0x0f000000u | (q << 30) | (f.u << 29) | (size << 22) | (l << 21) | (m << 20) |
                              (f.opc << 12) | (h << 11),
                          size, 6, "indexed", f.opc, size == 1 ? (kDN | kM4) : kDNM);
                }
    print_stats("after indexed");
}

FX_TEST(neon_dot_product) {
    for (unsigned u = 0; u < 2; ++u)
        for (unsigned q = 0; q < 2; ++q) {
            sweep(0x0e809400u | (q << 30) | (u << 29), 0, 24, "dot", 1);
            if (!u) {
                sweep(0x0e809c00u | (q << 30), 0, 24, "usdot", 3);   // USDOT (vector)
                for (unsigned hl = 0; hl < 4; ++hl)
                    for (const unsigned sz : {0u, 2u})                // SUDOT / USDOT by element
                        sweep(0x0f00f000u | (q << 30) | (sz << 22) | ((hl & 1) << 21) | ((hl >> 1) << 11), 0, 12,
                              "sudot_usdot_indexed", 4);
            }
            for (unsigned hl = 0; hl < 4; ++hl)
                sweep(0x0f80e000u | (q << 30) | (u << 29) | ((hl & 1) << 21) | ((hl >> 1) << 11), 0, 12,
                      "dot_indexed", 2);
        }
    print_stats("after dot");
}

FX_TEST(neon_scalar_three_same) {
    static constexpr unsigned opcodes[] = {0x01, 0x05, 0x06, 0x07, 0x10, 0x11, 0x16};
    for (const unsigned opc : opcodes)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                sweep(0x5e200400u | (u << 29) | (size << 22) | (opc << 11), size, 16, "scalar_three_same", opc,
                      kDNM, scalar_three_same_valid(opc, size));
    print_stats("after scalar_three_same");
}

FX_TEST(neon_scalar_three_different) {
    for (const unsigned opc : {0x9u, 0xbu, 0xdu})
        for (unsigned size = 0; size < 4; ++size)
            sweep(0x5e200000u | (size << 22) | (opc << 12), size, 16, "scalar_three_different", opc, kDNM,
                  size == 1 || size == 2);
    print_stats("after scalar_three_different");
}

FX_TEST(neon_scalar_two_reg_misc) {
    static constexpr unsigned opcodes[] = {0x03, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x12, 0x14};
    for (const unsigned opc : opcodes)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                sweep(0x5e200800u | (u << 29) | (size << 22) | (opc << 12), size, 16, "scalar_two_reg_misc",
                      opc, kDN, scalar_two_reg_misc_valid(opc, u, size));
    print_stats("after scalar_two_reg_misc");
}

FX_TEST(neon_scalar_pairwise_and_indexed) {
    sweep(0x5ef1b800u, 3, 24, "scalar_addp", 0, kDN);
    struct Form { unsigned u, opc; };
    static constexpr Form forms[] = {{0, 0x3}, {0, 0x7}, {0, 0xb}, {0, 0xc}, {0, 0xd}, {1, 0xd}, {1, 0xf}};
    for (const auto f : forms)
        for (unsigned size = 1; size < 3; ++size)
            for (unsigned hlm = 0; hlm < 8; ++hlm) {
                const unsigned h = hlm >> 2, l = (hlm >> 1) & 1, m = hlm & 1;
                if (size == 2 && m) continue;
                sweep(0x5f000000u | (f.u << 29) | (size << 22) | (l << 21) | (m << 20) | (f.opc << 12) | (h << 11),
                      size, 6, "scalar_indexed", f.opc, size == 1 ? (kDN | kM4) : kDNM);
            }
    print_stats("after scalar_indexed");
}

FX_TEST(neon_pmull_64) {
    // PMULL{2} Vd.1Q, Vn.1D, Vm.1D (also claimed by the system/crypto family).
    for (unsigned q = 0; q < 2; ++q)
        sweep(0x0ee0e000u | (q << 30), 3, 32, "pmull64", 1, kDNM, cpu_features().pclmul);
    print_stats("after pmull64");
}

FX_TEST(neon_rdm_and_matrix) {
    for (unsigned opc = 0; opc < 2; ++opc)
        for (unsigned size = 0; size < 4; ++size) {
            const bool valid = size == 1 || size == 2;
            for (unsigned q = 0; q < 2; ++q)
                sweep(0x2e008400u | (q << 30) | (size << 22) | (opc << 11), size, 16, "sqrdmlah", 5, kDNM, valid);
            sweep(0x7e008400u | (size << 22) | (opc << 11), size, 16, "sqrdmlah_scalar", 6, kDNM, valid);
        }
    for (const std::uint32_t base : {0x4e80a400u, 0x6e80a400u, 0x4e80ac00u})   // SMMLA UMMLA USMMLA
        sweep(base, 0, 32, "mmla", 7);
    print_stats("after rdm/mmla");
}
