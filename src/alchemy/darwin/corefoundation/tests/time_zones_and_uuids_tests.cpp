// CoreFoundation pieces moved from darwin/identities/framework_gaps.cpp
// (time_zones_and_uuids.cpp): CFTimeZone, CFUUID and CFString prefixes.
#include "alchemy/gui/tests/framework_case.hpp"

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
});

} // namespace
