#pragma once

// AppKit providers (owner: A4): shared state and helpers of
// src/alchemy/darwin/appkit/*.cpp. Not a contract header: other subsystems use
// AppKit only through Objective-C messages and the exported symbols.
//
// Model. Every AppKit class here is a host class registered with instance
// size 8 (darwin_frameworks.cpp declares most of them); guest subclasses
// (BlenderWindow : NSWindow, CocoaMetalView : NSView, ...) add their ivars
// after the isa, so all host state lives in ObjcRuntime::host_state side
// tables. Methods a guest may override (delegates, subclasses, the responder
// chain) are always invoked with ObjcRuntime::send / send_if_responds.
//
// Threading. AppKit is a main-thread framework. The window server's sink
// callbacks run on the main thread inside dispatch_messages(), which only
// runs inside the event pump of -[NSApplication nextEventMatchingMask:...]
// (or a nested Win32 modal loop). Shared tables are still guarded by one
// recursive mutex because guest helper threads do query NSScreen, NSEvent
// class state and pasteboards.
//
// Services. The window server and run loop are obtained through
// window_server_or_null() / run_loop_or_null(): the process objects of
// window_server.hpp / runloop.hpp when available, or the test overrides.
// Nothing is created at install time (alchemy-fx doctor installs too).

#include "alchemy/darwin/appkit/drawing.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/gui/window_server.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::appkit {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
using Handler = transmute::GuestCallHandler;

// ---- services ------------------------------------------------------------------
// Test seam: a fake window server / run loop used instead of the process ones.
// Pass nullptr to restore the default. Not thread-safe against concurrent
// AppKit calls; tests set it before installing.
void set_window_server_override(WindowServer* server) noexcept;
void set_run_loop_override(RunLoop* loop) noexcept;
[[nodiscard]] WindowServer* window_server_or_null() noexcept;
[[nodiscard]] WindowServer& require_window_server(std::string_view operation);
[[nodiscard]] RunLoop* run_loop_or_null() noexcept;

[[noreturn]] void fail(std::string_view why);
[[noreturn]] void unsupported(std::string_view what);
// Host environment variable ("" when unset).
[[nodiscard]] std::string host_environment(const char* name);
// Diagnostic notes go to stderr only while ALCHEMY_TRACE_OBJC is set (probe
// scripts read the last stderr line as the stop point).
void note_trace(std::string_view text);

// ---- NSAlert presentation (alert.cpp) ----------------------------------------------
// One -[NSAlert runModal]. Buttons are in AppKit order: buttons[0] answers
// NSAlertFirstButtonReturn (1000), buttons[1] 1001, ...
struct AlertRequest {
    std::string title;                        // window caption (the application name)
    std::string message, informative;
    std::vector<std::string> buttons;
    std::uint64_t style{};                    // NSAlertStyle
    double width{};                           // points, from a spacer accessory view (0: default)
    void* owner{};                            // native owner window (nullptr: none)
    // The suppression checkbox (-showsSuppressionButton): its title (empty:
    // none), initial state, and where the presenter writes its final state.
    std::string suppression;
    bool suppression_checked{};
    bool* suppression_result{};
};
// Presents `request` and returns the index of the chosen button.
using AlertPresenter = std::function<std::size_t(const AlertRequest&)>;
// Test seam: replaces the Win32 task dialog; {} restores it.
void set_alert_presenter_override(AlertPresenter presenter);
// The Win32 presenter (TaskDialogIndirect from comctl32 v6). `created` runs
// with the dialog's HWND once it exists (tests click a button from there).
std::size_t run_task_dialog(const AlertRequest& request, const std::function<void(void*)>& created = {});

// ---- open and save panels (panels.cpp) ---------------------------------------------
// One -[NSSavePanel runModal] / -[NSOpenPanel runModal]. Paths are guest paths.
struct FileDialogRequest {
    bool save{};
    std::string title, message, prompt, name_label, directory, name;
    std::vector<std::string> extensions;      // allowed filename extensions (empty: any file)
    bool choose_files{true}, choose_directories{}, multiple{}, shows_hidden{}, allows_other_types{};
    void* owner{};                            // native owner window (nullptr: none)
};
// The chosen paths, or nullopt when the panel was cancelled.
using FileDialogPresenter = std::function<std::optional<std::vector<std::string>>(const FileDialogRequest&)>;
// Test seam: replaces the Win32 common item dialog; {} restores it.
void set_file_dialog_override(FileDialogPresenter presenter);

inline constexpr std::string_view appkit_library = darwin_framework_libraries[0];
inline constexpr std::string_view cocoa_library = darwin_framework_libraries[1];
inline constexpr std::string_view core_services_library = darwin_framework_libraries[6];
inline constexpr std::string_view core_graphics_library = darwin_framework_libraries[7];
inline constexpr std::string_view carbon_library = darwin_framework_libraries[11];
inline constexpr std::string_view libsystem_library = "/usr/lib/libSystem.B.dylib";
inline constexpr Address not_found = static_cast<Address>(INT64_MAX);   // NSNotFound

// NSEventSubtype values used by AppKit itself.
namespace subtype {
inline constexpr std::int64_t mouse = 0, tablet_point = 1, tablet_proximity = 2;
} // namespace subtype

// Window-server text (IME composition, IME results, injected Unicode) reaches
// the guest the way macOS delivers input-method text: inside a key press.
// Each text event becomes a key-down (plus key-up) NSEvent whose
// -interpretKeyEvents: hands the text to the NSTextInputClient
// (-setMarkedText:... for a composition, -insertText:... for committed text)
// instead of the key bindings. Committed text is split into one key press per
// character, carried as the event's characters (applications that read
// -characters, like Blender outside its IME mode, see an ordinary typed
// character); a composition update carries no characters, like a dead key.
// The key code is kVK_ISO_Section, a key with no standard binding or
// meaning of its own.
inline constexpr std::uint16_t text_input_key_code = 0x0a;

// ---- register marshalling (Apple ARM64: NSRect = d0..d3, NSPoint = d0,d1) --------
[[nodiscard]] inline double d(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
[[nodiscard]] inline float f(const CpuState& s, unsigned index) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[index][0]));
}
inline void set_d(CpuState& s, unsigned index, double value) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }
inline void set_f(CpuState& s, unsigned index, float value) { s.v[index] = {std::bit_cast<std::uint32_t>(value), 0}; }
[[nodiscard]] inline Point point_arg(const CpuState& s, unsigned first = 0) { return {d(s, first), d(s, first + 1)}; }
[[nodiscard]] inline Size size_arg(const CpuState& s, unsigned first = 0) { return {d(s, first), d(s, first + 1)}; }
[[nodiscard]] inline Rect rect_arg(const CpuState& s, unsigned first = 0) {
    return {{d(s, first), d(s, first + 1)}, {d(s, first + 2), d(s, first + 3)}};
}
inline void return_point(CpuState& s, Point p) { set_d(s, 0, p.x); set_d(s, 1, p.y); }
inline void return_size(CpuState& s, Size v) { set_d(s, 0, v.width); set_d(s, 1, v.height); }
inline void return_rect(CpuState& s, Rect r) {
    set_d(s, 0, r.origin.x); set_d(s, 1, r.origin.y); set_d(s, 2, r.size.width); set_d(s, 3, r.size.height);
}
inline void return_bool(CpuState& s, bool value) { s.x[0] = value ? 1 : 0; }
[[nodiscard]] inline bool bool_arg(const CpuState& s, unsigned index) { return (s.x[index] & 0xff) != 0; }
// Register images for host -> guest sends.
inline void put_point(CallRegs& r, unsigned first, Point p) { r.set_d(first, p.x); r.set_d(first + 1, p.y); }
inline void put_size(CallRegs& r, unsigned first, Size v) { r.set_d(first, v.width); r.set_d(first + 1, v.height); }
inline void put_rect(CallRegs& r, unsigned first, Rect v) {
    r.set_d(first, v.origin.x); r.set_d(first + 1, v.origin.y); r.set_d(first + 2, v.size.width); r.set_d(first + 3, v.size.height);
}
[[nodiscard]] inline Point result_point(const CallResult& r) { return {r.d(0), r.d(1)}; }
[[nodiscard]] inline Size result_size(const CallResult& r) { return {r.d(0), r.d(1)}; }
[[nodiscard]] inline Rect result_rect(const CallResult& r) { return {{r.d(0), r.d(1)}, {r.d(2), r.d(3)}}; }
[[nodiscard]] inline bool rect_contains(Rect r, Point p) {
    return p.x >= r.origin.x && p.y >= r.origin.y && p.x < r.origin.x + r.size.width && p.y < r.origin.y + r.size.height;
}
[[nodiscard]] inline bool rect_empty(Rect r) { return r.size.width <= 0 || r.size.height <= 0; }

