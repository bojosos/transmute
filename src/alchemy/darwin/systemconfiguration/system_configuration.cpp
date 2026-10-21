#include "alchemy/darwin/systemconfiguration/system_configuration.hpp"

#include "alchemy/gui/install.hpp"
#include "alchemy/host/http.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

// SystemConfiguration proxy settings over the host's proxy configuration
// (host::http::user_proxy; on Windows the WinHTTP / Internet Options settings)
// (owner: A2). Key names and value types follow Apple's public
// SCSchemaDefinitions.h / SCDynamicStoreCopySpecific.h documentation; no
// Apple code is used.
namespace alchemy::gui {
namespace sysconfig {
namespace {

bool separator(char value) { return value == ';' || value == ' ' || value == '\t' || value == '\r' || value == '\n'; }

std::vector<std::string_view> tokens(std::string_view text) {
    std::vector<std::string_view> result;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && separator(text[at])) ++at;
        const auto begin = at;
        while (at < text.size() && !separator(text[at])) ++at;
        if (at > begin) result.push_back(text.substr(begin, at - begin));
    }
    return result;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& value : result) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    return result;
}

// "host", "host:port", "scheme://host:port/", "[v6]:port".
std::optional<ProxyEndpoint> endpoint(std::string_view text) {
    if (const auto scheme = text.find("://"); scheme != std::string_view::npos) text.remove_prefix(scheme + 3);
    if (const auto path = text.find('/'); path != std::string_view::npos) text = text.substr(0, path);
    ProxyEndpoint result;
    std::string_view port;
    if (text.starts_with('[')) {
        const auto close = text.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        result.host = std::string(text.substr(0, close + 1));
        const auto rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':')) return std::nullopt;
            port = rest.substr(1);
        }
    } else {
        const auto colon = text.rfind(':');
        // A bare IPv6 literal (several colons) has no port.
        if (colon != std::string_view::npos && text.find(':') == colon) {
            result.host = std::string(text.substr(0, colon));
            port = text.substr(colon + 1);
        } else {
            result.host = std::string(text);
        }
    }
    if (result.host.empty() || result.host == "[]") return std::nullopt;
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        const auto value = std::stoul(std::string(port));
        if (value == 0 || value > 65535) return std::nullopt;
        result.port = static_cast<std::uint16_t>(value);
    }
    return result;
}

} // namespace

ProxySettings parse_windows_proxy(std::string_view proxy, std::string_view bypass) {
    ProxySettings settings;
    std::optional<ProxyEndpoint> every;
    for (const auto entry : tokens(proxy)) {
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            // An entry without a scheme serves HTTP, HTTPS and FTP (the first one wins).
            if (!every) every = endpoint(entry);
            continue;
        }
        const auto scheme = lower(entry.substr(0, equals));
        auto value = endpoint(entry.substr(equals + 1));
        if (!value) continue;
        if (scheme == "http") { if (!settings.http) settings.http = std::move(value); }
        else if (scheme == "https") { if (!settings.https) settings.https = std::move(value); }
        else if (scheme == "ftp") { if (!settings.ftp) settings.ftp = std::move(value); }
        else if (scheme == "gopher") { if (!settings.gopher) settings.gopher = std::move(value); }
        else if (scheme == "socks") { if (!settings.socks) settings.socks = std::move(value); }
    }
    if (every) {
        if (!settings.http) settings.http = every;
        if (!settings.https) settings.https = every;
        if (!settings.ftp) settings.ftp = every;
    }
    for (const auto entry : tokens(bypass)) {
        if (lower(entry) == "<local>") settings.exclude_simple_hostnames = true;
        else if (std::find(settings.exceptions.begin(), settings.exceptions.end(), entry) == settings.exceptions.end())
            settings.exceptions.emplace_back(entry);
    }
    return settings;
}

namespace detail { std::function<ProxySettings()> settings_source; }

ProxySettings current_proxy_settings() {
    const auto config = host::http::user_proxy();
    if (!config) return {};
    auto settings = parse_windows_proxy(config->proxy, config->bypass);
    settings.auto_discovery = config->auto_discovery;
    settings.auto_config_url = config->auto_config_url;
    return settings;
}

} // namespace sysconfig

