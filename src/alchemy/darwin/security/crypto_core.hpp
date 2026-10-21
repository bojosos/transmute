#pragma once

// Host-only cryptography behind Alchemy's CryptoKit overlay
// (src/alchemy/darwin/kit/completion/graphics/swift/CryptoKit, crypto_exports.cpp):
//   - SHA-256, SHA-384, SHA-512 (FIPS 180-4),
//   - HMAC (RFC 2104) and HKDF (RFC 5869) over any of them,
//   - ChaCha20, Poly1305 and the ChaCha20-Poly1305 AEAD (RFC 8439),
//   - P-521 ECDH (FIPS 186-4 / SEC 2 curve secp521r1): public keys, point
//     validation and the shared x-coordinate.
// Portable C++20 with no platform headers and no guest access; tested by
// tests/crypto_tests.cpp. Randomness is the caller's (crypto_exports.cpp).
// The P-521 arithmetic is straightforward double-and-add and is not
// constant-time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace alchemy::gui::completion_kit::crypto {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// ---- SHA-2 ------------------------------------------------------------------------
enum class Hash : std::uint32_t { sha256 = 1, sha384 = 2, sha512 = 3 };

[[nodiscard]] std::size_t digest_size(Hash hash);   // 32, 48, 64
[[nodiscard]] std::size_t block_size(Hash hash);    // 64, 128, 128

// Incremental hashing.
class Hasher {
public:
    explicit Hasher(Hash hash);
    void update(ByteView bytes);
    [[nodiscard]] Bytes finish();   // the digest; the hasher is then reset
    [[nodiscard]] Hash kind() const { return hash_; }

private:
    void compress(const std::uint8_t* block);
    void reset();
    Hash hash_;
    std::array<std::uint64_t, 8> state_{};   // SHA-256 uses the low 32 bits of each word
    std::array<std::uint8_t, 128> buffer_{};
    std::size_t buffered_{};
    std::uint64_t length_{};   // bytes hashed (messages below 2^61 bytes)
};

[[nodiscard]] Bytes hash(Hash hash, ByteView message);
[[nodiscard]] std::array<std::uint8_t, 32> sha256(ByteView message);
[[nodiscard]] std::array<std::uint8_t, 64> sha512(ByteView message);

// ---- HMAC / HKDF ---------------------------------------------------------------------
[[nodiscard]] Bytes hmac(Hash hash, ByteView key, ByteView message);
[[nodiscard]] Bytes hkdf_extract(Hash hash, ByteView salt, ByteView input_key);
// nullopt when `length` exceeds 255 * digest_size(hash).
[[nodiscard]] std::optional<Bytes> hkdf_expand(Hash hash, ByteView pseudorandom_key, ByteView info, std::size_t length);
[[nodiscard]] std::optional<Bytes> hkdf(Hash hash, ByteView input_key, ByteView salt, ByteView info, std::size_t length);

// ---- ChaCha20-Poly1305 (RFC 8439) -------------------------------------------------------
using ChaChaKey = std::array<std::uint8_t, 32>;
using ChaChaNonce = std::array<std::uint8_t, 12>;
using Tag = std::array<std::uint8_t, 16>;

[[nodiscard]] std::array<std::uint8_t, 64> chacha20_block(const ChaChaKey& key, std::uint32_t counter, const ChaChaNonce& nonce);
// XORs `data` with the key stream starting at block `counter`.
[[nodiscard]] Bytes chacha20(const ChaChaKey& key, std::uint32_t counter, const ChaChaNonce& nonce, ByteView data);
[[nodiscard]] Tag poly1305(const std::array<std::uint8_t, 32>& one_time_key, ByteView message);

struct Sealed {
    Bytes ciphertext;
    Tag tag{};
};
[[nodiscard]] Sealed aead_seal(const ChaChaKey& key, const ChaChaNonce& nonce, ByteView aad, ByteView plaintext);
// nullopt when the tag does not authenticate (compared in constant time).
[[nodiscard]] std::optional<Bytes> aead_open(const ChaChaKey& key, const ChaChaNonce& nonce, ByteView aad,
                                             ByteView ciphertext, const Tag& tag);

// ---- P-521 ---------------------------------------------------------------------------
// Scalars and coordinates are 66-byte big-endian; a public key's raw
// representation is X || Y (132 bytes), as CryptoKit's rawRepresentation.
inline constexpr std::size_t p521_bytes = 66;
using P521Scalar = std::array<std::uint8_t, p521_bytes>;
using P521Point = std::array<std::uint8_t, 2 * p521_bytes>;

extern const P521Scalar p521_order;       // n
extern const P521Point p521_generator;    // G

// 1 <= d <= n - 1.
[[nodiscard]] bool p521_valid_private(const P521Scalar& scalar);
// Both coordinates below p and on y^2 = x^3 - 3x + b (the cofactor is 1).
[[nodiscard]] bool p521_valid_public(const P521Point& point);
// k * P for any 66-byte k (including 0 and values >= n); nullopt is the
// point at infinity or an invalid `point`.
[[nodiscard]] std::optional<P521Point> p521_multiply(const P521Scalar& scalar, const P521Point& point);
// d * G; nullopt for an invalid private key.
[[nodiscard]] std::optional<P521Point> p521_public_key(const P521Scalar& private_key);
// The x-coordinate of d * Q; nullopt for an invalid key or point, or when
// the product is the point at infinity.
[[nodiscard]] std::optional<P521Scalar> p521_shared_secret(const P521Scalar& private_key, const P521Point& public_key);
// y <= p - y (draft-jivsov-ecc-compact): the key is compact-representable.
[[nodiscard]] bool p521_compact(const P521Point& public_key);

struct P521KeyPair {
    P521Scalar private_key{};
    P521Point public_key{};
};
// Uniform private key from `random` (rejection sampling); with `compact`,
// regenerates until the public key is compact-representable.
[[nodiscard]] P521KeyPair p521_generate(const std::function<void(std::span<std::uint8_t>)>& random, bool compact);

// Constant-time equality of two equally long byte strings.
[[nodiscard]] bool equal_constant_time(ByteView a, ByteView b);

} // namespace alchemy::gui::completion_kit::crypto
