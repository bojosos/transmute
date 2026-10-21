#include "alchemy/darwin/appkit/windows/windows_internal.hpp"

// QLab gap group G4 (windows): AppKit windows, panels, popovers, documents, workspace, drag and drop, Auto Layout, stack/grid views, nib connectors.
// Owner and scope: docs/ALCHEMY_QLAB_GAPS.md, group G4. Each provider file in
// this directory adds one install_* function called from here; cmake/darwin/kit_completion_windows.cmake
// lists the sources and the group's tests.
namespace alchemy::gui::completion_kit {

void install_group_windows(Support& support) {
    if (!appkit::instance()) return;   // no AppKit provider (alchemy doctor without the GUI)
    windows::install_auto_layout(support);
    windows::install_control_metrics(support);
    windows::install_stack_and_grid(support);
    windows::install_window_extras(support);
    windows::install_workspace_and_dragging(support);
    windows::install_popover(support);
    windows::install_drag_session(support);
}

} // namespace alchemy::gui::completion_kit
