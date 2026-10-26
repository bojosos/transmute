#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/darwin/network/mdns.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Network.framework connections, listeners, endpoints, parameters and Bonjour
// browsing (<Network/connection.h>, listener.h, endpoint.h, parameters.h,
// browser.h; network_framework.cpp has the path monitor and interfaces).
//
// - nw_endpoint: host (name and port), address (sockaddr), URL and Bonjour
//   service endpoints with their accessors.
// - nw_parameters: TCP (nw_parameters_create_secure_tcp) or UDP
//   (create_secure_udp), whether TLS is configured (any configure_tls block
//   but NW_PARAMETERS_DISABLE_PROTOCOL), the local endpoint, reuse of the
//   local address, and the interface preferences (kept). The configure blocks
//   run with the protocol's nw_protocol_options, whose settings are kept.
// - nw_connection over the guest libSystem's sockets, so the network policy
//   applies: start resolves and connects on a guest worker, then reports
//   preparing and ready (or failed with the POSIX error) to the state
//   handler on the connection's queue. receive / receive_message read on a
//   per-connection serial queue (a TCP receive waits for minimum bytes or the
//   end of the stream; a UDP receive_message takes one datagram); send writes
//   on another, a final context with is_complete shuts the write side. The
//   content is dispatch_data. cancel closes the socket; pending receives end
//   with ECANCELED.
// - nw_listener: TCP only. start binds (port 0 picks one) and listens;
//   accepted connections reach the new-connection handler as connections that
//   are already connected (start reports ready). nw_listener_get_port answers
//   the bound port.
// - nw_browser over the host mDNS engine (mdns.hpp): found and lost services
//   reach the browse-results handler as nw_browse_result changes with Bonjour
//   endpoints, after the state handler sees ready.
// - Unavailable, failing explicitly: TLS (a connection whose parameters
//   configure TLS fails with an nw_error_domain_tls error), UDP listeners
//   (failed with EOPNOTSUPP), and other browse descriptors than Bonjour.
namespace alchemy::gui::network {
namespace {

using security::Bridge;
constexpr std::string_view framework = "/System/Library/Frameworks/Network.framework/Versions/A/Network";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// <Network/*.h> enumerations.
enum : std::uint64_t { endpoint_invalid = 0, endpoint_address = 1, endpoint_host = 2, endpoint_bonjour = 3, endpoint_url = 4 };
enum : std::uint64_t { connection_invalid = 0, connection_waiting = 1, connection_preparing = 2, connection_ready = 3,
                       connection_failed = 4, connection_cancelled = 5 };
enum : std::uint64_t { listener_invalid = 0, listener_waiting = 1, listener_ready = 2, listener_failed = 3, listener_cancelled = 4 };
enum : std::uint64_t { browser_invalid = 0, browser_ready = 1, browser_failed = 2, browser_cancelled = 3 };
enum : std::uint64_t { error_domain_posix = 1, error_domain_dns = 2, error_domain_tls = 3 };
enum : std::uint64_t { change_identical = 1 << 1, change_result_added = 1 << 2, change_result_removed = 1 << 3 };
constexpr std::int64_t econnrefused = 61, ecanceled = 89, eopnotsupp = 102, ehostunreach = 65, einval = 22;
constexpr std::int64_t err_ssl_internal = -9810;   // errSSLInternal (SecureTransport.h)

struct Endpoint final : ObjectPayload {
    std::uint64_t type{};
    std::string host, url, name, service, domain;   // host/url/bonjour
    std::uint16_t port{};                           // host byte order
    std::vector<std::uint8_t> address;              // Darwin sockaddr bytes
    std::vector<Address> strings;                   // guest C strings handed out (freed with the endpoint)
};
struct Options final : ObjectPayload {
    enum class Kind { tcp, udp, tls, other } kind{Kind::other};
    std::map<std::string, std::uint64_t> settings;
};
struct Parameters final : ObjectPayload {
    std::mutex mutex;
    bool udp{}, tls{}, reuse_address{}, peer_to_peer{};
    Address local{};                                // +1 nw_endpoint
    std::vector<std::uint64_t> prohibited;          // nw_interface_type
    std::uint64_t required{};
    Address transport{}, tls_options{};             // +1 nw_protocol_options
};
struct Context final : ObjectPayload { std::string identifier; bool final{}; bool constant{}; };
struct Error final : ObjectPayload { std::uint64_t domain{}; std::int64_t code{}; };

struct Connection final : ObjectPayload {
    std::mutex mutex;
    Address endpoint{}, parameters{}, queue{}, state_handler{};
    Address reader{}, writer{};                      // +1 serial dispatch queues
    std::int64_t fd{-1};
    std::uint64_t state{connection_invalid};
    bool connected{};                               // accepted by a listener
    bool udp{};
};
struct Listener final : ObjectPayload {
    std::mutex mutex;
    Address parameters{}, queue{}, state_handler{}, connection_handler{}, acceptor{};
    std::int64_t fd{-1};
    std::uint16_t port{};
    std::uint64_t state{listener_invalid};
};
struct Browser final : ObjectPayload {
    std::mutex mutex;
    Address descriptor{}, parameters{}, queue{}, state_handler{}, results_handler{};
    std::unique_ptr<mdns::Operation> operation;
    std::vector<std::pair<std::string, std::uint32_t>> services;   // name, interface
    bool started{}, cancelled{};
};
struct BrowseResult final : ObjectPayload { Address endpoint{}; std::uint64_t changes{}; };
struct Descriptor final : ObjectPayload { std::string type, domain; };

class Network : public std::enable_shared_from_this<Network> {
public:
    explicit Network(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)), pump(std::make_shared<GuestPump>(b)) {}
    std::shared_ptr<Bridge> b;
    std::shared_ptr<GuestPump> pump;
    Address endpoint_class{}, options_class{}, parameters_class{}, context_class{}, error_class{}, connection_class{},
        listener_class{}, browser_class{}, descriptor_class{}, result_class{}, stack_class{};
    Address final_context{}, default_message{}, default_stream{}, idempotent{}, disable_protocol{}, default_configuration{};

