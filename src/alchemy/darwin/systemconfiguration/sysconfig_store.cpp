#include "alchemy/darwin/network/network_internal.hpp"

#include "alchemy/darwin_threads.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/system.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <regex>
#include <thread>
#include <variant>


// SystemConfiguration's dynamic store and reachability, answered from the
// host's network state (host::net) (owner: v-netsec; the proxy part of the store is the
// sysconfig provider's SCDynamicStoreCopyProxies, which accepts these stores).
// Each export is installed only when no other provider exports it.
//
// Dynamic store keys with values (anything else reads as absent, NULL):
//   State:/Network/Global/IPv4|IPv6      PrimaryInterface, PrimaryService, Router
//   State:/Network/Global/DNS            ServerAddresses, DomainName (the primary
//                                        IPv4 adapter's DNS servers and suffix)
//   State:/Network/Interface             Interfaces
//   State:/Network/Interface/<if>/IPv4   Addresses, SubnetMasks
//   State:/Network/Interface/<if>/IPv6   Addresses, PrefixLength
//   Setup:/System                        ComputerName, ComputerNameEncoding
//   Setup:/Network/HostNames             LocalHostName
//   State:/Users/ConsoleUser             Name, UID, GID
// Interface names are Alchemy's guest names (lo / if<index>, as getifaddrs and
// if_nametoindex report them); services are the Windows adapter GUIDs.
// Notifications: with SCDynamicStoreSetDispatchQueue or
// SCDynamicStoreCreateRunLoopSource, a guest dispatch worker re-reads the
// watched keys (SetNotificationKeys keys and patterns) every second; the keys
// whose values changed reach the callout on the queue, or through the store's
// run-loop source (signalled, with the run loops it is scheduled on woken).
//
// SCNetworkReachability (CreateWithName / CreateWithAddress / GetFlags):
// the target is reachable when it resolves and Windows has a route to one of
// its addresses (GetBestInterfaceEx); loopback is local and direct. With a
// callback and SetDispatchQueue or ScheduleWithRunLoop, a guest dispatch
// worker re-reads the flags every second and delivers each change (on the
// queue, or through a run-loop source signalled on the scheduled loops). The
// failure returns (bad context version, not scheduled there) leave SCError
// as it was.
namespace alchemy::gui::network {
namespace {

constexpr std::string_view lib = "/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration";
constexpr std::string_view libsystem = security::libsystem_library;
constexpr std::uint32_t guest_uid = 1000;   // Alchemy's virtual Darwin identity (getuid)

// ---- host values ---------------------------------------------------------------------------
using Value = std::variant<std::string, std::int64_t, std::vector<std::string>, std::vector<std::int64_t>>;
using Entity = std::map<std::string, Value>;
using Snapshot = std::map<std::string, Entity>;

std::string address_text(const host::net::Address& address) { return host::net::host_text(address); }

std::string mask_text(unsigned prefix) {
    const auto mask = prefix == 0 ? 0U : 0xffffffffU << (32 - std::min(prefix, 32U));
    return std::to_string(mask >> 24) + "." + std::to_string((mask >> 16) & 255) + "." + std::to_string((mask >> 8) & 255) + "." +
           std::to_string(mask & 255);
}

std::string computer_name() { return host::system::host_name().value_or({}); }

std::string user_name() { return host::system::user_name().value_or({}); }

// The interface the host routes IPv4 or IPv6 traffic to the Internet through.
std::optional<std::uint32_t> best_interface(bool ipv6) {
    const auto target = ipv6 ? host::net::Address::ipv6({0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88})
                             : host::net::Address::ipv4({8, 8, 8, 8});
    const auto index = host::net::best_interface(target);
    if (!index) return std::nullopt;
    return *index;
}

// A DNS suffix as SystemConfiguration reported it: ASCII, '?' for each other character.
std::string ascii(std::string_view utf8) {
    std::string text;
    for (const auto c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) text += c;
        else if ((byte & 0xc0) != 0x80) text += '?';   // a lead byte; continuation bytes are skipped
    }
    return text;
}

Snapshot snapshot() {
    Snapshot result;
    if (const auto name = computer_name(); !name.empty()) {
        result["Setup:/System"] = {{"ComputerName", name}, {"ComputerNameEncoding", std::int64_t{0x08000100}}};
        result["Setup:/Network/HostNames"] = {{"LocalHostName", name}};
    }
    if (const auto user = user_name(); !user.empty())
        result["State:/Users/ConsoleUser"] = {{"Name", user}, {"UID", std::int64_t{guest_uid}}, {"GID", std::int64_t{guest_uid}}};

    const auto adapters = host::net::interfaces();
    if (!adapters) return result;
    using host::net::Family;
    const auto v4 = best_interface(false), v6 = best_interface(true);
    std::vector<std::string> names;
    for (const auto& adapter : *adapters) {
        const auto index = adapter.index;
        if (!index || !adapter.up) continue;
        const auto name = std::string(adapter.kind == host::net::InterfaceKind::loopback ? "lo" : "if") + std::to_string(index);
        names.push_back(name);
        std::vector<std::string> v4_addresses, v4_masks, v6_addresses;
        std::vector<std::int64_t> v6_prefixes;
        for (const auto& assigned : adapter.addresses) {
            if (assigned.address.family == Family::ipv4) {
                v4_addresses.push_back(address_text(assigned.address));
                v4_masks.push_back(mask_text(assigned.prefix_length));
            } else if (assigned.address.family == Family::ipv6) {
                v6_addresses.push_back(address_text(assigned.address));
                v6_prefixes.push_back(assigned.prefix_length);
            }
        }
        const auto base = "State:/Network/Interface/" + name;
        if (!v4_addresses.empty()) result[base + "/IPv4"] = {{"Addresses", v4_addresses}, {"SubnetMasks", v4_masks}};
        if (!v6_addresses.empty()) result[base + "/IPv6"] = {{"Addresses", v6_addresses}, {"PrefixLength", v6_prefixes}};
        const auto service = adapter.id.empty() ? name : adapter.id;
        const auto global = [&](Family family, const char* key) {
            Entity entity{{"PrimaryInterface", name}, {"PrimaryService", service}};
            for (const auto& gateway : adapter.gateways)
                if (gateway.family == family) { entity["Router"] = address_text(gateway); break; }
            result[key] = std::move(entity);
        };
        if (v4 && *v4 == index) {
            global(Family::ipv4, "State:/Network/Global/IPv4");
            const auto dns = host::net::dns_configuration(index);
            std::vector<std::string> servers;
            if (dns) for (const auto& server : dns->servers) servers.push_back(address_text(server));
            Entity entity{{"ServerAddresses", servers}};
            if (dns && !dns->search.empty()) entity["DomainName"] = ascii(dns->search.front());
            if (!servers.empty()) result["State:/Network/Global/DNS"] = std::move(entity);
        }
        if (v6 && *v6 == adapter.ipv6_index) global(Family::ipv6, "State:/Network/Global/IPv6");
    }
    result["State:/Network/Interface"] = {{"Interfaces", names}};
    return result;
}

// ---- guest objects -----------------------------------------------------------------------------
struct StoreState final : ObjectPayload {
    std::mutex mutex;
    Address callout{}, info{}, release_info{};
    std::vector<std::string> keys, patterns;
    Address queue{};                          // +1 dispatch queue (notifications)
    std::uint64_t generation{};               // bumps when the queue changes (stops a watcher)
    bool watching{};
    Address source{};                         // +1 run-loop source (SCDynamicStoreCreateRunLoopSource)
    std::vector<Address> loops;               // run loops the source is scheduled on
    std::vector<std::string> pending;         // changed keys awaiting the source's perform
};

struct ReachabilityState final : ObjectPayload {
    std::string name;                         // host name, or empty
    Bytes address;                            // Darwin sockaddr, or empty
    std::mutex mutex;
    Address callout{}, info{}, release_info{};
    Address queue{};                          // +1 dispatch queue (SetDispatchQueue)
    Address source{};                         // +1 run-loop source (ScheduleWithRunLoop)
    std::vector<std::pair<Address, Address>> scheduled;   // (run loop, +1 mode)
    std::uint64_t generation{};               // bumps when delivery stops (ends a watcher)
    bool watching{};
    std::optional<std::uint32_t> pending;     // flags awaiting the source's perform
};

class Store : public std::enable_shared_from_this<Store> {
public:
    explicit Store(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)) {}
    std::shared_ptr<Bridge> b;
    Address store_class{}, reachability_class{};

