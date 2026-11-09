#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <bit>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// NSMetadataQuery and NSHost (Foundation).
//
// NSMetadataQuery searches the Spotlight index, and there is none under
// Alchemy: a query keeps its predicate, scopes and other settings, starts, and
// then gathers nothing. -startQuery answers YES (NO without a predicate or once
// started) and posts NSMetadataQueryDidStartGatheringNotification and
// NSMetadataQueryDidFinishGatheringNotification on the main queue, with no
// results: -resultCount is 0, -results empty, and an index into the results is
// out of range (NSRangeException). The scope and notification names are the
// constants' own names: their values are not documented, and callers only hand
// them back to the query or observe the notifications the query posts.
//
// NSHost answers through libSystem's resolver (so the process's network policy
// applies): +currentHost is the host name (gethostname) with the addresses of
// the network interfaces (getifaddrs); +hostWithName: resolves a name
// (getaddrinfo; nil when it does not resolve); +hostWithAddress: a numeric
// address, with the name getnameinfo finds for it. Two hosts are equal when
// they share an address. There is no host cache to flush or enable.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// ---- NSMetadataQuery ----

struct Query final : ObjectPayload {
    std::mutex mutex;
    Address predicate{}, scopes{}, sort_descriptors{}, value_list_attributes{}, grouping_attributes{}, search_items{}, operation_queue{};
    Address delegate{};   // not retained
    double batching_interval{1.0};
    enum class Phase { idle, gathering, finished, stopped } phase{Phase::idle};
};

struct Queries {
    explicit Queries(Support& owner) : support(owner) {}
    Support& support;
    Address cls{};
    std::shared_ptr<Query> state(Address object) { return support.objc.host_state<Query>(object); }
};

