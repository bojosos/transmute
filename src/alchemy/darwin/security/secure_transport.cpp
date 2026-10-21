#include "alchemy/darwin/security/security_internal.hpp"

#include "alchemy/darwin/security/tls.hpp"

#include <algorithm>

// SecureTransport (SSLContext) over the host's TLS (host::crypto, SChannel
// on Windows), SecIdentity and PKCS#12 import. The SSL* calls drive a TlsSession through the guest's SSLReadFunc
// / SSLWriteFunc, so non-blocking sockets (GCDAsyncSocket) see
// errSSLWouldBlock exactly where SecureTransport reports it. Server
// certificates are verified like SecureTransport does by default (SSL
// policy with the peer domain name) unless the guest breaks on server
// authentication and evaluates SSLCopyPeerTrust itself.
//
// Not provided (explicit errSecUnimplemented): cipher-suite selection
// (SSLSetEnabledCiphers), custom Diffie-Hellman parameters, datagram (DTLS)
// contexts, breaking on certificate requests, and SecIdentityCopyPrivateKey
// for imported identities (their keys stay inside the host).
namespace alchemy::gui::security {
namespace {

constexpr std::string_view lib = security_library;
constexpr std::int32_t ssl_protocol = -9800, ssl_would_block = -9803, ssl_closed_graceful = -9805, ssl_closed_abort = -9806,
    ssl_cert_chain_invalid = -9807, ssl_cert_expired = -9814, ssl_cert_not_yet_valid = -9815, ssl_closed_no_notify = -9816,
    ssl_peer_auth_completed = -9841, ssl_host_name_mismatch = -9843, ssl_bad_configuration = -9848, auth_failed = -25293;
constexpr std::size_t scratch_size = 16384;

enum class SessionState : std::uint32_t { idle = 0, handshake = 1, connected = 2, closed = 3, aborted = 4 };

struct SslState final : ObjectPayload {
    std::recursive_mutex mutex;
    bool server{};
    Address connection{}, read_function{}, write_function{};
    Address identity{};                       // +1 SecIdentity
    Address scratch{}, length_cell{};         // guest I/O buffers (owned)
    std::string peer_name;
    bool break_on_server_auth{}, break_on_client_auth{};
    std::uint32_t client_auth{};              // SSLAuthenticate
    TlsVersion min_version{TlsVersion::tls10}, max_version{TlsVersion::tls13};
    std::vector<std::string> alpn;
    std::unique_ptr<TlsSession> session;
    SessionState state{SessionState::idle};
    bool auth_reported{}, auth_done{};
};

TlsVersion version_of(std::uint64_t protocol, bool minimum) {
    switch (protocol) {
    case 4: case 5: case 6: return TlsVersion::tls10;   // kTLSProtocol1, kTLSProtocol1Only, kSSLProtocolAll
    case 2: case 3: return minimum ? TlsVersion::tls10 : TlsVersion::unknown;   // SSL 3 is never negotiated
    case 7: return TlsVersion::tls11;
    case 8: return TlsVersion::tls12;
    case 10: case 999: return TlsVersion::tls13;
    default: return TlsVersion::unknown;
    }
}

Address status_value(std::int32_t value) { return static_cast<Address>(static_cast<std::int64_t>(value)); }

struct Transport {
    Security& sec;
    Bridge& b;
    Address context_class{};

    std::shared_ptr<SslState> state(Address object) {
        const auto value = object ? b.objc.find_host_state<SslState>(object) : nullptr;
        if (!value) fail("expected an SSLContextRef");
        return value;
    }

