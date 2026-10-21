#pragma once

// HTTP(S) exchanges over the host's HTTP stack (host::http: synchronous
// requests driven from a guest dispatch worker). Server certificates are
// never judged by the stack: it hands the server's chain to the exchange's
// TrustDecision after the TLS handshake, before any request byte is sent, so
// trust follows SecTrust evaluation (and NSURLSession server-trust
// challenges). A rejected connection is closed before the request is
// written. Redirects are not followed by the stack (URL loading follows them
// itself).

#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/host/http.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <variant>

namespace alchemy::gui::network {

using Headers = host::http::Headers;
[[nodiscard]] std::optional<std::string> header_value(const Headers& headers, std::string_view name);

struct ProxyConfiguration {
    enum class Kind { none, named, automatic } kind{Kind::none};
    std::string proxy, bypass;                // named: host::http proxy and bypass lists
};
// The user's proxy settings (SystemConfiguration's view) as a session configuration.
[[nodiscard]] ProxyConfiguration system_proxy();

// One HTTP session (connection pool).
class HttpClient {
public:
    [[nodiscard]] static std::shared_ptr<HttpClient> create(const std::string& user_agent, const ProxyConfiguration& proxy);
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    [[nodiscard]] const host::http::Session& session() const noexcept { return session_; }
private:
    HttpClient() = default;
    host::http::Session session_;
};

struct HttpRequest {
    UrlParts url;
    std::string method{"GET"};
    Headers headers;
    Bytes body;
    double timeout{60};
    bool cookies{true};
};

struct HttpResponseHead {
    int status{};
    std::string version;                      // "HTTP/1.1", "HTTP/2"
    Headers headers;
};

struct HttpFailure {
    std::int64_t code{url_error::unknown};    // NSURLError
    std::string description;
    bool trust{};                             // the server certificate was rejected
};

// Called on the exchange's thread with the server's chain (leaf first).
// Returns true to continue the connection.
using TrustDecision = std::function<bool(const std::vector<security::CertificatePtr>& chain)>;

enum class AuthTarget { server, proxy };
struct AuthChallenge {
    AuthTarget target{AuthTarget::server};
    std::vector<host::http::Scheme> schemes;  // offered, preferred first
    std::string realm;
};

class HttpExchange {
public:
    HttpExchange(std::shared_ptr<HttpClient> client, HttpRequest request, TrustDecision trust);
    ~HttpExchange();
    HttpExchange(const HttpExchange&) = delete;
    HttpExchange& operator=(const HttpExchange&) = delete;

    // Sends the request (again after set_credentials) and receives the response head.
    std::variant<HttpResponseHead, HttpFailure> send();
    // Body bytes; 0 at the end. On failure returns 0 and sets `failure`.
    std::size_t read(std::uint8_t* buffer, std::size_t size, std::optional<HttpFailure>& failure);
    // The authentication a 401 / 407 response asks for.
    [[nodiscard]] std::optional<AuthChallenge> challenge(int status);
    bool set_credentials(AuthTarget target, host::http::Scheme scheme, const std::string& user, const std::string& password);
    // Any thread: aborts a blocked call; later calls fail with NSURLErrorCancelled.
    void cancel();
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return sent_; }

private:
    HttpFailure failure_from(const host::http::Failure& cause);
    host::http::Request* request();
    host::http::Request* open();

    std::shared_ptr<HttpClient> client_;
    HttpRequest request_;
    TrustDecision trust_;
    std::mutex mutex_;
    std::optional<host::http::Request> host_;   // opened by the first send()
    bool closed_{};
    std::atomic<bool> cancelled_{};
    std::uint64_t sent_{};
};

} // namespace alchemy::gui::network
