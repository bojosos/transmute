#pragma once

// Shared declarations of the fp test files (owner: fp).

#include "fx/translator/emit_fp_test_util.hpp"

#include <cstdint>
#include <functional>
#include <initializer_list>

namespace transmute::fx::fptest {

// Instruction word encoders.
namespace enc {
inline std::uint32_t fp2(unsigned opcode, unsigned ftype, unsigned rd, unsigned rn, unsigned rm) {
    return 0x1e200800u | ftype << 22 | rm << 16 | opcode << 12 | rn << 5 | rd;
}
inline std::uint32_t fp1(unsigned opcode, unsigned ftype, unsigned rd, unsigned rn) {
    return 0x1e204000u | ftype << 22 | opcode << 15 | rn << 5 | rd;
}
inline std::uint32_t fp3(unsigned o1, unsigned o0, unsigned ftype, unsigned rd, unsigned rn, unsigned rm,
                         unsigned ra) {
    return 0x1f000000u | ftype << 22 | o1 << 21 | rm << 16 | o0 << 15 | ra << 10 | rn << 5 | rd;
}
inline std::uint32_t fcmp(unsigned ftype, unsigned rn, unsigned rm, unsigned opc2) {
    return 0x1e202000u | ftype << 22 | rm << 16 | rn << 5 | opc2;
}
inline std::uint32_t fccmp(unsigned ftype, unsigned rn, unsigned rm, unsigned cond, unsigned op, unsigned nzcv) {
    return 0x1e200400u | ftype << 22 | rm << 16 | cond << 12 | rn << 5 | op << 4 | nzcv;
}
inline std::uint32_t fcsel(unsigned ftype, unsigned rd, unsigned rn, unsigned rm, unsigned cond) {
    return 0x1e200c00u | ftype << 22 | rm << 16 | cond << 12 | rn << 5 | rd;
}
inline std::uint32_t fmov_imm(unsigned ftype, unsigned rd, unsigned imm8) {
    return 0x1e201000u | ftype << 22 | imm8 << 13 | rd;
}
inline std::uint32_t fpint(unsigned sf, unsigned ftype, unsigned rmode, unsigned opc, unsigned rd, unsigned rn) {
    return sf << 31 | 0x1e200000u | ftype << 22 | rmode << 19 | opc << 16 | rn << 5 | rd;
}
inline std::uint32_t fpfix(unsigned sf, unsigned ftype, unsigned rmode, unsigned opc, unsigned scale, unsigned rd,
                           unsigned rn) {
    return sf << 31 | 0x1e000000u | ftype << 22 | rmode << 19 | opc << 16 | scale << 10 | rn << 5 | rd;
}
inline std::uint32_t v3(unsigned q, unsigned u, unsigned size, unsigned opcode, unsigned rd, unsigned rn, unsigned rm) {
    return q << 30 | u << 29 | 0x0e200400u | size << 22 | rm << 16 | opcode << 11 | rn << 5 | rd;
}
inline std::uint32_t s3(unsigned u, unsigned size, unsigned opcode, unsigned rd, unsigned rn, unsigned rm) {
    return 0x5e200400u | u << 29 | size << 22 | rm << 16 | opcode << 11 | rn << 5 | rd;
}
inline std::uint32_t v2(unsigned q, unsigned u, unsigned size, unsigned opcode, unsigned rd, unsigned rn) {
    return q << 30 | u << 29 | 0x0e200800u | size << 22 | opcode << 12 | rn << 5 | rd;
}
inline std::uint32_t s2(unsigned u, unsigned size, unsigned opcode, unsigned rd, unsigned rn) {
    return 0x5e200800u | u << 29 | size << 22 | opcode << 12 | rn << 5 | rd;
}
inline std::uint32_t vacross(unsigned q, unsigned u, unsigned size, unsigned opcode, unsigned rd, unsigned rn) {
    return q << 30 | u << 29 | 0x0e300800u | size << 22 | opcode << 12 | rn << 5 | rd;
}
inline std::uint32_t spair(unsigned u, unsigned size, unsigned opcode, unsigned rd, unsigned rn) {
    return 0x5e300800u | u << 29 | size << 22 | opcode << 12 | rn << 5 | rd;
}
inline std::uint32_t velem(unsigned q, unsigned u, unsigned size, unsigned l, unsigned m, unsigned rm,
                           unsigned opcode, unsigned h, unsigned rd, unsigned rn) {
    return q << 30 | u << 29 | 0x0f000000u | size << 22 | l << 21 | m << 20 | (rm & 15) << 16 | opcode << 12 |
           h << 11 | rn << 5 | rd;
}
inline std::uint32_t selem(unsigned u, unsigned size, unsigned l, unsigned m, unsigned rm, unsigned opcode,
                           unsigned h, unsigned rd, unsigned rn) {
    return 0x5f000000u | u << 29 | size << 22 | l << 21 | m << 20 | (rm & 15) << 16 | opcode << 12 | h << 11 |
           rn << 5 | rd;
}
inline std::uint32_t vshift(unsigned q, unsigned u, unsigned immhb, unsigned opcode, unsigned rd, unsigned rn) {
    return q << 30 | u << 29 | 0x0f000400u | immhb << 16 | opcode << 11 | rn << 5 | rd;
}
inline std::uint32_t sshift(unsigned u, unsigned immhb, unsigned opcode, unsigned rd, unsigned rn) {
    return 0x5f000400u | u << 29 | immhb << 16 | opcode << 11 | rn << 5 | rd;
}
inline std::uint32_t vfmov(unsigned q, unsigned op, unsigned o2, unsigned imm8, unsigned rd) {
    return q << 30 | op << 29 | 0x0f00f400u | (imm8 >> 5) << 16 | o2 << 11 | (imm8 & 31) << 5 | rd;
}
} // namespace enc

// A differential test specification.
struct Spec {
    const char* name;
    std::function<std::uint32_t(Rng&)> gen;
    unsigned es = 32;                 // lane size for generated vector values
    CheckOptions opt{};
    unsigned words = 64;
    unsigned contexts = 6;
    bool all_modes = false;           // run every FPCR mode per context (else one random)
    bool snan = true;
    bool expect_accept = true;
    bool expect_oracle = true;
    std::uint64_t seed = 1;
    std::uint32_t fpcr_extra = 0;
    std::function<void(GuestContext&, Rng&)> prepare;   // adjust the context
    // Hand-written reference (used instead of the oracle when set). Returns
    // false if it cannot model the case.
    std::function<bool(std::uint32_t, GuestContext&)> reference;
};

bool accepts(std::uint32_t word);

// References for words the reference interpreter lacks (emit_fp_tests_ref.cpp).
bool ref_scalar_via_vector(std::uint32_t word, GuestContext& c, unsigned es);
bool ref_pairwise_vector(std::uint32_t word, GuestContext& c);
bool ref_pairwise_scalar(std::uint32_t word, GuestContext& c);
bool ref_fmulx(std::uint32_t word, GuestContext& c);
bool ref_fcvtxn(std::uint32_t word, GuestContext& c);
bool ref_frecpx(std::uint32_t word, GuestContext& c);
bool ref_missing(std::uint32_t word, GuestContext& c);
bool ref_scalar_minmax(std::uint32_t word, GuestContext& c);
bool ref_fjcvtzs(std::uint32_t word, GuestContext& c);
void run_spec(const Spec& spec, Tally& tally);
void run_specs(std::initializer_list<Spec> specs);

inline unsigned reg(Rng& r) { return r.below(32); }
inline CheckOptions fp_opt(unsigned es) { CheckOptions o; o.nan_es = es; o.fallback = ref_missing; return o; }
inline CheckOptions exact_opt() { CheckOptions o; o.fallback = ref_missing; return o; }

} // namespace transmute::fx::fptest
