// SkyLight (src/alchemy/darwin/skylight/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case skylight_case("skylight", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "SkyLight identity");
    require(!f.process.resolve(LIBRARY, "_SLSMainConnectionID"), "no exports: weak imports bind to NULL");
});

#undef LIBRARY
} // namespace
