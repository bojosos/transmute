#pragma once

#include "alchemy/gui/install.hpp"

// SkyLight's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::skylight {

// install.cpp (fill-only, through gui::values::Provider).
void install_skylight(gui::InstallContext& context);

} // namespace alchemy::skylight
