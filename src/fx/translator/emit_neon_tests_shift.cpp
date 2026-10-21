// Differential tests: shifts by immediate and by register (owner: neon).
#include "fx/translator/emit_neon_test_util.hpp"

#include <bit>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

void report_declined(const char* label, std::uint32_t word, Outcome outcome) {
    char buffer[112];
    std::snprintf(buffer, sizeof(buffer), "%s 0x%08x: valid encoding declined (oracle %s)", label, word,
                  outcome == Outcome::declined ? "implements it" : "declines too");
    testing::report_failure(__FILE__, __LINE__, buffer);
}

// Runs `seeds` random states for `base` with random rd/rn.
// C++ reference for the saturating narrowing shifts (SQSHRN, SQRSHRN,
// UQSHRN, UQRSHRN, SQSHRUN, SQRSHRUN; vector and scalar). The oracle
// overflows int64 when rounding a 64-bit all-ones source lane by 1.
bool is_saturating_narrow(std::uint32_t w) {
    const unsigned opc = (w >> 11) & 31, u = (w >> 29) & 1;
    return (opc == 0x12 || opc == 0x13) || ((opc == 0x10 || opc == 0x11) && u);
}

void reference_saturating_narrow(GuestContext& c, std::uint32_t w) {
    const bool scalar = ((w >> 28) & 1) != 0, q = ((w >> 30) & 1) != 0, u = ((w >> 29) & 1) != 0;
    const unsigned immh = (w >> 19) & 15, immb = (w >> 16) & 7, opc = (w >> 11) & 31;
    const unsigned rn = (w >> 5) & 31, rd = w & 31;
    const unsigned dsize = static_cast<unsigned>(std::bit_width(immh)) - 1;
    const unsigned dbits = 8u << dsize, sbits = 2 * dbits;
    const unsigned amount = sbits - ((immh << 3) | immb);
    const bool rounding = (opc & 1) != 0;
    const bool signed_source = !(u && (opc == 0x12 || opc == 0x13));
    const bool unsigned_dest = u;
    const std::uint64_t smask = sbits == 64 ? ~0ULL : (1ULL << sbits) - 1;
    const std::uint64_t dmask = (1ULL << dbits) - 1;
    const auto* src = reinterpret_cast<const std::uint8_t*>(c.v[rn]);
    std::uint64_t out[2] = {0, 0};
    if (!scalar && q) out[0] = c.v[rd][0];
    const unsigned lanes = scalar ? 1 : 64 / dbits;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        std::uint64_t raw = 0;
        std::memcpy(&raw, src + lane * (sbits / 8), sbits / 8);
        raw &= smask;
        std::int64_t lo, hi;
        if (unsigned_dest) { lo = 0; hi = static_cast<std::int64_t>(dmask); }
        else { lo = -(1LL << (dbits - 1)); hi = (1LL << (dbits - 1)) - 1; }
        std::uint64_t result;
        if (signed_source) {
            const std::int64_t value = static_cast<std::int64_t>(raw << (64 - sbits)) >> (64 - sbits);
            std::int64_t shifted = value >> amount;
            if (rounding) shifted += (value >> (amount - 1)) & 1;
            result = static_cast<std::uint64_t>(std::clamp(shifted, lo, hi)) & dmask;
        } else {
            std::uint64_t shifted = raw >> amount;
            if (rounding) shifted += (raw >> (amount - 1)) & 1;
            result = std::min<std::uint64_t>(shifted, dmask);
        }
        const unsigned bitpos = (q && !scalar ? 64 : 0) + lane * dbits;
        out[bitpos / 64] |= result << (bitpos % 64);
    }
    c.v[rd][0] = out[0];
    c.v[rd][1] = out[1];
}

unsigned oracle_disagreements = 0;

void sweep_dn(std::uint32_t base, unsigned size, unsigned seeds, const char* label, bool expect_valid) {
    std::uint64_t state = base * 0x2545f4914f6cdd1dULL + 99;
    for (unsigned s = 0; s < seeds; ++s) {
        unsigned rd = static_cast<unsigned>(rnd(state) % 32), rn = static_cast<unsigned>(rnd(state) % 32);
        if (rnd(state) % 4 == 0) rn = rd;
        const std::uint32_t word = base | (rn << 5) | rd;
        const auto ctx = special_context(rnd(state), size);
        if (expect_valid && is_saturating_narrow(word)) {
            GuestContext oracle = ctx, reference = ctx;
            const bool oracle_ok = testing::oracle_step(oracle, word);
            reference_saturating_narrow(reference, word);
            reference.pc += 4;
            if (!oracle_ok || std::memcmp(oracle.v, reference.v, sizeof(oracle.v)) != 0) {
                if (oracle_disagreements++ == 0)
                    std::printf("[neon] oracle disagrees with the C++ reference for 0x%08x (saturating narrow)\n", word);
                const auto reference_fn = [word](GuestContext& c) { reference_saturating_narrow(c, word); };
                if (check_with(emit_neon, ctx, word, label, reference_fn) != Outcome::ok) return;
                continue;
            }
        }
        const auto outcome = check(emit_neon, ctx, word, label);
        if (outcome == Outcome::declined || outcome == Outcome::both_declined) {
            if (expect_valid) report_declined(label, word, outcome);
            return;
        }
        if (outcome != Outcome::ok) return;
    }
}

bool vector_shift_valid(unsigned opc, unsigned u, unsigned size, unsigned q) {
    switch (opc) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x0a: case 0x0e: return size != 3 || q;
    case 0x08: case 0x0c: return u && (size != 3 || q);
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: return size != 3;
    default: return false;
    }
}

