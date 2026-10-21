// CRC32/CRC32C, AES, PMULL(2).1Q, SHA1/SHA256 (SHA-NI or helper), SHA512
// (helper) and SHA3 EOR3/BCAX/RAX1/XAR (owner: atomics/system).
#include "fx/translator/emit_system_internal.hpp"

namespace transmute::fx::system_detail {
namespace {

using x64::SseOp;

constexpr SseOp kPalignr{0x66, 3, 0x0f};
constexpr SseOp kPshufd{0x66, 1, 0x70};
constexpr SseOp kShufps{0, 1, 0xc6};
constexpr SseOp kPaddd{0x66, 1, 0xfe};
constexpr SseOp kPsubd{0x66, 1, 0xfa};
constexpr SseOp kPor{0x66, 1, 0xeb};
constexpr SseOp kPand{0x66, 1, 0xdb};
constexpr SseOp kPandn{0x66, 1, 0xdf};
constexpr SseOp kSha1rnds4{0, 3, 0xcc};
constexpr SseOp kSha256rnds2{0, 2, 0xcb};
constexpr SseOp kSha256msg1{0, 2, 0xcc};
constexpr SseOp kSha256msg2{0, 2, 0xcd};

// Reflected CRC-32 (ISO-HDLC) Barrett constants: P' = bitrev33(0x104C11DB7),
// mu' = bitrev33(floor(x^64 / P)).
constexpr std::uint64_t kCrcPoly = 0x1DB710641ULL;
constexpr std::uint64_t kCrcMu = 0x1F7011641ULL;

void movdqa_rr(x64::Assembler& a, Xmm dst, Xmm src) {
    if (dst != src) a.movaps(dst, src);
}

void call_crypto_helper(EmitContext& ctx, std::uint32_t w) {
    ctx.call_helper(helper_system_crypto, {HelperArg::imm(w)}, HelperEffects::reads_writes_state);
}

// One 32-bit Barrett step: value = hi32(clmul(lo32(clmul(value, mu)), P)).
// `value` holds the 32-bit operand (already xored and positioned).
void crc_barrett(x64::Assembler& a, Gp value, Gp scratch, Xmm x1, Xmm x2) {
    a.movq(x1, value);
    a.mov_imm(scratch, kCrcMu);
    a.movq(x2, scratch);
    a.sse_imm(x64::sse::pclmulqdq, x1, x2, 0x00);
    a.movd(value, x1);                 // q = low 32 bits
    a.movd(x1, value);
    a.mov_imm(scratch, kCrcPoly);
    a.movq(x2, scratch);
    a.sse_imm(x64::sse::pclmulqdq, x1, x2, 0x00);
    a.movq(value, x1);
    a.shr(64, value, 32);
}

unsigned emit_crc(EmitContext& ctx, std::uint32_t w) {
    auto& a = ctx.as();
    const unsigned sz = (w >> 10) & 3u;
    const bool sf = (w & 0x80000000u) != 0;
    const bool castagnoli = (w & (1u << 12)) != 0;
    if (sf != (sz == 3)) return 0;
    const unsigned bytes = 1u << sz;
    const unsigned rd = rd_of(w), rn = rn_of(w), rm = rm_of(w);
    if (castagnoli) {   // SSE4.2 crc32 is CRC32C with the same bit order
        const Gp acc = ctx.copy_gpr(rn);
        const Gp data = ctx.read_gpr(rm);
        a.crc32(acc, data, bytes * 8u);
        const Gp d = ctx.write_gpr(rd);
        a.mov(32, d, acc);
        return 1;
    }
    if (!host().pclmul) {
        const Gp r = ctx.call_helper(helper_system_crc32,
                                     {HelperArg::guest(rn), HelperArg::guest(rm), HelperArg::imm(bytes)},
                                     HelperEffects::pure);
        const Gp d = ctx.write_gpr(rd);
        a.mov(32, d, r);
        return 1;
    }
    const Gp acc = ctx.read_gpr(rn);
    const Gp data = ctx.read_gpr(rm);
    const Gp value = ctx.temp_gpr(), rest = ctx.temp_gpr(), scratch = ctx.temp_gpr();
    const Xmm x1 = ctx.temp_vec(), x2 = ctx.temp_vec();
    ctx.clobber_host_flags();
    a.mov(32, value, acc);
    a.xor_(32, value, data);
    if (bytes < 4) {
        a.movzx(32, value, value, bytes * 8u);
        a.shl(32, value, static_cast<std::uint8_t>(32 - bytes * 8u));
        a.mov(32, rest, acc);
        a.shr(32, rest, static_cast<std::uint8_t>(bytes * 8u));
        crc_barrett(a, value, scratch, x1, x2);
        a.xor_(32, value, rest);
    } else {
        a.mov(32, value, value);
        if (bytes == 8) a.mov(64, rest, data);   // keep the high half
        crc_barrett(a, value, scratch, x1, x2);
        if (bytes == 8) {
            a.shr(64, rest, 32);
            a.xor_(32, value, rest);
            crc_barrett(a, value, scratch, x1, x2);
        }
    }
    const Gp d = ctx.write_gpr(rd);
    a.mov(32, d, value);
    return 1;
}

bool is_aes(std::uint32_t w, unsigned opcode) { return (w & 0xfffffc00u) == (0x4e284800u | (opcode << 12)); }

unsigned emit_aes(EmitContext& ctx, std::span<const std::uint32_t> words) {
    const auto w = words[0];
    const unsigned opcode = (w >> 12) & 7u;   // 4 AESE, 5 AESD, 6 AESMC, 7 AESIMC
    const unsigned rd = rd_of(w), rn = rn_of(w);
    if (!host().aes) {
        call_crypto_helper(ctx, w);
        return 1;
    }
    auto& a = ctx.as();
    const Xmm t = ctx.temp_vec(), zero = ctx.temp_vec();
    if (opcode == 4 || opcode == 5) {
        const unsigned follow = opcode == 4 ? 6u : 7u;
        const bool fuse = fusion_enabled() && words.size() > 1 && is_aes(words[1], follow) &&
                          rd_of(words[1]) == rd && rn_of(words[1]) == rd;
        const Xmm d = ctx.read_vec(rd), n = ctx.read_vec(rn);
        movdqa_rr(a, t, d);
        a.pxor(t, n);
        a.pxor(zero, zero);
        if (opcode == 4) a.sse(fuse ? x64::sse::aesenc : x64::sse::aesenclast, t, zero);
        else a.sse(fuse ? x64::sse::aesdec : x64::sse::aesdeclast, t, zero);
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, t);
        return fuse ? 2 : 1;
    }
    const Xmm n = ctx.read_vec(rn);
    if (opcode == 6) {   // MixColumns = aesenc(aesdeclast(x, 0), 0)
        a.pxor(zero, zero);
        movdqa_rr(a, t, n);
        a.sse(x64::sse::aesdeclast, t, zero);
        a.sse(x64::sse::aesenc, t, zero);
    } else {
        a.sse(x64::sse::aesimc, t, n);
    }
    const Xmm out = ctx.write_vec(rd);
    movdqa_rr(a, out, t);
    return 1;
}

unsigned emit_pmull(EmitContext& ctx, std::uint32_t w) {
    if (!host().pclmul) {
        call_crypto_helper(ctx, w);
        return 1;
    }
    auto& a = ctx.as();
    const bool upper = (w & 0x40000000u) != 0;
    const Xmm t = ctx.temp_vec();
    const Xmm n = ctx.read_vec(rn_of(w)), m = ctx.read_vec(rm_of(w));
    movdqa_rr(a, t, n);
    a.sse_imm(x64::sse::pclmulqdq, t, m, upper ? 0x11 : 0x00);
    const Xmm out = ctx.write_vec(rd_of(w));
    movdqa_rr(a, out, t);
    return 1;
}

constexpr std::uint32_t kSha1K[3] = {0x5a827999u, 0x6ed9eba1u, 0x8f1bbcdcu};

unsigned emit_sha3reg(EmitContext& ctx, std::uint32_t w) {
    auto& a = ctx.as();
    const unsigned opcode = (w >> 12) & 7u;
    const unsigned rd = rd_of(w), rn = rn_of(w), rm = rm_of(w);
    if (opcode == 7) return 0;
    if (opcode == 3) {   // SHA1SU0: (n.lo : d.hi) ^ d ^ m
        const Xmm t = ctx.temp_vec();
        const Xmm d = ctx.read_vec(rd), n = ctx.read_vec(rn), m = ctx.read_vec(rm);
        movdqa_rr(a, t, n);
        a.sse_imm(kPalignr, t, d, 8);
        a.pxor(t, d);
        a.pxor(t, m);
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, t);
        return 1;
    }
    if (!host().sha) {
        call_crypto_helper(ctx, w);
        return 1;
    }
    if (opcode <= 2) {   // SHA1C / SHA1P / SHA1M
        const Xmm abcd = ctx.temp_vec(), msg = ctx.temp_vec(), k = ctx.temp_vec();
        const Gp g = ctx.temp_gpr();
        const Xmm x = ctx.read_vec(rd), e = ctx.read_vec(rn), wk = ctx.read_vec(rm);
        // x64 adds K itself and wants W0+E in the top lane, lanes reversed.
        movdqa_rr(a, msg, wk);
        a.mov_imm(g, kSha1K[opcode]);
        a.movd(k, g);
        a.sse_imm(kPshufd, k, k, 0x00);
        a.sse(kPsubd, msg, k);
        a.movd(g, e);
        a.movd(k, g);
        a.sse(kPaddd, msg, k);
        a.sse_imm(kPshufd, msg, msg, 0x1b);
        a.sse_imm(kPshufd, abcd, x, 0x1b);
        a.sse_imm(kSha1rnds4, abcd, msg, static_cast<std::uint8_t>(opcode));
        const Xmm out = ctx.write_vec(rd);
        a.sse_imm(kPshufd, out, abcd, 0x1b);
        return 1;
    }
    if (opcode == 4 || opcode == 5) {   // SHA256H / SHA256H2
        const bool part1 = opcode == 4;
        FixedXmm0 fx(ctx, 3);
        const Xmm abef = fx.temp(0), cdgh = fx.temp(1), result = fx.temp(2);
        Xmm x = ctx.read_vec(part1 ? rd : rn);
        Xmm y = ctx.read_vec(part1 ? rn : rd);
        Xmm wk = ctx.read_vec(rm);
        fx.save({&x, &y, &wk});
        movdqa_rr(a, abef, y);
        a.sse_imm(kShufps, abef, x, 0x11);   // [f e b a]
        movdqa_rr(a, cdgh, y);
        a.sse_imm(kShufps, cdgh, x, 0xbb);   // [h g d c]
        movdqa_rr(a, Xmm::xmm0, wk);
        a.sse(kSha256rnds2, cdgh, abef);
        a.sse_imm(kPshufd, Xmm::xmm0, wk, 0x0e);
        a.sse(kSha256rnds2, abef, cdgh);
        movdqa_rr(a, result, abef);
        a.sse_imm(kShufps, result, cdgh, part1 ? 0xbb : 0x11);
        fx.restore();
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, result);
        return 1;
    }
    // SHA256SU1: sha256msg2(d + (m:n >> 32), m)
    const Xmm t = ctx.temp_vec(), u = ctx.temp_vec();
    const Xmm d = ctx.read_vec(rd), n = ctx.read_vec(rn), m = ctx.read_vec(rm);
    movdqa_rr(a, t, m);
    a.sse_imm(kPalignr, t, n, 4);
    movdqa_rr(a, u, d);
    a.sse(kPaddd, u, t);
    a.sse(kSha256msg2, u, m);
    const Xmm out = ctx.write_vec(rd);
    movdqa_rr(a, out, u);
    return 1;
}

