#include "fx/translator/core_decode.hpp"

#include <bit>

namespace transmute::fx::core {
namespace {

constexpr std::uint32_t bits(std::uint32_t w, unsigned hi, unsigned lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}
constexpr std::int64_t sext(std::uint64_t value, unsigned width) {
    const auto shift = 64 - width;
    return static_cast<std::int64_t>(value << shift) >> shift;
}
constexpr std::uint32_t bit_of(unsigned r) { return r == 31 ? 0u : (1u << r); }   // ZR omitted
constexpr std::uint32_t bit_sp(unsigned r) { return 1u << r; }                    // 31 = SP

bool decode_data_imm(std::uint32_t w, std::uint64_t pc, Inst& d) {
    d.sf = (w >> 31) != 0;
    d.rd = static_cast<std::uint8_t>(w & 31);
    d.rn = static_cast<std::uint8_t>(bits(w, 9, 5));
    const auto cls = w & 0x1f800000u;
    if ((w & 0x1f000000u) == 0x10000000u) {   // ADR / ADRP
        const auto imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21);
        d.kind = Kind::adr;
        d.sf = true;
        if (w & 0x80000000u) d.imm = static_cast<std::int64_t>((pc & ~0xfffULL) + static_cast<std::uint64_t>(imm) * 4096);
        else d.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(imm));
        return true;
    }
    if (cls == 0x11000000u) {   // ADD/SUB immediate
        d.kind = Kind::add_sub_imm;
        d.op = static_cast<std::uint8_t>(bits(w, 30, 30));
        d.s = bits(w, 29, 29) != 0;
        d.imm = static_cast<std::int64_t>(bits(w, 21, 10)) << (bits(w, 22, 22) ? 12 : 0);
        return true;
    }
    if (cls == 0x12000000u) {   // logical immediate
        std::uint64_t mask;
        if (!decode_bit_masks(bits(w, 22, 22), bits(w, 15, 10), bits(w, 21, 16), d.sf, mask)) return false;
        d.kind = Kind::logic_imm;
        d.op = static_cast<std::uint8_t>(bits(w, 30, 29));
        d.s = d.op == 3;
        d.imm = static_cast<std::int64_t>(mask);
        return true;
    }
    if (cls == 0x12800000u) {   // move wide
        const auto opc = bits(w, 30, 29);
        const auto hw = bits(w, 22, 21);
        if (opc == 1 || (!d.sf && hw >= 2)) return false;
        d.kind = Kind::move_wide;
        d.op = static_cast<std::uint8_t>(opc);
        d.amount = static_cast<std::uint8_t>(hw * 16);
        d.imm = bits(w, 20, 5);
        return true;
    }
    if (cls == 0x13000000u) {   // bitfield
        const auto opc = bits(w, 30, 29);
        const bool n = bits(w, 22, 22) != 0;
        const auto immr = bits(w, 21, 16), imms = bits(w, 15, 10);
        if (opc == 3 || n != d.sf || (!d.sf && (immr >= 32 || imms >= 32))) return false;
        d.kind = Kind::bitfield;
        d.op = static_cast<std::uint8_t>(opc);
        d.amount = static_cast<std::uint8_t>(immr);
        d.imms = static_cast<std::uint8_t>(imms);
        return true;
    }
    if (cls == 0x13800000u) {   // EXTR
        const bool n = bits(w, 22, 22) != 0;
        const auto imms = bits(w, 15, 10);
        if (bits(w, 30, 29) != 0 || bits(w, 21, 21) != 0 || n != d.sf || (!d.sf && imms >= 32)) return false;
        d.kind = Kind::extr;
        d.rm = static_cast<std::uint8_t>(bits(w, 20, 16));
        d.amount = static_cast<std::uint8_t>(imms);
        return true;
    }
    return false;
}

