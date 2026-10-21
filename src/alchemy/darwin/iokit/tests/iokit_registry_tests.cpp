// IOKit's registry as a renderer plug-in's images query it (darwin/iokit/iokit_registry.cpp; the
// case moved from darwin/identities/tests/framework_gaps_tests.cpp).
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";

const Case iokit_registry_case("iokit_registry", [](Fixture& f) {
    // The platform UUID through the registry.
    const auto matching = f.call(iokit, "_IOServiceMatching", {f.text("IOPlatformExpertDevice")});
    const auto service = f.call(iokit, "_IOServiceGetMatchingService", {0, matching});
    require(service != 0, "IOPlatformExpertDevice not found");
    const auto uuid = f.call(iokit, "_IORegistryEntryCreateCFProperty", {service, f.cf.string("IOPlatformUUID"), 0, 0});
    require(uuid && f.cf.utf8(uuid).size() == 36, "IOPlatformUUID");
    require(f.call(iokit, "_IOObjectRelease", {service}) == 0, "IOObjectRelease");
    // Ethernet controllers expose IOMACAddress to a parent search from the interface.
    const auto iterator = f.block(4);
    require(f.call(iokit, "_IOServiceGetMatchingServices", {0, f.call(iokit, "_IOServiceMatching", {f.text("IOEthernetInterface")}), iterator}) == 0,
            "IOServiceGetMatchingServices");
    for (Address entry; (entry = f.call(iokit, "_IOIteratorNext", {f.engine.read32(iterator)})) != 0;) {
        const auto mac = f.call(iokit, "_IORegistryEntrySearchCFProperty", {entry, f.text("IOService"), f.cf.string("IOMACAddress"), 0, 3});
        require(mac != 0, "IOMACAddress not found through the parents");
        static_cast<void>(f.call(iokit, "_IOObjectRelease", {entry}));
    }
});

} // namespace
