#include "alchemy/darwin/kit/completion/graphics/providers.hpp"
#include "alchemy/darwin/security/install.hpp"

// QLab gap group G6 (graphics): QuartzCore animations/layers, CoreImage, MetalKit, OpenGL/GLKit, CryptoKit, Swift overlays, WebKit placeholders, AppKit drawing additions.
// Owner and scope: docs/ALCHEMY_QLAB_GAPS.md, group G6. Each provider file in
// this directory adds one install_* function called from here; cmake/darwin/kit_completion_graphics.cmake
// lists the sources and the group's tests.
namespace alchemy::gui::completion_kit {

void install_group_graphics(Support& support) {
    graphics::install_metalkit(support);
    graphics::install_glkit(support);
    graphics::install_webkit(support);
    graphics::install_animation(support);
    graphics::install_transform3d(support);
    graphics::install_appkit_drawing(support);
    graphics::install_core_image(support);
    gui::install_security_cryptokit_host(support);
    graphics::install_metal_display(support);
}

} // namespace alchemy::gui::completion_kit
