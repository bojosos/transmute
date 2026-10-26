#pragma once

#include "alchemy/gui/install.hpp"

// QuickLookThumbnailing's row of the GUI installer table
// (src/alchemy/gui/install.cpp; alchemy-fx only).
namespace alchemy::quicklookthumbnailing {

// install.cpp (through gui::values::Provider).
void install_quicklookthumbnailing(gui::InstallContext& context);

} // namespace alchemy::quicklookthumbnailing
