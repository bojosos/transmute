#pragma once

#include "alchemy/gui/install.hpp"

// LocalAuthenticationEmbeddedUI's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::localauthenticationembeddedui {

// install.cpp (fill-only, through gui::values::Provider).
void install_localauthenticationembeddedui(gui::InstallContext& context);

} // namespace alchemy::localauthenticationembeddedui
