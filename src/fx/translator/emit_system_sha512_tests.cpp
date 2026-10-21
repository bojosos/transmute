// SHA-512 known answer through the A64 SHA512H/SHA512H2/SHA512SU0/SHA512SU1
// instruction semantics (owner: atomics/system). The round structure follows
// the Linux arm64 sha512-ce implementation: 40 "drounds" of two rounds each,
// state rotating through v0..v4, message schedule in v12..v19.
#include "fx/translator/emit_system_test_util.hpp"
#include "fx/translator/emit_system_internal.hpp"

#include "fx/testing/test.hpp"

#include <array>

using namespace transmute::fx;
using namespace transmute::fx::systest;

namespace {

using V = std::array<std::uint64_t, 2>;

constexpr std::uint64_t kK[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};

V add2d(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1]}; }
V ext8(const V& n, const V& m) { return {n[1], m[0]}; }   // EXT Vd.16b, Vn.16b, Vm.16b, #8

// Executes a SHA512 instruction (d/n/m operand values) via the reference or
// emitted code (helper path).
V op(std::uint32_t base, const V& d, const V& n, const V& m, bool emitted, bool has_m = true) {
    auto ctx = make_context(4);
    ctx.v[2][0] = m[0]; ctx.v[2][1] = m[1];
    ctx.v[1][0] = n[0]; ctx.v[1][1] = n[1];
    ctx.v[0][0] = d[0]; ctx.v[0][1] = d[1];
    const std::uint32_t w = base | (has_m ? 2u << 16 : 0u) | (1u << 5) | 0u;
    if (emitted) Program({w}).run(ctx);
    else system_detail::crypto_reference(ctx, w);
    return {ctx.v[0][0], ctx.v[0][1]};
}
V sha512h(const V& d, const V& n, const V& m, bool e) { return op(0xce608000u, d, n, m, e); }
V sha512h2(const V& d, const V& n, const V& m, bool e) { return op(0xce608400u, d, n, m, e); }
V sha512su1(const V& d, const V& n, const V& m, bool e) { return op(0xce608800u, d, n, m, e); }
V sha512su0(const V& d, const V& n, bool e) { return op(0xcec08000u, d, n, {}, e, false); }

std::array<std::uint64_t, 8> sha512_abc(bool emitted) {
    std::array<V, 32> v{};
    std::array<std::uint64_t, 16> w{};
    w[0] = 0x6162638000000000ULL;
    w[15] = 24;
    for (unsigned k = 0; k < 8; ++k) v[12 + k] = {w[2 * k], w[2 * k + 1]};
    const std::array<std::uint64_t, 8> h0 = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                             0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                             0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    for (unsigned k = 0; k < 4; ++k) v[k] = {h0[2 * k], h0[2 * k + 1]};
    static constexpr unsigned kRot[5][5] = {{0, 1, 2, 3, 4}, {3, 0, 4, 2, 1}, {2, 3, 1, 4, 0}, {4, 2, 0, 1, 3}, {1, 4, 3, 0, 2}};
    for (unsigned r = 0; r < 40; ++r) {
        const auto& R = kRot[r % 5];
        const unsigned i0 = R[0], i1 = R[1], i2 = R[2], i3 = R[3], i4 = R[4];
        const unsigned in0 = 12 + r % 8, in1 = 12 + (r + 1) % 8, in2 = 12 + (r + 7) % 8;
        const unsigned in3 = 12 + (r + 4) % 8, in4 = 12 + (r + 5) % 8;
        const bool update = r < 32;
        V v5 = add2d({kK[2 * r], kK[2 * r + 1]}, v[in0]);
        const V v6 = ext8(v[i2], v[i3]);
        v5 = ext8(v5, v5);
        const V v7 = ext8(v[i1], v[i2]);
        v[i3] = add2d(v[i3], v5);
        if (update) {
            v5 = ext8(v[in3], v[in4]);
            v[in0] = sha512su0(v[in0], v[in1], emitted);
        }
        v[i3] = sha512h(v[i3], v6, v7, emitted);
        if (update) v[in0] = sha512su1(v[in0], v[in2], v5, emitted);
        v[i4] = add2d(v[i1], v[i3]);
        v[i3] = sha512h2(v[i3], v[i1], v[i0], emitted);
    }
    std::array<std::uint64_t, 8> out{};
    for (unsigned k = 0; k < 4; ++k) {
        out[2 * k] = h0[2 * k] + v[k][0];
        out[2 * k + 1] = h0[2 * k + 1] + v[k][1];
    }
    return out;
}

} // namespace

FX_TEST(crypto_sha512_known_answer) {
    const std::array<std::uint64_t, 8> expected = {0xddaf35a193617abaULL, 0xcc417349ae204131ULL, 0x12e6fa4e89a97ea2ULL,
                                                   0x0a9eeee64b55d39aULL, 0x2192992a274fc1a8ULL, 0x36ba3c23a3feebbdULL,
                                                   0x454d4423643ce80eULL, 0x2a9ac94fa54ca49fULL};
    const auto reference = sha512_abc(false);
    FX_CHECK(reference == expected);
    if (reference != expected) {
        for (auto x : reference) std::printf("    %016llx\n", static_cast<unsigned long long>(x));
    }
    FX_CHECK(sha512_abc(true) == expected);
}