bool decode_branch(std::uint32_t w, std::uint64_t pc, Inst& d) {
    if ((w & 0x7c000000u) == 0x14000000u) {   // B / BL
        d.kind = (w & 0x80000000u) ? Kind::branch_link : Kind::branch;
        d.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(sext(bits(w, 25, 0), 26) * 4));
        return true;
    }
    if ((w & 0x7e000000u) == 0x34000000u) {   // CBZ / CBNZ
        d.kind = Kind::compare_branch;
        d.sf = (w >> 31) != 0;
        d.op = static_cast<std::uint8_t>(bits(w, 24, 24));
        d.rd = static_cast<std::uint8_t>(w & 31);
        d.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(sext(bits(w, 23, 5), 19) * 4));
        return true;
    }
    if ((w & 0x7e000000u) == 0x36000000u) {   // TBZ / TBNZ
        d.kind = Kind::test_branch;
        d.op = static_cast<std::uint8_t>(bits(w, 24, 24));
        d.rd = static_cast<std::uint8_t>(w & 31);
        d.amount = static_cast<std::uint8_t>((bits(w, 31, 31) << 5) | bits(w, 23, 19));
        d.sf = d.amount >= 32;
        d.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(sext(bits(w, 18, 5), 14) * 4));
        return true;
    }
    if ((w & 0xff000000u) == 0x54000000u) {   // B.cond / BC.cond
        d.kind = Kind::branch_cond;
        d.cond = static_cast<std::uint8_t>(w & 15);
        d.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(sext(bits(w, 23, 5), 19) * 4));
        return true;
    }
    if ((w & 0xff000000u) == 0xd4000000u) {   // exception generation
        const auto opc = bits(w, 23, 21), op2 = bits(w, 4, 2), ll = bits(w, 1, 0);
        if (op2 != 0) return false;
        d.imm = bits(w, 20, 5);
        if (opc == 0 && ll == 1) { d.kind = Kind::svc; return true; }
        if (opc == 1 && ll == 0) { d.kind = Kind::brk; return true; }
        if (opc == 2 && ll == 0) { d.kind = Kind::hlt; return true; }
        return false;
    }
    if ((w & 0xfe000000u) == 0xd6000000u) {   // branch to register
        d.rn = static_cast<std::uint8_t>(bits(w, 9, 5));
        const auto plain = w & 0xfffffc1fu;
        if (plain == 0xd61f0000u || (w & 0xfffff81fu) == 0xd61f081fu || (w & 0xfffff800u) == 0xd71f0800u) {
            d.kind = Kind::branch_reg;   // BR, BRAAZ/BRABZ, BRAA/BRAB
            return true;
        }
        if (plain == 0xd63f0000u || (w & 0xfffff81fu) == 0xd63f081fu || (w & 0xfffff800u) == 0xd73f0800u) {
            d.kind = Kind::branch_link_reg;   // BLR, BLRAAZ/BLRABZ, BLRAA/BLRAB
            return true;
        }
        if (plain == 0xd65f0000u) { d.kind = Kind::ret; return true; }
        if ((w & 0xfffffbffu) == 0xd65f0bffu) { d.kind = Kind::ret; d.rn = 30; return true; }   // RETAA/RETAB
        return false;
    }
    return false;
}

