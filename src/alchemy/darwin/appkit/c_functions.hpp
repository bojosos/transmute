#pragma once

// AppKit's C functions (c_functions.cpp): installed by AppKit::install
// (appkit.cpp); graphics.cpp asks whether screen updates are suspended.
namespace alchemy::gui::appkit {

struct AppKit;
void install_c_functions(AppKit& kit);
// NSDisableScreenUpdates / NSEnableScreenUpdates: while disabled, windows
// draw into their backing stores and are presented when updates resume.
[[nodiscard]] bool screen_updates_disabled();

} // namespace alchemy::gui::appkit
