#pragma once

#include "alchemy/gui/install.hpp"

// QuickLookUI's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::quicklookui {

// install.cpp (fill-only, through gui::values::Provider).
void install_quicklookui(gui::InstallContext& context);

} // namespace alchemy::quicklookui
