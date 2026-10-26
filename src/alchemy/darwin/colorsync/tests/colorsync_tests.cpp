// ColorSync (src/alchemy/darwin/colorsync/): no display device is registered.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view color_sync = "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync";

const Case colorsync_case("colorsync", [](Fixture& f) {
    require(f.string_constant(color_sync, "_kColorSyncDeviceClass") == "DeviceClass", "kColorSyncDeviceClass");
    require(f.string_constant(color_sync, "_kColorSyncDisplayDeviceClass") == "mntr", "kColorSyncDisplayDeviceClass");
    require(f.call(color_sync, "_CGDisplayCreateUUIDFromDisplayID", {1}) == 0, "no display device");

    // Profiles (alchemy-fx, profiles.cpp): a named profile, its header, tags and description.
    constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    const auto profile = f.call(color_sync, "_ColorSyncProfileCreateWithName", {f.cf.string("kCGColorSpaceSRGB")});
    require(profile != 0, "the sRGB profile by name");
    require(f.call(cf, "_CFDataGetLength", {f.call(color_sync, "_ColorSyncProfileCopyHeader", {profile})}) == 128, "the 128-byte header");
    require((f.call(color_sync, "_ColorSyncProfileContainsTag", {profile, f.cf.string("desc")}) & 0xff) == 1 &&
                (f.call(color_sync, "_ColorSyncProfileContainsTag", {profile, f.cf.string("zzzz")}) & 0xff) == 0,
            "tags by signature");
    const auto description = f.call(color_sync, "_ColorSyncProfileCopyDescriptionString", {profile});
    require(description && !f.cf.utf8(description).empty(), "the profile description");
    require((f.call(color_sync, "_ColorSyncProfileVerify", {profile, 0, 0}) & 0xff) == 1, "the generated profile verifies");
    // The same bytes make an equal profile (the same MD5).
    const auto data = f.call(color_sync, "_ColorSyncProfileCopyData", {profile, 0});
    const auto again = f.call(color_sync, "_ColorSyncProfileCreate", {data, 0});
    alchemy::CallRegs md5{};
    md5.x[0] = profile;
    const auto first = f.regs(color_sync, "_ColorSyncProfileGetMD5", md5);
    md5.x[0] = again;
    const auto second = f.regs(color_sync, "_ColorSyncProfileGetMD5", md5);
    require(again && (first.x0 | first.x1) != 0 && first.x0 == second.x0 && first.x1 == second.x1, "the profile MD5");
    require(f.call(color_sync, "_ColorSyncProfileGetTypeID") == f.call(cf, "_CFGetTypeID", {profile}), "the profile type ID");
    // Not a profile: NULL and an error.
    const auto junk = f.block(16);
    const auto junk_data = f.call(cf, "_CFDataCreate", {0, junk, 16});
    const auto error = f.block(8);
    require(f.call(color_sync, "_ColorSyncProfileCreate", {junk_data, error}) == 0 && f.engine.read64(error) != 0, "junk data is no profile");
    bool stopped{};
    try { static_cast<void>(f.call(color_sync, "_ColorSyncTransformCreate", {0, 0})); } catch (const std::exception&) { stopped = true; }
    require(stopped, "transforms stop explicitly");
});

} // namespace
