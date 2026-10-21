#include "alchemy/darwin/security/x509.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/darwin/security/keys.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>

// X.509 certificates and trust evaluation over the host's chain engine
// (host::crypto). Behaviour follows Apple's public SecTrust / SecCertificate
// documentation; no Apple code is used.
namespace alchemy::gui::security {
namespace {

constexpr double seconds_1970_to_2001 = 978307200.0;

// Nanoseconds since the Unix epoch <-> CFAbsoluteTime.
double absolute(std::int64_t ns) { return static_cast<double>(ns) / 1e9 - seconds_1970_to_2001; }
std::int64_t unix_ns(double absolute_time) { return static_cast<std::int64_t>((absolute_time + seconds_1970_to_2001) * 1e9); }

// The children of TBSCertificate.
std::optional<std::vector<der::Node>> tbs_fields(const Bytes& certificate) {
    const auto outer = der::read(certificate);
    if (!outer || outer->tag != 0x30) return std::nullopt;
    const auto parts = der::children(*outer);
    if (!parts || parts->empty() || (*parts)[0].tag != 0x30) return std::nullopt;
    return der::children((*parts)[0]);
}

std::string cn_of(const Certificate& certificate) {
    auto name = certificate.common_name();
    return name.empty() ? certificate.subject_summary() : name;
}

} // namespace

// ---- Certificate ---------------------------------------------------------------------------

std::shared_ptr<const Certificate> Certificate::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > (1U << 24)) return nullptr;
    // Exactly one DER element (trailing garbage is not a certificate).
    const auto outer = der::read(bytes);
    if (!outer || outer->tag != 0x30 || outer->whole.size() != bytes.size()) return nullptr;
    auto host = host::crypto::Certificate::parse(std::as_bytes(bytes));
    if (!host) return nullptr;
    std::shared_ptr<Certificate> result(new Certificate());
    result->der_.assign(bytes.begin(), bytes.end());
    result->host_ = std::move(*host);
    return result;
}

std::shared_ptr<const Certificate> Certificate::from_host(const host::crypto::Certificate& certificate) {
    if (!certificate) return nullptr;
    std::shared_ptr<Certificate> result(new Certificate());
    result->der_ = certificate.der();
    result->host_ = certificate;
    return result;
}

std::string Certificate::common_name() const { return host_.common_name(); }

std::string Certificate::subject_summary() const {
    auto name = common_name();
    if (!name.empty()) return name;
    return host_.display_name();
}

std::vector<std::string> Certificate::email_addresses() const {
    std::vector<std::string> result;
    auto email = host_.email();
    if (!email.empty()) result.push_back(std::move(email));
    return result;
}

std::vector<std::string> Certificate::dns_names() const { return host_.dns_names(); }
Bytes Certificate::serial_number() const { return host_.serial_number(); }
Bytes Certificate::issuer_sequence() const { return host_.issuer(); }
Bytes Certificate::subject_sequence() const { return host_.subject(); }

Bytes Certificate::subject_public_key_info() const {
    const auto fields = tbs_fields(der_);
    if (!fields) return {};
    // [0] version (optional), serial, signature, issuer, validity, subject, spki.
    const std::size_t offset = !fields->empty() && (*fields)[0].tag == 0xa0 ? 1 : 0;
    if (fields->size() <= 5 + offset) return {};
    const auto& spki = (*fields)[5 + offset];
    return Bytes(spki.whole.begin(), spki.whole.end());
}

double Certificate::not_before() const { return absolute(host_.not_before_ns()); }
double Certificate::not_after() const { return absolute(host_.not_after_ns()); }

Bytes Certificate::sha256() const {
    auto value = digest("SHA256", der_);
    if (!value) fail("SHA-256 failed");
    return *value;
}

bool Certificate::self_issued() const { return host_.self_issued(); }

// ---- trust evaluation ---------------------------------------------------------------------------

Bytes trust_exceptions(const Certificate& leaf) {
    // "AlchemyTrustException" + SHA-256 of the leaf (opaque to callers, as on macOS).
    constexpr std::string_view tag = "AlchemyTrustException1";
    Bytes result(tag.begin(), tag.end());
    const auto digest = leaf.sha256();
    result.insert(result.end(), digest.begin(), digest.end());
    return result;
}

