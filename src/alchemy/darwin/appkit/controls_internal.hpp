#pragma once

// AppKit standard controls (controls.cpp, containers.cpp, text.cpp): the host
// state of NSCell / NSControl and of the container views, shared between
// those files. Internal to src/alchemy/appkit.

#include "alchemy/darwin/appkit/appkit_internal.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::gui::appkit {

// NSButtonType
namespace button_type {
inline constexpr std::int64_t momentary_light = 0, push_on_push_off = 1, toggle = 2, switch_ = 3, radio = 4,
    momentary_change = 5, on_off = 6, momentary_push_in = 7, accelerator = 8, multi_level_accelerator = 9;
}
// NSBezelStyle
namespace bezel_style {
inline constexpr std::int64_t rounded = 1, regular_square = 2, disclosure = 5, shadowless_square = 6, circular = 7,
    textured_square = 8, help_button = 9, small_square = 10, textured_rounded = 11, round_rect = 12, recessed = 13,
    rounded_disclosure = 14, inline_ = 15;
}
// NSControlStateValue
inline constexpr std::int64_t state_mixed = -1, state_off = 0, state_on = 1;
// NSTextAlignment. The guest (arm64 macOS) numbers center 1 and right 2;
// the cells and text views here keep the archived numbering of the nib cell
// flags (NSCellFlags2), right 1 and center 2. Left 0, justified 3 and
// natural 4 are the same. The guest-facing accessors convert.
[[nodiscard]] constexpr std::int64_t alignment_swap(std::int64_t value) { return value == 1 ? 2 : value == 2 ? 1 : value; }
[[nodiscard]] constexpr std::int64_t alignment_from_guest(std::int64_t value) { return alignment_swap(value); }
[[nodiscard]] constexpr std::int64_t alignment_to_guest(std::int64_t value) { return alignment_swap(value); }

// One NSCell (and its subclasses' extra state).
struct CellState final : ObjectPayload {
    enum class Kind { null_cell, text, image } kind{Kind::text};
    Address contents{};                       // +1 object value (NSString, NSNumber, NSAttributedString...)
    Address title{};                          // +1 button title
    Address attributed_title{}, title_color{};   // +1 -setAttributedTitle: and its foreground color
    Address alternate_title{}, image{}, alternate_image{}, key_equivalent{}, placeholder{};   // +1
    Address font{};                           // +1 (nil: the system font)
    Address background_color{}, text_color{}; // +1
    Address represented{};                    // +1
    Address formatter{};                      // +1
    Address control_view{};                   // not retained
    Address target{}, action{};               // NSActionCell (target not retained)
    Address menu{};                           // +1 NSPopUpButtonCell's menu
    std::int64_t tag{}, state{}, alignment{4}, line_break{}, control_size{}, image_position{}, image_scaling{};
    std::int64_t bezel_style{bezel_style::rounded}, button_type{button_type::momentary_push_in}, focus_ring_type{};
    std::int64_t selected_item{-1};           // NSPopUpButtonCell
    std::uint64_t key_modifier_mask{};
    std::uint64_t send_action_mask{4};        // NSEventMaskLeftMouseUp
    bool enabled{true}, highlighted{}, bordered{}, bezeled{}, editable{}, selectable{}, scrollable{}, wraps{};
    bool continuous{}, allows_mixed{}, transparent{}, draws_background{}, secure{}, pulls_down{}, single_line{};
    bool truncates_last_line{}, shows_first_responder{}, frame_style_bezel{}, image_frame{};
    // Widths the cell's leading / trailing controls take from its text area
    // (the NSComboBox button, the NSSearchField buttons); the field editor
    // is placed between them.
    double leading_inset{}, trailing_inset{};
    // NSSliderCell / NSStepperCell (controls_more.cpp): the value is `contents`.
    double min_value{}, max_value{1}, increment{1}, alt_increment{};
    std::int64_t tick_marks{}, tick_position{}, vertical{-1};   // vertical: -1 from the frame shape
    bool ticks_only{}, value_wraps{true}, autorepeat{true};
    // NSComboBoxCell (controls_more.cpp).
    std::vector<Address> combo_items;         // +1 object values (the internal list)
    Address data_source{};                    // not retained
    std::int64_t combo_selected{-1}, visible_items{5};
    double item_height{};
    bool uses_data_source{}, completes{}, button_bordered{true}, has_vertical_scroller{true};
    // NSButtonCell -highlightsBy / -showsStateBy (NSCellStyleMask: contents 1,
    // push-in 2, change gray 4, change background 8); -setButtonType: sets
    // AppKit's pair for the type, the setters override it.
    std::uint64_t highlights_by{6}, shows_state_by{};
    bool sends_action_on_end_editing{};
    Address content_tint{};                   // +1 -contentTintColor (NSButton / NSImageView)
    // State of cell subclasses defined outside this directory (the QLab
    // controls of src/alchemy/darwin/kit/completion/controls/): one object carries one
    // payload, so subclasses extend the cell this way (as ViewState::extension).
    std::shared_ptr<ObjectPayload> extension;
};

