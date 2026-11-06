#pragma once

#include "alchemy/gui/install.hpp"

// MetalFX's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::metalfx {

// install.cpp (fill-only, through gui::values::Provider).
void install_metalfx(gui::InstallContext& context);

} // namespace alchemy::metalfx
