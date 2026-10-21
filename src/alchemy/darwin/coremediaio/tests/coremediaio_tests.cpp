// CoreMediaIO (src/alchemy/darwin/coremediaio/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/CoreMediaIO"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case coremediaio_case("coremediaio", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "CoreMediaIO identity");
    require(f.status(LIBRARY, "_CMIOObjectGetPropertyData", {1, 0, 0, 0, 0, 0, 0}) == static_cast<std::int32_t>(0x216f626a), "no DAL objects");
    require(f.call(LIBRARY, "_CMIOObjectHasProperty", {1, 0}) == 0, "no properties");
});

#undef LIBRARY
} // namespace
