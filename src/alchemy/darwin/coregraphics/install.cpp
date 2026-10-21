#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"

// Quartz 2D's installation (install_quartz2d, install_darwin_gui): the
// process's Quartz provider object and its install steps, CoreText's
// included (coretext/install.cpp).
namespace alchemy::gui::quartz2d {
namespace {
std::shared_ptr<Quartz> process_provider;
} // namespace

Quartz* provider() noexcept { return process_provider.get(); }

void install(InstallContext& context) {
    auto quartz = std::make_shared<Quartz>(context.process, context.objc, context.cf);
    process_provider = quartz;
    quartz->install_geometry();
    quartz->install_colors();
    quartz->install_paths();
    quartz->install_images();
    quartz->install_gradients();
    quartz->install_contexts();
    quartz->install_imageio();
    quartz->install_coretext();
    quartz->install_appkit_drawing();
    quartz->install_displays();
}

} // namespace alchemy::gui::quartz2d

namespace alchemy::gui {
void install_quartz2d(InstallContext& context) { quartz2d::install(context); }
} // namespace alchemy::gui
