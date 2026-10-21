#pragma once

#include "alchemy/gui/install.hpp"

// AVFAudio's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::avfaudio {

// install.cpp (fill-only, through gui::values::Provider).
void install_avfaudio(gui::InstallContext& context);

} // namespace alchemy::avfaudio
