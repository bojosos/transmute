// CRC32/CRC32C, AES, PMULL, SHA1/SHA256/SHA512 and SHA3 (owner: atomics/system).
// The portable reference (crypto_reference/crc32_update) is validated with
// known-answer vectors computed through A64 instruction sequences; the
// emitted code (host extensions and helper fallbacks) is then compared with it.
#include "fx/translator/emit_system_test_util.hpp"
#include "fx/translator/emit_system_internal.hpp"

#include "fx/testing/test.hpp"

#include <array>
#include <bitset>
#include <cstring>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::systest;
using system_detail::crc32_update;
using system_detail::crypto_reference;

namespace {

struct ExtensionScope {
    explicit ExtensionScope(bool on) { system_set_host_extensions(on); }
    ~ExtensionScope() { system_set_host_extensions(true); }
};

// Literal Arm ARM CRC32 pseudocode: BitReverse + Poly32Mod2.
std::uint32_t arm_crc(std::uint32_t acc, std::uint64_t val, unsigned size, std::uint32_t poly) {
    std::bitset<96> data;
    for (unsigned i = 0; i < 32; ++i) data[size + 31 - i] = (acc >> i) & 1;        // BitReverse(acc):Zeros(size)
    for (unsigned i = 0; i < size; ++i) data[size + 31 - i] = data[size + 31 - i] ^ ((val >> i) & 1);  // ^ BitReverse(val):Zeros(32)
    for (int i = static_cast<int>(size) + 31; i >= 32; --i) {
        if (data[i]) {
            for (unsigned k = 0; k < 32; ++k) data[i - 32 + k] = data[i - 32 + k] ^ ((poly >> k) & 1);
        }
    }
    std::uint32_t r = 0;
    for (unsigned i = 0; i < 32; ++i) r |= static_cast<std::uint32_t>(data[i]) << (31 - i);   // BitReverse
    return r;
}

std::uint32_t crc_word(unsigned sz, bool c, unsigned rd, unsigned rn, unsigned rm) {
    return (sz == 3 ? 0x80000000u : 0u) | 0x1ac04000u | (rm << 16) | (c ? 1u << 12 : 0u) | (sz << 10) | (rn << 5) | rd;
}

using V = std::array<std::uint64_t, 2>;
void set_v(GuestContext& ctx, unsigned r, const V& v) { ctx.v[r][0] = v[0]; ctx.v[r][1] = v[1]; }
V get_v(const GuestContext& ctx, unsigned r) { return {ctx.v[r][0], ctx.v[r][1]}; }
V bytes_to_v(const std::uint8_t* b) { V v{}; std::memcpy(v.data(), b, 16); return v; }

std::uint32_t aese(unsigned d, unsigned n) { return 0x4e284800u | (n << 5) | d; }
std::uint32_t aesd(unsigned d, unsigned n) { return 0x4e285800u | (n << 5) | d; }
std::uint32_t aesmc(unsigned d, unsigned n) { return 0x4e286800u | (n << 5) | d; }
std::uint32_t aesimc(unsigned d, unsigned n) { return 0x4e287800u | (n << 5) | d; }
std::uint32_t sha3reg(unsigned opcode, unsigned d, unsigned n, unsigned m) { return 0x5e000000u | (m << 16) | (opcode << 12) | (n << 5) | d; }
std::uint32_t sha2reg(unsigned opcode, unsigned d, unsigned n) { return 0x5e280800u | (opcode << 12) | (n << 5) | d; }

// Runs one instruction through crypto_reference on a copy.
GuestContext ref(GuestContext ctx, std::uint32_t w) {
    crypto_reference(ctx, w);
    return ctx;
}

// ---- AES-128 known answer (FIPS-197 C.1) ----------------------------------------------

std::array<V, 11> expand_aes128(const std::uint8_t key[16]) {
    static constexpr std::uint8_t sbox[256] = {
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
    std::uint8_t w[176];
    std::memcpy(w, key, 16);
    std::uint8_t rcon = 1;
    for (unsigned i = 16; i < 176; i += 4) {
        std::uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0) {
            const std::uint8_t first = t[0];
            t[0] = static_cast<std::uint8_t>(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = static_cast<std::uint8_t>((rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0));
        }
        for (unsigned k = 0; k < 4; ++k) w[i + k] = static_cast<std::uint8_t>(w[i - 16 + k] ^ t[k]);
    }
    std::array<V, 11> rk{};
    for (unsigned r = 0; r < 11; ++r) rk[r] = bytes_to_v(w + 16 * r);
    return rk;
}

constexpr std::uint8_t kAesKey[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
constexpr std::uint8_t kAesPlain[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
constexpr std::uint8_t kAesCipher[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};

// Encrypts v0 with round keys in v1..v11 using AESE/AESMC words; the final
// AddRoundKey is done by the caller.
std::vector<std::uint32_t> aes_encrypt_words() {
    std::vector<std::uint32_t> words;
    for (unsigned r = 0; r < 9; ++r) { words.push_back(aese(0, 1 + r)); words.push_back(aesmc(0, 0)); }
    words.push_back(aese(0, 10));
    return words;
}
std::vector<std::uint32_t> aes_decrypt_words() {
    std::vector<std::uint32_t> words;
    for (unsigned r = 0; r < 9; ++r) { words.push_back(aesd(0, 12 + r)); words.push_back(aesimc(0, 0)); }
    words.push_back(aesd(0, 21));
    return words;
}

GuestContext aes_context(const std::array<V, 11>& rk, const std::uint8_t* block) {
    auto ctx = make_context(1);
    set_v(ctx, 0, bytes_to_v(block));
    for (unsigned r = 0; r < 11; ++r) set_v(ctx, 1 + r, rk[r]);
    // Equivalent inverse cipher keys: rk[10], InvMixColumns(rk[9..1]), rk[0]
    set_v(ctx, 12, rk[10]);
    for (unsigned r = 1; r < 10; ++r) {
        auto t = ctx;
        set_v(t, 30, rk[10 - r]);
        crypto_reference(t, aesimc(31, 30));
        set_v(ctx, 12 + r, get_v(t, 31));
    }
    set_v(ctx, 22, rk[0]);
    return ctx;
}

// ---- SHA known answers ---------------------------------------------------------------

V add32(const V& a, const V& b) {
    V r{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto x = static_cast<std::uint32_t>(a[i / 2] >> (32 * (i % 2))) + static_cast<std::uint32_t>(b[i / 2] >> (32 * (i % 2)));
        r[i / 2] |= static_cast<std::uint64_t>(x) << (32 * (i % 2));
    }
    return r;
}
V from32(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    return {a | (static_cast<std::uint64_t>(b) << 32), c | (static_cast<std::uint64_t>(d) << 32)};
}
std::uint32_t lane(const V& v, unsigned i) { return static_cast<std::uint32_t>(v[i / 2] >> (32 * (i % 2))); }

// Applies instruction `w` with registers d,n,m bound to the given values.
V apply(std::uint32_t w, const V& d, const V& n, const V& m, bool emitted, bool extensions = true) {
    auto ctx = make_context(2);
    const unsigned rd = w & 31u, rn = (w >> 5) & 31u, rm = (w >> 16) & 31u;
    set_v(ctx, rm, m);
    set_v(ctx, rn, n);
    set_v(ctx, rd, d);
    if (emitted) {
        ExtensionScope scope(extensions);
        Program({w}).run(ctx);
    } else {
        crypto_reference(ctx, w);
    }
    return get_v(ctx, rd);
}

std::array<std::uint32_t, 16> abc_block() {
    std::array<std::uint32_t, 16> w{};
    w[0] = 0x61626380u;
    w[15] = 24;
    return w;
}

std::array<std::uint32_t, 8> sha256_abc(bool emitted, bool extensions) {
    static constexpr std::uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    const auto blk = abc_block();
    V w[4];
    for (unsigned k = 0; k < 4; ++k) w[k] = from32(blk[4 * k], blk[4 * k + 1], blk[4 * k + 2], blk[4 * k + 3]);
    const V init_abcd = from32(0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    const V init_efgh = from32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19);
    V abcd = init_abcd, efgh = init_efgh;
    for (unsigned g = 0; g < 16; ++g) {
        const V t = add32(w[g % 4], from32(K[4 * g], K[4 * g + 1], K[4 * g + 2], K[4 * g + 3]));
        const V saved = abcd;
        abcd = apply(sha3reg(4, 0, 1, 2), abcd, efgh, t, emitted, extensions);      // sha256h q0, q1, v2
        efgh = apply(sha3reg(5, 0, 1, 2), efgh, saved, t, emitted, extensions);     // sha256h2 q0(efgh), q1(saved)
        if (g < 12) {
            auto x = apply(sha2reg(2, 0, 1), w[g % 4], w[(g + 1) % 4], {}, emitted, extensions);            // su0
            w[g % 4] = apply(sha3reg(6, 0, 1, 2), x, w[(g + 2) % 4], w[(g + 3) % 4], emitted, extensions);  // su1
        }
    }
    abcd = add32(abcd, init_abcd);
    efgh = add32(efgh, init_efgh);
    return {lane(abcd, 0), lane(abcd, 1), lane(abcd, 2), lane(abcd, 3), lane(efgh, 0), lane(efgh, 1), lane(efgh, 2), lane(efgh, 3)};
}

std::array<std::uint32_t, 5> sha1_abc(bool emitted, bool extensions) {
    static constexpr std::uint32_t K[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
    const auto blk = abc_block();
    V w[4];
    for (unsigned k = 0; k < 4; ++k) w[k] = from32(blk[4 * k], blk[4 * k + 1], blk[4 * k + 2], blk[4 * k + 3]);
    const V init = from32(0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476);
    const std::uint32_t init_e = 0xc3d2e1f0;
    V abcd = init;
    V e = {init_e, 0};
    for (unsigned g = 0; g < 20; ++g) {
        const auto k = K[g / 5];
        const V t = add32(w[g % 4], from32(k, k, k, k));
        const V e_next = apply(sha2reg(0, 0, 1), {}, abcd, {}, emitted, extensions);              // sha1h
        const unsigned op = g < 5 ? 0 : g < 10 ? 1 : g < 15 ? 2 : 1;                             // c p m p
        abcd = apply(sha3reg(op, 0, 1, 2), abcd, e, t, emitted, extensions);
        e = e_next;
        if (g < 16) {
            auto x = apply(sha3reg(3, 0, 1, 2), w[g % 4], w[(g + 1) % 4], w[(g + 2) % 4], emitted, extensions);  // su0
            w[g % 4] = apply(sha2reg(1, 0, 1), x, w[(g + 3) % 4], {}, emitted, extensions);                   // su1
        }
    }
    abcd = add32(abcd, init);
    return {lane(abcd, 0), lane(abcd, 1), lane(abcd, 2), lane(abcd, 3), static_cast<std::uint32_t>(e[0]) + init_e};
}

} // namespace

FX_TEST(crypto_crc_reference_known_answers) {
    const char* text = "123456789";
    std::uint32_t iso = ~0u, cas = ~0u;
    for (const char* p = text; *p; ++p) {
        iso = crc32_update(iso, static_cast<std::uint8_t>(*p), 1, false);
        cas = crc32_update(cas, static_cast<std::uint8_t>(*p), 1, true);
    }
    FX_CHECK_EQ(~iso, 0xcbf43926u);
    FX_CHECK_EQ(~cas, 0xe3069283u);
    std::uint64_t state = 5;
    for (int i = 0; i < 200; ++i) {
        const auto acc = static_cast<std::uint32_t>(testing::random_u64(state));
        const auto val = testing::random_u64(state);
        for (unsigned bytes : {1u, 2u, 4u, 8u}) {
            FX_CHECK_EQ(crc32_update(acc, val, bytes, false), arm_crc(acc, val, bytes * 8, 0x04c11db7u));
            FX_CHECK_EQ(crc32_update(acc, val, bytes, true), arm_crc(acc, val, bytes * 8, 0x1edc6f41u));
        }
    }
}

FX_TEST(crypto_crc_instructions_match_reference) {
    struct R { unsigned rd, rn, rm; };
    for (bool extensions : {true, false}) {
        ExtensionScope scope(extensions);
        for (unsigned sz = 0; sz < 4; ++sz) {
            for (bool c : {false, true}) {
                for (const R& r : {R{0, 1, 2}, R{3, 3, 4}, R{5, 6, 6}, R{7, 31, 8}, R{9, 10, 31}, R{11, 12, 11}}) {
                    const auto w = crc_word(sz, c, r.rd, r.rn, r.rm);
                    Program program({w});
                    for (std::uint64_t seed = 0; seed < 40; ++seed) {
                        auto ctx = make_context(seed * 7 + sz);
                        if (seed == 0) { ctx.x[r.rn] = 0; ctx.x[r.rm] = 0; }
                        if (seed == 1) { ctx.x[r.rn] = ~0ULL; ctx.x[r.rm] = ~0ULL; }
                        auto expected = ctx;
                        const std::uint64_t acc = r.rn == 31 ? 0 : ctx.x[r.rn];
                        const std::uint64_t val = r.rm == 31 ? 0 : ctx.x[r.rm];
                        if (r.rd != 31) expected.x[r.rd] = crc32_update(static_cast<std::uint32_t>(acc), val, 1u << sz, c);
                        expected.pc += 4;
                        program.run(ctx);
                        testing::compare_contexts(expected, ctx, "crc32 sz=" + std::to_string(sz) + " c=" + std::to_string(c) +
                                                  " ext=" + std::to_string(extensions));
                    }
                }
            }
        }
    }
    unsigned consumed = 1;
    x64::Assembler a;
    testing::MemoryEmitContext e(a, 0);
    const std::uint32_t bad = crc_word(3, false, 0, 1, 2) & ~0x80000000u;   // CRC32X with sf=0
    consumed = emit_system(e, std::span(&bad, 1));
    FX_CHECK_EQ(consumed, 0u);
}

FX_TEST(crypto_aes_known_answer_and_paths) {
    const auto rk = expand_aes128(kAesKey);
    // Reference through the instruction semantics.
    {
        auto ctx = aes_context(rk, kAesPlain);
        for (auto w : aes_encrypt_words()) crypto_reference(ctx, w);
        V out = get_v(ctx, 0);
        out[0] ^= rk[10][0];
        out[1] ^= rk[10][1];
        FX_CHECK(std::memcmp(out.data(), kAesCipher, 16) == 0);
        set_v(ctx, 0, bytes_to_v(kAesCipher));
        for (auto w : aes_decrypt_words()) crypto_reference(ctx, w);
        out = get_v(ctx, 0);
        out[0] ^= rk[0][0];
        out[1] ^= rk[0][1];
        FX_CHECK(std::memcmp(out.data(), kAesPlain, 16) == 0);
    }
    // Emitted: AES-NI (fused AESE+AESMC / AESD+AESIMC and unfused) and helper.
    for (int mode = 0; mode < 3; ++mode) {
        ExtensionScope scope(mode != 2);
        system_set_fusion(mode != 1);
        auto ctx = aes_context(rk, kAesPlain);
        Program enc(aes_encrypt_words());
        enc.run(ctx);
        V out = get_v(ctx, 0);
        out[0] ^= rk[10][0];
        out[1] ^= rk[10][1];
        FX_CHECK(std::memcmp(out.data(), kAesCipher, 16) == 0);
        if (mode == 0) FX_CHECK_EQ(enc.family_spans().front(), 2u);
        if (mode == 1) FX_CHECK_EQ(enc.family_spans().front(), 1u);
        set_v(ctx, 0, bytes_to_v(kAesCipher));
        Program(aes_decrypt_words()).run(ctx);
        out = get_v(ctx, 0);
        out[0] ^= rk[0][0];
        out[1] ^= rk[0][1];
        FX_CHECK(std::memcmp(out.data(), kAesPlain, 16) == 0);
        system_set_fusion(true);
    }
    // Random single instructions, incl. register aliasing.
    std::uint64_t seed = 70;
    for (bool extensions : {true, false}) {
        ExtensionScope scope(extensions);
        for (unsigned op = 4; op < 8; ++op) {
            for (auto [d, n] : {std::pair{0u, 1u}, std::pair{2u, 2u}, std::pair{31u, 5u}}) {
                const std::uint32_t w = 0x4e280800u | (op << 12) | (n << 5) | d;
                Program program({w});
                for (int i = 0; i < 20; ++i) {
                    auto ctx = make_context(seed++);
                    auto expected = ref(ctx, w);
                    expected.pc += 4;
                    program.run(ctx);
                    testing::compare_contexts(expected, ctx, "aes op=" + std::to_string(op));
                }
            }
        }
    }
}

FX_TEST(crypto_sha_known_answers) {
    const std::array<std::uint32_t, 8> sha256 = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
                                                 0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
    const std::array<std::uint32_t, 5> sha1 = {0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d};
    FX_CHECK(sha256_abc(false, true) == sha256);
    FX_CHECK(sha1_abc(false, true) == sha1);
    FX_CHECK(sha256_abc(true, true) == sha256);
    FX_CHECK(sha1_abc(true, true) == sha1);
    FX_CHECK(sha256_abc(true, false) == sha256);
    FX_CHECK(sha1_abc(true, false) == sha1);
}

FX_TEST(crypto_vector_instructions_match_reference) {
    std::vector<std::uint32_t> words;
    for (unsigned op = 0; op < 7; ++op) {
        words.push_back(sha3reg(op, 0, 1, 2));
        words.push_back(sha3reg(op, 3, 3, 4));
        words.push_back(sha3reg(op, 5, 6, 5));
    }
    for (unsigned op = 0; op < 3; ++op) { words.push_back(sha2reg(op, 0, 1)); words.push_back(sha2reg(op, 7, 7)); }
    words.push_back(0x0ee0e000u | (2u << 16) | (1u << 5) | 0u);   // pmull v0.1q, v1.1d, v2.1d
    words.push_back(0x4ee0e000u | (2u << 16) | (1u << 5) | 0u);   // pmull2
    words.push_back(0x4ee0e000u | (3u << 16) | (3u << 5) | 3u);
    words.push_back(0xce000000u | (2u << 16) | (3u << 10) | (1u << 5));   // eor3
    words.push_back(0xce200000u | (2u << 16) | (3u << 10) | (1u << 5));   // bcax
    words.push_back(0xce200000u | (0u << 16) | (0u << 10) | (1u << 5));
    words.push_back(0xce608c00u | (2u << 16) | (1u << 5));                 // rax1
    for (unsigned imm : {0u, 1u, 17u, 63u}) words.push_back(0xce800000u | (2u << 16) | (imm << 10) | (1u << 5));   // xar
    words.push_back(0xce608000u | (2u << 16) | (1u << 5));   // sha512h
    words.push_back(0xce608400u | (2u << 16) | (1u << 5));   // sha512h2
    words.push_back(0xce608800u | (2u << 16) | (1u << 5));   // sha512su1
    words.push_back(0xcec08000u | (1u << 5));                // sha512su0
    std::uint64_t seed = 400;
    for (bool extensions : {true, false}) {
        ExtensionScope scope(extensions);
        for (auto w : words) {
            Program program({w});
            for (int i = 0; i < 25; ++i) {
                auto ctx = make_context(seed++);
                auto expected = ref(ctx, w);
                expected.pc += 4;
                program.run(ctx);
                char hex[16];
                std::snprintf(hex, sizeof hex, "%08x", w);
                testing::compare_contexts(expected, ctx, std::string("crypto ") + hex + " ext=" + std::to_string(extensions));
            }
        }
    }
    // PMULL reference known answer: (2^63 + 1) * 3 = 2^64 + 2^63 + 3 (carry-less)
    auto ctx = make_context(1);
    ctx.v[1][0] = 0x8000000000000001ULL;
    ctx.v[2][0] = 3;
    crypto_reference(ctx, 0x0ee0e000u | (2u << 16) | (1u << 5));
    FX_CHECK_EQ(ctx.v[0][0], 0x8000000000000003ULL);
    FX_CHECK_EQ(ctx.v[0][1], std::uint64_t{1});
}

FX_TEST(debug_dump_emitted) {
    const char* env = std::getenv("FX_DUMP_WORD");
    if (!env) return;
    const auto w = static_cast<std::uint32_t>(std::strtoul(env, nullptr, 16));
    x64::Assembler a;
    testing::MemoryEmitContext e(a, 0);
    emit_system(e, std::span(&w, 1));
    e.end_instruction();
    for (auto b : a.bytes()) std::printf("%02x", b);
    std::printf("\n");
}

FX_TEST(crypto_crc_matches_oracle) {
    testing::Buffer buffer(4096);
    struct R { unsigned rd, rn, rm; };
    std::uint64_t seed = 9000;
    for (bool extensions : {true, false}) {
        ExtensionScope scope(extensions);
        // zlib (CPython) hot loop: crc32x w8, w8, x13
        for (int i = 0; i < 50; ++i) differential({0x9acd4d08u}, make_context(seed++), buffer, "crc32x w8, w8, x13");
        for (unsigned sz = 0; sz < 4; ++sz) {
            for (bool c : {false, true}) {
                for (const R& r : {R{0, 1, 2}, R{3, 3, 4}, R{5, 6, 6}, R{7, 31, 8}, R{9, 10, 31}, R{31, 1, 2}}) {
                    const auto w = crc_word(sz, c, r.rd, r.rn, r.rm);
                    for (int i = 0; i < 10; ++i) {
                        differential({w}, make_context(seed++), buffer,
                                     "crc oracle sz=" + std::to_string(sz) + " c=" + std::to_string(c));
                    }
                }
            }
        }
    }
}
