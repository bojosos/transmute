#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/darwin/network/cookies.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

// NSHTTPCookie and NSHTTPCookieStorage's cookie access (owner: mac/qlab) over
// cookies.cpp's jar. The shared storage (loading.cpp's object) is the shared
// jar; the URL loading system reads and fills it (loading.cpp).
//   NSHTTPCookie: +cookieWithProperties: / -initWithProperties: (NSHTTPCookie*
//   keys: Name, Value, Domain, Path, Expires (date or string), Max-Age,
//   Secure, HttpOnly (as "HttpOnly"), SameSite, Discard, OriginURL),
//   -properties, -name, -value, -domain (leading "." for domain cookies, as
//   Foundation), -path, -expiresDate, -isSecure, -isHTTPOnly, -isSessionOnly,
//   -sameSitePolicy, -version (0), -comment / -commentURL / -portList (nil),
//   +cookiesWithResponseHeaderFields:forURL:, +requestHeaderFieldsWithCookies:.
//   NSHTTPCookieStorage: -cookies, -cookiesForURL:, -setCookie:,
//   -deleteCookie:, -setCookies:forURL:mainDocumentURL:, -removeCookiesSinceDate:.
// Not provided (explicit failures): -sortedCookiesUsingDescriptors:,
// group-container storages and task-based accessors.
namespace alchemy::gui::network {
namespace {

struct CookieState final : ObjectPayload { Cookie cookie; };

constexpr double reference_to_unix = 978307200.0;

} // namespace

void install_cookies(const std::shared_ptr<Urls>& urls) {
    const auto b = urls->b;
    const auto object = b->objc.find_class("NSObject");
    const auto storage_class = b->objc.find_class("NSHTTPCookieStorage");
    if (!storage_class) security::fail("NSHTTPCookieStorage is not provided (install_url_loading first)");
    const auto cookie_class = b->define_class("NSHTTPCookie", object, foundation);
    // Property keys.
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSHTTPCookieName", "Name"}, {"_NSHTTPCookieValue", "Value"}, {"_NSHTTPCookieDomain", "Domain"},
             {"_NSHTTPCookiePath", "Path"}, {"_NSHTTPCookieExpires", "Expires"}, {"_NSHTTPCookieMaximumAge", "Max-Age"},
             {"_NSHTTPCookieSecure", "Secure"}, {"_NSHTTPCookieDiscard", "Discard"}, {"_NSHTTPCookieVersion", "Version"},
             {"_NSHTTPCookieComment", "Comment"}, {"_NSHTTPCookieCommentURL", "CommentURL"}, {"_NSHTTPCookieOriginURL", "OriginURL"},
             {"_NSHTTPCookiePort", "Port"}, {"_NSHTTPCookieSameSitePolicy", "SameSite"}, {"_NSHTTPCookieSameSiteLax", "lax"},
             {"_NSHTTPCookieSameSiteStrict", "strict"}, {"_NSHTTPCookieManagerAcceptPolicyChangedNotification",
                                                         "NSHTTPCookieManagerAcceptPolicyChangedNotification"},
             {"_NSHTTPCookieManagerCookiesChangedNotification", "NSHTTPCookieManagerCookiesChangedNotification"}})
        if (!b->exported(foundation, symbol)) b->export_string(foundation, symbol, value);

    const auto make_cookie = [b, cookie_class](const Cookie& cookie) {
        auto state = std::make_shared<CookieState>();
        state->cookie = cookie;
        return b->process.objects().create(16, cookie_class, ObjectKind::instance, state, {});
    };
    const auto cookie_of = [b](Address value) -> Cookie {
        const auto state = b->objc.find_host_state<CookieState>(value);
        if (!state) security::fail("not an NSHTTPCookie");
        return state->cookie;
    };
    const auto string_value = [b](Address dictionary, const char* key) -> std::optional<std::string> {
        const auto key_object = b->autorelease(b->string(key));
        const auto value = b->lookup(dictionary, key_object);
        if (!value) return std::nullopt;
        if (b->is_string(value)) return b->text(value);
        if (const auto number = b->integer(value)) return std::to_string(*number);
        return std::nullopt;
    };
    const auto flag = [string_value](Address dictionary, const char* key) {
        const auto value = string_value(dictionary, key);
        return value && (*value == "TRUE" || *value == "true" || *value == "1" || *value == "YES" || *value == "yes");
    };
    // NSHTTPCookie properties -> Cookie (nullopt when invalid, as Foundation's nil).
    const auto from_properties = [b, string_value, flag, urls](Address properties) -> std::optional<Cookie> {
        if (!properties || !b->is_dictionary(properties)) return std::nullopt;
        Cookie cookie;
        const auto name = string_value(properties, "Name"), value = string_value(properties, "Value");
        auto domain = string_value(properties, "Domain");
        if (!domain)
            if (const auto origin_key = b->autorelease(b->string("OriginURL")); const auto origin = b->lookup(properties, origin_key))
                if (const auto parts = b->is_string(origin) ? parse_url(b->text(origin)) : urls->parts(origin)) domain = parts->host_name();
        if (!name || !value || !domain || name->empty()) return std::nullopt;
        cookie.name = *name;
        cookie.value = *value;
        cookie.host_only = !domain->starts_with('.');
        cookie.domain = domain->starts_with('.') ? domain->substr(1) : *domain;
        std::transform(cookie.domain.begin(), cookie.domain.end(), cookie.domain.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        cookie.path = string_value(properties, "Path").value_or("/");
        cookie.secure = flag(properties, "Secure");
        cookie.http_only = flag(properties, "HttpOnly");
        cookie.same_site = string_value(properties, "SameSite").value_or("");
        cookie.created = unix_now();
        const auto expires_key = b->autorelease(b->string("Expires"));
        if (const auto expires = b->lookup(properties, expires_key)) {
            if (b->is_string(expires)) cookie.expires = parse_cookie_date(b->text(expires));
            else cookie.expires = b->cf.date_time(expires) + reference_to_unix;
        }
        if (const auto age = string_value(properties, "Max-Age")) {
            try { cookie.expires = cookie.created + static_cast<double>(std::stoll(*age)); } catch (...) {}
        }
        if (flag(properties, "Discard")) cookie.expires.reset();
        return cookie;
    };

    const auto cc = cookie_class;
    b->method(cc, "cookieWithProperties:", [b, from_properties, make_cookie](CpuState& s, GuestMemory&) {
        const auto cookie = from_properties(s.x[2]);
        s.x[0] = cookie ? b->autorelease(make_cookie(*cookie)) : 0;
    }, true);
    b->method(cc, "initWithProperties:", [b, from_properties](CpuState& s, GuestMemory&) {
        const auto cookie = from_properties(s.x[2]);
        if (!cookie) { b->release(s.x[0]); s.x[0] = 0; return; }
        auto state = std::make_shared<CookieState>();
        state->cookie = *cookie;
        static_cast<void>(b->objc.host_state<CookieState>(s.x[0], [&] { return state; }));
    });
    const auto shown_domain = [](const Cookie& c) { return c.host_only ? c.domain : "." + c.domain; };
    b->method(cc, "name", [b, cookie_of](CpuState& s, GuestMemory&) { s.x[0] = b->autorelease(b->string(cookie_of(s.x[0]).name)); });
    b->method(cc, "value", [b, cookie_of](CpuState& s, GuestMemory&) { s.x[0] = b->autorelease(b->string(cookie_of(s.x[0]).value)); });
    b->method(cc, "domain", [b, cookie_of, shown_domain](CpuState& s, GuestMemory&) { s.x[0] = b->autorelease(b->string(shown_domain(cookie_of(s.x[0])))); });
    b->method(cc, "path", [b, cookie_of](CpuState& s, GuestMemory&) { s.x[0] = b->autorelease(b->string(cookie_of(s.x[0]).path)); });
    b->method(cc, "expiresDate", [b, cookie_of](CpuState& s, GuestMemory&) {
        const auto cookie = cookie_of(s.x[0]);
        s.x[0] = cookie.expires ? b->autorelease(b->cf.date(*cookie.expires - reference_to_unix)) : 0;
    });
    b->method(cc, "isSecure", [cookie_of](CpuState& s, GuestMemory&) { s.x[0] = cookie_of(s.x[0]).secure; }, false, "B@:");
    b->method(cc, "isHTTPOnly", [cookie_of](CpuState& s, GuestMemory&) { s.x[0] = cookie_of(s.x[0]).http_only; }, false, "B@:");
    b->method(cc, "isSessionOnly", [cookie_of](CpuState& s, GuestMemory&) { s.x[0] = !cookie_of(s.x[0]).expires.has_value(); }, false, "B@:");
    b->method(cc, "sameSitePolicy", [b, cookie_of](CpuState& s, GuestMemory&) {
        const auto policy = cookie_of(s.x[0]).same_site;
        s.x[0] = policy == "lax" || policy == "strict" ? b->autorelease(b->string(policy)) : 0;
    });
    b->method(cc, "version", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "Q@:");
    for (const auto* name : {"comment", "commentURL", "portList"})
        b->method(cc, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    b->method(cc, "properties", [b, cookie_of, shown_domain](CpuState& s, GuestMemory&) {
        const auto c = cookie_of(s.x[0]);
        std::vector<std::pair<Address, Address>> entries{
            {b->string("Name"), b->string(c.name)}, {b->string("Value"), b->string(c.value)},
            {b->string("Domain"), b->string(shown_domain(c))}, {b->string("Path"), b->string(c.path)}};
        if (c.secure) entries.emplace_back(b->string("Secure"), b->string("TRUE"));
        if (c.http_only) entries.emplace_back(b->string("HttpOnly"), b->string("TRUE"));
        if (c.expires) entries.emplace_back(b->string("Expires"), b->cf.date(*c.expires - reference_to_unix));
        else entries.emplace_back(b->string("Discard"), b->string("TRUE"));
        if (!c.same_site.empty()) entries.emplace_back(b->string("SameSite"), b->string(c.same_site));
        const auto result = b->dictionary(entries);
        for (const auto& [key, value] : entries) { b->release(key); b->release(value); }
        s.x[0] = b->autorelease(result);
    });
    b->method(cc, "isEqual:", [b, cookie_of](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        if (!s.x[2] || !b->objc.find_host_state<CookieState>(s.x[2])) { s.x[0] = 0; return; }
        const auto a = cookie_of(s.x[0]), c = cookie_of(s.x[2]);
        s.x[0] = a.name == c.name && a.domain == c.domain && a.path == c.path;
    }, false, "B@:@");
    b->method(cc, "hash", [cookie_of](CpuState& s, GuestMemory&) {
        const auto c = cookie_of(s.x[0]);
        s.x[0] = std::hash<std::string>{}(c.name + "\n" + c.domain + "\n" + c.path);
    }, false, "Q@:");
    b->method(cc, "copyWithZone:", [b](CpuState& s, GuestMemory&) { s.x[0] = b->retain(s.x[0]); }, false, "@@:^v");
    b->method(cc, "description", [b, cookie_of, shown_domain](CpuState& s, GuestMemory&) {
        const auto c = cookie_of(s.x[0]);
        s.x[0] = b->autorelease(b->string("<NSHTTPCookie " + c.name + "=" + c.value + "; domain=" + shown_domain(c) + "; path=" + c.path + ">"));
    });
    // Header conversions.
    b->method(cc, "cookiesWithResponseHeaderFields:forURL:", [b, urls, make_cookie](CpuState& s, GuestMemory&) {
        const auto parts = s.x[3] ? urls->parts(s.x[3]) : std::nullopt;
        std::vector<Address> made;
        if (parts && s.x[2] && b->is_dictionary(s.x[2]))
            for (const auto& [key, value] : b->cf.dictionary_entries(s.x[2])) {
                if (!b->is_string(key) || !b->is_string(value) || !security::iequals(b->text(key), "Set-Cookie")) continue;
                for (const auto& piece : split_set_cookie_field(b->text(value)))
                    if (const auto cookie = parse_set_cookie(piece, parts->host_name(), parts->path.empty() ? "/" : parts->path, unix_now()))
                        made.push_back(make_cookie(*cookie));
            }
        const auto array = b->array(made);
        b->release_all(made);
        s.x[0] = b->autorelease(array);
    }, true);
    b->method(cc, "requestHeaderFieldsWithCookies:", [b, cookie_of](CpuState& s, GuestMemory&) {
        std::string header;
        if (s.x[2] && b->is_array(s.x[2]))
            for (const auto value : b->cf.array_values(s.x[2])) {
                const auto c = cookie_of(value);
                if (!header.empty()) header += "; ";
                header += c.name + "=" + c.value;
            }
        std::vector<std::pair<Address, Address>> entries;
        if (!header.empty()) entries.emplace_back(b->string("Cookie"), b->string(header));
        const auto result = b->dictionary(entries);
        for (const auto& [key, value] : entries) { b->release(key); b->release(value); }
        s.x[0] = b->autorelease(result);
    }, true);

    // ---- NSHTTPCookieStorage: the shared storage is the shared jar ----
    const auto jar_of = [](Address storage) {
        static_cast<void>(storage);
        return shared_cookie_jar();
    };
    const auto cookie_array = [b, make_cookie](const std::vector<Cookie>& cookies) {
        std::vector<Address> made;
        for (const auto& cookie : cookies) made.push_back(make_cookie(cookie));
        const auto array = b->array(made);
        b->release_all(made);
        return b->autorelease(array);
    };
    const auto sc = storage_class;
    b->method(sc, "cookies", [jar_of, cookie_array](CpuState& s, GuestMemory&) { s.x[0] = cookie_array(jar_of(s.x[0])->all(unix_now())); });
    b->method(sc, "cookiesForURL:", [urls, jar_of, cookie_array](CpuState& s, GuestMemory&) {
        const auto parts = s.x[2] ? urls->parts(s.x[2]) : std::nullopt;
        if (!parts) { s.x[0] = cookie_array({}); return; }
        s.x[0] = cookie_array(jar_of(s.x[0])->for_url(parts->scheme, parts->host_name(), parts->path, unix_now()));
    });
    b->method(sc, "setCookie:", [jar_of, cookie_of](CpuState& s, GuestMemory&) {
        if (s.x[2]) jar_of(s.x[0])->set(cookie_of(s.x[2]), unix_now());
    }, false, "v@:@");
    b->method(sc, "deleteCookie:", [jar_of, cookie_of](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(jar_of(s.x[0])->remove(cookie_of(s.x[2])));
    }, false, "v@:@");
    b->method(sc, "setCookies:forURL:mainDocumentURL:", [b, jar_of, cookie_of](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !b->is_array(s.x[2])) return;
        for (const auto value : b->cf.array_values(s.x[2])) jar_of(s.x[0])->set(cookie_of(value), unix_now());
    }, false, "v@:@@@");
    b->method(sc, "removeCookiesSinceDate:", [b, jar_of](CpuState& s, GuestMemory&) {
        if (s.x[2]) jar_of(s.x[0])->remove_since(b->cf.date_time(s.x[2]) + reference_to_unix);
    }, false, "v@:@");
    b->method(sc, "sortedCookiesUsingDescriptors:", [](CpuState&, GuestMemory&) {
        security::fail("unsupported: -[NSHTTPCookieStorage sortedCookiesUsingDescriptors:]");
    });
    for (const auto* name : {"getCookiesForTask:completionHandler:", "storeCookies:forTask:"})
        b->method(sc, name, [name](CpuState&, GuestMemory&) {
            security::fail(std::string("unsupported: -[NSHTTPCookieStorage ") + name + "]");
        }, false, "v@:@@");
    b->method(sc, "sharedCookieStorageForGroupContainerIdentifier:", [](CpuState&, GuestMemory&) {
        security::fail("unsupported: +[NSHTTPCookieStorage sharedCookieStorageForGroupContainerIdentifier:]");
    }, true);
}

} // namespace alchemy::gui::network
