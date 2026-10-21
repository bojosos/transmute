#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// ColorSync: no display device is registered; the device-info keys are
// real.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view color_sync = darwin_framework_libraries[8];

class ColorSync final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // ColorSync: no display device is registered.
        function(color_sync, "_CGDisplayCreateUUIDFromDisplayID", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        function(color_sync, "_ColorSyncDeviceCopyDeviceInfo", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        constexpr std::pair<std::string_view, std::string_view> sync[] = {
            {"_kColorSyncDisplayDeviceClass", "mntr"}, {"_kColorSyncDeviceDefaultProfileID", "DeviceDefaultProfileID"},
            {"_kColorSyncCustomProfiles", "CustomProfiles"}, {"_kColorSyncFactoryProfiles", "FactoryProfiles"},
            {"_kColorSyncDeviceProfileURL", "DeviceProfileURL"}, {"_kColorSyncDeviceClass", "DeviceClass"},
            {"_kColorSyncDeviceID", "DeviceID"}, {"_kColorSyncDeviceDescription", "DeviceDescription"}};
        for (const auto& [symbol, value] : sync) string_symbol(color_sync, std::string(symbol), value);
    }
};

} // namespace

void install_colorsync(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<ColorSync>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
