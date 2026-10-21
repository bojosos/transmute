#include "alchemy/gui/value_provider.hpp"

// Legacy Security interfaces Chrome's images import, cut from
// darwin/coreservices/services.cpp (install_services calls it in place): the
// legacy keychain status is real (the one default keychain, unlocked,
// readable and writable); code signing and CSSM-era ACL calls answer
// errSecUnimplemented. Code-signing dictionary keys are exported.
namespace alchemy::gui::values {
namespace {

using fill::security;
using fill::status;

constexpr std::int32_t sec_unimplemented = -4, sec_param = -50;

} // namespace

void Provider::install_legacy_keychain() {
    raw(security, "_SecKeychainGetStatus", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status(sec_param); return; }
        m.write32(s.x[1], 1 | 2 | 4);   // kSecUnlockStateStatus | kSecReadPermStatus | kSecWritePermStatus
        s.x[0] = 0;
    });
    raw(security, "_SecKeychainItemCopyKeychain", [this](CpuState& s, GuestMemory&) {
        // Every item is in the one default keychain.
        if (!s.x[0] || !s.x[1]) { s.x[0] = status(sec_param); return; }
        s.x[0] = call(security, "_SecKeychainCopyDefault", {s.x[1]});
    });
    failure(security, "_SecACLCopyContents", sec_unimplemented, {1, 2});
    failure(security, "_SecACLSetContents", sec_unimplemented);
    failure(security, "_SecAccessCopyACLList", sec_unimplemented, {1});
    failure(security, "_SecAccessCreate", sec_unimplemented, {2});
    failure(security, "_SecKeychainItemCreateFromContent", sec_unimplemented, {6});
    failure(security, "_SecCodeSignerCreate", sec_unimplemented, {2});
    failure(security, "_SecCodeSignerAddSignatureWithErrors", sec_unimplemented, {3});
    // <Security/SecCode.h>, CSCommon.h keys. The SecCodeSigner keys are SPI and
    // the kSecCFError* values unverified.
    string(security, "_kSecCodeInfoCertificates", "certificates");
    string(security, "_kSecCodeInfoFlags", "flags");
    string(security, "_kSecCodeInfoMainExecutable", "main-executable");
    string(security, "_kSecCodeInfoPList", "info-plist");
    string(security, "_kSecCodeInfoUnique", "unique");
    string(security, "_kSecGuestAttributeAudit", "audit");
    string(security, "_kSecGuestAttributeDynamicCode", "dynamicCode");
    string(security, "_kSecGuestAttributeDynamicCodeInfoPlist", "dynamicCodeInfoPlist");
    string(security, "_kSecGuestAttributePid", "pid");
    string(security, "_kSecCFErrorResourceAltered", "SecCSResourceAltered");
    string(security, "_kSecCFErrorResourceMissing", "SecCSResourceMissing");
    string(security, "_kSecCodeSignerEntitlements", "entitlements");
    string(security, "_kSecCodeSignerFlags", "flags");
    string(security, "_kSecCodeSignerIdentity", "signer");
}

} // namespace alchemy::gui::values