// Integer loads and stores (V = 0).
bool decode_load_store(std::uint32_t w, std::uint64_t pc, Inst& d) {
    d.rd = static_cast<std::uint8_t>(w & 31);
    d.rn = static_cast<std::uint8_t>(bits(w, 9, 5));
    if ((w & 0x3f000000u) == 0x18000000u) {   // load literal
        const auto opc = bits(w, 31, 30);
        d.kind = Kind::load_literal;
        d.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(sext(bits(w, 23, 5), 19) * 4));
        switch (opc) {
        case 0: d.size = 2; d.mem = MemOp::load; d.sf = false; break;
        case 1: d.size = 3; d.mem = MemOp::load; d.sf = true; break;
        case 2: d.size = 2; d.mem = MemOp::load_signed64; d.sf = true; break;
        default: d.kind = Kind::nop; break;   // PRFM (literal)
        }
        return true;
    }
    if ((w & 0x3e000000u) == 0x28000000u) {   // load/store pair (integer)
        const auto opc = bits(w, 31, 30), type = bits(w, 24, 23);
        const bool load = bits(w, 22, 22) != 0;
        d.ra = static_cast<std::uint8_t>(bits(w, 14, 10));
        if (opc == 3) return false;
        if (opc == 1 && (!load || type == 0)) return false;   // STGP / unallocated
        d.kind = Kind::load_store_pair;
        d.size = opc == 2 ? 3 : 2;
        d.sf = opc != 0;
        d.mem = !load ? MemOp::store : opc == 1 ? MemOp::load_signed64 : MemOp::load;
        d.mode = type == 1 ? AddrMode::post : type == 3 ? AddrMode::pre : AddrMode::offset;
        d.imm = sext(bits(w, 21, 15), 7) << d.size;
        return true;
    }
    const auto size = bits(w, 31, 30), opc = bits(w, 23, 22);
    const auto classify = [&](bool allow_prefetch) {
        d.size = static_cast<std::uint8_t>(size);
        switch (opc) {
        case 0: d.mem = MemOp::store; d.sf = size == 3; return true;
        case 1: d.mem = MemOp::load; d.sf = size == 3; return true;
        case 2:
            if (size == 3) { if (!allow_prefetch) return false; d.mem = MemOp::prefetch; return true; }
            d.mem = MemOp::load_signed64; d.sf = true; return true;
        default:
            if (size >= 2) return false;
            d.mem = MemOp::load_signed32; d.sf = false; return true;
        }
    };
    if ((w & 0xff200400u) == 0xf8200400u) {   // LDRAA/LDRAB (PAC disabled: plain loads)
        d.kind = Kind::load_store;
        d.size = 3;
        d.mem = MemOp::load;
        d.sf = true;
        d.imm = sext((bits(w, 22, 22) << 9) | bits(w, 20, 12), 10) * 8;
        d.mode = bits(w, 11, 11) ? AddrMode::pre : AddrMode::offset;
        return true;
    }
    if ((w & 0x3f200000u) == 0x38000000u) {   // imm9: unscaled / post / unprivileged / pre
        const auto type = bits(w, 11, 10);
        if (!classify(type == 0)) return false;
        d.kind = d.mem == MemOp::prefetch ? Kind::nop : Kind::load_store;
        d.mode = type == 1 ? AddrMode::post : type == 3 ? AddrMode::pre : AddrMode::offset;
        d.imm = sext(bits(w, 20, 12), 9);
        return true;
    }
    if ((w & 0x3f200c00u) == 0x38200800u) {   // register offset
        const auto option = bits(w, 15, 13);
        if ((option & 2) == 0) return false;
        if (!classify(true)) return false;
        d.kind = d.mem == MemOp::prefetch ? Kind::nop : Kind::load_store;
        d.mode = AddrMode::reg;
        d.rm = static_cast<std::uint8_t>(bits(w, 20, 16));
        d.shift = static_cast<std::uint8_t>(option);
        d.amount = static_cast<std::uint8_t>(bits(w, 12, 12) ? size : 0);
        return true;
    }
    if ((w & 0x3f000000u) == 0x39000000u) {   // unsigned offset
        if (!classify(true)) return false;
        d.kind = d.mem == MemOp::prefetch ? Kind::nop : Kind::load_store;
        d.mode = AddrMode::offset;
        d.imm = static_cast<std::int64_t>(bits(w, 21, 10)) << size;
        return true;
    }
    return false;
}

