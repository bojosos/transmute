#include "alchemy/darwin/network/network_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>

// Network NSURLs. Alchemy's Foundation represents file URLs only; this adds
// a host class _AlchemyNetworkURL : NSURL for every other scheme (http,
// https, ftp, mailto, custom schemes) with RFC 3986 parsing and reference
// resolution, routes +[NSURL URLWithString:] and friends to it, and adds the
// CFURL functions URL loading needs (string creation and component access),
// forwarding file URLs to the existing CFURL implementation.
namespace alchemy::gui::network {
namespace {

bool unreserved(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' || c == '~';
}
bool sub_delim(char c) { return std::string_view("!$&'()*+,;=").find(c) != std::string_view::npos; }
bool url_character(char c) {
    return unreserved(c) || sub_delim(c) || std::string_view(":/?#[]@%").find(c) != std::string_view::npos;
}
bool hex_digit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

std::string encode_invalid(std::string_view text) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto c = text[index];
        if (c == '%' && index + 2 < text.size() && hex_digit(text[index + 1]) && hex_digit(text[index + 2])) {
            result += c;
        } else if (c != '%' && url_character(c)) {
            result += c;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            result += '%';
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }
    }
    return result;
}

// Characters a path segment may contain unescaped.
std::string encode_segment(std::string_view text) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const auto c : text) {
        if (unreserved(c) || sub_delim(c) || c == ':' || c == '@') result += c;
        else {
            const auto byte = static_cast<unsigned char>(c);
            result += '%';
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }
    }
    return result;
}
std::string encode_path(std::string_view path) {
    std::string result;
    std::size_t at = 0;
    while (at <= path.size()) {
        const auto next = path.find('/', at);
        result += encode_segment(path.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (next == std::string_view::npos) break;
        result += '/';
        at = next + 1;
    }
    return result;
}

// A parsed reference: the scheme is empty for relative references.
std::optional<UrlParts> parse_reference(std::string_view text) {
    UrlParts parts;
    std::string_view rest = text;
    const auto colon = rest.find(':');
    const auto delimiter = rest.find_first_of("/?#");
    if (colon != std::string_view::npos && colon > 0 && (delimiter == std::string_view::npos || colon < delimiter)) {
        const auto scheme = rest.substr(0, colon);
        const bool valid = std::isalpha(static_cast<unsigned char>(scheme[0])) &&
            std::all_of(scheme.begin(), scheme.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.'; });
        if (valid) {
            parts.scheme = security::lower(scheme);
            rest.remove_prefix(colon + 1);
        }
    }
    if (rest.starts_with("//")) {
        parts.has_authority = true;
        rest.remove_prefix(2);
        const auto end = rest.find_first_of("/?#");
        auto authority = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
        if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
            const auto info = authority.substr(0, at);
            authority.remove_prefix(at + 1);
            if (const auto separator = info.find(':'); separator != std::string_view::npos) {
                parts.user = std::string(info.substr(0, separator));
                parts.password = std::string(info.substr(separator + 1));
            } else {
                parts.user = std::string(info);
            }
        }
        std::string_view port;
        if (authority.starts_with('[')) {
            const auto close = authority.find(']');
            if (close == std::string_view::npos) return std::nullopt;
            parts.host = std::string(authority.substr(0, close + 1));
            const auto after = authority.substr(close + 1);
            if (!after.empty()) {
                if (after[0] != ':') return std::nullopt;
                port = after.substr(1);
            }
        } else {
            const auto separator = authority.find(':');
            parts.host = std::string(authority.substr(0, separator));
            if (separator != std::string_view::npos) port = authority.substr(separator + 1);
        }
        if (!port.empty()) {
            if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
            const auto value = std::stoi(std::string(port));
            if (value > 65535) return std::nullopt;
            parts.port = value;
        }
    }
    const auto query = rest.find('?'), fragment = rest.find('#');
    const auto path_end = std::min(query, fragment);
    parts.path = std::string(rest.substr(0, path_end));
    if (fragment != std::string_view::npos) parts.fragment = std::string(rest.substr(fragment + 1));
    if (query != std::string_view::npos && (fragment == std::string_view::npos || query < fragment))
        parts.query = std::string(rest.substr(query + 1, fragment == std::string_view::npos ? std::string_view::npos : fragment - query - 1));
    return parts;
}

std::string remove_dot_segments(std::string_view input) {
    std::string output;
    std::string in(input);
    while (!in.empty()) {
        if (in.starts_with("../")) in.erase(0, 3);
        else if (in.starts_with("./")) in.erase(0, 2);
        else if (in.starts_with("/./")) in.erase(0, 2);
        else if (in == "/.") in = "/";
        else if (in.starts_with("/../") || in == "/..") {
            in = in.size() == 3 ? "/" : in.substr(3);
            const auto last = output.rfind('/');
            output.erase(last == std::string::npos ? 0 : last);
        } else if (in == "." || in == "..") in.clear();
        else {
            const auto start = in[0] == '/' ? 1 : 0;
            const auto next = in.find('/', static_cast<std::size_t>(start));
            output += in.substr(0, next);
            in = next == std::string::npos ? std::string{} : in.substr(next);
        }
    }
    return output;
}

std::string last_component(std::string_view path) {
    while (path.size() > 1 && path.ends_with('/')) path.remove_suffix(1);
    if (path == "/") return "/";
    const auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

} // namespace

std::string percent_decode(std::string_view text) {
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size() && hex_digit(text[index + 1]) && hex_digit(text[index + 2])) {
            result += static_cast<char>(std::stoi(std::string(text.substr(index + 1, 2)), nullptr, 16));
            index += 2;
        } else {
            result += text[index];
        }
    }
    return result;
}

