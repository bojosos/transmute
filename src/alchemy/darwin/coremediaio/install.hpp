#pragma once

#include "alchemy/gui/install.hpp"

// CoreMediaIO's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::coremediaio {

// install.cpp (fill-only, through gui::values::Provider).
void install_coremediaio(gui::InstallContext& context);

} // namespace alchemy::coremediaio
