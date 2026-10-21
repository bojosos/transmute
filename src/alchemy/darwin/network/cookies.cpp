#include "alchemy/darwin/network/cookies.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

// HTTP cookie jar; see cookies.hpp.
namespace alchemy::gui::network {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}
bool is_delimiter(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u == 0x09 || (u >= 0x20 && u <= 0x2F) || (u >= 0x3B && u <= 0x40) || (u >= 0x5B && u <= 0x60) || (u >= 0x7B && u <= 0x7E);
}
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2;
    const auto era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = y - era * 400;
    const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
std::string default_path(std::string_view request_path) {
    if (request_path.empty() || request_path[0] != '/') return "/";
    const auto last = request_path.rfind('/');
    return last == 0 ? "/" : std::string(request_path.substr(0, last));
}

} // namespace

double unix_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<double> parse_cookie_date(std::string_view text) {
    std::optional<int> hour, minute, second, day, month, year;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && is_delimiter(text[at])) ++at;
        const auto start = at;
        while (at < text.size() && !is_delimiter(text[at])) ++at;
        const auto token = text.substr(start, at - start);
        if (token.empty()) continue;
        // hh:mm:ss
        if (!hour) {
            int h{}, m{}, s{};
            if (std::sscanf(std::string(token).c_str(), "%2d:%2d:%2d", &h, &m, &s) == 3) { hour = h; minute = m; second = s; continue; }
        }
        // Leading digits: day-of-month is 1-2 digits, year 2-4 (each may be followed by non-digits).
        std::size_t digits = 0;
        while (digits < token.size() && std::isdigit(static_cast<unsigned char>(token[digits]))) ++digits;
        const auto leading = digits ? std::stoi(std::string(token.substr(0, std::min<std::size_t>(digits, 9)))) : 0;
        if (!day && digits >= 1 && digits <= 2) { day = leading; continue; }
        if (!month && token.size() >= 3) {
            static constexpr std::string_view months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
            const auto prefix = lower(token.substr(0, 3));
            const auto found = std::find(std::begin(months), std::end(months), prefix);
            if (found != std::end(months)) { month = static_cast<int>(found - std::begin(months)) + 1; continue; }
        }
        if (!year && digits >= 2 && digits <= 4) { year = leading; continue; }
    }
    if (year && *year >= 70 && *year <= 99) *year += 1900;
    if (year && *year >= 0 && *year <= 69) *year += 2000;
    if (!hour || !day || !month || !year) return std::nullopt;
    if (*day < 1 || *day > 31 || *year < 1601 || *hour > 23 || *minute > 59 || *second > 59) return std::nullopt;
    return static_cast<double>(days_from_civil(*year, *month, *day) * 86400 + *hour * 3600 + *minute * 60 + *second);
}

bool domain_matches(std::string_view host, std::string_view domain) {
    const auto h = lower(host), d = lower(domain);
    if (h == d) return true;
    if (h.size() <= d.size() || !h.ends_with(d) || h[h.size() - d.size() - 1] != '.') return false;
    // Not for IP addresses.
    return h.find_first_not_of("0123456789.") != std::string::npos && h.find(':') == std::string::npos;
}

bool path_matches(std::string_view request_path, std::string_view cookie_path) {
    if (request_path == cookie_path) return true;
    if (!request_path.starts_with(cookie_path)) return false;
    return cookie_path.ends_with('/') || (request_path.size() > cookie_path.size() && request_path[cookie_path.size()] == '/');
}

std::optional<Cookie> parse_set_cookie(std::string_view header, std::string_view host, std::string_view request_path, double now) {
    const auto semicolon = header.find(';');
    const auto pair = header.substr(0, semicolon);
    const auto equals = pair.find('=');
    if (equals == std::string_view::npos) return std::nullopt;
    Cookie cookie;
    cookie.name = trim(pair.substr(0, equals));
    cookie.value = trim(pair.substr(equals + 1));
    if (cookie.name.empty()) return std::nullopt;
    cookie.domain = lower(host);
    cookie.path = default_path(request_path);
    cookie.created = now;
    std::optional<double> expires, max_age_expiry;
    std::string attributes = semicolon == std::string_view::npos ? "" : std::string(header.substr(semicolon + 1));
    std::size_t at = 0;
    while (at <= attributes.size()) {
        auto end = attributes.find(';', at);
        if (end == std::string::npos) end = attributes.size();
        const auto part = std::string_view(attributes).substr(at, end - at);
        at = end + 1;
        const auto eq = part.find('=');
        const auto key = lower(trim(part.substr(0, eq)));
        const auto value = eq == std::string_view::npos ? std::string() : trim(part.substr(eq + 1));
        if (key == "expires") expires = parse_cookie_date(value);
        else if (key == "max-age") {
            if (value.empty() || !(std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '-')) continue;
            try { const auto seconds = std::stoll(value); max_age_expiry = seconds <= 0 ? 0.0 : now + static_cast<double>(seconds); } catch (...) {}
        } else if (key == "domain") {
            auto domain = lower(value);
            if (!domain.empty() && domain[0] == '.') domain.erase(0, 1);
            if (domain.empty()) continue;
            if (!domain_matches(host, domain)) return std::nullopt;
            if (domain.find('.') == std::string::npos && domain != lower(host)) return std::nullopt;   // a bare top-level label
            cookie.domain = domain;
            cookie.host_only = false;
        } else if (key == "path") {
            if (!value.empty() && value[0] == '/') cookie.path = value;
        } else if (key == "secure") cookie.secure = true;
        else if (key == "httponly") cookie.http_only = true;
        else if (key == "samesite") cookie.same_site = lower(value);
    }
    cookie.expires = max_age_expiry ? max_age_expiry : expires;
    return cookie;
}

