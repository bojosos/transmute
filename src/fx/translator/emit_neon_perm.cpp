// Moves and permutes: DUP/INS/UMOV/SMOV, ZIP/UZP/TRN, EXT, TBL/TBX and the
// modified-immediate group (MOVI/MVNI/ORR/BIC) (owner: neon).
#include "fx/translator/emit_neon_internal.hpp"

namespace transmute::fx::neon {

namespace {

// imm5 -> element size (0..3) and index, or false if reserved.
bool decode_imm5(unsigned imm5, unsigned& size, unsigned& index) {
    if (imm5 & 1) { size = 0; index = imm5 >> 1; return true; }
    if (imm5 & 2) { size = 1; index = imm5 >> 2; return true; }
    if (imm5 & 4) { size = 2; index = imm5 >> 3; return true; }
    if (imm5 & 8) { size = 3; index = imm5 >> 4; return true; }
    return false;
}

// pshufb control that copies element `index` (of `size`) into every element
// of the low `bytes` bytes and zeroes the rest.
Const128 dup_control(unsigned size, unsigned index, unsigned bytes) {
    std::uint8_t b[16];
    const unsigned esize = 1u << size;
    for (unsigned i = 0; i < 16; ++i)
        b[i] = i < bytes ? static_cast<std::uint8_t>(index * esize + i % esize) : 0x80;
    return bytes_const(b);
}

void vpinsr(V& v, unsigned size, Xmm d, Xmm src, Gp r, unsigned lane) {
    auto& a = v.a;
    if (v.avx) {
        static constexpr VexOp ops[4] = {{1, 3, 0x20, false}, {1, 1, 0xc4, false},
                                         {1, 3, 0x22, false}, {1, 3, 0x22, true}};
        a.vex_rr(ops[size], false, x64::id(d), x64::id(src), x64::id(r));
        a.db(static_cast<std::uint8_t>(lane));
        return;
    }
    v.mov(d, src);
    a.pinsr(8u << size, d, r, static_cast<std::uint8_t>(lane));
}

void pextr(V& v, unsigned size, Gp r, Xmm x, unsigned lane) {
    auto& a = v.a;
    if (size == 2 && lane == 0) { a.movd(r, x); return; }
    if (size == 3 && lane == 0) { a.movq(r, x); return; }
    a.pextr(8u << size, r, x, static_cast<std::uint8_t>(lane));
}

} // namespace

// ============================================================================
// Copy group: DUP, INS, SMOV, UMOV
// ============================================================================

unsigned emit_copy(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), opb = bit(w, 29);
    const unsigned imm5 = bits(w, 20, 16), imm4 = bits(w, 14, 11);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    unsigned size = 0, index = 0;
    if (!decode_imm5(imm5, size, index)) return 0;
    auto& a = ctx.as();

    if (opb) {   // INS (element)
        if (!q) return 0;
        const unsigned index2 = imm4 >> size;
        V v(ctx);
        if (size == 2 && rn != rd) {   // insertps also takes the element straight from memory
            const Src n = source(ctx, rn);
            const Xmm dold = ctx.read_vec(rd);
            const Xmm d = ctx.write_vec(rd);
            if (n.is_mem) v.op_imm(op::insertps, d, dold, n.element(2, index2), static_cast<std::uint8_t>(index << 4));
            else v.op_imm(op::insertps, d, dold, n.reg, static_cast<std::uint8_t>((index2 << 6) | (index << 4)));
            return 1;
        }
        const Xmm n = ctx.read_vec(rn);
        const Xmm dold = rd == rn ? n : ctx.read_vec(rd);
        switch (size) {
        case 2: {
            const Xmm d = ctx.write_vec(rd);
            v.op_imm(op::insertps, d, dold, n, static_cast<std::uint8_t>((index2 << 6) | (index << 4)));
            return 1;
        }
        case 3: {
            const Xmm d = ctx.write_vec(rd);
            if (index == 0) {
                if (index2 == 0) v.op_imm(op::pblendw, d, dold, n, 0x0f);
                else v.op_imm(op::shufpd, d, n, dold, 0x3);
            } else {
                if (index2 == 0) v.op(op::punpcklqdq, d, dold, n);
                else v.op_imm(op::pblendw, d, dold, n, 0xf0);
            }
            return 1;
        }
        default: {
            if (size == 1 && index == index2) {
                const Xmm d = ctx.write_vec(rd);
                v.op_imm(op::pblendw, d, dold, n, static_cast<std::uint8_t>(1u << index));
                return 1;
            }
            const Gp t = ctx.temp_gpr();
            pextr(v, size, t, n, index2);
            const Xmm d = ctx.write_vec(rd);
            vpinsr(v, size, d, dold, t, index);
            return 1;
        }
        }
    }

