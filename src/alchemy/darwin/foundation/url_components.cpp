#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>

// NSURLComponents and NSURLQueryItem lists, by RFC 3986:
// - a URL string splits into scheme, user, password, host, port, path, query
//   and fragment; a string that is not a valid URL reference gives nil;
// - each component is kept percent-encoded: the percentEncoded* properties
//   read and write it as it is (a setter given characters the component may
//   not hold stops, where Foundation raises NSInvalidArgumentException), the
//   plain properties decode it (UTF-8) and their setters encode with the
//   component's NSCharacterSet URL*AllowedCharacterSet;
// - -string and -URL compose the components back (nil when the path does not
//   begin with "/" after an authority, or begins with "//" without one), and
//   -rangeOfScheme ... -rangeOfFragment locate them in -string;
// - queryItems split the query at "&" and the first "=" (a name without "="
//   has a nil value); setting them encodes "&" and "=" inside names, and "&"
//   inside values, besides what the query cannot hold (unverified: which of
//   "&", "=" and "+" Foundation encodes in query items).
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::uint64_t ns_not_found = 0x7fffffffffffffffULL;   // NSNotFound

// The URL*AllowedCharacterSet characters (charset.cpp's sets).
constexpr std::string_view alnum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
bool allowed(char c, std::string_view extra) {
    return alnum.find(c) != std::string_view::npos || extra.find(c) != std::string_view::npos;
}
constexpr std::string_view user_extra = "!$&'()*+,-.;=_~";
constexpr std::string_view password_extra = "!$&'()*+,-.;=_~";
constexpr std::string_view host_extra = "!$&'()*+,-.:;=[]_~";
constexpr std::string_view path_extra = "!$&'()*+,-./:=@_~";
constexpr std::string_view query_extra = "!$&'()*+,-./:;=?@_~";
constexpr std::string_view fragment_extra = "!$&'()*+,-./:;=?@_~";

bool hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
int hex_value(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

// `text` holds only `extra`/alphanumerics and valid %XX escapes.
bool valid_encoded(std::string_view text, std::string_view extra) {
    for (std::size_t at = 0; at < text.size(); ++at) {
        const auto c = text[at];
        if (c == '%') {
            if (at + 2 >= text.size() || !hex(text[at + 1]) || !hex(text[at + 2])) return false;
            at += 2;
        } else if (!allowed(c, extra)) return false;
    }
    return true;
}
std::string encode(std::string_view text, std::string_view extra, std::string_view also_encode = {}) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const auto c : text) {
        if (allowed(c, extra) && also_encode.find(c) == std::string_view::npos) result += c;
        else {
            const auto byte = static_cast<unsigned char>(c);
            result += '%';
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }
    }
    return result;
}
// Percent-decoded UTF-8, or nullopt when an escape is malformed or the bytes are not UTF-8.
std::optional<std::string> decode(std::string_view text) {
    std::string bytes;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '%') {
            if (at + 2 >= text.size() || !hex(text[at + 1]) || !hex(text[at + 2])) return std::nullopt;
            bytes += static_cast<char>(hex_value(text[at + 1]) * 16 + hex_value(text[at + 2]));
            at += 2;
        } else bytes += text[at];
    }
    // UTF-8 validation.
    for (std::size_t at = 0; at < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[at]);
        const std::size_t more = first < 0x80 ? 0 : (first >> 5) == 6 ? 1 : (first >> 4) == 14 ? 2 : (first >> 3) == 30 ? 3 : 9;
        if (more == 9 || (more && at + more >= bytes.size())) return std::nullopt;   // not a lead byte, or truncated
        for (std::size_t k = 1; k <= more; ++k)
            if ((static_cast<unsigned char>(bytes[at + k]) & 0xc0) != 0x80) return std::nullopt;
        at += more + 1;
    }
    return bytes;
}

struct Parts {
    std::optional<std::string> scheme, user, password, host, port, query, fragment;   // percent-encoded (scheme and port plain)
    std::string path;
};
struct Components final : ObjectPayload, Parts {
    std::mutex mutex;
};

