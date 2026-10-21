#pragma once

// Host asymmetric keys over host::crypto: RSA and NIST P-256/384/521 EC,
// in the external representations SecKey uses (RSA: PKCS#1 RSAPublicKey /
// RSAPrivateKey DER; EC: ANSI X9.63 04||X||Y, private keys 04||X||Y||K).
// Host only. Owner: v-netsec.

#include "alchemy/darwin/security/bridge.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace alchemy::gui::security {

struct KeyMaterial {
    enum class Type { rsa, ec };
    Type type{Type::rsa};
    bool is_private{};
    unsigned bits{};                          // modulus / curve size
    // RSA components (big-endian, minimal); private ones only for private keys.
    Bytes n, e, d, p, q, dp, dq, qinv;
    // EC: coordinate size in bytes (32, 48, 66), point and private scalar.
    std::size_t coordinate{};
    Bytes x, y, k;
    [[nodiscard]] Bytes public_external() const;    // SecKeyCopyExternalRepresentation of the public key
    [[nodiscard]] Bytes private_external() const;   // ... of the private key (private keys only)
    [[nodiscard]] Bytes spki() const;                // DER SubjectPublicKeyInfo
    [[nodiscard]] std::size_t block_size() const noexcept;   // SecKeyGetBlockSize
};
using KeyPtr = std::shared_ptr<const KeyMaterial>;

// Parsers return nullptr when the input is not that format.
[[nodiscard]] KeyPtr key_from_spki(std::span<const std::uint8_t> der);           // SubjectPublicKeyInfo
[[nodiscard]] KeyPtr key_from_external(KeyMaterial::Type type, bool is_private, std::span<const std::uint8_t> bytes);
[[nodiscard]] KeyPtr key_from_pkcs8(std::span<const std::uint8_t> der);          // unencrypted PrivateKeyInfo
[[nodiscard]] KeyPtr key_from_sec1(std::span<const std::uint8_t> der);           // ECPrivateKey
[[nodiscard]] KeyPtr public_key_of(const KeyMaterial& key);
[[nodiscard]] KeyPtr generate_key(KeyMaterial::Type type, unsigned bits);

// SecKeyAlgorithm operations. `algorithm` is the algorithm constant's string.
enum class KeyOperation { encrypt = 0, decrypt = 1, sign = 2, verify = 3, key_exchange = 4 };
[[nodiscard]] bool key_supports(const KeyMaterial& key, KeyOperation operation, std::string_view algorithm);
struct KeyResult {
    std::optional<Bytes> value;
    std::int32_t status{};
    std::string error;
};
[[nodiscard]] KeyResult key_sign(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data);
[[nodiscard]] KeyResult key_verify(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data,
                                   std::span<const std::uint8_t> signature);   // value = {} on success
[[nodiscard]] KeyResult key_encrypt(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data);
[[nodiscard]] KeyResult key_decrypt(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data);
[[nodiscard]] KeyResult key_exchange(const KeyMaterial& key, const KeyMaterial& peer, std::string_view algorithm);

// Digests over host::crypto ("SHA1", "SHA224" (where the host has it), "SHA256", "SHA384", "SHA512", "MD5").
[[nodiscard]] std::optional<Bytes> digest(std::string_view name, std::span<const std::uint8_t> data);

// The SecKeyAlgorithm constants: {exported name, string value}.
struct AlgorithmName { const char* symbol; const char* value; };
[[nodiscard]] std::span<const AlgorithmName> key_algorithms();

} // namespace alchemy::gui::security
