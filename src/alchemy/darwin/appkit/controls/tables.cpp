#include "alchemy/darwin/appkit/controls/g3.hpp"

// Tables and outlines (the cue lists, the inspectors' lists, the feedback
// window's combo box list):
// * containers.cpp's additions (install_container_additions): row views
//   (NSTableRowView, the delegate's row views, -rowViewAtRow:makeIfNecessary:,
//   -enumerateAvailableRowViewsUsingBlock:), -moveRowAtIndex:toIndex:, drop
//   targets, -editedColumn / -editedRow, the table accessibility getters,
//   NSOutlineView's item-level insert / remove / move and child indexes;
// * NSComboTableView, the list inside an NSComboBox's pop-up (archived in
//   nibs that hold a combo box): a table view decoded like one.
namespace alchemy::gui::completion_kit::controls {

void install_tables(AppKit& kit) {
    appkit::install_container_additions(kit);
    static_cast<void>(kit.declare_class("NSComboTableView", "NSTableView"));
}

} // namespace alchemy::gui::completion_kit::controls
