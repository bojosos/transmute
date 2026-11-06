// CoreFoundation pieces moved from darwin/identities/framework_gaps.cpp
// (time_zones_and_uuids.cpp): CFTimeZone, CFUUID and CFString prefixes.
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/path.hpp"

#include <bit>
#include <filesystem>
#include <string>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case corefoundation_time_zones_and_uuids_case("corefoundation_time_zones_and_uuids", [](Fixture& f) {
    const auto zone = f.call(cf, "_CFTimeZoneCopyDefault");
    require(!f.cf.utf8(f.call(cf, "_CFTimeZoneGetName", {zone})).empty(), "CFTimeZoneGetName");
    const auto uuid = f.call(cf, "_CFUUIDCreate", {0});
    const auto text = f.cf.utf8(f.call(cf, "_CFUUIDCreateString", {0, uuid}));
    require(text.size() == 36 && text[14] == '4', "a version 4 CFUUID");
    require(f.call(cf, "_CFStringHasPrefix", {f.cf.string("Alchemy"), f.cf.string("Alc")}) == 1 &&
            f.call(cf, "_CFStringHasSuffix", {f.cf.string("Alchemy"), f.cf.string("Alc")}) == 0, "CFStringHasPrefix/HasSuffix");
    require(f.engine.read64(f.symbol(cf, "_kCFAllocatorNull")) != 0, "kCFAllocatorNull");
    // Godot's imports: CF's version number (open-source CF's newest documented value) and two URL keys,
    // answered for a real directory: nothing is system-immutable, no host volume is case-sensitive.
    require(std::bit_cast<double>(f.engine.read64(f.symbol(cf, "_kCFCoreFoundationVersionNumber"))) == 1299.0, "kCFCoreFoundationVersionNumber");
    const auto folder = f.cf.file_url(alchemy::guest_path_string(std::filesystem::temp_directory_path()), true);
    const auto value = f.block(8);
    for (const auto* key : {"_NSURLIsSystemImmutableKey", "_NSURLVolumeSupportsCaseSensitiveNamesKey"}) {
        const auto name = f.engine.read64(f.symbol(cf, key));
        require(f.call(cf, "_CFURLCopyResourcePropertyForKey", {folder, name, value, 0}) == 1 &&
                f.engine.read64(value) == f.engine.read64(f.symbol(cf, "_kCFBooleanFalse")), std::string(key) + " is false");
        const auto foundation_name = f.engine.read64(f.symbol("/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation", key));
        require(f.call(cf, "_CFEqual", {foundation_name, name}) == 1, std::string(key) + " under Foundation");
    }
});

} // namespace