// NSControl's extension of the view state (NSTableView's TableState extends
// it: containers.cpp).
struct ControlState : ObjectPayload {
    Address cell{};                           // +1
    Address delegate{};                       // NSTextField delegate (not retained)
    Address editor{};                         // the field editor while editing (not retained)
    Address target{}, action{}, double_action{};   // cell-less controls (NSTableView, NSScroller)
    bool refuses_first_responder{}, ignores_multi_click{};
    // NSScroller
    double knob_proportion{1};
    bool vertical{true};
    // NSProgressIndicator
    double min_value{0}, max_value{100}, value{0};
    bool indeterminate{true}, animating{}, bar_style{true}, displayed_when_stopped{true};
    double phase{};
    // NSBox
    Address box_title{};                      // +1
    Address content_view{};                   // NSBox content view (a subview)
    std::int64_t box_type{}, border_type{3}, title_position{2};
    Address fill_color{}, border_color{};     // +1
    double corner_radius{5};
    // NSStepper / NSSlider
    double increment{1};
    bool value_wraps{}, autorepeat{true};
};

// ---- the text system (text.cpp; TextKit objects: kit/completion/controls/text_system.cpp) ----
// NSText / NSTextView. The text is `value` (UTF-16, ranges in units as in
// Cocoa), drawn in one style (font, color); a view with an NSTextStorage
// keeps `value` equal to the storage's string and draws in the attributes
// of its first character.
struct TextState final : ObjectPayload {
    std::u16string value;
    std::size_t selection{}, selection_length{};
    std::optional<std::pair<std::size_t, std::size_t>> marked;   // location, length
    std::size_t anchor{};                                        // selection anchor for extending
    Address delegate{};                                          // not retained
    Address font{}, text_color{}, background_color{};            // +1
    Address client{};                                            // field editor: the edited NSTextField
    Size inset{0, 0};
    Size min_size{0, 0}, max_size{1e7, 1e7};
    double scroll_x{};                                           // field editor: horizontal scroll
    std::int64_t alignment{4};
    bool editable{true}, selectable{true}, rich_text{true}, field_editor{}, draws_background{true};
    bool horizontally_resizable{}, vertically_resizable{true}, secure{};
    Address container{};                                         // +1 NSTextContainer (made on demand)
    // TextKit (text_system.cpp): the storage and layout manager the view
    // shows (+1; made on demand, decoded, or given by
    // -initWithFrame:textContainer:), and NSTextViewSharedData's settings.
    Address storage{}, layout_manager{};
    Address typing_attributes{}, selected_attributes{}, marked_attributes{}, link_attributes{};   // +1 dictionaries
    Address default_paragraph{}, insertion_color{};              // +1
    std::vector<std::pair<std::size_t, std::size_t>> extra_selections;   // -selectedRanges after the first
    std::uint64_t checking_types{};
    std::int64_t inline_prediction{};
    bool syncing{};                                              // the storage and the view are being synchronized
    bool aborting{};                                             // field editor: editing ends by cancellation (no action)
};
[[nodiscard]] inline std::shared_ptr<TextState> text_state(AppKit& kit, Address view) { return kit.extension<TextState>(view); }
// NSTextContainer: the layout area of a text view. Lines wrap at the view's
// width while the container tracks it; a container wider than the text
// (widthTracksTextView NO) leaves lines unwrapped.
struct ContainerState final : ObjectPayload {
    Size size{1e7, 1e7};
    double padding{5};
    std::int64_t line_break{};                                   // NSLineBreakByWordWrapping
    Address text_view{};                                         // not retained
    Address layout_manager{};                                    // not retained
    bool tracks_width{true}, tracks_height{};
};

