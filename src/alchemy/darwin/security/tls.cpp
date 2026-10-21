#include "alchemy/darwin/security/tls.hpp"

#include "alchemy/darwin/security/bridge.hpp"

// TLS sessions over host::crypto::TlsSession; the SSLProtocol numbering and
// certificate objects are Security's.
namespace alchemy::gui::security {
namespace {

host::crypto::TlsVersion host_version(TlsVersion version) {
    using Host = host::crypto::TlsVersion;
    switch (version) {
    case TlsVersion::tls10: return Host::tls10;
    case TlsVersion::tls11: return Host::tls11;
    case TlsVersion::tls12: return Host::tls12;
    case TlsVersion::tls13: return Host::tls13;
    default: return Host::unknown;
    }
}

} // namespace

TlsSession::TlsSession(TlsOptions options) {
    host::crypto::TlsOptions settings;
    settings.server = options.server;
    settings.peer_name = std::move(options.peer_name);
    settings.identity = options.certificate;
    settings.alpn = std::move(options.alpn);
    settings.min_version = host_version(options.min_version);
    settings.max_version = host_version(options.max_version);
    settings.request_client_certificate = options.request_client_certificate;
    std::string why;
    auto session = host::crypto::TlsSession::create(settings, &why);
    if (!session) fail(why.empty() ? "the host refused the TLS session (" + std::string(host::name(session.error())) + ")" : why);
    session_ = std::move(*session);
}

std::vector<CertificatePtr> TlsSession::peer_certificates() const {
    std::vector<CertificatePtr> result;
    for (const auto& certificate : session_.peer_certificates())
        if (auto parsed = Certificate::from_host(certificate)) result.push_back(std::move(parsed));
    return result;
}

TlsVersion TlsSession::negotiated_version() const {
    using Host = host::crypto::TlsVersion;
    switch (session_.negotiated_version()) {
    case Host::tls10: return TlsVersion::tls10;
    case Host::tls11: return TlsVersion::tls11;
    case Host::tls12: return TlsVersion::tls12;
    case Host::tls13: return TlsVersion::tls13;
    default: return TlsVersion::unknown;
    }
}

} // namespace alchemy::gui::security
