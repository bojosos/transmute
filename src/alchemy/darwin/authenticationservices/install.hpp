#pragma once

#include "alchemy/gui/install.hpp"

// AuthenticationServices's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::authenticationservices {

// install.cpp (fill-only, through gui::values::Provider).
void install_authenticationservices(gui::InstallContext& context);

} // namespace alchemy::authenticationservices
