#include "alchemy/darwin/opengl/install.hpp"
#include "alchemy/darwin/opengl/opengl.hpp"

namespace alchemy::opengl {

void install_opengl(gui::InstallContext& context) {
    install_gl_exports(context.process);
    install_cgl(context);
    install_nsopengl(context);
}

} // namespace alchemy::opengl
