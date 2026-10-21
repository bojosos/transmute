// SIMD&FP loads and stores (owner: neon): LDR/STR B/H/S/D/Q (all addressing
// modes), LDP/STP/LDNP/STNP S/D/Q, LD1-LD4/ST1-ST4 (multiple structures),
// LD1R-LD4R and single-lane LD1-LD4/ST1-ST4, with immediate and register
// post-index.
//
// Fault discipline: every access is preceded by mark_may_fault(), and no
// guest register is written before the last access that can fault. For
// multi-register loads the LAST chunk is loaded first into a temp and the
// FIRST chunk next; once both succeeded every byte in between lies on one of
// those two pages (spans are at most 64 bytes), so the remaining loads can go
// straight into their destinations.
#include "fx/translator/emit_neon_internal.hpp"

#include <vector>

namespace transmute::fx::neon {

namespace {

using x64::ptr;

constexpr SseOp kPextrbMem{0x66, 3, 0x14}, kPextrwMem{0x66, 3, 0x15}, kPextrdMem{0x66, 3, 0x16};
constexpr SseOp kPinsrb{0x66, 3, 0x20}, kPinsrw{0x66, 1, 0xc4}, kPinsrd{0x66, 3, 0x22};

std::int64_t sext(std::uint64_t value, unsigned bits_) {
    const auto shift = 64 - bits_;
    return static_cast<std::int64_t>(value << shift) >> shift;
}

// A scalar register load (B/H/S/D) zeroes the rest of the register.
void note_scalar_load(EmitContext& ctx, unsigned rt, unsigned bytes) {
    if (bytes <= 8) ctx.note_vec_zero_above(rt, bytes <= 4 ? 32 : 64);
}

Mem offset(const Mem& m, std::int32_t delta) {
    Mem r = m;
    r.disp += delta;
    return r;
}

// ---- primitive accesses ----------------------------------------------------------

struct Mover {
    EmitContext& ctx;
    V v;
    x64::Assembler& a;
    explicit Mover(EmitContext& c) : ctx(c), v(c), a(c.as()) {}

