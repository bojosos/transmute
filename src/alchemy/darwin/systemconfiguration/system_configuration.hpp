#pragma once

// SystemConfiguration provider (alchemy-fx GUI build). Owner: A2.
//
// Identity: /System/Library/Frameworks/SystemConfiguration.framework/
// Versions/A/SystemConfiguration (listed in darwin_framework_libraries).
// Exports the proxy part of the dynamic store that CPython's _scproxy
// (urllib.request.getproxies / proxy_bypass on darwin) imports:
// SCDynamicStoreCopyProxies and the kSCPropNetProxies* keys.
//
// SCDynamicStoreCopyProxies answers from the user's proxy settings
// (host::http::user_proxy: on Windows the configuration Settings > Network >
// Proxy and Internet Options write), translated to the macOS dictionary
// layout. When that configuration cannot be read, or names
// no proxy, the dictionary enables no proxy. Proxy auto-configuration (PAC
// URL, auto-detect) is reported in the ProxyAutoConfig* / ProxyAutoDiscovery
// keys only; nothing evaluates PAC scripts (urllib ignores them on macOS too).
// Other SystemConfiguration interfaces are not provided: their imports stay
// unresolved and fail at bind.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui {
struct InstallContext;
} // namespace alchemy::gui

namespace alchemy::gui::sysconfig {

// SCDynamicStoreCopyProxies and the kSCPropNetProxies* keys (install.cpp).
void install_proxies(InstallContext& context);

inline constexpr std::string_view library =
    "/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration";

struct ProxyEndpoint {
    std::string host;                    // as configured (IPv6 literals keep their brackets)
    std::optional<std::uint16_t> port;   // absent when the setting names none
    bool operator==(const ProxyEndpoint&) const = default;
};

struct ProxySettings {
    std::optional<ProxyEndpoint> http, https, ftp, gopher, socks;
    std::vector<std::string> exceptions;    // bypass patterns other than <local>
    bool exclude_simple_hostnames{};        // <local> in the bypass list
    bool auto_discovery{};                  // WPAD auto-detect
    std::string auto_config_url;            // PAC URL, empty when none
};

// Translates a proxy configuration in host::http's list syntax (the
// WinHTTP/WinINet one, UTF-8):
//   proxy  "host:port" (HTTP, HTTPS and FTP) or "scheme=host:port" entries
//          for http, https, ftp, gopher and socks, separated by ';' or
//          whitespace; an entry may carry a "scheme://" prefix;
//   bypass ';'/whitespace-separated patterns, "<local>" meaning hosts
//          without a period.
// Entries with an unknown scheme, an empty host or an invalid port are
// skipped (they cannot be expressed as a macOS proxy entry).
[[nodiscard]] ProxySettings parse_windows_proxy(std::string_view proxy, std::string_view bypass);

// The current user's configuration (host::http::user_proxy). Returns
// settings with no proxy when the configuration cannot be read.
[[nodiscard]] ProxySettings current_proxy_settings();

namespace detail {
// Test seam (sysconfig_tests.cpp): when set, SCDynamicStoreCopyProxies answers
// from this function instead of current_proxy_settings(). Never set by Alchemy.
extern std::function<ProxySettings()> settings_source;
} // namespace detail

} // namespace alchemy::gui::sysconfig
