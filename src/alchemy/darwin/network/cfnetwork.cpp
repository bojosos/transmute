#include "alchemy/darwin/network/http.hpp"

#include "alchemy/darwin/systemconfiguration/system_configuration.hpp"

#include <algorithm>

// CFNetwork: system proxy settings (the SystemConfiguration provider's
// view), proxy selection for a URL, and CFHTTPMessage (requests / responses
// built or parsed in memory). CFSocket is in socket.cpp; stream sockets
// (CFStreamCreatePairWith*, CFReadStream / CFWriteStream) are not provided
// (no audited binary imports them); the kCFStreamSSL* keys SecureTransport users import are exported by
// the Security provider.
namespace alchemy::gui::network {
namespace {

constexpr std::string_view lib = security::cfnetwork_library;

struct MessageState final : ObjectPayload {
    std::mutex mutex;
    bool request{};
    std::string method, version{"HTTP/1.1"}, reason;
    std::int64_t status{};
    Address url{};                // +1
    Headers headers;
    Bytes body, buffer;
    bool header_complete{};
};

std::string reason_for(std::int64_t status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "";
    }
}

bool wildcard_match(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) return text.empty();
    if (pattern[0] == '*') {
        for (std::size_t skip = 0; skip <= text.size(); ++skip)
            if (wildcard_match(pattern.substr(1), text.substr(skip))) return true;
        return false;
    }
    return !text.empty() && std::tolower(static_cast<unsigned char>(pattern[0])) == std::tolower(static_cast<unsigned char>(text[0])) &&
        wildcard_match(pattern.substr(1), text.substr(1));
}

} // namespace