void install_metadata_query(Support& support) {
    auto queries = std::make_shared<Queries>(support);
    const auto cls = queries->cls = support.declare_class("NSMetadataQuery", "NSObject", foundation_library);
    for (const auto* name : {"NSMetadataQueryLocalComputerScope", "NSMetadataQueryUserHomeScope", "NSMetadataQueryNetworkScope",
                             "NSMetadataQueryIndexedLocalComputerScope", "NSMetadataQueryIndexedNetworkScope",
                             "NSMetadataQueryDidStartGatheringNotification", "NSMetadataQueryGatheringProgressNotification",
                             "NSMetadataQueryDidFinishGatheringNotification", "NSMetadataQueryDidUpdateNotification"})
        support.string_constant(foundation_library, std::string("_") + name, name);

    support.method(cls, "init", [queries](CpuState& s, GuestMemory&) { static_cast<void>(queries->state(s.x[0])); }, "@@:");
    support.method(cls, "dealloc", [queries, cls](CpuState& s, GuestMemory&) {
        if (const auto query = queries->support.objc.find_host_state<Query>(s.x[0])) {
            for (const auto value : {query->predicate, query->scopes, query->sort_descriptors, query->value_list_attributes,
                                     query->grouping_attributes, query->search_items, query->operation_queue})
                if (value) objc_release(queries->support, value);
        }
        static_cast<void>(queries->support.objc.send_super(s.x[0], cls, queries->support.objc.selector("dealloc")));
    }, "v@:");
    // Object properties (copied or retained, as their setters document).
    const auto property = [&](const char* getter, const char* setter, Address Query::*field, bool copy) {
        support.method(cls, getter, [queries, field](CpuState& s, GuestMemory&) {
            const auto query = queries->state(s.x[0]);
            std::lock_guard lock(query->mutex);
            s.x[0] = (*query).*field;
        }, "@@:");
        support.method(cls, setter, [queries, field, copy](CpuState& s, GuestMemory&) {
            const auto value = s.x[2] ? (copy ? queries->support.send_id(s.x[2], "copy") : objc_retain(queries->support, s.x[2])) : 0;
            const auto query = queries->state(s.x[0]);
            Address old{};
            { std::lock_guard lock(query->mutex); old = std::exchange((*query).*field, value); }
            if (old) objc_release(queries->support, old);
        }, "v@:@");
    };
    property("predicate", "setPredicate:", &Query::predicate, true);
    property("searchScopes", "setSearchScopes:", &Query::scopes, true);
    property("sortDescriptors", "setSortDescriptors:", &Query::sort_descriptors, true);
    property("valueListAttributes", "setValueListAttributes:", &Query::value_list_attributes, true);
    property("groupingAttributes", "setGroupingAttributes:", &Query::grouping_attributes, true);
    property("searchItems", "setSearchItems:", &Query::search_items, true);
    property("operationQueue", "setOperationQueue:", &Query::operation_queue, false);
    support.method(cls, "delegate", [queries](CpuState& s, GuestMemory&) { s.x[0] = queries->state(s.x[0])->delegate; }, "@@:");
    support.method(cls, "setDelegate:", [queries](CpuState& s, GuestMemory&) { queries->state(s.x[0])->delegate = s.x[2]; }, "v@:@");
    support.method(cls, "notificationBatchingInterval", [queries](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint64_t>(queries->state(s.x[0])->batching_interval), 0};
    }, "d@:");
    support.method(cls, "setNotificationBatchingInterval:", [queries](CpuState& s, GuestMemory&) {
        queries->state(s.x[0])->batching_interval = std::bit_cast<double>(s.v[0][0]);
    }, "v@:d");

    // -startQuery: gathering starts and finishes at once, with nothing found, the two notifications posted on the
    // main queue as the query posts them there.
    support.method(cls, "startQuery", [queries](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto query = queries->state(object);
        {
            std::lock_guard lock(query->mutex);
            if (!query->predicate || query->phase != Query::Phase::idle) { s.x[0] = 0; return; }
            query->phase = Query::Phase::gathering;
        }
        objc_retain(queries->support, object);
        const auto bridge = security::bridge();
        bridge->dispatch_async(bridge->main_queue(), [queries, query, object] {
            post_notification(queries->support, "NSMetadataQueryDidStartGatheringNotification", object);
            bool finished = false;
            {
                std::lock_guard lock(query->mutex);
                if (query->phase == Query::Phase::gathering) { query->phase = Query::Phase::finished; finished = true; }
            }
            if (finished) post_notification(queries->support, "NSMetadataQueryDidFinishGatheringNotification", object);
            objc_release(queries->support, object);
        });
        s.x[0] = 1;
    }, "B@:");
    support.method(cls, "stopQuery", [queries](CpuState& s, GuestMemory&) {
        const auto query = queries->state(s.x[0]);
        std::lock_guard lock(query->mutex);
        if (query->phase != Query::Phase::idle) query->phase = Query::Phase::stopped;
    }, "v@:");
    const auto phase_is = [&](const char* selector, std::initializer_list<Query::Phase> phases) {
        const std::vector<Query::Phase> wanted(phases);
        support.method(cls, selector, [queries, wanted](CpuState& s, GuestMemory&) {
            const auto query = queries->state(s.x[0]);
            std::lock_guard lock(query->mutex);
            s.x[0] = std::find(wanted.begin(), wanted.end(), query->phase) != wanted.end() ? 1 : 0;
        }, "B@:");
    };
    phase_is("isStarted", {Query::Phase::gathering, Query::Phase::finished, Query::Phase::stopped});
    phase_is("isGathering", {Query::Phase::gathering});
    phase_is("isStopped", {Query::Phase::stopped});
    for (const auto* name : {"disableUpdates", "enableUpdates"}) support.method(cls, name, [](CpuState&, GuestMemory&) {}, "v@:");
    // The results: none.
    support.method(cls, "resultCount", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");
    for (const auto* name : {"results", "groupedResults"})
        support.method(cls, name, [queries](CpuState& s, GuestMemory&) { s.x[0] = array(queries->support, {}); }, "@@:");
    support.method(cls, "valueLists", [queries](CpuState& s, GuestMemory&) { s.x[0] = dictionary(queries->support, {}); }, "@@:");
    support.method(cls, "indexOfResult:", [](CpuState& s, GuestMemory&) { s.x[0] = 0x7fffffffffffffffULL; }, "Q@:@");   // NSNotFound
    support.method(cls, "resultAtIndex:", [](CpuState& s, GuestMemory&) {
        fail("NSRangeException: -[NSMetadataQuery resultAtIndex:]: index " + std::to_string(s.x[2]) + " beyond the results (none)");
    }, "@@:Q");
    support.method(cls, "valueOfAttribute:forResultAtIndex:", [](CpuState& s, GuestMemory&) {
        fail("NSRangeException: -[NSMetadataQuery valueOfAttribute:forResultAtIndex:]: index " + std::to_string(s.x[3]) + " beyond the results (none)");
    }, "@@:@Q");
}

