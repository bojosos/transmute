#include "alchemy/darwin/kit/support/support_kit.hpp"

// ApplicationServices (Icon Services): PlotIconRefInContext fails explicitly
// (icon services draw nothing).
// Qt 6.5's bind-time surface first needed it (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

namespace {

constexpr std::string_view application_services = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";

} // namespace

void install_applicationservices_icon_services(Support& support) {
    support.fill_function(application_services, "_PlotIconRefInContext", [](CpuState&, GuestMemory&) {
        unsupported("PlotIconRefInContext (icon services draw nothing)");
    });
}

} // namespace alchemy::gui::support_kit
