#include "alchemy/darwin/network/http.hpp"
#include "alchemy/host/network_policy.hpp"

#include <array>
#include <cctype>

// CFNetworkExecuteProxyAutoConfigurationURL: the proxy auto-configuration
// script at a URL is fetched and evaluated by the host's autoproxy
// (host::http::auto_proxy; WinHTTP's on Windows) on a guest dispatch worker. The returned run-loop source is a version-0
// CFRunLoopSource made through the guest's CFRunLoopSourceCreate; it is
// signalled when the evaluation ends, and its perform callback (on the run
// loop that runs it) invokes the client's callback once with the proxy list
// (kCFProxyTypeHTTP/HTTPS dictionaries in the script's order, or
// kCFProxyTypeNone for DIRECT) or an error in kCFErrorDomainCFNetwork
// (kCFErrorPACFileError). The host fetches http(s) script URLs only; other
// schemes report the error.
namespace alchemy::gui::network {
namespace {

constexpr std::string_view lib = security::cfnetwork_library;
constexpr std::string_view core = core_foundation_library;

struct Proxy { std::string host; int port{}; };

struct PacState final : ObjectPayload {
    std::mutex mutex;
    bool done{}, delivered{};
    std::optional<std::vector<Proxy>> proxies;   // nullopt: failed
    std::string error;
    bool https{};                                // the target URL is https
    Address callback{}, info{}, release_info{};
    Address source{};                            // not retained (the caller owns it)
};

// Evaluates the script for `target` (host only; blocking).
std::optional<std::vector<Proxy>> evaluate(const std::string& script_url, const std::string& target, std::string& error) {
    // ALCHEMY_NETWORK=loopback: a script on another host is never downloaded.
    if (network_policy::loopback_only() && !network_policy::loopback_host(network_policy::url_host(script_url))) {
        network_policy::deny("PAC", script_url);
        error = "the proxy auto-configuration script is not reachable (network access is limited to this machine)";
        return std::nullopt;
    }
    if (network_policy::refuse_listed_host("PAC", network_policy::url_host(script_url), script_url)) {   // ALCHEMY_NETWORK_DENY
        error = "the proxy auto-configuration script is not reachable (its host is on ALCHEMY_NETWORK_DENY)";
        return std::nullopt;
    }
    network_policy::allowed("PAC", script_url);
    const auto list = host::http::auto_proxy(script_url, target, 10000);
    if (!list) {
        error = "the proxy auto-configuration script could not be evaluated (" + std::string(host::name(list.error())) + ")";
        return std::nullopt;
    }
    std::vector<Proxy> result;
    // "host:port; [scheme=]host:port ..." in the script's order.
    const std::string& text = *list;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find_first_of("; \t", start);
        if (end == std::string::npos) end = text.size();
        auto item = text.substr(start, end - start);
        start = end + 1;
        if (item.empty()) continue;
        if (const auto equals = item.find('='); equals != std::string::npos) item = item.substr(equals + 1);
        if (const auto scheme = item.find("://"); scheme != std::string::npos) item = item.substr(scheme + 3);
        Proxy proxy{item, 80};
        if (const auto colon = item.rfind(':'); colon != std::string::npos && item.find(']', colon) == std::string::npos) {
            proxy.host = item.substr(0, colon);
            try { proxy.port = std::stoi(item.substr(colon + 1)); } catch (...) { continue; }
        }
        if (!proxy.host.empty()) result.push_back(std::move(proxy));
    }
    return result;
}

} // namespace

