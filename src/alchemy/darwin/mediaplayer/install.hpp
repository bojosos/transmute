#pragma once

#include "alchemy/gui/install.hpp"

// MediaPlayer's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::mediaplayer {

// install.cpp (fill-only, through gui::values::Provider).
void install_mediaplayer(gui::InstallContext& context);

} // namespace alchemy::mediaplayer