// RFC 3986 section 3: the components of a URL reference, or nullopt.
std::optional<Parts> parse(std::string_view text) {
    Parts parts;
    std::string_view rest = text;
    const auto colon = rest.find(':');
    const auto delimiter = rest.find_first_of("/?#");
    if (colon != std::string_view::npos && colon > 0 && (delimiter == std::string_view::npos || colon < delimiter)) {
        const auto scheme = rest.substr(0, colon);
        if (!std::isalpha(static_cast<unsigned char>(scheme[0]))) return std::nullopt;
        for (const auto c : scheme)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return std::nullopt;
        parts.scheme = std::string(scheme);
        rest.remove_prefix(colon + 1);
    }
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
        const auto fragment = rest.substr(hash + 1);
        if (!valid_encoded(fragment, fragment_extra)) return std::nullopt;
        parts.fragment = std::string(fragment);
        rest = rest.substr(0, hash);
    }
    if (const auto question = rest.find('?'); question != std::string_view::npos) {
        const auto query = rest.substr(question + 1);
        if (!valid_encoded(query, query_extra)) return std::nullopt;
        parts.query = std::string(query);
        rest = rest.substr(0, question);
    }
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        const auto slash = rest.find('/');
        auto authority = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
        if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
            const auto userinfo = authority.substr(0, at);
            authority.remove_prefix(at + 1);
            const auto separator = userinfo.find(':');
            const auto user = userinfo.substr(0, separator);
            if (!valid_encoded(user, user_extra)) return std::nullopt;
            parts.user = std::string(user);
            if (separator != std::string_view::npos) {
                const auto password = userinfo.substr(separator + 1);
                if (!valid_encoded(password, password_extra)) return std::nullopt;
                parts.password = std::string(password);
            }
        }
        std::string_view host = authority;
        if (authority.starts_with('[')) {
            const auto close = authority.find(']');
            if (close == std::string_view::npos) return std::nullopt;
            host = authority.substr(0, close + 1);
            authority.remove_prefix(close + 1);
            if (!authority.empty()) {
                if (authority[0] != ':') return std::nullopt;
                parts.port = std::string(authority.substr(1));
            }
        } else if (const auto port = authority.rfind(':'); port != std::string_view::npos) {
            host = authority.substr(0, port);
            parts.port = std::string(authority.substr(port + 1));
        }
        if (parts.port) {
            if (parts.port->empty()) parts.port.reset();
            else for (const auto c : *parts.port) if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        }
        if (!valid_encoded(host, host_extra)) return std::nullopt;
        parts.host = std::string(host);
    }
    if (!valid_encoded(rest, path_extra)) return std::nullopt;
    parts.path = std::string(rest);
    return parts;
}

struct Composed {
    std::string text;
    // [location, length) of each component in `text`; length npos when absent.
    std::pair<std::size_t, std::size_t> scheme{0, std::string::npos}, user{0, std::string::npos}, password{0, std::string::npos},
        host{0, std::string::npos}, port{0, std::string::npos}, path{0, std::string::npos}, query{0, std::string::npos},
        fragment{0, std::string::npos};
    bool valid{true};
};
Composed compose(const Parts& parts) {
    Composed out;
    auto& t = out.text;
    const auto add = [&](std::pair<std::size_t, std::size_t>& range, const std::string& value) { range = {t.size(), value.size()}; t += value; };
    if (parts.scheme) { add(out.scheme, *parts.scheme); t += ':'; }
    const bool authority = parts.host || parts.user || parts.password || parts.port;
    if (authority) {
        t += "//";
        if (parts.user || parts.password) {
            if (parts.user) add(out.user, *parts.user);
            if (parts.password) { t += ':'; add(out.password, *parts.password); }
            t += '@';
        }
        if (parts.host) add(out.host, *parts.host);
        if (parts.port) { t += ':'; add(out.port, *parts.port); }
        if (!parts.path.empty() && parts.path[0] != '/') out.valid = false;
    } else if (parts.path.starts_with("//")) out.valid = false;
    add(out.path, parts.path);
    if (parts.query) { t += '?'; add(out.query, *parts.query); }
    if (parts.fragment) { t += '#'; add(out.fragment, *parts.fragment); }
    return out;
}

class UrlComponents {
public:
    explicit UrlComponents(Support& value) : support(value) {}
    Support& support;
    Address components_class{}, query_item_class{};