    template <class T>
    std::shared_ptr<T> state(Address object, const char* what) {
        auto value = object ? b->objc.find_host_state<T>(object) : nullptr;
        if (!value) throw std::runtime_error(std::string("Alchemy Network.framework: not an ") + what);
        return value;
    }
    template <class T>
    Address make(Address cls, std::shared_ptr<T> value) {
        const auto object = b->objc.allocate(cls);
        static_cast<void>(b->objc.host_state<T>(object, [&] { return value; }));
        return object;
    }
    GuestMemory& memory() { return b->memory(); }
    Address sys(std::string_view name, std::initializer_list<Address> arguments) { return b->call(libsystem, name, arguments); }
    std::int64_t guest_errno() {
        const auto location = b->call(libsystem, "___error", {});
        return location ? static_cast<std::int32_t>(memory().read32(location)) : einval;
    }
    Address c_string(std::string_view text) {   // guest malloc
        const auto address = sys("_malloc", {text.size() + 1});
        memory().write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        memory().write8(address + text.size(), 0);
        return address;
    }
    std::string read_text(Address address, std::size_t limit = 4096) {
        std::string value;
        for (std::size_t index = 0; address && index < limit; ++index) {
            const auto c = static_cast<char>(memory().read8(address + index));
            if (!c) break;
            value += c;
        }
        return value;
    }
    Address error(std::uint64_t domain, std::int64_t code) {   // +1
        auto value = std::make_shared<Error>();
        value->domain = domain;
        value->code = code;
        return make(error_class, value);
    }
    void retain_queue(Address queue) { if (queue) static_cast<void>(sys("_dispatch_retain", {queue})); }
    void release_queue(Address queue) { if (queue) static_cast<void>(sys("_dispatch_release", {queue})); }
    // Replaces a copied block slot; the previous block is released.
    void set_block(std::mutex& mutex, Address& slot, Address block) {
        const auto copied = block ? b->block_copy(block) : 0;
        Address previous{};
        { std::lock_guard lock(mutex); previous = std::exchange(slot, copied); }
        if (previous) b->block_release(previous);
    }
    void set_queue(std::mutex& mutex, Address& slot, Address queue) {
        retain_queue(queue);
        Address previous{};
        { std::lock_guard lock(mutex); previous = std::exchange(slot, queue); }
        release_queue(previous);
    }
    // Runs `block(arguments)` on `queue` (a copy is held while it waits); the arguments are released after.
    // `dispatched` are dispatch objects (dispatch_data), released with dispatch_release.
    void deliver(Address queue, Address block, std::vector<Address> arguments, std::vector<Address> owned = {}, std::vector<Address> dispatched = {}) {
        if (!queue || !block) { b->release_all(owned); for (const auto item : dispatched) release_queue(item); return; }
        const auto held = b->block_copy(block);
        b->dispatch_async(queue, [this, held, arguments, owned, dispatched] {
            switch (arguments.size()) {
            case 0: static_cast<void>(b->invoke_block(held, {})); break;
            case 1: static_cast<void>(b->invoke_block(held, {arguments[0]})); break;
            case 2: static_cast<void>(b->invoke_block(held, {arguments[0], arguments[1]})); break;
            case 3: static_cast<void>(b->invoke_block(held, {arguments[0], arguments[1], arguments[2]})); break;
            default: static_cast<void>(b->invoke_block(held, {arguments[0], arguments[1], arguments[2], arguments[3]})); break;
            }
            b->block_release(held);
            b->release_all(owned);
            for (const auto item : dispatched) release_queue(item);
        });
    }
    // Background work on a global queue (a guest worker).
    void background(std::function<void()> work) {
        b->dispatch_async(sys("_dispatch_get_global_queue", {0, 0}), std::move(work));
    }

    // ---- endpoints ----
    Address host_endpoint(std::string host, std::uint16_t port) {   // +1
        auto value = std::make_shared<Endpoint>();
        value->type = endpoint_host;
        value->host = std::move(host);
        value->port = port;
        return make(endpoint_class, value);
    }
    Address address_endpoint(std::span<const std::uint8_t> sockaddr) {   // +1
        auto value = std::make_shared<Endpoint>();
        value->type = endpoint_address;
        value->address.assign(sockaddr.begin(), sockaddr.end());
        if (value->address.size() >= 4) value->port = static_cast<std::uint16_t>((value->address[2] << 8) | value->address[3]);
        return make(endpoint_class, value);
    }
    // The numeric text of a sockaddr (getnameinfo NI_NUMERICHOST).
    std::string address_text(const std::vector<std::uint8_t>& address) {
        if (address.size() < 2) return {};
        const auto block = sys("_malloc", {address.size()});
        memory().write_bytes(block, address);
        const auto text = sys("_malloc", {64});
        const auto status = sys("_getnameinfo", {block, address[0], text, 64, 0, 0, 2});
        const auto value = status == 0 ? read_text(text, 64) : std::string{};
        sys("_free", {block});
        sys("_free", {text});
        return value;
    }
    Address handed_out(const std::shared_ptr<Endpoint>& value, std::string_view text) {   // a C string the endpoint keeps
        const auto address = c_string(text);
        value->strings.push_back(address);
        return address;
    }

    // ---- connections ----
    void report(Address connection, const std::shared_ptr<Connection>& value, std::uint64_t state, Address error = 0) {
        Address queue{}, handler{};
        {
            std::lock_guard lock(value->mutex);
            if (value->state == connection_cancelled) { if (error) b->release(error); return; }
            value->state = state;
            queue = value->queue;
            handler = value->state_handler;
        }
        if (!queue || !handler) { if (error) b->release(error); return; }
        b->retain(connection);
        std::vector<Address> owned{connection};
        if (error) owned.push_back(error);
        deliver(queue, handler, {state, error}, owned);
    }
    // Resolve and connect on a worker; ready, or failed with the POSIX error.
    void connect(Address connection, std::shared_ptr<Connection> value) {
        const auto endpoint = state<Endpoint>(value->endpoint, "nw_endpoint_t");
        std::int64_t fd = -1, code = ehostunreach;
        const auto type = value->udp ? 2 : 1;   // SOCK_DGRAM / SOCK_STREAM
        if (endpoint->type == endpoint_address) {
            const Address family = endpoint->address.size() > 1 ? endpoint->address[1] : 0;
            fd = static_cast<std::int32_t>(sys("_socket", {family, static_cast<Address>(type), Address{0}}));
            if (fd < 0) code = guest_errno();
            else {
                const auto block = sys("_malloc", {endpoint->address.size()});
                memory().write_bytes(block, endpoint->address);
                if (static_cast<std::int32_t>(sys("_connect", {static_cast<Address>(fd), block, endpoint->address.size()})) == 0) code = 0;
                else { code = guest_errno(); sys("_close", {static_cast<Address>(fd)}); fd = -1; }
                sys("_free", {block});
            }
        } else if (endpoint->type == endpoint_host) {
            const auto host = c_string(endpoint->host), port = c_string(std::to_string(endpoint->port));
            const auto hints = sys("_calloc", {1, 48}), result = sys("_calloc", {1, 8});
            memory().write32(hints + 8, static_cast<std::uint32_t>(type));
            if (sys("_getaddrinfo", {host, port, hints, result}) == 0) {
                const auto list = memory().read64(result);
                code = econnrefused;
                for (auto info = list; info; info = memory().read64(info + 40)) {
                    const auto family = memory().read32(info + 4), socktype = memory().read32(info + 8), protocol = memory().read32(info + 12),
                               length = memory().read32(info + 16);
                    fd = static_cast<std::int32_t>(sys("_socket", {family, socktype, protocol}));
                    if (fd < 0) { code = guest_errno(); continue; }
                    if (static_cast<std::int32_t>(sys("_connect", {static_cast<Address>(fd), memory().read64(info + 32), length})) == 0) { code = 0; break; }
                    code = guest_errno();
                    sys("_close", {static_cast<Address>(fd)});
                    fd = -1;
                }
                sys("_freeaddrinfo", {list});
            }
            for (const auto block : {host, port, hints, result}) sys("_free", {block});
        } else code = einval;
        bool cancelled{};
        {
            std::lock_guard lock(value->mutex);
            cancelled = value->state == connection_cancelled;
            if (!cancelled) value->fd = fd;
        }
        if (cancelled) { if (fd >= 0) sys("_close", {static_cast<Address>(fd)}); b->release(connection); return; }
        if (code == 0) report(connection, value, connection_ready);
        else report(connection, value, connection_failed, error(error_domain_posix, code));
        b->release(connection);
    }
    std::int64_t fd_of(const std::shared_ptr<Connection>& value) {
        std::lock_guard lock(value->mutex);
        return value->fd;
    }
    // Bytes of a dispatch_data (dispatch_data_create_map).
    std::vector<std::uint8_t> bytes_of(Address data) {
        if (!data) return {};
        const auto slots = sys("_calloc", {2, 8});
        const auto map = sys("_dispatch_data_create_map", {data, slots, slots + 8});
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(memory().read64(slots + 8)));
        if (!bytes.empty()) memory().read_bytes(memory().read64(slots), bytes);
        if (map) release_queue(map);
        sys("_free", {slots});
        return bytes;
    }
    Address dispatch_data(std::span<const std::uint8_t> bytes) {   // +1 (copied: DISPATCH_DATA_DESTRUCTOR_DEFAULT)
        if (bytes.empty()) return 0;
        const auto block = sys("_malloc", {bytes.size()});
        memory().write_bytes(block, bytes);
        const auto data = sys("_dispatch_data_create", {block, bytes.size(), 0, 0});
        sys("_free", {block});
        return data;
    }
    void install();
};

