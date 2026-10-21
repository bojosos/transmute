#pragma once

#include "alchemy/gui/install.hpp"

// ApplicationServices' rows of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only). Its other pieces are installed by the aggregators that
// installed them before, at their old position:
//   support kit: support_kit::install_applicationservices_icon_services (icon_services.cpp)

namespace alchemy::applicationservices {

// The umbrella identity and its re-exports (install.cpp).
void install_applicationservices(gui::InstallContext& context);

} // namespace alchemy::applicationservices