void install_cfnetwork(const std::shared_ptr<Urls>& urls) {
    auto b = urls->b;
    const auto message_class = b->define_class("_AlchemyCFHTTPMessage", b->object_class, lib);
    b->register_type_id(message_class, security::TypeId::http_message);
    auto constants = std::make_shared<std::map<std::string, Address, std::less<>>>();
    const auto constant = [b, constants](std::string_view name, std::string_view value) {
        (*constants)[std::string(name)] = b->export_string(lib, "_" + std::string(name), value);
    };
    for (const auto* name : {"HTTPEnable", "HTTPProxy", "HTTPPort", "HTTPSEnable", "HTTPSProxy", "HTTPSPort", "SOCKSEnable", "SOCKSProxy",
                             "SOCKSPort", "FTPEnable", "FTPProxy", "FTPPort", "ExceptionsList", "ExcludeSimpleHostnames",
                             "ProxyAutoConfigEnable", "ProxyAutoConfigURLString", "ProxyAutoConfigJavaScript", "ProxyAutoDiscoveryEnable"})
        constant("kCFNetworkProxies" + std::string(name), name);
    for (const auto* name : {"kCFProxyTypeKey", "kCFProxyHostNameKey", "kCFProxyPortNumberKey", "kCFProxyAutoConfigurationURLKey",
                             "kCFProxyAutoConfigurationJavaScriptKey", "kCFProxyUsernameKey", "kCFProxyPasswordKey", "kCFProxyTypeNone",
                             "kCFProxyTypeHTTP", "kCFProxyTypeHTTPS", "kCFProxyTypeSOCKS", "kCFProxyTypeFTP",
                             "kCFProxyTypeAutoConfigurationURL", "kCFProxyTypeAutoConfigurationJavaScript"})
        constant(name, name);
    constant("kCFHTTPVersion1_0", "HTTP/1.0");
    constant("kCFHTTPVersion1_1", "HTTP/1.1");
    constant("kCFHTTPVersion2_0", "HTTP/2.0");
    constant("kCFHTTPVersion3_0", "HTTP/3.0");
    for (const auto* name : {"Basic", "Digest", "NTLM", "Kerberos", "Negotiate", "Negotiate2", "XMobileMeAuthToken"})
        constant("kCFHTTPAuthenticationScheme" + std::string(name), name);
    const auto key = [constants](std::string_view name) { return constants->at(std::string(name)); };

    // CFDictionaryRef CFNetworkCopySystemProxySettings(void): the SystemConfiguration dictionary.
    b->function(lib, "_CFNetworkCopySystemProxySettings", [b](CpuState& s, GuestMemory&) {
        s.x[0] = b->call(sysconfig::library, "_SCDynamicStoreCopyProxies", {0});
    });
    // CFArrayRef CFNetworkCopyProxiesForURL(CFURLRef url, CFDictionaryRef proxySettings)
    b->function(lib, "_CFNetworkCopyProxiesForURL", [urls, b, key](CpuState& s, GuestMemory&) {
        const auto parts = urls->parts(s.x[0]);
        const auto settings = s.x[1];
        const auto enabled = [&](std::string_view name) {
            const auto value = b->integer(b->lookup(settings, key("kCFNetworkProxies" + std::string(name))));
            return value && *value != 0;
        };
        std::vector<std::pair<Address, Address>> entry;
        std::vector<Address> owned;
        const auto none = [&] { entry = {{key("kCFProxyTypeKey"), key("kCFProxyTypeNone")}}; };
        const auto host = parts ? parts->host_name() : std::string{};
        bool bypass = !parts || host.empty();
        if (!bypass && enabled("ExcludeSimpleHostnames") && host.find('.') == std::string::npos) bypass = true;
        if (!bypass)
            if (const auto list = b->lookup(settings, key("kCFNetworkProxiesExceptionsList")); list && b->is_array(list))
                for (const auto pattern : b->cf.array_values(list))
                    if (const auto text = b->maybe_text(pattern); text && wildcard_match(*text, host)) bypass = true;
        const std::string prefix = parts && parts->scheme == "https" ? "HTTPS" : parts && parts->scheme == "ftp" ? "FTP" : "HTTP";
        if (bypass || !parts) none();
        else if (enabled(prefix + "Enable")) {
            const auto proxy = b->lookup(settings, key("kCFNetworkProxies" + prefix + "Proxy"));
            const auto port = b->lookup(settings, key("kCFNetworkProxies" + prefix + "Port"));
            entry = {{key("kCFProxyTypeKey"), key(prefix == "HTTPS" ? "kCFProxyTypeHTTPS" : prefix == "FTP" ? "kCFProxyTypeFTP" : "kCFProxyTypeHTTP")}};
            if (proxy) entry.emplace_back(key("kCFProxyHostNameKey"), proxy);
            if (port) entry.emplace_back(key("kCFProxyPortNumberKey"), port);
        } else if (enabled("ProxyAutoConfigEnable")) {
            entry = {{key("kCFProxyTypeKey"), key("kCFProxyTypeAutoConfigurationURL")}};
            if (const auto url_string = b->maybe_text(b->lookup(settings, key("kCFNetworkProxiesProxyAutoConfigURLString")))) {
                const auto url = urls->create(*url_string, 0);
                if (url) { owned.push_back(url); entry.emplace_back(key("kCFProxyAutoConfigurationURLKey"), url); }
            }
        } else none();
        std::vector<Address> values{b->dictionary(entry)};
        owned.push_back(values[0]);
        // A PAC entry is followed by a direct connection fallback, as on macOS.
        if (entry[0].second == key("kCFProxyTypeAutoConfigurationURL")) {
            values.push_back(b->dictionary({{key("kCFProxyTypeKey"), key("kCFProxyTypeNone")}}));
            owned.push_back(values.back());
        }
        s.x[0] = b->array(values);
        b->release_all(owned);
    });

    // ---- CFHTTPMessage ---------------------------------------------------------------------------------
    const auto message = [b](Address object) {
        const auto value = object ? b->objc.find_host_state<MessageState>(object) : nullptr;
        if (!value) security::fail("expected a CFHTTPMessageRef");
        return value;
    };
    const auto create = [b, message_class](std::shared_ptr<MessageState> state) {
        const auto object = b->objc.allocate(message_class);
        static_cast<void>(b->objc.host_state<MessageState>(object, [&] { return state; }));
        return object;
    };
    b->method(message_class, "dealloc", [b, message_class](CpuState& s, GuestMemory&) {
        if (const auto state = b->objc.find_host_state<MessageState>(s.x[0])) b->release(std::exchange(state->url, 0));
        static_cast<void>(b->objc.send_super(s.x[0], message_class, b->objc.selector("dealloc")));
    }, false, "v@:");
    b->function(lib, "_CFHTTPMessageGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(security::TypeId::http_message); });
    // (alloc, requestMethod, url, httpVersion)
    b->function(lib, "_CFHTTPMessageCreateRequest", [b, create](CpuState& s, GuestMemory&) {
        auto state = std::make_shared<MessageState>();
        state->request = true;
        state->method = b->text(s.x[1]);
        state->url = b->retain(s.x[2]);
        if (s.x[3]) state->version = b->text(s.x[3]);
        state->header_complete = true;
        s.x[0] = create(state);
    });
    // (alloc, statusCode, statusDescription, httpVersion)
    b->function(lib, "_CFHTTPMessageCreateResponse", [b, create](CpuState& s, GuestMemory&) {
        auto state = std::make_shared<MessageState>();
        state->status = static_cast<std::int64_t>(s.x[1]);
        state->reason = s.x[2] ? b->text(s.x[2]) : reason_for(state->status);
        if (s.x[3]) state->version = b->text(s.x[3]);
        state->header_complete = true;
        s.x[0] = create(state);
    });
    b->function(lib, "_CFHTTPMessageCreateEmpty", [create](CpuState& s, GuestMemory&) {
        auto state = std::make_shared<MessageState>();
        state->request = (s.x[1] & 0xff) != 0;
        s.x[0] = create(state);
    });
    b->function(lib, "_CFHTTPMessageIsRequest", [message](CpuState& s, GuestMemory&) { s.x[0] = message(s.x[0])->request; });
    b->function(lib, "_CFHTTPMessageIsHeaderComplete", [message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        std::lock_guard lock(m->mutex);
        s.x[0] = m->header_complete;
    });
    b->function(lib, "_CFHTTPMessageCopyRequestMethod", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        std::lock_guard lock(m->mutex);
        s.x[0] = m->request && !m->method.empty() ? b->string(m->method) : 0;
    });
    b->function(lib, "_CFHTTPMessageCopyRequestURL", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        std::lock_guard lock(m->mutex);
        s.x[0] = b->retain(m->url);
    });
    b->function(lib, "_CFHTTPMessageCopyVersion", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        std::lock_guard lock(m->mutex);
        s.x[0] = b->string(m->version);
    });
    b->function(lib, "_CFHTTPMessageGetResponseStatusCode", [message](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(message(s.x[0])->status);
    });
    b->function(lib, "_CFHTTPMessageCopyResponseStatusLine", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        std::lock_guard lock(m->mutex);
        s.x[0] = m->request ? 0 : b->string(m->version + " " + std::to_string(m->status) + (m->reason.empty() ? "" : " " + m->reason));
    });
    b->function(lib, "_CFHTTPMessageCopyHeaderFieldValue", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        const auto name = b->text(s.x[1]);
        std::lock_guard lock(m->mutex);
        const auto value = header_value(m->headers, name);
        s.x[0] = value ? b->string(*value) : 0;
    });
    b->function(lib, "_CFHTTPMessageCopyAllHeaderFields", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        {
            std::lock_guard lock(m->mutex);
            for (const auto& [name, value] : m->headers) {
                owned.push_back(b->string(name));
                owned.push_back(b->string(value));
                entries.emplace_back(owned[owned.size() - 2], owned.back());
            }
        }
        s.x[0] = b->dictionary(entries);
        b->release_all(owned);
    });
    b->function(lib, "_CFHTTPMessageSetHeaderFieldValue", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        const auto name = b->text(s.x[1]);
        const auto value = b->maybe_text(s.x[2]);
        std::lock_guard lock(m->mutex);
        std::erase_if(m->headers, [&](const auto& header) { return security::iequals(header.first, name); });
        if (value) m->headers.emplace_back(name, *value);
        s.x[0] = 0;
    });
    b->function(lib, "_CFHTTPMessageSetBody", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        auto body = s.x[1] ? b->bytes(s.x[1]) : Bytes{};
        std::lock_guard lock(m->mutex);
        m->body = std::move(body);
        s.x[0] = 0;
    });
    b->function(lib, "_CFHTTPMessageCopyBody", [b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        Bytes body;
        { std::lock_guard lock(m->mutex); body = m->body; }
        s.x[0] = body.empty() ? 0 : b->data(body);
    });
    b->function(lib, "_CFHTTPMessageCopySerializedMessage", [urls, b, message](CpuState& s, GuestMemory&) {
        const auto m = message(s.x[0]);
        std::string head;
        Bytes body;
        {
            std::lock_guard lock(m->mutex);
            if (!m->header_complete) { s.x[0] = 0; return; }
            if (m->request) {
                const auto parts = m->url ? urls->parts(m->url) : std::nullopt;
                head = m->method + " " + (parts ? parts->path_and_query() : std::string("/")) + " " + m->version + "\r\n";
                if (parts && !header_value(m->headers, "Host"))
                    head += "Host: " + parts->host + (parts->port ? ":" + std::to_string(*parts->port) : std::string{}) + "\r\n";
            } else {
                head = m->version + " " + std::to_string(m->status) + (m->reason.empty() ? "" : " " + m->reason) + "\r\n";
            }
            for (const auto& [name, value] : m->headers) head += name + ": " + value + "\r\n";
            head += "\r\n";
            body = m->body;
        }
        Bytes bytes(head.begin(), head.end());
        bytes.insert(bytes.end(), body.begin(), body.end());
        s.x[0] = b->data(bytes);
    });
    // Boolean CFHTTPMessageAppendBytes(CFHTTPMessageRef, const UInt8 *, CFIndex)
    b->function(lib, "_CFHTTPMessageAppendBytes", [urls, b, message](CpuState& s, GuestMemory& m) {
        const auto state = message(s.x[0]);
        if (static_cast<std::int64_t>(s.x[2]) < 0 || s.x[2] > (1ULL << 30)) { s.x[0] = 0; return; }
        Bytes bytes(static_cast<std::size_t>(s.x[2]));
        if (!bytes.empty()) m.read_bytes(s.x[1], bytes);
        std::lock_guard lock(state->mutex);
        if (state->header_complete) { state->body.insert(state->body.end(), bytes.begin(), bytes.end()); s.x[0] = 1; return; }
        state->buffer.insert(state->buffer.end(), bytes.begin(), bytes.end());
        const std::string_view text(reinterpret_cast<const char*>(state->buffer.data()), state->buffer.size());
        const auto end = text.find("\r\n\r\n");
        if (end == std::string_view::npos) { s.x[0] = 1; return; }
        std::vector<std::string> lines;
        for (std::size_t at = 0; at < end;) {
            auto next = text.find("\r\n", at);
            if (next == std::string_view::npos || next > end) next = end;
            lines.emplace_back(text.substr(at, next - at));
            at = next + 2;
        }
        if (lines.empty()) { s.x[0] = 0; return; }
        const auto& start = lines[0];
        const auto first = start.find(' '), second = start.find(' ', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos) { s.x[0] = 0; return; }
        if (state->request) {
            if (second == std::string::npos) { s.x[0] = 0; return; }
            state->method = start.substr(0, first);
            state->version = start.substr(second + 1);
            const auto target = start.substr(first + 1, second - first - 1);
            std::string host;
            for (std::size_t index = 1; index < lines.size(); ++index) {
                const auto colon = lines[index].find(':');
                if (colon != std::string::npos && security::iequals(lines[index].substr(0, colon), "Host")) {
                    host = lines[index].substr(colon + 1);
                    host.erase(0, host.find_first_not_of(" \t"));
                }
            }
            if (target.find("://") != std::string::npos) state->url = urls->create(target, 0);
            else if (!host.empty()) state->url = urls->create("http://" + host + target, 0);
        } else {
            state->version = start.substr(0, first);
            try { state->status = std::stoll(start.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1)); }
            catch (...) { s.x[0] = 0; return; }
            state->reason = second == std::string::npos ? std::string{} : start.substr(second + 1);
        }
        for (std::size_t index = 1; index < lines.size(); ++index) {
            const auto colon = lines[index].find(':');
            if (colon == std::string::npos) { s.x[0] = 0; return; }
            auto value = lines[index].substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            state->headers.emplace_back(lines[index].substr(0, colon), value);
        }
        state->body.assign(state->buffer.begin() + static_cast<std::ptrdiff_t>(end + 4), state->buffer.end());
        state->buffer.clear();
        state->header_complete = true;
        s.x[0] = 1;
    });
}

} // namespace alchemy::gui::network
