#pragma once

// Security framework provider internals (owner: v-netsec). Not a contract
// header: the URL-loading provider (src/alchemy/network) uses the trust
// helpers below; everything else reaches Security through its exports.
//
// Model. SecCertificate, SecPolicy, SecTrust, SecKey, SecIdentity, SecCode,
// SecRequirement and SSLContext objects are instances of host classes
// (_AlchemySec*: NSObject) so CFRetain/CFRelease and -retain/-release work on
// them; CFGetTypeID answers their own type ids (Bridge::register_type_id).
// Their state is an ObjectPayload; references to other guest objects are +1
// and released by -dealloc.

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/darwin/security/x509.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::security {

void install_errors(const std::shared_ptr<Bridge>& b);

struct KeyMaterial;   // keys.cpp

struct CertificateState final : ObjectPayload { CertificatePtr certificate; };
struct PolicyState final : ObjectPayload { TrustPolicy policy; };
struct KeyState final : ObjectPayload { std::shared_ptr<const KeyMaterial> key; };
struct IdentityState final : ObjectPayload {
    Address certificate{};                    // +1 SecCertificate
    host::crypto::Identity identity;          // the certificate with its private key (TLS); an imported key dies with it
};
struct TrustState final : ObjectPayload {
    std::mutex mutex;
    std::vector<Address> certificates, policies, anchors;   // +1 guest objects
    std::vector<Address> chain;                             // +1, the evaluated chain (SecTrustGetCertificateAtIndex)
    TrustRequest request;
    std::optional<TrustOutcome> outcome;
};

class Security {
public:
    explicit Security(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)) {}
    std::shared_ptr<Bridge> b;
    Address certificate_class{}, policy_class{}, trust_class{}, key_class{}, identity_class{};

    // Interned constant strings by exported name without the leading
    // underscore ("kSecAttrKeyType"). `export_constant` exports
    // `_<name>` from Security (or `library`) once.
    Address export_constant(std::string_view name, std::string_view value, std::string_view library = security_library);
    [[nodiscard]] Address key(std::string_view name) const;     // fails when not exported
    [[nodiscard]] std::optional<std::string> key_name(Address value) const;   // reverse lookup by CFEqual
    // CFDictionaryGetValue(dictionary, key(name)).
    [[nodiscard]] Address get(Address dictionary, std::string_view name);
    [[nodiscard]] bool get_bool(Address dictionary, std::string_view name, bool fallback = false);
    // True when `value` is the constant `name` (by pointer or CFEqual).
    [[nodiscard]] bool is(Address value, std::string_view name);

    // Objects (+1 results).
    Address make_certificate(const CertificatePtr& certificate);
    [[nodiscard]] CertificatePtr certificate_of(Address object) const;   // nullptr when not a SecCertificate
    Address make_policy(const TrustPolicy& policy);
    [[nodiscard]] std::optional<TrustPolicy> policy_of(Address object) const;
    // A SecTrust over host certificates (URL loading / SecureTransport peers).
    Address make_trust(const std::vector<CertificatePtr>& certificates, const std::vector<TrustPolicy>& policies);
    // Evaluates (cached until the trust changes).
    TrustOutcome evaluate(Address trust);
    [[nodiscard]] std::shared_ptr<TrustState> trust_of(Address object) const;   // nullptr when not a SecTrust
    // The SecTrust's evaluation inputs, with any SecTrustSetExceptions data.
    [[nodiscard]] TrustRequest trust_request(Address trust) const;

    Address make_key(std::shared_ptr<const KeyMaterial> key);
    [[nodiscard]] std::shared_ptr<const KeyMaterial> key_of(Address object) const;

    // Writes OSStatus-domain CFError to a `CFErrorRef *`.
    void set_error(Address out, std::int32_t status, std::string_view description);

private:
    mutable std::mutex mutex_;
    std::map<std::string, Address, std::less<>> constants_;
    std::map<std::string, Address, std::less<>> by_value_;
};

[[nodiscard]] std::shared_ptr<Security> security();

void install_certificates(Security& security);
void install_keys(Security& security);
void install_keychain(Security& security);
void install_secure_transport(Security& security);
void install_code_signing(Security& security);
void install_common_crypto(Security& security);   // CommonCrypto digests / HMAC / random (libSystem)

// SecItemImport / SecPKCS12Import of a PKCS#12 blob: identities (+1
// SecIdentity) and the other certificates (+1 SecCertificate). OSStatus.
std::int32_t import_pkcs12(Security& security, std::span<const std::uint8_t> data, const std::string& passphrase,
                           std::vector<Address>& identities, std::vector<Address>& certificates);

// Security.framework's exports (security.cpp); install.cpp's install_security
// calls it, then GSS.
void install_security(InstallContext& context);

} // namespace alchemy::gui::security
