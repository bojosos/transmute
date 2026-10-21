#pragma once

// Random generators of valid core-owned A64 encodings (owner: translator core).
//
// Register roles in generated programs:
//   x16, x17, SP  data pointers (only changed by small writebacks / SP adds)
//   x14           small index (0..255) for register-offset addressing
//   x12           loop counter (only written by loop constructs)
//   x18           untouched (platform register)
//   x30           link register (only written by BL/BLR)
//   others        random data

#include "fx/translator/core_decode.hpp"
#include "fx/translator/core_test_encode.hpp"

#include <cstdint>
#include <vector>

namespace transmute::fx::coretest {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 0x9e3779b97f4a7c15ULL + 0x1234567) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    unsigned below(unsigned n) { return static_cast<unsigned>(next() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }
    template <std::size_t N>
    unsigned pick(const unsigned (&list)[N]) { return list[below(N)]; }

private:
    std::uint64_t state_;
};

inline constexpr unsigned kDest[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
inline constexpr unsigned kSrc[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20,
                                    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 31};
inline constexpr unsigned kBase[] = {16, 17, 31};

class InstGen {
public:
    explicit InstGen(Rng& rng) : r_(rng) {}

    unsigned dest() { return r_.chance(4) ? 31u : r_.pick(kDest); }
    unsigned src() { return r_.pick(kSrc); }
    unsigned src_sp() {   // Rn where 31 means SP
        if (r_.chance(15)) return 31;
        for (;;) {
            const auto r = src();
            if (r != 31) return r;
        }
    }

    std::uint32_t add_sub_imm() {
        const bool sf = r_.chance(70), sub = r_.chance(50), s = r_.chance(40);
        if (!s && r_.chance(5)) {   // small SP adjustment
            return a64::add_imm(true, sub, false, 31, 31, 16 * r_.below(4));
        }
        unsigned rd = s && r_.chance(40) ? 31u : r_.pick(kDest);
        if (!s && rd == 31) rd = 0;
        const bool sh = r_.chance(20);
        unsigned imm = r_.chance(30) ? r_.below(8) : r_.below(4096);
        return a64::add_imm(sf, sub, s, rd, src_sp(), imm, sh);
    }
    std::uint32_t logic_imm() {
        for (;;) {
            const bool sf = r_.chance(60);
            const unsigned n = sf ? r_.below(2) : 0;
            const unsigned immr = r_.below(sf ? 64 : 32), imms = r_.below(64);
            std::uint64_t mask;
            if (!core::decode_bit_masks(n, imms, immr, sf, mask)) continue;
            const unsigned opc = r_.below(4);
            unsigned rd = opc == 3 && r_.chance(40) ? 31u : r_.pick(kDest);
            return a64::logic_imm(sf, opc, n, immr, imms, src(), rd);
        }
    }
    std::uint32_t move_wide() {
        const bool sf = r_.chance(60);
        static constexpr unsigned opcs[] = {0, 2, 3};
        const unsigned imm = r_.chance(20) ? (r_.chance(50) ? 0u : 0xffffu) : static_cast<unsigned>(r_.next() & 0xffff);
        return a64::movw(sf, r_.pick(opcs), r_.below(sf ? 4 : 2), imm, r_.pick(kDest));
    }
    std::uint32_t adr() {
        return a64::adr(r_.chance(50), static_cast<std::int32_t>(r_.next() & 0x1fffff) - 0x100000, r_.pick(kDest));
    }
    std::uint32_t bitfield() {
        const bool sf = r_.chance(60);
        const unsigned w = sf ? 64 : 32;
        static constexpr unsigned opcs[] = {0, 1, 2};
        unsigned immr = r_.below(w), imms = r_.below(w);
        switch (r_.below(6)) {   // bias towards aliases
        case 0: imms = w - 1; break;                                         // LSR/ASR
        case 1: { const unsigned sh = 1 + r_.below(w - 1); immr = (w - sh) % w; imms = w - 1 - sh; break; }  // LSL
        case 2: immr = 0; { static constexpr unsigned e[] = {7, 15, 31}; imms = r_.pick(e) % w; } break;  // SXT/UXT
        default: break;
        }
        return a64::bitfield(sf, r_.pick(opcs), immr, imms, src(), r_.pick(kDest));
    }
    std::uint32_t extr() {
        const bool sf = r_.chance(60);
        const unsigned rn = src();
        const unsigned rm = r_.chance(40) ? rn : src();
        return a64::extr(sf, rm, r_.below(sf ? 64 : 32), rn, r_.pick(kDest));
    }
    std::uint32_t add_sub_shift() {
        const bool sf = r_.chance(60), sub = r_.chance(50), s = r_.chance(40);
        const unsigned rd = s && r_.chance(40) ? 31u : dest();
        const unsigned amount = r_.chance(40) ? 0u : r_.below(sf ? 64 : 32);
        return a64::add_shift(sf, sub, s, r_.below(3), src(), amount, src(), rd);
    }
    std::uint32_t add_sub_ext() {
        const bool sf = r_.chance(60), sub = r_.chance(50), s = r_.chance(40);
        unsigned rd = s && r_.chance(40) ? 31u : r_.pick(kDest);
        return a64::add_ext(sf, sub, s, src(), r_.below(8), r_.below(5), src_sp(), rd);
    }
    std::uint32_t adc() {
        return a64::adc(r_.chance(60), r_.chance(50), r_.chance(50), src(), src(), dest());
    }
    std::uint32_t logic_shift() {
        const bool sf = r_.chance(60);
        const unsigned opc = r_.below(4);
        const unsigned rd = opc == 3 && r_.chance(40) ? 31u : dest();
        const unsigned amount = r_.chance(40) ? 0u : r_.below(sf ? 64 : 32);
        return a64::logic_shift(sf, opc, r_.below(4), r_.chance(30), src(), amount, src(), rd);
    }
    std::uint32_t ccmp() {
        const bool imm = r_.chance(40);
        return a64::ccmp(r_.chance(60), r_.chance(60), imm, imm ? r_.below(32) : src(), r_.below(16), src(), r_.below(16));
    }
    std::uint32_t csel() {
        unsigned rn = src(), rm = src();
        if (r_.chance(15)) rn = rm = 31;   // CSET / CSETM
        return a64::csel(r_.chance(60), r_.chance(50), rm, r_.below(16), r_.below(2), rn, dest());
    }
    std::uint32_t dp1() {
        const bool sf = r_.chance(60);
        unsigned opcode = r_.below(5);   // RBIT REV16 REV32/REV REV CLZ (CLS: no oracle)
        if (opcode == 3 && !sf) opcode = 2;
        return a64::dp1(sf, opcode, src(), dest());
    }
    std::uint32_t dp2() {
        static constexpr unsigned ops[] = {2, 3, 8, 9, 10, 11};
        return a64::dp2(r_.chance(60), r_.pick(ops), src(), src(), dest());
    }
    std::uint32_t dp3() {
        static constexpr unsigned forms[][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {5, 0}, {5, 1}, {6, 0}};
        const auto& f = forms[r_.below(8)];
        const bool sf = f[0] != 0 ? true : r_.chance(60);
        return a64::dp3(sf, f[0], f[1] != 0, src(), f[0] == 2 || f[0] == 6 ? 31u : src(), src(), dest());
    }
    // Loads/stores against x16/x17/SP with small offsets.
    std::uint32_t load_store() {
        const unsigned base = r_.pick(kBase);
        for (;;) {
            const unsigned size = r_.below(4);
            unsigned opc = r_.below(4);
            if (size == 3 && opc >= 2) opc = r_.below(2);
            if (size == 2 && opc == 3) opc = 2;
            const bool store = opc == 0;
            const unsigned rt = store ? src() : dest();
            switch (r_.below(6)) {
            case 0:
            case 1: return a64::ldst_uimm(size, opc, r_.below(256), base, rt);
            case 2: return a64::ldst_imm9(size, opc, static_cast<std::int32_t>(r_.below(512)) - 256, 0, base, rt);
            case 3:
            case 4: {
                const unsigned type = r_.chance(50) ? 1u : 3u;
                const auto imm = static_cast<std::int32_t>(r_.below(129)) - 64;
                unsigned t = rt;
                if (base != 31 && r_.chance(8)) t = base;   // writeback onto the transfer register
                return a64::ldst_imm9(size, opc, imm, type, base, t);
            }
            default: {
                static constexpr unsigned options[] = {2, 3, 6, 7};
                const unsigned rm = r_.chance(10) ? 31u : 14u;
                return a64::ldst_reg(size, opc, rm, r_.pick(options), r_.chance(50), base, rt);
            }
            }
        }
    }
    std::uint32_t load_store_pair() {
        const unsigned base = r_.pick(kBase);
        unsigned opc = r_.chance(50) ? 2u : 0u;
        const bool load = r_.chance(50);
        unsigned type = r_.below(4);
        if (load && r_.chance(15)) { opc = 1; if (type == 0) type = 2; }
        const auto imm = static_cast<std::int32_t>(r_.below(33)) - 16;
        unsigned rt = load ? dest() : src(), rt2 = load ? dest() : src();
        if (r_.chance(5)) rt2 = rt;
        if ((type == 1 || type == 3) && base != 31 && r_.chance(8)) rt = base;
        return a64::ldst_pair(opc, type, load, imm, rt2, base, rt);
    }
    std::uint32_t prefetch() {
        switch (r_.below(3)) {
        case 0: return a64::ldst_uimm(3, 2, r_.below(4096), r_.pick(kBase), r_.below(32));
        case 1: return a64::ldst_imm9(3, 2, static_cast<std::int32_t>(r_.below(512)) - 256, 0, r_.pick(kBase), r_.below(32));
        default: return a64::ldst_reg(3, 2, 14, 3, r_.chance(50), r_.pick(kBase), r_.below(32));
        }
    }

    // Any straight-line (non-branch, non-literal) core instruction.
    std::uint32_t data_or_memory() {
        switch (r_.below(20)) {
        case 0: case 1: return add_sub_imm();
        case 2: return logic_imm();
        case 3: return move_wide();
        case 4: return adr();
        case 5: return bitfield();
        case 6: return extr();
        case 7: case 8: return add_sub_shift();
        case 9: return add_sub_ext();
        case 10: return adc();
        case 11: return logic_shift();
        case 12: return ccmp();
        case 13: return csel();
        case 14: return dp1();
        case 15: return dp2();
        case 16: return dp3();
        case 17: case 18: return load_store();
        default: return load_store_pair();
        }
    }

private:
    Rng& r_;
};

} // namespace transmute::fx::coretest
