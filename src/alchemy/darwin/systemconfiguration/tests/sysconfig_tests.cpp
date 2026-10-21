// SystemConfiguration provider tests (owner: A2): the Windows proxy string
// translation, and SCDynamicStoreCopyProxies / kSCPropNetProxies* as a guest
// (CPython's _scproxy) sees them through CoreFoundation.
#include "system_configuration.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Address = std::uint64_t;
using alchemy::gui::sysconfig::ProxyEndpoint;
using alchemy::gui::sysconfig::parse_windows_proxy;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

ProxyEndpoint at(std::string host, std::optional<std::uint16_t> port = {}) { return {std::move(host), port}; }

void test_parse() {
    auto none = parse_windows_proxy("", "");
    require(!none.http && !none.https && !none.ftp && !none.gopher && !none.socks && none.exceptions.empty() &&
            !none.exclude_simple_hostnames, "empty configuration enables a proxy");

    // One server for every protocol (WinINet: HTTP, HTTPS and FTP).
    auto single = parse_windows_proxy("proxy.example:8080", "<local>;*.example.com; 10.*;*.example.com");
    require(single.http == at("proxy.example", 8080) && single.https == single.http && single.ftp == single.http,
            "single server is not used for HTTP/HTTPS/FTP");
    require(!single.gopher && !single.socks, "single server enabled gopher or SOCKS");
    require(single.exclude_simple_hostnames, "<local> is not ExcludeSimpleHostnames");
    require(single.exceptions == std::vector<std::string>{"*.example.com", "10.*"}, "bypass list differs");

    // Per-protocol entries, separators and scheme prefixes.
    auto per = parse_windows_proxy("http=h1:80;HTTPS=https://h2:443/ ftp=h3\tsocks=s:1080;gopher=g:70", "<LOCAL>");
    require(per.http == at("h1", 80) && per.https == at("h2", 443) && per.ftp == at("h3") &&
            per.socks == at("s", 1080) && per.gopher == at("g", 70), "per-protocol entries differ");
    require(per.exclude_simple_hostnames && per.exceptions.empty(), "<LOCAL> is case-sensitive");

    // A schemeless entry fills only the protocols without their own entry.
    auto mixed = parse_windows_proxy("https=secure:443;fallback:3128", "");
    require(mixed.https == at("secure", 443) && mixed.http == at("fallback", 3128) && mixed.ftp == at("fallback", 3128),
            "schemeless fallback overrides a specific entry");

    // IPv6 literals.
    auto v6 = parse_windows_proxy("http=[fd00::1]:8080;https=fd00::2", "");
    require(v6.http == at("[fd00::1]", 8080) && v6.https == at("fd00::2"), "IPv6 proxies differ");

    // Entries that cannot be a macOS proxy entry are skipped.
    auto bad = parse_windows_proxy("http=h:99999;https=:80;ftp=h:0;unknown=u:1;socks=s:x1;gopher=[x", "");
    require(!bad.http && !bad.https && !bad.ftp && !bad.socks && !bad.gopher, "invalid entries were accepted");
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};

    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_system_configuration(context);
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing symbol " + std::string(name));
        return found->address;
    }
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return engine.invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    Address core(std::string_view name, std::initializer_list<Address> arguments = {}) {
        return call(alchemy::core_foundation_library, name, arguments);
    }
    Address key(std::string_view name) {
        return engine.read64(symbol(alchemy::gui::sysconfig::library, "_kSCPropNetProxies" + std::string(name)));
    }
    // CFDictionaryGetValue + CFNumberGetValue(kCFNumberSInt32Type), as _scproxy reads them.
    std::optional<std::int32_t> number(Address dictionary, std::string_view name) {
        const auto value = core("_CFDictionaryGetValue", {dictionary, key(name)});
        if (!value) return std::nullopt;
        const auto out = process.allocate_guest(8);
        engine.write32(out, 0x7fffffff);
        require(core("_CFNumberGetValue", {value, 3, out}) & 0xff, "CFNumberGetValue failed for " + std::string(name));
        const auto result = static_cast<std::int32_t>(engine.read32(out));
        process.release_guest(out);
        return result;
    }
    std::optional<std::string> text(Address dictionary, std::string_view name) {
        const auto value = core("_CFDictionaryGetValue", {dictionary, key(name)});
        if (!value) return std::nullopt;
        return cf.utf8(value);
    }
};

void check_proxies(Fixture& f, const alchemy::gui::sysconfig::ProxySettings& expected);

