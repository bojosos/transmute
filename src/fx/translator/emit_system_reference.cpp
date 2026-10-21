// Portable implementations of the A64 CRC32 and vector crypto instructions,
// written from the Arm ARM pseudocode (owner: atomics/system). Used by the
// fallback helpers (hosts without AES-NI/PCLMUL/SHA-NI, SHA512) and as the
// tests' reference (validated there with known-answer vectors).
#include "fx/translator/emit_system_internal.hpp"

#include <array>
#include <cstring>

namespace transmute::fx::system_detail {
namespace {

using V128 = std::array<std::uint64_t, 2>;

std::uint32_t lane32(const V128& v, unsigned e) { return static_cast<std::uint32_t>(v[e / 2] >> (32 * (e % 2))); }
void set_lane32(V128& v, unsigned e, std::uint32_t x) {
    const unsigned shift = 32 * (e % 2);
    v[e / 2] = (v[e / 2] & ~(0xffffffffULL << shift)) | (static_cast<std::uint64_t>(x) << shift);
}
std::uint8_t byte_of(const V128& v, unsigned i) { return static_cast<std::uint8_t>(v[i / 8] >> (8 * (i % 8))); }
void set_byte(V128& v, unsigned i, std::uint8_t b) {
    const unsigned shift = 8 * (i % 8);
    v[i / 8] = (v[i / 8] & ~(0xffULL << shift)) | (static_cast<std::uint64_t>(b) << shift);
}

std::uint32_t rol32(std::uint32_t x, unsigned n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
std::uint32_t ror32(std::uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }
std::uint64_t ror64(std::uint64_t x, unsigned n) { n &= 63; return n ? (x >> n) | (x << (64 - n)) : x; }

std::uint32_t sha_choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return ((y ^ z) & x) ^ z; }
std::uint32_t sha_parity(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return x ^ y ^ z; }
std::uint32_t sha_majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) | ((x | y) & z); }

// ---- AES (FIPS-197; byte i of the vector is state[i % 4][i / 4]) ------------------

constexpr std::array<std::uint8_t, 256> make_sbox(bool inverse) {
    std::array<std::uint8_t, 256> sbox{}, inv{};
    std::uint8_t p = 1, q = 1;
    do {
        p = static_cast<std::uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q = static_cast<std::uint8_t>(q ^ (q << 1));
        q = static_cast<std::uint8_t>(q ^ (q << 2));
        q = static_cast<std::uint8_t>(q ^ (q << 4));
        if (q & 0x80) q ^= 0x09;
        const auto rotl8 = [](std::uint8_t x, int s) { return static_cast<std::uint8_t>((x << s) | (x >> (8 - s))); };
        const std::uint8_t x = static_cast<std::uint8_t>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        sbox[p] = static_cast<std::uint8_t>(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (unsigned i = 0; i < 256; ++i) inv[sbox[i]] = static_cast<std::uint8_t>(i);
    return inverse ? inv : sbox;
}
constexpr auto kSbox = make_sbox(false);
constexpr auto kInvSbox = make_sbox(true);

std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
std::uint8_t gmul(std::uint8_t x, std::uint8_t y) {
    std::uint8_t r = 0;
    while (y) {
        if (y & 1) r ^= x;
        x = xtime(x);
        y >>= 1;
    }
    return r;
}

V128 aes_shift_rows(const V128& in, bool inverse) {
    V128 out{};
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            const unsigned src = inverse ? 4 * ((c + 4 - r) % 4) + r : 4 * ((c + r) % 4) + r;
            set_byte(out, 4 * c + r, byte_of(in, src));
        }
    }
    return out;
}
V128 aes_sub_bytes(const V128& in, bool inverse) {
    V128 out{};
    for (unsigned i = 0; i < 16; ++i) set_byte(out, i, (inverse ? kInvSbox : kSbox)[byte_of(in, i)]);
    return out;
}
V128 aes_mix_columns(const V128& in, bool inverse) {
    V128 out{};
    static constexpr std::uint8_t fwd[4] = {2, 3, 1, 1}, inv[4] = {14, 11, 13, 9};
    const auto* m = inverse ? inv : fwd;
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            std::uint8_t acc = 0;
            for (unsigned k = 0; k < 4; ++k) acc ^= gmul(byte_of(in, 4 * c + k), m[(k + 4 - r) % 4]);
            set_byte(out, 4 * c + r, acc);
        }
    }
    return out;
}

