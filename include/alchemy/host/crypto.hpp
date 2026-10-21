#pragma once

// Host area "crypto" (owner: r2). The rules are in include/alchemy/host/host.hpp. Random numbers,
// digests, ciphers, keys and certificates, and the TLS stack.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::host::crypto {

// Cryptographically secure random bytes (BCryptGenRandom, getrandom).
[[nodiscard]] Status random(Bytes buffer);

// Overwrites `buffer` with zeros in a way the compiler keeps (SecureZeroMemory, explicit_bzero).
void secure_zero(Bytes buffer) noexcept;

// Data sealed for the current user of this machine (DPAPI, the login
// keychain's key on macOS): `entropy` must match to unseal; `description`
// is stored in the clear. Never prompts.
[[nodiscard]] Result<std::vector<std::uint8_t>> protect(ConstBytes data, ConstBytes entropy, std::string_view description);
[[nodiscard]] Result<std::vector<std::uint8_t>> unprotect(ConstBytes data, ConstBytes entropy);

// A raw block cipher (ECB, one block at a time; the caller chains modes).
enum class BlockAlgorithm : std::uint8_t { aes, des, triple_des };
class BlockCipher {
public:
    // Error::invalid_argument for a key of the wrong size, Error::unsupported
    // when the host lacks the algorithm.
    [[nodiscard]] static Result<BlockCipher> create(BlockAlgorithm algorithm, ConstBytes key);
    BlockCipher() = default;  // no key
    BlockCipher(BlockCipher&& other) noexcept
        : algorithm_(std::exchange(other.algorithm_, nullptr)), key_(std::exchange(other.key_, nullptr)), block_(other.block_) {}
    BlockCipher& operator=(BlockCipher&& other) noexcept;
    BlockCipher(const BlockCipher&) = delete;
    BlockCipher& operator=(const BlockCipher&) = delete;
    ~BlockCipher();

    [[nodiscard]] std::size_t block_size() const noexcept { return block_; }
    // Whole blocks of `input` into `output` (the same size).
    [[nodiscard]] Status encrypt(ConstBytes input, Bytes output) const;
    [[nodiscard]] Status decrypt(ConstBytes input, Bytes output) const;

private:
    void* algorithm_{};
    void* key_{};
    std::size_t block_{};
};

// Digests (hash, PBKDF2's HMAC, signatures).
enum class Digest : std::uint8_t { sha1, sha224, sha256, sha384, sha512, md5 };
// PBKDF2 with HMAC-`digest` into `output`. Error::unsupported when the host lacks the digest.
[[nodiscard]] Status pbkdf2(Digest digest, ConstBytes password, ConstBytes salt, std::uint32_t rounds, Bytes output);

// Whether the host implements `digest` (hash, PBKDF2, signatures).
[[nodiscard]] bool supports(Digest digest) noexcept;
// The digest of `data`.
[[nodiscard]] Result<std::vector<std::uint8_t>> hash(Digest digest, ConstBytes data);

// ---- Asymmetric keys ----
// Keys travel as their components, big-endian. A public RSA key has n and e
// only; a private one every CRT part. EC keys are on NIST P-256, P-384 or
// P-521 (`coordinate` 32, 48 or 66 bytes); a public one has no k. Every call
// imports the key: Error::bad_handle when the host refuses it, any other
// error when the operation itself fails (a signature that does not verify
// included).
struct RsaKey {
    unsigned bits{};
    std::vector<std::uint8_t> n, e, d, p, q, dp, dq, qinv;
};
struct EcKey {
    std::size_t coordinate{};
    std::vector<std::uint8_t> x, y, k;
};
// Imports the key (its private half when `use_private`) and discards it.
[[nodiscard]] Status check(const RsaKey& key, bool use_private);
[[nodiscard]] Status check(const EcKey& key, bool use_private);
[[nodiscard]] Result<RsaKey> generate_rsa(unsigned bits);
[[nodiscard]] Result<EcKey> generate_ec(std::size_t coordinate);

