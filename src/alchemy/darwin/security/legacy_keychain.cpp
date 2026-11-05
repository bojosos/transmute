#include "alchemy/gui/value_provider.hpp"

// Legacy Security interfaces Chrome's images import, cut from
// darwin/coreservices/services.cpp (install_services calls it in place): the
// keychain calls, with their access control (SecAccess / SecACL), are the
// security provider's (keychain.cpp); code signers answer
// errSecUnimplemented. Guest attribute and code signer keys are exported (the
// signing-information keys are the security provider's, code_signing.cpp).
namespace alchemy::gui::values {
namespace {

using fill::security;
using fill::status;

constexpr std::int32_t sec_unimplemented = -4, sec_param = -50;

} // namespace

void Provider::install_legacy_keychain() {
    failure(security, "_SecCodeSignerCreate", sec_unimplemented, {2});
    failure(security, "_SecCodeSignerAddSignatureWithErrors", sec_unimplemented, {3});
    // <Security/SecCode.h>, CSCommon.h keys. The SecCodeSigner keys are SPI.
    string(security, "_kSecGuestAttributeAudit", "audit");
    string(security, "_kSecGuestAttributeDynamicCode", "dynamicCode");
    string(security, "_kSecGuestAttributeDynamicCodeInfoPlist", "dynamicCodeInfoPlist");
    string(security, "_kSecGuestAttributePid", "pid");
    string(security, "_kSecCodeSignerEntitlements", "entitlements");
    string(security, "_kSecCodeSignerFlags", "flags");
    string(security, "_kSecCodeSignerIdentity", "signer");
}

} // namespace alchemy::gui::values
