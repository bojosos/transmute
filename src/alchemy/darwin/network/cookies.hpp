#pragma once

// HTTP cookies (RFC 6265) for NSHTTPCookieStorage (owner: mac/qlab; used by
// loading.cpp): a host cookie jar that the URL loading system fills from
// Set-Cookie response headers and reads for Cookie request headers, as
// Foundation's cookie storage does. WinHTTP's own cookie handling is off.
//   Parsing:  name=value with Expires (the RFC 6265 cookie-date algorithm),
//             Max-Age (wins over Expires), Domain (leading dot ignored; must
//             domain-match the request host, else the cookie is rejected),
//             Path (default-path otherwise), Secure, HttpOnly, SameSite.
//             Public-suffix checks are not made (a Domain naming a top-level
//             label alone is rejected).
//   Matching: host-only or domain match, path match, Secure only over
//             https, expiry; ordered by longer path first, then creation.
// Cookies live for the process (Foundation's on-disk store is not kept).

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::gui::network {

struct Cookie {
    std::string name, value, domain, path{"/"};
    std::optional<double> expires;            // Unix seconds; nullopt: a session cookie
    bool secure{}, http_only{}, host_only{true};
    std::string same_site;                    // "", "lax", "strict" (lower case), "none"
    double created{};                         // Unix seconds (ordering)
    std::uint64_t sequence{};                 // creation order tie-break
};

// The RFC 6265 cookie-date algorithm; Unix seconds or nullopt.
[[nodiscard]] std::optional<double> parse_cookie_date(std::string_view text);
// One Set-Cookie value for a response from `host` and `request_path`; nullopt when rejected.
[[nodiscard]] std::optional<Cookie> parse_set_cookie(std::string_view header, std::string_view host, std::string_view request_path, double now);
// Splits a combined Set-Cookie field ("a=1; Expires=Wed, 21 Oct 2026 ..., b=2") into cookie strings.
[[nodiscard]] std::vector<std::string> split_set_cookie_field(std::string_view field);
[[nodiscard]] bool domain_matches(std::string_view host, std::string_view domain);
[[nodiscard]] bool path_matches(std::string_view request_path, std::string_view cookie_path);

class CookieJar {
public:
    // Stores or replaces (same name, domain, path); an expired cookie deletes.
    void set(Cookie cookie, double now);
    bool remove(const Cookie& cookie);
    void remove_since(double unix_time);
    [[nodiscard]] std::vector<Cookie> all(double now);
    [[nodiscard]] std::vector<Cookie> for_url(std::string_view scheme, std::string_view host, std::string_view path, double now);
    // "a=1; b=2", or nullopt when no cookie applies.
    [[nodiscard]] std::optional<std::string> request_header(std::string_view scheme, std::string_view host, std::string_view path, double now);
    void store_response(const std::vector<std::string>& set_cookie_values, std::string_view host, std::string_view path, double now);

private:
    void purge(double now);
    std::mutex mutex_;
    std::vector<Cookie> cookies_;
    std::uint64_t sequence_{};
};

// The process-wide jar of NSHTTPCookieStorage's shared storage.
[[nodiscard]] std::shared_ptr<CookieJar> shared_cookie_jar();
[[nodiscard]] double unix_now();

} // namespace alchemy::gui::network