bool decode_data_reg(std::uint32_t w, Inst& d) {
    d.sf = (w >> 31) != 0;
    d.rd = static_cast<std::uint8_t>(w & 31);
    d.rn = static_cast<std::uint8_t>(bits(w, 9, 5));
    d.rm = static_cast<std::uint8_t>(bits(w, 20, 16));
    if ((w & 0x1f000000u) == 0x0a000000u) {   // logical shifted register
        const auto imm6 = bits(w, 15, 10);
        if (!d.sf && imm6 >= 32) return false;
        d.kind = Kind::logic_shift;
        d.op = static_cast<std::uint8_t>(bits(w, 30, 29));
        d.s = d.op == 3;
        d.invert = bits(w, 21, 21) != 0;
        d.shift = static_cast<std::uint8_t>(bits(w, 23, 22));
        d.amount = static_cast<std::uint8_t>(imm6);
        return true;
    }
    if ((w & 0x1f200000u) == 0x0b000000u) {   // add/sub shifted register
        const auto type = bits(w, 23, 22), imm6 = bits(w, 15, 10);
        if (type == 3 || (!d.sf && imm6 >= 32)) return false;
        d.kind = Kind::add_sub_shift;
        d.op = static_cast<std::uint8_t>(bits(w, 30, 30));
        d.s = bits(w, 29, 29) != 0;
        d.shift = static_cast<std::uint8_t>(type);
        d.amount = static_cast<std::uint8_t>(imm6);
        return true;
    }
    if ((w & 0x1fe00000u) == 0x0b200000u) {   // add/sub extended register
        const auto imm3 = bits(w, 12, 10);
        if (imm3 > 4) return false;
        d.kind = Kind::add_sub_ext;
        d.op = static_cast<std::uint8_t>(bits(w, 30, 30));
        d.s = bits(w, 29, 29) != 0;
        d.shift = static_cast<std::uint8_t>(bits(w, 15, 13));
        d.amount = static_cast<std::uint8_t>(imm3);
        return true;
    }
    if ((w & 0x1fe0fc00u) == 0x1a000000u) {   // ADC/SBC(S)
        d.kind = Kind::adc;
        d.op = static_cast<std::uint8_t>(bits(w, 30, 30));
        d.s = bits(w, 29, 29) != 0;
        return true;
    }
    if ((w & 0x3fe00410u) == 0x3a400000u) {   // CCMN/CCMP (register or immediate)
        d.kind = Kind::ccmp;
        d.op = static_cast<std::uint8_t>(bits(w, 30, 30));
        d.imm_form = bits(w, 11, 11) != 0;
        d.imm = d.rm;
        d.cond = static_cast<std::uint8_t>(bits(w, 15, 12));
        d.imms = static_cast<std::uint8_t>(w & 15);
        d.s = true;
        return true;
    }
    if ((w & 0x3fe00800u) == 0x1a800000u) {   // conditional select (S = 0, op2<1> = 0)
        d.kind = Kind::csel;
        d.op = static_cast<std::uint8_t>((bits(w, 30, 30) << 1) | bits(w, 10, 10));
        d.cond = static_cast<std::uint8_t>(bits(w, 15, 12));
        return true;
    }
    if ((w & 0x7fe00000u) == 0x1ac00000u) {   // data processing (2 source), S = 0
        switch (bits(w, 15, 10)) {
        case 2: d.op = dp2_udiv; break;
        case 3: d.op = dp2_sdiv; break;
        case 8: d.op = dp2_lslv; break;
        case 9: d.op = dp2_lsrv; break;
        case 10: d.op = dp2_asrv; break;
        case 11: d.op = dp2_rorv; break;
        default: return false;
        }
        d.kind = Kind::dp2;
        return true;
    }
    if ((w & 0x7fe00000u) == 0x5ac00000u) {   // data processing (1 source), S = 0
        const auto opcode2 = bits(w, 20, 16), opcode = bits(w, 15, 10);
        if (opcode2 == 0) {
            switch (opcode) {
            case 0: d.op = dp1_rbit; break;
            case 1: d.op = dp1_rev16; break;
            case 2: d.op = d.sf ? dp1_rev32 : dp1_rev; break;
            case 3: if (!d.sf) return false; d.op = dp1_rev; break;
            case 4: d.op = dp1_clz; break;
            case 5: d.op = dp1_cls; break;
            default: return false;
            }
            d.kind = Kind::dp1;
            return true;
        }
        if (opcode2 == 1 && d.sf) {   // PAC*/AUT*/XPAC*: no-ops with pointer authentication disabled
            if (opcode <= 7 || (opcode <= 15 && d.rn == 31) || ((opcode == 16 || opcode == 17) && d.rn == 31)) {
                d.kind = Kind::nop;
                return true;
            }
        }
        return false;
    }
    if ((w & 0x1f000000u) == 0x1b000000u) {   // data processing (3 source)
        if (bits(w, 30, 29) != 0) return false;
        const auto op31 = bits(w, 23, 21), o0 = bits(w, 15, 15);
        d.ra = static_cast<std::uint8_t>(bits(w, 14, 10));
        switch (op31) {
        case 0: d.op = o0 ? dp3_msub : dp3_madd; break;
        case 1: if (!d.sf) return false; d.op = o0 ? dp3_smsubl : dp3_smaddl; break;
        case 2: if (!d.sf || o0) return false; d.op = dp3_smulh; break;
        case 5: if (!d.sf) return false; d.op = o0 ? dp3_umsubl : dp3_umaddl; break;
        case 6: if (!d.sf || o0) return false; d.op = dp3_umulh; break;
        default: return false;
        }
        d.kind = Kind::dp3;
        return true;
    }
    return false;
}

} // namespace