std::vector<std::string> split_set_cookie_field(std::string_view field) {
    // A comma separates cookies when what follows looks like "name=" before any ';'.
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t at = 0; at < field.size(); ++at) {
        if (field[at] != ',') continue;
        const auto rest = field.substr(at + 1);
        const auto equals = rest.find('='), semicolon = rest.find(';'), comma = rest.find(',');
        const auto name = trim(rest.substr(0, equals == std::string_view::npos ? 0 : equals));
        const bool next_cookie = equals != std::string_view::npos && (semicolon == std::string_view::npos || equals < semicolon) &&
                                 (comma == std::string_view::npos || equals < comma) && !name.empty() &&
                                 name.find(' ') == std::string::npos;
        if (!next_cookie) continue;
        if (auto piece = trim(field.substr(start, at - start)); !piece.empty()) out.push_back(piece);
        start = at + 1;
    }
    if (auto piece = trim(field.substr(start)); !piece.empty()) out.push_back(piece);
    return out;
}

void CookieJar::purge(double now) {
    std::erase_if(cookies_, [now](const Cookie& c) { return c.expires && *c.expires <= now; });
}

void CookieJar::set(Cookie cookie, double now) {
    std::lock_guard lock(mutex_);
    const auto same = [&](const Cookie& c) { return c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path; };
    const auto found = std::find_if(cookies_.begin(), cookies_.end(), same);
    if (found != cookies_.end()) {
        cookie.created = found->created;   // RFC 6265 5.3 step 11
        cookie.sequence = found->sequence;
        cookies_.erase(found);
    } else {
        cookie.sequence = ++sequence_;
    }
    if (cookie.expires && *cookie.expires <= now) return;   // expired: a deletion
    cookies_.push_back(std::move(cookie));
}

bool CookieJar::remove(const Cookie& cookie) {
    std::lock_guard lock(mutex_);
    return std::erase_if(cookies_, [&](const Cookie& c) { return c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path; }) > 0;
}

void CookieJar::remove_since(double unix_time) {
    std::lock_guard lock(mutex_);
    std::erase_if(cookies_, [unix_time](const Cookie& c) { return c.created >= unix_time; });
}

std::vector<Cookie> CookieJar::all(double now) {
    std::lock_guard lock(mutex_);
    purge(now);
    return cookies_;
}

std::vector<Cookie> CookieJar::for_url(std::string_view scheme, std::string_view host, std::string_view path, double now) {
    std::lock_guard lock(mutex_);
    purge(now);
    const auto request_path = path.empty() ? std::string_view("/") : path;
    const bool secure = lower(scheme) == "https";
    std::vector<Cookie> out;
    for (const auto& c : cookies_) {
        const bool host_ok = c.host_only ? lower(host) == c.domain : domain_matches(host, c.domain);
        if (!host_ok || !path_matches(request_path, c.path) || (c.secure && !secure)) continue;
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const Cookie& a, const Cookie& b) {
        if (a.path.size() != b.path.size()) return a.path.size() > b.path.size();
        return a.sequence < b.sequence;
    });
    return out;
}

std::optional<std::string> CookieJar::request_header(std::string_view scheme, std::string_view host, std::string_view path, double now) {
    const auto cookies = for_url(scheme, host, path, now);
    if (cookies.empty()) return std::nullopt;
    std::string header;
    for (const auto& c : cookies) {
        if (!header.empty()) header += "; ";
        header += c.name + "=" + c.value;
    }
    return header;
}

void CookieJar::store_response(const std::vector<std::string>& values, std::string_view host, std::string_view path, double now) {
    for (const auto& value : values)
        if (auto cookie = parse_set_cookie(value, host, path, now)) set(std::move(*cookie), now);
}

std::shared_ptr<CookieJar> shared_cookie_jar() {
    static const auto jar = std::make_shared<CookieJar>();
    return jar;
}

} // namespace alchemy::gui::network