// ---- SHA-256 / SHA-1 helpers ---------------------------------------------------------

std::uint32_t sigma0_256(std::uint32_t x) { return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22); }
std::uint32_t sigma1_256(std::uint32_t x) { return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25); }

V128 sha256_hash(V128 x, V128 y, const V128& w, bool part1) {
    for (unsigned e = 0; e < 4; ++e) {
        const auto chs = sha_choose(lane32(y, 0), lane32(y, 1), lane32(y, 2));
        const auto maj = sha_majority(lane32(x, 0), lane32(x, 1), lane32(x, 2));
        const auto t = lane32(y, 3) + sigma1_256(lane32(y, 0)) + chs + lane32(w, e);
        set_lane32(x, 3, t + lane32(x, 3));
        set_lane32(y, 3, t + sigma0_256(lane32(x, 0)) + maj);
        // <Y, X> = ROL(Y : X, 32)
        const auto x3 = lane32(x, 3), y3 = lane32(y, 3);
        for (unsigned i = 3; i > 0; --i) { set_lane32(x, i, lane32(x, i - 1)); set_lane32(y, i, lane32(y, i - 1)); }
        set_lane32(x, 0, y3);
        set_lane32(y, 0, x3);
    }
    return part1 ? x : y;
}

std::uint64_t clmul_lo(std::uint64_t a, std::uint64_t b, std::uint64_t& hi) {
    std::uint64_t lo = 0;
    hi = 0;
    for (unsigned i = 0; i < 64; ++i) {
        if ((b >> i) & 1) {
            lo ^= a << i;
            if (i) hi ^= a >> (64 - i);
        }
    }
    return lo;
}

V128& vreg(GuestContext& ctx, unsigned i) { return *reinterpret_cast<V128*>(&ctx.v[i][0]); }

std::array<std::uint32_t, 256> make_crc_table(std::uint32_t poly) {
    std::array<std::uint32_t, 256> t{};
    for (unsigned i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ ((c & 1) ? poly : 0);
        t[i] = c;
    }
    return t;
}

} // namespace

std::uint32_t crc32_update(std::uint32_t acc, std::uint64_t data, unsigned bytes, bool castagnoli) {
    static const auto iso = make_crc_table(0xedb88320u);
    static const auto cas = make_crc_table(0x82f63b78u);
    const auto& table = castagnoli ? cas : iso;
    for (unsigned i = 0; i < bytes; ++i) {
        acc = table[(acc ^ static_cast<std::uint8_t>(data >> (8 * i))) & 0xff] ^ (acc >> 8);
    }
    return acc;
}

