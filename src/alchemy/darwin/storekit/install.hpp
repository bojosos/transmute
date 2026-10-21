#pragma once

#include "alchemy/gui/install.hpp"

// StoreKit's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::storekit {

// install.cpp (fill-only, through gui::values::Provider).
void install_storekit(gui::InstallContext& context);

} // namespace alchemy::storekit
