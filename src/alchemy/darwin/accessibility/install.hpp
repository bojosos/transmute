#pragma once

#include "alchemy/gui/install.hpp"

// Accessibility's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::accessibility {

// install.cpp (fill-only, through gui::values::Provider).
void install_accessibility(gui::InstallContext& context);

} // namespace alchemy::accessibility