namespace {

// SCSchemaDefinitions.h: kSCPropNetProxies<name> = CFSTR("<name>").
// The first fourteen are the keys CPython's _scproxy imports.
constexpr std::string_view proxy_keys[] = {
    "ExceptionsList", "ExcludeSimpleHostnames",
    "FTPEnable", "FTPPort", "FTPProxy",
    "GopherEnable", "GopherPort", "GopherProxy",
    "HTTPEnable", "HTTPPort", "HTTPProxy",
    "HTTPSEnable", "HTTPSPort", "HTTPSProxy",
    "SOCKSEnable", "SOCKSPort", "SOCKSProxy",
    "ProxyAutoConfigEnable", "ProxyAutoConfigURLString", "ProxyAutoDiscoveryEnable"};

constexpr unsigned cf_number_sint32 = 3;   // kCFNumberSInt32Type

struct Provider {
    ProcessContext& process;
    CoreFoundation& cf;
    std::map<std::string, std::uint64_t, std::less<>> keys;   // key name -> immortal CFString

    std::uint64_t key(std::string_view name) const {
        const auto found = keys.find(name);
        if (found == keys.end()) throw std::logic_error("SystemConfiguration key not registered: " + std::string(name));
        return found->second;
    }

    // SCDynamicStoreCopyProxies: a +1 CFDictionary in the macOS layout.
    std::uint64_t copy_proxies() {
        const auto settings = sysconfig::detail::settings_source ? sysconfig::detail::settings_source()
                                                                 : sysconfig::current_proxy_settings();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;   // values are +1, released below
        const auto release_values = [&] { for (const auto& entry : entries) process.objects().release(entry.second); };
        try {
            const auto number = [&](std::string_view name, std::int64_t value) {
                entries.emplace_back(key(name), cf.integer(value, cf_number_sint32));
            };
            const auto text = [&](std::string_view name, std::string_view value) {
                entries.emplace_back(key(name), cf.string(value));
            };
            number("ExcludeSimpleHostnames", settings.exclude_simple_hostnames ? 1 : 0);
            if (!settings.exceptions.empty()) {
                std::vector<std::uint64_t> patterns;
                try {
                    for (const auto& pattern : settings.exceptions) patterns.push_back(cf.string(pattern));
                    entries.emplace_back(key("ExceptionsList"), cf.array(patterns));
                } catch (...) {
                    for (const auto value : patterns) process.objects().release(value);
                    throw;
                }
                for (const auto value : patterns) process.objects().release(value);
            }
            const std::pair<std::string_view, const std::optional<sysconfig::ProxyEndpoint>*> protocols[] = {
                {"HTTP", &settings.http}, {"HTTPS", &settings.https}, {"FTP", &settings.ftp},
                {"Gopher", &settings.gopher}, {"SOCKS", &settings.socks}};
            for (const auto& [prefix, value] : protocols) {
                const std::string name(prefix);
                number(name + "Enable", value->has_value() ? 1 : 0);
                if (!value->has_value()) continue;
                text(name + "Proxy", (*value)->host);
                if ((*value)->port) number(name + "Port", *(*value)->port);
            }
            number("ProxyAutoDiscoveryEnable", settings.auto_discovery ? 1 : 0);
            number("ProxyAutoConfigEnable", settings.auto_config_url.empty() ? 0 : 1);
            if (!settings.auto_config_url.empty()) text("ProxyAutoConfigURLString", settings.auto_config_url);
            const auto dictionary = cf.dictionary(entries);
            release_values();
            return dictionary;
        } catch (...) {
            release_values();
            throw;
        }
    }
};

} // namespace

// Registers the proxy exports. Creates only the key strings; WinHTTP is
// loaded when a guest first asks for the proxies.
void sysconfig::install_proxies(InstallContext& context) {
    auto provider = std::make_shared<Provider>(Provider{context.process, context.cf, {}});
    const std::string library(sysconfig::library);
    for (const auto name : proxy_keys) {
        const auto object = context.cf.constant_string(name);
        provider->keys.emplace(std::string(name), object);
        const auto cell = context.process.allocate_guest(8);
        context.process.main_engine().write64(cell, object);
        context.process.install_export(library, "_kSCPropNetProxies" + std::string(name), {cell, ExportKind::data});
    }
    // CFDictionaryRef SCDynamicStoreCopyProxies(SCDynamicStoreRef store).
    // Every store sees the same (per-user) proxy configuration.
    static_cast<void>(context.process.install_function("_SCDynamicStoreCopyProxies",
        [provider](transmute::CpuState& state, transmute::GuestMemory&) { state.x[0] = provider->copy_proxies(); },
        library));
}

} // namespace alchemy::gui