void install_pac(const std::shared_ptr<Urls>& urls) {
    auto b = urls->b;
    if (b->exported(lib, "_CFNetworkExecuteProxyAutoConfigurationURL")) return;
    const auto request_class = b->define_class("_AlchemyPACRequest", b->object_class, lib);
    const auto key = [b](std::string_view name) { return b->constant(lib, "_" + std::string(name)); };

    b->method(request_class, "dealloc", [b, request_class](CpuState& s, GuestMemory&) {
        if (const auto state = b->objc.find_host_state<PacState>(s.x[0])) {
            Address info{}, release_info{};
            {
                std::lock_guard lock(state->mutex);
                info = std::exchange(state->info, 0);
                release_info = state->release_info;
            }
            if (info && release_info) static_cast<void>(b->call_address(release_info, std::array{info}));
        }
        static_cast<void>(b->objc.send_super(s.x[0], request_class, b->objc.selector("dealloc")));
    }, false, "v@:");

    // The source's perform(info = the request object): deliver the result once.
    const auto perform = b->process.install_function("_alchemy_pac_perform", [b, key](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? b->objc.find_host_state<PacState>(s.x[0]) : nullptr;
        s.x[0] = 0;
        if (!state) return;
        std::optional<std::vector<Proxy>> proxies;
        std::string error;
        bool https;
        Address callback, info;
        {
            std::lock_guard lock(state->mutex);
            if (!state->done || state->delivered) return;
            state->delivered = true;
            proxies = state->proxies; error = state->error; https = state->https;
            callback = state->callback; info = state->info;
        }
        std::vector<Address> owned;
        Address list{}, failure{};
        if (proxies) {
            std::vector<Address> entries;
            if (proxies->empty()) entries.push_back(b->dictionary({{key("kCFProxyTypeKey"), key("kCFProxyTypeNone")}}));
            for (const auto& proxy : *proxies) {
                const auto host = b->string(proxy.host), port = b->number(proxy.port);
                entries.push_back(b->dictionary({{key("kCFProxyTypeKey"), key(https ? "kCFProxyTypeHTTPS" : "kCFProxyTypeHTTP")},
                                                 {key("kCFProxyHostNameKey"), host}, {key("kCFProxyPortNumberKey"), port}}));
                b->release(host); b->release(port);
            }
            list = b->array(entries);
            owned = entries;
            owned.push_back(list);
        } else {
            failure = b->error("kCFErrorDomainCFNetwork", 2, error);   // kCFErrorPACFileError
            owned.push_back(failure);
        }
        try {
            // void (*)(void *client, CFArrayRef proxyList, CFErrorRef error)
            if (callback) static_cast<void>(b->call_address(callback, std::array{info, list, failure}));
        } catch (...) { b->release_all(owned); throw; }
        b->release_all(owned);
    }, "alchemy.cfnetwork");

    // CFRunLoopSourceRef CFNetworkExecuteProxyAutoConfigurationURL(CFURLRef scriptURL, CFURLRef targetURL,
    //     CFProxyAutoConfigurationResultCallback cb, CFStreamClientContext *clientContext)
    b->function(lib, "_CFNetworkExecuteProxyAutoConfigurationURL", [urls, b, request_class, perform](CpuState& s, GuestMemory& m) {
        const auto script = urls->parts(s.x[0]);
        const auto target = urls->parts(s.x[1]);
        if (!script || !target || !s.x[2]) { s.x[0] = 0; return; }
        auto state = std::make_shared<PacState>();
        state->https = target->scheme == "https";
        state->callback = s.x[2];
        if (const auto context = s.x[3]) {
            if (m.read64(context) != 0) security::fail("unsupported CFStreamClientContext version");
            state->info = m.read64(context + 8);
            const auto retain_info = m.read64(context + 16);
            state->release_info = m.read64(context + 24);
            if (state->info && retain_info) state->info = b->call_address(retain_info, std::array{state->info});
        }
        const auto request = b->objc.allocate(request_class);
        static_cast<void>(b->objc.host_state<PacState>(request, [&] { return state; }));
        // CFRunLoopSourceContext: version, info, retain, release, copyDescription, equal, hash, schedule, cancel, perform.
        const auto context = b->process.allocate_guest(80);
        const std::array<Address, 10> fields{0, request, b->symbol(core, "_CFRetain"), b->symbol(core, "_CFRelease"), 0, 0, 0, 0, 0, perform};
        for (std::size_t index = 0; index < fields.size(); ++index) m.write64(context + index * 8, fields[index]);
        Address source{};
        try { source = b->call(core, "_CFRunLoopSourceCreate", {0, 0, context}); }
        catch (...) { b->process.release_guest(context); b->release(request); throw; }
        b->process.release_guest(context);
        b->release(request);   // the source holds it now
        state->source = source;
        const auto script_url = script->text(), target_url = target->text();
        // The worker keeps the source (and through it the request) alive until it has signalled.
        const auto held = b->retain(source);
        const auto worker = b->call(security::libsystem_library, "_dispatch_get_global_queue", {0, 0});
        b->dispatch_async(worker, [b, state, held, script_url, target_url] {
            std::string error;
            auto proxies = evaluate(script_url, target_url, error);
            {
                std::lock_guard lock(state->mutex);
                state->proxies = std::move(proxies);
                state->error = error;
                state->done = true;
            }
            static_cast<void>(b->call(core, "_CFRunLoopSourceSignal", {held}));
            b->release(held);
        });
        s.x[0] = source;
    });
}

} // namespace alchemy::gui::network