std::string UrlParts::text() const {
    std::string result = scheme + ":";
    if (has_authority) {
        result += "//";
        if (user) {
            result += *user;
            if (password) result += ":" + *password;
            result += "@";
        }
        result += host;
        if (port) result += ":" + std::to_string(*port);
    }
    result += path;
    if (query) result += "?" + *query;
    if (fragment) result += "#" + *fragment;
    return result;
}

std::string UrlParts::host_name() const {
    std::string_view value = host;
    if (value.starts_with('[') && value.ends_with(']')) value = value.substr(1, value.size() - 2);
    return percent_decode(value);
}

int UrlParts::effective_port() const {
    if (port) return *port;
    if (scheme == "http" || scheme == "ws") return 80;
    if (scheme == "https" || scheme == "wss") return 443;
    if (scheme == "ftp") return 21;
    return -1;
}

std::string UrlParts::path_and_query() const {
    std::string result = path.empty() ? "/" : path;
    if (query) result += "?" + *query;
    return result;
}

std::optional<UrlParts> parse_url(std::string_view text) {
    auto parts = parse_reference(encode_invalid(text));
    if (!parts || parts->scheme.empty()) return std::nullopt;
    return parts;
}

std::optional<UrlParts> resolve_url(const UrlParts& base, std::string_view reference) {
    auto relative = parse_reference(encode_invalid(reference));
    if (!relative) return std::nullopt;
    UrlParts target;
    if (!relative->scheme.empty()) {
        target = *relative;
        target.path = remove_dot_segments(target.path);
        return target;
    }
    target.scheme = base.scheme;
    if (relative->has_authority) {
        target.has_authority = true;
        target.user = relative->user; target.password = relative->password;
        target.host = relative->host; target.port = relative->port;
        target.path = remove_dot_segments(relative->path);
        target.query = relative->query;
    } else {
        target.has_authority = base.has_authority;
        target.user = base.user; target.password = base.password;
        target.host = base.host; target.port = base.port;
        if (relative->path.empty()) {
            target.path = base.path;
            target.query = relative->query ? relative->query : base.query;
        } else {
            if (relative->path.starts_with('/')) target.path = remove_dot_segments(relative->path);
            else {
                std::string merged;
                if (base.has_authority && base.path.empty()) merged = "/" + relative->path;
                else {
                    const auto slash = base.path.rfind('/');
                    merged = (slash == std::string::npos ? std::string{} : base.path.substr(0, slash + 1)) + relative->path;
                }
                target.path = remove_dot_segments(merged);
            }
            target.query = relative->query;
        }
    }
    target.fragment = relative->fragment;
    return target;
}

