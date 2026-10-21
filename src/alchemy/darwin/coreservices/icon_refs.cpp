#include "alchemy/darwin/kit/support/support_kit.hpp"

// CoreServices (Icon Services): ReleaseIconRef. GetIconRef hands out no
// icons, so there is nothing to release.
// Qt 6.5's bind-time surface first needed it (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

void install_coreservices_icon_refs(Support& support) {
    // GetIconRef hands out no icons, so there is nothing to release.
    support.fill_function("/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", "_ReleaseIconRef",
                          [](CpuState& c, GuestMemory&) { c.x[0] = 0; });
}

} // namespace alchemy::gui::support_kit