    TlsRead reader(SslState& st) {
        return [this, &st](std::uint8_t* buffer, std::size_t size) -> IoResult {
            auto& m = b.memory();
            size = std::min(size, scratch_size);
            m.write64(st.length_cell, size);
            const std::array<Address, 3> arguments{st.connection, st.scratch, st.length_cell};
            const auto status = static_cast<std::int32_t>(b.call_address(st.read_function, arguments));
            const auto count = std::min<std::size_t>(static_cast<std::size_t>(m.read64(st.length_cell)), size);
            if (count) m.read_bytes(st.scratch, std::span(buffer, count));
            if (status == 0 || status == ssl_would_block) return count ? IoResult{IoResult::Kind::ok, count} : IoResult{IoResult::Kind::would_block, 0};
            if (status == ssl_closed_graceful || status == ssl_closed_abort || status == ssl_closed_no_notify)
                return count ? IoResult{IoResult::Kind::ok, count} : IoResult{IoResult::Kind::closed, 0};
            return {IoResult::Kind::error, 0};
        };
    }
    TlsWrite writer(SslState& st) {
        return [this, &st](const std::uint8_t* data, std::size_t size) -> IoResult {
            auto& m = b.memory();
            size = std::min(size, scratch_size);
            m.write_bytes(st.scratch, std::span(data, size));
            m.write64(st.length_cell, size);
            const std::array<Address, 3> arguments{st.connection, st.scratch, st.length_cell};
            const auto status = static_cast<std::int32_t>(b.call_address(st.write_function, arguments));
            const auto count = std::min<std::size_t>(static_cast<std::size_t>(m.read64(st.length_cell)), size);
            if (status == 0) return {IoResult::Kind::ok, count};
            if (status == ssl_would_block) return count ? IoResult{IoResult::Kind::ok, count} : IoResult{IoResult::Kind::would_block, 0};
            if (status == ssl_closed_graceful || status == ssl_closed_abort || status == ssl_closed_no_notify) return {IoResult::Kind::closed, 0};
            return {IoResult::Kind::error, 0};
        };
    }

    std::int32_t handshake(SslState& st) {
        if (st.state == SessionState::closed || st.state == SessionState::aborted) return ssl_closed_abort;
        if (!st.read_function || !st.write_function) return ssl_bad_configuration;
        if (!st.session) {
            TlsOptions options;
            options.server = st.server;
            options.peer_name = st.peer_name;
            options.alpn = st.alpn;
            options.min_version = st.min_version;
            options.max_version = st.max_version;
            options.request_client_certificate = st.server && st.client_auth != 0;
            if (st.identity) {
                const auto identity = b.objc.find_host_state<IdentityState>(st.identity);
                if (!identity || !identity->identity) return ssl_bad_configuration;
                options.certificate = &identity->identity;
            }
            if (st.server && !options.certificate) return ssl_bad_configuration;
            try { st.session = std::make_unique<TlsSession>(std::move(options)); }
            catch (const std::exception&) { st.state = SessionState::aborted; return ssl_bad_configuration; }
            st.state = SessionState::handshake;
        }
        const auto read = reader(st);
        const auto write = writer(st);
        if (!st.session->complete()) {
            switch (st.session->handshake(read, write)) {
            case TlsSession::Status::ok: break;
            case TlsSession::Status::would_block: return ssl_would_block;
            case TlsSession::Status::closed: st.state = SessionState::aborted; return ssl_closed_abort;
            default: st.state = SessionState::aborted; return ssl_protocol;
            }
        }
        if (!st.auth_done) {
            const bool peer_has_certificate = !st.session->peer_certificates().empty();
            const bool break_now = st.server ? st.break_on_client_auth && peer_has_certificate : st.break_on_server_auth;
            if (break_now) {
                if (!st.auth_reported) { st.auth_reported = true; return ssl_peer_auth_completed; }
            } else if (!st.server) {
                TrustRequest request;
                request.certificates = st.session->peer_certificates();
                request.policies.push_back({TrustPolicy::Kind::ssl, true, st.peer_name, 0});
                const auto outcome = evaluate_trust(request);
                if (!outcome.trusted) {
                    static_cast<void>(st.session->shutdown(write));
                    st.state = SessionState::aborted;
                    switch (outcome.status) {
                    case status::host_name_mismatch: return ssl_host_name_mismatch;
                    case status::certificate_expired: return ssl_cert_expired;
                    case status::certificate_not_yet_valid: return ssl_cert_not_yet_valid;
                    default: return ssl_cert_chain_invalid;
                    }
                }
            }
            st.auth_done = true;
        }
        if (st.session->flush(write) == TlsSession::Status::would_block) return ssl_would_block;
        st.state = SessionState::connected;
        return 0;
    }