// NSLayoutManager (TextKit 1): the storage it lays out and the containers
// it lays out into, plus the temporary attributes (display-only attribute
// runs over the storage's characters; NSBackgroundColor is drawn by text.cpp).
struct LayoutManagerState final : ObjectPayload {
    Address storage{};                       // not retained (the storage owns its layout managers)
    std::vector<Address> containers;         // +1 NSTextContainers
    Address delegate{};                      // not retained
    struct Run { std::size_t length{}; Address attributes{}; };   // attributes +1 (0: none)
    std::vector<Run> temporary;              // lengths add up to the storage length
    bool non_contiguous{}, background_layout{true}, shows_invisibles{}, shows_control_characters{};
    std::uint64_t flags{};                   // archived NSLMFlags
};

// The process's AppKit provider: set by AppKit::install_controls, read by the
// QLab control providers (src/alchemy/darwin/kit/completion/controls/) that add AppKit
// classes over the same host state and drawing layer. Null before AppKit
// installed (they fail explicitly then).
[[nodiscard]] std::shared_ptr<AppKit> installed_appkit();

// containers.cpp additions for QLab (installed by kit/completion/controls/tables.cpp):
// NSTableRowView, table row views / row moves / drop targets / accessibility,
// NSOutlineView item-level updates, NSScroller parts, NSScrollView rulers,
// insets and floating subviews.
void install_container_additions(AppKit& kit);

// Dynamic system colours (controlTextColor, labelColor, ...: the QLab
// controls' appearance.cpp) resolve for the appearance a view draws in, as
// AppKit's do for NSAppearance.currentDrawingAppearance. Control drawing
// calls this before reading colours; it does nothing until a resolver is set.
using DynamicColorResolver = void (*)(AppKit& kit, Address view);
void set_dynamic_color_resolver(DynamicColorResolver resolver);
void resolve_dynamic_colors(AppKit& kit, Address view);
// The dynamic system colours themselves (controls/appearance.cpp), installed
// with AppKit's other named colours (install_system_colors).
void install_appearance(AppKit& kit);

// Typed cell extension state (see CellState::extension).
template <class T>
std::shared_ptr<T> cell_extension(AppKit& kit, Address cell) {
    const auto state = kit.objc.host_state<CellState>(cell);
    if (!state->extension) state->extension = std::make_shared<T>();
    auto typed = std::dynamic_pointer_cast<T>(state->extension);
    if (!typed) fail("cell extension state of another type");
    return typed;
}

// ---- helpers shared by the control files (controls.cpp) ----------------------------
// The cell of a control (nil when it has none).
[[nodiscard]] Address control_cell(AppKit& kit, Address control);
[[nodiscard]] std::shared_ptr<CellState> cell_state(AppKit& kit, Address cell);
// Text of a cell's object value (NSString, NSAttributedString, NSNumber ...).
[[nodiscard]] std::u16string cell_text(AppKit& kit, Address cell);
// The text style a cell draws its contents with.
[[nodiscard]] TextStyle cell_text_style(AppKit& kit, Address cell, const Theme& theme);
// Text truncated with an ellipsis to `width` (NSLineBreakByTruncatingTail).
[[nodiscard]] std::u16string truncate_text(const drawing::FontSpec& font, std::u16string_view text, double width);
// Draws one line of text vertically centred in `rect` of the current
// context, aligned by `alignment` (NSTextAlignment), clipped to the rect.
void draw_line_in(AppKit& kit, std::u16string_view text, const TextStyle& style, Rect rect, std::int64_t alignment,
                  bool truncate = true);
// A rounded "control face" in the current context.
void draw_bezel(AppKit& kit, Rect rect, double radius, drawing::Color face, drawing::Color border);
// The pointer-tracking loop of a control: highlights while the pointer is
// inside, returns whether the left button was released inside `view`'s
// bounds. `moved` sees every location (view coordinates) while dragging.
bool track_mouse(AppKit& kit, Address view, Address down_event, const std::function<void(bool inside)>& highlight,
                 const std::function<void(Point)>& moved = {});
// The view-local location of a mouse event.
[[nodiscard]] Point event_location_in(AppKit& kit, Address view, Address event);

} // namespace alchemy::gui::appkit
