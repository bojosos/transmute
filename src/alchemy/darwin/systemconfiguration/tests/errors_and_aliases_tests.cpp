// SystemConfiguration pieces moved from darwin/identities/system_gaps.cpp
// (errors_and_aliases.cpp): SCErrorString, the proxies key, and IOKit's
// SCDynamicStore aliases.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view sc = "/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration", iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";

const Case systemconfiguration_errors_and_aliases_case("systemconfiguration_errors_and_aliases", [](Fixture& f) {
    require(f.read_text(f.call(sc, "_SCErrorString", {1001})) == "Failed!", "SCErrorString");
    require(f.cf.utf8(f.call(sc, "_SCDynamicStoreKeyCreateProxies", {0})) == "State:/Network/Global/Proxies", "proxies key");
    require(f.symbol(iokit, "_SCDynamicStoreCreate") == f.symbol(sc, "_SCDynamicStoreCreate"), "IOKit SCDynamicStore alias");
});

} // namespace