    [[noreturn]] static void stop(const std::string& why) { throw std::runtime_error("Alchemy Foundation: " + why); }
    std::shared_ptr<Components> of(Address object) {
        auto state = object ? support.objc.find_host_state<Components>(object) : nullptr;
        if (!state) stop("NSURLComponents is not initialized");
        return state;
    }
    Address ns(const std::optional<std::string>& value) { return value ? string(support, *value) : 0; }
    Address decoded(const std::optional<std::string>& value) {
        if (!value) return 0;
        const auto text = decode(*value);
        return text ? string(support, *text) : 0;
    }
    std::optional<std::string> argument(Address value) {
        if (!value) return std::nullopt;
        return text(support, value);
    }
    Address make_from(std::optional<Parts> parts) {
        if (!parts) return 0;
        auto state = std::make_shared<Components>();
        static_cast<Parts&>(*state) = *parts;
        return make_object(support, components_class, state);
    }
    // -init... on an allocated instance: its state, or nil (the instance released) for an invalid string.
    Address initialize(Address self, std::optional<Parts> parts) {
        if (!parts) { objc_release(support, self); return 0; }
        const auto state = support.objc.host_state<Components>(self);
        std::lock_guard lock(state->mutex);
        static_cast<Parts&>(*state) = *parts;
        return self;
    }
    Parts snapshot(Address object) {
        const auto state = of(object);
        std::lock_guard lock(state->mutex);
        return static_cast<const Parts&>(*state);
    }
    std::optional<std::string> url_text(Address url, bool resolve) {
        if (!url) return std::nullopt;
        const auto value = support.send_id(url, resolve ? "absoluteString" : "relativeString");
        return value ? std::optional<std::string>(text(support, value)) : std::nullopt;
    }
    // A query item list (NSArray of NSURLQueryItem) from a percent-encoded query.
    Address items(const std::optional<std::string>& query, bool decode_items) {
        if (!query) return 0;
        std::vector<Address> list;
        std::string_view rest = *query;
        while (true) {
            const auto amp = rest.find('&');
            const auto item = rest.substr(0, amp);
            const auto equals = item.find('=');
            const auto name = item.substr(0, equals);
            const auto value = equals == std::string_view::npos ? std::optional<std::string_view>{} : item.substr(equals + 1);
            const auto convert = [&](std::string_view part) -> Address {
                if (!decode_items) return string(support, part);
                const auto text = decode(part);
                return string(support, text ? *text : std::string(part));
            };
            const auto name_object = convert(name);
            const auto value_object = value ? convert(*value) : 0;
            if (!query_item_class) query_item_class = support.objc.find_class("NSURLQueryItem");
            if (!query_item_class) stop("NSURLQueryItem is not registered");
            list.push_back(support.send_id(query_item_class, "queryItemWithName:value:", {name_object, value_object}));
            if (amp == std::string_view::npos) break;
            rest.remove_prefix(amp + 1);
        }
        return array(support, list);
    }
    // A percent-encoded query from NSURLQueryItems (encoding them when `encode_items`).
    std::optional<std::string> query_of(Address list, bool encode_items) {
        if (!list) return std::nullopt;
        std::string query;
        const auto count = support.send_id(list, "count");
        for (Address index = 0; index < count; ++index) {
            const auto item = support.send_id(list, "objectAtIndex:", {index});
            if (index) query += '&';
            const auto name = text(support, support.send_id(item, "name"));
            const auto value_object = support.send_id(item, "value");
            if (encode_items) query += encode(name, query_extra, "&=#+");
            else {
                if (!valid_encoded(name, query_extra)) stop("NSInvalidArgumentException: a percent-encoded query item name holds characters a query cannot");
                query += name;
            }
            if (value_object) {
                const auto value = text(support, value_object);
                query += '=';
                if (encode_items) query += encode(value, query_extra, "&#+");
                else {
                    if (!valid_encoded(value, query_extra)) stop("NSInvalidArgumentException: a percent-encoded query item value holds characters a query cannot");
                    query += value;
                }
            }
        }
        return query;
    }

    void install();
};