bool decode_bit_masks(unsigned n, unsigned imms, unsigned immr, bool sf, std::uint64_t& mask) {
    if (!sf && n) return false;
    const unsigned pattern = (n << 6) | (~imms & 0x3fu);
    if (pattern == 0) return false;
    const unsigned len = static_cast<unsigned>(std::bit_width(pattern)) - 1;
    if (len < 1 || len > (sf ? 6u : 5u)) return false;
    const unsigned esize = 1u << len;
    const unsigned levels = esize - 1;
    const unsigned s = imms & levels, r = immr & levels;
    if (s == levels) return false;
    const std::uint64_t emask = esize == 64 ? ~0ULL : (1ULL << esize) - 1;
    const std::uint64_t welem = (s + 1 == 64) ? ~0ULL : (1ULL << (s + 1)) - 1;
    const std::uint64_t elem = r == 0 ? welem : (((welem >> r) | (welem << (esize - r))) & emask);
    std::uint64_t result = 0;
    for (unsigned b = 0; b < (sf ? 64u : 32u); b += esize) result |= elem << b;
    mask = result;
    return true;
}

bool condition_holds(unsigned cond, std::uint32_t nzcv) {
    const bool n = nzcv & 0x80000000u, z = nzcv & 0x40000000u, c = nzcv & 0x20000000u, v = nzcv & 0x10000000u;
    bool result;
    switch (cond >> 1) {
    case 0: result = z; break;
    case 1: result = c; break;
    case 2: result = n; break;
    case 3: result = v; break;
    case 4: result = c && !z; break;
    case 5: result = n == v; break;
    case 6: result = n == v && !z; break;
    default: return true;
    }
    return (cond & 1) ? !result : result;
}

bool decode(std::uint32_t w, std::uint64_t pc, Inst& out) {
    out = Inst{};
    out.word = w;
    bool ok = false;
    if ((w & 0xffff0000u) == 0) {
        out.kind = Kind::udf;
        out.imm = w & 0xffff;
        return true;
    }
    const auto op0 = bits(w, 28, 25);
    if ((op0 & 0b1110) == 0b1000) ok = decode_data_imm(w, pc, out);
    else if ((op0 & 0b1110) == 0b1010) ok = decode_branch(w, pc, out);
    else if ((op0 & 0b0101) == 0b0100) ok = (w & 0x04000000u) == 0 && decode_load_store(w, pc, out);
    else if ((op0 & 0b0111) == 0b0101) ok = decode_data_reg(w, out);
    if (!ok) {
        out = Inst{};
        out.word = w;
        out.kind = Kind::unknown;
    }
    return ok;
}

