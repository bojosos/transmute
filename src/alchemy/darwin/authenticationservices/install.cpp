#include "alchemy/darwin/authenticationservices/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// AuthenticationServices: no authenticator exists. Authorization requests
// complete with ASAuthorizationErrorNotHandled; the browser credential
// manager reports access as denied and has no platform credentials. The
// Apple ID, password and platform public-key providers make real request
// objects (and the Apple ID provider knows no user: credential state
// NotFound); a web authentication session cannot start (no browser can
// present it: -canStart and -start are NO, the completion is not called).
// String constants carry the values of Apple's public headers; values
// marked "(unverified)" follow the framework's naming convention and could
// not be checked against a header. Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::authenticationservices {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view authentication_services = "/System/Library/Frameworks/AuthenticationServices.framework/Versions/A/AuthenticationServices";
constexpr std::int64_t as_authorization_error_not_handled = 1003;

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(authentication_services));
    const auto nothing = [](CpuState&, GuestMemory&) {};
    string(authentication_services, "_ASAuthorizationErrorDomain", "com.apple.AuthenticationServices.AuthorizationError");
    string(authentication_services, "_ASWebAuthenticationSessionErrorDomain", "com.apple.AuthenticationServices.WebAuthenticationSession");
    // WebAuthn's attestation conveyance and user verification strings.
    string(authentication_services, "_ASAuthorizationPublicKeyCredentialAttestationKindNone", "none");
    string(authentication_services, "_ASAuthorizationPublicKeyCredentialAttestationKindDirect", "direct");
    string(authentication_services, "_ASAuthorizationPublicKeyCredentialAttestationKindIndirect", "indirect");
    string(authentication_services, "_ASAuthorizationPublicKeyCredentialAttestationKindEnterprise", "enterprise");
    string(authentication_services, "_ASAuthorizationPublicKeyCredentialUserVerificationPreferenceDiscouraged", "discouraged");
    string(authentication_services, "_ASAuthorizationPublicKeyCredentialUserVerificationPreferencePreferred", "preferred");
    string(authentication_services, "_ASAuthorizationPublicKeyCredentialUserVerificationPreferenceRequired", "required");
    const auto controller = cls(authentication_services, "ASAuthorizationController", "NSObject");
    object_properties(controller, {"authorizationRequests"});
    weak_properties(controller, {"delegate", "presentationContextProvider"});
    method(controller, "initWithAuthorizationRequests:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "authorizationRequests", s.x[2]);
    });
    // -performRequests: no authenticator handles any request; the delegate
    // hears authorizationController:didCompleteWithError: on the main queue,
    // as after a real attempt. Sent through the runtime (Chrome subclasses
    // ASAuthorizationController and implements the delegate).
    const auto perform = [this](CpuState& s, GuestMemory&) {
        const auto self = retain(s.x[0]);
        bridge->dispatch_async(bridge->main_queue(), [this, self] {
            const auto delegate = static_cast<Address>(stored_integer(self, "delegate"));
            if (delegate) {
                const auto error = bridge->error("com.apple.AuthenticationServices.AuthorizationError",
                                                 as_authorization_error_not_handled, "No authenticator is available");
                CallRegs registers{};
                registers.x[2] = self;
                registers.x[3] = error;
                static_cast<void>(objc.send_if_responds(delegate, objc.selector("authorizationController:didCompleteWithError:"), registers));
                release(error);
            }
            release(self);
        });
    };
    method(controller, "performRequests", perform, false, "v@:");
    method(controller, "performAutoFillAssistedRequests", perform, false, "v@:");
    method(controller, "cancel", nothing, false, "v@:");
    for (const auto* name : {"ASAuthorizationPlatformPublicKeyCredentialAssertion", "ASAuthorizationPlatformPublicKeyCredentialRegistration",
                             "ASAuthorizationPlatformPublicKeyCredentialDescriptor", "ASAuthorizationSingleSignOnProvider",
                             "ASWebAuthenticationSessionWebBrowserSessionManager",
                             "ASAuthorizationPublicKeyCredentialLargeBlobAssertionInput",
                             "ASAuthorizationPublicKeyCredentialLargeBlobRegistrationInput",
                             "ASAuthorizationPublicKeyCredentialPRFAssertionInput",
                             "ASAuthorizationPublicKeyCredentialPRFAssertionInputValues",
                             "ASAuthorizationPublicKeyCredentialPRFRegistrationInput"})
        static_cast<void>(cls(authentication_services, name, "NSObject"));
    // The browser session manager: Chrome registers its session handler; the
    // system never starts a web authentication session in this browser.
    const auto session_manager = objc.find_class("ASWebAuthenticationSessionWebBrowserSessionManager");
    singleton(session_manager, "sharedManager");
    weak_properties(session_manager, {"sessionHandler"});
    method(session_manager, "wasLaunchedByAuthenticationServices", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    const auto descriptor = objc.find_class("ASAuthorizationPlatformPublicKeyCredentialDescriptor");
    object_properties(descriptor, {"credentialID"});
    method(descriptor, "initWithCredentialID:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "credentialID", s.x[2]); });
    const auto platform_provider = cls(authentication_services, "ASAuthorizationPlatformPublicKeyCredentialProvider", "NSObject");
    object_properties(platform_provider, {"relyingPartyIdentifier"});
    method(platform_provider, "initWithRelyingPartyIdentifier:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "relyingPartyIdentifier", s.x[2]);
    });

    // Requests (value objects) and the providers that make them.
    string(authentication_services, "_ASAuthorizationScopeFullName", "full_name");
    string(authentication_services, "_ASAuthorizationScopeEmail", "email");
    string(authentication_services, "_ASAuthorizationOperationImplicit", "implicit");
    string(authentication_services, "_ASAuthorizationOperationLogin", "login");
    string(authentication_services, "_ASAuthorizationOperationRefresh", "refresh");
    string(authentication_services, "_ASAuthorizationOperationLogout", "logout");
    const auto request = cls(authentication_services, "ASAuthorizationRequest", "NSObject");
    object_properties(request, {"provider"});
    const auto openid_request = cls(authentication_services, "ASAuthorizationOpenIDRequest", "ASAuthorizationRequest");
    object_properties(openid_request, {"provider", "requestedScopes", "state", "nonce", "requestedOperation"});
    const auto apple_id_request = cls(authentication_services, "ASAuthorizationAppleIDRequest", "ASAuthorizationOpenIDRequest");
    object_properties(apple_id_request, {"provider", "requestedScopes", "state", "nonce", "requestedOperation", "user"});
    const auto password_request = cls(authentication_services, "ASAuthorizationPasswordRequest", "ASAuthorizationRequest");
    const auto registration_request = cls(authentication_services, "ASAuthorizationPlatformPublicKeyCredentialRegistrationRequest", "ASAuthorizationRequest");
    object_properties(registration_request, {"provider", "relyingPartyIdentifier", "challenge", "name", "displayName", "userID",
                                             "userVerificationPreference", "attestationPreference", "largeBlob", "prf"});
    const auto assertion_request = cls(authentication_services, "ASAuthorizationPlatformPublicKeyCredentialAssertionRequest", "ASAuthorizationRequest");
    object_properties(assertion_request, {"provider", "relyingPartyIdentifier", "challenge", "allowedCredentials",
                                          "userVerificationPreference", "largeBlob", "prf"});
    // A request of `cls` whose provider is `provider` (autoreleased).
    const auto make_request = [this](Address target, Address provider) {
        const auto object = create(target);
        store(object, "provider", provider);
        return bridge->autorelease(object);
    };
    const auto operation = [this](std::string_view name) { return bridge->constant(authentication_services, std::string("_") + std::string(name)); };
    method(platform_provider, "createCredentialRegistrationRequestWithChallenge:name:userID:",
           [this, make_request, registration_request](CpuState& s, GuestMemory&) {
        const auto object = make_request(registration_request, s.x[0]);
        store(object, "relyingPartyIdentifier", stored(s.x[0], "relyingPartyIdentifier"));
        store(object, "challenge", s.x[2]);
        store(object, "name", s.x[3]);
        store(object, "userID", s.x[4]);
        s.x[0] = object;
    });
    method(platform_provider, "createCredentialAssertionRequestWithChallenge:",
           [this, make_request, assertion_request](CpuState& s, GuestMemory&) {
        const auto object = make_request(assertion_request, s.x[0]);
        store(object, "relyingPartyIdentifier", stored(s.x[0], "relyingPartyIdentifier"));
        store(object, "challenge", s.x[2]);
        s.x[0] = object;
    });
    const auto apple_id_provider = cls(authentication_services, "ASAuthorizationAppleIDProvider", "NSObject");
    method(apple_id_provider, "createRequest", [this, make_request, apple_id_request, operation](CpuState& s, GuestMemory&) {
        const auto object = make_request(apple_id_request, s.x[0]);
        store(object, "requestedOperation", operation("ASAuthorizationOperationLogin"));
        s.x[0] = object;
    });
    // -getCredentialStateForUserID:completion: no Apple ID is signed in, so no user is known
    // (ASAuthorizationAppleIDProviderCredentialNotFound, no error).
    constexpr std::uint64_t credential_not_found = 2;
    method(apple_id_provider, "getCredentialStateForUserID:completion:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[3]) return;
        const auto block = bridge->block_copy(s.x[3]);
        bridge->dispatch_async(bridge->main_queue(), [this, block] {
            static_cast<void>(bridge->invoke_block(block, {credential_not_found, 0}));
            bridge->block_release(block);
        });
    }, false, "v@:@@?");
    const auto password_provider = cls(authentication_services, "ASAuthorizationPasswordProvider", "NSObject");
    method(password_provider, "createRequest", [make_request, password_request](CpuState& s, GuestMemory&) {
        s.x[0] = make_request(password_request, s.x[0]);
    });

    // ASWebAuthenticationSession: no browser can present the session.
    const auto web_session = cls(authentication_services, "ASWebAuthenticationSession", "NSObject");
    object_properties(web_session, {"URL", "callbackURLScheme", "completionHandler", "additionalHeaderFields"});
    integer_properties(web_session, {"prefersEphemeralWebBrowserSession"});
    weak_properties(web_session, {"presentationContextProvider"});
    method(web_session, "initWithURL:callbackURLScheme:completionHandler:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "URL", s.x[2]);
        store(s.x[0], "callbackURLScheme", s.x[3]);
        if (s.x[4]) {
            const auto block = bridge->block_copy(s.x[4]);
            store(s.x[0], "completionHandler", block);
            bridge->block_release(block);
        }
    });
    method(web_session, "canStart", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    method(web_session, "start", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    method(web_session, "cancel", nothing, false, "v@:");
    // ASAuthorizationWebBrowserPublicKeyCredentialManager: access to platform
    // credentials is denied (AuthorizationState: Authorized 0, Denied 1,
    // NotDetermined 2; unverified order).
    constexpr std::uint64_t authorization_denied = 1;
    const auto browser_manager = cls(authentication_services, "ASAuthorizationWebBrowserPublicKeyCredentialManager", "NSObject");
    method(browser_manager, "authorizationStateForPlatformCredentials", [](CpuState& s, GuestMemory&) { s.x[0] = authorization_denied; }, false, "q@:");
    method(browser_manager, "requestAuthorizationForPublicKeyCredentials:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto block = bridge->block_copy(s.x[2]);
        bridge->dispatch_async(bridge->main_queue(), [this, block] {
            static_cast<void>(bridge->invoke_block(block, {authorization_denied}));
            bridge->block_release(block);
        });
    }, false, "v@:@?");
    method(browser_manager, "getPlatformCredentialsForRelyingParty:completionHandler:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[3]) return;
        const auto block = bridge->block_copy(s.x[3]);
        bridge->dispatch_async(bridge->main_queue(), [this, block] {
            const auto empty = cf.array({});
            static_cast<void>(bridge->invoke_block(block, {empty}));
            release(empty);
            bridge->block_release(block);
        });
    }, false, "v@:@@?");
}

} // namespace

void install_authenticationservices(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::authenticationservices
