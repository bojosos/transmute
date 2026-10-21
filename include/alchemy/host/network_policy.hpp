#pragma once

// Process-wide network policy: ALCHEMY_NETWORK=loopback lets guest programs
// reach only this machine (the audit runs of Google Chrome and its updater
// must never talk to the Internet). The value is read once from the host
// environment, which every Alchemy child process inherits (children are
// created without an explicit environment block, and guest setenv/envp
// never reach the host environment), so it holds for the whole process tree.
//
// Values: "full" (default; no restriction) and "loopback". Any other value
// is treated as "loopback" (fail closed) with a one-time warning.
//
// With loopback, every path out of the machine is refused: BSD connect,
// sendto and sendmsg to a non-loopback address (ENETUNREACH; AF_UNIX is
// local), name resolution of anything but localhost (getaddrinfo,
// gethostbyname, libresolv, DNSServiceGetAddrInfo), Bonjour browsing,
// resolving and registration, NSURLSession/CFNetwork over WinHTTP
// (NSURLErrorNotConnectedToInternet) and PAC script downloads. bind, listen
// and loopback traffic stay allowed. Each refusal prints one stderr line:
//   alchemy: network denied (loopback-only): <api> <host or address>

// ALCHEMY_NETWORK_DENY=<entry>[,<entry>...] (commas or spaces) refuses the
// listed hosts in every mode, on top of ALCHEMY_NETWORK (full included), for
// runs that may use the network except for some hosts (an app's updater and
// crash reporter). It is read once from the host environment, as above. An
// entry is a host name ("update.googleapis.com", that name only),
// "*.<domain>" (every name under <domain>, not <domain> itself) or a numeric
// IPv4/IPv6 address. Case, a trailing dot and IPv6 brackets are ignored.
// Names are refused at the name gates: getaddrinfo, gethostbyname, libresolv,
// DNSServiceGetAddrInfo, NSURLSession/CFNetwork over WinHTTP and PAC script
// downloads (these fail as they do under loopback). A numeric entry is also
// refused by connect, sendto and sendmsg on any port (ENETUNREACH), and an
// IPv4 entry covers its IPv4-mapped IPv6 form. A program with its own DNS
// client (UDP queries to a server) passes the name gates: only numeric
// entries reach its connections. Each refusal prints one stderr line:
//   alchemy: network denied (deny list): <api> <host or address>

// Lives in the host layer's common part (src/alchemy/host/common/), above the
// backends, so every backend inherits it.

#include "alchemy/host/net.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace alchemy::network_policy {

enum class Mode { full, loopback };

[[nodiscard]] Mode mode();
[[nodiscard]] inline bool loopback_only() { return mode() == Mode::loopback; }

// Loopback addresses: 127.0.0.0/8, ::1 and IPv4-mapped ::ffff:127.0.0.0/104.
// A local (AF_UNIX) address counts as loopback.
[[nodiscard]] bool loopback_address(const host::net::Address& address);
// A host name or literal that stays on this machine: "localhost", names
// ending in ".localhost" (RFC 6761), and loopback IPv4/IPv6 literals (an
// IPv6 literal may be bracketed). A trailing dot is ignored.
[[nodiscard]] bool loopback_host(std::string_view host);
// Whether `host` is a numeric IPv4/IPv6 literal (resolving it needs no DNS).
[[nodiscard]] bool numeric_host(std::string_view host);
// The host of an absolute URL ("scheme://[user@]host[:port]/..."), lower-case.
[[nodiscard]] std::string url_host(std::string_view url);
// Text form of an address ("192.0.2.1:80", "[2001:db8::1]:443").
[[nodiscard]] std::string address_text(const host::net::Address& address);

// Prints the denial line for `api` and `target`.
void deny(std::string_view api, std::string_view target);

// ALCHEMY_NETWORK_DENY: whether a host name or literal, or an address (any
// port), is on the deny list.
[[nodiscard]] bool listed_host(std::string_view host);
[[nodiscard]] bool listed_address(const host::net::Address& address);
// The gates' form: true (refused, and the deny-list line printed for `api`
// and `target`, or `host` when `target` is empty) when the host or address is
// listed.
[[nodiscard]] bool refuse_listed_host(std::string_view api, std::string_view host, std::string_view target = {});
[[nodiscard]] bool refuse_listed_address(std::string_view api, const host::net::Address& address);

// ALCHEMY_NETWORK_LOG=1: each gate that lets traffic through also prints, once
// per distinct api and target in a process, for audits of what a run reached:
//   alchemy: network allowed: <api> <host or address>
[[nodiscard]] bool log_enabled();
void allowed(std::string_view api, std::string_view target);
inline void allowed(std::string_view api, const host::net::Address& address) {
    if (log_enabled()) allowed(api, address_text(address));
}

// Tests only: override the environment's values for this process (nullopt
// goes back to the environment's).
void set_mode_for_testing(std::optional<Mode> mode);
void set_deny_list_for_testing(std::optional<std::string_view> list);

} // namespace alchemy::network_policy
