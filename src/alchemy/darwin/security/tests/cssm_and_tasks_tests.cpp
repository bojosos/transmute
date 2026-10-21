// Security pieces moved from darwin/identities/system_gaps.cpp (cssm_and_tasks.cpp):
// CSSM OIDs and unimplemented calls, SecTask, SecAccessControl, the keychain
// search list, user interaction and keychain callbacks.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view sec = "/System/Library/Frameworks/Security.framework/Versions/A/Security", cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }

const Case security_cssm_and_tasks_case("security_cssm_and_tasks", [](Fixture& f) {
    const auto oid = f.symbol(sec, "_CSSMOID_SHA1WithRSA");
    require(f.engine.read64(oid) == 9 && f.memory().read8(f.engine.read64(oid + 8) + 8) == 5, "CSSMOID_SHA1WithRSA");
    const auto task = f.call(sec, "_SecTaskCreateFromSelf", {0});
    require(task && f.call(sec, "_SecTaskCopyValueForEntitlement", {task, string(f, "com.apple.security.app-sandbox"), 0}) == 0, "no entitlements");
    require(f.call(sec, "_SecTaskGetCodeSignStatus", {task}) == 0, "unsigned");
    require(f.status(sec, "_SecTrustSetKeychains", {0x1, f.cf.array({})}) == 0, "empty keychain list honoured");
    require(f.status(sec, "_SecTrustSetOCSPResponse", {0x1, 0}) == -4, "OCSP responses unsupported");
    const auto access = f.call(sec, "_SecAccessControlCreateWithFlags", {0, string(f, "ak"), 1, 0});
    require(access && f.call(cf, "_CFGetTypeID", {access}) == f.call(sec, "_SecAccessControlGetTypeID"), "SecAccessControl");
    const auto allowed = f.block(1);
    static_cast<void>(f.call(sec, "_SecKeychainSetUserInteractionAllowed", {0}));
    require(f.status(sec, "_SecKeychainGetUserInteractionAllowed", {allowed}) == 0 && f.memory().read8(allowed) == 0, "user interaction state");
    const auto list = f.block(8);
    require(f.status(sec, "_SecKeychainCopySearchList", {list}) == 0 && f.cf.length(f.engine.read64(list)) == 1, "search list");
    const auto keychain_callback = f.symbol(sec, "_SecKeychainOpen");   // any function address
    require(f.status(sec, "_SecKeychainAddCallback", {keychain_callback, 0xffff, 0}) == 0, "keychain callback added");
    require(f.status(sec, "_SecKeychainAddCallback", {keychain_callback, 0xffff, 0}) == -25297, "duplicate keychain callback");
    require(f.status(sec, "_SecKeychainRemoveCallback", {keychain_callback}) == 0 &&
            f.status(sec, "_SecKeychainRemoveCallback", {keychain_callback}) == -25298, "keychain callback removed");
});

} // namespace