// ---- Urls --------------------------------------------------------------------------------------

std::shared_ptr<UrlState> Urls::state(Address object) const {
    return object ? b->objc.find_host_state<UrlState>(object) : nullptr;
}

Address Urls::make(const UrlParts& parts) {
    const auto object = b->objc.allocate(network_class);
    auto value = std::make_shared<UrlState>();
    value->parts = parts;
    value->string = b->string(parts.text());
    static_cast<void>(b->objc.host_state<UrlState>(object, [&] { return value; }));
    return object;
}

std::optional<UrlParts> Urls::parts(Address object) {
    if (const auto value = state(object)) return value->parts;
    if (!object || b->cf.kind(object) != ObjectKind::url) return std::nullopt;
    UrlParts file;
    file.scheme = "file";
    file.has_authority = true;
    file.path = encode_path(b->cf.url_path(object));
    if (b->cf.url_is_directory(object) && !file.path.ends_with('/')) file.path += '/';
    return file;
}

std::string Urls::text(Address object) {
    if (const auto value = state(object)) return value->parts.text();
    if (const auto value = parts(object)) return value->text();
    security::fail("expected an NSURL");
}

Address Urls::create(std::string_view text, Address base) {
    std::optional<UrlParts> parts;
    if (base) {
        const auto base_parts = this->parts(base);
        if (!base_parts) return 0;
        parts = resolve_url(*base_parts, text);
    } else {
        if (text.size() >= 5 && security::iequals(text.substr(0, 5), "file:")) {
            // File URLs stay with Foundation's own NSURL.
            const auto string = b->string(text);
            const std::array<Address, 3> arguments{url_class, b->objc.selector("URLWithString:"), string};
            const auto result = b->call_address(original_url_with_string, arguments);
            b->release(string);
            return b->retain(result);
        }
        parts = parse_url(text);
    }
    if (!parts) return 0;
    if (parts->scheme == "file") {
        const auto path = percent_decode(parts->path);
        if (path.empty() || !path.starts_with('/')) return 0;
        return b->cf.file_url(path, path.ends_with('/'));
    }
    return make(*parts);
}

