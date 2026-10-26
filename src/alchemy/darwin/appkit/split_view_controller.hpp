#pragma once

// NSSplitViewController and NSSplitViewItem (split_view_controller.cpp),
// installed by AppKit::install (appkit.cpp) after the view controllers.
namespace alchemy::gui::appkit {

struct AppKit;
void install_split_view_controller(AppKit& kit);

} // namespace alchemy::gui::appkit