    std::shared_ptr<StoreState> state(Address object) const {
        auto value = object ? b->objc.find_host_state<StoreState>(object) : nullptr;
        if (!value) security::fail("expected an SCDynamicStoreRef");
        return value;
    }

    // +1 CF value of an entity.
    Address make(const Entity& entity) {
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        for (const auto& [name, value] : entity) {
            const auto key = b->string(name);
            Address object{};
            if (const auto* text = std::get_if<std::string>(&value)) object = b->string(*text);
            else if (const auto* number = std::get_if<std::int64_t>(&value)) object = b->number(*number);
            else if (const auto* texts = std::get_if<std::vector<std::string>>(&value)) {
                std::vector<Address> items;
                for (const auto& item : *texts) items.push_back(b->string(item));
                object = b->array(items);
                b->release_all(items);
            } else {
                std::vector<Address> items;
                for (const auto item : std::get<std::vector<std::int64_t>>(value)) items.push_back(b->number(item));
                object = b->array(items);
                b->release_all(items);
            }
            entries.emplace_back(key, object);
            owned.push_back(key); owned.push_back(object);
        }
        const auto result = b->dictionary(entries);
        b->release_all(owned);
        return result;
    }

    std::vector<std::string> strings(Address array) {
        std::vector<std::string> result;
        if (!array || !b->is_array(array)) return result;
        for (const auto item : b->cf.array_values(array))
            if (const auto text = b->maybe_text(item)) result.push_back(*text);
        return result;
    }

