#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/darwin/security/crypto_core.hpp"
#include "alchemy/host/crypto.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>


// The host side of Alchemy's CryptoKit overlay (graphics/swift/CryptoKit).
// Project-private C entry points exported from the libSystem identity; the
// overlay declares them with @_silgen_name and the Swift link-stub generator
// binds them to /usr/lib/libSystem.B.dylib. Every argument is a 64-bit
// integer or guest pointer (x0-x7) and every entry point returns a status in
// x0, which the overlay maps to CryptoKitError:
//   0 success, 1 incorrect key size, 2 incorrect parameter size,
//   3 authentication failure, 4 invalid P-521 key (scalar or point).
// Randomness (keys, nonces) comes from the host's system-preferred RNG
// (host::crypto::random); the cryptography is crypto_core.cpp.
//
//   _alchemy_cryptokit_random(buffer, count)
//   _alchemy_cryptokit_hash(algorithm, data, count, digest)           algorithm 1 SHA-256, 2 SHA-384, 3 SHA-512
//   _alchemy_cryptokit_hkdf_extract(algorithm, salt, saltCount, key, keyCount, prk)
//   _alchemy_cryptokit_hkdf_expand(algorithm, prk, prkCount, info, infoCount, output, outputCount)
//   _alchemy_cryptokit_chachapoly_seal(key32, nonce12, aad, aadCount, plaintext, count, ciphertext, tag16)
//   _alchemy_cryptokit_chachapoly_open(key32, nonce12, aad, aadCount, ciphertext, count, tag16, plaintext)
//   _alchemy_cryptokit_p521_generate(compact, private66, public132)
//   _alchemy_cryptokit_p521_public_key(private66, public132)
//   _alchemy_cryptokit_p521_validate(public132)
//   _alchemy_cryptokit_p521_shared_secret(private66, public132, secret66)
namespace alchemy::gui::completion_kit::graphics {
namespace {

using namespace crypto;

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr std::uint64_t status_ok = 0;
constexpr std::uint64_t status_incorrect_parameter_size = 2;
constexpr std::uint64_t status_authentication_failure = 3;
constexpr std::uint64_t status_invalid_key = 4;
constexpr std::uint64_t max_length = std::uint64_t{1} << 31;   // a sanity bound on guest buffer lengths

Bytes read(GuestMemory& m, Address address, std::uint64_t count, std::string_view what) {
    if (count > max_length) fail("CryptoKit " + std::string(what) + ": length " + std::to_string(count) + " out of range");
    Bytes bytes(static_cast<std::size_t>(count));
    if (!count) return bytes;
    if (!address) fail("CryptoKit " + std::string(what) + ": null buffer");
    m.read_bytes(address, bytes);
    return bytes;
}

template <std::size_t N>
std::array<std::uint8_t, N> read_fixed(GuestMemory& m, Address address, std::string_view what) {
    std::array<std::uint8_t, N> bytes{};
    if (!address) fail("CryptoKit " + std::string(what) + ": null buffer");
    m.read_bytes(address, bytes);
    return bytes;
}

void write(GuestMemory& m, Address address, ByteView bytes, std::string_view what) {
    if (bytes.empty()) return;
    if (!address) fail("CryptoKit " + std::string(what) + ": null output buffer");
    m.write_bytes(address, bytes);
}

void random_fill(std::span<std::uint8_t> bytes) {
    if (!host::crypto::random(std::as_writable_bytes(bytes))) fail("CryptoKit: the host random generator failed");
}

std::optional<Hash> algorithm(std::uint64_t value) {
    switch (value) {
    case 1: return Hash::sha256;
    case 2: return Hash::sha384;
    case 3: return Hash::sha512;
    default: return std::nullopt;
    }
}

} // namespace

void install_crypto(Support& support) {
    support.function(libsystem, "_alchemy_cryptokit_random", [](CpuState& s, GuestMemory& m) {
        if (s.x[1] > max_length) fail("CryptoKit random: length " + std::to_string(s.x[1]) + " out of range");
        Bytes bytes(static_cast<std::size_t>(s.x[1]));
        random_fill(bytes);
        write(m, s.x[0], bytes, "random");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_hash", [](CpuState& s, GuestMemory& m) {
        const auto kind = algorithm(s.x[0]);
        if (!kind) { s.x[0] = status_incorrect_parameter_size; return; }
        write(m, s.x[3], hash(*kind, read(m, s.x[1], s.x[2], "hash")), "hash");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_hkdf_extract", [](CpuState& s, GuestMemory& m) {
        const auto kind = algorithm(s.x[0]);
        if (!kind) { s.x[0] = status_incorrect_parameter_size; return; }
        const auto salt = read(m, s.x[1], s.x[2], "HKDF salt");
        const auto key = read(m, s.x[3], s.x[4], "HKDF key");
        write(m, s.x[5], hkdf_extract(*kind, salt, key), "HKDF");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_hkdf_expand", [](CpuState& s, GuestMemory& m) {
        const auto kind = algorithm(s.x[0]);
        if (!kind) { s.x[0] = status_incorrect_parameter_size; return; }
        const auto prk = read(m, s.x[1], s.x[2], "HKDF key");
        const auto info = read(m, s.x[3], s.x[4], "HKDF info");
        if (s.x[6] > max_length) { s.x[0] = status_incorrect_parameter_size; return; }
        const auto output = hkdf_expand(*kind, prk, info, static_cast<std::size_t>(s.x[6]));
        if (!output) { s.x[0] = status_incorrect_parameter_size; return; }
        write(m, s.x[5], *output, "HKDF");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_chachapoly_seal", [](CpuState& s, GuestMemory& m) {
        const auto key = read_fixed<32>(m, s.x[0], "ChaChaPoly key");
        const auto nonce = read_fixed<12>(m, s.x[1], "ChaChaPoly nonce");
        const auto aad = read(m, s.x[2], s.x[3], "ChaChaPoly authenticated data");
        const auto plaintext = read(m, s.x[4], s.x[5], "ChaChaPoly plaintext");
        const auto sealed = aead_seal(key, nonce, aad, plaintext);
        write(m, s.x[6], sealed.ciphertext, "ChaChaPoly ciphertext");
        write(m, s.x[7], sealed.tag, "ChaChaPoly tag");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_chachapoly_open", [](CpuState& s, GuestMemory& m) {
        const auto key = read_fixed<32>(m, s.x[0], "ChaChaPoly key");
        const auto nonce = read_fixed<12>(m, s.x[1], "ChaChaPoly nonce");
        const auto aad = read(m, s.x[2], s.x[3], "ChaChaPoly authenticated data");
        const auto ciphertext = read(m, s.x[4], s.x[5], "ChaChaPoly ciphertext");
        const auto tag = read_fixed<16>(m, s.x[6], "ChaChaPoly tag");
        const auto plaintext = aead_open(key, nonce, aad, ciphertext, tag);
        if (!plaintext) { s.x[0] = status_authentication_failure; return; }
        write(m, s.x[7], *plaintext, "ChaChaPoly plaintext");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_p521_generate", [](CpuState& s, GuestMemory& m) {
        const auto pair = p521_generate(random_fill, s.x[0] != 0);
        write(m, s.x[1], pair.private_key, "P521 private key");
        write(m, s.x[2], pair.public_key, "P521 public key");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_p521_public_key", [](CpuState& s, GuestMemory& m) {
        const auto point = p521_public_key(read_fixed<p521_bytes>(m, s.x[0], "P521 private key"));
        if (!point) { s.x[0] = status_invalid_key; return; }
        write(m, s.x[1], *point, "P521 public key");
        s.x[0] = status_ok;
    });
    support.function(libsystem, "_alchemy_cryptokit_p521_validate", [](CpuState& s, GuestMemory& m) {
        s.x[0] = p521_valid_public(read_fixed<2 * p521_bytes>(m, s.x[0], "P521 public key")) ? status_ok : status_invalid_key;
    });
    support.function(libsystem, "_alchemy_cryptokit_p521_shared_secret", [](CpuState& s, GuestMemory& m) {
        const auto secret = p521_shared_secret(read_fixed<p521_bytes>(m, s.x[0], "P521 private key"),
                                               read_fixed<2 * p521_bytes>(m, s.x[1], "P521 public key"));
        if (!secret) { s.x[0] = status_invalid_key; return; }
        write(m, s.x[2], *secret, "P521 shared secret");
        s.x[0] = status_ok;
    });
}

} // namespace alchemy::gui::completion_kit::graphics