bool crypto_reference(GuestContext& ctx, std::uint32_t w) {
    const unsigned rd = rd_of(w), rn = rn_of(w), rm = rm_of(w), ra = (w >> 10) & 31u;
    if ((w & 0xffffcc00u) == 0x4e284800u) {
        const unsigned opcode = (w >> 12) & 7u;
        V128 r = vreg(ctx, rn);
        switch (opcode) {
        case 4: { V128 x = {vreg(ctx, rd)[0] ^ r[0], vreg(ctx, rd)[1] ^ r[1]};
                  r = aes_sub_bytes(aes_shift_rows(x, false), false); break; }
        case 5: { V128 x = {vreg(ctx, rd)[0] ^ r[0], vreg(ctx, rd)[1] ^ r[1]};
                  r = aes_sub_bytes(aes_shift_rows(x, true), true); break; }
        case 6: r = aes_mix_columns(r, false); break;
        default: r = aes_mix_columns(r, true); break;
        }
        vreg(ctx, rd) = r;
        return true;
    }
    if ((w & 0xbfe0fc00u) == 0x0ee0e000u) {   // PMULL{2} Vd.1Q
        const unsigned half = (w & 0x40000000u) ? 1 : 0;
        std::uint64_t hi;
        const auto lo = clmul_lo(vreg(ctx, rn)[half], vreg(ctx, rm)[half], hi);
        vreg(ctx, rd) = {lo, hi};
        return true;
    }
    if ((w & 0xff208c00u) == 0x5e000000u) {
        const unsigned opcode = (w >> 12) & 7u;
        V128 x = vreg(ctx, rd);
        const V128 n = vreg(ctx, rn), m = vreg(ctx, rm);
        switch (opcode) {
        case 0: case 1: case 2: {   // SHA1C / SHA1P / SHA1M
            std::uint32_t y = lane32(n, 0);
            for (unsigned e = 0; e < 4; ++e) {
                const auto b = lane32(x, 1), c = lane32(x, 2), d = lane32(x, 3);
                const auto t = opcode == 0 ? sha_choose(b, c, d) : opcode == 1 ? sha_parity(b, c, d) : sha_majority(b, c, d);
                y = y + rol32(lane32(x, 0), 5) + t + lane32(m, e);
                set_lane32(x, 1, rol32(lane32(x, 1), 30));
                const auto x3 = lane32(x, 3);
                for (unsigned i = 3; i > 0; --i) set_lane32(x, i, lane32(x, i - 1));
                set_lane32(x, 0, y);
                y = x3;
            }
            vreg(ctx, rd) = x;
            return true;
        }
        case 3:   // SHA1SU0
            vreg(ctx, rd) = {x[1] ^ x[0] ^ m[0], n[0] ^ x[1] ^ m[1]};
            return true;
        case 4: vreg(ctx, rd) = sha256_hash(x, n, m, true); return true;
        case 5: vreg(ctx, rd) = sha256_hash(n, x, m, false); return true;
        case 6: {   // SHA256SU1
            V128 t0{};
            for (unsigned e = 0; e < 3; ++e) set_lane32(t0, e, lane32(n, e + 1));
            set_lane32(t0, 3, lane32(m, 0));
            V128 result{};
            const auto s1 = [](std::uint32_t v) { return ror32(v, 17) ^ ror32(v, 19) ^ (v >> 10); };
            for (unsigned e = 0; e < 2; ++e)
                set_lane32(result, e, s1(lane32(m, e + 2)) + lane32(x, e) + lane32(t0, e));
            for (unsigned e = 2; e < 4; ++e)
                set_lane32(result, e, s1(lane32(result, e - 2)) + lane32(x, e) + lane32(t0, e));
            vreg(ctx, rd) = result;
            return true;
        }
        default: return false;
        }
    }
    if ((w & 0xffffcc00u) == 0x5e280800u) {
        const unsigned opcode = (w >> 12) & 31u;
        const V128 x = vreg(ctx, rd), y = vreg(ctx, rn);
        if (opcode == 0) { vreg(ctx, rd) = {rol32(lane32(y, 0), 30), 0}; return true; }
        if (opcode == 1) {   // SHA1SU1
            V128 t{};
            for (unsigned e = 0; e < 4; ++e) set_lane32(t, e, lane32(x, e) ^ (e < 3 ? lane32(y, e + 1) : 0u));
            V128 r{};
            for (unsigned e = 0; e < 3; ++e) set_lane32(r, e, rol32(lane32(t, e), 1));
            set_lane32(r, 3, rol32(lane32(t, 3), 1) ^ rol32(lane32(t, 0), 2));
            vreg(ctx, rd) = r;
            return true;
        }
        if (opcode == 2) {   // SHA256SU0
            V128 t{};
            for (unsigned e = 0; e < 3; ++e) set_lane32(t, e, lane32(x, e + 1));
            set_lane32(t, 3, lane32(y, 0));
            V128 r{};
            for (unsigned e = 0; e < 4; ++e) {
                const auto v = lane32(t, e);
                set_lane32(r, e, (ror32(v, 7) ^ ror32(v, 18) ^ (v >> 3)) + lane32(x, e));
            }
            vreg(ctx, rd) = r;
            return true;
        }
        return false;
    }
    const auto key = w & 0xffe0fc00u;
    if ((w & 0xffe08000u) == 0xce000000u) {   // EOR3
        const auto& n = vreg(ctx, rn); const auto& m = vreg(ctx, rm); const auto& av = vreg(ctx, ra);
        vreg(ctx, rd) = {n[0] ^ m[0] ^ av[0], n[1] ^ m[1] ^ av[1]};
        return true;
    }
    if ((w & 0xffe08000u) == 0xce200000u) {   // BCAX
        const auto& n = vreg(ctx, rn); const auto& m = vreg(ctx, rm); const auto& av = vreg(ctx, ra);
        vreg(ctx, rd) = {n[0] ^ (m[0] & ~av[0]), n[1] ^ (m[1] & ~av[1])};
        return true;
    }
    if (key == 0xce608c00u) {   // RAX1
        const auto& n = vreg(ctx, rn); const auto& m = vreg(ctx, rm);
        vreg(ctx, rd) = {n[0] ^ ror64(m[0], 63), n[1] ^ ror64(m[1], 63)};
        return true;
    }
    if ((w & 0xffe00000u) == 0xce800000u) {   // XAR
        const unsigned imm = (w >> 10) & 63u;
        const auto& n = vreg(ctx, rn); const auto& m = vreg(ctx, rm);
        vreg(ctx, rd) = {ror64(n[0] ^ m[0], imm), ror64(n[1] ^ m[1], imm)};
        return true;
    }
    if (key == 0xce608000u) {   // SHA512H
        const V128 x = vreg(ctx, rn), y = vreg(ctx, rm), wv = vreg(ctx, rd);
        V128 t{};
        auto s1 = ror64(y[1], 14) ^ ror64(y[1], 18) ^ ror64(y[1], 41);
        t[1] = (y[1] & x[0]) ^ (~y[1] & x[1]);
        t[1] = t[1] + s1 + wv[1];
        const auto tmp = t[1] + y[0];
        s1 = ror64(tmp, 14) ^ ror64(tmp, 18) ^ ror64(tmp, 41);
        t[0] = (tmp & y[1]) ^ (~tmp & x[0]);
        t[0] = t[0] + s1 + wv[0];
        vreg(ctx, rd) = t;
        return true;
    }
    if (key == 0xce608400u) {   // SHA512H2
        const V128 x = vreg(ctx, rn), y = vreg(ctx, rm), wv = vreg(ctx, rd);
        V128 t{};
        auto s0 = ror64(y[0], 28) ^ ror64(y[0], 34) ^ ror64(y[0], 39);
        t[1] = (x[0] & y[1]) ^ (x[0] & y[0]) ^ (y[1] & y[0]);
        t[1] = t[1] + s0 + wv[1];
        s0 = ror64(t[1], 28) ^ ror64(t[1], 34) ^ ror64(t[1], 39);
        t[0] = (t[1] & y[0]) ^ (t[1] & y[1]) ^ (y[1] & y[0]);
        t[0] = t[0] + s0 + wv[0];
        vreg(ctx, rd) = t;
        return true;
    }
    if (key == 0xce608800u) {   // SHA512SU1
        const V128 wv = vreg(ctx, rd), x = vreg(ctx, rn), y = vreg(ctx, rm);
        const auto s1 = [](std::uint64_t v) { return ror64(v, 19) ^ ror64(v, 61) ^ (v >> 6); };
        vreg(ctx, rd) = {wv[0] + s1(x[0]) + y[0], wv[1] + s1(x[1]) + y[1]};
        return true;
    }
    if ((w & 0xfffffc00u) == 0xcec08000u) {   // SHA512SU0
        const V128 wv = vreg(ctx, rd), x = vreg(ctx, rn);
        const auto s0 = [](std::uint64_t v) { return ror64(v, 1) ^ ror64(v, 8) ^ (v >> 7); };
        vreg(ctx, rd) = {wv[0] + s0(wv[1]), wv[1] + s0(x[0])};
        return true;
    }
    return false;
}

std::uint64_t helper_crc32(GuestContext*, std::uint64_t acc, std::uint64_t data, std::uint64_t kind) {
    return crc32_update(static_cast<std::uint32_t>(acc), data, static_cast<unsigned>(kind & 0xff),
                        (kind & 0x100) != 0);
}

void helper_crypto(GuestContext* ctx, std::uint64_t word) {
    crypto_reference(*ctx, static_cast<std::uint32_t>(word));
}

} // namespace transmute::fx::system_detail