// ---- NSHost ----

struct Host final : ObjectPayload {
    std::vector<std::string> names, addresses;
};

struct Hosts {
    explicit Hosts(Support& owner) : support(owner) {}
    Support& support;
    Address cls{};
    std::mutex mutex;
    Address current{};

    Address call(std::string_view name, std::initializer_list<Address> arguments) { return support.call(libsystem, name, arguments); }
    // inet_ntop of a sockaddr (Darwin layout: sin_len, sin_family, then the address).
    std::string numeric(Address address) {
        auto& m = support.memory();
        const auto family = m.read8(address + 1);
        if (family != 2 && family != 30) return {};   // AF_INET, AF_INET6
        GuestBlock text(support, 64);
        const auto bytes = address + (family == 2 ? 4 : 8);
        if (!call("_inet_ntop", {family, bytes, text, 64})) return {};
        return c_string(m, text, 64);
    }
    void add(std::vector<std::string>& list, std::string value) {
        if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end()) list.push_back(std::move(value));
    }
    // getaddrinfo(name): the addresses it resolves to, and its canonical name; false when it does not resolve.
    bool resolve(const std::string& name, bool numeric_only, Host& host) {
        GuestBlock text(support, name);
        GuestBlock hints(support, 48);
        support.memory().write32(hints, 2 | (numeric_only ? 4 : 0));   // AI_CANONNAME | AI_NUMERICHOST
        support.memory().write32(hints + 8, 1);                          // SOCK_STREAM: one entry per address
        GuestBlock result(support, 8);
        if (call("_getaddrinfo", {text, 0, hints, result}) != 0) return false;
        const auto list = support.memory().read64(result);
        for (auto info = list; info; info = support.memory().read64(info + 40)) {
            if (const auto canonical = support.memory().read64(info + 24)) add(host.names, c_string(support.memory(), canonical, 1025));
            if (const auto address = support.memory().read64(info + 32)) add(host.addresses, numeric(address));
        }
        static_cast<void>(call("_freeaddrinfo", {list}));
        return true;
    }
    Address make(std::shared_ptr<Host> host) { return make_object(support, cls, std::move(host)); }
    std::shared_ptr<Host> state(Address object) {
        const auto host = support.objc.find_host_state<Host>(object);
        if (!host) fail("NSHost: not an NSHost made by +currentHost, +hostWithName: or +hostWithAddress:");
        return host;
    }
};