    Address make_trust(SslState& st) {
        if (!st.session) return 0;
        const auto certificates = st.session->peer_certificates();
        if (certificates.empty()) return 0;
        return sec.make_trust(certificates, {{TrustPolicy::Kind::ssl, !st.server, st.server ? std::string{} : st.peer_name, 0}});
    }
};

} // namespace

std::int32_t import_pkcs12(Security& sec, std::span<const std::uint8_t> data, const std::string& passphrase,
                           std::vector<Address>& identities, std::vector<Address>& certificates) {
    auto imported = host::crypto::Identity::import_pkcs12(std::as_bytes(data), passphrase);
    if (!imported) return imported.error() == host::Error::access_denied ? auth_failed : status::decode;
    auto& b = *sec.b;
    for (auto& held : imported->identities) {
        const auto identity = b.objc.allocate(sec.identity_class);
        auto state = std::make_shared<IdentityState>();
        state->certificate = sec.make_certificate(Certificate::from_host(held.certificate()));
        state->identity = std::move(held);
        static_cast<void>(b.objc.host_state<IdentityState>(identity, [&] { return state; }));
        identities.push_back(identity);
    }
    std::vector<CertificatePtr> plain;
    for (const auto& certificate : imported->certificates)
        if (auto parsed = Certificate::from_host(certificate)) plain.push_back(std::move(parsed));
    for (const auto& certificate : plain) certificates.push_back(sec.make_certificate(certificate));
    return identities.empty() && certificates.empty() ? status::decode : status::success;
}

void install_secure_transport(Security& sec) {
    auto& b = *sec.b;
    auto self = std::make_shared<Transport>(Transport{sec, b});
    self->context_class = b.define_class("_AlchemySSLContext", b.object_class, lib);
    b.register_type_id(self->context_class, TypeId::ssl_context);

    // CFNetwork stream SSL settings keys (GCDAsyncSocket / CFStream TLS dictionaries).
    const std::pair<const char*, const char*> stream_keys[] = {
        {"kCFStreamSSLLevel", "kCFStreamSSLLevel"}, {"kCFStreamSSLPeerName", "kCFStreamSSLPeerName"},
        {"kCFStreamSSLCertificates", "kCFStreamSSLCertificates"}, {"kCFStreamSSLIsServer", "kCFStreamSSLIsServer"},
        {"kCFStreamSSLValidatesCertificateChain", "kCFStreamSSLValidatesCertificateChain"},
        {"kCFStreamSSLAllowsExpiredCertificates", "kCFStreamSSLAllowsExpiredCertificates"},
        {"kCFStreamSSLAllowsExpiredRoots", "kCFStreamSSLAllowsExpiredRoots"},
        {"kCFStreamSSLAllowsAnyRoot", "kCFStreamSSLAllowsAnyRoot"},
        {"kCFStreamSocketSecurityLevelNone", "kCFStreamSocketSecurityLevelNone"},
        {"kCFStreamSocketSecurityLevelSSLv2", "kCFStreamSocketSecurityLevelSSLv2"},
        {"kCFStreamSocketSecurityLevelSSLv3", "kCFStreamSocketSecurityLevelSSLv3"},
        {"kCFStreamSocketSecurityLevelTLSv1", "kCFStreamSocketSecurityLevelTLSv1"},
        {"kCFStreamSocketSecurityLevelNegotiatedSSL", "kCFStreamSocketSecurityLevelNegotiatedSSL"},
        {"kCFStreamPropertySSLSettings", "kCFStreamPropertySSLSettings"},
        {"kCFStreamPropertySSLPeerTrust", "kCFStreamPropertySSLPeerTrust"},
        {"kCFStreamPropertySocketSecurityLevel", "kCFStreamPropertySocketSecurityLevel"}};
    for (const auto& [name, value] : stream_keys) sec.export_constant(name, value, cfnetwork_library);

    b.method(self->context_class, "dealloc", [self, cls = self->context_class](CpuState& s, GuestMemory&) {
        if (const auto state = self->b.objc.find_host_state<SslState>(s.x[0])) {
            std::lock_guard lock(state->mutex);
            self->b.release(std::exchange(state->identity, 0));
            if (state->scratch) self->b.process.release_guest(std::exchange(state->scratch, 0));
            if (state->length_cell) self->b.process.release_guest(std::exchange(state->length_cell, 0));
            state->session.reset();
        }
        static_cast<void>(self->b.objc.send_super(s.x[0], cls, self->b.objc.selector("dealloc")));
    }, false, "v@:");

    b.function(lib, "_SSLContextGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::ssl_context); });
    // SSLContextRef SSLCreateContext(CFAllocatorRef, SSLProtocolSide, SSLConnectionType)
    b.function(lib, "_SSLCreateContext", [self](CpuState& s, GuestMemory&) {
        if (s.x[2] != 0 || s.x[1] > 1) { s.x[0] = 0; return; }   // stream contexts only (no DTLS)
        const auto object = self->b.objc.allocate(self->context_class);
        auto state = std::make_shared<SslState>();
        state->server = s.x[1] == 0;
        state->scratch = self->b.process.allocate_guest(scratch_size);
        state->length_cell = self->b.process.allocate_guest(8);
        static_cast<void>(self->b.objc.host_state<SslState>(object, [&] { return state; }));
        s.x[0] = object;
    });
    const auto with = [self](auto body) {
        return [self, body](CpuState& s, GuestMemory& m) {
            const auto state = self->state(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = status_value(body(*state, s, m));
        };
    };
    const auto configurable = [](SslState& st) { return st.state == SessionState::idle; };
    b.function(lib, "_SSLSetIOFuncs", with([configurable](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t {
        if (!configurable(st)) return status::param;
        st.read_function = s.x[1];
        st.write_function = s.x[2];
        return 0;
    }));
    b.function(lib, "_SSLSetConnection", with([](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t { st.connection = s.x[1]; return 0; }));
    b.function(lib, "_SSLGetConnection", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write64(s.x[1], st.connection);
        return 0;
    }));
    b.function(lib, "_SSLSetPeerDomainName", with([self, configurable](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!configurable(st) || s.x[2] > 1024) return status::param;
        std::string name(static_cast<std::size_t>(s.x[2]), '\0');
        if (!name.empty()) m.read_bytes(s.x[1], std::span(reinterpret_cast<std::uint8_t*>(name.data()), name.size()));
        while (!name.empty() && name.back() == '\0') name.pop_back();
        st.peer_name = name;
        return 0;
    }));
    b.function(lib, "_SSLGetPeerDomainNameLength", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write64(s.x[1], st.peer_name.size());
        return 0;
    }));
    b.function(lib, "_SSLGetPeerDomainName", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[2] || m.read64(s.x[2]) < st.peer_name.size()) return status::param;
        m.write_bytes(s.x[1], std::span(reinterpret_cast<const std::uint8_t*>(st.peer_name.data()), st.peer_name.size()));
        m.write64(s.x[2], st.peer_name.size());
        return 0;
    }));
    b.function(lib, "_SSLSetSessionOption", with([](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t {
        const bool value = (s.x[2] & 0xff) != 0;
        switch (s.x[1]) {
        case 0: st.break_on_server_auth = value; return 0;   // kSSLSessionOptionBreakOnServerAuth
        case 2: st.break_on_client_auth = value; return 0;   // kSSLSessionOptionBreakOnClientAuth
        case 1: case 7: return value ? status::unimplemented : 0;   // break on certificate request / client hello
        // False start, one-byte records, renegotiation, session tickets: SChannel
        // chooses these itself; the option does not change what the guest sees.
        case 3: case 4: case 5: case 6: case 8: case 9: return 0;
        default: return status::param;
        }
    }));
    b.function(lib, "_SSLGetSessionOption", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[2]) return status::param;
        m.write8(s.x[2], s.x[1] == 0 ? st.break_on_server_auth : s.x[1] == 2 ? st.break_on_client_auth : 0);
        return 0;
    }));
    // The peer ID keys SecureTransport's session cache; SChannel resumes
    // sessions by target name on its own.
    b.function(lib, "_SSLSetPeerID", with([](SslState&, CpuState&, GuestMemory&) -> std::int32_t { return 0; }));
    b.function(lib, "_SSLSetProtocolVersionMin", with([configurable](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t {
        const auto version = version_of(s.x[1], true);
        if (!configurable(st) || version == TlsVersion::unknown) return status::param;
        st.min_version = version;
        return 0;
    }));
    b.function(lib, "_SSLSetProtocolVersionMax", with([configurable](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t {
        const auto version = version_of(s.x[1], false);
        if (!configurable(st) || version == TlsVersion::unknown) return status::param;
        st.max_version = version;
        return 0;
    }));
    b.function(lib, "_SSLGetProtocolVersionMin", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write32(s.x[1], static_cast<std::uint32_t>(st.min_version));
        return 0;
    }));
    b.function(lib, "_SSLGetProtocolVersionMax", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write32(s.x[1], static_cast<std::uint32_t>(st.max_version));
        return 0;
    }));
    b.function(lib, "_SSLGetNegotiatedProtocolVersion", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write32(s.x[1], st.session ? static_cast<std::uint32_t>(st.session->negotiated_version()) : 0);
        return 0;
    }));
    b.function(lib, "_SSLSetClientSideAuthenticate", with([](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t {
        if (!st.server || s.x[1] > 2) return status::param;
        st.client_auth = static_cast<std::uint32_t>(s.x[1]);
        return 0;
    }));
    b.function(lib, "_SSLSetCertificate", with([self, configurable](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t {
        if (!configurable(st)) return status::param;
        const auto values = s.x[1] ? self->b.cf.array_values(s.x[1]) : std::vector<Address>{};
        if (values.empty()) { self->b.release(std::exchange(st.identity, 0)); return 0; }
        if (!self->b.objc.find_host_state<IdentityState>(values[0])) return status::param;
        // Further entries are intermediates; SChannel sends the chain it builds itself.
        self->b.release(std::exchange(st.identity, self->b.retain(values[0])));
        return 0;
    }));
    b.function(lib, "_SSLSetALPNProtocols", with([self, configurable](SslState& st, CpuState& s, GuestMemory&) -> std::int32_t {
        if (!configurable(st)) return status::param;
        st.alpn.clear();
        for (const auto value : s.x[1] ? self->b.cf.array_values(s.x[1]) : std::vector<Address>{}) st.alpn.push_back(self->b.text(value));
        return 0;
    }));
    b.function(lib, "_SSLCopyALPNProtocols", with([self](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        const auto chosen = st.session ? st.session->negotiated_alpn() : std::string{};
        std::vector<Address> values;
        if (!chosen.empty()) values.push_back(self->b.string(chosen));
        m.write64(s.x[1], self->b.array(values));
        self->b.release_all(values);
        return 0;
    }));
    for (const auto* name : {"_SSLSetEnabledCiphers", "_SSLSetDiffieHellmanParams"})
        b.function(lib, name, with([](SslState&, CpuState&, GuestMemory&) -> std::int32_t { return status::unimplemented; }));
    b.function(lib, "_SSLHandshake", with([self](SslState& st, CpuState&, GuestMemory&) { return self->handshake(st); }));
    b.function(lib, "_SSLGetSessionState", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write32(s.x[1], static_cast<std::uint32_t>(st.state));
        return 0;
    }));
    b.function(lib, "_SSLCopyPeerTrust", with([self](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write64(s.x[1], self->make_trust(st));
        return 0;
    }));
    b.function(lib, "_SSLGetBufferedReadSize", with([](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (!s.x[1]) return status::param;
        m.write64(s.x[1], st.session ? st.session->buffered() : 0);
        return 0;
    }));
    // OSStatus SSLRead(SSLContextRef, void *data, size_t dataLength, size_t *processed)
    b.function(lib, "_SSLRead", with([self](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (s.x[3]) m.write64(s.x[3], 0);
        if (st.state != SessionState::connected) {
            if (st.state == SessionState::closed) return ssl_closed_graceful;
            if (const auto status = self->handshake(st); status != 0) return status;
        }
        const auto read = self->reader(st);
        const auto write = self->writer(st);
        const auto wanted = static_cast<std::size_t>(std::min<Address>(s.x[2], 1U << 20));
        Bytes buffer(wanted);
        std::size_t total = 0;
        std::int32_t result = 0;
        while (total < wanted) {
            std::size_t got = 0;
            const auto status = st.session->read(read, write, buffer.data() + total, wanted - total, got);
            total += got;
            if (status == TlsSession::Status::ok) {
                if (!st.session->buffered()) break;   // never block for more once data arrived
                continue;
            }
            if (status == TlsSession::Status::would_block) { result = total ? 0 : ssl_would_block; break; }
            if (status == TlsSession::Status::closed) {
                if (!total) { st.state = SessionState::closed; result = st.session->close_notified() ? ssl_closed_graceful : ssl_closed_no_notify; }
                break;
            }
            st.state = SessionState::aborted;
            result = total ? 0 : ssl_protocol;
            break;
        }
        if (total) m.write_bytes(s.x[1], std::span(buffer.data(), total));
        if (s.x[3]) m.write64(s.x[3], total);
        return result;
    }));
    // OSStatus SSLWrite(SSLContextRef, const void *data, size_t dataLength, size_t *processed)
    b.function(lib, "_SSLWrite", with([self](SslState& st, CpuState& s, GuestMemory& m) -> std::int32_t {
        if (s.x[3]) m.write64(s.x[3], 0);
        if (st.state != SessionState::connected) {
            if (st.state == SessionState::closed || st.state == SessionState::aborted) return ssl_closed_abort;
            if (const auto status = self->handshake(st); status != 0) return status;
        }
        const auto write = self->writer(st);
        if (s.x[2] > (1ULL << 30)) return status::param;
        Bytes data(static_cast<std::size_t>(s.x[2]));
        if (!data.empty()) m.read_bytes(s.x[1], data);
        std::size_t accepted = 0;
        const auto status = data.empty() ? st.session->flush(write) : st.session->write(write, data.data(), data.size(), accepted);
        if (s.x[3]) m.write64(s.x[3], accepted);
        if (status == TlsSession::Status::ok) return 0;
        if (status == TlsSession::Status::would_block) return ssl_would_block;
        st.state = SessionState::aborted;
        return ssl_closed_abort;
    }));
    b.function(lib, "_SSLClose", with([self](SslState& st, CpuState&, GuestMemory&) -> std::int32_t {
        if (st.session && st.state == SessionState::connected) static_cast<void>(st.session->shutdown(self->writer(st)));
        st.state = SessionState::closed;
        return 0;
    }));

    // ---- SecIdentity ----------------------------------------------------------------------------------
    b.function(lib, "_SecIdentityGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::identity); });
    b.function(lib, "_SecIdentityCopyCertificate", [self](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0] ? self->b.objc.find_host_state<IdentityState>(s.x[0]) : nullptr;
        if (!state || !s.x[1]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[1], self->b.retain(state->certificate));
        s.x[0] = 0;
    });
    b.function(lib, "_SecIdentityCopyPrivateKey", [](CpuState& s, GuestMemory&) { s.x[0] = status_value(status::unimplemented); });
    // Private keys are never stored in the keychain, so no certificate has an identity there.
    b.function(lib, "_SecIdentityCreateWithCertificate", [](CpuState& s, GuestMemory&) { s.x[0] = status_value(status::item_not_found); });
    // OSStatus SecPKCS12Import(CFDataRef, CFDictionaryRef options, CFArrayRef *items)
    b.function(lib, "_SecPKCS12Import", [self](CpuState& s, GuestMemory& m) {
        if (!self->b.is_data(s.x[0]) || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        std::string passphrase;
        if (const auto value = self->sec.get(s.x[1], "kSecImportExportPassphrase")) passphrase = self->b.text(value);
        std::vector<Address> identities, certificates;
        const auto result = import_pkcs12(self->sec, self->b.bytes(s.x[0]), passphrase, identities, certificates);
        if (result) { s.x[0] = status_value(result); return; }
        std::vector<Address> entries;
        for (const auto identity : identities) {
            const auto state = self->b.objc.find_host_state<IdentityState>(identity);
            std::vector<Address> chain{state->certificate};
            chain.insert(chain.end(), certificates.begin(), certificates.end());
            const auto chain_array = self->b.array(chain);
            std::vector<CertificatePtr> hosts;
            for (const auto certificate : chain) hosts.push_back(self->sec.certificate_of(certificate));
            const auto trust = self->sec.make_trust(hosts, {{}});
            entries.push_back(self->b.dictionary({{self->sec.key("kSecImportItemIdentity"), identity},
                                                  {self->sec.key("kSecImportItemCertChain"), chain_array},
                                                  {self->sec.key("kSecImportItemTrust"), trust}}));
            self->b.release_all({chain_array, trust});
        }
        m.write64(s.x[2], self->b.array(entries));
        self->b.release_all(entries);
        self->b.release_all(identities);
        self->b.release_all(certificates);
        s.x[0] = 0;
    });
}

} // namespace alchemy::gui::security