// ---- image files (image_codec.cpp, Windows Imaging Component) --------------------
enum class ImageFileType { tiff = 0, bmp = 1, gif = 2, jpeg = 3, png = 4 };   // NSBitmapImageFileType
struct DecodedImage { Image pixels; };
// Straight RGBA8 of the first frame; nullopt when the data is not an image
// format the host can read.
[[nodiscard]] std::optional<DecodedImage> decode_image(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_image(const Image& pixels, ImageFileType type);

// ---- host state ---------------------------------------------------------------------
// One payload per object: NSApplication, NSWindow and NSView states extend
// the NSResponder state so nextResponder works on all of them.
struct ResponderState : ObjectPayload {
    Address next_responder{};
};

// A window's software backing store (graphics.cpp): the content area in
// physical pixels, BGRA8 premultiplied, rows top to bottom, in guest memory
// so a guest CGContext can draw into it too. `mirror` is the host copy handed
// to WindowServer::present_bitmap.
struct BackingStore {
    Address pixels{};                         // owned (allocate_guest)
    std::uint32_t width{}, height{};
    std::size_t stride{};
    double scale{1};
    Size points;                              // the content size it was made for
    Address cg_context{};                     // +1 CGContextRef over `pixels` (made on demand)
    Address graphics_context{};               // +1 NSGraphicsContext of the window
    Address foreign_context{};                // +1 the Quartz 2D layer's NSGraphicsContext over cg_context (when it owns the class)
    std::vector<std::uint8_t> mirror;
};

struct WindowSink;
struct WindowState final : ResponderState {
    WindowId id{};
    std::shared_ptr<WindowSink> sink;
    Address content_view{}, delegate{}, first_responder{}, initial_first_responder{};
    Address title{}, represented_filename{}, background_color{}, appearance{};   // +1 references
    Address autosave_name{};                  // +1 (frame autosave, window.cpp)
    Address mouse_down_view{}, parent{};
    std::vector<Address> children;
    Rect last_content;                // content rect when the server window is gone
    std::uint64_t style{};
    WindowLevel level{};
    std::uint64_t collection_behavior{}, tabbing_mode{}, title_visibility{};
    std::uint64_t key_view_direction{};       // NSSelectionDirection while the key view loop moves focus
    Size min_content{}, max_content{};
    bool visible{}, key{}, main{}, closed{}, released_when_closed{true}, accepts_mouse_moved{}, opaque{true};
    bool document_edited{}, titlebar_transparent{}, has_shadow{true}, live_resize{}, full_screen{};
    bool miniaturized{}, needs_display{}, accepts_drops{}, hidden_by_application{}, cursor_rects_enabled{true};
    std::vector<std::string> dragged_types;
    // Software drawing (graphics.cpp).
    std::shared_ptr<BackingStore> backing;
    Rect dirty;                               // window base coordinates waiting for display
    bool software{};                          // the window presents its backing store
    Address field_editor{};                   // +1 the shared field editor (controls.cpp)
    Address default_button{};                 // the key-equivalent Return button (not retained)
    Address window_controller{};              // not retained (controllers.cpp)
    // NSPanel (controls_more.cpp).
    bool floating_panel{}, becomes_key_only_if_needed{}, works_when_modal{}, hides_on_deactivate{};
    // Auto Layout engine of the window's view tree (kit/completion/windows/auto_layout.cpp).
    std::shared_ptr<ObjectPayload> layout_engine;
    // Sheets (kit/completion/windows/window_extras.cpp): the attached sheet,
    // the window a sheet is attached to, the sheet's completion handler (+1 block).
    Address attached_sheet{}, sheet_parent{}, sheet_handler{};
    // NSWindow properties kept for their getters (window_extras.cpp).
    Address subtitle{}, identifier{}, content_view_controller{};   // +1
    Size min_full_screen{}, max_full_screen{};
    std::array<double, 4> content_border{};          // by NSRectEdge
    std::array<bool, 4> content_border_automatic{true, true, true, true};
    bool concurrent_drawing{};
    // Occlusion (chrome-s2 w1, window.cpp): the server hid the window
    // (occlusion_changed visible = NO), the state last posted, an update
    // waiting for the run loop.
    bool server_hidden{}, occlusion_posted{}, occlusion_pending{};
    Address frame_view{};   // +1 the frame view, the content view's superview (frame_view.cpp)
    // -aspectRatio (frame) and -contentAspectRatio ({0, 0}: none; the content
    // ratio wins), enforced while the user resizes (will_resize).
    Size aspect_ratio{}, content_aspect_ratio{};
    bool can_hide{true};                      // -[NSApplication hide:] leaves the window when NO
    std::int64_t toolbar_style{};             // NSWindowToolbarStyle (kept; the toolbar draws one style)
    // Toolbars and tabs (window_tabs.cpp): the toolbar (+1); the views of the
    // top strip between the title bar and the content view (frame-view
    // subviews, held by the frame view) and the strip's height; the tab
    // group (+1, shared by its windows), the window's NSWindowTab (+1) and
    // tabbing identifier (+1). -sharingType (NSWindowSharingType).
    Address toolbar{}, toolbar_view{}, tab_bar_view{}, tab_group{}, tab{}, tabbing_identifier{};
    double strip{};
    std::uint64_t sharing_type{1};
};

struct CursorRect { Rect rect; Address cursor{}; };   // view (bounds) coordinates; cursor +1
struct ViewState final : ResponderState {
    Rect frame;
    // Bounds transform: bounds = {bounds_origin, frame.size / bounds_scale};
    // the scale is kept across frame size changes, as in AppKit.
    Point bounds_origin;
    Size bounds_scale{1, 1};
    std::vector<CursorRect> cursor_rects;
    Address window{}, superview{};
    Address next_key_view{}, previous_key_view{};   // weak (the key view loop)
    std::vector<Address> subviews;            // +1 references
    Address layer{}, input_context{};         // +1 references
    std::vector<Address> tracking_areas;      // +1 references
    std::vector<Address> inside_areas;        // tracking areas the pointer is in
    std::vector<std::string> dragged_types;
    std::uint64_t autoresizing_mask{}, allowed_touch_types{};
    bool wants_layer{}, backing_layer{}, needs_display{}, hidden{}, posts_frame_changes{true}, posts_bounds_changes{true};
    bool autoresizes_subviews{true}, best_resolution_gl{};
    Rect dirty;                               // -setNeedsDisplayInRect: union (bounds coordinates)
    Address focus_previous{};                 // -lockFocus: the context to restore (+1)
    bool focus_locked{};
    std::int64_t tag{-1};                     // NSView -tag (NSControl keeps its own)
    Address identifier{};                     // +1 NSUserInterfaceItemIdentification
    std::vector<Address> gesture_recognizers; // +1 (controls_more.cpp)
    // Auto Layout (kit/completion/windows/auto_layout.cpp).
    std::vector<Address> constraints;         // +1 the NSLayoutConstraints this view holds (-constraints)
    bool translates_autoresizing{true};       // -translatesAutoresizingMaskIntoConstraints
    std::array<float, 2> hugging{250, 250}, compression{750, 750};   // by NSLayoutConstraintOrientation
    bool needs_layout{}, needs_update_constraints{true};
    Address shadow{};                         // +1 -[NSView shadow] (kept; the compositor draws no view shadows)
    bool clips_to_bounds{};
    // Tool-tip and tracking rectangles (kit/completion/windows/window_extras.cpp).
    struct ToolTipRect { std::int64_t tag{}; Rect rect; Address owner{}, user_data{}; };
    std::vector<ToolTipRect> tool_tips;
    Address tool_tip{};                       // +1 -toolTip
    Address appearance{};                     // +1 -[NSView appearance] (nil: inherited)
    Address accessibility_overrides{};        // +1 NSMutableDictionary (-accessibilitySetOverrideValue:forAttribute:)
    std::map<std::int64_t, Address> tracking_rects;   // tag -> NSTrackingArea (held by tracking_areas)
    bool draws_concurrently{};
    std::int64_t layout_direction{};          // NSUserInterfaceLayoutDirection (0 left to right)
    // State of AppKit subclasses (controls, scroll views, text views...):
    // one object carries one payload, so subclasses extend the view this way.
    std::shared_ptr<ObjectPayload> extension;
};

struct ApplicationState final : ResponderState {
    Address delegate{}, main_menu{}, windows_menu{}, services_menu{}, help_menu{};   // menus +1
    Address icon_image{}, appearance{};       // +1
    std::int64_t activation_policy{}, presentation_options{};
    std::int64_t attention_requests{};
    bool launched{}, running{}, active{}, hidden{}, stop_requested{};
};

// NSWindowController / NSViewController (controllers.cpp).
struct ControllerState final : ResponderState {
    Address window{}, view{};                 // +1
    Address nib_name{}, nib_bundle{}, title{}, represented{}, autosave_name{};   // +1
    Address identifier{}, content_view_controller{};   // +1
    Address owner{}, document{}, parent{};    // not retained
    std::vector<Address> children;            // +1 child view controllers
    std::vector<Address> top_level;           // +1 top-level objects of the loaded nib
    Size preferred_content_size;
    bool should_cascade{true}, loading{}, should_close_document{};
};

struct EventState final : ObjectPayload {
    Event event;
    Address window{};                         // the NSWindow (not retained, like AppKit's weak window)
    Address characters{}, characters_ignoring{}, tracking_area{};   // +1 caches / references
    std::int64_t subtype{}, data1{}, data2{};
    std::uint64_t event_number{};
    // A key press carrying window-server text (see text_input_key_code):
    // event.text / event.marked / event.selection_* are what
    // -interpretKeyEvents: delivers.
    bool text_input{};
    // +enterExitEventWithType:...: the tracking number and user data (events
    // of an NSTrackingArea answer from tracking_area instead).
    std::int64_t tracking_number{};
    Address user_data{};
    Address cg_event{};                       // +1, -CGEvent's CGEventRef (made on first use)
};

struct MenuState final : ObjectPayload {
    Address title{}, supermenu{};
    std::vector<Address> items;               // +1
    bool autoenables{true};
    Address delegate{};                       // weak (NSMenuDelegate)
    Address font{};                           // +1
    double minimum_width{};
    bool shows_state_column{true}, allows_context_menu_plugins{true};
};
struct MenuItemState final : ObjectPayload {
    Address title{}, key_equivalent{}, submenu{}, represented_object{};   // +1
    Address tool_tip{}, image{}, attributed_title{}, view{};              // +1
    Address action{}, target{}, menu{};
    std::int64_t indentation{};
    bool alternate{};
    std::uint64_t modifier_mask{1ULL << 20};  // NSEventModifierFlagCommand
    std::int64_t tag{}, state{};
    bool enabled{true}, separator{}, hidden{};
};

struct PasteboardState final : ObjectPayload {
    std::string name;
    bool general{}, drag{};
    std::int64_t change_count{};
    std::vector<std::string> declared;        // types declared by the owner since clearContents
    std::vector<std::string> file_paths;      // guest paths (drag pasteboard)
    std::map<std::string, std::string> strings;   // private pasteboards
};

struct CursorState final : ObjectPayload {
    std::optional<SystemCursor> system;
    Address image{};                          // +1 (custom cursors)
    Point hot_spot;
    CursorId id{};                            // window-server cursor, created on first set
};

struct BitmapState final : ObjectPayload {
    std::int64_t width{}, height{}, bits_per_sample{8}, samples_per_pixel{4}, bytes_per_row{}, bits_per_pixel{32};
    std::uint64_t format{};                   // NSBitmapFormat
    bool has_alpha{true}, planar{}, owns{};
    Address data{};                           // guest pixels (owned when `owns`)
    Address color_space_name{};               // +1
    Size size;                                // points
    std::optional<bool> opaque;               // -setOpaque: (unset: opaque when there is no alpha)
};
struct ImageState final : ObjectPayload {
    Size size;
    std::vector<Address> representations;     // +1
    Address name{};                           // +1
    bool template_image{};
    Address background_color{}, accessibility_description{};   // +1
    std::array<double, 4> cap_insets{};       // NSEdgeInsets: top, left, bottom, right
    std::int64_t resizing_mode{};             // NSImageResizingModeTile (0) or Stretch (1)
};
struct ColorState final : ObjectPayload { double red{}, green{}, blue{}, alpha{1}; bool white{}; };
struct ColorSpaceState final : ObjectPayload { std::string name; std::int64_t model{1}; };   // NSColorSpaceModelRGB
struct AppearanceState final : ObjectPayload { Address name{}; };   // +1
// NSBezierPath: `path` always; a path made by +bezierPathWithRect: /
// ...RoundedRect: / ...OvalInRect: alone also keeps its shape in rect/radius
// (the -[NSImage lockFocus] renderer handles those shapes only).
struct BezierState final : ObjectPayload {
    Rect rect; double radius_x{}, radius_y{};
    drawing::Path path;
    bool general{};                           // anything else was added
    double line_width{1};
    std::uint64_t winding{};                  // NSWindingRule: 0 non-zero, 1 even-odd
};
struct AlertState final : ObjectPayload {
    std::string message, informative;
    std::vector<std::string> buttons;
    std::vector<Address> button_objects;      // +1 the NSButtons of -buttons (their titles win at -runModal)
    std::uint64_t style{};
    Address accessory{};                      // +1
    Address icon{};                           // +1 (nil: the application icon)
    Address suppression_button{};             // +1 (made on demand)
    bool shows_help{}, shows_suppression{};
};
struct ScreenState final : ObjectPayload { ScreenInfo info; };
struct TextInputState final : ObjectPayload { Address client{}; bool active{}; };
struct TrackingAreaState final : ObjectPayload {
    Rect rect;
    std::uint64_t options{};
    Address owner{}, user_info{};             // user_info +1
    Address view{};
};
struct DraggingState final : ObjectPayload {
    Address pasteboard{}, window{};           // pasteboard +1
    Point location;                           // window base coordinates
    std::uint64_t source_mask{};              // NSDragOperation mask the source allows
    std::int64_t sequence{};
    std::int64_t valid_items{-1};             // -setNumberOfValidItemsForDrop: (-1: every item)
    Address source{};                         // the dragging source in this application (weak; 0: another process)
};

// One file drag over one window (window-server drag_* notifications): the
// NSDraggingInfo handed to every NSDraggingDestination message of the drag
// and the destination (the deepest registered view under the pointer, else
// the window) currently receiving them.
struct DragSession {
    Address info{};                           // +1 _AlchemyDraggingInfo
    Address destination{};                    // +1 view or window (0: none under the pointer)
    std::uint64_t answer{};                   // the destination's last NSDragOperation answer
};
struct InputSourceState final : ObjectPayload {
    KeyboardLayout layout;
    Address layout_data{}, identifier{}, name{}, languages{};   // +1 CF objects, released by -dealloc
};
struct MonitorState final : ObjectPayload { std::uint64_t mask{}; Address block{}; bool local{}; };

// ---- software drawing (graphics.cpp) -----------------------------------------------
// One NSGraphicsContext: a canvas over a window backing store or a bitmap
// image rep, or a wrapper of a guest CGContextRef (no host canvas then).
struct GraphicsContextState final : ObjectPayload {
    std::shared_ptr<drawing::PixelBuffer> buffer;
    std::unique_ptr<drawing::Canvas> canvas;
    std::shared_ptr<BackingStore> backing;    // window contexts
    drawing::Affine base;                     // window base points -> device pixels (window contexts)
    Address window{}, bitmap{};               // bitmap +1
    Address cg_context{};                     // the CGContextRef -CGContext answers (+1 when owned)
    bool owns_cg{};
    unsigned cg_saved{};                      // CGContextSaveGState calls to balance
    bool flipped{};
    bool in_view{};                           // drawing a view (display pass or -lockFocus)
    std::uint64_t image_interpolation{}, compositing{2};
};
struct FontState final : ObjectPayload { drawing::FontSpec spec; };
// NSStringDrawing attributes of one string: the attribute dictionary read
// once (font, foreground color, paragraph alignment / line break).
struct TextStyle {
    drawing::FontSpec font;
    drawing::Color color{0, 0, 0, 1};
    std::optional<drawing::Color> background;
    std::int64_t alignment{};                 // NSTextAlignment (0 left, 1 right, 2 center, 3 justified, 4 natural)
    std::int64_t line_break{};                // NSLineBreakMode
    bool underline{};
};

// The flat theme of AppKit's own control drawing (light / dark aqua).
struct Theme {
    bool dark{};
    drawing::Color window_background, control_background, control_face, control_face_pressed, control_border,
        text, secondary_text, disabled_text, accent, accent_text, selection, unfocused_selection, text_background,
        separator, scroller_track, scroller_knob, header_background, grid, alternate_row, focus_ring;
};
[[nodiscard]] const Theme& theme(bool dark);

// ---- the AppKit provider ---------------------------------------------------------------
struct AppKit : std::enable_shared_from_this<AppKit> {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;

    // Classes (host classes, the guest's superclasses).
    Address object_class{}, responder{}, application{}, window{}, view{}, event{}, menu{}, menu_item{};
    Address pasteboard{}, cursor{}, image{}, image_rep{}, bitmap_rep{}, color{}, color_space{}, appearance{};
    Address color_sampler{}, alert{}, workspace{}, document_controller{}, screen{}, text_input_context{};
    Address tracking_area{}, dragging_info{}, bezier_path{}, input_source{}, event_monitor{};
    Address ns_app_slot{};                    // the unified _NSApp cell
    // Software drawing and controls (graphics.cpp, controls.cpp, containers.cpp, text.cpp, nib.cpp).
    Address graphics_context{}, font{}, font_manager{}, control{}, cell{}, action_cell{}, button{}, button_cell{};
    Address text_field{}, text_field_cell{}, secure_text_field{}, secure_text_field_cell{}, progress_indicator{};
    Address image_view{}, image_cell{}, box{}, popup_button{}, popup_button_cell{}, scroll_view{}, clip_view{}, scroller{};
    Address tab_view{}, tab_view_item{}, split_view{}, table_view{}, table_column{}, table_header_view{}, outline_view{};
    Address table_cell_view{}, table_row_view{}, text_class{}, text_view{}, menu_window{}, menu_view{}, view_controller{};
    Address nib{}, nib_coder{}, custom_object{}, custom_view{}, window_template{}, nib_connector{}, outlet_connector{};
    Address control_connector{}, class_swapper{}, nib_object_data{}, stepper{}, slider{}, color_well{};
    Address slider_cell{}, stepper_cell{}, combo_box{}, combo_box_cell{}, panel{}, toolbar{}, toolbar_item{};
    Address gesture_recognizer{}, animation{}, view_animation{}, pasteboard_item{}, dragging_item{}, print_info{};
    Address save_panel{}, open_panel{}, window_controller{};
    Address view_draw_rect_imp{};             // -[NSView drawRect:] (views that do not override it draw nothing)
    Address current_context{};                // +1 AppKit's current context (its own drawing; see foreign_graphics)
    std::vector<Address> context_stack;       // +saveGraphicsState (+1 each)
    // Coexistence with the Quartz 2D layer (src/alchemy/quartz2d), which
    // installs after AppKit and replaces NSGraphicsContext / NSFont /
    // NSBezierPath / NSColor -set / NSString and NSImage drawing with
    // CoreGraphics-backed versions. AppKit then keeps drawing its controls
    // with its own context (current_context) and, around every view it
    // draws, makes the Quartz layer's NSGraphicsContext over the backing
    // store's CGContext current for the guest; AppKit drawing requested
    // inside a foreign context (a control drawn by -cacheDisplayInRect:)
    // rasterizes into that CGBitmapContext's pixels (canvas_over_cg).
    struct ForeignFocus { bool active{}; Address previous{}; };   // previous: +1
    std::vector<ForeignFocus> foreign_focus;
    struct CgCanvas {
        Address cg{}, data{};
        std::uint32_t width{}, height{};
        std::size_t stride{};
        bool rgba{};
        std::shared_ptr<drawing::PixelBuffer> buffer;
        std::unique_ptr<drawing::Canvas> canvas;
    };
    std::vector<std::unique_ptr<CgCanvas>> cg_canvases;
    unsigned draw_depth{};                    // > 0 inside a window display pass
    Address tracking_menu{};                  // the menu a popup tracking loop shows

    std::recursive_mutex mutex;
    Address app{};                            // NSApp (immortal)
    std::map<WindowId, Address> windows;      // live server windows -> NSWindow
    std::vector<Address> window_order;        // NSApp windows (front to back, approximate z-order)
    std::deque<Address> queue;                // pending NSEvents (+1)
    Address current_event{};                  // +1
    Address key_window{}, main_window{};
    // Launch activation: a launching application becomes active (and the
    // window it ordered front becomes key) once it runs its event loop, as
    // with the macOS window server's asynchronous activation. From
    // -finishLaunching to the first event-loop pump a window that becomes key
    // is recorded here and made key (-becomeKeyWindow, windowDidBecomeKey:)
    // by that pump, after the application finished setting it up (Blender
    // sets the window delegate after -makeKeyAndOrderFront:).
    bool launch_activation_pending{};
    Address deferred_key_window{};
    std::map<ScreenId, Address> screens;      // NSScreen cache (+1)
    std::map<std::string, Address> pasteboards;   // named NSPasteboards (immortal)
    std::map<int, Address> system_cursors;    // NSCursor singletons (immortal)
    std::map<std::string, Address> appearances, color_spaces;   // immortal singletons
    std::map<std::string, Address> cg_color_spaces;             // +1 CGColorSpaceRefs
    std::uint64_t unique_pasteboards{};
    std::vector<Address> cursor_stack;        // +1 (-[NSCursor push] entries; 0: none was set)
    Address current_cursor{};                 // +1
    Address active_input_context{};           // +1 the NSTextInputContext update_text_input activated
    Address cursor_rect_cursor{};             // the cursor a cursor rectangle set (+1)
    std::vector<Address> monitors;            // +1 monitor objects
    std::vector<Address> recent_documents;    // +1 NSURLs
    Address shared_workspace{}, shared_document_controller{};
    Address workspace_center{};               // -[NSWorkspace notificationCenter] (immortal)
    std::uint64_t event_number{};
    std::int64_t drag_sequence{};
    std::map<WindowId, std::shared_ptr<DragSession>> drags;   // file drags in progress, by window
    // > 0 while -[NSAlert runModal] runs an application-modal session: input
    // for the application's windows is discarded and drags are refused.
    unsigned modal_depth{};
    // The window and characters of the last window-server event when it was
    // a key press with characters (a WM_CHAR repeating it is dropped).
    std::optional<std::pair<WindowId, std::string>> last_keystroke;
    unsigned pump_depth{};
    std::uint64_t pump_mask{};
    unsigned display_depth{};
    WindowId dispatching{};                   // window whose sink callback is running
    Address ordering_window{};                // chrome-s2 w1: window AppKit is ordering (its occlusion update waits)
    std::vector<WindowId> pending_destroy;
    std::shared_ptr<WindowEventSink> application_sink;   // window-less events (activation, quit, screens)
    bool coalesce_mouse{true}, cursor_hidden_until_moved{}, allows_tabbing{true};

    AppKit(ProcessContext& p, ObjcRuntime& o, CoreFoundation& c) : process(p), objc(o), cf(c) {}

    // Extension hooks of providers installed after AppKit (QLab gap group
    // G4, kit/completion/windows/): the Auto Layout pass of a window before it
    // is displayed, and frame changes made by anyone but that pass.
    std::function<void(Address window)> layout_window_hook;
    std::function<void(Address view)> frame_changed_hook;
    // A view was added to a superview (old_parent 0) or is being removed
    // from `old_parent` (still in its window).
    std::function<void(Address view, Address old_parent)> hierarchy_changed_hook;
    // A mouse button went down in `window` (before -sendEvent:): transient
    // popovers close on clicks outside them.
    std::function<void(Address window)> mouse_down_hook;
    std::function<void()> resign_active_hook;   // the application is resigning active

    // Installation (one per subsystem file).
    void install();
    void install_responder();
    void install_application();
    void install_window();
    void install_window_autosave();
    void install_window_geometry();           // window.cpp: constraints, cascading, alignment, aspect ratios, ordering
    std::string saved_frame_string(Address window_object);                  // AppKit's frame string
    bool set_frame_from_string(Address window_object, std::string_view text);
    void autosave_frame(Address window_object);                             // when it has an autosave name
    void install_view();
    void install_event();
    void install_resources();
    void install_alert();
    void install_carbon();
    void install_graphics();
    void install_bezier_paths();
    void install_system_colors();
    void install_controls();
    void install_buttons();
    void install_text_fields();
    void install_simple_views();
    void install_popups();
    // NSSlider, NSStepper, NSComboBox (controls_more.cpp).
    void install_more_controls();
    // NSPanel, NSToolbar, NSSavePanel / NSOpenPanel, NSPrintInfo and the
    // print / color / font panels, gesture recognizers, NSPasteboardItem,
    // NSDraggingItem, NSAnimation / NSViewAnimation, NSLayoutManager
    // (panels.cpp).
    void install_panels();
    // HIShape regions, HITheme drawing and Carbon key modifiers (hitoolbox.cpp).
    void install_hitoolbox();
    // NSWindowController / NSViewController (controllers.cpp).
    void install_controllers();
    void install_documents();                 // documents.cpp: NSDocument / NSDocumentController
    void install_container_coding();          // containers.cpp: -initWithCoder: of scroll/clip/table views
    void install_user_defaults_controller();
    // A popup menu window over `anchor` (view coordinates of `anchor_view`)
    // until an item is chosen (its index) or the menu is dismissed (-1).
    // Tracks a popup menu; the menu's delegate hears -menuNeedsUpdate: and
    // -menuWillOpen: first and -menuDidClose: after.
    std::int64_t popup_menu_tracking(Address menu_object, Address anchor_view, Rect anchor, std::int64_t initial);
    std::int64_t popup_menu_rows_tracking(Address menu_object, Address anchor_view, Rect anchor, std::int64_t initial);
    // NSTextField editing through the window's field editor (text.cpp).
    Address field_editor_for(Address window_object);
    void begin_field_editing(Address text_field_object);
    void end_field_editing(Address text_field_object, bool send_action_too);
    Address editing_field_editor(Address text_field_object);   // 0 when not editing
    void install_containers();
    void install_index_sets();
    void install_tabs_and_splits();
    void install_tables(Address header_cell_class);
    void install_text();
    void install_nib();
    void install_nib_coding();
    // Instantiates a compiled nib (nib.cpp) with `owner` as the File's
    // Owner; returns the top-level objects (each keeps the load's +1).
    std::vector<Address> load_nib(std::span<const std::uint8_t> bytes, Address owner);
    // The bytes of nib `name` in `bundle` (nil: the main bundle).
    std::optional<std::vector<std::uint8_t>> read_nib_file(Address bundle, std::string_view name);

    // ---- software drawing (graphics.cpp) ------------------------------------------------
    // Registers a method and returns its IMP.
    Address method_imp(Address cls, std::string name, Handler handler, std::string types);
    // Typed view extension state (see ViewState::extension).
    template <class T>
    std::shared_ptr<T> extension(Address object) {
        const auto state = view_state(object);
        if (!state->extension) state->extension = std::make_shared<T>();
        auto typed = std::dynamic_pointer_cast<T>(state->extension);
        if (!typed) fail("view extension state of another type");
        return typed;
    }
    template <class T>
    std::shared_ptr<T> find_extension(Address object) {
        const auto state = objc.find_host_state<ViewState>(object);
        return state ? std::dynamic_pointer_cast<T>(state->extension) : nullptr;
    }
    // The window display pass: backing-store windows draw every dirty view
    // into the backing store and present it; windows of layer-hosting views
    // only (Blender's CAMetalLayer view) keep the plain -drawRect: pass.
    void display_window(Address window, bool force = false);
    bool window_uses_backing(Address window);
    bool layer_hosting(Address view);
    bool view_draws(Address view);
    std::shared_ptr<BackingStore> ensure_backing(Address window);
    void release_backing(WindowState& state);
    void present_window(Address window, Rect window_rect);
    // Diagnostic capture and autopress (autopress.cpp; ALCHEMY_CONTENTS_CAPTURE,
    // ALCHEMY_GUI_AUTOPRESS).
    struct Autopress;
    std::shared_ptr<Autopress> autopress;
    std::map<WindowId, double> last_present_time;
    std::map<WindowId, std::uint64_t> present_count;
    std::map<WindowId, std::pair<std::uint64_t, double>> capture_throttle;   // frames, last capture time
    std::set<Address> pending_capture;
    Address drawing_view{};                   // the view whose -drawRect: runs (display pass)
    std::map<Address, std::vector<std::u16string>> drawn_labels;   // view -> text its last -drawRect: drew
    void note_drawn_text(std::u16string_view text);
    void autopress_click_view(Address window, Address view);
    // NSApplication modal sessions (modal.cpp).
    struct ModalSession;
    std::vector<std::shared_ptr<ModalSession>> modal_sessions;
    std::map<Address, std::shared_ptr<ModalSession>> modal_handles;
    std::uint64_t modal_session_serial{0x4d0000};
    Address modal_target() const;
    bool input_allowed_for(Address window);
    std::shared_ptr<ModalSession> begin_modal(Address window);
    void end_modal(const std::shared_ptr<ModalSession>& session);
    void stop_modal(std::int64_t code);
    void install_modal();
    Address current_appearance_override{};    // +1, +[NSAppearance setCurrentAppearance:]
    void capture_backing(Address window, const std::string& stem);
    void capture_presented(Address window);
    void start_autopress();
    void autopress_tick();
    // Marks part of a view (bounds coordinates; nullopt: all) for redisplay.
    void invalidate_view(Address view, std::optional<Rect> rect = std::nullopt);
    // View geometry in window base coordinates.
    drawing::Affine view_transform(Address view);          // view -> window base points
    Rect view_rect_in_window(Address view, Rect rect);
    Rect window_rect_in_view(Address view, Rect rect);
    Rect visible_rect(Address view);                       // bounds coordinates, clipped by ancestors
    // NSGraphicsContext.
    Address make_context(std::shared_ptr<drawing::PixelBuffer> buffer, bool flipped);   // +1
    Address window_context(Address window);                 // the window's context (not retained)
    Address bitmap_context(Address bitmap_rep);             // +1 context drawing into an 8-bit RGB(A) NSBitmapImageRep
    void set_current_context(Address context);
    std::shared_ptr<GraphicsContextState> context_state(Address context);
    drawing::Canvas* current_canvas();
    drawing::Canvas& require_canvas(std::string_view operation);
    // Points the window context at `view` (ctm and clip) and makes it current;
    // returns the previously current context (+1) for end_view_drawing.
    Address begin_view_drawing(Address view, Rect window_clip);
    void end_view_drawing(Address previous);
    void apply_cg_state(GraphicsContextState& state);       // CGContext save + concat + clip
    void restore_cg_state(GraphicsContextState& state);
    // The CGContextRef of an AppKit NSGraphicsContext (made over the
    // backing store on demand; the view's CTM and clip applied in a view).
    Address context_cg(Address context);
    // Whether another provider answers +[NSGraphicsContext currentContext] /
    // owns NSFont (the Quartz 2D layer).
    bool foreign_graphics();
    bool foreign_fonts();
    // A canvas over a CGBitmapContext's pixels with its CTM and clip (32-bit
    // premultiplied BGRA / RGBA layouts only; nullptr otherwise).
    drawing::Canvas* canvas_over_cg(Address cg);
    // Whether the current context (AppKit's, else the Quartz layer's) is flipped.
    bool current_flipped();
    // Values.
    drawing::Color color_value(Address color, drawing::Color fallback = {});
    drawing::FontSpec font_spec(Address font_object);       // system font for nil
    Address font_object(const drawing::FontSpec& spec);     // autoreleased NSFont
    TextStyle text_style(Address attributes, const TextStyle& defaults);
    bool dark_appearance(Address view_or_window);
    const Theme& theme_for(Address view);
    // String drawing in the current context (NSStringDrawing): one or more
    // lines in `rect` (view coordinates of the current context), honouring
    // the context's flippedness. Returns the used size.
    Size draw_string(std::u16string_view text, const TextStyle& style, Rect rect, bool wrap);
    Size measure_string(std::u16string_view text, const TextStyle& style, double width);
    std::u16string string16(Address string_object);
    // Draws `image` (an NSImage) into `rect` of the current context.
    void draw_image_in(Address image_object, Rect rect, double alpha, bool respect_flipped);

    // ---- runtime plumbing --------------------------------------------------------------
    GuestMemory& memory() { return process.calling_engine(); }
    Address sel(std::string_view name) { return objc.selector(name); }
    // The registration helpers take their caller's location, which the
    // registration audit reports as the owner (include/alchemy/registration_audit.hpp).
    void method(Address cls, std::string name, Handler handler, std::string types = "@@:", bool class_method = false,
                const std::source_location& where = std::source_location::current());
    void class_method(Address cls, std::string name, Handler handler, std::string types = "@@:",
                      const std::source_location& where = std::source_location::current()) {
        method(cls, std::move(name), std::move(handler), std::move(types), true, where);
    }
    void function(std::string_view library, std::string symbol, Handler handler,
                  const std::source_location& where = std::source_location::current());
    Address declare_class(std::string_view name, std::string_view superclass);
    void string_constant(std::string symbol, std::string_view value,
                         const std::source_location& where = std::source_location::current());
    // A data export of `object` (immortal) under AppKit and Cocoa.
    void object_constant(std::string symbol, Address object,
                         const std::source_location& where = std::source_location::current());

    CallResult send(Address receiver, std::string_view selector, CallRegs registers = {}) {
        return objc.send(receiver, sel(selector), std::move(registers));
    }
    Address send_id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {});
    bool send_bool(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        return (send_id(receiver, selector, arguments) & 0xff) != 0;
    }
    std::optional<CallResult> send_optional(Address receiver, std::string_view selector, CallRegs registers = {}) {
        return objc.send_if_responds(receiver, sel(selector), std::move(registers));
    }
    bool responds(Address receiver, std::string_view selector) { return objc.responds(receiver, sel(selector)); }
    Address retain(Address object) { return object ? send(object, "retain").x0 : 0; }
    void release(Address object) { if (object) static_cast<void>(send(object, "release")); }
    Address autorelease(Address object) { return object ? send(object, "autorelease").x0 : 0; }
    // Replaces a +1 slot with a retained `value`.
    void assign(Address& slot, Address value) { const auto old = slot; slot = retain(value); release(old); }
    Address alloc_init(Address cls) { return send(send(cls, "alloc").x0, "init").x0; }
    bool is_kind(Address object, Address cls);
    Address superclass_of(Address cls) { return memory().read64(cls + 8); }
    Address guest_call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments);
    Address invoke_block(Address block, std::initializer_list<Address> arguments);
    void post_block_exit(std::uint32_t status);   // guest exit()

    // Strings, numbers, arrays.
    Address string(std::string_view utf8) { return cf.string(utf8); }   // +1
    Address auto_string(std::string_view utf8) { return autorelease(cf.string(utf8)); }
    std::string text(Address object);          // NSString -> UTF-8 ("" for nil)
    std::vector<Address> items(Address array); // NSArray elements via count/objectAtIndex:
    Address auto_array(const std::vector<Address>& values) { return autorelease(cf.array(values)); }

    // Notifications: NSNotificationCenter post plus the delegate method.
    Address notification(std::string_view name, Address object, Address user_info = 0);   // autoreleased
    void post(std::string_view name, Address object, Address delegate = 0, std::string_view delegate_selector = {},
              Address user_info = 0);

    // ---- responder ---------------------------------------------------------------------
    std::shared_ptr<ResponderState> responder_state(Address object);
    std::shared_ptr<WindowState> window_state(Address object);
    std::shared_ptr<ViewState> view_state(Address object);
    std::shared_ptr<ApplicationState> app_state();
    void forward(Address responder, std::string_view selector, Address event);   // NSResponder default

    // ---- application (application.cpp) --------------------------------------------------
    Address shared_application(Address cls);
    void finish_launching();
    void complete_launch_activation();        // the first event-loop pump after -finishLaunching
    Address next_event(std::uint64_t mask, double deadline, std::string_view mode, bool dequeue);
    void pump(std::string_view mode, double timeout);
    Address take_event(std::uint64_t mask, bool dequeue);
    void enqueue(Address event, bool at_start = false);
    void dispatch_event(Address event);        // -[NSApplication sendEvent:]
    void set_key_window(Address window, bool key);
    void set_active(bool active);
    void set_application_delegate(Address delegate);   // -[NSApplication setDelegate:]
    // A delegate's notification method, observed through the default center.
    struct DelegateNotification { std::string_view name, selector; };
    // Moves `object`'s delegate observations from `previous` to `delegate`
    // for each table entry the delegate implements (application.cpp). AppKit
    // posts those notifications without calling the delegate directly.
    void observe_delegate(Address object, Address previous, Address delegate, std::span<const DelegateNotification> table);
    static const DelegateNotification window_delegate_notifications[25];   // chrome-s2 w1: NSWindowDelegate (window.cpp)
    void terminate(Address sender);
    void display_pending();
    bool perform_key_equivalent(Address menu, Address event);
    bool send_action(Address action, Address target, Address sender);
    Address target_for_action(Address action, Address target);
    void deliver_text(Address client, const Event& text);

    // ---- windows (window.cpp) ----------------------------------------------------------
    Address window_for_id(WindowId id);
    void handle_window_event(WindowId id, const Event& event);
    Size will_resize(WindowId id, Size proposed);
    std::optional<Rect> caret_rect(WindowId id);
    Rect content_rect(Address window);
    double backing_scale(Address window);
    void close_window(Address window);
    void destroy_server_window(WindowState& state);
    void window_send_event(Address window, Address event);
    bool make_first_responder(Address window, Address responder);
    void update_text_input(Address window);
    void window_did_resize(Address window, const std::shared_ptr<WindowState>& state, Rect content);
    void window_order_front(Address window, bool make_key);
    void remove_from_order(Address window);
    void bring_to_front(Address window);
    // ---- ordering and occlusion (chrome-s2 w1, window.cpp) ----
    // -[NSWindow orderWindow:relativeTo:], the primitive every ordering
    // method sends (NSWindowOrderingMode `place`; `relative` 0 or a window
    // number).
    void order_window(Address window, std::int64_t place, WindowId relative);
    // NSWindowOcclusionStateVisible: ordered in, not miniaturized, and not
    // hidden by the window server (a cloaked window counts as visible).
    bool occlusion_visible(const WindowState& state) const;
    // Posts NSWindowDidChangeOcclusionStateNotification when the state
    // differs from the one last posted; scheduled for the next run-loop turn
    // (AppKit posts it after the window server reports), at once without a
    // run loop.
    void update_occlusion(Address window);
    void schedule_occlusion_update(Address window);
    // ---- the frame view (chrome-s2 w1, frame_view.cpp) ----
    Address frame_view_class_root{};          // NSFrameView
    void install_frame_views();
    // Window toolbars and tabs (window_tabs.cpp). The window server's content
    // rect is the AppKit content rect plus the top strip (toolbar, tab bar).
    void install_window_tabs();
    [[nodiscard]] Rect to_server_content(Address window, Rect content);
    [[nodiscard]] Rect from_server_content(Address window, Rect content);
    void update_top_strip(Address window);    // after the toolbar or the tab bar changed
    void layout_top_strip(Address window);    // frame-view layout (frame_view.cpp)
    [[nodiscard]] std::vector<Address> top_strip_views(Address window);
    // Tabs follow ordering: a tabbed window ordered in becomes its group's
    // selected window; one ordered out or closed leaves its group.
    void tab_window_ordered_in(Address window);
    void tab_window_leaving(Address window);
    void release_window_tabs(WindowState& state);
    void toolbar_changed(Address toolbar);   // items, visibility or display mode (panels.cpp)
    bool switching_tabs{};
    // Creates the frame view (+frameViewClassForStyleMask:), or replaces it
    // when the class for the window's style changed, and lays it out.
    void update_frame_view(Address window);
    void layout_frame_view(Address window);
    // The content view becomes / stops being the frame view's subview.
    void attach_to_frame_view(Address window, Address child);
    void detach_from_frame_view(Address window, Address child);
    double titlebar_height(Address window);   // -_titlebarHeight of a titled window's frame view, else 0
    Address standard_window_button(Address window, std::uint64_t which);
    // NSDraggingDestination: drag_entered / drag_updated / drag_exited /
    // files_dropped of `window`'s server window.
    void handle_drag(Address window, const Event& event);
    // WindowEventSink::drag_operation: the current destination's answer.
    std::uint64_t drag_answer(WindowId id);
    // Forgets the drag over `id` (no messages): releases the NSDraggingInfo
    // and the destination and clears them in the session object, which a
    // running handle_drag may still hold.
    void end_drag(WindowId id);
    // Queues the key presses that carry a window-server text event.
    void enqueue_text(Address window, const Event& event);

    // ---- views (view.cpp) --------------------------------------------------------------
    // The NSAppearance a view or window draws with: its own, else its
    // superview's, else its window's, else NSApp's -effectiveAppearance
    // (-[NSView effectiveAppearance] answers the same).
    Address effective_appearance(Address view_or_window);
    void set_view_window(Address view, Address window);
    bool flipped_against_superview(Address view);
    Point view_to_window(Address view, Point point);
    Point window_to_view(Address view, Point point);
    Address hit_test(Address window, Point location);
    void display_view(Address view);
    void update_tracking(Address window, Address event);
    void interpret_key(Address client, Address event);
    Address input_context_of(Address responder);
    std::vector<Address> all_views(Address root);
    // Bounds transforms: a view's own (bounds) coordinates <-> its frame
    // space (unflipped, relative to the frame origin).
    static Point bounds_to_frame(const ViewState& state, Point point);
    static Point frame_to_bounds(const ViewState& state, Point point);
    static Rect view_bounds(const ViewState& state);
    // Cursor rectangles (-addCursorRect:cursor:): sets the cursor of the
    // frontmost rectangle under `location` (window base coordinates).
    void update_cursor_rects(Address window, Point location);
    void discard_cursor_rects(Address view);
    // QuartzCore presentation hosts (quartzcore.hpp): tells QuartzCore where
    // the CAMetalLayer of `view` (or of every view under `root`) is shown.
    void update_layer_host(Address view);
    void update_layer_hosts(Address root);
    void drop_layer(Address view, Address layer);   // chrome-s2 w1: releases a view's layer, clearing its delegate (view.cpp)

    // ---- events (event.cpp) ------------------------------------------------------------
    Address make_event(const Event& event, Address window);   // +1
    Address make_cg_event(Address event);                     // +1 CGEventRef of -[NSEvent CGEvent]
    Address make_event_from_cg(Address cg_event);             // +1 NSEvent of +eventWithCGEvent:
    std::shared_ptr<EventState> event_state(Address event);
    std::uint64_t mask_bit(std::uint32_t type) { return type < 64 ? 1ULL << type : 0; }
    Address run_monitors(Address event);

    // ---- resources (resources.cpp) -------------------------------------------------------
    Address screen_object(ScreenId id);
    Address main_screen();
    Address general_pasteboard();
    Address pasteboard_named(std::string_view name);
    Address make_cursor(SystemCursor kind);
    void set_cursor(Address cursor);
    std::shared_ptr<BitmapState> first_bitmap(Address image);
    Address make_bitmap(std::int64_t width, std::int64_t height);   // +1 RGBA8 non-premultiplied
    void setup_bitmap(Address rep, std::int64_t width, std::int64_t height);
    void fill_image(Address image, const DecodedImage& decoded);
    std::vector<std::uint8_t> data_bytes(Address data);
    Address make_data(std::span<const std::uint8_t> bytes);   // autoreleased NSData
    std::optional<std::vector<std::uint8_t>> read_file(std::string_view guest_path);
    std::optional<Image> image_pixels(Address image);   // the first bitmap representation
    std::optional<Image> bitmap_pixels(const std::shared_ptr<BitmapState>& rep);
    Address image_from_pixels(const Image& pixels);   // +1 NSImage
    Address color_object(double r, double g, double b, double a);   // autoreleased
    // A named colour that is one shared, immortal object for the process, as
    // on macOS (Qt keeps some in statics without retaining them): the first
    // answer `make` gives for `name` (resources.cpp).
    Address shared_color(const std::string& name, const std::function<Address()>& make);
    // NSApplicationIcon (resources.cpp): +1 once made.
    Address application_icon_image{};
    Address make_application_icon();

    // Per-object state another installer keeps for AppKit's objects in a side
    // table (Chrome's properties): the -dealloc of NSWindow, NSMenu,
    // NSMenuItem and NSBitmapImageRep tells every observer before its own
    // cleanup, and their -copyWithZone: after making the copy. One observer
    // per `domain`; the registration audit lists each one as an extension.
    struct ObjectLifetimeObserver {
        std::function<void(Address object)> deallocated;
        std::function<void(Address source, Address copy)> copied;
    };
    void observe_object_lifetimes(std::string domain, ObjectLifetimeObserver observer,
                                  const std::source_location& where = std::source_location::current());
    void object_deallocated(Address object);
    void object_copied(Address source, Address copy);
    std::mutex lifetime_mutex;
    std::vector<std::pair<std::string, ObjectLifetimeObserver>> lifetime_observers;

    // The layout engine (QLab's Auto Layout, windows/auto_layout.cpp):
    // -[NSWindow display] and -displayIfNeeded run its layout pass before
    // drawing, as the display cycle does, and -[NSView setAutoresizingMask:]
    // tells it when a mask changes. At most one; the registration audit lists
    // it as an extension.
    struct LayoutEngine {
        std::function<void(Address window)> layout_window;
        std::function<void(Address view)> autoresizing_changed;
    };
    void set_layout_engine(std::string domain, LayoutEngine engine,
                           const std::source_location& where = std::source_location::current());
    std::optional<std::pair<std::string, LayoutEngine>> layout_engine;
    std::mutex shared_colors_mutex;
    std::map<std::string, Address> shared_colors;   // +1, immortal
    Address appearance_named(std::string_view name);   // autoreleased, nil when unknown
    std::string system_appearance_name();
    std::string host_path(std::string_view guest);
};

