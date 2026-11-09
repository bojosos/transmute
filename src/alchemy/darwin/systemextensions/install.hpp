#pragma once

#include "alchemy/gui/install.hpp"

// SystemExtensions' row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::systemextensions {

// install.cpp (fill-only, through gui::values::Provider).
void install_systemextensions(gui::InstallContext& context);

} // namespace alchemy::systemextensions
