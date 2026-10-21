#pragma once

// Host TLS sessions (host::crypto::TlsSession: SChannel on Windows), client
// or server, driven through caller-supplied byte I/O so they work over
// blocking sockets (tests) and over SecureTransport's guest read/write
// callbacks (non-blocking: every operation is resumable after
// `would_block`). Host only; no guest calls. Owner: v-netsec
// (src/alchemy/security).

#include "alchemy/darwin/security/x509.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alchemy::gui::security {

using IoResult = host::crypto::TlsIo;
using TlsRead = host::crypto::TlsRead;
using TlsWrite = host::crypto::TlsWrite;

// SSLProtocol values (SecureTransport): kTLSProtocol1 = 4, kTLSProtocol11 = 7,
// kTLSProtocol12 = 8, kTLSProtocol13 = 10.
enum class TlsVersion : int { unknown = 0, tls10 = 4, tls11 = 7, tls12 = 8, tls13 = 10 };

struct TlsOptions {
    bool server{};
    std::string peer_name;                        // client: SNI and expected name
    const host::crypto::Identity* certificate{};  // server (required) or client identity; referenced, not owned
    std::vector<std::string> alpn;
    TlsVersion min_version{TlsVersion::tls12};
    TlsVersion max_version{TlsVersion::tls13};
    bool request_client_certificate{};
};

class TlsSession {
public:
    using Status = host::crypto::TlsSession::Status;

    // Fails (std::runtime_error) when the host refuses the options.
    explicit TlsSession(TlsOptions options);
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    // Runs the handshake until it completes (ok), needs I/O that would block,
    // or fails. Idempotent once complete.
    Status handshake(const TlsRead& read, const TlsWrite& write) { return session_.handshake(read, write); }
    // Up to `size` plaintext bytes; `got` 0 with closed at the peer's close_notify.
    Status read(const TlsRead& read, const TlsWrite& write, std::uint8_t* buffer, std::size_t size, std::size_t& got) {
        return session_.read(read, write, buffer, size, got);
    }
    // Encrypts all of `data` (accepted = size) and flushes; would_block when
    // encrypted bytes remain queued (a later write or flush sends them).
    Status write(const TlsWrite& write, const std::uint8_t* data, std::size_t size, std::size_t& accepted) {
        return session_.write(write, data, size, accepted);
    }
    Status flush(const TlsWrite& write) { return session_.flush(write); }
    // Sends close_notify (best effort).
    Status shutdown(const TlsWrite& write) { return session_.shutdown(write); }

    [[nodiscard]] bool complete() const noexcept { return session_.complete(); }
    [[nodiscard]] std::size_t buffered() const noexcept { return session_.buffered(); }
    [[nodiscard]] std::vector<CertificatePtr> peer_certificates() const;   // leaf first
    [[nodiscard]] std::string negotiated_alpn() const { return session_.negotiated_alpn(); }
    [[nodiscard]] TlsVersion negotiated_version() const;
    [[nodiscard]] const std::string& error() const noexcept { return session_.error(); }
    // The peer ended the session with close_notify (not a bare transport close).
    [[nodiscard]] bool close_notified() const noexcept { return session_.close_notified(); }

private:
    host::crypto::TlsSession session_;
};

} // namespace alchemy::gui::security