std::shared_ptr<Network> network_instance;

} // namespace

void install_network_connections(std::shared_ptr<Bridge> bridge) {
    auto self = std::make_shared<Network>(std::move(bridge));
    network_instance = self;
    self->install();
}

namespace {

void Network::install() {
    const auto self = shared_from_this();
    auto& objc = b->objc;
    const auto cls = [&](const char* name) {
        if (const auto found = objc.find_class(name)) return found;
        return b->define_class(name, objc.find_class("NSObject"), framework);
    };
    endpoint_class = cls("OS_nw_endpoint");
    options_class = cls("OS_nw_protocol_options");
    parameters_class = cls("OS_nw_parameters");
    stack_class = cls("OS_nw_protocol_stack");
    context_class = cls("OS_nw_content_context");
    error_class = cls("OS_nw_error");
    connection_class = cls("OS_nw_connection");
    listener_class = cls("OS_nw_listener");
    browser_class = cls("OS_nw_browser");
    descriptor_class = cls("OS_nw_browse_descriptor");
    result_class = cls("OS_nw_browse_result");
    const auto fn = [this](const char* name, transmute::GuestCallHandler handler) { b->function(framework, name, std::move(handler)); };
    const auto dealloc = [this](Address target, std::function<void(Address)> drop) {
        b->method(target, "dealloc", [this, target, drop](CpuState& s, GuestMemory&) {
            drop(s.x[0]);
            static_cast<void>(b->objc.send_super(s.x[0], target, b->objc.selector("dealloc")));
        }, false, "v@:");
    };

    // ---- errors ----
    // NULL (no error) answers 0; codes are int (sign-extended).
    fn("_nw_error_get_error_code", [self](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? self->b->objc.find_host_state<Error>(s.x[0]) : nullptr;
        s.x[0] = value ? static_cast<Address>(static_cast<std::int64_t>(static_cast<std::int32_t>(value->code))) : 0;
    });
    fn("_nw_error_get_error_domain", [self](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? self->b->objc.find_host_state<Error>(s.x[0]) : nullptr;
        s.x[0] = value ? value->domain : 0;
    });

    // ---- endpoints ----
    dealloc(endpoint_class, [self](Address object) {
        if (const auto value = self->b->objc.find_host_state<Endpoint>(object))
            for (const auto text : std::exchange(value->strings, {})) self->sys("_free", {text});
    });
    // nw_endpoint_t nw_endpoint_create_host(const char* hostname, const char* port)
    fn("_nw_endpoint_create_host", [self](CpuState& s, GuestMemory&) {
        const auto port = self->read_text(s.x[1], 16);
        s.x[0] = self->host_endpoint(self->read_text(s.x[0]), static_cast<std::uint16_t>(std::strtoul(port.c_str(), nullptr, 10)));
    });
    fn("_nw_endpoint_create_address", [self](CpuState& s, GuestMemory& m) {
        const auto length = s.x[0] ? m.read8(s.x[0]) : 0;
        std::vector<std::uint8_t> bytes(length);
        if (length) m.read_bytes(s.x[0], bytes);
        s.x[0] = length >= 16 ? self->address_endpoint(bytes) : 0;
    });
    fn("_nw_endpoint_create_url", [self](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<Endpoint>();
        value->type = endpoint_url;
        value->url = self->read_text(s.x[0]);
        const auto scheme = value->url.find("://");
        if (scheme != std::string::npos) {
            const auto start = scheme + 3, end = value->url.find_first_of("/?#", start);
            auto authority = value->url.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (const auto at = authority.rfind('@'); at != std::string::npos) authority.erase(0, at + 1);
            const auto colon = authority.rfind(':');
            const bool bracketed = !authority.empty() && authority.front() == '[';
            if (colon != std::string::npos && (!bracketed || authority[colon - 1] == ']')) {
                value->port = static_cast<std::uint16_t>(std::strtoul(authority.c_str() + colon + 1, nullptr, 10));
                authority.resize(colon);
            } else value->port = value->url.starts_with("https") ? 443 : value->url.starts_with("http") ? 80 : 0;
            if (bracketed && authority.size() >= 2) authority = authority.substr(1, authority.size() - 2);
            value->host = authority;
        }
        s.x[0] = self->make(self->endpoint_class, value);
    });
    fn("_nw_endpoint_create_bonjour_service", [self](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<Endpoint>();
        value->type = endpoint_bonjour;
        value->name = self->read_text(s.x[0]);
        value->service = self->read_text(s.x[1]);
        value->domain = s.x[2] ? self->read_text(s.x[2]) : std::string("local.");
        s.x[0] = self->make(self->endpoint_class, value);
    });
    fn("_nw_endpoint_get_type", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<Endpoint>(s.x[0], "nw_endpoint_t")->type; });
    fn("_nw_endpoint_get_hostname", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Endpoint>(s.x[0], "nw_endpoint_t");
        s.x[0] = value->type == endpoint_host || value->type == endpoint_url ? self->handed_out(value, value->host) : 0;
    });
    fn("_nw_endpoint_get_port", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<Endpoint>(s.x[0], "nw_endpoint_t")->port; });
    fn("_nw_endpoint_copy_port_string", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->c_string(std::to_string(self->state<Endpoint>(s.x[0], "nw_endpoint_t")->port));
    });
    fn("_nw_endpoint_get_address", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Endpoint>(s.x[0], "nw_endpoint_t");
        if (value->type != endpoint_address) { s.x[0] = 0; return; }
        const auto address = self->sys("_malloc", {value->address.size()});
        self->memory().write_bytes(address, value->address);
        value->strings.push_back(address);
        s.x[0] = address;
    });
    fn("_nw_endpoint_copy_address_string", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Endpoint>(s.x[0], "nw_endpoint_t");
        s.x[0] = value->type == endpoint_address ? self->c_string(self->address_text(value->address)) : 0;
    });
    fn("_nw_endpoint_get_url", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Endpoint>(s.x[0], "nw_endpoint_t");
        s.x[0] = value->type == endpoint_url ? self->handed_out(value, value->url) : 0;
    });
    for (const auto& [name, member] : std::initializer_list<std::pair<const char*, std::string Endpoint::*>>{
             {"_nw_endpoint_get_bonjour_service_name", &Endpoint::name}, {"_nw_endpoint_get_bonjour_service_type", &Endpoint::service},
             {"_nw_endpoint_get_bonjour_service_domain", &Endpoint::domain}}) {
        const auto field = member;
        fn(name, [self, field](CpuState& s, GuestMemory&) {
            const auto value = s.x[0] ? self->b->objc.find_host_state<Endpoint>(s.x[0]) : nullptr;
            s.x[0] = value && value->type == endpoint_bonjour ? self->handed_out(value, (*value).*field) : 0;
        });
    }

    // ---- protocol options and parameters ----
    const auto options = [self](Options::Kind kind) {
        auto value = std::make_shared<Options>();
        value->kind = kind;
        return self->make(self->options_class, value);
    };
    fn("_nw_tcp_create_options", [options](CpuState& s, GuestMemory&) { s.x[0] = options(Options::Kind::tcp); });
    fn("_nw_udp_create_options", [options](CpuState& s, GuestMemory&) { s.x[0] = options(Options::Kind::udp); });
    fn("_nw_tls_create_options", [options](CpuState& s, GuestMemory&) { s.x[0] = options(Options::Kind::tls); });
    // TCP and UDP option setters: values kept (the socket's own defaults apply).
    for (const auto* name : {"no_delay", "no_push", "no_options", "enable_keepalive", "keepalive_count", "keepalive_idle_time",
                             "keepalive_interval", "maximum_segment_size", "connection_timeout", "persist_timeout",
                             "retransmit_connection_drop_time", "retransmit_fin_drop", "disable_ack_stretching", "enable_fast_open",
                             "disable_ecn"}) {
        const std::string key = name;
        fn(("_nw_tcp_options_set_" + key).c_str(), [self, key](CpuState& s, GuestMemory&) {
            self->state<Options>(s.x[0], "nw_protocol_options_t")->settings[key] = s.x[1];
        });
    }
    fn("_nw_udp_options_set_prefer_no_checksum", [self](CpuState& s, GuestMemory&) {
        self->state<Options>(s.x[0], "nw_protocol_options_t")->settings["prefer_no_checksum"] = s.x[1];
    });
    fn("_nw_protocol_options_is_tls", [self](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? self->b->objc.find_host_state<Options>(s.x[0]) : nullptr;
        s.x[0] = value && value->kind == Options::Kind::tls;
    });
    // NW_PARAMETERS_DEFAULT_CONFIGURATION / NW_PARAMETERS_DISABLE_PROTOCOL: global blocks.
    default_configuration = b->make_block([](CpuState&, GuestMemory&) {});
    disable_protocol = b->make_block([](CpuState&, GuestMemory&) {});
    b->export_value(framework, "__nw_parameters_configure_protocol_default_configuration", default_configuration);
    b->export_value(framework, "__nw_parameters_configure_protocol_disable", disable_protocol);
    dealloc(parameters_class, [self](Address object) {
        if (const auto value = self->b->objc.find_host_state<Parameters>(object)) {
            std::vector<Address> held;
            { std::lock_guard lock(value->mutex); held = {std::exchange(value->local, 0), std::exchange(value->transport, 0), std::exchange(value->tls_options, 0)}; }
            for (const auto item : held) if (item) self->b->release(item);
        }
    });
    // nw_parameters_create_secure_tcp(configure_tls, configure_tcp) / _udp(configure_dtls, configure_udp): the
    // configure blocks run with the protocols' options (not with DEFAULT_CONFIGURATION or DISABLE_PROTOCOL).
    const auto create_parameters = [self, options](bool udp, Address configure_tls, Address configure_transport) {
        auto value = std::make_shared<Parameters>();
        value->udp = udp;
        value->transport = options(udp ? Options::Kind::udp : Options::Kind::tcp);
        if (configure_tls && configure_tls != self->disable_protocol) {
            value->tls = true;
            value->tls_options = options(Options::Kind::tls);
            if (configure_tls != self->default_configuration) static_cast<void>(self->b->invoke_block(configure_tls, {value->tls_options}));
        }
        if (configure_transport && configure_transport != self->disable_protocol && configure_transport != self->default_configuration)
            static_cast<void>(self->b->invoke_block(configure_transport, {value->transport}));
        return self->make(self->parameters_class, value);
    };
    fn("_nw_parameters_create_secure_tcp", [create_parameters](CpuState& s, GuestMemory&) { s.x[0] = create_parameters(false, s.x[0], s.x[1]); });
    fn("_nw_parameters_create_secure_udp", [create_parameters](CpuState& s, GuestMemory&) { s.x[0] = create_parameters(true, s.x[0], s.x[1]); });
    fn("_nw_parameters_create", [create_parameters](CpuState& s, GuestMemory&) { s.x[0] = create_parameters(false, 0, 0); });
    fn("_nw_parameters_copy", [self](CpuState& s, GuestMemory&) {
        const auto from = self->state<Parameters>(s.x[0], "nw_parameters_t");
        auto value = std::make_shared<Parameters>();
        {
            std::lock_guard lock(from->mutex);
            value->udp = from->udp; value->tls = from->tls; value->reuse_address = from->reuse_address; value->peer_to_peer = from->peer_to_peer;
            value->prohibited = from->prohibited; value->required = from->required;
            value->local = from->local ? self->b->retain(from->local) : 0;
            value->transport = from->transport ? self->b->retain(from->transport) : 0;
            value->tls_options = from->tls_options ? self->b->retain(from->tls_options) : 0;
        }
        s.x[0] = self->make(self->parameters_class, value);
    });
    fn("_nw_parameters_set_local_endpoint", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Parameters>(s.x[0], "nw_parameters_t");
        const auto local = s.x[1] ? self->b->retain(s.x[1]) : 0;
        Address previous{};
        { std::lock_guard lock(value->mutex); previous = std::exchange(value->local, local); }
        if (previous) self->b->release(previous);
    });
    fn("_nw_parameters_copy_local_endpoint", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Parameters>(s.x[0], "nw_parameters_t");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->local ? self->b->retain(value->local) : 0;
    });
    fn("_nw_parameters_set_reuse_local_address", [self](CpuState& s, GuestMemory&) {
        self->state<Parameters>(s.x[0], "nw_parameters_t")->reuse_address = (s.x[1] & 0xff) != 0;
    });
    fn("_nw_parameters_get_reuse_local_address", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<Parameters>(s.x[0], "nw_parameters_t")->reuse_address; });
    fn("_nw_parameters_set_include_peer_to_peer", [self](CpuState& s, GuestMemory&) {
        self->state<Parameters>(s.x[0], "nw_parameters_t")->peer_to_peer = (s.x[1] & 0xff) != 0;
    });
    fn("_nw_parameters_get_include_peer_to_peer", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<Parameters>(s.x[0], "nw_parameters_t")->peer_to_peer; });
    fn("_nw_parameters_prohibit_interface_type", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Parameters>(s.x[0], "nw_parameters_t");
        std::lock_guard lock(value->mutex);
        value->prohibited.push_back(s.x[1]);
    });
    fn("_nw_parameters_set_required_interface_type", [self](CpuState& s, GuestMemory&) {
        self->state<Parameters>(s.x[0], "nw_parameters_t")->required = s.x[1];
    });
    fn("_nw_parameters_get_required_interface_type", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<Parameters>(s.x[0], "nw_parameters_t")->required; });
    // The protocol stack is the parameters' view: the transport and the application (TLS) protocols.
    fn("_nw_parameters_copy_default_protocol_stack", [self](CpuState& s, GuestMemory&) {
        const auto stack = self->b->objc.allocate(self->stack_class);
        static_cast<void>(self->b->objc.host_state<Parameters>(stack, [&] { return self->state<Parameters>(s.x[0], "nw_parameters_t"); }));
        s.x[0] = stack;
    });
    fn("_nw_protocol_stack_copy_transport_protocol", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Parameters>(s.x[0], "nw_protocol_stack_t");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->transport ? self->b->retain(value->transport) : 0;
    });
    fn("_nw_protocol_stack_set_transport_protocol", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Parameters>(s.x[0], "nw_protocol_stack_t");
        const auto transport = self->state<Options>(s.x[1], "nw_protocol_options_t");
        const auto retained = self->b->retain(s.x[1]);
        Address previous{};
        { std::lock_guard lock(value->mutex); previous = std::exchange(value->transport, retained); value->udp = transport->kind == Options::Kind::udp; }
        if (previous) self->b->release(previous);
    });
    fn("_nw_protocol_stack_prepend_application_protocol", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Parameters>(s.x[0], "nw_protocol_stack_t");
        const auto added = self->state<Options>(s.x[1], "nw_protocol_options_t");
        if (added->kind != Options::Kind::tls) return;
        const auto retained = self->b->retain(s.x[1]);
        Address previous{};
        { std::lock_guard lock(value->mutex); value->tls = true; previous = std::exchange(value->tls_options, retained); }
        if (previous) self->b->release(previous);
    });
    fn("_nw_protocol_stack_clear_application_protocols", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Parameters>(s.x[0], "nw_protocol_stack_t");
        Address previous{};
        { std::lock_guard lock(value->mutex); value->tls = false; previous = std::exchange(value->tls_options, 0); }
        if (previous) self->b->release(previous);
    });

    // ---- content contexts ----
    const auto context = [self](std::string identifier, bool final, bool constant) {
        auto value = std::make_shared<Context>();
        value->identifier = std::move(identifier);
        value->final = final;
        value->constant = constant;
        return self->make(self->context_class, value);
    };
    default_message = context("default", false, true);
    final_context = context("final", true, true);
    default_stream = context("stream", false, true);
    b->export_value(framework, "__nw_content_context_default_message", default_message);
    b->export_value(framework, "__nw_content_context_final_send", final_context);
    b->export_value(framework, "__nw_content_context_default_stream", default_stream);
    fn("_nw_content_context_create", [self, context](CpuState& s, GuestMemory&) { s.x[0] = context(self->read_text(s.x[0]), false, false); });
    fn("_nw_content_context_get_identifier", [self](CpuState& s, GuestMemory&) {
        // A stable C string per context object: the identifier's text lives in a guest string kept by the context.
        const auto value = self->state<Context>(s.x[0], "nw_content_context_t");
        static std::mutex texts_mutex;
        static std::map<std::string, Address> texts;
        std::lock_guard lock(texts_mutex);
        auto& text = texts[value->identifier];
        if (!text) text = self->c_string(value->identifier);
        s.x[0] = text;
    });
    fn("_nw_content_context_get_is_final", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<Context>(s.x[0], "nw_content_context_t")->final; });
    fn("_nw_content_context_set_is_final", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Context>(s.x[0], "nw_content_context_t");
        if (!value->constant) value->final = (s.x[1] & 0xff) != 0;
    });
    // NW_CONNECTION_SEND_IDEMPOTENT_CONTENT: a global completion block; a send with it reports nothing.
    idempotent = b->make_block([](CpuState&, GuestMemory&) {});
    b->export_value(framework, "__nw_connection_send_idempotent_content", idempotent);

    // ---- connections ----
    dealloc(connection_class, [self](Address object) {
        if (const auto value = self->b->objc.find_host_state<Connection>(object)) {
            std::vector<Address> objects, blocks, queues;
            std::int64_t fd = -1;
            {
                std::lock_guard lock(value->mutex);
                objects = {std::exchange(value->endpoint, 0), std::exchange(value->parameters, 0)};
                blocks = {std::exchange(value->state_handler, 0)};
                queues = {std::exchange(value->queue, 0), std::exchange(value->reader, 0), std::exchange(value->writer, 0)};
                fd = std::exchange(value->fd, -1);
            }
            if (fd >= 0) self->sys("_close", {static_cast<Address>(fd)});
            for (const auto item : objects) if (item) self->b->release(item);
            for (const auto block : blocks) if (block) self->b->block_release(block);
            for (const auto queue : queues) self->release_queue(queue);
        }
    });
    const auto new_connection = [self](Address endpoint, Address parameters, std::int64_t fd) {
        auto value = std::make_shared<Connection>();
        value->endpoint = self->b->retain(endpoint);
        value->parameters = parameters ? self->b->retain(parameters) : 0;
        value->udp = parameters && self->state<Parameters>(parameters, "nw_parameters_t")->udp;
        value->fd = fd;
        value->connected = fd >= 0;
        value->reader = self->b->create_serial_queue("com.alchemy.nw.connection.receive");
        value->writer = self->b->create_serial_queue("com.alchemy.nw.connection.send");
        return self->make(self->connection_class, value);
    };
    // nw_connection_t nw_connection_create(nw_endpoint_t, nw_parameters_t)
    fn("_nw_connection_create", [self, new_connection](CpuState& s, GuestMemory&) {
        static_cast<void>(self->state<Endpoint>(s.x[0], "nw_endpoint_t"));
        s.x[0] = new_connection(s.x[0], s.x[1], -1);
    });
    fn("_nw_connection_set_queue", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Connection>(s.x[0], "nw_connection_t");
        self->set_queue(value->mutex, value->queue, s.x[1]);
    });
    fn("_nw_connection_set_state_changed_handler", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Connection>(s.x[0], "nw_connection_t");
        self->set_block(value->mutex, value->state_handler, s.x[1]);
    });
    // Viability, better-path and path changes are kept but never reported (paths do not change under a connection here).
    for (const auto* name : {"_nw_connection_set_viability_changed_handler", "_nw_connection_set_better_path_available_handler",
                             "_nw_connection_set_path_changed_handler"})
        fn(name, [self](CpuState& s, GuestMemory&) { static_cast<void>(self->state<Connection>(s.x[0], "nw_connection_t")); });
    fn("_nw_connection_start", [self](CpuState& s, GuestMemory&) {
        const auto connection = s.x[0];
        const auto value = self->state<Connection>(connection, "nw_connection_t");
        bool tls{}, connected{};
        {
            std::lock_guard lock(value->mutex);
            if (value->state != connection_invalid) return;
            if (!value->queue) throw std::runtime_error("Alchemy Network.framework: nw_connection_start before nw_connection_set_queue");
            connected = value->connected;
            if (value->parameters) {
                const auto parameters = self->state<Parameters>(value->parameters, "nw_parameters_t");
                tls = parameters->tls;
            }
        }
        if (connected) { self->report(connection, value, connection_ready); return; }
        self->report(connection, value, connection_preparing);
        // TLS is unavailable: the connection fails with a TLS-domain error instead of connecting in the clear.
        if (tls) { self->report(connection, value, connection_failed, self->error(error_domain_tls, err_ssl_internal)); return; }
        self->b->retain(connection);
        self->background([self, connection, value] { self->connect(connection, value); });
    });
    fn("_nw_connection_restart", [self](CpuState& s, GuestMemory&) {
        const auto connection = s.x[0];
        const auto value = self->state<Connection>(connection, "nw_connection_t");
        {
            std::lock_guard lock(value->mutex);
            if (value->state != connection_failed && value->state != connection_waiting) return;
            value->state = connection_preparing;
        }
        self->b->retain(connection);
        self->background([self, connection, value] { self->connect(connection, value); });
    });
    const auto cancel = [self](Address connection) {
        const auto value = self->state<Connection>(connection, "nw_connection_t");
        std::int64_t fd = -1;
        Address queue{}, handler{};
        {
            std::lock_guard lock(value->mutex);
            if (value->state == connection_cancelled) return;
            value->state = connection_cancelled;
            fd = std::exchange(value->fd, -1);
            queue = value->queue;
            handler = value->state_handler;
        }
        if (fd >= 0) { self->sys("_shutdown", {static_cast<Address>(fd), 2}); self->sys("_close", {static_cast<Address>(fd)}); }
        if (queue && handler) {
            self->b->retain(connection);
            self->deliver(queue, handler, {connection_cancelled, 0}, {connection});
        }
    };
    fn("_nw_connection_cancel", [cancel](CpuState& s, GuestMemory&) { cancel(s.x[0]); });
    fn("_nw_connection_force_cancel", [cancel](CpuState& s, GuestMemory&) { cancel(s.x[0]); });
    fn("_nw_connection_cancel_current_endpoint", [](CpuState&, GuestMemory&) {});   // one endpoint: nothing to skip to
    fn("_nw_connection_copy_endpoint", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Connection>(s.x[0], "nw_connection_t");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->endpoint ? self->b->retain(value->endpoint) : 0;
    });
    fn("_nw_connection_copy_parameters", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Connection>(s.x[0], "nw_connection_t");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->parameters ? self->b->retain(value->parameters) : 0;
    });
    fn("_nw_connection_get_maximum_datagram_size", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->state<Connection>(s.x[0], "nw_connection_t")->udp ? 65507 : 0;
    });
    // void nw_connection_batch(nw_connection_t, dispatch_block_t): the block runs now.
    fn("_nw_connection_batch", [self](CpuState& s, GuestMemory&) { if (s.x[1]) static_cast<void>(self->b->invoke_block(s.x[1], {})); });
    // receive(connection, minimum, maximum, completion(content, context, is_complete, error)) on the receive queue.
    const auto receive = [self](Address connection, std::uint64_t minimum, std::uint64_t maximum, Address completion, bool message) {
        const auto value = self->state<Connection>(connection, "nw_connection_t");
        if (!completion) return;
        Address reader{};
        { std::lock_guard lock(value->mutex); reader = value->reader; }
        const auto held = self->b->block_copy(completion);
        self->b->retain(connection);
        self->b->dispatch_async(reader, [self, connection, value, minimum, maximum, held, message] {
            std::vector<std::uint8_t> bytes;
            std::int64_t code = 0;
            bool complete = false;
            const auto limit = std::max<std::uint64_t>(1, std::min<std::uint64_t>(maximum ? maximum : 65536, 1u << 20));
            const auto buffer = self->sys("_malloc", {limit});
            while (true) {
                const auto fd = self->fd_of(value);
                if (fd < 0) { code = ecanceled; break; }
                const auto count = static_cast<std::int64_t>(self->sys("_recv", {static_cast<Address>(fd), buffer, limit - bytes.size(), 0}));
                if (count < 0) { code = self->fd_of(value) < 0 ? ecanceled : self->guest_errno(); break; }
                if (count == 0) { complete = true; break; }
                const auto at = bytes.size();
                bytes.resize(at + static_cast<std::size_t>(count));
                self->memory().read_bytes(buffer, std::span(bytes.data() + at, static_cast<std::size_t>(count)));
                if (message || bytes.size() >= std::max<std::uint64_t>(minimum, 1) || bytes.size() >= limit) break;
            }
            self->sys("_free", {buffer});
            if (message && code == 0 && !bytes.empty()) complete = true;   // a datagram is a whole message
            Address queue{};
            { std::lock_guard lock(value->mutex); queue = value->queue; }
            const auto content = self->dispatch_data(bytes);
            const auto error = code ? self->error(error_domain_posix, code) : 0;
            const auto context = complete && !message ? self->final_context : self->default_message;
            std::vector<Address> owned{connection}, dispatched;
            if (content) dispatched.push_back(content);
            if (error) owned.push_back(error);
            self->deliver(queue, held, {content, context, complete ? 1u : 0u, error}, owned, dispatched);
            self->b->block_release(held);
        });
    };
    fn("_nw_connection_receive", [receive](CpuState& s, GuestMemory&) { receive(s.x[0], s.x[1] & 0xffffffff, s.x[2] & 0xffffffff, s.x[3], false); });
    fn("_nw_connection_receive_message", [receive](CpuState& s, GuestMemory&) { receive(s.x[0], 1, 65536, s.x[1], true); });
    // send(connection, content, context, is_complete, completion(error)) on the send queue.
    fn("_nw_connection_send", [self](CpuState& s, GuestMemory&) {
        const auto connection = s.x[0];
        const auto value = self->state<Connection>(connection, "nw_connection_t");
        const auto bytes = self->bytes_of(s.x[1]);
        const auto context = s.x[2];
        const bool complete = (s.x[3] & 0xff) != 0;
        const bool final = context && context == self->final_context;
        const auto completion = s.x[4] && s.x[4] != self->idempotent ? self->b->block_copy(s.x[4]) : 0;
        Address writer{};
        { std::lock_guard lock(value->mutex); writer = value->writer; }
        self->b->retain(connection);
        self->b->dispatch_async(writer, [self, connection, value, bytes, complete, final, completion] {
            std::int64_t code = 0;
            const auto fd = self->fd_of(value);
            if (fd < 0) code = ecanceled;
            else if (!bytes.empty()) {
                const auto buffer = self->sys("_malloc", {bytes.size()});
                self->memory().write_bytes(buffer, bytes);
                std::size_t sent = 0;
                while (sent < bytes.size()) {
                    const auto count = static_cast<std::int64_t>(self->sys("_send", {static_cast<Address>(fd), buffer + sent, bytes.size() - sent, 0}));
                    if (count <= 0) { code = self->guest_errno(); break; }
                    sent += static_cast<std::size_t>(count);
                }
                self->sys("_free", {buffer});
            }
            if (code == 0 && complete && final && !value->udp) self->sys("_shutdown", {static_cast<Address>(fd), 1});   // SHUT_WR
            if (completion) {
                Address queue{};
                { std::lock_guard lock(value->mutex); queue = value->queue; }
                const auto error = code ? self->error(error_domain_posix, code) : 0;
                std::vector<Address> owned{connection};
                if (error) owned.push_back(error);
                self->deliver(queue, completion, {error}, owned);
                self->b->block_release(completion);
            } else self->b->release(connection);
        });
    });

    // ---- listeners (TCP) ----
    dealloc(listener_class, [self](Address object) {
        if (const auto value = self->b->objc.find_host_state<Listener>(object)) {
            std::vector<Address> blocks, queues;
            Address parameters{};
            std::int64_t fd = -1;
            {
                std::lock_guard lock(value->mutex);
                blocks = {std::exchange(value->state_handler, 0), std::exchange(value->connection_handler, 0)};
                queues = {std::exchange(value->queue, 0), std::exchange(value->acceptor, 0)};
                parameters = std::exchange(value->parameters, 0);
                fd = std::exchange(value->fd, -1);
            }
            if (fd >= 0) self->sys("_close", {static_cast<Address>(fd)});
            if (parameters) self->b->release(parameters);
            for (const auto block : blocks) if (block) self->b->block_release(block);
            for (const auto queue : queues) self->release_queue(queue);
        }
    });
    const auto new_listener = [self](Address parameters, std::uint16_t port) {
        auto value = std::make_shared<Listener>();
        value->parameters = parameters ? self->b->retain(parameters) : 0;
        value->port = port;
        value->acceptor = self->b->create_serial_queue("com.alchemy.nw.listener.accept");
        return self->make(self->listener_class, value);
    };
    fn("_nw_listener_create", [new_listener](CpuState& s, GuestMemory&) { s.x[0] = new_listener(s.x[0], 0); });
    fn("_nw_listener_create_with_port", [self, new_listener](CpuState& s, GuestMemory&) {
        s.x[0] = new_listener(s.x[1], static_cast<std::uint16_t>(std::strtoul(self->read_text(s.x[0], 16).c_str(), nullptr, 10)));
    });
    fn("_nw_listener_set_queue", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Listener>(s.x[0], "nw_listener_t");
        self->set_queue(value->mutex, value->queue, s.x[1]);
    });
    fn("_nw_listener_set_state_changed_handler", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Listener>(s.x[0], "nw_listener_t");
        self->set_block(value->mutex, value->state_handler, s.x[1]);
    });
    fn("_nw_listener_set_new_connection_handler", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Listener>(s.x[0], "nw_listener_t");
        self->set_block(value->mutex, value->connection_handler, s.x[1]);
    });
    fn("_nw_listener_set_new_connection_limit", [](CpuState&, GuestMemory&) {});   // every connection is delivered
    fn("_nw_listener_get_port", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Listener>(s.x[0], "nw_listener_t");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->port;
    });
    const auto report_listener = [self](Address listener, const std::shared_ptr<Listener>& value, std::uint64_t state, Address error = 0) {
        Address queue{}, handler{};
        {
            std::lock_guard lock(value->mutex);
            if (value->state == listener_cancelled) { if (error) self->b->release(error); return; }
            value->state = state;
            queue = value->queue;
            handler = value->state_handler;
        }
        if (!queue || !handler) { if (error) self->b->release(error); return; }
        self->b->retain(listener);
        std::vector<Address> owned{listener};
        if (error) owned.push_back(error);
        self->deliver(queue, handler, {state, error}, owned);
    };
    fn("_nw_listener_start", [self, report_listener, new_connection](CpuState& s, GuestMemory&) {
        const auto listener = s.x[0];
        const auto value = self->state<Listener>(listener, "nw_listener_t");
        bool udp{}, reuse{}, tls{};
        Address local{};
        {
            std::lock_guard lock(value->mutex);
            if (value->state != listener_invalid) return;
            if (!value->queue) throw std::runtime_error("Alchemy Network.framework: nw_listener_start before nw_listener_set_queue");
            if (value->parameters) {
                const auto parameters = self->state<Parameters>(value->parameters, "nw_parameters_t");
                std::lock_guard parameters_lock(parameters->mutex);
                udp = parameters->udp; reuse = parameters->reuse_address; tls = parameters->tls; local = parameters->local;
            }
        }
        if (udp) { report_listener(listener, value, listener_failed, self->error(error_domain_posix, eopnotsupp)); return; }
        if (tls) { report_listener(listener, value, listener_failed, self->error(error_domain_tls, err_ssl_internal)); return; }
        // Bind (IPv4: the local endpoint's address, else any) and listen.
        std::array<std::uint8_t, 16> address{16, 2};   // sockaddr_in: len, AF_INET
        if (local) {
            const auto endpoint = self->state<Endpoint>(local, "nw_endpoint_t");
            if (endpoint->type == endpoint_address && endpoint->address.size() >= 16 && endpoint->address[1] == 2)
                std::copy_n(endpoint->address.begin(), 16, address.begin());
        }
        address[2] = static_cast<std::uint8_t>(value->port >> 8);
        address[3] = static_cast<std::uint8_t>(value->port);
        const auto fd = static_cast<std::int32_t>(self->sys("_socket", {2, 1, 0}));
        std::int64_t code = fd < 0 ? self->guest_errno() : 0;
        if (code == 0 && reuse) {
            const auto one = self->sys("_malloc", {4});
            self->memory().write32(one, 1);
            self->sys("_setsockopt", {static_cast<Address>(fd), 0xffff, 4, one, 4});   // SOL_SOCKET, SO_REUSEADDR
            self->sys("_free", {one});
        }
        if (code == 0) {
            const auto block = self->sys("_malloc", {16});
            self->memory().write_bytes(block, address);
            if (static_cast<std::int32_t>(self->sys("_bind", {static_cast<Address>(fd), block, 16})) != 0 ||
                static_cast<std::int32_t>(self->sys("_listen", {static_cast<Address>(fd), 128})) != 0)
                code = self->guest_errno();
            else {
                const auto length = self->sys("_malloc", {4});
                self->memory().write32(length, 16);
                if (static_cast<std::int32_t>(self->sys("_getsockname", {static_cast<Address>(fd), block, length})) == 0) {
                    std::lock_guard lock(value->mutex);
                    value->port = static_cast<std::uint16_t>((self->memory().read8(block + 2) << 8) | self->memory().read8(block + 3));
                }
                self->sys("_free", {length});
            }
            self->sys("_free", {block});
        }
        if (code) {
            if (fd >= 0) self->sys("_close", {static_cast<Address>(fd)});
            report_listener(listener, value, listener_failed, self->error(error_domain_posix, code));
            return;
        }
        Address acceptor{};
        {
            std::lock_guard lock(value->mutex);
            value->fd = fd;
            acceptor = value->acceptor;
        }
        report_listener(listener, value, listener_ready);
        // The accept loop: each connection to the handler, already connected.
        self->b->retain(listener);
        self->b->dispatch_async(acceptor, [self, listener, value, new_connection] {
            const auto peer = self->sys("_malloc", {128}), length = self->sys("_malloc", {4});
            while (true) {
                std::int64_t fd{};
                { std::lock_guard lock(value->mutex); fd = value->fd; }
                if (fd < 0) break;
                self->memory().write32(length, 128);
                const auto accepted = static_cast<std::int32_t>(self->sys("_accept", {static_cast<Address>(fd), peer, length}));
                if (accepted < 0) {
                    std::lock_guard lock(value->mutex);
                    if (value->fd < 0 || value->state == listener_cancelled) break;
                    continue;
                }
                const auto size = std::min<std::uint32_t>(self->memory().read32(length), 128);
                std::vector<std::uint8_t> bytes(size);
                self->memory().read_bytes(peer, bytes);
                Address queue{}, handler{}, parameters{};
                { std::lock_guard lock(value->mutex); queue = value->queue; handler = value->connection_handler; parameters = value->parameters; }
                const auto endpoint = self->address_endpoint(bytes);
                const auto connection = new_connection(endpoint, parameters, accepted);
                self->b->release(endpoint);
                if (queue && handler) self->deliver(queue, handler, {connection}, {connection});
                else self->b->release(connection);
            }
            self->sys("_free", {peer});
            self->sys("_free", {length});
            self->b->release(listener);
        });
    });
    fn("_nw_listener_cancel", [self, report_listener](CpuState& s, GuestMemory&) {
        const auto value = self->state<Listener>(s.x[0], "nw_listener_t");
        std::int64_t fd = -1;
        Address queue{}, handler{};
        {
            std::lock_guard lock(value->mutex);
            if (value->state == listener_cancelled) return;
            value->state = listener_cancelled;
            fd = std::exchange(value->fd, -1);
            queue = value->queue;
            handler = value->state_handler;
        }
        if (fd >= 0) { self->sys("_shutdown", {static_cast<Address>(fd), 2}); self->sys("_close", {static_cast<Address>(fd)}); }
        if (queue && handler) {
            self->b->retain(s.x[0]);
            self->deliver(queue, handler, {listener_cancelled, 0}, {s.x[0]});
        }
    });

    // ---- Bonjour browsing over the host mDNS engine ----
    fn("_nw_browse_descriptor_create_bonjour_service", [self](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<Descriptor>();
        value->type = self->read_text(s.x[0]);
        value->domain = s.x[1] ? self->read_text(s.x[1]) : std::string("local.");
        s.x[0] = self->make(self->descriptor_class, value);
    });
    dealloc(browser_class, [self](Address object) {
        if (const auto value = self->b->objc.find_host_state<Browser>(object)) {
            std::unique_ptr<mdns::Operation> operation;
            std::vector<Address> objects, blocks;
            Address queue{};
            {
                std::lock_guard lock(value->mutex);
                operation = std::move(value->operation);
                objects = {std::exchange(value->descriptor, 0), std::exchange(value->parameters, 0)};
                blocks = {std::exchange(value->state_handler, 0), std::exchange(value->results_handler, 0)};
                queue = std::exchange(value->queue, 0);
            }
            if (operation) operation->cancel();
            for (const auto item : objects) if (item) self->b->release(item);
            for (const auto block : blocks) if (block) self->b->block_release(block);
            self->release_queue(queue);
        }
    });
    dealloc(result_class, [self](Address object) {
        if (const auto value = self->b->objc.find_host_state<BrowseResult>(object))
            if (const auto endpoint = std::exchange(value->endpoint, 0)) self->b->release(endpoint);
    });
    fn("_nw_browser_create", [self](CpuState& s, GuestMemory&) {
        static_cast<void>(self->state<Descriptor>(s.x[0], "nw_browse_descriptor_t"));
        auto value = std::make_shared<Browser>();
        value->descriptor = self->b->retain(s.x[0]);
        value->parameters = s.x[1] ? self->b->retain(s.x[1]) : 0;
        s.x[0] = self->make(self->browser_class, value);
    });
    fn("_nw_browser_set_queue", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Browser>(s.x[0], "nw_browser_t");
        self->set_queue(value->mutex, value->queue, s.x[1]);
    });
    fn("_nw_browser_set_state_changed_handler", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Browser>(s.x[0], "nw_browser_t");
        self->set_block(value->mutex, value->state_handler, s.x[1]);
    });
    fn("_nw_browser_set_browse_results_changed_handler", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<Browser>(s.x[0], "nw_browser_t");
        self->set_block(value->mutex, value->results_handler, s.x[1]);
    });
    const auto report_browser = [self](Address browser, const std::shared_ptr<Browser>& value, std::uint64_t state, Address error = 0) {
        Address queue{}, handler{};
        { std::lock_guard lock(value->mutex); queue = value->queue; handler = value->state_handler; }
        if (!queue || !handler) { if (error) self->b->release(error); return; }
        self->b->retain(browser);
        std::vector<Address> owned{browser};
        if (error) owned.push_back(error);
        self->deliver(queue, handler, {state, error}, owned);
    };
    // One found or lost service as an nw_browse_result change (on the guest pump, then the browser's queue).
    const auto change = [self](Address browser, std::shared_ptr<Browser> value, mdns::Event event) {
        Address queue{}, handler{};
        {
            std::lock_guard lock(value->mutex);
            if (value->cancelled) return;
            queue = value->queue;
            handler = value->results_handler;
            auto& list = value->services;
            const auto key = std::pair{event.name, event.interface_index};
            const auto found = std::find(list.begin(), list.end(), key);
            if (event.kind == mdns::Event::Kind::found) { if (found != list.end()) return; list.push_back(key); }
            else { if (found == list.end()) return; list.erase(found); }
        }
        if (!queue || !handler) return;
        auto endpoint = std::make_shared<Endpoint>();
        endpoint->type = endpoint_bonjour;
        endpoint->name = event.name;
        endpoint->service = event.type;
        endpoint->domain = event.domain.empty() ? std::string("local.") : event.domain;
        const auto endpoint_object = self->make(self->endpoint_class, endpoint);
        auto result = std::make_shared<BrowseResult>();
        result->endpoint = endpoint_object;
        result->changes = event.kind == mdns::Event::Kind::found ? change_result_added : change_result_removed;
        const auto result_object = self->make(self->result_class, result);
        self->b->retain(browser);
        const bool added = event.kind == mdns::Event::Kind::found;
        // (old_result, new_result, batch_complete): an added result has no old one, a removed one no new one.
        self->deliver(queue, handler, {added ? 0 : result_object, added ? result_object : 0, event.more_coming ? 0u : 1u}, {result_object, browser});
    };
    fn("_nw_browser_start", [self, report_browser, change](CpuState& s, GuestMemory&) {
        const auto browser = s.x[0];
        const auto value = self->state<Browser>(browser, "nw_browser_t");
        Descriptor descriptor;
        {
            std::lock_guard lock(value->mutex);
            if (value->started || value->cancelled) return;
            value->started = true;
            descriptor = *self->state<Descriptor>(value->descriptor, "nw_browse_descriptor_t");
        }
        const auto type = mdns::normalize_type(descriptor.type);
        if (!type || !mdns::is_local_domain(descriptor.domain)) {
            report_browser(browser, value, browser_failed, self->error(error_domain_dns, mdns::error::unsupported));
            return;
        }
        static auto lease = self->pump->lease();   // the pump runs for the process once a browser starts
        std::int32_t status = 0;
        const std::weak_ptr<Browser> weak = value;
        auto operation = mdns::browse(*type, 0, [self, browser, weak, change](mdns::Event event) {
            if (event.kind != mdns::Event::Kind::found && event.kind != mdns::Event::Kind::lost) return;
            self->pump->post([self, browser, weak, change, event] {
                if (const auto value = weak.lock()) change(browser, value, event);
            });
        }, status);
        if (!operation) {
            report_browser(browser, value, browser_failed, self->error(error_domain_dns, status ? status : mdns::error::service_not_running));
            return;
        }
        {
            std::lock_guard lock(value->mutex);
            value->operation = std::move(operation);
        }
        report_browser(browser, value, browser_ready);
    });
    fn("_nw_browser_cancel", [self, report_browser](CpuState& s, GuestMemory&) {
        const auto value = self->state<Browser>(s.x[0], "nw_browser_t");
        std::unique_ptr<mdns::Operation> operation;
        {
            std::lock_guard lock(value->mutex);
            if (value->cancelled) return;
            value->cancelled = true;
            operation = std::move(value->operation);
        }
        if (operation) operation->cancel();
        report_browser(s.x[0], value, browser_cancelled);
    });
    fn("_nw_browse_result_copy_endpoint", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<BrowseResult>(s.x[0], "nw_browse_result_t");
        s.x[0] = value->endpoint ? self->b->retain(value->endpoint) : 0;
    });
    // nw_browse_result_get_changes(old_result, new_result)
    fn("_nw_browse_result_get_changes", [self](CpuState& s, GuestMemory&) {
        const auto old_result = s.x[0] ? self->b->objc.find_host_state<BrowseResult>(s.x[0]) : nullptr;
        const auto new_result = s.x[1] ? self->b->objc.find_host_state<BrowseResult>(s.x[1]) : nullptr;
        s.x[0] = new_result ? new_result->changes : old_result ? old_result->changes : 0;
        if (old_result && new_result && old_result == new_result) s.x[0] = change_identical;
    });
}

} // namespace

} // namespace alchemy::gui::network