// RSA paddings. `none` is the raw operation on one modulus-sized block; pss
// uses a salt as long as the hash; OAEP has no label. `digest` names the hash
// for pkcs1 signatures (none: no DigestInfo), pss and oaep.
enum class RsaPadding : std::uint8_t { none, pkcs1, pss, oaep };
// Outputs are as long as the host wrote (a raw result may be shorter than the modulus).
[[nodiscard]] Result<std::vector<std::uint8_t>> rsa_sign(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes hash);
[[nodiscard]] Status rsa_verify(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes hash, ConstBytes signature);
[[nodiscard]] Result<std::vector<std::uint8_t>> rsa_encrypt(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes data);
[[nodiscard]] Result<std::vector<std::uint8_t>> rsa_decrypt(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes data);
// ECDSA over a hash; signatures are r || s, each `coordinate` bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> ecdsa_sign(const EcKey& key, ConstBytes hash);
[[nodiscard]] Status ecdsa_verify(const EcKey& key, ConstBytes hash, ConstBytes signature);
// The ECDH shared secret (the x coordinate, big-endian) of `key` (private) and `peer` (public).
[[nodiscard]] Result<std::vector<std::uint8_t>> ecdh(const EcKey& key, const EcKey& peer);

// ---- Certificates and trust ----

// An X.509 certificate the host parsed. Copies share it.
class Certificate {
public:
    // Error::invalid_argument when `der` is not one DER certificate.
    [[nodiscard]] static Result<Certificate> parse(ConstBytes der);
    Certificate() = default;  // no certificate
    Certificate(const Certificate& other);
    Certificate& operator=(const Certificate& other);
    Certificate(Certificate&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Certificate& operator=(Certificate&& other) noexcept;
    ~Certificate();
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] std::vector<std::uint8_t> der() const;
    [[nodiscard]] std::string common_name() const;    // the subject's commonName ("" without one)
    [[nodiscard]] std::string display_name() const;   // the host's simple display name of the subject
    [[nodiscard]] std::string email() const;          // the subject's e-mail address ("" without one)
    [[nodiscard]] std::vector<std::string> dns_names() const;      // subjectAltName dNSName entries
    [[nodiscard]] std::vector<std::uint8_t> serial_number() const;  // big-endian, as encoded
    [[nodiscard]] std::vector<std::uint8_t> issuer() const;         // DER Name
    [[nodiscard]] std::vector<std::uint8_t> subject() const;        // DER Name
    [[nodiscard]] std::int64_t not_before_ns() const;               // since the Unix epoch
    [[nodiscard]] std::int64_t not_after_ns() const;
    [[nodiscard]] bool self_issued() const;                         // issuer and subject names match
    // The backend's certificate object (a PCCERT_CONTEXT on Windows), for tests.
    [[nodiscard]] void* native() const noexcept { return handle_; }

private:
    friend struct CertificateAccess;   // the backend's
    void* handle_{};
};

// The host's trusted root stores: roots the user added (those the system
// store lacks), enterprise / group-policy roots, and the machine's roots.
enum class RootStore : std::uint8_t { user, admin, system };
[[nodiscard]] Result<std::vector<Certificate>> root_certificates(RootStore store);

// A chain policy: basic X.509, or SSL (the server or client usage, and the
// host name when not empty).
struct ChainPolicy {
    bool ssl{};
    bool server{true};
    std::string hostname;
};
struct ChainRequest {
    Certificate leaf;
    std::vector<Certificate> intermediates;   // candidates for the chain
    std::vector<Certificate> anchors;         // the only trusted roots; empty: the host's roots
    std::vector<ChainPolicy> policies;        // every one must pass; empty: basic X.509
    std::optional<std::int64_t> time_ns;      // nullopt: now
    bool revocation{};                        // check revocation below the root
    bool network_fetch{true};                 // may fetch intermediates and revocation data
};
// Why a chain failed: the first failing policy, then revocation.
enum class ChainFailure : std::uint8_t { none, expired, name_mismatch, wrong_usage, revoked, revocation_unknown, bad_signature, untrusted };
struct ChainResult {
    std::vector<Certificate> chain;           // leaf to root, as far as it was built
    ChainFailure failure{ChainFailure::none};
};
[[nodiscard]] Result<ChainResult> evaluate_chain(const ChainRequest& request);