    // The watched part of a snapshot.
    static std::map<std::string, Entity> watched(const Snapshot& all, const std::vector<std::string>& keys,
                                                 const std::vector<std::string>& patterns) {
        std::map<std::string, Entity> result;
        for (const auto& key : keys) if (const auto found = all.find(key); found != all.end()) result.insert(*found);
        for (const auto& pattern : patterns) {
            std::regex expression;
            try { expression = std::regex(pattern, std::regex::extended); } catch (const std::regex_error&) { continue; }
            for (const auto& [key, value] : all) if (std::regex_search(key, expression)) result.emplace(key, value);
        }
        return result;
    }

    // Notification watcher (guest dispatch worker) for one store and queue generation.
    void watch(Address object, std::shared_ptr<StoreState> value, std::uint64_t generation) {
        std::vector<std::string> initial_keys, initial_patterns;
        {
            std::lock_guard lock(value->mutex);
            initial_keys = value->keys; initial_patterns = value->patterns;
        }
        auto previous = watched(snapshot(), initial_keys, initial_patterns);
        for (;;) {
            for (int slice = 0; slice < 20; ++slice) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (guest_worker_must_stop(b->process)) {
                    std::lock_guard lock(value->mutex);
                    value->watching = false;
                    throw GuestThreadStop{};
                }
                bool ended{};
                {
                    std::lock_guard lock(value->mutex);
                    if (value->generation != generation) { value->watching = false; ended = true; }
                }
                // Released outside the lock: the last reference runs dealloc, which takes it.
                if (ended) { b->release(object); return; }
            }
            std::vector<std::string> keys, patterns;
            Address queue{}, source{};
            {
                std::lock_guard lock(value->mutex);
                keys = value->keys; patterns = value->patterns; queue = value->queue; source = value->source;
            }
            const auto current = watched(snapshot(), keys, patterns);
            std::vector<std::string> changed;
            for (const auto& [key, entity] : current) if (const auto found = previous.find(key); found == previous.end() || found->second != entity) changed.push_back(key);
            for (const auto& [key, entity] : previous) if (!current.contains(key)) changed.push_back(key);
            previous = current;
            if (changed.empty()) continue;
            if (!queue && source) {
                std::vector<Address> loops;
                {
                    std::lock_guard lock(value->mutex);
                    value->pending.insert(value->pending.end(), changed.begin(), changed.end());
                    loops = value->loops;
                }
                static_cast<void>(b->call(alchemy::core_foundation_library, "_CFRunLoopSourceSignal", {source}));
                for (const auto loop : loops) static_cast<void>(b->call(alchemy::core_foundation_library, "_CFRunLoopWakeUp", {loop}));
                continue;
            }
            if (!queue) continue;
            const auto self = shared_from_this();
            static_cast<void>(b->retain(object));
            b->dispatch_async(queue, [self, object, value, changed] {
                Address callout{}, info{};
                {
                    std::lock_guard lock(value->mutex);
                    callout = value->callout; info = value->info;
                }
                std::vector<Address> names;
                for (const auto& key : changed) names.push_back(self->b->string(key));
                const auto list = self->b->array(names);
                self->b->release_all(names);
                try {
                    // void (*)(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
                    if (callout) static_cast<void>(self->b->call_address(callout, std::array{object, list, info}));
                } catch (...) { self->b->release(list); self->b->release(object); throw; }
                self->b->release(list);
                self->b->release(object);
            });
        }
    }

    // GetFlags for a reachability target.
    std::uint32_t reachability(const ReachabilityState& target) {
        constexpr std::uint32_t reachable = 1U << 1, is_local = 1U << 16, is_direct = 1U << 17;
        std::vector<host::net::Address> addresses;
        if (!target.address.empty()) {
            // Darwin sockaddr: len, family (AF_INET 2 / AF_INET6 30), then the address.
            const auto family = target.address.size() > 1 ? target.address[1] : 0;
            if (family == 2 && target.address.size() >= 8) {
                std::array<std::uint8_t, 4> bytes{};
                std::memcpy(bytes.data(), target.address.data() + 4, 4);
                addresses.push_back(host::net::Address::ipv4(bytes));
            } else if (family == 30 && target.address.size() >= 24) {
                std::array<std::uint8_t, 16> bytes{};
                std::memcpy(bytes.data(), target.address.data() + 8, 16);
                addresses.push_back(host::net::Address::ipv6(bytes));
            } else {
                return 0;
            }
        } else {
            static_cast<void>(host::net::initialize());
            if (const auto found = host::net::resolve(target.name, std::nullopt, {}))
                for (const auto& entry : *found) addresses.push_back(entry.address);
        }
        std::uint32_t flags = 0;
        for (const auto& address : addresses) {
            constexpr std::array<std::uint8_t, 16> v6_loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            const bool loopback = address.family == host::net::Family::ipv4 ? address.bytes[0] == 127 : address.bytes == v6_loopback;
            if (loopback) return reachable | is_local | is_direct;
            if (host::net::best_interface(address)) flags = reachable;
        }
        return flags;
    }

    // Reachability watcher (guest dispatch worker): re-reads the target's
    // flags every second while a queue or run loop is set and delivers each
    // change to the callout.
    void watch_reachability(Address object, std::shared_ptr<ReachabilityState> value, std::uint64_t generation) {
        auto previous = reachability(*value);
        for (;;) {
            for (int slice = 0; slice < 20; ++slice) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (guest_worker_must_stop(b->process)) {
                    std::lock_guard lock(value->mutex);
                    value->watching = false;
                    throw GuestThreadStop{};
                }
                bool ended{};
                {
                    std::lock_guard lock(value->mutex);
                    ended = value->generation != generation;   // update_watcher cleared `watching`
                }
                // Released outside the lock: the last reference runs dealloc, which takes it.
                if (ended) { b->release(object); return; }
            }
            const auto flags = reachability(*value);
            if (flags == previous) continue;
            previous = flags;
            deliver(object, value, flags);
        }
    }

    // A flags change: through the run-loop source on the scheduled loops, and
    // to the callout on the dispatch queue.
    void deliver(Address object, const std::shared_ptr<ReachabilityState>& value, std::uint32_t flags) {
        Address queue{}, source{};
        std::vector<Address> loops;
        {
            std::lock_guard lock(value->mutex);
            queue = value->queue; source = value->source;
            for (const auto& [loop, mode] : value->scheduled) loops.push_back(loop);
            if (source && !loops.empty()) value->pending = flags;
        }
        if (source && !loops.empty()) {
            static_cast<void>(b->call(alchemy::core_foundation_library, "_CFRunLoopSourceSignal", {source}));
            for (const auto loop : loops) static_cast<void>(b->call(alchemy::core_foundation_library, "_CFRunLoopWakeUp", {loop}));
        }
        if (!queue) return;
        const auto self = shared_from_this();
        static_cast<void>(b->retain(object));
        b->dispatch_async(queue, [self, object, value, flags] {
            Address callout{}, info{};
            {
                std::lock_guard lock(value->mutex);
                callout = value->callout; info = value->info;
            }
            try {
                // void (*)(SCNetworkReachabilityRef target, SCNetworkReachabilityFlags flags, void *info)
                if (callout) static_cast<void>(self->b->call_address(callout, std::array<Address, 3>{object, flags, info}));
            } catch (...) { self->b->release(object); throw; }
            self->b->release(object);
        });
    }

    // Starts the watcher when delivery is set up, or stops it when neither a
    // queue nor a run loop remains.
    void update_watcher(Address object, const std::shared_ptr<ReachabilityState>& value) {
        std::uint64_t generation{};
        bool start{};
        {
            std::lock_guard lock(value->mutex);
            const bool wanted = value->queue || !value->scheduled.empty();
            if (wanted && !value->watching) { start = true; value->watching = true; generation = value->generation; }
            else if (!wanted && value->watching) { value->watching = false; ++value->generation; }
        }
        if (!start) return;
        static_cast<void>(b->retain(object));   // released by the watcher when it ends
        const auto self = shared_from_this();
        const auto worker = b->call(libsystem, "_dispatch_get_global_queue", {0, 0});
        b->dispatch_async(worker, [self, object, value, generation] { self->watch_reachability(object, value, generation); });
    }
};

} // namespace

