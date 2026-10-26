#pragma once

#include "alchemy/gui/window_server.hpp"

// NSStatusBar / NSStatusItem / NSStatusBarButton and NSSound (status_items.cpp),
// installed by AppKit::install (appkit.cpp); window.cpp hands status item
// clicks over.
namespace alchemy::gui::appkit {

struct AppKit;
void install_status_items(AppKit& kit);
void status_item_clicked(AppKit& kit, const Event& event);

} // namespace alchemy::gui::appkit