    // Loads `bytes` (1/2/4/8/16) from m into the low bits of d, zeroing the
    // rest. For 1/2 bytes `gpr` is a scratch GPR.
    void load(unsigned bytes, Xmm d, const Mem& m, Gp gpr) {
        switch (bytes) {
        case 1:
            ctx.mark_may_fault();
            a.movzx(32, gpr, m, 8);
            a.movd(d, gpr);
            break;
        case 2:
            ctx.mark_may_fault();
            a.movzx(32, gpr, m, 16);
            a.movd(d, gpr);
            break;
        case 4: ctx.mark_may_fault(); a.movd(d, m); break;
        case 8: ctx.mark_may_fault(); a.movq(d, m); break;
        default: ctx.mark_may_fault(); a.movdqu(d, m); break;
        }
    }
    // Code-byte hygiene (see emit_load_store.cpp): a disp32 followed by a zero
    // imm8 (pextrb/pextrw lane 0) can read as an image address to the
    // persistent cache's relocation audit; such byte/halfword stores go
    // through a GPR instead.
    // (TRANSMUTE_FX_NEON=nofold: as before, pextrb/pextrw with any displacement.)
    static bool disp32(const Mem& m) { return neon_on(neon_no_fold) && (m.disp < -128 || m.disp > 127); }
    void store_low_via_gpr(unsigned bytes, const Mem& m, Xmm x) {
        const Gp t = ctx.temp_gpr();
        a.movd(t, x);
        ctx.mark_may_fault();
        a.mov(bytes * 8, m, t);
    }
    void store(unsigned bytes, const Mem& m, Xmm x) {
        if (bytes <= 2 && disp32(m)) {
            store_low_via_gpr(bytes, m, x);
            return;
        }
        ctx.mark_may_fault();
        switch (bytes) {
        case 1: a.sse_raw(kPextrbMem, false, x64::id(x), m, 1); a.db(0); break;
        case 2: a.sse_raw(kPextrwMem, false, x64::id(x), m, 1); a.db(0); break;
        case 4: a.movd(m, x); break;
        case 8: a.movq(m, x); break;
        default: a.movdqu(m, x); break;
        }
    }
    // d = dold with lane `lane` (element size `size`) loaded from m.
    // Legacy forms copy dold into d first; d must then not be a guest register
    // other than the one being loaded (it is: write_vec of the same Vt).
    void insert_lane(unsigned size, Xmm d, Xmm dold, const Mem& m, unsigned lane) {
        if (!v.avx) v.mov(d, dold);
        ctx.mark_may_fault();
        if (size == 3) {
            const SseOp o = lane ? op::movhps_load : op::movlps_load;
            if (v.avx) a.vex_rm(vex_of(o), false, x64::id(d), x64::id(dold), m);
            else a.sse(o, d, m);
            return;
        }
        static constexpr SseOp ops[3] = {kPinsrb, kPinsrw, kPinsrd};
        if (v.avx) a.vex_rm(vex_of(ops[size]), false, x64::id(d), x64::id(dold), m, 1);
        else a.sse_raw(ops[size], false, x64::id(d), m, 1);
        a.db(static_cast<std::uint8_t>(lane));
    }
    void store_lane(unsigned size, const Mem& m, Xmm x, unsigned lane) {
        if (size <= 1 && lane == 0 && disp32(m)) {
            store_low_via_gpr(1u << size, m, x);
            return;
        }
        ctx.mark_may_fault();
        switch (size) {
        case 0: a.sse_raw(kPextrbMem, false, x64::id(x), m, 1); a.db(static_cast<std::uint8_t>(lane)); break;
        case 1: a.sse_raw(kPextrwMem, false, x64::id(x), m, 1); a.db(static_cast<std::uint8_t>(lane)); break;
        case 2:
            if (lane == 0) a.movd(m, x);
            else { a.sse_raw(kPextrdMem, false, x64::id(x), m, 1); a.db(static_cast<std::uint8_t>(lane)); }
            break;
        default:
            if (lane == 0) a.movq(m, x);
            else a.sse(op::movhps_store, m, x);
            break;
        }
    }
    // Loads an element (1/2/4/8 bytes) into a GPR (zero-extended).
    void load_gpr(unsigned size, Gp r, const Mem& m) {
        ctx.mark_may_fault();
        switch (size) {
        case 0: a.movzx(32, r, m, 8); break;
        case 1: a.movzx(32, r, m, 16); break;
        case 2: a.mov(32, r, m); break;
        default: a.mov(64, r, m); break;
        }
    }
    // d = dold with lane `lane` replaced by the GPR value.
    void insert_gpr(unsigned size, Xmm d, Xmm dold, Gp r, unsigned lane) {
        if (v.avx) {
            static constexpr VexOp ops[4] = {{1, 3, 0x20, false}, {1, 1, 0xc4, false},
                                             {1, 3, 0x22, false}, {1, 3, 0x22, true}};
            a.vex_rr(ops[size], false, x64::id(d), x64::id(dold), x64::id(r));
            a.db(static_cast<std::uint8_t>(lane));
            return;
        }
        v.mov(d, dold);
        a.pinsr(8u << size, d, r, static_cast<std::uint8_t>(lane));
    }
};

// Address of an access without base writeback, [Xn|SP + disp] or
// [Xn|SP + ext(Xm) << shift]: EmitContext::address, which may fold a
// preceding ADD/SUB into the operand and knows extension facts
// (TRANSMUTE_FX_NEON=nofold: the default lowering, registers read as before).
Mem guest_address(EmitContext& ctx, const EmitContext::GuestAddress& g) {
    return neon_on(neon_no_fold) ? ctx.address(g) : ctx.EmitContext::address(g);
}
Mem base_offset(EmitContext& ctx, unsigned rn, std::int64_t disp) {
    EmitContext::GuestAddress g;
    g.base = rn;
    g.disp = static_cast<std::int32_t>(disp);
    return guest_address(ctx, g);
}
Mem register_offset(EmitContext& ctx, unsigned rn, unsigned rm, unsigned option, unsigned shift) {
    EmitContext::GuestAddress g;
    g.base = rn;
    g.index = rm;
    g.option = option;
    g.shift = shift;
    return guest_address(ctx, g);
}

void writeback_imm(EmitContext& ctx, unsigned rn, Gp base, std::int64_t imm) {
    const Gp w = ctx.write_gpr(rn, true);
    ctx.as().lea(64, w, ptr(base, static_cast<std::int32_t>(imm)));
}

void writeback_reg(EmitContext& ctx, unsigned rn, Gp base, Gp offset_reg) {
    const Gp w = ctx.write_gpr(rn, true);
    ctx.as().lea(64, w, ptr(base, offset_reg, 1));
}

// ============================================================================
// LDR/STR (immediate, unscaled, pre/post-index, register offset, literal)
// ============================================================================

unsigned single_register(EmitContext& ctx, std::uint32_t w) {
    const unsigned size = bits(w, 31, 30), opc = bits(w, 23, 22);
    const unsigned rn = bits(w, 9, 5), rt = bits(w, 4, 0);
    // Access size: size:opc<1> -> 1,2,4,8 or 16 bytes.
    unsigned bytes = 0;
    if ((opc & 2) == 0) bytes = 1u << size;
    else if (size == 0) bytes = 16;
    else return 0;
    const bool load = (opc & 1) != 0;

    enum class Mode { unsigned_imm, unscaled, post, pre, reg };
    Mode mode;
    std::int64_t imm = 0;
    unsigned option = 0, shift = 0, rm = 0;
    if ((w & 0x3f000000u) == 0x3d000000u) {
        mode = Mode::unsigned_imm;
        imm = static_cast<std::int64_t>(bits(w, 21, 10)) * bytes;
    } else if ((w & 0x3f200000u) == 0x3c000000u) {
        imm = sext(bits(w, 20, 12), 9);
        switch (bits(w, 11, 10)) {
        case 0: mode = Mode::unscaled; break;
        case 1: mode = Mode::post; break;
        case 3: mode = Mode::pre; break;
        default: return 0;
        }
    } else if ((w & 0x3f200c00u) == 0x3c200800u) {
        mode = Mode::reg;
        option = bits(w, 15, 13);
        if ((option & 2) == 0) return 0;
        rm = bits(w, 20, 16);
        const unsigned log2 = bytes == 16 ? 4 : size;
        shift = bit(w, 12) ? log2 : 0;
    } else {
        return 0;
    }

    Mover mv(ctx);
    const bool writeback = mode == Mode::post || mode == Mode::pre;
    const Gp base = writeback ? ctx.read_gpr(rn, true) : Gp::none;
    Mem m;
    switch (mode) {
    case Mode::unsigned_imm: case Mode::unscaled: m = base_offset(ctx, rn, imm); break;
    case Mode::pre: m = ptr(base, static_cast<std::int32_t>(imm)); break;
    case Mode::post: m = ptr(base); break;
    case Mode::reg: m = register_offset(ctx, rn, rm, option, shift); break;
    }
    if (load) {
        const Gp g = bytes <= 2 ? ctx.temp_gpr() : Gp::none;
        const Xmm d = ctx.write_vec(rt);
        mv.load(bytes, d, m, g);
        note_scalar_load(ctx, rt, bytes);
    } else {
        const Xmm x = ctx.read_vec(rt);
        mv.store(bytes, m, x);
    }
    if (writeback) writeback_imm(ctx, rn, base, imm);
    return 1;
}

unsigned load_literal(EmitContext& ctx, std::uint32_t w) {
    const unsigned opc = bits(w, 31, 30), rt = bits(w, 4, 0);
    if (opc == 3) return 0;
    const unsigned bytes = 4u << opc;
    const auto address = ctx.pc() + static_cast<std::uint64_t>(sext(bits(w, 23, 5), 19) * 4);
    Mover mv(ctx);
    // Absolute guest address: EmitContext::guest_address records the fixup.
    const Gp t = ctx.guest_address(address);
    const Xmm d = ctx.write_vec(rt);
    mv.load(bytes, d, ptr(t), Gp::none);
    note_scalar_load(ctx, rt, bytes);
    return 1;
}

// ============================================================================
// LDP/STP/LDNP/STNP
// ============================================================================

unsigned pair(EmitContext& ctx, std::uint32_t w) {
    const unsigned opc = bits(w, 31, 30), mode = bits(w, 24, 23);
    const bool load = bit(w, 22);
    if (opc == 3) return 0;
    const unsigned bytes = 4u << opc;
    const std::int64_t imm = sext(bits(w, 21, 15), 7) * bytes;
    const unsigned rt2 = bits(w, 14, 10), rn = bits(w, 9, 5), rt = bits(w, 4, 0);
    Mover mv(ctx);
    const bool writeback = mode == 1 || mode == 3;
    const Gp base = writeback ? ctx.read_gpr(rn, true) : Gp::none;
    const Mem m = mode == 1 ? ptr(base) : writeback ? ptr(base, static_cast<std::int32_t>(imm)) : base_offset(ctx, rn, imm);
    const Mem m2 = offset(m, static_cast<std::int32_t>(bytes));
    if (load) {
        // The first value goes through a temp unless its destination is only
        // committed at the end of the instruction (rule 2).
        const bool direct = ctx.vec_write_deferred(rt) && rt != rt2;
        const Xmm t = direct ? Xmm{} : ctx.temp_vec();
        const Xmm d1 = ctx.write_vec(rt);
        const Xmm d2 = ctx.write_vec(rt2);
        mv.load(bytes, direct ? d1 : t, m, Gp::none);
        mv.load(bytes, d2, m2, Gp::none);
        if (!direct) mv.v.mov(d1, t);
        note_scalar_load(ctx, rt, bytes);
        note_scalar_load(ctx, rt2, bytes);
    } else {
        const Xmm x1 = ctx.read_vec(rt);
        const Xmm x2 = ctx.read_vec(rt2);
        mv.store(bytes, m, x1);
        mv.store(bytes, m2, x2);
    }
    if (writeback) writeback_imm(ctx, rn, base, imm);
    return 1;
}

// ============================================================================
// Structure shuffles (LD2/LD3/LD4, ST2/ST3/ST4)
// ============================================================================

// pshufb control gathering, for de-interleaving `count` registers of
// `reg_bytes` bytes with `esize`-byte elements, the bytes of output `j` that
// live in 16-byte memory chunk `chunk`.
Const128 gather_control(unsigned count, unsigned esize, unsigned reg_bytes, unsigned j, unsigned chunk,
                        bool& any) {
    std::uint8_t c[16];
    any = false;
    for (unsigned p = 0; p < 16; ++p) {
        c[p] = 0x80;
        if (p >= reg_bytes) continue;
        const unsigned off = ((p / esize) * count + j) * esize + p % esize;
        if (off / 16 == chunk) {
            c[p] = static_cast<std::uint8_t>(off % 16);
            any = true;
        }
    }
    return bytes_const(c);
}

// Control building memory chunk `chunk` from source register j (interleave).
Const128 scatter_control(unsigned count, unsigned esize, unsigned reg_bytes, unsigned chunk, unsigned j,
                         bool& any) {
    std::uint8_t c[16];
    any = false;
    const unsigned total = count * reg_bytes;
    for (unsigned p = 0; p < 16; ++p) {
        c[p] = 0x80;
        const unsigned off = chunk * 16 + p;
        if (off >= total) continue;
        const unsigned k = off / esize, b = off % esize;
        if (k % count == j) {
            c[p] = static_cast<std::uint8_t>((k / count) * esize + b);
            any = true;
        }
    }
    return bytes_const(c);
}

// Generic de-interleave: out[j] = OR over chunks of pshufb(chunk, control).
void gather(V& v, unsigned count, unsigned size, bool q, const Xmm* chunks, unsigned nchunks,
            const Xmm* out) {
    const unsigned esize = 1u << size, reg_bytes = q ? 16 : 8;
    const Xmm t = v.temp();
    for (unsigned j = 0; j < count; ++j) {
        bool first = true;
        for (unsigned c = 0; c < nchunks; ++c) {
            bool any = false;
            const Const128 control = gather_control(count, esize, reg_bytes, j, c, any);
            if (!any) continue;
            if (first) {
                v.pshufb(out[j], chunks[c], control);
                first = false;
            } else {
                v.pshufb(t, chunks[c], control);
                v.por(out[j], out[j], t);
            }
        }
    }
}

void scatter(V& v, unsigned count, unsigned size, bool q, const Xmm* src, unsigned nchunks,
             const Xmm* chunks) {
    const unsigned esize = 1u << size, reg_bytes = q ? 16 : 8;
    const Xmm t = v.temp();
    for (unsigned c = 0; c < nchunks; ++c) {
        bool first = true;
        for (unsigned j = 0; j < count; ++j) {
            bool any = false;
            const Const128 control = scatter_control(count, esize, reg_bytes, c, j, any);
            if (!any) continue;
            if (first) {
                v.pshufb(chunks[c], src[j], control);
                first = false;
            } else {
                v.pshufb(t, src[j], control);
                v.por(chunks[c], chunks[c], t);
            }
        }
    }
}

// Groups the 4 components of each 32-bit... (LD4/ST4 helpers): per chunk, bytes
// of component c gathered into dword c (B: 4 elements, H: 2 elements).
const Const128 kGroup4[2] = {
    bytes_const({0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15}),
    bytes_const({0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6, 7, 14, 15}),
};
// Inverses for ST4.
const Const128 kUngroup4[2] = {
    bytes_const({0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15}),
    bytes_const({0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15}),
};

// 4x4 transpose of 32-bit lanes: out[k] = {in0[k], in1[k], in2[k], in3[k]}.
// `in` must not alias out[0] or out[1].
void transpose4(V& v, const Xmm* in, const Xmm* out, Xmm t0, Xmm t1) {
    v.op(op::punpckldq, t0, in[0], in[1]);   // a0 b0 a1 b1
    v.op(op::punpckldq, t1, in[2], in[3]);   // c0 d0 c1 d1
    v.op(op::punpcklqdq, out[0], t0, t1);
    v.op(op::punpckhqdq, out[1], t0, t1);
    v.op(op::punpckhdq, t0, in[0], in[1]);   // a2 b2 a3 b3
    v.op(op::punpckhdq, t1, in[2], in[3]);   // c2 d2 c3 d3
    v.op(op::punpcklqdq, out[2], t0, t1);
    v.op(op::punpckhqdq, out[3], t0, t1);
}

// ============================================================================
// LD1-LD4 / ST1-ST4 (multiple structures)
// ============================================================================

unsigned multiple(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), load = bit(w, 22), post = bit(w, 23);
    const unsigned rm = bits(w, 20, 16), opcode = bits(w, 15, 12), size = bits(w, 11, 10);
    const unsigned rn = bits(w, 9, 5), rt = bits(w, 4, 0);
    unsigned regs = 0, structure = 0;   // structure: elements interleaved (1 = LD1)
    switch (opcode) {
    case 0x0: regs = 4; structure = 4; break;
    case 0x2: regs = 4; structure = 1; break;
    case 0x4: regs = 3; structure = 3; break;
    case 0x6: regs = 3; structure = 1; break;
    case 0x7: regs = 1; structure = 1; break;
    case 0x8: regs = 2; structure = 2; break;
    case 0xa: regs = 2; structure = 1; break;
    default: return 0;
    }
    if (structure > 1 && size == 3 && !q) return 0;
    const unsigned reg_bytes = q ? 16 : 8;
    const unsigned total = regs * reg_bytes;

