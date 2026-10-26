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
    // The C API: no session or node, each with a +1 CFError; kODSessionDefault is the NULL default session.
    const auto c_error = f.block(8);
    require(f.call(od, "_ODSessionCreate", {0, 0, c_error}) == 0 && f.engine.read64(c_error) != 0, "ODSessionCreate fails with an error");
    require(f.engine.read64(f.symbol(od, "_kODSessionDefault")) == 0, "kODSessionDefault");
    f.engine.write64(c_error, 0);
    require(f.call(od, "_ODNodeCreateWithNodeType", {0, 0, 0x2201, c_error}) == 0 && f.engine.read64(c_error) != 0, "no node");
    require(f.call(od, "_ODRecordGetRecordName", {0}) == 0, "NULL record");
    bool refused = false;
    try { static_cast<void>(f.call(od, "_ODRecordCopyValues", {0x1234, 0, 0})); } catch (const std::exception&) { refused = true; }
    require(refused, "records cannot exist");
});

} // namespace