// A certificate with its private key, as TLS uses it. Move-only; a key the
// identity created or imported is deleted with it.
class Identity {
public:
    // A self-signed RSA-2048 serverAuth certificate for `common_name` and
    // `dns_names`, valid from a day ago for thirty days.
    [[nodiscard]] static Result<Identity> self_signed(std::string_view common_name, const std::vector<std::string>& dns_names);
    struct Imported {
        std::vector<Identity> identities;
        std::vector<Certificate> certificates;  // those without a private key
    };
    // A PKCS#12 blob: Error::invalid_argument when it is not one,
    // Error::access_denied for the wrong password.
    [[nodiscard]] static Result<Imported> import_pkcs12(ConstBytes data, std::string_view password);
    // Whether `data` looks like a PKCS#12 blob (without decrypting it).
    [[nodiscard]] static bool is_pkcs12(ConstBytes data);
    Identity() = default;
    Identity(Identity&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)), key_(std::move(other.key_)) {}
    Identity& operator=(Identity&& other) noexcept;
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;
    ~Identity();
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] Certificate certificate() const;
    // The backend's certificate with its key attached (a PCCERT_CONTEXT on Windows), for tests.
    [[nodiscard]] void* native() const noexcept { return handle_; }

private:
    void* handle_{};
    std::string key_;   // the persisted key to delete (empty: none)
};

// ---- TLS ----

// TLS sessions, client or server, over caller-supplied byte I/O: blocking
// sockets or non-blocking callbacks (every call can be resumed after
// would_block).
enum class TlsVersion : std::uint8_t { unknown, tls10, tls11, tls12, tls13 };
struct TlsIo {
    enum class Kind { ok, would_block, closed, error } kind{Kind::ok};
    std::size_t size{};
};
using TlsRead = std::function<TlsIo(std::uint8_t* buffer, std::size_t size)>;
using TlsWrite = std::function<TlsIo(const std::uint8_t* data, std::size_t size)>;

struct TlsOptions {
    bool server{};
    std::string peer_name;                 // client: SNI (never verified here: the caller judges the chain)
    const Identity* identity{};            // server (required) or client identity; must outlive the session
    std::vector<std::string> alpn;
    TlsVersion min_version{TlsVersion::tls12};
    TlsVersion max_version{TlsVersion::tls13};
    bool request_client_certificate{};
};

class TlsSession {
public:
    enum class Status { ok, would_block, closed, error };

    // Error::invalid_argument for inconsistent options; error_text says why the host refused.
    [[nodiscard]] static Result<TlsSession> create(const TlsOptions& options, std::string* error_text = nullptr);
    TlsSession();  // no session
    TlsSession(TlsSession&&) noexcept;
    TlsSession& operator=(TlsSession&&) noexcept;
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    ~TlsSession();

    // Runs the handshake until it completes (ok), needs I/O that would block,
    // or fails. Idempotent once complete.
    Status handshake(const TlsRead& read, const TlsWrite& write);
    // Up to `size` plaintext bytes; `got` 0 with closed at the peer's close_notify.
    Status read(const TlsRead& read, const TlsWrite& write, std::uint8_t* buffer, std::size_t size, std::size_t& got);
    // Encrypts all of `data` (accepted = size) and flushes; would_block when
    // encrypted bytes remain queued (a later write or flush sends them).
    Status write(const TlsWrite& write, const std::uint8_t* data, std::size_t size, std::size_t& accepted);
    Status flush(const TlsWrite& write);
    // Sends close_notify (best effort).
    Status shutdown(const TlsWrite& write);

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::size_t buffered() const noexcept;
    [[nodiscard]] std::vector<Certificate> peer_certificates() const;   // leaf first
    [[nodiscard]] std::string negotiated_alpn() const;
    [[nodiscard]] TlsVersion negotiated_version() const;
    [[nodiscard]] const std::string& error() const noexcept;
    // The peer ended the session with close_notify (not a bare transport close).
    [[nodiscard]] bool close_notified() const noexcept;

    struct State;

private:
    explicit TlsSession(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace alchemy::host::crypto