    Mover mv(ctx);
    V& v = mv.v;
    const Gp base = post ? ctx.read_gpr(rn, true) : Gp::none;
    const Gp offset_reg = post && rm != 31 ? ctx.read_gpr(rm) : Gp::none;
    const Mem m = post ? ptr(base) : base_offset(ctx, rn, 0);
    auto reg = [&](unsigned i) { return (rt + i) & 31; };

    if (!load) {
        Xmm x[4];
        for (unsigned i = 0; i < regs; ++i) x[i] = ctx.read_vec(reg(i));
        if (structure == 1) {
            for (unsigned i = 0; i < regs; ++i)
                mv.store(reg_bytes, offset(m, static_cast<std::int32_t>(i * reg_bytes)), x[i]);
        } else if (structure == 2) {
            const Xmm lo = v.temp();
            v.op(kUnpackLo[size], lo, x[0], x[1]);
            mv.store(16, m, lo);
            if (q) {
                v.op(kUnpackHi[size], lo, x[0], x[1]);
                mv.store(16, offset(m, 16), lo);
            }
        } else if (structure == 4) {
            Xmm c[4] = {v.temp(), v.temp(), v.temp(), v.temp()};
            const Xmm t0 = v.temp(), t1 = v.temp();
            if (size == 3) {
                v.op(op::punpcklqdq, c[0], x[0], x[1]);
                v.op(op::punpcklqdq, c[1], x[2], x[3]);
                v.op(op::punpckhqdq, c[2], x[0], x[1]);
                v.op(op::punpckhqdq, c[3], x[2], x[3]);
                for (unsigned i = 0; i < 4; ++i) mv.store(16, offset(m, static_cast<std::int32_t>(16 * i)), c[i]);
            } else if (q) {
                transpose4(v, x, c, t0, t1);
                for (unsigned i = 0; i < 4; ++i) {
                    if (size < 2) v.pshufb(c[i], c[i], kUngroup4[size]);
                    mv.store(16, offset(m, static_cast<std::int32_t>(16 * i)), c[i]);
                }
            } else {
                // 64-bit registers: two 16-byte chunks.
                v.op(op::punpckldq, t0, x[0], x[1]);
                v.op(op::punpckldq, t1, x[2], x[3]);
                v.op(op::punpcklqdq, c[0], t0, t1);
                v.op(op::punpckhqdq, c[1], t0, t1);
                for (unsigned i = 0; i < 2; ++i) {
                    if (size < 2) v.pshufb(c[i], c[i], kUngroup4[size]);
                    mv.store(16, offset(m, static_cast<std::int32_t>(16 * i)), c[i]);
                }
            }
        } else {   // ST3
            const unsigned nchunks = (total + 15) / 16;
            Xmm c[3] = {v.temp(), v.temp(), v.temp()};
            if (size == 3) {
                v.op(op::punpcklqdq, c[0], x[0], x[1]);
                v.op_imm(op::pblendw, c[1], x[2], x[0], 0xf0);
                v.op(op::punpckhqdq, c[2], x[1], x[2]);
            } else {
                scatter(v, 3, size, q, x, nchunks, c);
            }
            for (unsigned i = 0; i < nchunks; ++i) {
                const unsigned bytes = std::min(16u, total - 16 * i);
                mv.store(bytes, offset(m, static_cast<std::int32_t>(16 * i)), c[i]);
            }
        }
    } else {
        Xmm d[4];
        if (structure == 1) {
            // Last chunk first into a temp, then the others straight into
            // their destinations; all in order when every destination is
            // committed at the end of the instruction.
            bool direct = regs > 1;
            for (unsigned i = 0; i < regs && direct; ++i) direct = ctx.vec_write_deferred(reg(i));
            const Xmm t = regs > 1 && !direct ? ctx.temp_vec() : Xmm{};
            for (unsigned i = 0; i < regs; ++i) d[i] = ctx.write_vec(reg(i));
            if (regs == 1) {
                mv.load(reg_bytes, d[0], m, Gp::none);
            } else if (direct) {
                for (unsigned i = 0; i < regs; ++i)
                    mv.load(reg_bytes, d[i], offset(m, static_cast<std::int32_t>(i * reg_bytes)), Gp::none);
            } else {
                mv.load(reg_bytes, t, offset(m, static_cast<std::int32_t>((regs - 1) * reg_bytes)), Gp::none);
                for (unsigned i = 0; i + 1 < regs; ++i)
                    mv.load(reg_bytes, d[i], offset(m, static_cast<std::int32_t>(i * reg_bytes)), Gp::none);
                v.mov(d[regs - 1], t);
            }
        } else {
            const unsigned nchunks = (total + 15) / 16;
            Xmm c[4];
            for (unsigned i = 0; i < nchunks; ++i) c[i] = ctx.temp_vec();
            // Load the last chunk first (the only partial one for 64-bit LD3).
            for (unsigned k = nchunks; k-- > 0;) {
                const unsigned bytes = std::min(16u, total - 16 * k);
                mv.load(bytes, c[k], offset(m, static_cast<std::int32_t>(16 * k)), Gp::none);
            }
            for (unsigned i = 0; i < regs; ++i) d[i] = ctx.write_vec(reg(i));
            if (structure == 2) {
                if (q) {
                    if (size == 3) {
                        v.op(op::punpcklqdq, d[0], c[0], c[1]);
                        v.op(op::punpckhqdq, d[1], c[0], c[1]);
                    } else if (size == 2) {
                        v.op_imm(op::shufps, d[0], c[0], c[1], 0x88);
                        v.op_imm(op::shufps, d[1], c[0], c[1], 0xdd);
                    } else {
                        static const Const128 eo[2] = {
                            bytes_const({0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15}),
                            bytes_const({0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15}),
                        };
                        v.pshufb(c[0], c[0], eo[size]);
                        v.pshufb(c[1], c[1], eo[size]);
                        v.op(op::punpcklqdq, d[0], c[0], c[1]);
                        v.op(op::punpckhqdq, d[1], c[0], c[1]);
                    }
                } else {
                    gather(v, 2, size, false, c, 1, d);
                }
            } else if (structure == 4) {
                if (size == 3) {
                    v.op(op::punpcklqdq, d[0], c[0], c[2]);
                    v.op(op::punpckhqdq, d[1], c[0], c[2]);
                    v.op(op::punpcklqdq, d[2], c[1], c[3]);
                    v.op(op::punpckhqdq, d[3], c[1], c[3]);
                } else if (q) {
                    if (size < 2)
                        for (unsigned i = 0; i < 4; ++i) v.pshufb(c[i], c[i], kGroup4[size]);
                    const Xmm t0 = v.temp(), t1 = v.temp();
                    transpose4(v, c, d, t0, t1);
                } else {
                    if (size < 2)
                        for (unsigned i = 0; i < 2; ++i) v.pshufb(c[i], c[i], kGroup4[size]);
                    const Xmm t0 = v.temp(), t1 = v.temp();
                    v.op(op::punpckldq, t0, c[0], c[1]);
                    v.op(op::punpckhdq, t1, c[0], c[1]);
                    v.movq(d[0], t0);
                    v.psrldq(d[1], t0, 8);
                    v.movq(d[2], t1);
                    v.psrldq(d[3], t1, 8);
                }
            } else {   // LD3
                if (size == 3) {
                    v.op_imm(op::pblendw, d[0], c[0], c[1], 0xf0);
                    v.op_imm(op::palignr, d[1], c[2], c[0], 8);
                    v.op_imm(op::pblendw, d[2], c[1], c[2], 0xf0);
                } else {
                    gather(v, 3, size, q, c, nchunks, d);
                }
            }
        }
    }
    if (post) {
        if (rm == 31) writeback_imm(ctx, rn, base, total);
        else writeback_reg(ctx, rn, base, offset_reg);
    }
    return 1;
}

// ============================================================================
// Single structure: lanes and replicate (LD1R..LD4R)
// ============================================================================

unsigned single(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), load = bit(w, 22), r = bit(w, 21), post = bit(w, 23);
    const unsigned rm = bits(w, 20, 16), opcode = bits(w, 15, 13), s = bits(w, 12, 12);
    const unsigned size = bits(w, 11, 10), rn = bits(w, 9, 5), rt = bits(w, 4, 0);
    const unsigned regs = ((opcode & 1) ? 3u : 1u) + (r ? 1u : 0u);
    auto reg = [&](unsigned i) { return (rt + i) & 31; };