TrustOutcome evaluate_trust(const TrustRequest& request) {
    TrustOutcome outcome;
    if (request.certificates.empty() || !request.certificates[0]) {
        outcome.result = TrustResult::invalid;
        outcome.status = status::param;
        outcome.description = "No certificates to evaluate";
        return outcome;
    }
    const auto& leaf = *request.certificates[0];
    host::crypto::ChainRequest chain;
    chain.leaf = leaf.host();
    for (std::size_t index = 1; index < request.certificates.size(); ++index)
        if (request.certificates[index]) chain.intermediates.push_back(request.certificates[index]->host());
    if (!request.anchors.empty()) {
        for (const auto& anchor : request.anchors) if (anchor) chain.anchors.push_back(anchor->host());
        if (!request.anchors_only) {
            for (const auto& system : root_certificates(2)) chain.anchors.push_back(system->host());
            for (const auto& user : root_certificates(0)) chain.anchors.push_back(user->host());
        }
    }
    std::vector<TrustPolicy> policies = request.policies;
    if (policies.empty()) policies.push_back({});
    for (const auto& policy : policies) {
        if (policy.kind == TrustPolicy::Kind::revocation) chain.revocation = true;
        if (policy.kind == TrustPolicy::Kind::ssl) chain.policies.push_back({true, policy.server, policy.hostname});
        else chain.policies.push_back({});   // basic X.509 (a revocation policy checks the chain like the basic one)
    }
    chain.network_fetch = request.network_fetch;
    if (request.verify_time) chain.time_ns = unix_ns(*request.verify_time);

    const auto evaluated = host::crypto::evaluate_chain(chain);
    if (!evaluated) fail("the certificate chain could not be evaluated (" + std::string(host::name(evaluated.error())) + ")");
    for (const auto& element : evaluated->chain)
        if (auto certificate = Certificate::from_host(element)) outcome.chain.push_back(std::move(certificate));

    using host::crypto::ChainFailure;
    const auto name = cn_of(leaf);
    if (evaluated->failure == ChainFailure::none) {
        outcome.trusted = true;
        outcome.result = TrustResult::unspecified;
        outcome.status = status::success;
        return outcome;
    }
    outcome.result = TrustResult::recoverable;
    switch (evaluated->failure) {
    case ChainFailure::expired: {
        const auto now = request.verify_time.value_or(absolute(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
        if (now < leaf.not_before()) {
            outcome.status = status::certificate_not_yet_valid;
            outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate is not yet valid";
        } else {
            outcome.status = status::certificate_expired;
            outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate is expired";
        }
        break;
    }
    case ChainFailure::name_mismatch:
        outcome.status = status::host_name_mismatch;
        outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate name does not match input";
        break;
    case ChainFailure::wrong_usage:
        outcome.status = status::invalid_extended_key_usage;
        outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate is not permitted for this usage";
        break;
    case ChainFailure::revoked:
        outcome.status = status::certificate_revoked;
        outcome.result = TrustResult::fatal;
        outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate is revoked";
        break;
    case ChainFailure::revocation_unknown:
        outcome.status = status::incomplete_revocation;
        outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate revocation status could not be determined";
        break;
    case ChainFailure::bad_signature:
        outcome.status = status::not_trusted;
        outcome.result = TrustResult::fatal;
        outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate has an invalid signature";
        break;
    default:
        outcome.status = status::not_trusted;
        outcome.description = "\xE2\x80\x9C" + name + "\xE2\x80\x9D certificate is not trusted";
        break;
    }
    // User exceptions (SecTrustSetExceptions) accept a recoverable failure for
    // the exact leaf they were copied from.
    if (outcome.result == TrustResult::recoverable && !request.exceptions.empty() && request.exceptions == trust_exceptions(leaf)) {
        outcome.trusted = true;
        outcome.result = TrustResult::proceed;
        outcome.status = status::success;
        outcome.description.clear();
    }
    return outcome;
}

std::vector<CertificatePtr> root_certificates(int domain) {
    using host::crypto::RootStore;
    std::vector<CertificatePtr> result;
    if (domain < 0 || domain > 2) return result;
    const auto store = domain == 0 ? RootStore::user : domain == 1 ? RootStore::admin : RootStore::system;
    const auto roots = host::crypto::root_certificates(store);
    if (!roots) return result;
    for (const auto& root : *roots)
        if (auto certificate = Certificate::from_host(root)) result.push_back(std::move(certificate));
    return result;
}

// ---- self-signed identities -------------------------------------------------------------------------

std::unique_ptr<SelfSignedIdentity> SelfSignedIdentity::create(const std::string& common_name, const std::vector<std::string>& dns_names) {
    auto identity = host::crypto::Identity::self_signed(common_name, dns_names);
    if (!identity) fail("creating the self-signed TLS identity failed (" + std::string(host::name(identity.error())) + ")");
    std::unique_ptr<SelfSignedIdentity> result(new SelfSignedIdentity());
    result->identity_ = std::move(*identity);
    return result;
}

CertificatePtr SelfSignedIdentity::certificate() const { return Certificate::from_host(identity_.certificate()); }

} // namespace alchemy::gui::security