void test_guest(Fixture& f) {
    namespace sc = alchemy::gui::sysconfig;
    require(alchemy::ProcessContext::is_provider_library(sc::library), "SystemConfiguration is not a provider identity");
    // The fifteen symbols CPython 3.11's _scproxy imports.
    for (const auto name : {"ExceptionsList", "ExcludeSimpleHostnames", "FTPEnable", "FTPPort", "FTPProxy",
                            "GopherEnable", "GopherPort", "GopherProxy", "HTTPEnable", "HTTPPort", "HTTPProxy",
                            "HTTPSEnable", "HTTPSPort", "HTTPSProxy"}) {
        require(f.cf.utf8(f.key(name)) == name, std::string("kSCPropNetProxies") + name + " has the wrong value");
    }
    // The host's own WinHTTP answer, in the macOS layout.
    const auto actual = sc::current_proxy_settings();
    check_proxies(f, actual);
    std::cout << "proxies: http=" << (actual.http ? actual.http->host : "none")
              << " https=" << (actual.https ? actual.https->host : "none")
              << " exceptions=" << actual.exceptions.size()
              << " pac=" << (actual.auto_config_url.empty() ? "none" : actual.auto_config_url)
              << " autodetect=" << actual.auto_discovery << '\n';
    // A configured proxy (the machine's own settings are not changed by tests).
    auto configured = parse_windows_proxy("http=proxy.example:3128;https=secure.example:8443;ftp=ftp.example;socks=s:1080",
                                          "<local>;*.corp.example;10.*");
    configured.auto_config_url = "http://wpad.example/wpad.dat";
    configured.auto_discovery = true;
    sc::detail::settings_source = [configured] { return configured; };
    try { check_proxies(f, configured); } catch (...) { sc::detail::settings_source = nullptr; throw; }
    sc::detail::settings_source = nullptr;
}

void check_proxies(Fixture& f, const alchemy::gui::sysconfig::ProxySettings& expected) {
    namespace sc = alchemy::gui::sysconfig;
    const auto proxies = f.call(sc::library, "_SCDynamicStoreCopyProxies", {0});
    require(proxies != 0, "SCDynamicStoreCopyProxies returned NULL");
    require(f.number(proxies, "ExcludeSimpleHostnames") == (expected.exclude_simple_hostnames ? 1 : 0),
            "ExcludeSimpleHostnames differs");
    const auto list = f.core("_CFDictionaryGetValue", {proxies, f.key("ExceptionsList")});
    require(expected.exceptions.empty() ? list == 0 : f.core("_CFArrayGetCount", {list}) == expected.exceptions.size(),
            "ExceptionsList differs");
    for (std::size_t index = 0; list && index < expected.exceptions.size(); ++index)
        require(f.cf.utf8(f.core("_CFArrayGetValueAtIndex", {list, index})) == expected.exceptions[index],
                "ExceptionsList entry differs");
    const std::pair<std::string_view, const std::optional<sc::ProxyEndpoint>*> protocols[] = {
        {"HTTP", &expected.http}, {"HTTPS", &expected.https}, {"FTP", &expected.ftp}, {"Gopher", &expected.gopher},
        {"SOCKS", &expected.socks}};
    for (const auto& [prefix, value] : protocols) {
        const std::string name(prefix);
        require(f.number(proxies, name + "Enable") == (value->has_value() ? 1 : 0), name + "Enable differs");
        if (!value->has_value()) {
            require(!f.text(proxies, name + "Proxy") && !f.number(proxies, name + "Port"), name + " has a host while disabled");
            continue;
        }
        require(f.text(proxies, name + "Proxy") == (*value)->host, name + "Proxy differs");
        require(f.number(proxies, name + "Port") ==
                ((*value)->port ? std::optional<std::int32_t>(*(*value)->port) : std::nullopt), name + "Port differs");
    }
    require(f.number(proxies, "ProxyAutoDiscoveryEnable") == (expected.auto_discovery ? 1 : 0), "ProxyAutoDiscoveryEnable differs");
    require(f.number(proxies, "ProxyAutoConfigEnable") == (expected.auto_config_url.empty() ? 0 : 1), "ProxyAutoConfigEnable differs");
    require(f.text(proxies, "ProxyAutoConfigURLString") ==
            (expected.auto_config_url.empty() ? std::nullopt : std::optional<std::string>(expected.auto_config_url)),
            "ProxyAutoConfigURLString differs");
    // The caller owns the dictionary (Copy rule).
    static_cast<void>(f.core("_CFRelease", {proxies}));
}

} // namespace

int main() {
    try {
        test_parse();
        Fixture fixture;
        test_guest(fixture);
        std::cout << "Alchemy SystemConfiguration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
