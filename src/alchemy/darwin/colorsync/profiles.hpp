#pragma once

#include "alchemy/gui/install.hpp"

// ColorSync profiles' row of the GUI installer table (src/alchemy/gui/install.cpp; alchemy-fx only).
namespace alchemy::colorsync {

// profiles.cpp (through gui::values::Provider).
void install_colorsync_profiles(gui::InstallContext& context);

} // namespace alchemy::colorsync
