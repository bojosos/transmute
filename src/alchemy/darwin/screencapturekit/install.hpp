#pragma once

#include "alchemy/gui/install.hpp"

// ScreenCaptureKit's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::screencapturekit {

// install.cpp (through the support kit, which runs before this row).
void install_screencapturekit(gui::InstallContext& context);

} // namespace alchemy::screencapturekit
