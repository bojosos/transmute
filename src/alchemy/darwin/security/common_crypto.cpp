#include "alchemy/darwin/security/security_internal.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>

#include "alchemy/host/crypto.hpp"

// CommonCrypto (libSystem's libcommonCrypto) digests, HMAC and random bytes,
// project-authored: MD5, SHA-1, SHA-224/256, SHA-384/512 one-shot and
// Init/Update/Final over contexts that live in guest memory with Apple's
// CC_*_CTX sizes (the layout inside is Alchemy's, so contexts can be copied
// like the originals), CCHmac* over the same digests and
// CCRandomGenerateBytes over the host's random generator. Each symbol is installed only
// when no other provider exports it (the export-fill provider owns
// CC_MD4_*). Not provided: CCCrypt / CCCryptor* (no audited binary imports
// them) and key derivation.
namespace alchemy::gui::security {
namespace {

constexpr std::string_view lib = libsystem_library;

enum class Kind { md5, sha1, sha224, sha256, sha384, sha512 };

struct Layout {
    std::uint64_t size;           // Apple's context size
    std::uint64_t state, length, buffer;
    std::int64_t num;             // offset of the buffered-byte count (-1: derived)
};
// MD5: A..D, Nl/Nh, data[16], num.  SHA1: h0..h4, Nl/Nh, data[16], num.
// SHA256: count[2], hash[8], wbuf[16].  SHA512: count[2] (64-bit), hash[8], wbuf[16].
constexpr Layout md5_layout{92, 0, 16, 24, 88}, sha1_layout{96, 0, 20, 28, 92},
    sha256_layout{104, 8, 0, 40, -1}, sha512_layout{208, 16, 0, 80, -1};

bool wide_words(Kind kind) { return kind == Kind::sha384 || kind == Kind::sha512; }
std::size_t block_size(Kind kind) { return wide_words(kind) ? 128 : 64; }
std::size_t digest_size(Kind kind) {
    switch (kind) {
    case Kind::md5: return 16;
    case Kind::sha1: return 20;
    case Kind::sha224: return 28;
    case Kind::sha256: return 32;
    case Kind::sha384: return 48;
    case Kind::sha512: return 64;
    }
    return 0;
}
std::size_t state_words(Kind kind) { return kind == Kind::md5 ? 4 : kind == Kind::sha1 ? 5 : 8; }
const Layout& layout_of(Kind kind) {
    switch (kind) {
    case Kind::md5: return md5_layout;
    case Kind::sha1: return sha1_layout;
    case Kind::sha224: case Kind::sha256: return sha256_layout;
    default: return sha512_layout;
    }
}

constexpr std::array<std::uint32_t, 64> sha256_k{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr std::array<std::uint64_t, 80> sha512_k{
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
    0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
    0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
    0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
    0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
    0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
    0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3]; }
std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t{be32(p)} << 32) | be32(p + 4); }
std::uint32_t le32(const std::uint8_t* p) { return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24); }

// A running digest (host side); loaded from / stored to a guest context.
struct Digest {
    Kind kind{};
    std::array<std::uint64_t, 8> h{};
    std::uint64_t bytes{};        // total message length
    std::array<std::uint8_t, 128> buffer{};

