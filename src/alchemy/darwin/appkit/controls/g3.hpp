#pragma once

// QLab gap group G3 (docs/ALCHEMY_QLAB_GAPS.md): AppKit controls, text,
// tables and accessibility. Shared by the providers of this directory, which
// add AppKit classes (and methods to AppKit's own) over AppKit's host state:
// they draw on AppKit's software drawing layer and track the mouse through
// its event loop, exactly as src/alchemy/darwin/appkit/controls.cpp does.
//   segmented.cpp     NSSegmentedControl / NSSegmentedCell / NSSegmentItem
//   path_control.cpp  NSPathControl / NSPathCell / NSPathComponentCell
//   search_field.cpp  NSSearchField / NSSearchFieldCell
//   date_picker.cpp   NSDatePicker / NSDatePickerCell
//   color_well.cpp    NSColorWell
//   text_system.cpp   NSTextStorage / NSLayoutManager / NSTextContainer /
//                     NSTextViewSharedData coding, NSTextView's attribute,
//                     selection and typing API, NSFontManager / NSFontPanel
//   tables.cpp        NSComboTableView, NSTableRowView emphasis, table accessibility
//   collection.cpp    NSCollectionView, its items and its flow layout
//   ruler.cpp         NSRulerView and NSScrollView's rulers and insets
//   accessibility.cpp NSAccessibilityElement and the NSView accessibility properties
//   small.cpp         button, menu, popup, slider, scroller, progress and touch additions
//   appearance.cpp    dynamic system colours resolved for the appearance drawn in
// The host-only cores (layout.cpp, date_elements.cpp) have unit tests in tests/.

#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace alchemy::gui::completion_kit::controls {

using appkit::AppKit;
using appkit::CellState;
using appkit::ControlState;
using appkit::Theme;
using appkit::ViewState;
using drawing_color = appkit::drawing::Color;

// The AppKit provider of `support`'s process these classes extend (null
// when AppKit is not installed there).
[[nodiscard]] AppKit* appkit_of(Support& support);

// Keyed decoding through any NSCoder (the nib coder of nib.cpp, or a keyed
// unarchiver): the -decode...ForKey: messages, so the coder stays the
// coder's business.
struct Decoder {
    AppKit& kit;
    Address coder;
    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] Address object(std::string_view key) const;                     // not retained (owned by the coder/nib)
    [[nodiscard]] std::int64_t integer(std::string_view key, std::int64_t fallback = 0) const;
    [[nodiscard]] double real(std::string_view key, double fallback = 0) const;
    [[nodiscard]] bool flag(std::string_view key, bool fallback = false) const;
    [[nodiscard]] std::optional<std::string> string(std::string_view key) const;  // an NSString object's text
    [[nodiscard]] std::optional<Rect> rect(std::string_view key) const;
};

// Registers forwarders on a control class for cell methods (NSControl's
// pattern: the control's API is its cell's). `redisplay` marks the control
// for display after the call.
void forward_to_cell(AppKit& kit, Address control_class, std::string name, std::string types, bool redisplay);

// A dictionary's value for a string key (0: none).
[[nodiscard]] Address dictionary_get(AppKit& kit, Address dictionary, std::string_view key);

// Super's -initWithCoder: (from `cls`'s superclass).
Address super_init_with_coder(AppKit& kit, Address self, Address cls, Address coder);

void install_segmented(AppKit& kit);
void install_path_control(AppKit& kit);
void install_search_field(AppKit& kit);
void install_date_picker(AppKit& kit);
void install_color_well(AppKit& kit);
void install_text_system(AppKit& kit);
// NSLayoutManager's glyphs and line fragments laid out by CoreText, and the
// NSTextContainer additions (text_layout.cpp; installed by install_text_system).
void install_text_layout(AppKit& kit);
// The manager's layout is made again on next use / dropped with the manager.
void invalidate_text_layout(AppKit& kit, Address manager);
void forget_text_layout(AppKit& kit, Address manager);
// NSText's and NSTextView's actions and API (text_actions.cpp), NSTextFinder
// and the find actions it shares with NSTextView, and the TextKit 2 stops
// (text_finder.cpp); installed by install_text_system.
void install_text_actions(AppKit& kit);
void install_text_finder(AppKit& kit);
[[nodiscard]] bool find_action_available(AppKit& kit, Address client, std::int64_t action);
void perform_find_action(AppKit& kit, Address client, std::int64_t action);
void install_tables(AppKit& kit);
void install_collection_view(AppKit& kit);
void install_rulers(AppKit& kit);
void install_accessibility(AppKit& kit);
void install_small_additions(AppKit& kit);

} // namespace alchemy::gui::completion_kit::controls
