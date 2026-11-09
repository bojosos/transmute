// NSMetadataQuery with no Spotlight index, NSHost (metadata_and_hosts.cpp), NSProtocolFromString
// (foundation_bridge.cpp) and kMDItemTextContent (coreservices/launch_services.cpp).
#include "alchemy/gui/tests/framework_case.hpp"

#include <string>

namespace {
using namespace fill_test;

constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::string_view core_services = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
constexpr std::string_view libobjc = "/usr/lib/libobjc.A.dylib";

const Case foundation_metadata_and_hosts_case("foundation_metadata_and_hosts", [](Fixture& f) {
    require(f.string_constant(core_services, "_kMDItemTextContent") == "kMDItemTextContent", "kMDItemTextContent");
    require(f.string_constant(foundation, "_NSMetadataQueryLocalComputerScope") == "NSMetadataQueryLocalComputerScope",
            "NSMetadataQueryLocalComputerScope");

    // A query starts with a predicate, once, and finds nothing.
    const auto query = f.send(f.send(f.objc.find_class("NSMetadataQuery"), "alloc"), "init");
    require((f.send(query, "startQuery") & 0xff) == 0, "no query without a predicate");
    f.send(query, "setPredicate:", {f.cf.string("kMDItemFSName == '*.dt'")});
    f.send(query, "setSearchScopes:", {f.engine.read64(f.symbol(foundation, "_NSMetadataQueryLocalComputerScope"))});
    require((f.send(query, "startQuery") & 0xff) == 1 && (f.send(query, "isStarted") & 0xff) == 1, "-startQuery");
    require((f.send(query, "startQuery") & 0xff) == 0, "a started query does not start again");
    require(f.send(query, "resultCount") == 0 && f.cf.length(f.send(query, "results")) == 0, "no results");
    f.send(query, "stopQuery");
    require((f.send(query, "isStopped") & 0xff) == 1, "-stopQuery");

    // NSHost.
    const auto host_class = f.objc.find_class("NSHost");
    const auto current = f.send(host_class, "currentHost");
    require(current && f.send(host_class, "currentHost") == current && !f.cf.utf8(f.send(current, "name")).empty(), "+currentHost");
    const auto loopback = f.send(host_class, "hostWithAddress:", {f.cf.string("127.0.0.1")});
    require(loopback && f.cf.utf8(f.send(loopback, "address")) == "127.0.0.1" && (f.send(loopback, "isEqualToHost:", {loopback}) & 0xff) == 1,
            "+hostWithAddress:");
    require(f.send(host_class, "hostWithAddress:", {f.cf.string("not an address")}) == 0, "+hostWithAddress: of a name");

    // NSProtocolFromString / NSStringFromProtocol, for a protocol registered at run time.
    const auto made = f.call(libobjc, "_objc_allocateProtocol", {f.text("AlchemyTestProtocol")});
    f.call(libobjc, "_objc_registerProtocol", {made});
    require(f.call(foundation, "_NSProtocolFromString", {f.cf.string("AlchemyTestProtocol")}) == made, "NSProtocolFromString");
    require(f.cf.utf8(f.call(foundation, "_NSStringFromProtocol", {made})) == "AlchemyTestProtocol", "NSStringFromProtocol");
    require(f.call(foundation, "_NSProtocolFromString", {f.cf.string("AlchemyNoSuchProtocol")}) == 0, "an unknown protocol");
});

} // namespace