    explicit Digest(Kind k) : kind(k) {
        switch (kind) {
        case Kind::md5: h = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}; break;
        case Kind::sha1: h = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0}; break;
        case Kind::sha224: h = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4}; break;
        case Kind::sha256: h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; break;
        case Kind::sha384:
            h = {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
                 0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};
            break;
        case Kind::sha512:
            h = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                 0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
            break;
        }
    }

    void compress(const std::uint8_t* block) {
        switch (kind) {
        case Kind::md5: md5_block(block); break;
        case Kind::sha1: sha1_block(block); break;
        case Kind::sha224: case Kind::sha256: sha256_block(block); break;
        default: sha512_block(block); break;
        }
    }

    void update(std::span<const std::uint8_t> data) {
        const auto block = block_size(kind);
        auto used = static_cast<std::size_t>(bytes % block);
        bytes += data.size();
        std::size_t offset = 0;
        if (used) {
            const auto take = std::min(block - used, data.size());
            std::memcpy(buffer.data() + used, data.data(), take);
            offset = take; used += take;
            if (used < block) return;
            compress(buffer.data());
        }
        for (; offset + block <= data.size(); offset += block) compress(data.data() + offset);
        if (offset < data.size()) std::memcpy(buffer.data(), data.data() + offset, data.size() - offset);
    }

    Bytes finish() {
        const auto block = block_size(kind);
        const auto length_bytes = wide_words(kind) ? 16U : 8U;
        const auto bits = bytes * 8;
        Bytes pad{0x80};
        while ((bytes + pad.size()) % block != block - length_bytes) pad.push_back(0);
        if (kind == Kind::md5) {
            for (unsigned i = 0; i < 8; ++i) pad.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
        } else {
            for (unsigned i = 8; i < length_bytes; ++i) pad.push_back(0);
            for (int i = 7; i >= 0; --i) pad.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
        }
        update(pad);
        Bytes out;
        const auto size = digest_size(kind);
        for (std::size_t i = 0; out.size() < size; ++i) {
            if (kind == Kind::md5) {
                for (unsigned b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(h[i] >> (8 * b)));
            } else if (wide_words(kind)) {
                for (int b = 7; b >= 0; --b) out.push_back(static_cast<std::uint8_t>(h[i] >> (8 * b)));
            } else {
                for (int b = 3; b >= 0; --b) out.push_back(static_cast<std::uint8_t>(h[i] >> (8 * b)));
            }
        }
        out.resize(size);
        return out;
    }

    void md5_block(const std::uint8_t* p) {
        static constexpr std::array<std::uint32_t, 64> s{7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        static const auto k = [] {
            std::array<std::uint32_t, 64> value{};
            // floor(abs(sin(i + 1)) * 2^32)
            for (int i = 0; i < 64; ++i) value[static_cast<std::size_t>(i)] =
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(std::abs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
            return value;
        }();
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) m[i] = le32(p + i * 4);
        auto a = static_cast<std::uint32_t>(h[0]), b = static_cast<std::uint32_t>(h[1]),
             c = static_cast<std::uint32_t>(h[2]), d = static_cast<std::uint32_t>(h[3]);
        for (std::uint32_t i = 0; i < 64; ++i) {
            std::uint32_t f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            f += a + k[i] + m[g];
            a = d; d = c; c = b;
            b += std::rotl(f, static_cast<int>(s[i]));
        }
        h[0] = static_cast<std::uint32_t>(h[0] + a); h[1] = static_cast<std::uint32_t>(h[1] + b);
        h[2] = static_cast<std::uint32_t>(h[2] + c); h[3] = static_cast<std::uint32_t>(h[3] + d);
    }

    void sha1_block(const std::uint8_t* p) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
        for (std::size_t i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::array<std::uint32_t, 5> v{};
        for (std::size_t i = 0; i < 5; ++i) v[i] = static_cast<std::uint32_t>(h[i]);
        auto [a, b, c, d, e] = v;
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
            else { f = b ^ c ^ d; k = 0xca62c1d6; }
            const auto t = std::rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = std::rotl(b, 30); b = a; a = t;
        }
        const std::array<std::uint32_t, 5> add{a, b, c, d, e};
        for (std::size_t i = 0; i < 5; ++i) h[i] = static_cast<std::uint32_t>(h[i] + add[i]);
    }

    void sha256_block(const std::uint8_t* p) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::array<std::uint32_t, 8> v{};
        for (std::size_t i = 0; i < 8; ++i) v[i] = static_cast<std::uint32_t>(h[i]);
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
            const auto ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const auto t1 = v[7] + s1 + ch + sha256_k[i] + w[i];
            const auto s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
            const auto maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const auto t2 = s0 + maj;
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) h[i] = static_cast<std::uint32_t>(h[i] + v[i]);
    }

    void sha512_block(const std::uint8_t* p) {
        std::array<std::uint64_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) w[i] = be64(p + i * 8);
        for (std::size_t i = 16; i < 80; ++i) {
            const auto s0 = std::rotr(w[i - 15], 1) ^ std::rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const auto s1 = std::rotr(w[i - 2], 19) ^ std::rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h;
        for (std::size_t i = 0; i < 80; ++i) {
            const auto s1 = std::rotr(v[4], 14) ^ std::rotr(v[4], 18) ^ std::rotr(v[4], 41);
            const auto ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const auto t1 = v[7] + s1 + ch + sha512_k[i] + w[i];
            const auto s0 = std::rotr(v[0], 28) ^ std::rotr(v[0], 34) ^ std::rotr(v[0], 39);
            const auto maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            v = {t1 + s0 + maj, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) h[i] += v[i];
    }

    // ---- guest contexts ---------------------------------------------------------------------------
    void store(GuestMemory& m, Address ctx) const {
        const auto& l = layout_of(kind);
        for (std::size_t i = 0; i < state_words(kind); ++i) {
            if (wide_words(kind)) m.write64(ctx + l.state + i * 8, h[i]);
            else m.write32(ctx + l.state + i * 4, static_cast<std::uint32_t>(h[i]));
        }
        m.write64(ctx + l.length, bytes * 8);   // bit count (low 64 bits)
        if (wide_words(kind)) m.write64(ctx + l.length + 8, 0);
        m.write_bytes(ctx + l.buffer, std::span(buffer.data(), block_size(kind)));
        if (l.num >= 0) m.write32(ctx + static_cast<Address>(l.num), static_cast<std::uint32_t>(bytes % block_size(kind)));
    }
    static Digest load(GuestMemory& m, Address ctx, Kind kind) {
        Digest d(kind);
        const auto& l = layout_of(kind);
        for (std::size_t i = 0; i < state_words(kind); ++i)
            d.h[i] = wide_words(kind) ? m.read64(ctx + l.state + i * 8) : m.read32(ctx + l.state + i * 4);
        d.bytes = m.read64(ctx + l.length) / 8;
        m.read_bytes(ctx + l.buffer, std::span(d.buffer.data(), block_size(kind)));
        return d;
    }
};

