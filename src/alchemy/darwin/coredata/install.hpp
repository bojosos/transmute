#pragma once

#include "alchemy/gui/install.hpp"

// CoreData's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::coredata {

// install.cpp (fill-only, through gui::values::Provider).
void install_coredata(gui::InstallContext& context);

} // namespace alchemy::coredata
