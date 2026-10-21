#include "alchemy/gui/value_provider.hpp"

// IOKit gaps of Chrome's images, cut from darwin/coreservices/services.cpp
// (install_services calls it in place): HID access is granted (no TCC);
// SCDynamicStoreCopyValue binds to the SystemConfiguration provider through
// IOKit's ordinal.
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
}

} // namespace alchemy::gui::values
