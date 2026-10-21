// Host-only tests of the HTTP cookie jar (cookies.cpp): cookie dates,
// Set-Cookie parsing, path matching, the jar's request headers. Split out of
// the QLab support core tests (darwin/foundation/tests/formats_tests.cpp).
#include "alchemy/darwin/network/cookies.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

namespace net = alchemy::gui::network;

void cookie_jar() {
    // 2026-10-13 07:28:00 UTC = 1791876480.
    const double when = 1791876480;
    require(net::parse_cookie_date("Tue, 13 Oct 2026 07:28:00 GMT") == when, "IMF-fixdate");
    require(net::parse_cookie_date("Tuesday, 13-Oct-26 07:28:00 GMT") == when, "RFC 850 date");
    require(net::parse_cookie_date("Tue Oct 13 07:28:00 2026") == when, "asctime date");
    require(!net::parse_cookie_date("not a date"), "garbage date");
    const double now = when - 3600;
    const auto c = net::parse_set_cookie("sid=abc123; Path=/api; Domain=.Example.com; Secure; HttpOnly; SameSite=Lax; Max-Age=60",
                                         "www.example.com", "/login", now);
    require(c && c->name == "sid" && c->value == "abc123" && c->domain == "example.com" && !c->host_only && c->path == "/api" &&
                c->secure && c->http_only && c->same_site == "lax" && c->expires == now + 60, "Set-Cookie attributes");
    require(!net::parse_set_cookie("x=1; Domain=other.com", "www.example.com", "/", now), "foreign domain rejected");
    require(!net::parse_set_cookie("x=1; Domain=com", "example.com", "/", now), "top-level domain rejected");
    const auto plain = net::parse_set_cookie("token=t", "example.com", "/a/b/c", now);
    require(plain && plain->host_only && plain->path == "/a/b" && !plain->expires, "host-only, default path, session cookie");
    require(net::path_matches("/api/x", "/api") && !net::path_matches("/apix", "/api") && net::path_matches("/api/", "/api/"), "path matching");
    net::CookieJar jar;
    jar.store_response({"a=1; Path=/", "b=2; Path=/api", "c=3; Secure; Path=/", "d=4; Domain=example.com; Path=/"}, "www.example.com", "/api/v1", now);
    require(jar.request_header("https", "www.example.com", "/api/v1", now) == std::optional<std::string>("b=2; a=1; c=3; d=4"), "header ordering");
    require(jar.request_header("http", "www.example.com", "/", now) == std::optional<std::string>("a=1; d=4"), "secure and path filtering");
    require(jar.request_header("https", "shop.example.com", "/", now) == std::optional<std::string>("d=4"), "domain cookie on a sibling host");
    jar.store_response({"a=gone; Path=/; Max-Age=0"}, "www.example.com", "/", now);
    require(jar.request_header("http", "www.example.com", "/", now) == std::optional<std::string>("d=4"), "Max-Age=0 deletes");
    const auto pieces = net::split_set_cookie_field("a=1; Expires=Tue, 13 Oct 2026 07:28:00 GMT; Path=/, b=2, c=3; HttpOnly");
    require(pieces.size() == 3 && pieces[1] == "b=2" && pieces[2] == "c=3; HttpOnly", "combined Set-Cookie split");
}

} // namespace

int main() {
    try {
        cookie_jar();
    } catch (const std::exception& error) {
        std::cerr << "alchemy_gui_network_cookies: " << error.what() << '\n';
        return 1;
    }
    std::cout << "alchemy_gui_network_cookies: passed\n";
    return 0;
}