unsigned emit_sha2reg(EmitContext& ctx, std::uint32_t w) {
    auto& a = ctx.as();
    const unsigned opcode = (w >> 12) & 31u;
    const unsigned rd = rd_of(w), rn = rn_of(w);
    if (opcode == 0) {   // SHA1H: ROL(Sn, 30)
        const Gp g = ctx.temp_gpr();
        const Xmm n = ctx.read_vec(rn);
        ctx.clobber_host_flags();
        a.movd(g, n);
        a.rol(32, g, 30);
        const Xmm out = ctx.write_vec(rd);
        a.movd(out, g);
        return 1;
    }
    if (opcode == 1) {   // SHA1SU1
        const Xmm t = ctx.temp_vec(), u = ctx.temp_vec(), v = ctx.temp_vec();
        const Xmm x = ctx.read_vec(rd), y = ctx.read_vec(rn);
        movdqa_rr(a, t, y);
        a.sse_shift_imm(0x73, 3, t, 4);       // psrldq 4
        a.pxor(t, x);                          // T
        movdqa_rr(a, u, t);
        a.sse_shift_imm(0x72, 6, u, 1);       // pslld 1
        movdqa_rr(a, v, t);
        a.sse_shift_imm(0x72, 2, v, 31);      // psrld 31
        a.sse(kPor, u, v);                     // ROL(T, 1)
        movdqa_rr(a, v, t);
        a.sse_shift_imm(0x72, 6, v, 2);
        a.sse_shift_imm(0x72, 2, t, 30);
        a.sse(kPor, v, t);                     // ROL(T, 2)
        a.sse_shift_imm(0x73, 7, v, 12);      // pslldq 12: lane0 -> lane3
        a.pxor(u, v);
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, u);
        return 1;
    }
    if (opcode == 2) {   // SHA256SU0 == sha256msg1
        if (!host().sha) {
            call_crypto_helper(ctx, w);
            return 1;
        }
        const Xmm t = ctx.temp_vec();
        const Xmm d = ctx.read_vec(rd), n = ctx.read_vec(rn);
        movdqa_rr(a, t, d);
        a.sse(kSha256msg1, t, n);
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, t);
        return 1;
    }
    return 0;
}