// -backingAlignedRect:options: (view.cpp): `rect` in points aligned to the
// pixel grid of `scale` per NSAlignmentOptions (per axis exactly two of the
// minimum edge, the maximum edge and the size; inward, outward or nearest).
[[nodiscard]] Rect backing_aligned(Rect rect, double scale, std::uint64_t options);

// A window's caption for the window server: title and subtitle (window.cpp).
[[nodiscard]] std::string caption(AppKit& kit, const WindowState& state);

// The process's AppKit provider (nullptr before install_appkit_gui).
[[nodiscard]] std::shared_ptr<AppKit> instance() noexcept;

// Shared between files: the per-window sink, so NSWindow objects receive
// window-server callbacks.
struct WindowSink final : WindowEventSink {
    std::weak_ptr<AppKit> kit;
    explicit WindowSink(std::weak_ptr<AppKit> owner) : kit(std::move(owner)) {}
    void handle_event(const Event& event) override;
    Size will_resize(WindowId window, Size proposed) override;
    std::optional<Rect> text_caret_rect(WindowId window) override;
    std::uint64_t drag_operation(WindowId window) override;
};

// NSString values of the AppKit notification names posted here.
namespace note {
inline constexpr std::string_view did_finish_launching = "NSApplicationDidFinishLaunchingNotification",
    will_finish_launching = "NSApplicationWillFinishLaunchingNotification",
    did_become_active = "NSApplicationDidBecomeActiveNotification",
    will_become_active = "NSApplicationWillBecomeActiveNotification",
    did_resign_active = "NSApplicationDidResignActiveNotification",
    will_resign_active = "NSApplicationWillResignActiveNotification",
    will_terminate = "NSApplicationWillTerminateNotification",
    did_hide = "NSApplicationDidHideNotification", will_hide = "NSApplicationWillHideNotification",
    did_unhide = "NSApplicationDidUnhideNotification", will_unhide = "NSApplicationWillUnhideNotification",
    screen_parameters = "NSApplicationDidChangeScreenParametersNotification",
    will_update = "NSApplicationWillUpdateNotification", did_update = "NSApplicationDidUpdateNotification",
    did_change_occlusion = "NSApplicationDidChangeOcclusionStateNotification",
    protected_data_will_become_unavailable = "NSApplicationProtectedDataWillBecomeUnavailableNotification",
    protected_data_did_become_available = "NSApplicationProtectedDataDidBecomeAvailableNotification",
    window_did_resize = "NSWindowDidResizeNotification", window_did_move = "NSWindowDidMoveNotification",
    window_did_become_key = "NSWindowDidBecomeKeyNotification",
    window_did_resign_key = "NSWindowDidResignKeyNotification",
    window_did_become_main = "NSWindowDidBecomeMainNotification",
    window_did_resign_main = "NSWindowDidResignMainNotification",
    window_will_close = "NSWindowWillCloseNotification",
    window_did_miniaturize = "NSWindowDidMiniaturizeNotification",
    window_will_miniaturize = "NSWindowWillMiniaturizeNotification",
    window_did_deminiaturize = "NSWindowDidDeminiaturizeNotification",
    window_will_enter_full_screen = "NSWindowWillEnterFullScreenNotification",
    window_did_enter_full_screen = "NSWindowDidEnterFullScreenNotification",
    window_will_exit_full_screen = "NSWindowWillExitFullScreenNotification",
    window_did_exit_full_screen = "NSWindowDidExitFullScreenNotification",
    window_did_change_backing = "NSWindowDidChangeBackingPropertiesNotification",
    window_did_change_screen = "NSWindowDidChangeScreenNotification",
    window_did_change_occlusion = "NSWindowDidChangeOcclusionStateNotification",
    window_will_start_live_resize = "NSWindowWillStartLiveResizeNotification",
    window_did_end_live_resize = "NSWindowDidEndLiveResizeNotification",
    window_did_expose = "NSWindowDidExposeNotification",
    window_will_move = "NSWindowWillMoveNotification", window_did_update = "NSWindowDidUpdateNotification",
    window_did_change_screen_profile = "NSWindowDidChangeScreenProfileNotification",
    window_will_begin_sheet = "NSWindowWillBeginSheetNotification", window_did_end_sheet = "NSWindowDidEndSheetNotification",
    view_frame_did_change = "NSViewFrameDidChangeNotification",
    view_bounds_did_change = "NSViewBoundsDidChangeNotification",
    keyboard_selection = "NSTextInputContextKeyboardSelectionDidChangeNotification";
} // namespace note

} // namespace alchemy::gui::appkit
