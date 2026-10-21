#pragma once

#include "alchemy/gui/install.hpp"

// SafariServices's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::safariservices {

// install.cpp (fill-only, through gui::values::Provider).
void install_safariservices(gui::InstallContext& context);

} // namespace alchemy::safariservices