    if (opcode >= 6) {   // replicate
        if (!load || s) return 0;
        const unsigned esize = 1u << size;
        Mover mv(ctx);
        V& v = mv.v;
        const Gp base = post ? ctx.read_gpr(rn, true) : Gp::none;
        const Gp offset_reg = post && rm != 31 ? ctx.read_gpr(rm) : Gp::none;
        const Mem m = post ? ptr(base) : base_offset(ctx, rn, 0);
        // Broadcast element i into register d (upper half zero when !q).
        auto broadcast_mem = [&](Xmm d, const Mem& em, Gp g) {
            if (size == 3) {
                ctx.mark_may_fault();
                if (q) v.un(op::movddup, d, em);
                else ctx.as().movq(d, em);
                return;
            }
            if (v.avx2) {
                static constexpr VexOp ops[3] = {vop::vpbroadcastb, vop::vpbroadcastw, vop::vpbroadcastd};
                ctx.mark_may_fault();
                v.vex2(ops[size], d, em);
                if (!q) v.movq(d, d);
                return;
            }
            mv.load(esize, d, em, g);
            switch (size) {
            case 0: {
                std::uint8_t c[16];
                for (unsigned i = 0; i < 16; ++i) c[i] = (q || i < 8) ? 0 : 0x80;
                v.pshufb(d, d, bytes_const(c));
                break;
            }
            case 1:
                v.un_imm(op::pshuflw, d, d, 0);
                if (q) v.pshufd(d, d, 0x44);
                break;
            default: v.pshufd(d, d, q ? 0x00 : 0x50); break;
            }
        };
        const Gp g = size < 2 ? ctx.temp_gpr() : Gp::none;
        Xmm d[4];
        const Xmm t = regs > 1 ? ctx.temp_vec() : Xmm{};
        for (unsigned i = 0; i < regs; ++i) d[i] = ctx.write_vec(reg(i));
        if (regs == 1) {
            broadcast_mem(d[0], m, g);
        } else {
            broadcast_mem(t, offset(m, static_cast<std::int32_t>((regs - 1) * esize)), g);
            for (unsigned i = 0; i + 1 < regs; ++i)
                broadcast_mem(d[i], offset(m, static_cast<std::int32_t>(i * esize)), g);
            v.mov(d[regs - 1], t);
        }
        if (post) {
            if (rm == 31) writeback_imm(ctx, rn, base, regs * esize);
            else writeback_reg(ctx, rn, base, offset_reg);
        }
        return 1;
    }

