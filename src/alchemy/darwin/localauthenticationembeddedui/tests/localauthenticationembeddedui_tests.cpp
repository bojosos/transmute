// LocalAuthenticationEmbeddedUI (src/alchemy/darwin/localauthenticationembeddedui/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/LocalAuthenticationEmbeddedUI.framework/Versions/A/LocalAuthenticationEmbeddedUI"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case localauthenticationembeddedui_case("localauthenticationembeddedui", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "LocalAuthenticationEmbeddedUI identity");
    require(f.send(f.cls("LAAuthenticationView"), "superclass") == f.cls("NSView"), "LAAuthenticationView is an NSView");
});

#undef LIBRARY
} // namespace
