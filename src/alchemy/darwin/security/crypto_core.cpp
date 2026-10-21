#include "alchemy/darwin/security/crypto_core.hpp"

#include <algorithm>
#include <stdexcept>

namespace alchemy::gui::completion_kit::crypto {
namespace {

// ---- byte order ------------------------------------------------------------------------
std::uint32_t load_le32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}
void store_le32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void store_le64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t load_be(const std::uint8_t* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
    return v;
}
void store_be(std::uint8_t* p, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
}

std::uint32_t rotr32(std::uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
std::uint32_t rotl32(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
std::uint64_t rotr64(std::uint64_t v, int n) { return (v >> n) | (v << (64 - n)); }

// ---- SHA-2 constants (FIPS 180-4 sections 4.2.2, 4.2.3, 5.3) ------------------------------------
constexpr std::array<std::uint32_t, 64> k256{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::array<std::uint64_t, 80> k512{
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
    0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
    0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
    0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
    0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
    0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
    0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
    0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
    0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

constexpr std::array<std::uint64_t, 8> iv256{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
constexpr std::array<std::uint64_t, 8> iv384{0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
                                             0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};
constexpr std::array<std::uint64_t, 8> iv512{0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                             0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};

bool wide(Hash hash) { return hash != Hash::sha256; }

} // namespace

std::size_t digest_size(Hash hash) {
    switch (hash) {
    case Hash::sha256: return 32;
    case Hash::sha384: return 48;
    case Hash::sha512: return 64;
    }
    throw std::invalid_argument("unknown hash function");
}

std::size_t block_size(Hash hash) { return wide(hash) ? 128 : 64; }

Hasher::Hasher(Hash hash) : hash_(hash) {
    static_cast<void>(digest_size(hash));   // validates
    reset();
}

void Hasher::reset() {
    state_ = hash_ == Hash::sha256 ? iv256 : hash_ == Hash::sha384 ? iv384 : iv512;
    buffered_ = 0;
    length_ = 0;
}

void Hasher::compress(const std::uint8_t* block) {
    if (!wide(hash_)) {
        std::array<std::uint32_t, 64> w{};
        for (int t = 0; t < 16; ++t) w[t] = static_cast<std::uint32_t>(load_be(block + 4 * t, 4));
        for (int t = 16; t < 64; ++t) {
            const auto s0 = rotr32(w[t - 15], 7) ^ rotr32(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const auto s1 = rotr32(w[t - 2], 17) ^ rotr32(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        std::array<std::uint32_t, 8> v{};
        for (int i = 0; i < 8; ++i) v[i] = static_cast<std::uint32_t>(state_[i]);
        for (int t = 0; t < 64; ++t) {
            const auto [a, b, c, d, e, f, g, h] = v;
            const auto t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) + ((e & f) ^ (~e & g)) + k256[t] + w[t];
            const auto t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            v = {t1 + t2, a, b, c, d + t1, e, f, g};
        }
        for (int i = 0; i < 8; ++i) state_[i] = static_cast<std::uint32_t>(state_[i] + v[i]);
        return;
    }
    std::array<std::uint64_t, 80> w{};
    for (int t = 0; t < 16; ++t) w[t] = load_be(block + 8 * t, 8);
    for (int t = 16; t < 80; ++t) {
        const auto s0 = rotr64(w[t - 15], 1) ^ rotr64(w[t - 15], 8) ^ (w[t - 15] >> 7);
        const auto s1 = rotr64(w[t - 2], 19) ^ rotr64(w[t - 2], 61) ^ (w[t - 2] >> 6);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    auto v = state_;
    for (int t = 0; t < 80; ++t) {
        const auto [a, b, c, d, e, f, g, h] = v;
        const auto t1 = h + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + k512[t] + w[t];
        const auto t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        v = {t1 + t2, a, b, c, d + t1, e, f, g};
    }
    for (int i = 0; i < 8; ++i) state_[i] += v[i];
}

void Hasher::update(ByteView bytes) {
    const auto block = block_size(hash_);
    length_ += bytes.size();
    std::size_t at = 0;
    if (buffered_) {
        const auto take = std::min(block - buffered_, bytes.size());
        std::copy_n(bytes.data(), take, buffer_.data() + buffered_);
        buffered_ += take;
        at = take;
        if (buffered_ < block) return;
        compress(buffer_.data());
        buffered_ = 0;
    }
    for (; bytes.size() - at >= block; at += block) compress(bytes.data() + at);
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end(), buffer_.begin());
    buffered_ = bytes.size() - at;
}

Bytes Hasher::finish() {
    const auto block = block_size(hash_);
    const auto length_field = wide(hash_) ? 16u : 8u;
    const auto bits = length_ * 8;
    buffer_[buffered_++] = 0x80;
    if (buffered_ > block - length_field) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + static_cast<std::ptrdiff_t>(block), std::uint8_t{0});
        compress(buffer_.data());
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + static_cast<std::ptrdiff_t>(block), std::uint8_t{0});
    store_be(buffer_.data() + block - 8, bits, 8);
    // The length's high bits (bytes * 8 >= 2^64) are the top bits of length_.
    if (wide(hash_)) store_be(buffer_.data() + block - 16, length_ >> 61, 8);
    compress(buffer_.data());
    Bytes digest(digest_size(hash_));
    const int word = wide(hash_) ? 8 : 4;
    for (std::size_t i = 0; i < digest.size(); i += static_cast<std::size_t>(word))
        store_be(digest.data() + i, state_[i / static_cast<std::size_t>(word)], word);
    reset();
    return digest;
}

Bytes hash(Hash kind, ByteView message) {
    Hasher hasher(kind);
    hasher.update(message);
    return hasher.finish();
}

std::array<std::uint8_t, 32> sha256(ByteView message) {
    const auto digest = hash(Hash::sha256, message);
    std::array<std::uint8_t, 32> out{};
    std::copy(digest.begin(), digest.end(), out.begin());
    return out;
}

std::array<std::uint8_t, 64> sha512(ByteView message) {
    const auto digest = hash(Hash::sha512, message);
    std::array<std::uint8_t, 64> out{};
    std::copy(digest.begin(), digest.end(), out.begin());
    return out;
}

// ---- HMAC / HKDF ---------------------------------------------------------------------------
Bytes hmac(Hash kind, ByteView key, ByteView message) {
    const auto block = block_size(kind);
    Bytes padded = key.size() > block ? hash(kind, key) : Bytes(key.begin(), key.end());
    padded.resize(block, 0);
    Bytes pad(block);
    std::transform(padded.begin(), padded.end(), pad.begin(), [](std::uint8_t b) { return static_cast<std::uint8_t>(b ^ 0x36); });
    Hasher inner(kind);
    inner.update(pad);
    inner.update(message);
    const auto inner_digest = inner.finish();
    std::transform(padded.begin(), padded.end(), pad.begin(), [](std::uint8_t b) { return static_cast<std::uint8_t>(b ^ 0x5c); });
    Hasher outer(kind);
    outer.update(pad);
    outer.update(inner_digest);
    return outer.finish();
}

Bytes hkdf_extract(Hash kind, ByteView salt, ByteView input_key) {
    if (salt.empty()) {
        const Bytes zeros(digest_size(kind), 0);
        return hmac(kind, zeros, input_key);
    }
    return hmac(kind, salt, input_key);
}

std::optional<Bytes> hkdf_expand(Hash kind, ByteView pseudorandom_key, ByteView info, std::size_t length) {
    const auto size = digest_size(kind);
    if (length > 255 * size) return std::nullopt;
    Bytes output;
    output.reserve(length);
    Bytes previous;
    for (std::uint8_t counter = 1; output.size() < length; ++counter) {
        Bytes message = previous;
        message.insert(message.end(), info.begin(), info.end());
        message.push_back(counter);
        previous = hmac(kind, pseudorandom_key, message);
        const auto take = std::min(size, length - output.size());
        output.insert(output.end(), previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return output;
}

std::optional<Bytes> hkdf(Hash kind, ByteView input_key, ByteView salt, ByteView info, std::size_t length) {
    return hkdf_expand(kind, hkdf_extract(kind, salt, input_key), info, length);
}

// ---- ChaCha20-Poly1305 ------------------------------------------------------------------------
std::array<std::uint8_t, 64> chacha20_block(const ChaChaKey& key, std::uint32_t counter, const ChaChaNonce& nonce) {
    std::array<std::uint32_t, 16> initial{0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) initial[4 + i] = load_le32(key.data() + 4 * i);
    initial[12] = counter;
    for (int i = 0; i < 3; ++i) initial[13 + i] = load_le32(nonce.data() + 4 * i);
    auto x = initial;
    const auto quarter = [&x](int a, int b, int c, int d) {
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 16);
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 12);
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 8);
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 7);
    };
    for (int round = 0; round < 10; ++round) {
        quarter(0, 4, 8, 12); quarter(1, 5, 9, 13); quarter(2, 6, 10, 14); quarter(3, 7, 11, 15);
        quarter(0, 5, 10, 15); quarter(1, 6, 11, 12); quarter(2, 7, 8, 13); quarter(3, 4, 9, 14);
    }
    std::array<std::uint8_t, 64> out{};
    for (int i = 0; i < 16; ++i) store_le32(out.data() + 4 * i, x[i] + initial[i]);
    return out;
}

Bytes chacha20(const ChaChaKey& key, std::uint32_t counter, const ChaChaNonce& nonce, ByteView data) {
    Bytes out(data.begin(), data.end());
    for (std::size_t at = 0; at < out.size(); at += 64, ++counter) {
        const auto stream = chacha20_block(key, counter, nonce);
        const auto count = std::min<std::size_t>(64, out.size() - at);
        for (std::size_t i = 0; i < count; ++i) out[at + i] ^= stream[i];
    }
    return out;
}

// Poly1305 over five 26-bit limbs (the accumulator mod 2^130 - 5).
Tag poly1305(const std::array<std::uint8_t, 32>& key, ByteView message) {
    constexpr std::uint32_t mask = 0x3ffffff;
    const std::uint32_t r0 = load_le32(key.data()) & 0x3ffffff;
    const std::uint32_t r1 = (load_le32(key.data() + 3) >> 2) & 0x3ffff03;
    const std::uint32_t r2 = (load_le32(key.data() + 6) >> 4) & 0x3ffc0ff;
    const std::uint32_t r3 = (load_le32(key.data() + 9) >> 6) & 0x3f03fff;
    const std::uint32_t r4 = (load_le32(key.data() + 12) >> 8) & 0x00fffff;
    const std::uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    std::uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    for (std::size_t at = 0; at < message.size(); at += 16) {
        std::array<std::uint8_t, 17> block{};
        const auto count = std::min<std::size_t>(16, message.size() - at);
        std::copy_n(message.data() + at, count, block.data());
        block[count] = 1;   // the 2^(8*count) bit; 2^128 for full blocks
        h0 += load_le32(block.data()) & mask;
        h1 += (load_le32(block.data() + 3) >> 2) & mask;
        h2 += (load_le32(block.data() + 6) >> 4) & mask;
        h3 += (load_le32(block.data() + 9) >> 6) & mask;
        h4 += (load_le32(block.data() + 12) >> 8) | (std::uint32_t{block[16]} << 24);
        using W = std::uint64_t;
        W d0 = W{h0} * r0 + W{h1} * s4 + W{h2} * s3 + W{h3} * s2 + W{h4} * s1;
        W d1 = W{h0} * r1 + W{h1} * r0 + W{h2} * s4 + W{h3} * s3 + W{h4} * s2;
        W d2 = W{h0} * r2 + W{h1} * r1 + W{h2} * r0 + W{h3} * s4 + W{h4} * s3;
        W d3 = W{h0} * r3 + W{h1} * r2 + W{h2} * r1 + W{h3} * r0 + W{h4} * s4;
        W d4 = W{h0} * r4 + W{h1} * r3 + W{h2} * r2 + W{h3} * r1 + W{h4} * r0;
        W c = d0 >> 26; h0 = static_cast<std::uint32_t>(d0) & mask;
        d1 += c; c = d1 >> 26; h1 = static_cast<std::uint32_t>(d1) & mask;
        d2 += c; c = d2 >> 26; h2 = static_cast<std::uint32_t>(d2) & mask;
        d3 += c; c = d3 >> 26; h3 = static_cast<std::uint32_t>(d3) & mask;
        d4 += c; c = d4 >> 26; h4 = static_cast<std::uint32_t>(d4) & mask;
        h0 += static_cast<std::uint32_t>(c) * 5; c = h0 >> 26; h0 &= mask;
        h1 += static_cast<std::uint32_t>(c);
    }
    // Full carry, then h mod 2^130 - 5.
    std::uint32_t c = h1 >> 26; h1 &= mask;
    h2 += c; c = h2 >> 26; h2 &= mask;
    h3 += c; c = h3 >> 26; h3 &= mask;
    h4 += c; c = h4 >> 26; h4 &= mask;
    h0 += c * 5; c = h0 >> 26; h0 &= mask;
    h1 += c;
    std::uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= mask;
    std::uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= mask;
    std::uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= mask;
    std::uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= mask;
    std::uint32_t g4 = h4 + c - (1u << 26);
    // g4's sign bit set: h < p, keep h; otherwise take g = h - p.
    std::uint32_t select = (g4 >> 31) - 1;
    g0 &= select; g1 &= select; g2 &= select; g3 &= select; g4 &= select;
    select = ~select;
    h0 = (h0 & select) | g0; h1 = (h1 & select) | g1; h2 = (h2 & select) | g2;
    h3 = (h3 & select) | g3; h4 = (h4 & select) | g4;
    // h mod 2^128, plus s.
    const std::uint32_t w0 = h0 | (h1 << 26);
    const std::uint32_t w1 = (h1 >> 6) | (h2 << 20);
    const std::uint32_t w2 = (h2 >> 12) | (h3 << 14);
    const std::uint32_t w3 = (h3 >> 18) | (h4 << 8);
    Tag tag{};
    std::uint64_t f = std::uint64_t{w0} + load_le32(key.data() + 16);
    store_le32(tag.data(), static_cast<std::uint32_t>(f));
    f = std::uint64_t{w1} + load_le32(key.data() + 20) + (f >> 32);
    store_le32(tag.data() + 4, static_cast<std::uint32_t>(f));
    f = std::uint64_t{w2} + load_le32(key.data() + 24) + (f >> 32);
    store_le32(tag.data() + 8, static_cast<std::uint32_t>(f));
    f = std::uint64_t{w3} + load_le32(key.data() + 28) + (f >> 32);
    store_le32(tag.data() + 12, static_cast<std::uint32_t>(f));
    return tag;
}

namespace {

Tag aead_tag(const ChaChaKey& key, const ChaChaNonce& nonce, ByteView aad, ByteView ciphertext) {
    const auto block = chacha20_block(key, 0, nonce);
    std::array<std::uint8_t, 32> one_time{};
    std::copy_n(block.data(), 32, one_time.data());
    Bytes mac;
    const auto pad16 = [&mac] { mac.resize((mac.size() + 15) / 16 * 16, 0); };
    mac.insert(mac.end(), aad.begin(), aad.end());
    pad16();
    mac.insert(mac.end(), ciphertext.begin(), ciphertext.end());
    pad16();
    std::array<std::uint8_t, 16> lengths{};
    store_le64(lengths.data(), aad.size());
    store_le64(lengths.data() + 8, ciphertext.size());
    mac.insert(mac.end(), lengths.begin(), lengths.end());
    return poly1305(one_time, mac);
}

} // namespace

Sealed aead_seal(const ChaChaKey& key, const ChaChaNonce& nonce, ByteView aad, ByteView plaintext) {
    Sealed sealed;
    sealed.ciphertext = chacha20(key, 1, nonce, plaintext);
    sealed.tag = aead_tag(key, nonce, aad, sealed.ciphertext);
    return sealed;
}

std::optional<Bytes> aead_open(const ChaChaKey& key, const ChaChaNonce& nonce, ByteView aad, ByteView ciphertext, const Tag& tag) {
    const auto expected = aead_tag(key, nonce, aad, ciphertext);
    if (!equal_constant_time(expected, tag)) return std::nullopt;
    return chacha20(key, 1, nonce, ciphertext);
}

bool equal_constant_time(ByteView a, ByteView b) {
    if (a.size() != b.size()) return false;
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return difference == 0;
}

// ---- P-521 ------------------------------------------------------------------------------------
// Field elements mod p = 2^521 - 1 as seventeen little-endian 32-bit limbs,
// always fully reduced (0 <= v < p).
namespace {

constexpr std::size_t limbs = 17;
using Fe = std::array<std::uint32_t, limbs>;

constexpr Fe fe_p = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
                     0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x1ff};

// Reduces a value below 2^522 + 2^32 held in 17 limbs (limb 16 may exceed 9 bits).
void fe_normalize(Fe& v) {
    for (int pass = 0; pass < 2; ++pass) {
        // 2^521 == 1 (mod p): fold the bits above 521 into the bottom.
        std::uint64_t carry = v[16] >> 9;
        v[16] &= 0x1ff;
        for (std::size_t i = 0; i < limbs && carry; ++i) {
            const auto sum = std::uint64_t{v[i]} + carry;
            v[i] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
    }
    if (v == fe_p) v.fill(0);
}

Fe fe_add(const Fe& a, const Fe& b) {
    Fe r{};
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < limbs; ++i) {
        const auto sum = std::uint64_t{a[i]} + b[i] + carry;
        r[i] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
    fe_normalize(r);   // limb 16 holds at most 10 bits, no carry out
    return r;
}

// p - a for 0 <= a <= p: every limb of p is all ones (limb 16: nine bits).
Fe fe_neg(const Fe& a) {
    Fe r{};
    for (std::size_t i = 0; i < limbs; ++i) r[i] = fe_p[i] - a[i];
    fe_normalize(r);
    return r;
}

Fe fe_sub(const Fe& a, const Fe& b) { return fe_add(a, fe_neg(b)); }

Fe fe_mul(const Fe& a, const Fe& b) {
    std::array<std::uint32_t, 2 * limbs + 1> t{};
    for (std::size_t i = 0; i < limbs; ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < limbs; ++j) {
            const auto cell = std::uint64_t{t[i + j]} + std::uint64_t{a[i]} * b[j] + carry;
            t[i + j] = static_cast<std::uint32_t>(cell);
            carry = cell >> 32;
        }
        t[i + limbs] = static_cast<std::uint32_t>(carry);
    }
    // t = low + high * 2^521 with both below 2^521; the sum is below 2^522.
    Fe low{}, high{};
    std::copy_n(t.begin(), limbs, low.begin());
    low[16] &= 0x1ff;
    for (std::size_t k = 0; k < limbs; ++k) high[k] = (t[16 + k] >> 9) | (t[17 + k] << 23);
    Fe r{};
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < limbs; ++i) {
        const auto sum = std::uint64_t{low[i]} + high[i] + carry;
        r[i] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
    fe_normalize(r);
    return r;
}

Fe fe_sqr(const Fe& a) { return fe_mul(a, a); }

Fe fe_small(std::uint32_t v) {
    Fe r{};
    r[0] = v;
    return r;
}

bool fe_zero(const Fe& a) {
    return std::all_of(a.begin(), a.end(), [](std::uint32_t v) { return v == 0; });
}

// a^(p-2) = a^-1 (Fermat); p - 2 = 2^521 - 3 has every bit set except bit 1.
Fe fe_inv(const Fe& a) {
    Fe r = fe_small(1);
    for (int bit = 520; bit >= 0; --bit) {
        r = fe_sqr(r);
        if (bit != 1) r = fe_mul(r, a);
    }
    return r;
}

// 66-byte big-endian; nullopt when the value is not below p.
std::optional<Fe> fe_from_bytes(const std::uint8_t* bytes) {
    Fe r{};
    for (std::size_t j = 0; j < p521_bytes; ++j)
        r[j / 4] |= std::uint32_t{bytes[p521_bytes - 1 - j]} << (8 * (j % 4));
    if (r[16] > 0x1ff || r == fe_p) return std::nullopt;
    return r;
}

void fe_to_bytes(const Fe& v, std::uint8_t* bytes) {
    for (std::size_t j = 0; j < p521_bytes; ++j)
        bytes[p521_bytes - 1 - j] = static_cast<std::uint8_t>(v[j / 4] >> (8 * (j % 4)));
}

constexpr P521Scalar from_hex(const char (&hex)[2 * p521_bytes + 1]) {
    P521Scalar out{};
    const auto nibble = [](char c) -> std::uint8_t {
        return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < p521_bytes; ++i)
        out[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    return out;
}

// SEC 2 v2 section 2.6.1 / FIPS 186-4 D.1.2.5.
constexpr P521Scalar curve_b = from_hex(
    "0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00");
constexpr P521Scalar generator_x = from_hex(
    "00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66");
constexpr P521Scalar generator_y = from_hex(
    "011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650");
constexpr P521Scalar order_n = from_hex(
    "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409");

// Jacobian coordinates (x = X/Z^2, y = Y/Z^3); Z = 0 is the point at infinity.
struct Jacobian {
    Fe x{}, y{}, z{};
};

bool infinite(const Jacobian& p) { return fe_zero(p.z); }

// dbl-2001-b (a = -3).
Jacobian dbl(const Jacobian& p) {
    if (infinite(p) || fe_zero(p.y)) return {};
    const auto delta = fe_sqr(p.z);
    const auto gamma = fe_sqr(p.y);
    const auto beta = fe_mul(p.x, gamma);
    const auto t = fe_mul(fe_sub(p.x, delta), fe_add(p.x, delta));
    const auto alpha = fe_add(fe_add(t, t), t);
    const auto beta4 = fe_add(fe_add(beta, beta), fe_add(beta, beta));
    const auto beta8 = fe_add(beta4, beta4);
    Jacobian r;
    r.x = fe_sub(fe_sqr(alpha), beta8);
    r.z = fe_sub(fe_sub(fe_sqr(fe_add(p.y, p.z)), gamma), delta);
    const auto gamma2 = fe_sqr(gamma);
    const auto gamma2x8 = fe_add(fe_add(fe_add(gamma2, gamma2), fe_add(gamma2, gamma2)),
                                 fe_add(fe_add(gamma2, gamma2), fe_add(gamma2, gamma2)));
    r.y = fe_sub(fe_mul(alpha, fe_sub(beta4, r.x)), gamma2x8);
    return r;
}

Jacobian add(const Jacobian& p, const Jacobian& q) {
    if (infinite(p)) return q;
    if (infinite(q)) return p;
    const auto z1z1 = fe_sqr(p.z);
    const auto z2z2 = fe_sqr(q.z);
    const auto u1 = fe_mul(p.x, z2z2);
    const auto u2 = fe_mul(q.x, z1z1);
    const auto s1 = fe_mul(p.y, fe_mul(q.z, z2z2));
    const auto s2 = fe_mul(q.y, fe_mul(p.z, z1z1));
    if (u1 == u2) return s1 == s2 ? dbl(p) : Jacobian{};
    const auto h = fe_sub(u2, u1);
    const auto r = fe_sub(s2, s1);
    const auto h2 = fe_sqr(h);
    const auto h3 = fe_mul(h, h2);
    const auto u1h2 = fe_mul(u1, h2);
    Jacobian out;
    out.x = fe_sub(fe_sub(fe_sqr(r), h3), fe_add(u1h2, u1h2));
    out.y = fe_sub(fe_mul(r, fe_sub(u1h2, out.x)), fe_mul(s1, h3));
    out.z = fe_mul(h, fe_mul(p.z, q.z));
    return out;
}

Jacobian multiply(const P521Scalar& k, const Jacobian& p) {
    Jacobian q;
    for (std::size_t byte = 0; byte < p521_bytes; ++byte)
        for (int bit = 7; bit >= 0; --bit) {
            q = dbl(q);
            if ((k[byte] >> bit) & 1) q = add(q, p);
        }
    return q;
}

P521Point to_affine(const Jacobian& p) {
    const auto zi = fe_inv(p.z);
    const auto zi2 = fe_sqr(zi);
    P521Point out{};
    fe_to_bytes(fe_mul(p.x, zi2), out.data());
    fe_to_bytes(fe_mul(p.y, fe_mul(zi2, zi)), out.data() + p521_bytes);
    return out;
}

// The affine point as Jacobian (Z = 1) when it lies on the curve.
std::optional<Jacobian> from_affine(const P521Point& point) {
    const auto x = fe_from_bytes(point.data());
    const auto y = fe_from_bytes(point.data() + p521_bytes);
    if (!x || !y) return std::nullopt;
    const auto b = fe_from_bytes(curve_b.data());
    // y^2 == x^3 - 3x + b
    const auto x3 = fe_mul(fe_sqr(*x), *x);
    const auto three_x = fe_add(fe_add(*x, *x), *x);
    const auto rhs = fe_add(fe_sub(x3, three_x), *b);
    if (fe_sqr(*y) != rhs) return std::nullopt;
    return Jacobian{*x, *y, fe_small(1)};
}

bool less_than(const P521Scalar& a, const P521Scalar& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

P521Point make_generator() {
    P521Point g{};
    std::copy(generator_x.begin(), generator_x.end(), g.begin());
    std::copy(generator_y.begin(), generator_y.end(), g.begin() + p521_bytes);
    return g;
}

} // namespace

const P521Scalar p521_order = order_n;
const P521Point p521_generator = make_generator();

bool p521_valid_private(const P521Scalar& scalar) {
    const bool zero = std::all_of(scalar.begin(), scalar.end(), [](std::uint8_t b) { return b == 0; });
    return !zero && less_than(scalar, order_n);
}

bool p521_valid_public(const P521Point& point) { return from_affine(point).has_value(); }

std::optional<P521Point> p521_multiply(const P521Scalar& scalar, const P521Point& point) {
    const auto p = from_affine(point);
    if (!p) return std::nullopt;
    const auto q = multiply(scalar, *p);
    if (infinite(q)) return std::nullopt;
    return to_affine(q);
}

std::optional<P521Point> p521_public_key(const P521Scalar& private_key) {
    if (!p521_valid_private(private_key)) return std::nullopt;
    return p521_multiply(private_key, p521_generator);
}

std::optional<P521Scalar> p521_shared_secret(const P521Scalar& private_key, const P521Point& public_key) {
    if (!p521_valid_private(private_key)) return std::nullopt;
    const auto product = p521_multiply(private_key, public_key);
    if (!product) return std::nullopt;
    P521Scalar x{};
    std::copy_n(product->begin(), p521_bytes, x.begin());
    return x;
}

bool p521_compact(const P521Point& public_key) {
    const auto y = fe_from_bytes(public_key.data() + p521_bytes);
    if (!y) return false;
    P521Scalar y_bytes{}, negated{};
    fe_to_bytes(*y, y_bytes.data());
    fe_to_bytes(fe_neg(*y), negated.data());
    return !less_than(negated, y_bytes);   // y <= p - y
}

P521KeyPair p521_generate(const std::function<void(std::span<std::uint8_t>)>& random, bool compact) {
    for (int attempt = 0; attempt < 4096; ++attempt) {
        P521KeyPair pair;
        random(pair.private_key);
        pair.private_key[0] &= 0x01;   // n < 2^521: draw 521 bits, reject values >= n
        if (!p521_valid_private(pair.private_key)) continue;
        const auto point = p521_public_key(pair.private_key);
        if (!point) continue;
        pair.public_key = *point;
        if (compact && !p521_compact(pair.public_key)) continue;
        return pair;
    }
    throw std::runtime_error("P-521 key generation: the random source produced no usable key");
}

} // namespace alchemy::gui::completion_kit::crypto
