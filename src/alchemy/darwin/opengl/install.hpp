#pragma once

#include "alchemy/gui/install.hpp"

// OpenGL's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::opengl {

// install.cpp: the gl* entry points and CGL over host::gl.
void install_opengl(gui::InstallContext& context);

} // namespace alchemy::opengl