Use usage(const Inst& d) {
    Use u;
    switch (d.kind) {
    case Kind::unknown:
    case Kind::nop:
    case Kind::udf:
    case Kind::brk:
    case Kind::hlt:
    case Kind::svc:
    case Kind::branch:
        break;
    case Kind::add_sub_imm:
        u.read = bit_sp(d.rn);
        u.write = d.s ? bit_of(d.rd) : bit_sp(d.rd);
        u.writes_flags = d.s;
        break;
    case Kind::logic_imm:
        u.read = bit_of(d.rn);
        u.write = d.s ? bit_of(d.rd) : bit_sp(d.rd);
        u.writes_flags = d.s;
        break;
    case Kind::move_wide:
        u.read = d.op == 3 ? bit_of(d.rd) : 0;
        u.write = bit_of(d.rd);
        break;
    case Kind::adr:
        u.write = bit_of(d.rd);
        break;
    case Kind::bitfield:
        u.read = bit_of(d.rn) | (d.op == 1 ? bit_of(d.rd) : 0);
        u.write = bit_of(d.rd);
        break;
    case Kind::extr:
    case Kind::dp2:
        u.read = bit_of(d.rn) | bit_of(d.rm);
        u.write = bit_of(d.rd);
        break;
    case Kind::add_sub_shift:
    case Kind::logic_shift:
        u.read = bit_of(d.rn) | bit_of(d.rm);
        u.write = bit_of(d.rd);
        u.writes_flags = d.s;
        break;
    case Kind::add_sub_ext:
        u.read = bit_sp(d.rn) | bit_of(d.rm);
        u.write = d.s ? bit_of(d.rd) : bit_sp(d.rd);
        u.writes_flags = d.s;
        break;
    case Kind::adc:
        u.read = bit_of(d.rn) | bit_of(d.rm);
        u.write = bit_of(d.rd);
        u.reads_flags = true;
        u.writes_flags = d.s;
        break;
    case Kind::ccmp:
        u.read = bit_of(d.rn) | (d.imm_form ? 0 : bit_of(d.rm));
        u.reads_flags = d.cond < 14;
        u.writes_flags = true;
        break;
    case Kind::csel:
        u.read = bit_of(d.rn) | bit_of(d.rm);
        u.write = bit_of(d.rd);
        u.reads_flags = d.cond < 14;
        break;
    case Kind::dp1:
        u.read = bit_of(d.rn);
        u.write = bit_of(d.rd);
        break;
    case Kind::dp3:
        u.read = bit_of(d.rn) | bit_of(d.rm) | (d.op <= dp3_umsubl ? bit_of(d.ra) : 0);
        u.write = bit_of(d.rd);
        break;
    case Kind::load_store:
        u.may_fault = true;
        u.read = bit_sp(d.rn) | (d.mode == AddrMode::reg ? bit_of(d.rm) : 0);
        if (d.mem == MemOp::store) u.read |= bit_of(d.rd);
        else u.write |= bit_of(d.rd);
        if (d.mode == AddrMode::pre || d.mode == AddrMode::post) u.write |= bit_sp(d.rn);
        break;
    case Kind::load_store_pair:
        u.may_fault = true;
        u.read = bit_sp(d.rn);
        if (d.mem == MemOp::store) u.read |= bit_of(d.rd) | bit_of(d.ra);
        else u.write |= bit_of(d.rd) | bit_of(d.ra);
        if (d.mode == AddrMode::pre || d.mode == AddrMode::post) u.write |= bit_sp(d.rn);
        break;
    case Kind::load_literal:
        u.may_fault = true;
        u.write = bit_of(d.rd);
        break;
    case Kind::branch_link:
        u.write = 1u << 30;
        break;
    case Kind::branch_cond:
        u.reads_flags = d.cond < 14;
        break;
    case Kind::compare_branch:
    case Kind::test_branch:
        u.read = bit_of(d.rd);
        break;
    case Kind::branch_reg:
    case Kind::ret:
        u.read = bit_of(d.rn);
        break;
    case Kind::branch_link_reg:
        u.read = bit_of(d.rn);
        u.write = 1u << 30;
        break;
    }
    return u;
}

} // namespace transmute::fx::core