    unsigned esize_log = 0, lane = 0;
    switch (opcode >> 1) {
    case 0: esize_log = 0; lane = (q << 3) | (s << 2) | size; break;
    case 1:
        if (size & 1) return 0;
        esize_log = 1; lane = (q << 2) | (s << 1) | (size >> 1);
        break;
    default:
        if (size == 0) { esize_log = 2; lane = (q << 1) | s; }
        else if (size == 1 && s == 0) { esize_log = 3; lane = q; }
        else return 0;
        break;
    }
    const unsigned esize = 1u << esize_log;
    Mover mv(ctx);
    const Gp base = post ? ctx.read_gpr(rn, true) : Gp::none;
    const Gp offset_reg = post && rm != 31 ? ctx.read_gpr(rm) : Gp::none;
    const Mem m = post ? ptr(base) : base_offset(ctx, rn, 0);
    if (load) {
        Xmm dold[4], d[4];
        for (unsigned i = 0; i < regs; ++i) dold[i] = ctx.read_vec(reg(i));
        const Gp g = regs > 1 ? ctx.temp_gpr() : Gp::none;
        // Last element first into a GPR; the others insert from memory.
        if (regs > 1) mv.load_gpr(esize_log, g, offset(m, static_cast<std::int32_t>((regs - 1) * esize)));
        for (unsigned i = 0; i < regs; ++i) d[i] = ctx.write_vec(reg(i));
        for (unsigned i = 0; i + 1 < regs || (regs == 1 && i == 0); ++i)
            mv.insert_lane(esize_log, d[i], dold[i], offset(m, static_cast<std::int32_t>(i * esize)), lane);
        if (regs > 1) mv.insert_gpr(esize_log, d[regs - 1], dold[regs - 1], g, lane);
    } else {
        for (unsigned i = 0; i < regs; ++i) {
            const Xmm x = ctx.read_vec(reg(i));
            mv.store_lane(esize_log, offset(m, static_cast<std::int32_t>(i * esize)), x, lane);
        }
    }
    if (post) {
        if (rm == 31) writeback_imm(ctx, rn, base, regs * esize);
        else writeback_reg(ctx, rn, base, offset_reg);
    }
    return 1;
}

} // namespace

