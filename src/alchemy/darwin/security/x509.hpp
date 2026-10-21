#pragma once

// Host-only X.509 certificates and trust evaluation over host::crypto (the
// host's certificate parser and chain engine). No guest calls: usable from
// URL-loading I/O threads. Owner: v-netsec (src/alchemy/security).

#include "alchemy/host/crypto.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::security {

using Bytes = std::vector<std::uint8_t>;

// One parsed certificate (immutable).
class Certificate {
public:
    // nullptr when `der` is not a DER X.509 certificate.
    [[nodiscard]] static std::shared_ptr<const Certificate> parse(std::span<const std::uint8_t> der);
    // A certificate the host produced (a chain, a TLS peer, an identity).
    [[nodiscard]] static std::shared_ptr<const Certificate> from_host(const host::crypto::Certificate& certificate);
    Certificate(const Certificate&) = delete;
    Certificate& operator=(const Certificate&) = delete;

    [[nodiscard]] const Bytes& der() const noexcept { return der_; }
    [[nodiscard]] const host::crypto::Certificate& host() const noexcept { return host_; }
    [[nodiscard]] std::string subject_summary() const;      // CN, else the first name element
    [[nodiscard]] std::string common_name() const;          // "" when absent
    [[nodiscard]] std::vector<std::string> email_addresses() const;
    [[nodiscard]] std::vector<std::string> dns_names() const;   // subjectAltName dNSName entries
    [[nodiscard]] Bytes serial_number() const;              // big-endian, as encoded
    [[nodiscard]] Bytes issuer_sequence() const;            // DER Name
    [[nodiscard]] Bytes subject_sequence() const;
    [[nodiscard]] Bytes subject_public_key_info() const;    // DER SubjectPublicKeyInfo
    [[nodiscard]] double not_before() const;                // CFAbsoluteTime
    [[nodiscard]] double not_after() const;
    [[nodiscard]] Bytes sha256() const;
    [[nodiscard]] bool self_issued() const;

private:
    Certificate() = default;
    Bytes der_;
    host::crypto::Certificate host_;
};
using CertificatePtr = std::shared_ptr<const Certificate>;

// SecTrustResultType.
enum class TrustResult : std::uint32_t { invalid = 0, proceed = 1, deny = 3, unspecified = 4, recoverable = 5, fatal = 6, other = 7 };

// Security framework OSStatus values used by trust evaluation.
namespace status {
inline constexpr std::int32_t success = 0, unimplemented = -4, param = -50, allocate = -108, not_available = -25291,
    duplicate_item = -25299, item_not_found = -25300, interaction_not_allowed = -25308, decode = -26275,
    host_name_mismatch = -67602, invalid_extended_key_usage = -67609, incomplete_revocation = -67635,
    certificate_expired = -67818, certificate_not_yet_valid = -67819, certificate_revoked = -67820,
    not_trusted = -67843, invalid_key_ref = -67712, unsupported_algorithm = -67722, verify_failed = -67808,
    no_trust_settings = -25263, internal_component = -2070, cs_unimplemented = -67072,
    authorization_denied = -60005, authorization_tool_execute_failure = -60031, authorization_invalid_set = -60001;
} // namespace status

struct TrustPolicy {
    enum class Kind { basic, ssl, revocation } kind{Kind::basic};
    bool server{true};          // SSL: evaluating a server certificate
    std::string hostname;       // SSL: the expected name ("" skips the name check)
    std::uint64_t revocation_flags{};
};

struct TrustRequest {
    std::vector<CertificatePtr> certificates;   // leaf first, then candidate intermediates
    std::vector<TrustPolicy> policies;          // empty: basic X.509
    std::vector<CertificatePtr> anchors;        // custom anchors (empty: system roots)
    bool anchors_only{true};                    // with custom anchors: ignore the system roots
    std::optional<double> verify_time;          // CFAbsoluteTime; nullopt = now
    bool network_fetch{true};
    Bytes exceptions;                           // SecTrustSetExceptions data
};

struct TrustOutcome {
    bool trusted{};
    TrustResult result{TrustResult::invalid};
    std::int32_t status{status::not_trusted};   // OSStatus of the failure (0 when trusted)
    std::string description;                    // Apple-style message of the failure
    std::vector<CertificatePtr> chain;          // leaf to anchor (as far as it was built)
};

// Evaluates like SecTrustEvaluateWithError: the host's chain engine builds
// the chain (system roots or the custom anchors), then the SSL / basic
// policies check names, validity and usage.
[[nodiscard]] TrustOutcome evaluate_trust(const TrustRequest& request);
// SecTrustCopyExceptions data for a chain whose leaf is `leaf`.
[[nodiscard]] Bytes trust_exceptions(const Certificate& leaf);

// The certificates of a host root store, SecTrustSettings domain order:
// 0 user (roots the user added), 1 admin (enterprise / group-policy roots),
// 2 system (machine roots).
[[nodiscard]] std::vector<CertificatePtr> root_certificates(int domain);

// A self-signed RSA-2048 server certificate whose private key the host keeps
// and deletes with the object (host::crypto::Identity). For in-process TLS
// servers (SecureTransport server mode tests and the URL-loading tests'
// local HTTPS server).
class SelfSignedIdentity {
public:
    [[nodiscard]] static std::unique_ptr<SelfSignedIdentity> create(const std::string& common_name,
                                                                   const std::vector<std::string>& dns_names);
    SelfSignedIdentity(const SelfSignedIdentity&) = delete;
    SelfSignedIdentity& operator=(const SelfSignedIdentity&) = delete;
    // The identity a TLS server presents (TlsOptions::certificate).
    [[nodiscard]] const host::crypto::Identity* context() const noexcept { return &identity_; }
    [[nodiscard]] CertificatePtr certificate() const;
private:
    SelfSignedIdentity() = default;
    host::crypto::Identity identity_;
};

} // namespace alchemy::gui::security
