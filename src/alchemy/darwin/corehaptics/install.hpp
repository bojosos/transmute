#pragma once

#include "alchemy/gui/install.hpp"

// CoreHaptics's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::corehaptics {

// install.cpp (fill-only, through gui::values::Provider).
void install_corehaptics(gui::InstallContext& context);

} // namespace alchemy::corehaptics