// FamilyEmitter::address_base: the forms whose base goes through
// base_offset()/register_offset() only (no writeback).
bool address_base(std::uint32_t w, unsigned& base, bool& indexed) {
    if (!neon_on(neon_no_fold)) return false;
    base = bits(w, 9, 5);
    indexed = false;
    if ((w & 0x3f000000u) == 0x3d000000u) return true;                                  // LDR/STR (unsigned offset)
    if ((w & 0x3f200c00u) == 0x3c000000u) return true;                                  // LDUR/STUR
    if ((w & 0x3f200c00u) == 0x3c200800u) {                                             // register offset
        indexed = true;
        return bits(w, 20, 16) != base;
    }
    if ((w & 0x3e000000u) == 0x2c000000u) return (bits(w, 24, 23) & 1u) == 0;          // LDP/STP/LDNP/STNP offset
    if ((w & 0xbfbf0000u) == 0x0c000000u || (w & 0xbf9f0000u) == 0x0d000000u) return true;   // LDn/STn, no post-index
    return false;
}

unsigned dispatch_memory(EmitContext& ctx, std::uint32_t w) {
    // Loads and stores with V = 1 (bit 26).
    if ((w & 0x3f000000u) == 0x3d000000u || (w & 0x3f200000u) == 0x3c000000u ||
        (w & 0x3f200c00u) == 0x3c200800u)
        return single_register(ctx, w);
    if ((w & 0x3f000000u) == 0x1c000000u) return load_literal(ctx, w);
    if ((w & 0x3e000000u) == 0x2c000000u) return pair(ctx, w);
    if ((w & 0xbfbf0000u) == 0x0c000000u || (w & 0xbfa00000u) == 0x0c800000u) return multiple(ctx, w);
    if ((w & 0xbf9f0000u) == 0x0d000000u || (w & 0xbf800000u) == 0x0d800000u) return single(ctx, w);
    return 0;
}

} // namespace transmute::fx::neon