void install_system_configuration_store(const std::shared_ptr<Bridge>& b) {
    const auto self = std::make_shared<Store>(b);
    static std::shared_ptr<Store> keep;
    keep = self;
    const auto install = [b](std::string name, transmute::GuestCallHandler handler) {
        if (!b->exported(lib, name)) b->function(lib, name, std::move(handler));
    };
    const auto constant = [b](std::string_view name, std::string_view value) {
        const auto symbol = "_" + std::string(name);
        if (!b->exported(lib, symbol)) static_cast<void>(b->export_string(lib, symbol, value));
    };
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"kSCDynamicStoreDomainState", "State:"}, {"kSCDynamicStoreDomainSetup", "Setup:"},
             {"kSCDynamicStoreUseSessionKeys", "UseSessionKeys"},   // an SCDynamicStoreCreateWithOptions option (no per-session keys exist)
             {"kSCDynamicStoreDomainFile", "File:"}, {"kSCDynamicStoreDomainPlugin", "Plugin:"}, {"kSCDynamicStoreDomainPrefs", "Prefs:"},
             {"kSCEntNetIPv4", "IPv4"}, {"kSCEntNetIPv6", "IPv6"}, {"kSCEntNetInterface", "Interface"},
             {"kSCDynamicStorePropNetPrimaryInterface", "PrimaryInterface"}, {"kSCDynamicStorePropNetPrimaryService", "PrimaryService"},
             {"kSCDynamicStorePropNetInterfaces", "Interfaces"}, {"kSCPropNetIPv4Router", "Router"}, {"kSCPropNetIPv6Router", "Router"},
             {"kSCPropNetIPv4Addresses", "Addresses"}, {"kSCPropNetIPv4SubnetMasks", "SubnetMasks"},
             {"kSCPropNetIPv6Addresses", "Addresses"}, {"kSCPropNetIPv6PrefixLength", "PrefixLength"},
             {"kSCPropSystemComputerName", "ComputerName"}, {"kSCPropSystemComputerNameEncoding", "ComputerNameEncoding"},
             {"kSCPropNetLocalHostName", "LocalHostName"}, {"kSCCompNetwork", "Network"}, {"kSCCompGlobal", "Global"},
             {"kSCCompInterface", "Interface"}, {"kSCCompService", "Service"}, {"kSCCompSystem", "System"},
             {"kSCCompAnyRegex", "[^/]+"}})
        constant(name, value);

    self->store_class = b->define_class("_AlchemySCDynamicStore", b->object_class, lib);
    self->reachability_class = b->define_class("_AlchemySCNetworkReachability", b->object_class, lib);
    b->method(self->store_class, "dealloc", [self](CpuState& s, GuestMemory&) {
        if (const auto value = self->b->objc.find_host_state<StoreState>(s.x[0])) {
            Address queue{}, info{}, release_info{};
            {
                std::lock_guard lock(value->mutex);
                ++value->generation;
                queue = std::exchange(value->queue, 0);
                info = std::exchange(value->info, 0);
                release_info = value->release_info;
            }
            if (queue) static_cast<void>(self->b->call(libsystem, "_dispatch_release", {queue}));
            if (info && release_info) static_cast<void>(self->b->call_address(release_info, std::array{info}));
            Address source{};
            { std::lock_guard lock(value->mutex); source = std::exchange(value->source, 0); }
            self->b->release(source);
        }
        static_cast<void>(self->b->objc.send_super(s.x[0], self->store_class, self->b->objc.selector("dealloc")));
    }, false, "v@:");

    // SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef name, SCDynamicStoreCallBack, SCDynamicStoreContext *)
    install("_SCDynamicStoreCreate", [self](CpuState& s, GuestMemory& m) {
        auto value = std::make_shared<StoreState>();
        value->callout = s.x[2];
        if (const auto context = s.x[3]) {
            if (m.read64(context) != 0) security::fail("unsupported SCDynamicStoreContext version");
            value->info = m.read64(context + 8);
            const auto retain_info = m.read64(context + 16);
            value->release_info = m.read64(context + 24);
            if (value->info && retain_info) value->info = self->b->call_address(retain_info, std::array{value->info});
        }
        const auto object = self->b->objc.allocate(self->store_class);
        static_cast<void>(self->b->objc.host_state<StoreState>(object, [&] { return value; }));
        s.x[0] = object;
    });
    install("_SCDynamicStoreCreateWithOptions", [self](CpuState& s, GuestMemory& m) {
        // (allocator, name, storeOptions, callout, context)
        auto value = std::make_shared<StoreState>();
        value->callout = s.x[3];
        if (const auto context = s.x[4]) {
            if (m.read64(context) != 0) security::fail("unsupported SCDynamicStoreContext version");
            value->info = m.read64(context + 8);
            const auto retain_info = m.read64(context + 16);
            value->release_info = m.read64(context + 24);
            if (value->info && retain_info) value->info = self->b->call_address(retain_info, std::array{value->info});
        }
        const auto object = self->b->objc.allocate(self->store_class);
        static_cast<void>(self->b->objc.host_state<StoreState>(object, [&] { return value; }));
        s.x[0] = object;
    });
    // CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef, CFStringRef key)
    install("_SCDynamicStoreCopyValue", [self](CpuState& s, GuestMemory&) {
        const auto key = self->b->maybe_text(s.x[1]);
        s.x[0] = 0;
        if (!key) return;
        const auto all = snapshot();
        if (const auto found = all.find(*key); found != all.end()) s.x[0] = self->make(found->second);
    });
    // CFDictionaryRef SCDynamicStoreCopyMultiple(SCDynamicStoreRef, CFArrayRef keys, CFArrayRef patterns)
    install("_SCDynamicStoreCopyMultiple", [self](CpuState& s, GuestMemory&) {
        const auto values = Store::watched(snapshot(), self->strings(s.x[1]), self->strings(s.x[2]));
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        for (const auto& [key, entity] : values) {
            entries.emplace_back(self->b->string(key), self->make(entity));
            owned.push_back(entries.back().first); owned.push_back(entries.back().second);
        }
        s.x[0] = self->b->dictionary(entries);
        self->b->release_all(owned);
    });
    // CFArrayRef SCDynamicStoreCopyKeyList(SCDynamicStoreRef, CFStringRef pattern)
    install("_SCDynamicStoreCopyKeyList", [self](CpuState& s, GuestMemory&) {
        const auto pattern = self->b->maybe_text(s.x[1]);
        std::vector<Address> names;
        if (pattern)
            for (const auto& [key, entity] : Store::watched(snapshot(), {}, {*pattern})) names.push_back(self->b->string(key));
        s.x[0] = self->b->array(names);
        self->b->release_all(names);
    });
    // Boolean SCDynamicStoreSetNotificationKeys(SCDynamicStoreRef, CFArrayRef keys, CFArrayRef patterns)
    install("_SCDynamicStoreSetNotificationKeys", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state(s.x[0]);
        auto keys = self->strings(s.x[1]), patterns = self->strings(s.x[2]);
        std::lock_guard lock(value->mutex);
        value->keys = std::move(keys); value->patterns = std::move(patterns);
        s.x[0] = 1;
    });
    // CFRunLoopSourceRef SCDynamicStoreCreateRunLoopSource(CFAllocatorRef, SCDynamicStoreRef, CFIndex order):
    // a version 0 source whose info is the store; schedule/cancel record the
    // run loops it joins, perform calls the callout with the changed keys.
    const auto schedule = self->b->process.install_function("__alchemy_sc_store_schedule", [self](CpuState& s, GuestMemory&) {
        // schedule(info, CFRunLoopRef, mode)
        const auto value = self->b->objc.find_host_state<StoreState>(s.x[0]);
        if (!value) return;
        std::lock_guard lock(value->mutex);
        if (std::find(value->loops.begin(), value->loops.end(), s.x[1]) == value->loops.end()) value->loops.push_back(s.x[1]);
    }, "alchemy.internal");
    const auto cancel = self->b->process.install_function("__alchemy_sc_store_cancel", [self](CpuState& s, GuestMemory&) {
        const auto value = self->b->objc.find_host_state<StoreState>(s.x[0]);
        if (!value) return;
        std::lock_guard lock(value->mutex);
        std::erase(value->loops, s.x[1]);
    }, "alchemy.internal");
    const auto perform = self->b->process.install_function("__alchemy_sc_store_perform", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto value = self->b->objc.find_host_state<StoreState>(object);
        if (!value) return;
        std::vector<std::string> changed;
        Address callout{}, info{};
        {
            std::lock_guard lock(value->mutex);
            changed.swap(value->pending);
            callout = value->callout; info = value->info;
        }
        if (changed.empty() || !callout) return;
        std::vector<Address> names;
        for (const auto& key : changed) names.push_back(self->b->string(key));
        const auto list = self->b->array(names);
        self->b->release_all(names);
        try {
            static_cast<void>(self->b->call_address(callout, std::array{object, list, info}));
        } catch (...) { self->b->release(list); throw; }
        self->b->release(list);
    }, "alchemy.internal");
    install("_SCDynamicStoreCreateRunLoopSource", [self, schedule, cancel, perform](CpuState& s, GuestMemory& m) {
        const auto object = s.x[1];
        const auto value = self->state(object);
        // CFRunLoopSourceContext (version 0): version, info, retain, release,
        // copyDescription, equal, hash, schedule, cancel, perform.
        const auto context = self->b->process.allocate_guest(80, 16);
        m.fill_bytes(context, 0, 80);
        m.write64(context + 8, object);
        m.write64(context + 56, schedule);
        m.write64(context + 64, cancel);
        m.write64(context + 72, perform);
        const auto source = self->b->call(alchemy::core_foundation_library, "_CFRunLoopSourceCreate", {s.x[0], s.x[2], context});
        self->b->process.release_guest(context);
        if (!source) { s.x[0] = 0; return; }
        Address previous{};
        std::uint64_t generation{};
        bool start{};
        {
            std::lock_guard lock(value->mutex);
            previous = std::exchange(value->source, self->b->retain(source));
            if (!value->watching) { start = true; value->watching = true; generation = value->generation; }
        }
        self->b->release(previous);
        if (start) {
            static_cast<void>(self->b->retain(object));   // released by the watcher when it ends
            const auto worker = self->b->call(libsystem, "_dispatch_get_global_queue", {0, 0});
            self->b->dispatch_async(worker, [self, object, value, generation] { self->watch(object, value, generation); });
        }
        s.x[0] = source;
    });
    // Boolean SCDynamicStoreSetDispatchQueue(SCDynamicStoreRef, dispatch_queue_t)
    install("_SCDynamicStoreSetDispatchQueue", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto value = self->state(object);
        const auto queue = s.x[1];
        if (queue) static_cast<void>(self->b->call(libsystem, "_dispatch_retain", {queue}));
        Address previous{};
        std::uint64_t generation{};
        bool start{};
        {
            std::lock_guard lock(value->mutex);
            previous = std::exchange(value->queue, queue);
            generation = ++value->generation;
            if (queue) { start = true; value->watching = true; }
        }
        if (previous) static_cast<void>(self->b->call(libsystem, "_dispatch_release", {previous}));
        if (start) {
            static_cast<void>(self->b->retain(object));   // released by the watcher when it ends
            const auto worker = self->b->call(libsystem, "_dispatch_get_global_queue", {0, 0});
            self->b->dispatch_async(worker, [self, object, value, generation] { self->watch(object, value, generation); });
        }
        s.x[0] = 1;
    });
    // CFStringRef SCDynamicStoreCopyComputerName(SCDynamicStoreRef, CFStringEncoding *nameEncoding)
    install("_SCDynamicStoreCopyComputerName", [self](CpuState& s, GuestMemory& m) {
        const auto name = computer_name();
        if (name.empty()) { s.x[0] = 0; return; }
        if (s.x[1]) m.write32(s.x[1], 0x08000100);   // kCFStringEncodingUTF8
        s.x[0] = self->b->string(name);
    });
    // CFStringRef SCDynamicStoreCopyLocalHostName(SCDynamicStoreRef)
    install("_SCDynamicStoreCopyLocalHostName", [self](CpuState& s, GuestMemory&) {
        const auto name = computer_name();
        s.x[0] = name.empty() ? 0 : self->b->string(name);
    });
    // CFStringRef SCDynamicStoreCopyConsoleUser(SCDynamicStoreRef, uid_t *uid, gid_t *gid)
    install("_SCDynamicStoreCopyConsoleUser", [self](CpuState& s, GuestMemory& m) {
        const auto name = user_name();
        if (name.empty()) { s.x[0] = 0; return; }
        if (s.x[1]) m.write32(s.x[1], guest_uid);
        if (s.x[2]) m.write32(s.x[2], guest_uid);
        s.x[0] = self->b->string(name);
    });
    // CFStringRef SCDynamicStoreKeyCreateNetworkGlobalEntity(allocator, domain, entity)
    install("_SCDynamicStoreKeyCreateNetworkGlobalEntity", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->string(self->b->text(s.x[1]) + "/Network/Global/" + self->b->text(s.x[2]));
    });
    // CFStringRef SCDynamicStoreKeyCreateNetworkInterfaceEntity(allocator, domain, ifname, entity)
    install("_SCDynamicStoreKeyCreateNetworkInterfaceEntity", [self](CpuState& s, GuestMemory&) {
        auto key = self->b->text(s.x[1]) + "/Network/Interface/" + self->b->text(s.x[2]);
        if (s.x[3]) key += "/" + self->b->text(s.x[3]);
        s.x[0] = self->b->string(key);
    });
    install("_SCDynamicStoreKeyCreateComputerName", [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->string("Setup:/System"); });
    install("_SCDynamicStoreKeyCreateConsoleUser", [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->string("State:/Users/ConsoleUser"); });
    install("_SCDynamicStoreKeyCreateHostNames", [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->string("Setup:/Network/HostNames"); });

    // ---- reachability -----------------------------------------------------------------------------
    // SCNetworkReachabilityRef SCNetworkReachabilityCreateWithName(CFAllocatorRef, const char *nodename)
    install("_SCNetworkReachabilityCreateWithName", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = 0; return; }
        auto value = std::make_shared<ReachabilityState>();
        for (Address at = s.x[1]; value->name.size() < 1025; ++at) {
            const auto c = m.read8(at);
            if (!c) break;
            value->name.push_back(static_cast<char>(c));
        }
        const auto object = self->b->objc.allocate(self->reachability_class);
        static_cast<void>(self->b->objc.host_state<ReachabilityState>(object, [&] { return value; }));
        s.x[0] = object;
    });
    // SCNetworkReachabilityRef SCNetworkReachabilityCreateWithAddress(CFAllocatorRef, const struct sockaddr *)
    install("_SCNetworkReachabilityCreateWithAddress", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = 0; return; }
        auto value = std::make_shared<ReachabilityState>();
        const auto length = m.read8(s.x[1]);
        if (length < 8) { s.x[0] = 0; return; }
        value->address.resize(length);
        m.read_bytes(s.x[1], value->address);
        const auto object = self->b->objc.allocate(self->reachability_class);
        static_cast<void>(self->b->objc.host_state<ReachabilityState>(object, [&] { return value; }));
        s.x[0] = object;
    });
    // Boolean SCNetworkReachabilityGetFlags(SCNetworkReachabilityRef, SCNetworkReachabilityFlags *)
    install("_SCNetworkReachabilityGetFlags", [self](CpuState& s, GuestMemory& m) {
        const auto value = s.x[0] ? self->b->objc.find_host_state<ReachabilityState>(s.x[0]) : nullptr;
        if (!value || !s.x[1]) { s.x[0] = 0; return; }
        m.write32(s.x[1], self->reachability(*value));
        s.x[0] = 1;
    });
    const auto reachability_of = [self](Address object) {
        auto value = object ? self->b->objc.find_host_state<ReachabilityState>(object) : nullptr;
        if (!value) security::fail("expected an SCNetworkReachabilityRef");
        return value;
    };
    b->method(self->reachability_class, "dealloc", [self](CpuState& s, GuestMemory&) {
        // The watcher holds a reference, so no delivery is active here.
        if (const auto value = self->b->objc.find_host_state<ReachabilityState>(s.x[0])) {
            Address info{}, release_info{}, source{};
            {
                std::lock_guard lock(value->mutex);
                ++value->generation;
                info = std::exchange(value->info, 0);
                release_info = value->release_info;
                source = std::exchange(value->source, 0);
            }
            if (info && release_info) static_cast<void>(self->b->call_address(release_info, std::array{info}));
            self->b->release(source);
        }
        static_cast<void>(self->b->objc.send_super(s.x[0], self->reachability_class, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    // Boolean SCNetworkReachabilitySetCallback(target, SCNetworkReachabilityCallBack, SCNetworkReachabilityContext *)
    install("_SCNetworkReachabilitySetCallback", [self, reachability_of](CpuState& s, GuestMemory& m) {
        const auto value = reachability_of(s.x[0]);
        Address info{}, retain_info{}, release_info{};
        if (const auto context = s.x[2]) {
            // {CFIndex version; void *info; retain; release; copyDescription}
            if (m.read64(context) != 0) { s.x[0] = 0; return; }   // unknown context version
            info = m.read64(context + 8);
            retain_info = m.read64(context + 16);
            release_info = m.read64(context + 24);
        }
        if (!s.x[1]) info = 0;
        if (info && retain_info) info = self->b->call_address(retain_info, std::array{info});
        Address old_info{}, old_release{};
        {
            std::lock_guard lock(value->mutex);
            old_info = std::exchange(value->info, info);
            old_release = std::exchange(value->release_info, info ? release_info : 0);
            value->callout = s.x[1];
        }
        if (old_info && old_release) static_cast<void>(self->b->call_address(old_release, std::array{old_info}));
        s.x[0] = 1;
    });
    // The run-loop source (version 0, info = the target) whose perform calls
    // the callout with the flags the watcher found.
    const auto reachability_perform = self->b->process.install_function("__alchemy_sc_reachability_perform", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto value = self->b->objc.find_host_state<ReachabilityState>(object);
        if (!value) return;
        Address callout{}, info{};
        std::optional<std::uint32_t> flags;
        {
            std::lock_guard lock(value->mutex);
            flags = std::exchange(value->pending, std::nullopt);
            callout = value->callout; info = value->info;
        }
        if (flags && callout) static_cast<void>(self->b->call_address(callout, std::array<Address, 3>{object, *flags, info}));
    }, "alchemy.internal");
    // Boolean SCNetworkReachabilityScheduleWithRunLoop(target, CFRunLoopRef, CFStringRef mode)
    install("_SCNetworkReachabilityScheduleWithRunLoop", [self, reachability_of, reachability_perform](CpuState& s, GuestMemory& m) {
        const auto object = s.x[0];
        const auto value = reachability_of(object);
        if (!s.x[1] || !s.x[2]) { s.x[0] = 0; return; }
        Address source{};
        {
            std::lock_guard lock(value->mutex);
            source = value->source;
        }
        if (!source) {
            const auto context = self->b->process.allocate_guest(80, 16);
            m.fill_bytes(context, 0, 80);
            m.write64(context + 8, object);
            m.write64(context + 72, reachability_perform);
            source = self->b->call(alchemy::core_foundation_library, "_CFRunLoopSourceCreate", {0, 0, context});
            self->b->process.release_guest(context);
            if (!source) { s.x[0] = 0; return; }
            std::lock_guard lock(value->mutex);
            if (value->source) { self->b->release(source); source = value->source; }
            else value->source = source;
        }
        {
            std::lock_guard lock(value->mutex);
            value->scheduled.emplace_back(s.x[1], self->b->retain(s.x[2]));
        }
        static_cast<void>(self->b->call(alchemy::core_foundation_library, "_CFRunLoopAddSource", {s.x[1], source, s.x[2]}));
        self->update_watcher(object, value);
        s.x[0] = 1;
    });
    // Boolean SCNetworkReachabilityUnscheduleFromRunLoop(target, CFRunLoopRef, CFStringRef mode)
    install("_SCNetworkReachabilityUnscheduleFromRunLoop", [self, reachability_of](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto value = reachability_of(object);
        Address source{}, mode{};
        {
            std::lock_guard lock(value->mutex);
            source = value->source;
            const auto mode_text = self->b->maybe_text(s.x[2]);
            for (auto it = value->scheduled.begin(); it != value->scheduled.end(); ++it)
                if (it->first == s.x[1] && self->b->maybe_text(it->second) == mode_text) {
                    mode = it->second;
                    value->scheduled.erase(it);
                    break;
                }
        }
        if (!mode) { s.x[0] = 0; return; }   // not scheduled there
        static_cast<void>(self->b->call(alchemy::core_foundation_library, "_CFRunLoopRemoveSource", {s.x[1], source, mode}));
        self->b->release(mode);
        self->update_watcher(object, value);
        s.x[0] = 1;
    });
    // Boolean SCNetworkReachabilitySetDispatchQueue(target, dispatch_queue_t or NULL)
    install("_SCNetworkReachabilitySetDispatchQueue", [self, reachability_of](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto value = reachability_of(object);
        if (s.x[1]) static_cast<void>(self->b->call(libsystem, "_dispatch_retain", {s.x[1]}));
        Address previous{};
        {
            std::lock_guard lock(value->mutex);
            previous = std::exchange(value->queue, s.x[1]);
        }
        if (previous) static_cast<void>(self->b->call(libsystem, "_dispatch_release", {previous}));
        self->update_watcher(object, value);
        s.x[0] = 1;
    });
    // Test hook: deliver `flags` to a target as if the watcher saw them change.
    static_cast<void>(self->b->process.install_function("__alchemy_sc_reachability_inject", [self, reachability_of](CpuState& s, GuestMemory&) {
        self->deliver(s.x[0], reachability_of(s.x[0]), static_cast<std::uint32_t>(s.x[1]));
    }, "alchemy.internal"));
}

} // namespace alchemy::gui::network
