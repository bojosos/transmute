// AuthenticationServices (src/alchemy/darwin/authenticationservices/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/AuthenticationServices.framework/Versions/A/AuthenticationServices"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case authenticationservices_case("authenticationservices", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "AuthenticationServices identity");
    require(f.send(f.cls("ASAuthorizationController"), "superclass") == f.cls("NSObject"), "ASAuthorizationController");
    require(f.string_constant(LIBRARY, "_ASAuthorizationErrorDomain") == "com.apple.AuthenticationServices.AuthorizationError", "error domain");
    const auto manager = f.send(f.send(f.cls("ASAuthorizationWebBrowserPublicKeyCredentialManager"), "alloc"), "init");
    require(f.send(manager, "authorizationStateForPlatformCredentials") == 1, "platform credentials denied");
});

#undef LIBRARY
} // namespace
