#include "alchemy/gui/value_provider.hpp"

#include "alchemy/darwin_threads.hpp"
#include "alchemy/host/net.hpp"

#include <chrono>
#include <thread>

// Network.framework (<Network/Network.h>) as Chrome uses it:
// - nw_path_monitor: real over the host's adapters. A path is satisfied when
//   an operational, non-loopback adapter has a unicast address, unsatisfied
//   otherwise; its interfaces are those adapters, typed from the Windows
//   interface type (Ethernet = wired, IEEE 802.11 = wifi, WWAN = cellular).
//   The first path is delivered on the monitor's queue after start; a watcher
//   (a guest dispatch worker) re-reads the adapters every second and delivers
//   a new path when the set or the status changes, until the monitor is
//   cancelled.
// - nw_browser (Bonjour discovery): no mDNS responder exists. A started
//   browser moves to nw_browser_state_failed with a DNS-domain error
//   (kDNSServiceErr_ServiceNotRunning); a cancelled one to
//   nw_browser_state_cancelled. No result is ever reported.
// - Parameters and protocol configuration blocks are real values; Alchemy
//   opens no Network.framework connection, so they are only stored.
// Enumerations (<Network/path.h>, interface.h, browser.h, error.h):
//   nw_path_status: invalid 0, satisfied 1, unsatisfied 2, satisfiable 3;
//   nw_interface_type: other 0, wifi 1, cellular 2, wired 3, loopback 4;
//   nw_browser_state: invalid 0, ready 1, failed 2, cancelled 3, waiting 4;
//   nw_error_domain: invalid 0, posix 1, dns 2, tls 3.
namespace alchemy::gui::values {
namespace {

constexpr std::uint64_t path_satisfied = 1, path_unsatisfied = 2;
constexpr std::uint64_t interface_other = 0, interface_wifi = 1, interface_cellular = 2, interface_wired = 3, interface_loopback = 4;
constexpr std::uint64_t browser_failed = 2, browser_cancelled = 3;
constexpr std::uint64_t error_domain_dns = 2;
constexpr std::int64_t dns_service_not_running = -65563;

struct HostInterface {
    std::string name;
    std::uint32_t index{};
    std::uint64_t type{};
    bool operator==(const HostInterface&) const = default;
};

// Operational adapters with a unicast address, Darwin-named like getifaddrs
// (lo0, en<n> in adapter order, utun<n> for tunnels).
std::vector<HostInterface> host_interfaces() {
    std::vector<HostInterface> result;
    const auto adapters = host::net::interfaces();
    if (!adapters) return result;
    using Kind = host::net::InterfaceKind;
    unsigned ethernet = 0, tunnel = 0;
    for (const auto& adapter : *adapters) {
        if (!adapter.up || adapter.addresses.empty()) continue;
        HostInterface entry;
        entry.index = adapter.index;
        switch (adapter.kind) {
        case Kind::loopback: entry.type = interface_loopback; entry.name = "lo0"; break;
        case Kind::ethernet: entry.type = interface_wired; break;
        case Kind::wireless: entry.type = interface_wifi; break;
        case Kind::cellular: entry.type = interface_cellular; break;
        default: entry.type = interface_other; break;
        }
        if (entry.name.empty()) {
            const bool point_to_point = adapter.kind == Kind::tunnel || adapter.kind == Kind::ppp;
            entry.name = point_to_point ? "utun" + std::to_string(tunnel++) : "en" + std::to_string(ethernet++);
        }
        result.push_back(std::move(entry));
    }
    return result;
}

struct Path final : ObjectPayload {
    std::uint64_t status{};
    std::vector<Address> interfaces;   // retained nw_interface objects
};
struct Interface final : ObjectPayload { HostInterface value; Address name{}; };
struct Monitor final : ObjectPayload {
    std::mutex mutex;
    Address queue{}, update{}, cancel{};   // retained queue, copied blocks
    bool started{}, cancelled{};
    std::uint64_t generation{};
};
struct Browser final : ObjectPayload {
    std::mutex mutex;
    Address descriptor{}, parameters{}, queue{}, state_handler{}, results_handler{};
    bool started{}, cancelled{};
};
struct Error final : ObjectPayload { std::uint64_t domain{}; std::int64_t code{}; };

class NetworkProvider {
public:
    NetworkProvider(Provider& owner) : f(owner) {}
    Provider& f;
    Address path_class{}, interface_class{}, monitor_class{}, error_class{}, browser_class{}, descriptor_class{},
        parameters_class{};
    std::mutex mutex;
    std::map<std::string, Address> names;   // interface name -> immortal C string

