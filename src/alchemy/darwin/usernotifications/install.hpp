#pragma once

#include "alchemy/gui/install.hpp"

// UserNotifications's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::usernotifications {

// install.cpp (fill-only, through gui::values::Provider).
void install_usernotifications(gui::InstallContext& context);

} // namespace alchemy::usernotifications
