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
    // Providers make requests.
    const auto apple = f.send(f.send(f.cls("ASAuthorizationAppleIDProvider"), "alloc"), "init");
    const auto request = f.send(apple, "createRequest");
    require(request && f.send(request, "provider") == apple && f.cf.utf8(f.send(request, "requestedOperation")) == "login", "Apple ID request");
    static_cast<void>(f.send(request, "setRequestedScopes:", {f.cf.array(std::vector<Address>{f.cf.string("email")})}));
    require(f.send(request, "requestedScopes") != 0 && f.string_constant(LIBRARY, "_ASAuthorizationScopeEmail") == "email", "scopes");
    const auto platform = f.send(f.send(f.cls("ASAuthorizationPlatformPublicKeyCredentialProvider"), "alloc"), "initWithRelyingPartyIdentifier:",
                                 {f.cf.string("example.com")});
    const auto registration = f.send(platform, "createCredentialRegistrationRequestWithChallenge:name:userID:",
                                     {f.cf.string("challenge"), f.cf.string("user"), f.cf.string("id")});
    require(f.cf.utf8(f.send(registration, "relyingPartyIdentifier")) == "example.com" && f.cf.utf8(f.send(registration, "name")) == "user",
            "registration request");
    const auto password = f.send(f.send(f.send(f.cls("ASAuthorizationPasswordProvider"), "alloc"), "init"), "createRequest");
    require(f.send(password, "isKindOfClass:", {f.cls("ASAuthorizationRequest")}) == 1, "password request");
    // A web authentication session cannot start.
    const auto session = f.send(f.send(f.cls("ASWebAuthenticationSession"), "alloc"), "initWithURL:callbackURLScheme:completionHandler:",
                                {f.cf.string("https://example.com/"), f.cf.string("app"), 0});
    require(f.send(session, "canStart") == 0 && f.send(session, "start") == 0, "ASWebAuthenticationSession cannot start");
});

#undef LIBRARY
} // namespace
