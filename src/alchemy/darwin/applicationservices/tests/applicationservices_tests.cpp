// ApplicationServices (src/alchemy/darwin/applicationservices/): the umbrella identity
// re-exports its subframeworks; the Process Manager answers; icon plotting
// fails explicitly.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view as = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices", cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";

const Case applicationservices_case("applicationservices", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(as), "ApplicationServices identity");
    const auto through_umbrella = f.process.resolve(as, "_CGColorSpaceCreateDeviceRGB");
    require(through_umbrella && through_umbrella->address == f.symbol(cg, "_CGColorSpaceCreateDeviceRGB"),
            "a CoreGraphics symbol bound to the umbrella resolves to CoreGraphics' export");
    const auto psn = f.block(8);
    require(f.call(as, "_GetCurrentProcess", {psn}) == 0 && f.engine.read32(psn + 4) == 2, "GetCurrentProcess");
    require(f.process.resolve(as, "_PlotIconRefInContext").has_value(), "PlotIconRefInContext binds (and fails when called)");
});

} // namespace
