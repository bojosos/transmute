#pragma once

// A64 encoders for translator core tests (owner: translator core).

#include <cstdint>

namespace transmute::fx::coretest::a64 {

using u32 = std::uint32_t;

constexpr u32 bit(bool b, unsigned at) { return static_cast<u32>(b ? 1u : 0u) << at; }

constexpr u32 add_imm(bool sf, bool sub, bool s, unsigned rd, unsigned rn, unsigned imm12, bool sh = false) {
    return bit(sf, 31) | bit(sub, 30) | bit(s, 29) | (0b100010u << 23) | bit(sh, 22) | ((imm12 & 0xfff) << 10) | (rn << 5) | rd;
}
constexpr u32 logic_imm(bool sf, unsigned opc, unsigned n, unsigned immr, unsigned imms, unsigned rn, unsigned rd) {
    return bit(sf, 31) | (opc << 29) | (0b100100u << 23) | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd;
}
constexpr u32 movw(bool sf, unsigned opc, unsigned hw, unsigned imm16, unsigned rd) {
    return bit(sf, 31) | (opc << 29) | (0b100101u << 23) | (hw << 21) | ((imm16 & 0xffff) << 5) | rd;
}
constexpr u32 bitfield(bool sf, unsigned opc, unsigned immr, unsigned imms, unsigned rn, unsigned rd) {
    return bit(sf, 31) | (opc << 29) | (0b100110u << 23) | bit(sf, 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd;
}
constexpr u32 extr(bool sf, unsigned rm, unsigned lsb, unsigned rn, unsigned rd) {
    return bit(sf, 31) | (0b00100111u << 23) | bit(sf, 22) | (rm << 16) | (lsb << 10) | (rn << 5) | rd;
}
constexpr u32 adr(bool page, std::int32_t imm, unsigned rd) {
    const auto v = static_cast<u32>(imm) & 0x1fffff;
    return bit(page, 31) | ((v & 3) << 29) | (0b10000u << 24) | (((v >> 2) & 0x7ffff) << 5) | rd;
}
constexpr u32 add_shift(bool sf, bool sub, bool s, unsigned shift, unsigned rm, unsigned imm6, unsigned rn, unsigned rd) {
    return bit(sf, 31) | bit(sub, 30) | bit(s, 29) | (0b01011u << 24) | (shift << 22) | (rm << 16) | (imm6 << 10) | (rn << 5) | rd;
}
constexpr u32 add_ext(bool sf, bool sub, bool s, unsigned rm, unsigned option, unsigned imm3, unsigned rn, unsigned rd) {
    return bit(sf, 31) | bit(sub, 30) | bit(s, 29) | (0b01011u << 24) | (1u << 21) | (rm << 16) | (option << 13) | (imm3 << 10) | (rn << 5) | rd;
}
constexpr u32 adc(bool sf, bool sub, bool s, unsigned rm, unsigned rn, unsigned rd) {
    return bit(sf, 31) | bit(sub, 30) | bit(s, 29) | (0b11010000u << 21) | (rm << 16) | (rn << 5) | rd;
}
constexpr u32 logic_shift(bool sf, unsigned opc, unsigned shift, bool n, unsigned rm, unsigned imm6, unsigned rn, unsigned rd) {
    return bit(sf, 31) | (opc << 29) | (0b01010u << 24) | (shift << 22) | bit(n, 21) | (rm << 16) | (imm6 << 10) | (rn << 5) | rd;
}
constexpr u32 ccmp(bool sf, bool cmp, bool imm, unsigned rm_imm5, unsigned cond, unsigned rn, unsigned nzcv) {
    return bit(sf, 31) | bit(cmp, 30) | (1u << 29) | (0b11010010u << 21) | (rm_imm5 << 16) | (cond << 12) | bit(imm, 11) | (rn << 5) | nzcv;
}
constexpr u32 csel(bool sf, bool op, unsigned rm, unsigned cond, unsigned op2, unsigned rn, unsigned rd) {
    return bit(sf, 31) | bit(op, 30) | (0b11010100u << 21) | (rm << 16) | (cond << 12) | (op2 << 10) | (rn << 5) | rd;
}
constexpr u32 dp1(bool sf, unsigned opcode, unsigned rn, unsigned rd) {
    return bit(sf, 31) | (1u << 30) | (0b11010110u << 21) | (opcode << 10) | (rn << 5) | rd;
}
constexpr u32 dp2(bool sf, unsigned opcode, unsigned rm, unsigned rn, unsigned rd) {
    return bit(sf, 31) | (0b11010110u << 21) | (rm << 16) | (opcode << 10) | (rn << 5) | rd;
}
constexpr u32 dp3(bool sf, unsigned op31, bool o0, unsigned rm, unsigned ra, unsigned rn, unsigned rd) {
    return bit(sf, 31) | (0b11011u << 24) | (op31 << 21) | (rm << 16) | bit(o0, 15) | (ra << 10) | (rn << 5) | rd;
}
// size: 0..3; opc: 0 store, 1 load, 2 signed64/prfm, 3 signed32
constexpr u32 ldst_uimm(unsigned size, unsigned opc, unsigned imm12, unsigned rn, unsigned rt) {
    return (size << 30) | (0b111u << 27) | (0b01u << 24) | (opc << 22) | ((imm12 & 0xfff) << 10) | (rn << 5) | rt;
}
// type: 0 unscaled, 1 post-index, 2 unprivileged, 3 pre-index
constexpr u32 ldst_imm9(unsigned size, unsigned opc, std::int32_t imm9, unsigned type, unsigned rn, unsigned rt) {
    return (size << 30) | (0b111u << 27) | (opc << 22) | ((static_cast<u32>(imm9) & 0x1ff) << 12) | (type << 10) | (rn << 5) | rt;
}
constexpr u32 ldst_reg(unsigned size, unsigned opc, unsigned rm, unsigned option, bool s, unsigned rn, unsigned rt) {
    return (size << 30) | (0b111u << 27) | (opc << 22) | (1u << 21) | (rm << 16) | (option << 13) | bit(s, 12) | (0b10u << 10) | (rn << 5) | rt;
}
// opc: 0 W, 1 LDPSW, 2 X; type: 0 no-allocate, 1 post, 2 offset, 3 pre
constexpr u32 ldst_pair(unsigned opc, unsigned type, bool load, std::int32_t imm7, unsigned rt2, unsigned rn, unsigned rt) {
    return (opc << 30) | (0b101u << 27) | (type << 23) | bit(load, 22) | ((static_cast<u32>(imm7) & 0x7f) << 15) | (rt2 << 10) | (rn << 5) | rt;
}
// opc: 0 LDR W, 1 LDR X, 2 LDRSW, 3 PRFM
constexpr u32 ldr_literal(unsigned opc, std::int32_t imm19, unsigned rt) {
    return (opc << 30) | (0b011u << 27) | ((static_cast<u32>(imm19) & 0x7ffff) << 5) | rt;
}
constexpr u32 b(std::int32_t words) { return 0x14000000u | (static_cast<u32>(words) & 0x3ffffff); }
constexpr u32 bl(std::int32_t words) { return 0x94000000u | (static_cast<u32>(words) & 0x3ffffff); }
constexpr u32 bcond(unsigned cond, std::int32_t words) { return 0x54000000u | ((static_cast<u32>(words) & 0x7ffff) << 5) | cond; }
constexpr u32 cbz(bool sf, bool nz, std::int32_t words, unsigned rt) {
    return bit(sf, 31) | (0b011010u << 25) | bit(nz, 24) | ((static_cast<u32>(words) & 0x7ffff) << 5) | rt;
}
constexpr u32 tbz(bool nz, unsigned bitno, std::int32_t words, unsigned rt) {
    return ((bitno >> 5) << 31) | (0b011011u << 25) | bit(nz, 24) | ((bitno & 31) << 19) | ((static_cast<u32>(words) & 0x3fff) << 5) | rt;
}
constexpr u32 br(unsigned rn) { return 0xd61f0000u | (rn << 5); }
constexpr u32 blr(unsigned rn) { return 0xd63f0000u | (rn << 5); }
constexpr u32 ret(unsigned rn = 30) { return 0xd65f0000u | (rn << 5); }
constexpr u32 brk(unsigned imm) { return 0xd4200000u | ((imm & 0xffff) << 5); }
constexpr u32 svc(unsigned imm) { return 0xd4000001u | ((imm & 0xffff) << 5); }
constexpr u32 mrs_nzcv(unsigned rt) { return 0xd53b4200u | rt; }
constexpr u32 msr_nzcv(unsigned rt) { return 0xd51b4200u | rt; }
constexpr u32 nop() { return 0xd503201fu; }
// NEON ADD Vd.2D, Vn.2D, Vm.2D (not a core instruction).
constexpr u32 neon_add2d(unsigned rd, unsigned rn, unsigned rm) { return 0x4ee08400u | (rm << 16) | (rn << 5) | rd; }

} // namespace transmute::fx::coretest::a64
