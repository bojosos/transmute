#pragma once

// Host area "http" (owner: r2). The rules are in include/alchemy/host/host.hpp. The system HTTP
// stack (NSURLSession and CFNetwork, over WinHTTP today) and the system proxy configuration.
//
// Strings are UTF-8. A Request is synchronous: each call blocks its thread
// until the stack answers, and cancel() from any other thread ends a blocked
// call. Server certificates are never judged by the stack: the Request hands
// the server's chain to its Trust callback after the TLS handshake and before
// any request byte is written, and closes the connection when it answers
// false. Redirects are never followed, and the stack keeps no cookies and
// sends no credentials it was not given.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::host::http {

using Headers = std::vector<std::pair<std::string, std::string>>;

// ---- Proxies ----

// Proxy lists use one syntax: entries "[scheme=]host[:port]" separated by
// ';' or whitespace (an entry without a scheme serves every scheme). Bypass
// lists hold host patterns separated the same way; "<local>" stands for every
// host name without a dot.
struct UserProxy {
    std::string proxy, bypass;    // the manually set proxies
    std::string auto_config_url;  // a proxy auto-configuration (PAC) script
    bool auto_discovery{};        // WPAD
};
// The user's proxy settings. Error::unsupported when the host has none to report.
[[nodiscard]] Result<UserProxy> user_proxy();

// The proxies the PAC script at `script_url` picks for `target_url`, in the
// script's order (proxy list syntax; empty for DIRECT). Fetches the script
// directly, with `timeout_ms` for each step.
[[nodiscard]] Result<std::string> auto_proxy(std::string_view script_url, std::string_view target_url, std::uint32_t timeout_ms);

// ---- Sessions and requests ----

struct Proxy {
    enum class Kind : std::uint8_t { none, named, automatic } kind{Kind::none};
    std::string list, bypass;  // named: proxy and bypass lists
};

// A session: a connection pool with HTTP/2 and response decompression.
class Session {
public:
    [[nodiscard]] static Result<Session> open(std::string_view user_agent, const Proxy& proxy);
    Session() = default;  // no session (what a failed open leaves)
    Session(Session&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Session& operator=(Session&& other) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

private:
    friend class Request;
    explicit Session(void* handle) noexcept : handle_(handle) {}
    void* handle_{};
};

// Why a Request call failed (Request::failure).
struct Failure {
    enum class Kind : std::uint8_t {
        none,
        cancelled,                    // cancel() was called
        trust_rejected,               // the Trust callback refused the server's chain
        timed_out,
        name_not_resolved,
        cannot_connect,
        connection_lost,
        secure_channel,               // the TLS handshake failed
        client_certificate_required,
        bad_response,
        bad_url,
        unsupported_scheme,
        other,
    } kind{Kind::none};
    std::string detail;  // the stack's own description of an `other` failure
};

struct Target {
    bool secure{};          // https
    std::string host;       // as in the URL (no brackets around IPv6 literals)
    std::uint16_t port{};
    std::string method{"GET"};
    std::string path;       // path and query, already escaped
};

struct Head {
    int status{};
    std::string version;  // "HTTP/1.1", "HTTP/2"
    Headers headers;
};

enum class Scheme : std::uint8_t { basic, digest, ntlm, negotiate };
struct Challenge {
    bool proxy{};                 // a proxy asks (407), not the server
    std::vector<Scheme> schemes;  // offered, preferred first
    std::string realm;
};

class Request {
public:
    // The server's chain, DER certificates leaf first. Called on the thread
    // in send(); true continues the connection. An exception it throws
    // refuses the chain and leaves send() with that exception.
    using Trust = std::function<bool(const std::vector<std::vector<std::uint8_t>>& chain)>;

    // Opens a request on `session` (which must outlive it); `timeout_ms`
    // bounds each step. A request that could not be opened fails its first
    // send(), and failure() says why.
    [[nodiscard]] static Request open(const Session& session, const Target& target, const Headers& headers, std::uint32_t timeout_ms,
                                      Trust trust);
    Request(Request&&) noexcept;
    Request& operator=(Request&&) noexcept;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    ~Request();

    // Sends the request with `body` (again after set_credentials) and receives the response head.
    [[nodiscard]] Result<Head> send(ConstBytes body);
    // Body bytes; 0 at the end.
    [[nodiscard]] Result<std::size_t> read(Bytes buffer);
    // The authentication a 401 or 407 response asks for (`status`).
    [[nodiscard]] Result<Challenge> challenge(int status);
    [[nodiscard]] Status set_credentials(bool proxy, Scheme scheme, std::string_view user, std::string_view password);
    // Any thread: ends a blocked call; later calls fail as cancelled.
    void cancel() noexcept;
    // Why the last call that failed did.
    [[nodiscard]] Failure failure() const;

    struct State;

private:
    explicit Request(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace alchemy::host::http
