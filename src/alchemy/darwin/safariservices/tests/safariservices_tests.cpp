// SafariServices (src/alchemy/darwin/safariservices/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/SafariServices.framework/Versions/A/SafariServices"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case safariservices_case("safariservices", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "SafariServices identity");
    require(f.send(f.send(f.cls("SFUniversalLink"), "alloc"), "initWithWebpageURL:", {f.cf.string("https://example.com")}) == 0, "no universal link");
});

#undef LIBRARY
} // namespace
