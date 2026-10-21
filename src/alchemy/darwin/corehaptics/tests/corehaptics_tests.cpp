// CoreHaptics (src/alchemy/darwin/corehaptics/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/CoreHaptics.framework/Versions/A/CoreHaptics"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case corehaptics_case("corehaptics", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "CoreHaptics identity");
    require(f.string_constant(LIBRARY, "_CHHapticEventTypeHapticTransient") == "HapticTransient", "event type");
    const auto parameter = f.send(f.send(f.cls("CHHapticEventParameter"), "alloc"), "initWithParameterID:value:",
                                  {f.cf.string("HapticIntensity")});
    require(f.cf.utf8(f.send(parameter, "parameterID")) == "HapticIntensity", "event parameter");
});

#undef LIBRARY
} // namespace