bool scalar_shift_valid(unsigned opc, unsigned u, unsigned size) {
    switch (opc) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x0a: return size == 3;
    case 0x08: return u && size == 3;
    case 0x0c: return u;
    case 0x0e: return true;
    case 0x10: case 0x11: return u && size != 3;
    case 0x12: case 0x13: return size != 3;
    default: return false;
    }
}

constexpr unsigned kShiftOpcodes[] = {0x00, 0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e,
                                      0x10, 0x11, 0x12, 0x13, 0x14};

// Fills the low byte of every lane of v[reg] with an interesting shift count.
void set_counts(GuestContext& ctx, unsigned reg, unsigned size, std::uint64_t& state) {
    const int esize = 8 << size;
    const unsigned lanes = 16u >> size;
    auto* bytes = reinterpret_cast<std::uint8_t*>(ctx.v[reg]);
    for (unsigned lane = 0; lane < lanes; ++lane) {
        int count;
        switch (rnd(state) % 6) {
        case 0: count = static_cast<int>(rnd(state) % 256) - 128; break;
        case 1: count = (rnd(state) & 1) ? esize : -esize; break;
        case 2: count = (rnd(state) & 1) ? esize + 1 : -esize - 1; break;
        default: count = static_cast<int>(rnd(state) % (2 * esize + 5)) - esize - 2; break;
        }
        bytes[lane << size] = static_cast<std::uint8_t>(count);
    }
}

} // namespace

FX_TEST(neon_shift_imm_vector) {
    for (const unsigned opc : kShiftOpcodes)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned q = 0; q < 2; ++q)
                for (unsigned immh = 1; immh < 16; ++immh)
                    for (unsigned immb = 0; immb < 8; ++immb) {
                        const unsigned size = static_cast<unsigned>(std::bit_width(immh)) - 1;
                        const unsigned element = opc >= 0x10 && opc <= 0x13 ? size + 1 : size;
                        sweep_dn(0x0f000400u | (q << 30) | (u << 29) | (immh << 19) | (immb << 16) | (opc << 11),
                                 element > 3 ? 3 : element, 2, "shift_imm", vector_shift_valid(opc, u, size, q));
                    }
    print_stats("after shift_imm_vector");
}

FX_TEST(neon_shift_imm_scalar) {
    for (const unsigned opc : kShiftOpcodes)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned immh = 1; immh < 16; ++immh)
                for (unsigned immb = 0; immb < 8; ++immb) {
                    if (opc == 0x14) continue;
                    const unsigned size = static_cast<unsigned>(std::bit_width(immh)) - 1;
                    const unsigned element = opc >= 0x10 && opc <= 0x13 ? size + 1 : size;
                    sweep_dn(0x5f000400u | (u << 29) | (immh << 19) | (immb << 16) | (opc << 11),
                             element > 3 ? 3 : element, 3, "scalar_shift_imm", scalar_shift_valid(opc, u, size));
                }
    print_stats("after shift_imm_scalar");
    std::printf("[neon] saturating-narrow words checked against the C++ reference: %u\n", oracle_disagreements);
}

FX_TEST(neon_shift_register) {
    for (unsigned opc = 0x08; opc <= 0x0b; ++opc)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned q = 0; q < 2; ++q) {
                    const std::uint32_t base = 0x0e200400u | (q << 30) | (u << 29) | (size << 22) | (opc << 11);
                    std::uint64_t state = base + 7;
                    for (unsigned s = 0; s < 24; ++s) {
                        unsigned rd = static_cast<unsigned>(rnd(state) % 32);
                        unsigned rn = static_cast<unsigned>(rnd(state) % 32);
                        unsigned rm = static_cast<unsigned>(rnd(state) % 32);
                        if (rnd(state) % 4 == 0) rn = rd;
                        if (rnd(state) % 8 == 0) rm = rd;
                        auto ctx = special_context(rnd(state), size);
                        set_counts(ctx, rm, size, state);
                        const std::uint32_t word = base | (rm << 16) | (rn << 5) | rd;
                        if (std::getenv("NEON_TRACE")) { std::printf("word %08x\n", word); std::fflush(stdout); }
                        const auto outcome = check(emit_neon, ctx, word, "shift_register");
                        if (outcome == Outcome::declined || outcome == Outcome::both_declined) {
                            if (size != 3 || q) report_declined("shift_register", word, outcome);
                            break;
                        }
                        if (outcome != Outcome::ok) break;
                    }
                }
    print_stats("after shift_register");
}

FX_TEST(neon_shift_register_scalar) {
    for (unsigned opc = 0x08; opc <= 0x0b; ++opc)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 4; ++size) {
                const std::uint32_t base = 0x5e200400u | (u << 29) | (size << 22) | (opc << 11);
                const bool valid = size == 3 || (opc & 1);
                std::uint64_t state = base + 11;
                for (unsigned s = 0; s < 32; ++s) {
                    const unsigned rd = static_cast<unsigned>(rnd(state) % 32);
                    const unsigned rn = static_cast<unsigned>(rnd(state) % 32);
                    const unsigned rm = static_cast<unsigned>(rnd(state) % 32);
                    auto ctx = special_context(rnd(state), size);
                    set_counts(ctx, rm, size, state);
                    const std::uint32_t word = base | (rm << 16) | (rn << 5) | rd;
                    const auto outcome = check(emit_neon, ctx, word, "scalar_shift_register");
                    if (outcome == Outcome::declined || outcome == Outcome::both_declined) {
                        if (valid) report_declined("scalar_shift_register", word, outcome);
                        break;
                    }
                    if (outcome != Outcome::ok) break;
                }
            }
    print_stats("after shift_register_scalar");
}