    Address name_text(const std::string& name) {
        std::lock_guard lock(mutex);
        auto& text = names[name];
        if (!text) text = f.cstring(name);
        return text;
    }

    Address make_path(const std::vector<HostInterface>& interfaces) {
        const auto path = f.objc.allocate(path_class);
        auto state = std::make_shared<Path>();
        bool usable = false;
        for (const auto& entry : interfaces) {
            if (entry.type == interface_loopback) continue;
            usable = true;
            const auto object = f.objc.allocate(interface_class);
            auto value = std::make_shared<Interface>();
            value->value = entry;
            value->name = name_text(entry.name);
            static_cast<void>(f.objc.host_state<Interface>(object, [&] { return value; }));
            state->interfaces.push_back(object);
        }
        state->status = usable ? path_satisfied : path_unsatisfied;
        static_cast<void>(f.objc.host_state<Path>(path, [&] { return state; }));
        return path;
    }

    // Delivers a path for `interfaces` to the monitor's update handler on its queue.
    void deliver(Address monitor, const std::shared_ptr<Monitor>& state, const std::vector<HostInterface>& interfaces) {
        Address queue{};
        {
            std::lock_guard lock(state->mutex);
            if (state->cancelled || !state->update || !state->queue) return;
            queue = state->queue;
        }
        static_cast<void>(f.retain(monitor));
        f.bridge->dispatch_async(queue, [this, monitor, state, interfaces] {
            Address update{};
            {
                std::lock_guard lock(state->mutex);
                if (!state->cancelled) update = state->update;
            }
            if (update) {
                const auto path = make_path(interfaces);
                static_cast<void>(f.bridge->invoke_block(update, {path}));
                f.release(path);
            }
            f.release(monitor);
        });
    }

    // The watcher (a guest dispatch worker) of one monitor start.
    void watch(Address monitor, std::shared_ptr<Monitor> state, std::uint64_t generation, std::vector<HostInterface> previous) {
        for (;;) {
            for (int slice = 0; slice < 20; ++slice) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (guest_worker_must_stop(f.process)) throw GuestThreadStop{};
                std::lock_guard lock(state->mutex);
                if (state->cancelled || state->generation != generation) { f.release(monitor); return; }
            }
            auto current = host_interfaces();
            if (current == previous) continue;
            previous = current;
            deliver(monitor, state, current);
        }
    }
};

std::shared_ptr<Monitor> monitor_of(ObjcRuntime& objc, Address object) {
    auto value = object ? objc.find_host_state<Monitor>(object) : nullptr;
    if (!value) throw std::runtime_error("Alchemy: Network.framework call needs an nw_path_monitor_t");
    return value;
}
std::shared_ptr<Browser> browser_of(ObjcRuntime& objc, Address object) {
    auto value = object ? objc.find_host_state<Browser>(object) : nullptr;
    if (!value) throw std::runtime_error("Alchemy: Network.framework call needs an nw_browser_t");
    return value;
}

} // namespace