// Per-64-bit-lane rotate left of `t` by `amount` using `scratch`.
void rol64_lanes(x64::Assembler& a, Xmm t, Xmm scratch, unsigned amount) {
    amount &= 63u;
    if (amount == 0) return;
    movdqa_rr(a, scratch, t);
    a.sse_shift_imm(0x73, 6, t, static_cast<std::uint8_t>(amount));          // psllq
    a.sse_shift_imm(0x73, 2, scratch, static_cast<std::uint8_t>(64 - amount)); // psrlq
    a.sse(kPor, t, scratch);
}

unsigned emit_sha512_sha3(EmitContext& ctx, std::uint32_t w) {
    auto& a = ctx.as();
    const unsigned rd = rd_of(w), rn = rn_of(w), rm = rm_of(w), ra = (w >> 10) & 31u;
    if ((w & 0xffe08000u) == 0xce000000u || (w & 0xffe08000u) == 0xce200000u) {   // EOR3 / BCAX
        const bool bcax = (w & 0x00200000u) != 0;
        const Xmm t = ctx.temp_vec();
        const Xmm n = ctx.read_vec(rn), m = ctx.read_vec(rm), av = ctx.read_vec(ra);
        if (bcax) {   // n ^ (m & ~a)
            movdqa_rr(a, t, av);
            a.sse(kPandn, t, m);
        } else {
            movdqa_rr(a, t, av);
            a.pxor(t, m);
        }
        a.pxor(t, n);
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, t);
        return 1;
    }
    if ((w & 0xffe0fc00u) == 0xce608c00u) {   // RAX1: n ^ rol(m, 1)
        const Xmm t = ctx.temp_vec(), s = ctx.temp_vec();
        const Xmm n = ctx.read_vec(rn), m = ctx.read_vec(rm);
        movdqa_rr(a, t, m);
        rol64_lanes(a, t, s, 1);
        a.pxor(t, n);
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, t);
        return 1;
    }
    if ((w & 0xffe00000u) == 0xce800000u) {   // XAR: ror(n ^ m, imm6)
        const unsigned imm = (w >> 10) & 63u;
        const Xmm t = ctx.temp_vec(), s = ctx.temp_vec();
        const Xmm n = ctx.read_vec(rn), m = ctx.read_vec(rm);
        movdqa_rr(a, t, n);
        a.pxor(t, m);
        rol64_lanes(a, t, s, (64u - imm) & 63u);
        const Xmm out = ctx.write_vec(rd);
        movdqa_rr(a, out, t);
        return 1;
    }
    const auto key = w & 0xffe0fc00u;
    if (key == 0xce608000u || key == 0xce608400u || key == 0xce608800u ||   // SHA512H/H2/SU1
        (w & 0xfffffc00u) == 0xcec08000u) {                                  // SHA512SU0
        call_crypto_helper(ctx, w);
        return 1;
    }
    (void)a;
    return 0;
}

} // namespace