void install_urls(Urls& urls) {
    auto& b = *urls.b;
    urls.url_class = b.objc.find_class("NSURL");
    if (!urls.url_class) security::fail("NSURL is missing");
    urls.original_url_with_string = b.objc.lookup_imp(urls.url_class, b.objc.selector("URLWithString:"));
    urls.network_class = b.define_class("_AlchemyNetworkURL", urls.url_class, foundation);
    auto self = std::shared_ptr<Urls>(&urls, [](Urls*) {});   // urls outlives the process (owned by the installer)
    const auto cls = urls.network_class;
    const auto string_arg = [self](Address object) -> std::optional<std::string> { return self->b->maybe_text(object); };

    // ---- construction (on NSURL) ----------------------------------------------------------------
    const auto with_string = [self, string_arg](Address string, Address base) -> Address {
        const auto text = string_arg(string);
        if (!text) return 0;
        return self->create(*text, base);
    };
    // +URLWithString: and -initWithString: stay Foundation's; they ask this
    // handler first. File URLs ("file:") go on to Foundation itself, as
    // Urls::create does; anything that is not a string is nil.
    b.cf.extend_url_strings("URL schemes other than file:, and non-strings (nil)", [self, string_arg](Address string) -> std::optional<std::uint64_t> {
        const auto text = string_arg(string);
        if (!text) return Address{0};
        if (text->size() >= 5 && security::iequals(std::string_view(*text).substr(0, 5), "file:")) return std::nullopt;
        return self->create(*text, 0);
    });
    b.method(urls.url_class, "URLWithString:relativeToURL:", [self, with_string](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(with_string(s.x[2], s.x[3]));
    }, true, "@@:@@");
    b.method(urls.url_class, "URLWithString:encodingInvalidCharacters:", [self, with_string](CpuState& s, GuestMemory&) {
        const auto text = self->b->maybe_text(s.x[2]);
        if (!text || (!(s.x[3] & 0xff) && !std::all_of(text->begin(), text->end(), url_character))) { s.x[0] = 0; return; }
        s.x[0] = self->b->autorelease(with_string(s.x[2], 0));
    }, true, "@@:@B");
    const auto replace = [self](CpuState& s, Address value) {
        self->b->release(s.x[0]);
        s.x[0] = value;
    };
    b.method(urls.url_class, "initWithString:relativeToURL:", [with_string, replace](CpuState& s, GuestMemory&) {
        replace(s, with_string(s.x[2], s.x[3]));
    }, false, "@@:@@");

    // ---- network URL accessors -----------------------------------------------------------------
    const auto get = [self](Address object) {
        const auto value = self->state(object);
        if (!value) security::fail("expected a network NSURL");
        return value;
    };
    const auto text_or_nil = [self](const std::optional<std::string>& value) -> Address {
        return value ? self->b->autorelease(self->b->string(*value)) : 0;
    };
    b.method(cls, "dealloc", [self, cls](CpuState& s, GuestMemory&) {
        if (const auto value = self->state(s.x[0])) self->b->release(std::exchange(value->string, 0));
        static_cast<void>(self->b->objc.send_super(s.x[0], cls, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    for (const auto* name : {"absoluteString", "relativeString", "description", "debugDescription"})
        b.method(cls, name, [get](CpuState& s, GuestMemory&) { s.x[0] = get(s.x[0])->string; });
    b.method(cls, "scheme", [get, text_or_nil](CpuState& s, GuestMemory&) { s.x[0] = text_or_nil(get(s.x[0])->parts.scheme); });
    b.method(cls, "host", [get, text_or_nil](CpuState& s, GuestMemory&) {
        const auto value = get(s.x[0]);
        s.x[0] = value->parts.has_authority && !value->parts.host.empty() ? text_or_nil(value->parts.host_name()) : 0;
    });
    b.method(cls, "port", [self, get](CpuState& s, GuestMemory&) {
        const auto value = get(s.x[0]);
        s.x[0] = value->parts.port ? self->b->autorelease(self->b->number(*value->parts.port)) : 0;
    });
    b.method(cls, "user", [get, text_or_nil](CpuState& s, GuestMemory&) {
        const auto value = get(s.x[0]);
        s.x[0] = text_or_nil(value->parts.user ? std::optional(percent_decode(*value->parts.user)) : std::nullopt);
    });
    b.method(cls, "password", [get, text_or_nil](CpuState& s, GuestMemory&) {
        const auto value = get(s.x[0]);
        s.x[0] = text_or_nil(value->parts.password ? std::optional(percent_decode(*value->parts.password)) : std::nullopt);
    });
    for (const auto* name : {"path", "relativePath"})
        b.method(cls, name, [get, text_or_nil](CpuState& s, GuestMemory&) { s.x[0] = text_or_nil(percent_decode(get(s.x[0])->parts.path)); });
    b.method(cls, "query", [get, text_or_nil](CpuState& s, GuestMemory&) { s.x[0] = text_or_nil(get(s.x[0])->parts.query); });
    b.method(cls, "fragment", [get, text_or_nil](CpuState& s, GuestMemory&) { s.x[0] = text_or_nil(get(s.x[0])->parts.fragment); });
    b.method(cls, "parameterString", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    b.method(cls, "resourceSpecifier", [get, text_or_nil](CpuState& s, GuestMemory&) {
        const auto value = get(s.x[0]);
        s.x[0] = text_or_nil(value->parts.text().substr(value->parts.scheme.size() + 1));
    });
    b.method(cls, "baseURL", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    for (const auto* name : {"absoluteURL", "standardizedURL", "URLByStandardizingPath"})
        b.method(cls, name, [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->autorelease(self->b->retain(s.x[0])); });
    b.method(cls, "isFileURL", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    b.method(cls, "isFileReferenceURL", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    b.method(cls, "hasDirectoryPath", [get](CpuState& s, GuestMemory&) { s.x[0] = get(s.x[0])->parts.path.ends_with('/'); }, false, "B@:");
    b.method(cls, "lastPathComponent", [get, text_or_nil](CpuState& s, GuestMemory&) {
        s.x[0] = text_or_nil(percent_decode(last_component(get(s.x[0])->parts.path)));
    });
    b.method(cls, "pathExtension", [get, text_or_nil](CpuState& s, GuestMemory&) {
        const auto last = percent_decode(last_component(get(s.x[0])->parts.path));
        const auto dot = last.rfind('.');
        s.x[0] = text_or_nil(dot == std::string::npos || dot == 0 ? std::string{} : last.substr(dot + 1));
    });
    b.method(cls, "pathComponents", [self, get](CpuState& s, GuestMemory&) {
        const auto path = percent_decode(get(s.x[0])->parts.path);
        std::vector<Address> values;
        if (path.starts_with('/')) values.push_back(self->b->string("/"));
        std::size_t at = 0;
        while (at < path.size()) {
            const auto next = path.find('/', at);
            const auto piece = path.substr(at, next == std::string::npos ? std::string::npos : next - at);
            if (!piece.empty()) values.push_back(self->b->string(piece));
            if (next == std::string::npos) break;
            at = next + 1;
        }
        s.x[0] = self->b->autorelease(self->b->array(values));
        self->b->release_all(values);
    });
    const auto derived = [self, get](CpuState& s, auto change) {
        auto parts = get(s.x[0])->parts;
        change(parts);
        parts.query.reset();
        parts.fragment.reset();
        s.x[0] = self->b->autorelease(self->make(parts));
    };
    const auto append = [](UrlParts& parts, const std::string& component, bool directory) {
        if (!parts.path.ends_with('/')) parts.path += '/';
        parts.path += encode_path(component);
        if (directory && !parts.path.ends_with('/')) parts.path += '/';
    };
    b.method(cls, "URLByAppendingPathComponent:", [self, derived, append](CpuState& s, GuestMemory&) {
        const auto component = self->b->text(s.x[2]);
        derived(s, [&](UrlParts& parts) { append(parts, component, false); });
    });
    b.method(cls, "URLByAppendingPathComponent:isDirectory:", [self, derived, append](CpuState& s, GuestMemory&) {
        const auto component = self->b->text(s.x[2]);
        const bool directory = (s.x[3] & 0xff) != 0;
        derived(s, [&](UrlParts& parts) { append(parts, component, directory); });
    }, false, "@@:@B");
    b.method(cls, "URLByAppendingPathExtension:", [self, derived](CpuState& s, GuestMemory&) {
        const auto extension = encode_segment(self->b->text(s.x[2]));
        derived(s, [&](UrlParts& parts) {
            const bool directory = parts.path.ends_with('/');
            if (directory) parts.path.pop_back();
            parts.path += "." + extension;
            if (directory) parts.path += '/';
        });
    });
    b.method(cls, "URLByDeletingLastPathComponent", [derived](CpuState& s, GuestMemory&) {
        derived(s, [](UrlParts& parts) {
            auto path = parts.path;
            while (path.size() > 1 && path.ends_with('/')) path.pop_back();
            const auto slash = path.rfind('/');
            parts.path = slash == std::string::npos ? std::string("../") : path.substr(0, slash + 1);
        });
    });
    b.method(cls, "URLByDeletingPathExtension", [derived](CpuState& s, GuestMemory&) {
        derived(s, [](UrlParts& parts) {
            const bool directory = parts.path.ends_with('/');
            auto path = directory ? parts.path.substr(0, parts.path.size() - 1) : parts.path;
            const auto slash = path.rfind('/');
            const auto dot = path.rfind('.');
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) path.erase(dot);
            parts.path = path + (directory ? "/" : "");
        });
    });
    b.method(cls, "isEqual:", [self](CpuState& s, GuestMemory&) {
        const auto left = self->state(s.x[0]), right = self->state(s.x[2]);
        s.x[0] = left && right && left->parts.text() == right->parts.text();
    }, false, "B@:@");
    b.method(cls, "hash", [get](CpuState& s, GuestMemory&) { s.x[0] = std::hash<std::string>{}(get(s.x[0])->parts.text()); }, false, "Q@:");
    b.method(cls, "copyWithZone:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->retain(s.x[0]); }, false, "@@:^v");

    // ---- CFURL ------------------------------------------------------------------------------------------
    const std::string core(core_foundation_library);
    // A CFURL function for network URLs. When CoreFoundation implements it,
    // network URLs are answered here through CoreFoundation::extend and file
    // URLs by CoreFoundation itself (one owner of the export). Otherwise the
    // function is this provider's: it wraps whatever export exists, or, with
    // none, answers file URLs from their components (file://<path>) the same way.
    const auto wrap = [self, core](std::string symbol, std::function<Address(UrlState&, CpuState&, GuestMemory&)> mine) {
        if (const auto name = std::string_view(symbol).substr(1); self->b->cf.implements(name)) {
            self->b->cf.extend(name, "network URLs (_AlchemyNetworkURL)", [self, mine](CpuState& s, GuestMemory& m) -> std::optional<std::uint64_t> {
                if (const auto value = self->state(s.x[0])) return mine(*value, s, m);
                return std::nullopt;
            });
            return;
        }
        const auto previous = self->b->process.resolve(core, symbol);
        const auto previous_address = previous ? previous->address : 0;
        self->b->function(core, symbol, [self, mine, previous_address, symbol](CpuState& s, GuestMemory& m) {
            if (const auto value = self->state(s.x[0])) { s.x[0] = mine(*value, s, m); return; }
            if (!previous_address && s.x[0]) {
                if (const auto parts = self->parts(s.x[0])) {
                    UrlState file;
                    file.parts = *parts;
                    // Get-rule results (CFURLGetString) live in the autorelease pool.
                    file.string = self->b->autorelease(self->b->string(parts->text()));
                    s.x[0] = mine(file, s, m);
                    return;
                }
            }
            if (!previous_address) security::unsupported("CFURL function " + symbol + " for file URLs");
            const std::array<Address, 4> arguments{s.x[0], s.x[1], s.x[2], s.x[3]};
            s.x[0] = self->b->call_address(previous_address, arguments);
        });
    };
    const auto string_or_null = [self](const std::optional<std::string>& value) -> Address { return value ? self->b->string(*value) : 0; };
    wrap("_CFURLGetString", [](UrlState& u, CpuState&, GuestMemory&) { return u.string; });
    wrap("_CFURLCopyAbsoluteURL", [self](UrlState&, CpuState& s, GuestMemory&) { return self->b->retain(s.x[0]); });
    wrap("_CFURLHasDirectoryPath", [](UrlState& u, CpuState&, GuestMemory&) -> Address { return u.parts.path.ends_with('/'); });
    wrap("_CFURLCopyFileSystemPath", [self](UrlState& u, CpuState&, GuestMemory&) { return self->b->string(percent_decode(u.parts.path)); });
    wrap("_CFURLGetFileSystemRepresentation", [](UrlState&, CpuState&, GuestMemory&) -> Address { return 0; });
    wrap("_CFURLCopyScheme", [self](UrlState& u, CpuState&, GuestMemory&) { return self->b->string(u.parts.scheme); });
    wrap("_CFURLCopyHostName", [string_or_null](UrlState& u, CpuState&, GuestMemory&) {
        return string_or_null(u.parts.has_authority && !u.parts.host.empty() ? std::optional(u.parts.host_name()) : std::nullopt);
    });
    wrap("_CFURLGetPortNumber", [](UrlState& u, CpuState&, GuestMemory&) { return static_cast<Address>(static_cast<std::int64_t>(u.parts.port.value_or(-1))); });
    wrap("_CFURLCopyPath", [self](UrlState& u, CpuState&, GuestMemory&) { return self->b->string(u.parts.path); });
    // (url, charactersToLeaveEscaped): NULL leaves every escape, "" decodes them all.
    const auto escaped = [self](const std::optional<std::string>& value, Address leave) -> Address {
        if (!value) return 0;
        return self->b->string(leave && self->b->is_string(leave) && self->b->cf.length(leave) == 0 ? percent_decode(*value) : *value);
    };
    wrap("_CFURLCopyQueryString", [escaped](UrlState& u, CpuState& s, GuestMemory&) { return escaped(u.parts.query, s.x[1]); });
    wrap("_CFURLCopyFragment", [escaped](UrlState& u, CpuState& s, GuestMemory&) { return escaped(u.parts.fragment, s.x[1]); });
    wrap("_CFURLCopyUserName", [string_or_null](UrlState& u, CpuState&, GuestMemory&) {
        return string_or_null(u.parts.user ? std::optional(percent_decode(*u.parts.user)) : std::nullopt);
    });
    wrap("_CFURLCopyPassword", [string_or_null](UrlState& u, CpuState&, GuestMemory&) {
        return string_or_null(u.parts.password ? std::optional(percent_decode(*u.parts.password)) : std::nullopt);
    });
    wrap("_CFURLCopyNetLocation", [string_or_null](UrlState& u, CpuState&, GuestMemory&) {
        if (!u.parts.has_authority) return string_or_null(std::nullopt);
        const auto text = u.parts.text();
        const auto start = u.parts.scheme.size() + 3;
        return string_or_null(text.substr(start, u.parts.path.empty() && !u.parts.query && !u.parts.fragment
            ? std::string::npos : text.find_first_of("/?#", start) - start));
    });
    wrap("_CFURLCopyResourceSpecifier", [self](UrlState& u, CpuState&, GuestMemory&) {
        return self->b->string(u.parts.text().substr(u.parts.scheme.size() + 1));
    });
    wrap("_CFURLCanBeDecomposed", [](UrlState& u, CpuState&, GuestMemory&) -> Address { return u.parts.has_authority || u.parts.path.starts_with('/'); });
    wrap("_CFURLGetBaseURL", [](UrlState&, CpuState&, GuestMemory&) -> Address { return 0; });
    // CFURLCreateData(alloc, url, encoding, escapeWhitespace): the URL string's
    // bytes (URL strings are ASCII after percent-encoding).
    if (!b.exported(core, "_CFURLCreateData"))
        b.function(core, "_CFURLCreateData", [self](CpuState& s, GuestMemory&) {
            const auto text = self->text(s.x[1]);
            s.x[0] = self->b->data(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        });
    if (!b.exported(core, "_CFURLCreateWithString"))
        b.function(core, "_CFURLCreateWithString", [self](CpuState& s, GuestMemory&) {
            const auto text = self->b->maybe_text(s.x[1]);
            s.x[0] = text ? self->create(*text, s.x[2]) : 0;
        });
    if (!b.exported(core, "_CFURLCreateWithBytes"))
        b.function(core, "_CFURLCreateWithBytes", [self](CpuState& s, GuestMemory& m) {
            // (alloc, bytes, length, encoding, baseURL): UTF-8, ASCII and Latin-1 bytes.
            if (s.x[2] > (1U << 20)) { s.x[0] = 0; return; }
            const auto encoding = static_cast<std::uint32_t>(s.x[3]);
            if (encoding != 0x08000100 && encoding != 0x0600 && encoding != 0x0201) security::unsupported("CFURLCreateWithBytes encoding");
            std::string text(static_cast<std::size_t>(s.x[2]), '\0');
            if (!text.empty()) m.read_bytes(s.x[1], std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size()));
            s.x[0] = self->create(text, s.x[4]);
        });
}

} // namespace alchemy::gui::network