void UrlComponents::install() {
    const auto self = this;
    components_class = support.declare_class("NSURLComponents", "NSObject", foundation_library);
    const auto c = components_class;

    // ---- creation ----
    support.method(c, "init", [self](CpuState& s, GuestMemory&) { s.x[0] = self->initialize(s.x[0], Parts{}); }, "@@:");
    support.method(c, "initWithString:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->initialize(s.x[0], s.x[2] ? parse(text(self->support, s.x[2])) : std::nullopt);
    }, "@@:@");
    support.method(c, "initWithURL:resolvingAgainstBaseURL:", [self](CpuState& s, GuestMemory&) {
        const auto value = self->url_text(s.x[2], (s.x[3] & 0xff) != 0);
        s.x[0] = self->initialize(s.x[0], value ? parse(*value) : std::nullopt);
    }, "@@:@B");
    support.method(c, "componentsWithString:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, s.x[2] ? self->make_from(parse(text(self->support, s.x[2]))) : 0);
    }, "@@:@", true);
    support.method(c, "componentsWithURL:resolvingAgainstBaseURL:", [self](CpuState& s, GuestMemory&) {
        const auto value = self->url_text(s.x[2], (s.x[3] & 0xff) != 0);
        s.x[0] = autoreleased(self->support, value ? self->make_from(parse(*value)) : 0);
    }, "@@:@B", true);
    for (const auto* name : {"copy", "copyWithZone:", "mutableCopy", "mutableCopyWithZone:"})
        support.method(c, name, [self](CpuState& s, GuestMemory&) { s.x[0] = self->make_from(self->snapshot(s.x[0])); }, "@@:");

    // ---- the URL and its string ----
    support.method(c, "string", [self](CpuState& s, GuestMemory&) {
        const auto composed = compose(self->snapshot(s.x[0]));
        s.x[0] = composed.valid ? string(self->support, composed.text) : 0;
    }, "@@:");
    support.method(c, "URL", [self](CpuState& s, GuestMemory&) {
        const auto composed = compose(self->snapshot(s.x[0]));
        s.x[0] = composed.valid ? self->support.send_id(self->support.objc.find_class("NSURL"), "URLWithString:", {string(self->support, composed.text)}) : 0;
    }, "@@:");
    support.method(c, "URLRelativeToURL:", [self](CpuState& s, GuestMemory&) {
        const auto composed = compose(self->snapshot(s.x[0]));
        s.x[0] = composed.valid ? self->support.send_id(self->support.objc.find_class("NSURL"), "URLWithString:relativeToURL:",
                                                        {string(self->support, composed.text), s.x[2]}) : 0;
    }, "@@:@");
    support.method(c, "description", [self](CpuState& s, GuestMemory&) {
        const auto composed = compose(self->snapshot(s.x[0]));
        s.x[0] = string(self->support, "<NSURLComponents> " + composed.text);
    }, "@@:");
    support.method(c, "isEqual:", [self](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? self->support.objc.find_host_state<Components>(s.x[2]) : nullptr;
        s.x[0] = other && compose(self->snapshot(s.x[0])).text == compose(self->snapshot(s.x[2])).text;
    }, "B@:@");
    support.method(c, "hash", [self](CpuState& s, GuestMemory&) {
        s.x[0] = std::hash<std::string>{}(compose(self->snapshot(s.x[0])).text);
    }, "Q@:");
    // NSRange -rangeOf...: in -string; {NSNotFound, 0} for an absent component.
    for (const auto& [selector, which] : std::array<std::pair<const char*, int>, 8>{{
             {"rangeOfScheme", 0}, {"rangeOfUser", 1}, {"rangeOfPassword", 2}, {"rangeOfHost", 3},
             {"rangeOfPort", 4}, {"rangeOfPath", 5}, {"rangeOfQuery", 6}, {"rangeOfFragment", 7}}}) {
        support.method(c, selector, [self, which](CpuState& s, GuestMemory&) {
            const auto composed = compose(self->snapshot(s.x[0]));
            const std::array ranges{composed.scheme, composed.user, composed.password, composed.host, composed.port, composed.path,
                                    composed.query, composed.fragment};
            const auto range = ranges[static_cast<std::size_t>(which)];
            const bool absent = !composed.valid || range.second == std::string::npos;
            // A string's location and length count UTF-16 units: the composed text is ASCII.
            s.x[0] = absent ? ns_not_found : range.first;
            s.x[1] = absent ? 0 : range.second;
        }, "{_NSRange=QQ}@:");
    }

    // ---- components ----
    struct Field { const char* name; std::optional<std::string> Parts::* member; std::string_view extra; };
    const std::array<Field, 6> fields{{
        {"user", &Parts::user, user_extra}, {"password", &Parts::password, password_extra},
        {"host", &Parts::host, host_extra}, {"query", &Parts::query, query_extra},
        {"fragment", &Parts::fragment, fragment_extra}, {"scheme", &Parts::scheme, {}}}};
    for (const auto& field : fields) {
        const std::string name = field.name;
        const auto member = field.member;
        const auto extra = field.extra;
        const auto capital = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])))) + name.substr(1);
        if (name == "scheme") {
            support.method(c, "scheme", [self, member](CpuState& s, GuestMemory&) {
                const auto state = self->of(s.x[0]);
                std::lock_guard lock(state->mutex);
                s.x[0] = self->ns((*state).*member);
            }, "@@:");
            support.method(c, "setScheme:", [self, member](CpuState& s, GuestMemory&) {
                auto value = self->argument(s.x[2]);
                if (value) {
                    const bool valid = !value->empty() && std::isalpha(static_cast<unsigned char>((*value)[0])) &&
                        std::all_of(value->begin(), value->end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '+' || ch == '-' || ch == '.'; });
                    if (!valid) stop("NSInvalidArgumentException: invalid characters in -[NSURLComponents setScheme:]");
                }
                const auto state = self->of(s.x[0]);
                std::lock_guard lock(state->mutex);
                (*state).*member = value;
            }, "v@:@");
            continue;
        }
        support.method(c, name, [self, member](CpuState& s, GuestMemory&) {
            const auto state = self->of(s.x[0]);
            std::optional<std::string> value;
            { std::lock_guard lock(state->mutex); value = (*state).*member; }
            s.x[0] = self->decoded(value);
        }, "@@:");
        support.method(c, "percentEncoded" + capital, [self, member](CpuState& s, GuestMemory&) {
            const auto state = self->of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = self->ns((*state).*member);
        }, "@@:");
        support.method(c, "set" + capital + ":", [self, member, extra](CpuState& s, GuestMemory&) {
            const auto value = self->argument(s.x[2]);
            const auto state = self->of(s.x[0]);
            std::lock_guard lock(state->mutex);
            (*state).*member = value ? std::optional<std::string>(encode(*value, extra)) : std::nullopt;
        }, "v@:@");
        support.method(c, "setPercentEncoded" + capital + ":", [self, member, extra, name](CpuState& s, GuestMemory&) {
            const auto value = self->argument(s.x[2]);
            if (value && !valid_encoded(*value, extra))
                stop("NSInvalidArgumentException: invalid characters in -[NSURLComponents setPercentEncoded" + name + ":]");
            const auto state = self->of(s.x[0]);
            std::lock_guard lock(state->mutex);
            (*state).*member = value;
        }, "v@:@");
    }
    // The encoded host is the percent-encoded one (no IDNA conversion is applied).
    support.method(c, "encodedHost", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = self->ns(state->host);
    }, "@@:");
    support.method(c, "path", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::string value;
        { std::lock_guard lock(state->mutex); value = state->path; }
        s.x[0] = self->decoded(value);
    }, "@@:");
    support.method(c, "percentEncodedPath", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = string(self->support, state->path);
    }, "@@:");
    support.method(c, "setPath:", [self](CpuState& s, GuestMemory&) {
        const auto value = self->argument(s.x[2]).value_or("");
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->path = encode(value, path_extra);
    }, "v@:@");
    support.method(c, "setPercentEncodedPath:", [self](CpuState& s, GuestMemory&) {
        const auto value = self->argument(s.x[2]).value_or("");
        if (!valid_encoded(value, path_extra)) stop("NSInvalidArgumentException: invalid characters in -[NSURLComponents setPercentEncodedPath:]");
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->path = value;
    }, "v@:@");
    support.method(c, "port", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::optional<std::string> port;
        { std::lock_guard lock(state->mutex); port = state->port; }
        s.x[0] = port ? number(self->support, std::stoll(*port)) : 0;
    }, "@@:");
    support.method(c, "setPort:", [self](CpuState& s, GuestMemory&) {
        std::optional<std::string> port;
        if (s.x[2]) {
            const auto value = static_cast<std::int64_t>(self->support.send_id(s.x[2], "longLongValue"));
            if (value < 0) stop("NSInvalidArgumentException: a negative port in -[NSURLComponents setPort:]");
            port = std::to_string(value);
        }
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->port = port;
    }, "v@:@");
    support.method(c, "queryItems", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::optional<std::string> query;
        { std::lock_guard lock(state->mutex); query = state->query; }
        s.x[0] = self->items(query, true);
    }, "@@:");
    support.method(c, "percentEncodedQueryItems", [self](CpuState& s, GuestMemory&) {
        const auto state = self->of(s.x[0]);
        std::optional<std::string> query;
        { std::lock_guard lock(state->mutex); query = state->query; }
        s.x[0] = self->items(query, false);
    }, "@@:");
    support.method(c, "setQueryItems:", [self](CpuState& s, GuestMemory&) {
        const auto query = self->query_of(s.x[2], true);
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->query = query;
    }, "v@:@");
    support.method(c, "setPercentEncodedQueryItems:", [self](CpuState& s, GuestMemory&) {
        const auto query = self->query_of(s.x[2], false);
        const auto state = self->of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->query = query;
    }, "v@:@");
}

std::unique_ptr<UrlComponents> components_instance;

} // namespace

void install_url_components(Support& support) {
    components_instance = std::make_unique<UrlComponents>(support);
    components_instance->install();
}

} // namespace alchemy::gui::completion_kit
