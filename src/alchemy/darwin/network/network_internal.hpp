#pragma once

// Networking provider internals (owner: v-netsec, src/alchemy/network):
// network NSURLs, the WinHTTP exchange, URL loading (NSURLSession and
// friends) and CFNetwork. Not a contract header.

#include "alchemy/darwin/security/security_internal.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::network {

using security::Address;
using security::Bridge;
using security::Bytes;
using security::CpuState;
using security::GuestMemory;

inline constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";

// ---- URLs (url.cpp) ----------------------------------------------------------------------
// RFC 3986 absolute URL. Components keep their percent-encoding.
struct UrlParts {
    std::string scheme;                       // lower case
    bool has_authority{};
    std::optional<std::string> user, password;
    std::string host;                         // as written (IPv6 literals keep their brackets)
    std::optional<int> port;
    std::string path;
    std::optional<std::string> query, fragment;
    [[nodiscard]] std::string text() const;   // the composed absolute string
    [[nodiscard]] std::string host_name() const;   // host without IPv6 brackets, percent-decoded
    [[nodiscard]] int effective_port() const; // port or the scheme default (-1 when unknown)
    [[nodiscard]] std::string path_and_query() const;   // request target ("/" when empty)
};
// Percent-encodes characters that may not appear in a URL (macOS 14 NSURL
// behaviour), then parses. nullopt for a string without a scheme or with an
// invalid authority.
[[nodiscard]] std::optional<UrlParts> parse_url(std::string_view text);
// RFC 3986 section 5.2 reference resolution.
[[nodiscard]] std::optional<UrlParts> resolve_url(const UrlParts& base, std::string_view reference);
[[nodiscard]] std::string percent_decode(std::string_view text);

struct UrlState final : ObjectPayload {
    UrlParts parts;
    Address string{};                         // +1 CFString of parts.text()
};

class Urls {
public:
    explicit Urls(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)) {}
    std::shared_ptr<Bridge> b;
    Address url_class{}, network_class{};
    Address original_url_with_string{};       // NSURL's own +URLWithString: (file URLs)

    // +1 NSURL for an absolute or base-relative string (network or file
    // URL); 0 when the string is not a URL.
    Address create(std::string_view text, Address base);
    Address make(const UrlParts& parts);      // +1 network NSURL
    [[nodiscard]] std::shared_ptr<UrlState> state(Address object) const;   // nullptr unless a network URL
    // The absolute string of any NSURL (network or file).
    [[nodiscard]] std::string text(Address object);
    // Parts of any NSURL (file URLs as file://<path>).
    [[nodiscard]] std::optional<UrlParts> parts(Address object);
};

void install_urls(Urls& urls);

// ---- operation queues (operation_queue.cpp) -------------------------------------------------
// The dispatch queue behind an NSOperationQueue (0 when it has none).
[[nodiscard]] Address dispatch_queue_of(Bridge& b, Address operation_queue);
// +1 serial NSOperationQueue (NSURLSession's default delegate queue).
Address make_serial_operation_queue(Bridge& b, std::string_view name);
void install_operation_queues(Bridge& b);

// ---- URL loading (loading.cpp) / CFNetwork (cfnetwork.cpp) ------------------------------------
void install_url_loading(const std::shared_ptr<Urls>& urls);
void install_cfnetwork(const std::shared_ptr<Urls>& urls);
// NSHTTPCookie and NSHTTPCookieStorage cookie access over cookies.cpp (cookie_classes.cpp).
void install_cookies(const std::shared_ptr<Urls>& urls);
// CFNetworkExecuteProxyAutoConfigurationURL over WinHTTP autoproxy (pac.cpp).
void install_pac(const std::shared_ptr<Urls>& urls);
// SCDynamicStore / SCNetworkReachability over the Windows network state (sysconfig_store.cpp).
void install_system_configuration_store(const std::shared_ptr<Bridge>& b);
// libresolv res_9_n* DNS queries against the Windows DNS servers (resolver.cpp).
void install_resolver(const std::shared_ptr<Bridge>& b);
// CFSocket (socket.cpp); replaces the installer frameworks' explicit failures.
void install_sockets(const std::shared_ptr<Bridge>& b);

// ---- guest pump (pump.cpp) ---------------------------------------------------------------------
// Runs guest work posted from host threads (the mDNS callbacks) on one guest
// dispatch worker. The worker runs while a lease is held or work is queued.
class GuestPump : public std::enable_shared_from_this<GuestPump> {
public:
    explicit GuestPump(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)) {}
    // Keeps the worker running until the returned handle is dropped. Guest
    // thread only (it may start the worker).
    std::shared_ptr<void> lease();
    // Any thread. The work runs on the worker's guest thread, in order.
    void post(std::function<void()> work);
    std::shared_ptr<Bridge> b;

private:
    void run();
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::function<void()>> queue_;
    std::size_t leases_{};
    bool running_{};
};
[[nodiscard]] std::shared_ptr<GuestPump> guest_pump();   // created by install_network

// ---- Bonjour (dns_sd.cpp, net_service.cpp; host engine in mdns.hpp) -------------------------------
// libSystem's dns_sd DNSService* / TXTRecord* API (each export only when no other provider exports it).
void install_dns_sd(const std::shared_ptr<Bridge>& b);
// Foundation NSNetService / NSNetServiceBrowser (skipped when another provider defines them).
void install_net_services(const std::shared_ptr<Bridge>& b);

// NSURLError codes.
namespace url_error {
inline constexpr std::int64_t unknown = -1, cancelled = -999, bad_url = -1000, timed_out = -1001, unsupported_url = -1002,
    cannot_find_host = -1003, cannot_connect = -1004, connection_lost = -1005, too_many_redirects = -1007,
    resource_unavailable = -1008, not_connected = -1009, bad_server_response = -1011, user_cancelled_authentication = -1012,
    user_authentication_required = -1013, cannot_decode_content = -1016, secure_connection_failed = -1200,
    certificate_untrusted = -1202, client_certificate_required = -1206, cannot_create_file = -3000, cannot_write_file = -3003;
} // namespace url_error

} // namespace alchemy::gui::network
