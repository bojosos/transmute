#pragma once

#include "alchemy/gui/install.hpp"

// CryptoTokenKit's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::cryptotokenkit {

// install.cpp (fill-only, through gui::values::Provider).
void install_cryptotokenkit(gui::InstallContext& context);

} // namespace alchemy::cryptotokenkit