Bytes read_guest(GuestMemory& m, Address data, std::uint64_t length) {
    if (length > (1ULL << 32)) fail("CommonCrypto input larger than 4 GiB");
    Bytes bytes(static_cast<std::size_t>(length));
    if (length) {
        if (!data) fail("CommonCrypto: null input with a non-zero length");
        m.read_bytes(data, bytes);
    }
    return bytes;
}

Bytes digest(Kind kind, std::span<const std::uint8_t> data) {
    Digest d(kind);
    d.update(data);
    return d.finish();
}

// CCHmacAlgorithm values.
std::optional<Kind> hmac_kind(std::uint64_t algorithm) {
    switch (algorithm) {
    case 0: return Kind::sha1;
    case 1: return Kind::md5;
    case 2: return Kind::sha256;
    case 3: return Kind::sha384;
    case 4: return Kind::sha512;
    case 5: return Kind::sha224;
    default: return std::nullopt;
    }
}

// CCHmacContext (384 bytes): algorithm (u32) at 0, the inner digest context at
// 8 (up to 208 bytes), the outer padded key block at 216 (up to 128 bytes).
constexpr Address hmac_inner = 8, hmac_outer_key = 216;

Bytes padded_key(Kind kind, std::span<const std::uint8_t> key, std::uint8_t pad) {
    Bytes block(block_size(kind), 0);
    const auto material = key.size() > block.size() ? digest(kind, key) : Bytes(key.begin(), key.end());
    std::copy(material.begin(), material.end(), block.begin());
    for (auto& value : block) value ^= pad;
    return block;
}

Bytes hmac(std::uint64_t algorithm, std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    const auto kind = hmac_kind(algorithm);
    if (!kind) fail("unsupported CCHmacAlgorithm " + std::to_string(algorithm));
    Digest inner(*kind);
    inner.update(padded_key(*kind, key, 0x36));
    inner.update(data);
    Digest outer(*kind);
    outer.update(padded_key(*kind, key, 0x5c));
    outer.update(inner.finish());
    return outer.finish();
}

} // namespace

