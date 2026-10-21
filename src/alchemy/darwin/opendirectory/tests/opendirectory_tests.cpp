// OpenDirectory (src/alchemy/darwin/opendirectory/): the constants are real; no
// directory service runs.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view od = "/System/Library/Frameworks/OpenDirectory.framework/Versions/A/OpenDirectory";

const Case opendirectory_case("opendirectory", [](Fixture& f) {
    require(f.send(f.cls("ODSession"), "defaultSession") == 0, "no directory service");
    const auto error = f.block(8);
    require(f.send(f.cls("ODSession"), "sessionWithOptions:error:", {0, error}) == 0 && f.engine.read64(error), "sessions fail with an error");
    require(f.string_constant(od, "_kODRecordTypeUsers") == "dsRecTypeStandard:Users", "kODRecordTypeUsers");
    require(f.string_constant(od, "_kODRecordTypeGroups") == "dsRecTypeStandard:Groups", "kODRecordTypeGroups");
});

} // namespace
