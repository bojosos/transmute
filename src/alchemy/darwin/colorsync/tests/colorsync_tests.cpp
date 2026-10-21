// ColorSync (src/alchemy/darwin/colorsync/): no display device is registered.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view color_sync = "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync";

const Case colorsync_case("colorsync", [](Fixture& f) {
    require(f.string_constant(color_sync, "_kColorSyncDeviceClass") == "DeviceClass", "kColorSyncDeviceClass");
    require(f.string_constant(color_sync, "_kColorSyncDisplayDeviceClass") == "mntr", "kColorSyncDisplayDeviceClass");
    require(f.call(color_sync, "_CGDisplayCreateUUIDFromDisplayID", {1}) == 0, "no display device");
});

} // namespace