void install_common_crypto(Security& sec) {
    auto b = sec.b;
    const auto install = [b](std::string name, transmute::GuestCallHandler handler) {
        if (b->exported(lib, name)) return;
        b->function(lib, name, std::move(handler));
    };
    struct Family { const char* name; Kind kind; const char* context; };
    for (const auto& family : {Family{"MD5", Kind::md5, "MD5"}, Family{"SHA1", Kind::sha1, "SHA1"},
                               Family{"SHA224", Kind::sha224, "SHA256"}, Family{"SHA256", Kind::sha256, "SHA256"},
                               Family{"SHA384", Kind::sha384, "SHA512"}, Family{"SHA512", Kind::sha512, "SHA512"}}) {
        const auto kind = family.kind;
        const std::string prefix = std::string("_CC_") + family.name;
        // unsigned char *CC_X(const void *data, CC_LONG len, unsigned char *md)
        install(prefix, [kind](CpuState& s, GuestMemory& m) {
            const auto out = digest(kind, read_guest(m, s.x[0], static_cast<std::uint32_t>(s.x[1])));
            if (s.x[2]) m.write_bytes(s.x[2], out);
            s.x[0] = s.x[2];
        });
        install(prefix + "_Init", [kind](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) fail("CC_*_Init with a null context");
            Digest(kind).store(m, s.x[0]);
            s.x[0] = 1;
        });
        install(prefix + "_Update", [kind](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) fail("CC_*_Update with a null context");
            auto d = Digest::load(m, s.x[0], kind);
            d.update(read_guest(m, s.x[1], static_cast<std::uint32_t>(s.x[2])));
            d.store(m, s.x[0]);
            s.x[0] = 1;
        });
        // int CC_X_Final(unsigned char *md, CC_X_CTX *c)
        install(prefix + "_Final", [kind](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) fail("CC_*_Final with a null context");
            auto d = Digest::load(m, s.x[1], kind);
            const auto out = d.finish();
            if (s.x[0]) m.write_bytes(s.x[0], out);
            m.fill_bytes(s.x[1], 0, layout_of(kind).size);
            s.x[0] = 1;
        });
    }

    // void CCHmac(CCHmacAlgorithm, const void *key, size_t keyLength, const void *data, size_t dataLength, void *macOut)
    install("_CCHmac", [](CpuState& s, GuestMemory& m) {
        const auto mac = hmac(s.x[0], read_guest(m, s.x[1], s.x[2]), read_guest(m, s.x[3], s.x[4]));
        if (s.x[5]) m.write_bytes(s.x[5], mac);
        s.x[0] = 0;
    });
    // void CCHmacInit(CCHmacContext *ctx, CCHmacAlgorithm, const void *key, size_t keyLength)
    install("_CCHmacInit", [](CpuState& s, GuestMemory& m) {
        const auto kind = hmac_kind(s.x[1]);
        if (!s.x[0] || !kind) fail("CCHmacInit: null context or unsupported algorithm " + std::to_string(s.x[1]));
        const auto key = read_guest(m, s.x[2], s.x[3]);
        m.fill_bytes(s.x[0], 0, 384);
        m.write32(s.x[0], static_cast<std::uint32_t>(s.x[1]));
        Digest inner(*kind);
        inner.update(padded_key(*kind, key, 0x36));
        inner.store(m, s.x[0] + hmac_inner);
        m.write_bytes(s.x[0] + hmac_outer_key, padded_key(*kind, key, 0x5c));
        s.x[0] = 0;
    });
    install("_CCHmacUpdate", [](CpuState& s, GuestMemory& m) {
        const auto kind = s.x[0] ? hmac_kind(m.read32(s.x[0])) : std::nullopt;
        if (!kind) fail("CCHmacUpdate on an uninitialized context");
        auto inner = Digest::load(m, s.x[0] + hmac_inner, *kind);
        inner.update(read_guest(m, s.x[1], s.x[2]));
        inner.store(m, s.x[0] + hmac_inner);
        s.x[0] = 0;
    });
    install("_CCHmacFinal", [](CpuState& s, GuestMemory& m) {
        const auto kind = s.x[0] ? hmac_kind(m.read32(s.x[0])) : std::nullopt;
        if (!kind) fail("CCHmacFinal on an uninitialized context");
        auto inner = Digest::load(m, s.x[0] + hmac_inner, *kind);
        Bytes outer_key(block_size(*kind));
        m.read_bytes(s.x[0] + hmac_outer_key, outer_key);
        Digest outer(*kind);
        outer.update(outer_key);
        outer.update(inner.finish());
        if (s.x[1]) m.write_bytes(s.x[1], outer.finish());
        m.fill_bytes(s.x[0], 0, 384);
        s.x[0] = 0;
    });
    // CCRNGStatus CCRandomGenerateBytes(void *bytes, size_t count)
    install("_CCRandomGenerateBytes", [](CpuState& s, GuestMemory& m) {
        if (s.x[1] > (1ULL << 30)) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(-4300)); return; }   // kCCParamError
        Bytes bytes(static_cast<std::size_t>(s.x[1]));
        if (!bytes.empty()) {
            if (!s.x[0] || !host::crypto::random(std::as_writable_bytes(std::span(bytes)))) {
                s.x[0] = static_cast<Address>(static_cast<std::int64_t>(-4300));
                return;
            }
            m.write_bytes(s.x[0], bytes);
        }
        s.x[0] = 0;   // kCCSuccess
    });
}

} // namespace alchemy::gui::security