    switch (imm4) {
    case 0x0: {   // DUP (element)
        if (size == 3 && !q) return 0;
        V v(ctx);
        if (q) {   // broadcast straight from memory when Vn is there
            const Src n = dup_source(v, size, element_source(ctx, rn, size));
            const Xmm d = ctx.write_vec(rd);
            dup_element(v, size, d, n, index);
            return 1;
        }
        const Xmm n = ctx.read_vec(rn);
        const Xmm d = ctx.write_vec(rd);
        v.pshufb(d, n, dup_control(size, index, 8));
        return 1;
    }
    case 0x1: {   // DUP (general)
        if (size == 3 && !q) return 0;
        V v(ctx);
        if (q && rn != 31 && (v.avx2 || (v.avx && size == 2))) {   // broadcast straight from memory
            const auto g = ctx.gsrc(rn);
            if (g.is_mem) {
                static constexpr VexOp ops[4] = {vop::vpbroadcastb, vop::vpbroadcastw, vop::vbroadcastss,
                                                 vop::vpbroadcastq};
                const Xmm d = ctx.write_vec(rd);
                v.vex2(ops[size], d, g.mem);
                return 1;
            }
        }
        const Gp r = ctx.read_gpr(rn);
        const Xmm d = ctx.write_vec(rd);
        const Xmm t = v.temp();
        if (size == 3) a.movq(t, r);
        else a.movd(t, r);
        if (q && v.avx2) {
            static constexpr VexOp ops[4] = {vop::vpbroadcastb, vop::vpbroadcastw, vop::vpbroadcastd,
                                             vop::vpbroadcastq};
            v.vex2(ops[size], d, t);
            return 1;
        }
        switch (size) {
        case 0: v.pshufb(d, t, dup_control(0, 0, q ? 16 : 8)); break;
        case 1:
            if (q) {
                v.un_imm(op::pshuflw, t, t, 0);
                v.pshufd(d, t, 0x44);
            } else {
                v.un_imm(op::pshuflw, d, t, 0);
            }
            break;
        case 2: v.pshufd(d, t, q ? 0x00 : 0x50); break;
        default: v.op(op::punpcklqdq, d, t, t, true); break;
        }
        return 1;
    }
    case 0x3: {   // INS (general)
        if (!q) return 0;
        V v(ctx);
        if (rn == 31 && size == 2) {   // MOV Vd.S[i], WZR: insertps zero mask
            const Xmm dold = ctx.read_vec(rd);
            const Xmm d = ctx.write_vec(rd);
            v.op_imm(op::insertps, d, dold, dold, static_cast<std::uint8_t>((index << 6) | (index << 4) | (1u << index)));
            return 1;
        }
        if (v.avx && rn != 31) {   // pinsr reads the element straight from memory
            const auto g = ctx.gsrc(rn);
            if (g.is_mem) {
                static constexpr VexOp ops[4] = {{1, 3, 0x20, false}, {1, 1, 0xc4, false},
                                                 {1, 3, 0x22, false}, {1, 3, 0x22, true}};
                const Xmm dold = ctx.read_vec(rd);
                const Xmm d = ctx.write_vec(rd);
                a.vex_rm(ops[size], false, x64::id(d), x64::id(dold), g.mem, 1);
                a.db(static_cast<std::uint8_t>(index));
                return 1;
            }
        }
        const Gp r = ctx.read_gpr(rn);
        const Xmm dold = ctx.read_vec(rd);
        const Xmm d = ctx.write_vec(rd);
        vpinsr(v, size, d, dold, r, index);
        return 1;
    }
    case 0x5: {   // SMOV
        if (size == 3 || (size == 2 && !q)) return 0;
        V v(ctx);
        const Src n = source(ctx, rn);
        if (n.is_mem) {   // sign-extending load of the element
            const Gp d = ctx.write_gpr(rd);
            a.movsx(q ? 64 : 32, d, n.element(size, index), 8u << size);
            return 1;
        }
        const Gp t = ctx.temp_gpr();
        pextr(v, size, t, n.reg, index);
        const Gp d = ctx.write_gpr(rd);
        a.movsx(q ? 64 : 32, d, t, 8u << size);
        return 1;
    }
    case 0x7: {   // UMOV
        if (q ? size != 3 : size == 3) return 0;
        V v(ctx);
        const Src n = source(ctx, rn);
        const Gp d = ctx.write_gpr(rd);
        if (n.is_mem) {   // zero-extending load of the element
            const Mem m = n.element(size, index);
            if (size < 2) a.movzx(32, d, m, 8u << size);
            else a.mov(size == 3 ? 64 : 32, d, m);
            return 1;
        }
        pextr(v, size, d, n.reg, index);
        return 1;
    }
    default: return 0;
    }
}

