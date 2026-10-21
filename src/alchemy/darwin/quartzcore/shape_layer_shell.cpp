#include "alchemy/darwin/kit/support/support_kit.hpp"

// QuartzCore: the CAShapeLayer class shell (a CALayer with no methods of its
// own).
// Qt 6.5's bind-time surface first needed it (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

namespace {

constexpr std::string_view quartz_core = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";

} // namespace

void install_quartzcore_shape_layer_shell(Support& support) {
    const auto shell = [&](std::string_view library, const char* name, const char* superclass) {
        if (support.process.resolve(library, "_OBJC_CLASS_$_" + std::string(name))) return;
        if (!support.objc.find_class(superclass)) return;
        static_cast<void>(support.declare_class(name, superclass, library));
    };
    shell(quartz_core, "CAShapeLayer", "CALayer");
}

} // namespace alchemy::gui::support_kit
