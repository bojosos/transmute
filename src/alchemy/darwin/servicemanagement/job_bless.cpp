#include "alchemy/gui/value_provider.hpp"

// ServiceManagement: SMJobBless fails (no launchd), in kSMErrorDomainLaunchd;
// no job is ever loaded and login items cannot be enabled. SMAppService (macOS 13)
// reports every service as not registered, and registering one fails. Cut from
// darwin/coreservices/services.cpp (install_services calls it in place);
// Chrome's images first imported it.
namespace alchemy::gui::values {

void Provider::install_service_management() {
    raw(service_management, "_SMJobBless", [this](CpuState& s, GuestMemory&) {
        // Boolean SMJobBless(domain, label, auth, CFErrorRef *): no launchd.
        if (s.x[3]) memory().write64(s.x[3], bridge->error("CFErrorDomainLaunchd", 2, "No launchd exists under Alchemy"));   // kSMErrorInternalFailure
        s.x[0] = 0;
    });
    // SMErrors.h: the launchd error domain, as SMJobBless's errors report it
    // ("CFErrorDomainLaunchd error 2"; the value is from those reports, not a header).
    string(service_management, "_kSMErrorDomainLaunchd", "CFErrorDomainLaunchd");
    // CFArrayRef SMCopyAllJobDictionaries(CFStringRef domain): no launchd, so
    // no job is loaded (an empty array). Boolean SMLoginItemSetEnabled(id,
    // enabled): login items cannot be registered (false).
    raw(service_management, "_SMCopyAllJobDictionaries", [this](CpuState& s, GuestMemory&) { s.x[0] = cf.array({}); });
    // CFDictionaryRef SMJobCopyDictionary(CFStringRef domain, CFStringRef label): no job is loaded (NULL).
    raw(service_management, "_SMJobCopyDictionary", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    constant(service_management, "_SMLoginItemSetEnabled", 0);

    // SMAppService: the main app, login items (by bundle identifier), and launch agents and
    // daemons (by property-list name). Without launchd nothing can be registered: status is
    // SMAppServiceStatusNotRegistered (0), -registerAndReturnError: fails with
    // kSMErrorServiceUnavailable (7) and unregistering fails with kSMErrorJobNotFound (6), both
    // in the launchd error domain SMJobBless reports.
    const auto service = cls(service_management, "SMAppService", "NSObject");
    object_properties(service, {"alchemyName"});
    integer_properties(service, {"alchemyKind"});
    cleanup_on_dealloc(service);
    const auto make = [this, service](std::int64_t kind, Address name) {
        const auto object = create(service);
        store_integer(object, "alchemyKind", kind);
        if (name) store(object, "alchemyName", name);
        return call(fill::libobjc, "_objc_autorelease", {object});
    };
    singleton(service, "mainAppService");
    method(service, "loginItemServiceWithIdentifier:", [make](CpuState& s, GuestMemory&) { s.x[0] = make(1, s.x[2]); }, true, "@@:@");
    method(service, "agentServiceWithPlistName:", [make](CpuState& s, GuestMemory&) { s.x[0] = make(2, s.x[2]); }, true, "@@:@");
    method(service, "daemonServiceWithPlistName:", [make](CpuState& s, GuestMemory&) { s.x[0] = make(3, s.x[2]); }, true, "@@:@");
    method(service, "status", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "q@:");
    // + (SMAppServiceStatus)statusForLegacyURL:(NSURL*): no legacy login item exists either.
    method(service, "statusForLegacyURL:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "q@:@");
    method(service, "registerAndReturnError:", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], bridge->error("CFErrorDomainLaunchd", 7, "No launchd exists under Alchemy, so services cannot be registered"));
        s.x[0] = 0;
    }, false, "B@:^@");
    method(service, "unregisterAndReturnError:", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], bridge->error("CFErrorDomainLaunchd", 6, "The service is not registered"));
        s.x[0] = 0;
    }, false, "B@:^@");
    method(service, "unregisterWithCompletionHandler:", [this](CpuState& s, GuestMemory&) {
        if (s.x[2]) complete_with_error(s.x[2], "CFErrorDomainLaunchd", 6, "The service is not registered", 0);
    }, false, "v@:@?");
    // + openSystemSettingsLoginItems: there is no System Settings to open.
    method(service, "openSystemSettingsLoginItems", [this](CpuState&, GuestMemory&) {
        notice_once("SMAppService openSystemSettingsLoginItems: no System Settings under Alchemy");
    }, true, "v@:");
}

} // namespace alchemy::gui::values
