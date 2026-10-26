#include "alchemy/gui/value_provider.hpp"

#include <stdexcept>
#include <string>

// IOKit gaps of Chrome's images, cut from darwin/coreservices/services.cpp
// (install_services calls it in place): HID access is granted (no TCC);
// SCDynamicStoreCopyValue binds to the SystemConfiguration provider through
// IOKit's ordinal. The HID event-system client (IOHIDFamily's open-source
// IOHIDEventSystemClient interface, which Electron reads thermal sensors with)
// is unavailable: no client is ever created, so there are no services or events.
namespace alchemy::gui::values {
namespace {

using fill::iokit;
using fill::system_configuration;

} // namespace

void Provider::install_iokit_gaps() {
    // Chromium binds SCDynamicStoreCopyValue through IOKit's ordinal.
    if (!has(iokit, "_SCDynamicStoreCopyValue"))
        if (const auto found = process.resolve(system_configuration, "_SCDynamicStoreCopyValue"))
            process.install_export(std::string(iokit), "_SCDynamicStoreCopyValue", {found->address, ExportKind::function});
    constant(iokit, "_IOHIDCheckAccess", 0);   // kIOHIDAccessTypeGranted: no input-monitoring permission exists
    constant(iokit, "_IOHIDRequestAccess", 1);  // true

    // IOHIDEventSystemClientCreate(allocator) answers NULL; the calls on a client, service or
    // event accept only NULL (nothing else can exist) and answer empty.
    const auto null_only = [](std::string name) {
        return [name = std::move(name)](CpuState& s, GuestMemory&) {
            if (s.x[0]) throw std::runtime_error("Alchemy IOKit: " + name + " of an object Alchemy never created (no HID event system)");
            s.x[0] = 0;
        };
    };
    raw(iokit, "_IOHIDEventSystemClientCreate", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    for (const auto* name : {"_IOHIDEventSystemClientSetMatching", "_IOHIDEventSystemClientCopyServices",
                             "_IOHIDServiceClientCopyProperty", "_IOHIDServiceClientCopyEvent"})
        raw(iokit, name, null_only(name + 1));
    raw(iokit, "_IOHIDEventGetFloatValue", [check = null_only("IOHIDEventGetFloatValue")](CpuState& s, GuestMemory& m) {
        check(s, m);
        s.v[0][0] = 0; s.v[0][1] = 0;   // 0.0
    });
}

} // namespace alchemy::gui::values