void install_hosts(Support& support) {
    auto hosts = std::make_shared<Hosts>(support);
    const auto cls = hosts->cls = support.declare_class("NSHost", "NSObject", foundation_library);

    // +currentHost: the host name, and the addresses of the network interfaces (getifaddrs).
    support.method(cls, "currentHost", [hosts](CpuState& s, GuestMemory&) {
        std::lock_guard lock(hosts->mutex);
        if (!hosts->current) {
            auto host = std::make_shared<Host>();
            GuestBlock name(hosts->support, 256);
            if (hosts->call("_gethostname", {name, 256}) == 0) hosts->add(host->names, c_string(hosts->support.memory(), name, 256));
            GuestBlock list(hosts->support, 8);
            if (hosts->call("_getifaddrs", {list}) == 0) {
                const auto first = hosts->support.memory().read64(list);
                // struct ifaddrs { next; name; flags; addr; ... }
                for (auto entry = first; entry; entry = hosts->support.memory().read64(entry))
                    if (const auto address = hosts->support.memory().read64(entry + 24)) hosts->add(host->addresses, hosts->numeric(address));
                static_cast<void>(hosts->call("_freeifaddrs", {first}));
            }
            hosts->current = hosts->make(host);
            if (const auto record = hosts->support.process.objects().find(hosts->current)) record->immortal = true;
        }
        s.x[0] = hosts->current;
    }, "@@:", true);
    support.method(cls, "hostWithName:", [hosts](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        auto host = std::make_shared<Host>();
        const auto name = text(hosts->support, s.x[2]);
        hosts->add(host->names, name);
        s.x[0] = hosts->resolve(name, false, *host) ? autoreleased(hosts->support, hosts->make(host)) : 0;
    }, "@@:@", true);
    support.method(cls, "hostWithAddress:", [hosts](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        auto host = std::make_shared<Host>();
        const auto address = text(hosts->support, s.x[2]);
        if (!hosts->resolve(address, true, *host)) { s.x[0] = 0; return; }   // not a numeric address
        // The name for the address (getnameinfo with NI_NAMEREQD), when the resolver knows one.
        GuestBlock numeric(hosts->support, address);
        GuestBlock hints(hosts->support, 48);
        hosts->support.memory().write32(hints, 4);    // AI_NUMERICHOST
        GuestBlock result(hosts->support, 8);
        if (hosts->call("_getaddrinfo", {numeric, 0, hints, result}) == 0) {
            const auto list = hosts->support.memory().read64(result);
            if (list) {
                GuestBlock name(hosts->support, 1025);
                if (hosts->call("_getnameinfo", {hosts->support.memory().read64(list + 32), hosts->support.memory().read32(list + 16), name, 1025, 0, 0, 4}) == 0)
                    hosts->add(host->names, c_string(hosts->support.memory(), name, 1025));
            }
            static_cast<void>(hosts->call("_freeaddrinfo", {list}));
        }
        s.x[0] = autoreleased(hosts->support, hosts->make(host));
    }, "@@:@", true);
    const auto strings = [hosts](const std::vector<std::string>& values) {
        std::vector<Address> objects;
        for (const auto& value : values) objects.push_back(string(hosts->support, value));
        return array(hosts->support, objects);
    };
    support.method(cls, "name", [hosts](CpuState& s, GuestMemory&) {
        const auto host = hosts->state(s.x[0]);
        s.x[0] = host->names.empty() ? 0 : string(hosts->support, host->names.front());
    }, "@@:");
    support.method(cls, "names", [hosts, strings](CpuState& s, GuestMemory&) { s.x[0] = strings(hosts->state(s.x[0])->names); }, "@@:");
    support.method(cls, "address", [hosts](CpuState& s, GuestMemory&) {
        const auto host = hosts->state(s.x[0]);
        s.x[0] = host->addresses.empty() ? 0 : string(hosts->support, host->addresses.front());
    }, "@@:");
    support.method(cls, "addresses", [hosts, strings](CpuState& s, GuestMemory&) { s.x[0] = strings(hosts->state(s.x[0])->addresses); }, "@@:");
    // The current host's localized name is its host name; other hosts have none.
    support.method(cls, "localizedName", [hosts](CpuState& s, GuestMemory&) {
        const auto host = hosts->state(s.x[0]);
        s.x[0] = s.x[0] == hosts->current && !host->names.empty() ? string(hosts->support, host->names.front()) : 0;
    }, "@@:");
    support.method(cls, "isEqualToHost:", [hosts](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto a = hosts->state(s.x[0]), b = hosts->state(s.x[2]);
        s.x[0] = std::any_of(a->addresses.begin(), a->addresses.end(),
                             [&](const std::string& value) { return std::find(b->addresses.begin(), b->addresses.end(), value) != b->addresses.end(); });
    }, "B@:@");
    support.method(cls, "flushHostCache", [](CpuState&, GuestMemory&) {}, "v@:", true);
    support.method(cls, "isHostCacheEnabled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:", true);
    support.method(cls, "setHostCacheEnabled:", [](CpuState&, GuestMemory&) {}, "v@:B", true);
}

} // namespace

void install_metadata_queries_and_hosts(Support& support) {
    install_metadata_query(support);
    install_hosts(support);
}

} // namespace alchemy::gui::completion_kit
