#include "alchemy/darwin/kit/support/support_kit.hpp"

// CoreGraphics: CGColorSpaceCopyPropertyList answers NULL: no serialized
// property list exists for the provider's color spaces.
// Qt 6.5's bind-time surface first needed it (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

void install_coregraphics_color_space_property_list(Support& support) {
    // No serialized property list exists for the provider's color spaces.
    support.fill_function(core_graphics_library, "_CGColorSpaceCopyPropertyList", [](CpuState& c, GuestMemory&) { c.x[0] = 0; });
}

} // namespace alchemy::gui::support_kit