void Provider::install_network() {
    ProcessContext::add_provider_library(std::string(network));
    auto provider = std::make_shared<NetworkProvider>(*this);
    auto& n = *provider;
    n.path_class = cls(network, "OS_nw_path", "NSObject");
    n.interface_class = cls(network, "OS_nw_interface", "NSObject");
    n.monitor_class = cls(network, "OS_nw_path_monitor", "NSObject");
    n.error_class = cls(network, "OS_nw_error", "NSObject");
    n.browser_class = cls(network, "OS_nw_browser", "NSObject");
    n.descriptor_class = cls(network, "OS_nw_browse_descriptor", "NSObject");
    n.parameters_class = cls(network, "OS_nw_parameters", "NSObject");
    object_properties(n.descriptor_class, {"type", "domain"});
    // Releasing what host state holds (payload destructors may not call guest code).
    method(n.path_class, "dealloc", [this, provider](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<Path>(s.x[0])) {
            for (const auto object : std::exchange(state->interfaces, {})) release(object);
        }
        static_cast<void>(objc.send_super(s.x[0], provider->path_class, objc.selector("dealloc")));
    }, false, "v@:");
    method(n.monitor_class, "dealloc", [this, provider](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<Monitor>(s.x[0])) {
            Address queue{}, update{}, cancel{};
            {
                std::lock_guard lock(state->mutex);
                queue = std::exchange(state->queue, 0);
                update = std::exchange(state->update, 0);
                cancel = std::exchange(state->cancel, 0);
            }
            if (queue) static_cast<void>(call(fill::libsystem, "_dispatch_release", {queue}));
            if (update) bridge->block_release(update);
            if (cancel) bridge->block_release(cancel);
        }
        static_cast<void>(objc.send_super(s.x[0], provider->monitor_class, objc.selector("dealloc")));
    }, false, "v@:");

    // ---- path monitor ----
    raw(network, "_nw_path_monitor_create", [this, provider](CpuState& s, GuestMemory&) {
        const auto object = objc.allocate(provider->monitor_class);
        static_cast<void>(objc.host_state<Monitor>(object));
        s.x[0] = object;
    });
    raw(network, "_nw_path_monitor_create_with_type", [this, provider](CpuState& s, GuestMemory&) {
        // Interface-type monitors watch the same adapter set (types are not filtered).
        const auto object = objc.allocate(provider->monitor_class);
        static_cast<void>(objc.host_state<Monitor>(object));
        s.x[0] = object;
    });
    raw(network, "_nw_path_monitor_set_queue", [this](CpuState& s, GuestMemory&) {
        const auto state = monitor_of(objc, s.x[0]);
        if (s.x[1]) static_cast<void>(call(fill::libsystem, "_dispatch_retain", {s.x[1]}));
        Address previous{};
        {
            std::lock_guard lock(state->mutex);
            previous = std::exchange(state->queue, s.x[1]);
        }
        if (previous) static_cast<void>(call(fill::libsystem, "_dispatch_release", {previous}));
    });
    const auto set_block = [this](Address object, Address block, Address Monitor::*slot) {
        const auto state = monitor_of(objc, object);
        const auto copied = block ? bridge->block_copy(block) : 0;
        Address previous{};
        {
            std::lock_guard lock(state->mutex);
            previous = std::exchange((*state).*slot, copied);
        }
        if (previous) bridge->block_release(previous);
    };
    raw(network, "_nw_path_monitor_set_update_handler", [set_block](CpuState& s, GuestMemory&) { set_block(s.x[0], s.x[1], &Monitor::update); });
    raw(network, "_nw_path_monitor_set_cancel_handler", [set_block](CpuState& s, GuestMemory&) { set_block(s.x[0], s.x[1], &Monitor::cancel); });
    raw(network, "_nw_path_monitor_start", [this, provider](CpuState& s, GuestMemory&) {
        const auto state = monitor_of(objc, s.x[0]);
        std::uint64_t generation{};
        {
            std::lock_guard lock(state->mutex);
            if (state->started || state->cancelled) return;
            state->started = true;
            generation = ++state->generation;
        }
        const auto monitor = s.x[0];
        const auto interfaces = host_interfaces();
        provider->deliver(monitor, state, interfaces);
        static_cast<void>(retain(monitor));   // released by the watcher when it ends
        const auto worker = call(fill::libsystem, "_dispatch_get_global_queue", {0, 0});
        bridge->dispatch_async(worker, [provider, monitor, state, generation, interfaces] {
            provider->watch(monitor, state, generation, interfaces);
        });
    });
    raw(network, "_nw_path_monitor_cancel", [this](CpuState& s, GuestMemory&) {
        const auto state = monitor_of(objc, s.x[0]);
        Address cancel{}, queue{};
        {
            std::lock_guard lock(state->mutex);
            if (state->cancelled) return;
            state->cancelled = true;
            ++state->generation;
            cancel = std::exchange(state->cancel, 0);
            queue = state->queue;
        }
        if (!cancel) return;
        if (!queue) { bridge->block_release(cancel); return; }
        bridge->dispatch_async(queue, [this, cancel] {
            static_cast<void>(bridge->invoke_block(cancel, {}));
            bridge->block_release(cancel);
        });
    });

    // ---- paths and interfaces ----
    raw(network, "_nw_path_get_status", [this](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? objc.find_host_state<Path>(s.x[0]) : nullptr;
        s.x[0] = state ? state->status : 0;   // nw_path_status_invalid
    });
    raw(network, "_nw_path_enumerate_interfaces", [this](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? objc.find_host_state<Path>(s.x[0]) : nullptr;
        if (!state || !s.x[1]) return;
        const auto interfaces = state->interfaces;   // the path is immutable
        for (const auto object : interfaces)
            if (!(bridge->invoke_block(s.x[1], {object}) & 0xff)) break;
    });
    raw(network, "_nw_path_uses_interface_type", [this](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? objc.find_host_state<Path>(s.x[0]) : nullptr;
        bool found = false;
        if (state) for (const auto object : state->interfaces)
            if (const auto value = objc.find_host_state<Interface>(object); value && value->value.type == s.x[1]) found = true;
        s.x[0] = found;
    });
    for (const auto* name : {"_nw_path_is_expensive", "_nw_path_is_constrained", "_nw_path_has_dns"})
        constant(network, name, 0);
    raw(network, "_nw_interface_get_type", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<Interface>(s.x[0]) : nullptr;
        s.x[0] = value ? value->value.type : interface_other;
    });
    raw(network, "_nw_interface_get_name", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<Interface>(s.x[0]) : nullptr;
        s.x[0] = value ? value->name : 0;
    });
    raw(network, "_nw_interface_get_index", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<Interface>(s.x[0]) : nullptr;
        s.x[0] = value ? value->value.index : 0;
    });

    // ---- errors ----
    raw(network, "_nw_error_get_error_code", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<Error>(s.x[0]) : nullptr;
        s.x[0] = value ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value->code))) : 0;
    });
    raw(network, "_nw_error_get_error_domain", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<Error>(s.x[0]) : nullptr;
        s.x[0] = value ? value->domain : 0;
    });

    // ---- parameters: values only ----
    // NW_PARAMETERS_DEFAULT_CONFIGURATION / NW_PARAMETERS_DISABLE_PROTOCOL are
    // global blocks; configuring a protocol with them does nothing here.
    value(network, "__nw_parameters_configure_protocol_default_configuration", bridge->make_block([](CpuState&, GuestMemory&) {}));
    value(network, "__nw_parameters_configure_protocol_disable", bridge->make_block([](CpuState&, GuestMemory&) {}));
    raw(network, "_nw_parameters_create_secure_tcp", [this, provider](CpuState& s, GuestMemory&) {
        s.x[0] = objc.allocate(provider->parameters_class);
    });
    raw(network, "_nw_parameters_create_secure_udp", [this, provider](CpuState& s, GuestMemory&) {
        s.x[0] = objc.allocate(provider->parameters_class);
    });
    raw(network, "_nw_parameters_create", [this, provider](CpuState& s, GuestMemory&) { s.x[0] = objc.allocate(provider->parameters_class); });

    // ---- Bonjour browsing: no mDNS responder ----
    raw(network, "_nw_browse_descriptor_create_bonjour_service", [this, provider](CpuState& s, GuestMemory& m) {
        const auto object = objc.allocate(provider->descriptor_class);
        const auto type = cf.string(read_cstring(m, s.x[0]));
        store(object, "type", type);
        release(type);
        if (s.x[1]) {
            const auto domain = cf.string(read_cstring(m, s.x[1]));
            store(object, "domain", domain);
            release(domain);
        }
        s.x[0] = object;
    });
    raw(network, "_nw_browser_create", [this, provider](CpuState& s, GuestMemory&) {
        const auto object = objc.allocate(provider->browser_class);
        auto state = std::make_shared<Browser>();
        state->descriptor = retain(s.x[0]);
        state->parameters = retain(s.x[1]);
        static_cast<void>(objc.host_state<Browser>(object, [&] { return state; }));
        s.x[0] = object;
    });
    method(n.browser_class, "dealloc", [this, provider](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<Browser>(s.x[0])) {
            std::vector<Address> objects, blocks;
            Address queue{};
            {
                std::lock_guard lock(state->mutex);
                objects = {std::exchange(state->descriptor, 0), std::exchange(state->parameters, 0)};
                blocks = {std::exchange(state->state_handler, 0), std::exchange(state->results_handler, 0)};
                queue = std::exchange(state->queue, 0);
            }
            for (const auto object : objects) release(object);
            for (const auto block : blocks) if (block) bridge->block_release(block);
            if (queue) static_cast<void>(call(fill::libsystem, "_dispatch_release", {queue}));
        }
        static_cast<void>(objc.send_super(s.x[0], provider->browser_class, objc.selector("dealloc")));
    }, false, "v@:");
    raw(network, "_nw_browser_set_queue", [this](CpuState& s, GuestMemory&) {
        const auto state = browser_of(objc, s.x[0]);
        if (s.x[1]) static_cast<void>(call(fill::libsystem, "_dispatch_retain", {s.x[1]}));
        Address previous{};
        {
            std::lock_guard lock(state->mutex);
            previous = std::exchange(state->queue, s.x[1]);
        }
        if (previous) static_cast<void>(call(fill::libsystem, "_dispatch_release", {previous}));
    });
    const auto set_browser_block = [this](Address object, Address block, Address Browser::*slot) {
        const auto state = browser_of(objc, object);
        const auto copied = block ? bridge->block_copy(block) : 0;
        Address previous{};
        {
            std::lock_guard lock(state->mutex);
            previous = std::exchange((*state).*slot, copied);
        }
        if (previous) bridge->block_release(previous);
    };
    raw(network, "_nw_browser_set_state_changed_handler", [set_browser_block](CpuState& s, GuestMemory&) {
        set_browser_block(s.x[0], s.x[1], &Browser::state_handler);
    });
    raw(network, "_nw_browser_set_browse_results_changed_handler", [set_browser_block](CpuState& s, GuestMemory&) {
        set_browser_block(s.x[0], s.x[1], &Browser::results_handler);   // never called: no results
    });
    // Reports `browser_state` (with an error for failed) to the state handler.
    const auto report = [this, provider](Address object, const std::shared_ptr<Browser>& state, std::uint64_t browser_state) {
        Address queue{};
        {
            std::lock_guard lock(state->mutex);
            queue = state->queue;
            if (!queue || !state->state_handler) return;
        }
        static_cast<void>(retain(object));
        bridge->dispatch_async(queue, [this, provider, object, state, browser_state] {
            Address handler{};
            {
                std::lock_guard lock(state->mutex);
                handler = state->state_handler ? bridge->block_copy(state->state_handler) : 0;
            }
            if (handler) {
                Address error{};
                if (browser_state == browser_failed) {
                    error = objc.allocate(provider->error_class);
                    auto value = std::make_shared<Error>();
                    value->domain = error_domain_dns;
                    value->code = dns_service_not_running;
                    static_cast<void>(objc.host_state<Error>(error, [&] { return value; }));
                }
                static_cast<void>(bridge->invoke_block(handler, {browser_state, error}));
                release(error);
                bridge->block_release(handler);
            }
            release(object);
        });
    };
    raw(network, "_nw_browser_start", [this, report](CpuState& s, GuestMemory&) {
        const auto state = browser_of(objc, s.x[0]);
        {
            std::lock_guard lock(state->mutex);
            if (state->started || state->cancelled) return;
            state->started = true;
        }
        report(s.x[0], state, browser_failed);
    });
    raw(network, "_nw_browser_cancel", [this, report](CpuState& s, GuestMemory&) {
        const auto state = browser_of(objc, s.x[0]);
        {
            std::lock_guard lock(state->mutex);
            if (state->cancelled) return;
            state->cancelled = true;
        }
        report(s.x[0], state, browser_cancelled);
    });
    // Browse results and Bonjour endpoints: none is ever created, so these
    // only see NULL or a non-Bonjour object.
    constant(network, "_nw_browse_result_copy_endpoint", 0);
    constant(network, "_nw_browse_result_get_changes", 0);   // nw_browse_result_change_invalid
    constant(network, "_nw_endpoint_get_bonjour_service_name", 0);
}

} // namespace alchemy::gui::values
