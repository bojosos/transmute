// CoreServices pieces moved from darwin/identities/: LaunchServices, Time Machine, the
// Component Manager and Spotlight (launch_services.cpp), and CarbonCore's
// FSGetVolumeInfo over the host's fixed volumes (carbon_core.cpp).
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view cs = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }

const Case coreservices_launch_services_case("coreservices_launch_services", [](Fixture& f) {
    const auto url = f.cf.file_url("/Applications/Example.app", true);
    const auto name = f.block(8);
    require(f.status(cs, "_LSCopyDisplayNameForURL", {url, name}) == 0 && f.cf.utf8(f.engine.read64(name)) == "Example", "LSCopyDisplayNameForURL");
    require(f.status(cs, "_LSSetDefaultHandlerForURLScheme", {string(f, "http"), string(f, "com.example")}) == -10822, "LaunchServices unavailable");
    require(f.call(cs, "_OpenDefaultComponent", {0x6f736120, 0x73637074}) == 0, "no OSA component");
    const auto excluded = f.block(1, 1);
    require(f.status(cs, "_CSBackupIsItemExcluded", {url, excluded}) == 0 && f.memory().read8(excluded) == 0, "nothing excluded");
    require(f.call(cs, "_MDItemCreate", {0, string(f, "/tmp")}) == 0, "no Spotlight");
});

const Case coreservices_carbon_core_case("coreservices_carbon_core", [](Fixture& f) {
    const auto actual = f.block(8), volume_info = f.block(128);
    require(f.call(cs, "_FSGetVolumeInfo", {0, 1, actual, 0, volume_info, 0, 0}) == 0, "FSGetVolumeInfo");
    require(f.engine.read64(volume_info + 40) > 0, "volume size");
    require(f.call(cs, "_FSGetVolumeInfo", {0, 9999, actual, 0, volume_info, 0, 0}) == static_cast<Address>(-35), "nsvErr past the last volume");
});

} // namespace
