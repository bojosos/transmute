#include "alchemy/darwin/headless_additions.hpp"

// alchemy-fx: CGL, CGLTexImageIOSurface2D included, is the GUI OpenGL
// provider's (cgl_contexts.cpp, installer row install_opengl); the headless
// answer (cgl.cpp) is alchemy.exe's only.
namespace alchemy::darwin_headless {

void install_opengl_cgl(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_headless
