#include "alchemy/gui/value_provider.hpp"

// ServiceManagement: SMJobBless fails (no launchd), in kSMErrorDomainLaunchd;
// no job is ever loaded and login items cannot be enabled. Cut from
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
    constant(service_management, "_SMLoginItemSetEnabled", 0);
}

} // namespace alchemy::gui::values