unsigned emit_scalar_copy(EmitContext& ctx, std::uint32_t w) {
    const unsigned imm5 = bits(w, 20, 16), imm4 = bits(w, 14, 11);
    if (bit(w, 29) || imm4 != 0) return 0;
    unsigned size = 0, index = 0;
    if (!decode_imm5(imm5, size, index)) return 0;
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    V v(ctx);
    if (size >= 2) {   // an in-memory element is one zero-extending vector load
        const Src n = source(ctx, rn);
        if (n.is_mem) {
            const Xmm d = ctx.write_vec(rd);
            v.un(size == 2 ? SseOp{0xf3, 1, 0x10} : op::movq_load, d, n.element(size, index));   // movss / movq
            return 1;
        }
    }
    const Xmm n = ctx.read_vec(rn);
    const Xmm d = ctx.write_vec(rd);
    switch (size) {
    case 0: case 1: v.pshufb(d, n, dup_control(size, index, 1u << size)); break;
    case 2: v.op_imm(op::insertps, d, n, n, static_cast<std::uint8_t>((index << 6) | 0x0e)); break;
    default:
        if (index == 0) v.movq(d, n);
        else v.psrldq(d, n, 8);
        break;
    }
    return 1;
}

// ============================================================================
// ZIP / UZP / TRN
// ============================================================================

