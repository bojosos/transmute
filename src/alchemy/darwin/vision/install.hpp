#pragma once

#include "alchemy/gui/install.hpp"

// Vision's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::vision {

// install.cpp (fill-only, through gui::values::Provider).
void install_vision(gui::InstallContext& context);

} // namespace alchemy::vision
