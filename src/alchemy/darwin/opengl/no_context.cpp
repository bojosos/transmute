#include "alchemy/darwin/kit/support/support_kit.hpp"

// The AGL identity: libqcocoa links it without importing from it. OpenGL
// itself is the install_opengl row's (darwin/opengl/install.cpp).
namespace alchemy::gui::support_kit {

void install_opengl_no_context(Support&) { ProcessContext::add_provider_library(std::string(agl_library)); }

} // namespace alchemy::gui::support_kit