unsigned emit_permute(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30);
    const unsigned size = bits(w, 23, 22), rm = bits(w, 20, 16), opc = bits(w, 14, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (opc == 0 || opc == 4) return 0;
    if (size == 3 && !q) return 0;
    V v(ctx);
    const bool second = (opc & 4) != 0;
    // Forms that use Vm once, as the second source: it may stay in memory.
    const bool m_once = q && ((opc & 3) == 3 || ((opc & 3) == 1 && size >= 2) || ((opc & 3) == 2 && size == 3));
    const Xmm n = ctx.read_vec(rn);
    const Src ms = rm == rn ? Src{n, {}, false, rm} : source(ctx, rm);
    const Xmm m = m_once ? Xmm{} : in_reg(ctx, ms);
    const Xmm d = ctx.write_vec(rd);
    if (m_once) {
        switch (opc & 3) {
        case 3: v.op(second ? kUnpackHi[size] : kUnpackLo[size], d, n, ms); break;   // ZIP1 / ZIP2
        case 1:   // UZP1 / UZP2
            if (size == 2) {
                if (ms.is_mem) v.op_imm(op::shufps, d, n, ms.mem, second ? 0xdd : 0x88);
                else v.op_imm(op::shufps, d, n, ms.reg, second ? 0xdd : 0x88);
            } else {
                v.op(second ? op::punpckhqdq : op::punpcklqdq, d, n, ms);
            }
            break;
        default: v.op(second ? op::punpckhqdq : op::punpcklqdq, d, n, ms); break;   // TRN1 / TRN2 (D)
        }
        return 1;
    }
    switch (opc & 3) {
    case 3:   // ZIP1 / ZIP2
        if (q) {
            v.op(second ? kUnpackHi[size] : kUnpackLo[size], d, n, m);
        } else {
            v.op(kUnpackLo[size], d, n, m);
            if (second) v.psrldq(d, d, 8);
            else v.movq(d, d);
        }
        return 1;
    case 1:   // UZP1 / UZP2
        if (q) {
            switch (size) {
            case 0: case 1: {
                const Xmm a = v.temp();
                const Xmm b = v.temp();
                if (second) {
                    if (size == 0) { v.psrlw(a, n, 8); v.psrlw(b, m, 8); }
                    else { v.psrld(a, n, 16); v.psrld(b, m, 16); }
                } else {
                    const Const128 mask = size == 0 ? splat16(0x00ff) : splat32(0xffff);
                    v.op(op::pand, a, n, mask);
                    v.op(op::pand, b, m, mask);
                }
                v.op(size == 0 ? op::packuswb : op::packusdw, d, a, b);
                break;
            }
            case 2: v.op_imm(op::shufps, d, n, m, second ? 0xdd : 0x88); break;
            default: v.op(second ? op::punpckhqdq : op::punpcklqdq, d, n, m); break;
            }
        } else {
            if (size == 2) {
                v.op(op::punpckldq, d, n, m);
                if (second) v.psrldq(d, d, 8);
                else v.movq(d, d);
            } else {
                static const Const128 sel[2][2] = {
                    {bytes_const({0, 2, 4, 6, 8, 10, 12, 14, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
                     bytes_const({1, 3, 5, 7, 9, 11, 13, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80})},
                    {bytes_const({0, 1, 4, 5, 8, 9, 12, 13, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
                     bytes_const({2, 3, 6, 7, 10, 11, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80})},
                };
                const Xmm t = v.temp();
                v.op(op::punpcklqdq, t, n, m);
                v.pshufb(d, t, sel[size][second]);
            }
        }
        return 1;
    case 2: {   // TRN1 / TRN2
        if (size == 3) {
            v.op(second ? op::punpckhqdq : op::punpcklqdq, d, n, m);
            return 1;
        }
        const Xmm t = v.temp();
        if (size == 0) {
            const Xmm u = v.temp();
            if (!second) {
                v.op(op::pand, t, n, splat16(0x00ff));
                v.psllw(u, m, 8);
            } else {
                v.psrlw(t, n, 8);
                v.op(op::pand, u, m, splat16(0xff00));
            }
            v.por(d, t, u);
        } else if (size == 1) {
            if (!second) {
                v.pslld(t, m, 16);
                v.op_imm(op::pblendw, d, n, t, 0xaa);
            } else {
                v.psrld(t, n, 16);
                v.op_imm(op::pblendw, d, t, m, 0xaa);
            }
        } else {
            if (!second) {
                v.psllq(t, m, 32);
                if (v.avx2) v.vex3_imm(vop::vpblendd, d, n, t, 0x0a);
                else v.op_imm(op::pblendw, d, n, t, 0xcc);
            } else {
                v.psrlq(t, n, 32);
                if (v.avx2) v.vex3_imm(vop::vpblendd, d, t, m, 0x0a);
                else v.op_imm(op::pblendw, d, t, m, 0xcc);
            }
        }
        v.finish(d, q);
        return 1;
    }
    default: return 0;
    }
}

// ============================================================================
// EXT
// ============================================================================

unsigned emit_extract(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30);
    const unsigned rm = bits(w, 20, 16), imm4 = bits(w, 14, 11);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (!q && (imm4 & 8)) return 0;
    V v(ctx);
    const Src ns = source(ctx, rn);
    if (imm4 == 0) {   // a copy of (the low half of) Vn
        const Xmm d = ctx.write_vec(rd);
        if (ns.is_mem) v.un(q ? op::movdqu : op::movq_load, d, ns.mem);
        else if (q) v.mov(d, ns.reg);
        else v.movq(d, ns.reg);
        return 1;
    }
    if (q) {   // palignr takes Vn (the low part) as its second source
        const Xmm m = ctx.read_vec(rm);
        const Xmm d = ctx.write_vec(rd);
        if (ns.is_mem) v.op_imm(op::palignr, d, m, ns.mem, static_cast<std::uint8_t>(imm4));
        else v.op_imm(op::palignr, d, m, ns.reg, static_cast<std::uint8_t>(imm4));
        return 1;
    }
    const Xmm n = in_reg(ctx, ns);
    const Xmm m = rm == rn ? n : ctx.read_vec(rm);
    const Xmm d = ctx.write_vec(rd);
    std::uint8_t c[16];
    for (unsigned i = 0; i < 16; ++i) c[i] = i < 8 ? static_cast<std::uint8_t>(imm4 + i) : 0x80;
    const Xmm t = v.temp();
    v.op(op::punpcklqdq, t, n, m);
    v.pshufb(d, t, bytes_const(c));
    return 1;
}

// ============================================================================
// TBL / TBX
// ============================================================================

unsigned emit_table(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30);
    const unsigned rm = bits(w, 20, 16), len = bits(w, 14, 13);
    const bool tbx = bit(w, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    const unsigned count = len + 1;
    V v(ctx);
    // Tier 2: the adjusted indices precomputed after the index register's
    // definitions (EmitContext::vec_derived; tbl_derive in emit_neon_fusion.cpp).
    if (!tbx && neon_on(neon_no_tbl_derived)) {
        Mem slot[4];
        bool all = true;
        for (unsigned k = 0; k < count && all; ++k) all = ctx.vec_derived(rm, k, slot[k]);
        if (all) {
            Xmm table[4];
            for (unsigned i = 0; i < count; ++i) table[i] = ctx.read_vec((rn + i) & 31);
            const Xmm d = ctx.write_vec(rd);
            if (count == 1) {
                v.op(op::pshufb, d, table[0], slot[0]);
            } else {
                const Xmm r = v.temp();
                const Xmm u = v.temp();
                v.op(op::pshufb, r, table[0], slot[0]);
                for (unsigned k = 1; k < count; ++k) {
                    v.op(op::pshufb, u, table[k], slot[k]);
                    v.por(k + 1 == count ? d : r, r, u);
                }
            }
            v.finish(d, q);
            return 1;
        }
    }
    const Xmm idx = ctx.read_vec(rm);
    Xmm table[4];
    for (unsigned i = 0; i < count; ++i) table[i] = ctx.read_vec((rn + i) & 31);
    const Xmm dold = tbx ? ctx.read_vec(rd) : Xmm{};
    const Xmm d = ctx.write_vec(rd);
    // TBL with one table (AVX): the adjusted indices go into the destination
    // itself unless it is the table register; no scratch (and no eviction of
    // a cached register) needed. TRANSMUTE_FX_NEON=notbltemp: always a temp.
    if (count == 1 && !tbx && v.avx && d != table[0] && neon_on(neon_no_tbl_dest)) {
        v.op(op::paddusb, d, idx, splat8(0x70));   // bit 7 set for idx >= 16
        v.pshufb(d, table[0], d);
        v.finish(d, q);
        return 1;
    }
    const Xmm t = v.temp();

    if (count == 1) {
        v.op(op::paddusb, t, idx, splat8(0x70));   // bit 7 set for idx >= 16
        if (!tbx) {
            v.pshufb(d, table[0], t);
        } else {
            const Xmm r = v.temp();
            v.pshufb(r, table[0], t);
            if (!v.avx) {
                const Xmm z = v.temp();
                v.zero(z);
                v.op(op::pcmpgtb, t, z, t);   // full mask for out-of-range lanes
            }
            v.blendv(d, r, dold, t);
        }
        v.finish(d, q);
        return 1;
    }

    const Xmm r = v.temp();
    const Xmm ik = v.temp();
    const Xmm u = v.temp();
    v.op(op::paddusb, t, idx, splat8(0x70));
    v.pshufb(r, table[0], t);
    for (unsigned k = 1; k < count; ++k) {
        // idx - 16k keeps the low nibble; lanes outside [16k, 16k + 16)
        // saturate to bit 7 set after + 0x70, so pshufb zeroes them.
        if (k == 1) v.op(op::psubb, ik, idx, splat8(16));
        else v.op(op::psubb, ik, ik, splat8(16));
        v.op(op::paddusb, t, ik, splat8(0x70));
        v.pshufb(u, table[k], t);
        if (k + 1 == count && !tbx) v.por(d, r, u);
        else v.por(r, r, u);
    }
    if (tbx) {   // keep the destination where idx >= 16 * count
        v.op(op::pminub, t, idx, splat8(static_cast<std::uint8_t>(16 * count - 1)));
        v.op(op::pcmpeqb, t, t, idx, true);   // in range
        v.pandn(t, t, dold);
        v.por(d, r, t);
    }
    v.finish(d, q);
    return 1;
}

// ============================================================================
// Modified immediate: MOVI / MVNI / ORR / BIC
// ============================================================================

unsigned emit_modified_immediate(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), opb = bit(w, 29);
    const unsigned cmode = bits(w, 15, 12);
    const unsigned rd = bits(w, 4, 0);
    if (bit(w, 11)) return 0;          // FMOV (half precision)
    if (cmode == 0xf) return 0;        // FMOV (single/double)
    const std::uint64_t imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5);
    std::uint64_t imm64 = 0;
    bool modify = false;   // ORR/BIC
    bool invert = false;   // MVNI / BIC
    switch (cmode >> 1) {
    case 0: case 1: case 2: case 3:
        imm64 = rep32(static_cast<std::uint32_t>(imm8 << (8 * (cmode >> 1))));
        modify = (cmode & 1) != 0;
        invert = opb;
        break;
    case 4: case 5:
        imm64 = rep16(static_cast<std::uint16_t>(imm8 << (8 * ((cmode >> 1) & 1))));
        modify = (cmode & 1) != 0;
        invert = opb;
        break;
    case 6:
        imm64 = rep32(cmode & 1 ? static_cast<std::uint32_t>((imm8 << 16) | 0xffff)
                                : static_cast<std::uint32_t>((imm8 << 8) | 0xff));
        invert = opb;
        break;
    default:   // cmode 1110
        if (!opb) {
            imm64 = rep8(static_cast<std::uint8_t>(imm8));
        } else {
            for (unsigned i = 0; i < 8; ++i)
                if (imm8 & (1ULL << i)) imm64 |= 0xffULL << (8 * i);
        }
        break;
    }
    V v(ctx);
    if (!modify) {
        const std::uint64_t value = invert ? ~imm64 : imm64;
        const Xmm d = ctx.write_vec(rd);
        v.load_const(d, Const128{value, q ? value : 0});
        return 1;
    }
    const Xmm dold = ctx.read_vec(rd);
    const Xmm d = ctx.write_vec(rd);
    if (invert) {   // BIC
        v.op(op::pand, d, dold, Const128{~imm64, q ? ~imm64 : 0});
    } else {        // ORR
        v.op(op::por, d, dold, Const128{imm64, imm64});
        v.finish(d, q);
    }
    return 1;
}

} // namespace transmute::fx::neon
