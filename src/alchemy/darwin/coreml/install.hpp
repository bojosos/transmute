#pragma once

#include "alchemy/gui/install.hpp"

// CoreML's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::coreml {

// install.cpp (fill-only, through gui::values::Provider).
void install_coreml(gui::InstallContext& context);

} // namespace alchemy::coreml
