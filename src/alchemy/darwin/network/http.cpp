#include "alchemy/darwin/network/http.hpp"
#include "alchemy/host/network_policy.hpp"

#include "alchemy/darwin/systemconfiguration/system_configuration.hpp"

#include <algorithm>
#include <span>

// HTTP exchanges for URL loading over host::http (see http.hpp).
namespace alchemy::gui::network {
namespace {

std::uint32_t milliseconds(double seconds) {
    if (!(seconds > 0)) return 60000;
    if (seconds > 2'000'000) return 0x7fffffff;
    return static_cast<std::uint32_t>(seconds * 1000);
}

std::string endpoint_text(const sysconfig::ProxyEndpoint& endpoint) {
    return endpoint.host + (endpoint.port ? ":" + std::to_string(*endpoint.port) : std::string{});
}

} // namespace

std::optional<std::string> header_value(const Headers& headers, std::string_view name) {
    for (const auto& [key, value] : headers) if (security::iequals(key, name)) return value;
    return std::nullopt;
}

ProxyConfiguration system_proxy() {
    const auto settings = sysconfig::current_proxy_settings();
    ProxyConfiguration result;
    std::string proxy;
    if (settings.http) proxy += "http=" + endpoint_text(*settings.http) + ";";
    if (settings.https) proxy += "https=" + endpoint_text(*settings.https) + ";";
    if (!proxy.empty()) {
        result.kind = ProxyConfiguration::Kind::named;
        result.proxy = proxy;
        for (const auto& exception : settings.exceptions) result.bypass += exception + ";";
        if (settings.exclude_simple_hostnames) result.bypass += "<local>";
        return result;
    }
    if (settings.auto_discovery || !settings.auto_config_url.empty()) result.kind = ProxyConfiguration::Kind::automatic;
    return result;
}

std::shared_ptr<HttpClient> HttpClient::create(const std::string& user_agent, const ProxyConfiguration& proxy) {
    using Kind = host::http::Proxy::Kind;
    host::http::Proxy settings;
    settings.kind = proxy.kind == ProxyConfiguration::Kind::named ? Kind::named
                  : proxy.kind == ProxyConfiguration::Kind::automatic ? Kind::automatic : Kind::none;
    settings.list = proxy.proxy;
    settings.bypass = proxy.bypass;
    if (network_policy::loopback_only()) settings.kind = Kind::none;   // a proxy would leave the machine
    auto session = host::http::Session::open(user_agent, settings);
    if (!session) security::fail("the host HTTP stack could not open a session (" + std::string(host::name(session.error())) + ")");
    std::shared_ptr<HttpClient> client(new HttpClient());
    client->session_ = std::move(*session);
    return client;
}

HttpExchange::HttpExchange(std::shared_ptr<HttpClient> client, HttpRequest request, TrustDecision trust)
    : client_(std::move(client)), request_(std::move(request)), trust_(std::move(trust)) {}

HttpExchange::~HttpExchange() = default;

void HttpExchange::cancel() {
    cancelled_ = true;
    std::lock_guard lock(mutex_);
    closed_ = true;
    if (host_) host_->cancel();
}

host::http::Request* HttpExchange::request() {
    std::lock_guard lock(mutex_);
    return closed_ || !host_ ? nullptr : &*host_;
}

host::http::Request* HttpExchange::open() {
    std::lock_guard lock(mutex_);
    if (closed_) return nullptr;
    if (!host_) {
        const auto& url = request_.url;
        host::http::Target target;
        target.secure = url.scheme == "https";
        target.host = url.host_name();
        target.port = static_cast<std::uint16_t>(url.effective_port());
        target.method = request_.method;
        target.path = url.path_and_query();
        // Certificates are judged by the TrustDecision (SecTrust), not by the stack.
        auto trust = [this](const std::vector<std::vector<std::uint8_t>>& der) {
            std::vector<security::CertificatePtr> chain;
            for (const auto& certificate : der)
                if (auto parsed = security::Certificate::parse(certificate)) chain.push_back(std::move(parsed));
            return !chain.empty() && trust_ && trust_(chain);
        };
        host_.emplace(host::http::Request::open(client_->session(), target, request_.headers, milliseconds(request_.timeout), std::move(trust)));
    }
    return &*host_;
}

HttpFailure HttpExchange::failure_from(const host::http::Failure& cause) {
    using Kind = host::http::Failure::Kind;
    HttpFailure failure;
    const auto server = request_.url.host_name();
    if (cause.kind == Kind::trust_rejected) {
        failure.code = url_error::certificate_untrusted;
        failure.trust = true;
        failure.description = "The certificate for this server is invalid. You might be connecting to a server that is pretending to be "
            "\xE2\x80\x9C" + server + "\xE2\x80\x9D which could put your confidential information at risk.";
        return failure;
    }
    if (cancelled_) {
        failure.code = url_error::cancelled;
        failure.description = "cancelled";
        return failure;
    }
    switch (cause.kind) {
    case Kind::timed_out: failure.code = url_error::timed_out; failure.description = "The request timed out."; break;
    case Kind::name_not_resolved:
        failure.code = url_error::cannot_find_host; failure.description = "A server with the specified hostname could not be found."; break;
    case Kind::cannot_connect: failure.code = url_error::cannot_connect; failure.description = "Could not connect to the server."; break;
    case Kind::connection_lost: failure.code = url_error::connection_lost; failure.description = "The network connection was lost."; break;
    case Kind::secure_channel:
        failure.code = url_error::secure_connection_failed;
        failure.description = "An SSL error has occurred and a secure connection to the server cannot be made.";
        break;
    case Kind::client_certificate_required:
        failure.code = url_error::client_certificate_required; failure.description = "The server requires a client certificate."; break;
    case Kind::bad_response: failure.code = url_error::bad_server_response; failure.description = "The server returned a bad response."; break;
    case Kind::bad_url: failure.code = url_error::bad_url; failure.description = "unsupported URL"; break;
    case Kind::unsupported_scheme: failure.code = url_error::unsupported_url; failure.description = "unsupported URL"; break;
    case Kind::cancelled: failure.code = url_error::cancelled; failure.description = "cancelled"; break;
    case Kind::none: case Kind::trust_rejected: case Kind::other:
        failure.code = url_error::unknown;
        failure.description = cause.detail.empty() ? "the host HTTP stack failed" : cause.detail;
        break;
    }
    return failure;
}

std::variant<HttpResponseHead, HttpFailure> HttpExchange::send() {
    if (cancelled_) return failure_from({host::http::Failure::Kind::cancelled, {}});
    // ALCHEMY_NETWORK_DENY, and ALCHEMY_NETWORK=loopback: only this machine (alchemy/host/network_policy.hpp).
    const auto refused = [&] {
        const auto target = request_.url.scheme + "://" + request_.url.host_name();
        if (network_policy::refuse_listed_host("WinHTTP " + request_.method, request_.url.host_name(), target)) return true;   // ALCHEMY_NETWORK_DENY
        if (!network_policy::loopback_only() || network_policy::loopback_host(request_.url.host_name())) {
            network_policy::allowed("WinHTTP " + request_.method, target);
            return false;
        }
        network_policy::deny("WinHTTP " + request_.method, target);
        return true;
    };
    if (refused()) {
        HttpFailure failure;
        failure.code = url_error::not_connected;
        failure.description = "The Internet connection appears to be offline.";
        return failure;
    }
    const auto stack = open();
    if (!stack) return failure_from({host::http::Failure::Kind::cancelled, {}});
    auto head = stack->send(std::as_bytes(std::span(request_.body)));
    if (!head) return failure_from(stack->failure());
    sent_ = request_.body.size();
    return HttpResponseHead{head->status, std::move(head->version), std::move(head->headers)};
}

std::size_t HttpExchange::read(std::uint8_t* buffer, std::size_t size, std::optional<HttpFailure>& failure) {
    host::http::Request* stack{};
    bool closed{};
    {
        std::lock_guard lock(mutex_);
        closed = closed_;
        if (!closed_ && host_) stack = &*host_;
    }
    if (closed) { failure = failure_from({host::http::Failure::Kind::cancelled, {}}); return 0; }
    // Before any send there is no request: NSURLErrorUnknown, as WinHTTP's invalid handle was.
    if (!stack) { failure = failure_from({host::http::Failure::Kind::other, "the request was not sent"}); return 0; }
    const auto count = stack->read(std::as_writable_bytes(std::span(buffer, size)));
    if (!count) {
        failure = failure_from(stack->failure());
        return 0;
    }
    return *count;
}

std::optional<AuthChallenge> HttpExchange::challenge(int status) {
    const auto stack = request();
    if (!stack) return std::nullopt;
    auto offered = stack->challenge(status);
    if (!offered) return std::nullopt;
    return AuthChallenge{offered->proxy ? AuthTarget::proxy : AuthTarget::server, std::move(offered->schemes), std::move(offered->realm)};
}

bool HttpExchange::set_credentials(AuthTarget target, host::http::Scheme scheme, const std::string& user, const std::string& password) {
    const auto stack = request();
    return stack && stack->set_credentials(target == AuthTarget::proxy, scheme, user, password);
}

} // namespace alchemy::gui::network