unsigned emit_crypto(EmitContext& ctx, std::span<const std::uint32_t> words) {
    const auto w = words[0];
    if ((w & 0x7fe0e000u) == 0x1ac04000u) return emit_crc(ctx, w);
    if ((w & 0xffffcc00u) == 0x4e284800u) return emit_aes(ctx, words);
    if ((w & 0xbfe0fc00u) == 0x0ee0e000u) return emit_pmull(ctx, w);
    if ((w & 0xff208c00u) == 0x5e000000u) return emit_sha3reg(ctx, w);
    if ((w & 0xffffcc00u) == 0x5e280800u) return emit_sha2reg(ctx, w);
    if ((w & 0xff000000u) == 0xce000000u) return emit_sha512_sha3(ctx, w);
    return 0;
}

bool describe_crypto(std::uint32_t w, InstructionUse& use) {
    use = {};
    const unsigned rd = rd_of(w), rn = rn_of(w), rm = rm_of(w), ra = (w >> 10) & 31u;
    const auto g = [](unsigned r) { return r == 31 ? 0u : 1u << r; };
    if ((w & 0x7fe0e000u) == 0x1ac04000u) {
        const unsigned sz = (w >> 10) & 3u;
        if (((w & 0x80000000u) != 0) != (sz == 3)) return false;
        use.gpr_read = g(rn) | g(rm);
        use.gpr_write = g(rd);
        return true;
    }
    if ((w & 0xffffcc00u) == 0x4e284800u) {
        const unsigned opcode = (w >> 12) & 7u;
        use.vec_read = (1u << rn) | (opcode <= 5 ? 1u << rd : 0u);
        use.vec_write = 1u << rd;
        return true;
    }
    if ((w & 0xbfe0fc00u) == 0x0ee0e000u) {
        use.vec_read = (1u << rn) | (1u << rm);
        use.vec_write = 1u << rd;
        return true;
    }
    if ((w & 0xff208c00u) == 0x5e000000u) {
        if (((w >> 12) & 7u) == 7u) return false;
        use.vec_read = (1u << rd) | (1u << rn) | (1u << rm);
        use.vec_write = 1u << rd;
        return true;
    }
    if ((w & 0xffffcc00u) == 0x5e280800u) {
        const unsigned opcode = (w >> 12) & 31u;
        if (opcode > 2) return false;
        use.vec_read = (1u << rn) | (opcode ? 1u << rd : 0u);
        use.vec_write = 1u << rd;
        return true;
    }
    if ((w & 0xffe08000u) == 0xce000000u || (w & 0xffe08000u) == 0xce200000u) {
        use.vec_read = (1u << rn) | (1u << rm) | (1u << ra);
        use.vec_write = 1u << rd;
        return true;
    }
    const auto key = w & 0xffe0fc00u;
    if (key == 0xce608c00u || (w & 0xffe00000u) == 0xce800000u) {
        use.vec_read = (1u << rn) | (1u << rm);
        use.vec_write = 1u << rd;
        return true;
    }
    if (key == 0xce608000u || key == 0xce608400u || key == 0xce608800u) {
        use.vec_read = (1u << rd) | (1u << rn) | (1u << rm);
        use.vec_write = 1u << rd;
        return true;
    }
    if ((w & 0xfffffc00u) == 0xcec08000u) {
        use.vec_read = (1u << rd) | (1u << rn);
        use.vec_write = 1u << rd;
        return true;
    }
    return false;
}

} // namespace transmute::fx::system_detail
