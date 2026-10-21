// DiskArbitration (src/alchemy/darwin/diskarbitration/): the identity, sessions over the
// guest's volumes, and claims that cannot be made.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view da = "/System/Library/Frameworks/DiskArbitration.framework/Versions/A/DiskArbitration";

const Case diskarbitration_case("diskarbitration", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(da), "DiskArbitration identity");
    require(f.call(da, "_DASessionCreate", {0}) != 0, "a session");
    require(f.process.resolve(da, "_DADiskClaim").has_value() && f.process.resolve(da, "_DADiskUnclaim").has_value(), "claims bind");
});

} // namespace
