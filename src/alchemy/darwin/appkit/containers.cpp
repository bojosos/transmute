#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"

#include <algorithm>
#include <cmath>
#include <set>

// Container views: NSScrollView / NSClipView / NSScroller (legacy-style
// scrollers, scroll wheel, scrollers dragging), NSView scrolling helpers,
// NSTabView / NSTabViewItem, NSSplitView, NSTableView / NSTableColumn /
// NSTableHeaderView / NSTableCellView / NSOutlineView (cell-based drawing and
// view-based cells from the delegate, selection, header), and the
// NSIndexSet / NSMutableIndexSet classes the table API needs (installed only
// when Foundation does not provide them).
namespace alchemy::gui::appkit {
namespace {
using drawing::Color;
constexpr double scroller_width = 15;
constexpr double header_height = 24;

struct EdgeInsets { double top{}, left{}, bottom{}, right{}; };
struct ScrollState final : ObjectPayload {
    Address clip{}, vertical{}, horizontal{}, header{};   // subviews (the subview list retains them)
    Address background_color{};                           // +1
    bool has_vertical{}, has_horizontal{}, autohides{true}, draws_background{true};
    std::int64_t border_type{};
    double line_scroll{10}, page_scroll{10};
    // Magnification bounds; the document is drawn at magnification 1 only.
    double min_magnification{0.25}, max_magnification{4};
    bool allows_magnification{};
    // Rulers (NSRulerView, kit/completion/controls/ruler.cpp): +1, subviews while shown.
    Address horizontal_ruler{}, vertical_ruler{};
    bool has_horizontal_ruler{}, has_vertical_ruler{}, rulers_visible{};
    EdgeInsets content_insets, scroller_insets;
    // Floating subviews (-addFloatingSubview:forAxis:): subviews of the clip
    // view held still along their axis; their frame origin in the document.
    struct Floating { Address view{}; std::int64_t axis{}; Point origin; };   // view +1
    std::vector<Floating> floating;
};
struct ClipState final : ObjectPayload {
    Address document{};                                   // a subview
    Address background_color{};                           // +1
    bool draws_background{true};
};
struct TabState final : ObjectPayload {
    std::vector<Address> items;                           // +1 NSTabViewItems
    std::int64_t selected{-1}, type{0};
    std::uint64_t control_size{};                         // NSControlSize (regular)
    Address delegate{}, font{};                           // font +1
    bool draws_background{true};
};
struct TabItemState final : ObjectPayload {
    Address label{}, view{}, identifier{}, tab_view{};    // label/view/identifier +1
    Address color{};
};
struct SplitState final : ObjectPayload {
    bool vertical{};
    std::int64_t divider_style{1};                        // NSSplitViewDividerStyleThick
    Address delegate{};
    Address autosave_name{};                              // +1
    std::vector<float> holding;                           // by subview index (NSLayoutPriorityDefaultLow when absent)
    bool spring_loaded{};
};
struct ColumnState final : ObjectPayload {
    Address identifier{}, header_cell{}, data_cell{};     // +1
    Address table{};
    double width{100}, min_width{10}, max_width{1e6};
    std::uint64_t resizing_mask{3};
    bool hidden{}, editable{true};
    Address header_tool_tip{}, sort_prototype{};       // +1
};
struct TableState final : ControlState {
    std::vector<Address> columns;                         // +1 NSTableColumns
    Address data_source{}, table_delegate{};
    Address header_view{}, corner_view{};                 // +1
    std::set<std::int64_t> selection;
    std::int64_t rows{}, clicked_row{-1}, clicked_column{-1}, grid_style{}, highlight_style{0}, autoresizing_style{1};
    double row_height{17};
    Size spacing{3, 2};
    bool alternating{}, multiple{}, empty_selection{true}, column_selection{}, row_height_set{};
    // Kept settings: the grid colour is drawn; the rest are answered back
    // (row heights are not computed from cell views, there is no type-select
    // and group rows do not float).
    Address grid_color{};                                 // +1 (nil: +[NSColor gridColor])
    std::int64_t table_style{}, feedback_style{};
    bool automatic_row_heights{}, type_select{true}, floats_group_rows{true};
    // View-based cells: [row][column] -> view (+1, subviews of the table).
    std::map<std::pair<std::int64_t, std::size_t>, Address> cell_views;
    std::map<std::string, std::vector<Address>> reuse;    // +1 by identifier
    std::map<std::string, Address> registered_nibs;      // +1 NSNib by identifier (registerNib:forIdentifier:)
    // Row views (view-based tables): one per row, the row's cell views inside (+1).
    std::map<std::int64_t, Address> row_views;
    // In-place editing (-editColumn:row:withEvent:select:): the edited cell and,
    // in a view-based table, the cell view's text field doing the editing.
    std::int64_t edited_row{-1}, edited_column{-1};
    Address edited_field{};                               // not retained (a subview of a cell view)
    // Drag and drop targets (-setDropRow:dropOperation:, -setDropItem:dropChildIndex:).
    std::int64_t drop_row{-1}, drop_operation{}, drop_child{-1};
    Address drop_item{};
    // NSOutlineView: the expanded tree flattened into rows.
    struct Row { Address item; std::int64_t level; bool expandable; };
    std::vector<Row> outline_rows;
    std::set<Address> expanded;                           // items retained while expanded? (not retained: data-source owned)
    Address outline_column{};
    double indentation{16};
};
struct HeaderState final : ObjectPayload { Address table{}; };
// NSTableRowView: its table and row, the drawing state the table gives it.
struct RowViewState final : ObjectPayload {
    Address table{};                                      // not retained
    std::int64_t row{-1};
    Address background_color{};                           // +1
    std::int64_t highlight_style{};                       // NSTableViewSelectionHighlightStyle
    std::int64_t feedback_style{};                        // NSTableViewDraggingDestinationFeedbackStyle (regular)
    bool selected{}, emphasized{true}, group_row{}, floating{}, target_for_drop{};
};
struct IndexSetState final : ObjectPayload { std::set<std::uint64_t> values; };
struct CellViewState final : ObjectPayload {};

Rect inset(Rect r, double dx, double dy) {
    return {{r.origin.x + dx, r.origin.y + dy}, {std::max(0.0, r.size.width - 2 * dx), std::max(0.0, r.size.height - 2 * dy)}};
}
} // namespace

void AppKit::install_containers() {
    scroll_view = declare_class("NSScrollView", "NSView");
    clip_view = declare_class("NSClipView", "NSView");
    scroller = declare_class("NSScroller", "NSControl");
    tab_view = declare_class("NSTabView", "NSView");
    tab_view_item = declare_class("NSTabViewItem", "NSObject");
    split_view = declare_class("NSSplitView", "NSView");
    table_view = declare_class("NSTableView", "NSControl");
    outline_view = declare_class("NSOutlineView", "NSTableView");
    table_column = declare_class("NSTableColumn", "NSObject");
    table_header_view = declare_class("NSTableHeaderView", "NSView");
    table_cell_view = declare_class("NSTableCellView", "NSView");
    table_row_view = declare_class("NSTableRowView", "NSView");
    const auto header_cell_class = declare_class("NSTableHeaderCell", "NSTextFieldCell");
    string_constant("_NSViewBoundsDidChangeNotification", "NSViewBoundsDidChangeNotification");
    string_constant("_NSTableViewSelectionDidChangeNotification", "NSTableViewSelectionDidChangeNotification");
    string_constant("_NSOutlineViewSelectionDidChangeNotification", "NSOutlineViewSelectionDidChangeNotification");
    string_constant("_NSOutlineViewItemDidExpandNotification", "NSOutlineViewItemDidExpandNotification");
    string_constant("_NSOutlineViewItemDidCollapseNotification", "NSOutlineViewItemDidCollapseNotification");
    install_index_sets();

    // ---- NSView scrolling helpers ----
    method(view, "enclosingScrollView", [this](CpuState& s, GuestMemory&) {
        for (auto current = view_state(s.x[0])->superview; current; current = view_state(current)->superview)
            if (is_kind(current, scroll_view)) { s.x[0] = current; return; }
        s.x[0] = 0;
    });
    method(view, "scrollRectToVisible:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto parent = view_state(self)->superview;
        if (!parent || !is_kind(parent, clip_view)) { s.x[0] = 0; return; }
        const auto wanted = drawing::bounding_rect(view_transform(self), rect_arg(s));        // window coordinates
        const auto clip_bounds = view_bounds(*view_state(parent));
        const auto in_clip = window_rect_in_view(parent, wanted);
        auto origin = clip_bounds.origin;
        if (in_clip.origin.x < origin.x) origin.x = in_clip.origin.x;
        else if (in_clip.origin.x + in_clip.size.width > origin.x + clip_bounds.size.width)
            origin.x = in_clip.origin.x + in_clip.size.width - clip_bounds.size.width;
        if (in_clip.origin.y < origin.y) origin.y = in_clip.origin.y;
        else if (in_clip.origin.y + in_clip.size.height > origin.y + clip_bounds.size.height)
            origin.y = in_clip.origin.y + in_clip.size.height - clip_bounds.size.height;
        CallRegs registers;
        put_point(registers, 0, origin);
        static_cast<void>(send(parent, "scrollToPoint:", registers));
        if (const auto owner = view_state(parent)->superview; owner && is_kind(owner, scroll_view))
            static_cast<void>(send_id(owner, "reflectScrolledClipView:", {parent}));
        s.x[0] = 1;
    }, "B@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(view, "scrollPoint:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, {point_arg(s), view_state(s.x[0])->superview ? view_bounds(*view_state(view_state(s.x[0])->superview)).size : Size{}});
        static_cast<void>(send(s.x[0], "scrollRectToVisible:", registers));
    }, "v@:{CGPoint=dd}");
    method(view, "autoscroll:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@");

    // ---- NSClipView ----
    const auto cv = clip_view;
    const auto clip_of = [this](Address value) { return extension<ClipState>(value); };
    method(cv, "documentView", [clip_of](CpuState& s, GuestMemory&) { s.x[0] = clip_of(s.x[0])->document; });
    method(cv, "setDocumentView:", [this, clip_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = clip_of(self);
        if (state->document == s.x[2]) return;
        if (state->document) static_cast<void>(send(state->document, "removeFromSuperview"));
        state->document = s.x[2];
        if (s.x[2]) static_cast<void>(send_id(self, "addSubview:", {s.x[2]}));
        CallRegs registers;
        put_point(registers, 0, {0, 0});
        static_cast<void>(send(self, "scrollToPoint:", registers));
    }, "v@:@");
    method(cv, "isFlipped", [this, clip_of](CpuState& s, GuestMemory&) {
        const auto document = clip_of(s.x[0])->document;
        return_bool(s, document && send_bool(document, "isFlipped"));
    }, "B@:");
    method(cv, "documentRect", [this, clip_of](CpuState& s, GuestMemory&) {
        const auto document = clip_of(s.x[0])->document;
        const auto own = view_bounds(*view_state(s.x[0]));
        if (!document) { return_rect(s, own); return; }
        const auto frame = view_state(document)->frame;
        return_rect(s, {frame.origin, {std::max(frame.size.width, own.size.width), std::max(frame.size.height, own.size.height)}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(cv, "documentVisibleRect", [this, clip_of](CpuState& s, GuestMemory&) {
        const auto document = clip_of(s.x[0])->document;
        const auto own = view_bounds(*view_state(s.x[0]));
        return_rect(s, document ? drawing::intersect(own, view_state(document)->frame) : own);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(cv, "constrainBoundsRect:", [this, clip_of](CpuState& s, GuestMemory&) {
        auto proposed = rect_arg(s);
        const auto document = clip_of(s.x[0])->document;
        if (document) {
            const auto frame = view_state(document)->frame;
            proposed.origin.x = std::clamp(proposed.origin.x, frame.origin.x, std::max(frame.origin.x, frame.origin.x + frame.size.width - proposed.size.width));
            proposed.origin.y = std::clamp(proposed.origin.y, frame.origin.y, std::max(frame.origin.y, frame.origin.y + frame.size.height - proposed.size.height));
        } else {
            proposed.origin = {0, 0};
        }
        return_rect(s, proposed);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(cv, "scrollToPoint:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        CallRegs registers;
        put_rect(registers, 0, {point_arg(s), view_bounds(*view_state(self)).size});
        const auto constrained = result_rect(send(self, "constrainBoundsRect:", registers));
        CallRegs origin;
        put_point(origin, 0, constrained.origin);
        static_cast<void>(send(self, "setBoundsOrigin:", origin));
        invalidate_view(self);
    }, "v@:{CGPoint=dd}");
    method(cv, "drawsBackground", [clip_of](CpuState& s, GuestMemory&) { return_bool(s, clip_of(s.x[0])->draws_background); }, "B@:");
    method(cv, "setDrawsBackground:", [this, clip_of](CpuState& s, GuestMemory&) { clip_of(s.x[0])->draws_background = bool_arg(s, 2); invalidate_view(s.x[0]); }, "v@:B");
    method(cv, "backgroundColor", [clip_of](CpuState& s, GuestMemory&) { s.x[0] = clip_of(s.x[0])->background_color; });
    method(cv, "setBackgroundColor:", [this, clip_of](CpuState& s, GuestMemory&) { assign(clip_of(s.x[0])->background_color, s.x[2]); invalidate_view(s.x[0]); }, "v@:@");
    method(cv, "copiesOnScroll", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(cv, "setCopiesOnScroll:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(cv, "drawRect:", [this, clip_of](CpuState& s, GuestMemory&) {
        resolve_dynamic_colors(*this, s.x[0]);
        const auto state = clip_of(s.x[0]);
        if (!state->draws_background) return;
        require_canvas("NSClipView drawing").fill_rect(view_bounds(*view_state(s.x[0])),
                                                       color_value(state->background_color, theme_for(s.x[0]).control_background));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(cv, "viewFrameChanged:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(cv, "dealloc", [this, clip_of](CpuState& s, GuestMemory&) {
        if (const auto state = find_extension<ClipState>(s.x[0])) release(std::exchange(state->background_color, 0));
        static_cast<void>(objc.send_super(s.x[0], clip_view, sel("dealloc")));
    }, "v@:");

    // ---- NSScroller ----
    const auto sc = scroller;
    const auto bar = [this](Address value) { return extension<ControlState>(value); };
    class_method(sc, "scrollerWidthForControlSize:scrollerStyle:", [](CpuState& s, GuestMemory&) { set_d(s, 0, scroller_width); }, "d@:QQ");
    class_method(sc, "scrollerWidth", [](CpuState& s, GuestMemory&) { set_d(s, 0, scroller_width); }, "d@:");
    class_method(sc, "preferredScrollerStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");   // legacy
    method(sc, "initWithFrame:", [this, bar](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        const auto self = objc.send_super(s.x[0], scroller, sel("initWithFrame:"), registers).x0;
        s.x[0] = self;
        const auto frame = view_state(self)->frame;
        bar(self)->vertical = frame.size.height >= frame.size.width;
        bar(self)->value = 0;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(sc, "doubleValue", [bar](CpuState& s, GuestMemory&) { set_d(s, 0, bar(s.x[0])->value); }, "d@:");
    method(sc, "setDoubleValue:", [this, bar](CpuState& s, GuestMemory&) { bar(s.x[0])->value = std::clamp(d(s, 0), 0.0, 1.0); invalidate_view(s.x[0]); }, "v@:d");
    method(sc, "floatValue", [bar](CpuState& s, GuestMemory&) { set_f(s, 0, static_cast<float>(bar(s.x[0])->value)); }, "f@:");
    method(sc, "knobProportion", [bar](CpuState& s, GuestMemory&) { set_d(s, 0, bar(s.x[0])->knob_proportion); }, "d@:");
    method(sc, "setKnobProportion:", [this, bar](CpuState& s, GuestMemory&) { bar(s.x[0])->knob_proportion = std::clamp(d(s, 0), 0.0, 1.0); invalidate_view(s.x[0]); }, "v@:d");
    method(sc, "isEnabled", [bar](CpuState& s, GuestMemory&) { return_bool(s, bar(s.x[0])->knob_proportion < 1); }, "B@:");
    method(sc, "setEnabled:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(sc, "scrollerStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(sc, "setScrollerStyle:", [](CpuState&, GuestMemory&) {}, "v@:q");
    method(sc, "knobStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(sc, "setKnobStyle:", [](CpuState&, GuestMemory&) {}, "v@:q");
    method(sc, "controlSize", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");
    method(sc, "setControlSize:", [](CpuState&, GuestMemory&) {}, "v@:Q");
    method(sc, "hitPart", [](CpuState& s, GuestMemory&) { s.x[0] = 2; }, "Q@:");   // NSScrollerKnob
    const auto knob_rect = [this, bar](Address self) {
        const auto state = bar(self);
        const auto bounds = view_bounds(*view_state(self));
        const auto track = inset(bounds, 3, 3);
        const auto length = state->vertical ? track.size.height : track.size.width;
        const auto knob = std::max(18.0, length * state->knob_proportion);
        const auto offset = (length - knob) * state->value;
        return state->vertical ? Rect{{track.origin.x, track.origin.y + offset}, {track.size.width, knob}}
                               : Rect{{track.origin.x + offset, track.origin.y}, {knob, track.size.height}};
    };
    method(sc, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(sc, "drawRect:", [this, bar, knob_rect](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto& th = theme_for(self);
        auto& canvas = require_canvas("NSScroller drawing");
        const auto bounds = view_bounds(*view_state(self));
        // A legacy scroller: the track with a separator line along the
        // content edge (the leading edge of a vertical scroller, the top of a
        // horizontal one), then the knob when the content overflows.
        canvas.fill_rect(bounds, th.scroller_track);
        if (bar(self)->vertical) canvas.fill_rect({bounds.origin, {1, bounds.size.height}}, th.separator);
        else canvas.fill_rect({bounds.origin, {bounds.size.width, 1}}, th.separator);
        if (bar(self)->knob_proportion >= 1) return;
        const auto knob = knob_rect(self);
        const auto radius = std::min(knob.size.width, knob.size.height) / 2;
        drawing::Path path;
        path.add_rounded_rect(knob, radius, radius);
        canvas.fill_path(path, th.scroller_knob);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(sc, "mouseDown:", [this, bar, knob_rect](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = bar(self);
        if (state->knob_proportion >= 1) return;
        const auto bounds = view_bounds(*view_state(self));
        const auto start = event_location_in(*this, self, s.x[2]);
        const auto knob = knob_rect(self);
        const auto track = inset(bounds, 3, 3);
        const auto length = (state->vertical ? track.size.height : track.size.width) - (state->vertical ? knob.size.height : knob.size.width);
        const bool on_knob = rect_contains(knob, start);
        const auto start_value = state->value;
        const auto notify = [&] {
            if (state->action) static_cast<void>(send_action(state->action, state->target, self));
            invalidate_view(self);
        };
        if (!on_knob) {
            // A click in the track pages toward the click.
            const auto before = state->vertical ? start.y < knob.origin.y : start.x < knob.origin.x;
            const auto page = state->knob_proportion / std::max(1e-9, 1 - state->knob_proportion);
            state->value = std::clamp(state->value + (before ? -page : page), 0.0, 1.0);
            notify();
            return;
        }
        static_cast<void>(track_mouse(*this, self, s.x[2], [](bool) {}, [&](Point at) {
            const auto delta = state->vertical ? at.y - start.y : at.x - start.x;
            state->value = std::clamp(start_value + (length > 0 ? delta / length : 0), 0.0, 1.0);
            notify();
        }));
    }, "v@:@");

    // ---- NSScrollView ----
    const auto sv = scroll_view;
    const auto scroll_of = [this](Address value) { return extension<ScrollState>(value); };
    class_method(sv, "scrollerStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    for (const auto* name : {"contentSizeForFrameSize:horizontalScrollerClass:verticalScrollerClass:borderType:controlSize:scrollerStyle:",
             "frameSizeForContentSize:horizontalScrollerClass:verticalScrollerClass:borderType:controlSize:scrollerStyle:"}) {
        const bool content = std::string_view(name).starts_with("content");
        class_method(sv, name, [content](CpuState& s, GuestMemory&) {
            auto size = size_arg(s);
            const double sign = content ? -1 : 1;
            if (s.x[2]) size.height += sign * scroller_width;
            if (s.x[3]) size.width += sign * scroller_width;
            if (s.x[4]) { size.width += sign * 2; size.height += sign * 2; }
            return_size(s, size);
        }, "{CGSize=dd}@:{CGSize=dd}##QQq");
    }
    for (const auto* name : {"contentSizeForFrameSize:hasHorizontalScroller:hasVerticalScroller:borderType:",
             "frameSizeForContentSize:hasHorizontalScroller:hasVerticalScroller:borderType:"}) {
        const bool content = std::string_view(name).starts_with("content");
        class_method(sv, name, [content](CpuState& s, GuestMemory&) {
            auto size = size_arg(s);
            const double sign = content ? -1 : 1;
            if (s.x[2] & 0xff) size.height += sign * scroller_width;
            if (s.x[3] & 0xff) size.width += sign * scroller_width;
            if (s.x[4]) { size.width += sign * 2; size.height += sign * 2; }
            return_size(s, size);
        }, "{CGSize=dd}@:{CGSize=dd}BBQ");
    }
    method(sv, "initWithFrame:", [this, scroll_of](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        const auto self = objc.send_super(s.x[0], scroll_view, sel("initWithFrame:"), registers).x0;
        s.x[0] = self;
        const auto state = scroll_of(self);
        CallRegs zero;
        put_rect(zero, 0, {});
        const auto made = send(send(clip_view, "alloc").x0, "initWithFrame:", zero).x0;
        static_cast<void>(send_id(self, "addSubview:", {made}));
        release(made);
        state->clip = made;
        static_cast<void>(send(self, "tile"));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(sv, "dealloc", [this, scroll_of](CpuState& s, GuestMemory&) {
        if (const auto state = find_extension<ScrollState>(s.x[0])) {
            release(std::exchange(state->background_color, 0));
            release(std::exchange(state->horizontal_ruler, 0));
            release(std::exchange(state->vertical_ruler, 0));
            for (auto& floating : std::exchange(state->floating, {})) release(floating.view);
        }
        static_cast<void>(objc.send_super(s.x[0], scroll_view, sel("dealloc")));
    }, "v@:");
    method(sv, "contentView", [scroll_of](CpuState& s, GuestMemory&) { s.x[0] = scroll_of(s.x[0])->clip; });
    method(sv, "setContentView:", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto state = scroll_of(s.x[0]);
        if (!s.x[2] || state->clip == s.x[2]) return;
        const auto document = state->clip ? send(state->clip, "documentView").x0 : 0;
        if (state->clip) static_cast<void>(send(state->clip, "removeFromSuperview"));
        state->clip = s.x[2];
        static_cast<void>(send_id(s.x[0], "addSubview:", {s.x[2]}));
        if (document && !send(s.x[2], "documentView").x0) static_cast<void>(send_id(s.x[2], "setDocumentView:", {document}));
        static_cast<void>(send(s.x[0], "tile"));
    }, "v@:@");
    method(sv, "documentView", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto clip = scroll_of(s.x[0])->clip;
        s.x[0] = clip ? send(clip, "documentView").x0 : 0;
    });
    method(sv, "setDocumentView:", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto clip = scroll_of(s.x[0])->clip;
        if (!clip) return;
        static_cast<void>(send_id(clip, "setDocumentView:", {s.x[2]}));
        static_cast<void>(send(s.x[0], "tile"));
        static_cast<void>(send_id(s.x[0], "reflectScrolledClipView:", {clip}));
    }, "v@:@");
    const auto make_scroller = [this, scroll_of](Address self, bool vertical) {
        const auto state = scroll_of(self);
        auto& slot = vertical ? state->vertical : state->horizontal;
        if (slot) return slot;
        CallRegs registers;
        put_rect(registers, 0, {{0, 0}, vertical ? Size{scroller_width, 100} : Size{100, scroller_width}});
        const auto made = send(send(scroller, "alloc").x0, "initWithFrame:", registers).x0;
        extension<ControlState>(made)->vertical = vertical;
        extension<ControlState>(made)->target = self;
        extension<ControlState>(made)->action = sel("_alchemyScrollerMoved:");
        static_cast<void>(send_id(self, "addSubview:", {made}));
        release(made);
        slot = made;
        return made;
    };
    for (const bool vertical : {true, false}) {
        const std::string axis = vertical ? "Vertical" : "Horizontal";
        method(sv, "has" + axis + "Scroller", [scroll_of, vertical](CpuState& s, GuestMemory&) {
            const auto state = scroll_of(s.x[0]);
            return_bool(s, vertical ? state->has_vertical : state->has_horizontal);
        }, "B@:");
        method(sv, "setHas" + axis + "Scroller:", [this, scroll_of, vertical, make_scroller](CpuState& s, GuestMemory&) {
            const auto state = scroll_of(s.x[0]);
            (vertical ? state->has_vertical : state->has_horizontal) = bool_arg(s, 2);
            if (bool_arg(s, 2)) static_cast<void>(make_scroller(s.x[0], vertical));
            static_cast<void>(send(s.x[0], "tile"));
        }, "v@:B");
        std::string lower = axis;
        lower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[0])));
        method(sv, lower + "Scroller", [scroll_of, vertical, make_scroller](CpuState& s, GuestMemory&) {
            s.x[0] = make_scroller(s.x[0], vertical);
            static_cast<void>(scroll_of);
        });
        method(sv, "set" + axis + "Scroller:", [](CpuState&, GuestMemory&) { unsupported("custom NSScroller classes of NSScrollView"); }, "v@:@");
        method(sv, lower + "LineScroll", [scroll_of](CpuState& s, GuestMemory&) { set_d(s, 0, scroll_of(s.x[0])->line_scroll); }, "d@:");
        method(sv, "set" + axis + "LineScroll:", [scroll_of](CpuState& s, GuestMemory&) { scroll_of(s.x[0])->line_scroll = d(s, 0); }, "v@:d");
        method(sv, lower + "PageScroll", [scroll_of](CpuState& s, GuestMemory&) { set_d(s, 0, scroll_of(s.x[0])->page_scroll); }, "d@:");
        method(sv, "set" + axis + "PageScroll:", [scroll_of](CpuState& s, GuestMemory&) { scroll_of(s.x[0])->page_scroll = d(s, 0); }, "v@:d");
        method(sv, "set" + axis + "ScrollElasticity:", [](CpuState&, GuestMemory&) {}, "v@:q");
        method(sv, lower + "ScrollElasticity", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    }
    method(sv, "setLineScroll:", [scroll_of](CpuState& s, GuestMemory&) { scroll_of(s.x[0])->line_scroll = d(s, 0); }, "v@:d");
    method(sv, "lineScroll", [scroll_of](CpuState& s, GuestMemory&) { set_d(s, 0, scroll_of(s.x[0])->line_scroll); }, "d@:");
    method(sv, "setPageScroll:", [scroll_of](CpuState& s, GuestMemory&) { scroll_of(s.x[0])->page_scroll = d(s, 0); }, "v@:d");
    method(sv, "autohidesScrollers", [scroll_of](CpuState& s, GuestMemory&) { return_bool(s, scroll_of(s.x[0])->autohides); }, "B@:");
    method(sv, "setAutohidesScrollers:", [this, scroll_of](CpuState& s, GuestMemory&) {
        scroll_of(s.x[0])->autohides = bool_arg(s, 2);
        static_cast<void>(send(s.x[0], "tile"));
    }, "v@:B");
    method(sv, "borderType", [scroll_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(scroll_of(s.x[0])->border_type); }, "Q@:");
    method(sv, "setBorderType:", [this, scroll_of](CpuState& s, GuestMemory&) {
        scroll_of(s.x[0])->border_type = static_cast<std::int64_t>(s.x[2]);
        static_cast<void>(send(s.x[0], "tile"));
    }, "v@:Q");
    method(sv, "drawsBackground", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto clip = scroll_of(s.x[0])->clip;
        return_bool(s, clip && send_bool(clip, "drawsBackground"));
    }, "B@:");
    method(sv, "setDrawsBackground:", [this, scroll_of](CpuState& s, GuestMemory&) {
        if (const auto clip = scroll_of(s.x[0])->clip) static_cast<void>(send_id(clip, "setDrawsBackground:", {s.x[2]}));
    }, "v@:B");
    method(sv, "backgroundColor", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto clip = scroll_of(s.x[0])->clip;
        s.x[0] = clip ? send(clip, "backgroundColor").x0 : 0;
    });
    method(sv, "setBackgroundColor:", [this, scroll_of](CpuState& s, GuestMemory&) {
        if (const auto clip = scroll_of(s.x[0])->clip) static_cast<void>(send_id(clip, "setBackgroundColor:", {s.x[2]}));
    }, "v@:@");
    method(sv, "scrollerStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(sv, "setScrollerStyle:", [](CpuState&, GuestMemory&) {}, "v@:q");
    method(sv, "scrollerKnobStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(sv, "setScrollerKnobStyle:", [](CpuState&, GuestMemory&) {}, "v@:q");
    method(sv, "setUsesPredominantAxisScrolling:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(sv, "setAutomaticallyAdjustsContentInsets:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(sv, "contentSize", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto clip = scroll_of(s.x[0])->clip;
        return_size(s, clip ? view_state(clip)->frame.size : Size{});
    }, "{CGSize=dd}@:");
    method(sv, "documentVisibleRect", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto clip = scroll_of(s.x[0])->clip;
        return_rect(s, clip ? result_rect(send(clip, "documentVisibleRect")) : Rect{});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(sv, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(sv, "tile", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = scroll_of(self);
        if (!state->clip) return;
        auto area = view_bounds(*view_state(self));   // flipped: y down from the top
        if (state->border_type != 0) area = inset(area, 1, 1);
        // Rulers along the top and the leading edge, when shown.
        const auto place_ruler = [&](Address ruler, bool shown, bool horizontal) {
            if (!ruler) return;
            const auto attached = view_state(ruler)->superview == self;
            if (!shown) { if (attached) static_cast<void>(send(ruler, "removeFromSuperview")); return; }
            if (!attached) static_cast<void>(send_id(self, "addSubview:", {ruler}));
            const auto thickness = send(ruler, "requiredThickness").d(0);
            CallRegs ruler_frame;
            if (horizontal) {
                put_rect(ruler_frame, 0, {area.origin, {area.size.width, thickness}});
                area.origin.y += thickness;
                area.size.height = std::max(0.0, area.size.height - thickness);
            } else {
                put_rect(ruler_frame, 0, {area.origin, {thickness, area.size.height}});
                area.origin.x += thickness;
                area.size.width = std::max(0.0, area.size.width - thickness);
            }
            static_cast<void>(send(ruler, "setFrame:", ruler_frame));
        };
        place_ruler(state->horizontal_ruler, state->rulers_visible && state->has_horizontal_ruler, true);
        place_ruler(state->vertical_ruler, state->rulers_visible && state->has_vertical_ruler, false);
        const auto document = send(state->clip, "documentView").x0;
        const auto document_size = document ? view_state(document)->frame.size : Size{};
        // A table's header view sits above the clip view.
        Address header{};
        if (document && is_kind(document, table_view)) header = send(document, "headerView").x0;
        if (header != state->header) {
            if (state->header) static_cast<void>(send(state->header, "removeFromSuperview"));
            state->header = header;
            if (header) static_cast<void>(send_id(self, "addSubview:", {header}));
        }
        const double top = header ? header_height : 0;
        bool show_vertical = state->has_vertical, show_horizontal = state->has_horizontal;
        if (state->autohides) {
            const auto height = area.size.height - top - (show_horizontal ? scroller_width : 0);
            show_vertical = show_vertical && document_size.height > height + 0.5;
            const auto width = area.size.width - (show_vertical ? scroller_width : 0);
            show_horizontal = show_horizontal && document_size.width > width + 0.5;
        }
        // Content insets keep the content clear of the edges (the clip view is
        // inset by them; content does not scroll under the inset area).
        const auto& ci = state->content_insets;
        const Rect clip_frame{{area.origin.x + ci.left, area.origin.y + top + ci.top},
                              {std::max(0.0, area.size.width - (show_vertical ? scroller_width : 0) - ci.left - ci.right),
                               std::max(0.0, area.size.height - top - (show_horizontal ? scroller_width : 0) - ci.top - ci.bottom)}};
        CallRegs registers;
        put_rect(registers, 0, clip_frame);
        static_cast<void>(send(state->clip, "setFrame:", registers));
        if (header) {
            CallRegs header_frame;
            put_rect(header_frame, 0, {{area.origin.x, area.origin.y}, {clip_frame.size.width, header_height}});
            static_cast<void>(send(header, "setFrame:", header_frame));
        }
        const auto place = [&](Address bar_view, bool show, Rect frame) {
            if (!bar_view) return;
            CallRegs hidden;
            hidden.x[2] = show ? 0 : 1;
            static_cast<void>(send(bar_view, "setHidden:", hidden));
            CallRegs bar_frame;
            put_rect(bar_frame, 0, frame);
            static_cast<void>(send(bar_view, "setFrame:", bar_frame));
        };
        const auto& si = state->scroller_insets;
        place(state->vertical, show_vertical, {{clip_frame.origin.x + clip_frame.size.width + ci.right - si.right, area.origin.y + si.top},
                                               {scroller_width, std::max(0.0, area.size.height - (show_horizontal ? scroller_width : 0) - si.top - si.bottom)}});
        place(state->horizontal, show_horizontal, {{area.origin.x + si.left, clip_frame.origin.y + clip_frame.size.height + ci.bottom - si.bottom},
                                                   {std::max(0.0, clip_frame.size.width + ci.left + ci.right - si.left - si.right), scroller_width}});
        // A document view narrower than the clip view fills its width when it
        // tracks it (text views, tables).
        if (document && (is_kind(document, text_view) || is_kind(document, table_view))) {
            auto size = view_state(document)->frame.size;
            bool resize{};
            if (size.width < clip_frame.size.width) { size.width = clip_frame.size.width; resize = true; }
            if (is_kind(document, text_view) && size.width > clip_frame.size.width && !send_bool(document, "isHorizontallyResizable")) {
                size.width = clip_frame.size.width;
                resize = true;
            }
            if (resize) {
                CallRegs document_size_registers;
                put_size(document_size_registers, 0, size);
                static_cast<void>(send(document, "setFrameSize:", document_size_registers));
                if (is_kind(document, text_view)) static_cast<void>(send(document, "sizeToFit"));
            }
        }
        static_cast<void>(send_id(self, "reflectScrolledClipView:", {state->clip}));
        invalidate_view(self);
    }, "v@:");
    method(sv, "resizeSubviewsWithOldSize:", [this](CpuState& s, GuestMemory&) { static_cast<void>(send(s.x[0], "tile")); }, "v@:{CGSize=dd}");
    method(sv, "reflectScrolledClipView:", [this, scroll_of, bar](CpuState& s, GuestMemory&) {
        const auto state = scroll_of(s.x[0]);
        const auto clip = state->clip;
        if (!clip) return;
        const auto document = send(clip, "documentView").x0;
        const auto visible = view_bounds(*view_state(clip));
        const auto frame = document ? view_state(document)->frame : visible;
        const bool flipped = document && send_bool(document, "isFlipped");
        const auto update = [&](Address bar_view, double position, double visible_length, double total) {
            if (!bar_view) return;
            const auto info = bar(bar_view);
            info->knob_proportion = total > 0 ? std::clamp(visible_length / total, 0.0, 1.0) : 1;
            const auto room = total - visible_length;
            info->value = room > 0 ? std::clamp(position / room, 0.0, 1.0) : 0;
            invalidate_view(bar_view);
        };
        update(state->vertical, flipped ? visible.origin.y - frame.origin.y : frame.origin.y + frame.size.height - visible.origin.y - visible.size.height,
               visible.size.height, frame.size.height);
        update(state->horizontal, visible.origin.x - frame.origin.x, visible.size.width, frame.size.width);
        if (state->header) invalidate_view(state->header);
        // Floating subviews keep still along their axis (horizontal 1, vertical 2).
        for (const auto& floating : state->floating) {
            auto origin = floating.origin;
            if (floating.axis == 1) origin.x += visible.origin.x;
            if (floating.axis == 2) origin.y += visible.origin.y;
            CallRegs registers;
            put_point(registers, 0, origin);
            static_cast<void>(send(floating.view, "setFrameOrigin:", registers));
        }
        for (const auto ruler : {state->horizontal_ruler, state->vertical_ruler}) if (ruler) invalidate_view(ruler);
    }, "v@:@");
    method(sv, "_alchemyScrollerMoved:", [this, scroll_of, bar](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = scroll_of(self);
        const auto moved = s.x[2];
        if (!state->clip) return;
        const auto document = send(state->clip, "documentView").x0;
        if (!document) return;
        const auto visible = view_bounds(*view_state(state->clip));
        const auto frame = view_state(document)->frame;
        auto origin = visible.origin;
        const auto value = bar(moved)->value;
        if (moved == state->vertical) {
            const auto room = frame.size.height - visible.size.height;
            origin.y = send_bool(document, "isFlipped") ? frame.origin.y + value * room : frame.origin.y + (1 - value) * room;
        } else {
            origin.x = frame.origin.x + value * (frame.size.width - visible.size.width);
        }
        CallRegs registers;
        put_point(registers, 0, origin);
        static_cast<void>(send(state->clip, "scrollToPoint:", registers));
        static_cast<void>(send_id(self, "reflectScrolledClipView:", {state->clip}));
    }, "v@:@");
    method(sv, "scrollWheel:", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = scroll_of(self);
        if (!state->clip) return;
        const auto document = send(state->clip, "documentView").x0;
        if (!document) return;
        const auto info = event_state(s.x[2]);
        const auto factor = info->event.precise_scrolling ? 1.0 : state->line_scroll;
        const auto dy = info->event.delta_y * factor, dx = info->event.delta_x * factor;
        auto origin = view_bounds(*view_state(state->clip)).origin;
        // Positive deltas scroll toward the top / left.
        origin.y += send_bool(document, "isFlipped") ? -dy : dy;
        origin.x -= dx;
        CallRegs registers;
        put_point(registers, 0, origin);
        static_cast<void>(send(state->clip, "scrollToPoint:", registers));
        static_cast<void>(send_id(self, "reflectScrolledClipView:", {state->clip}));
    }, "v@:@");
    method(sv, "drawRect:", [this, scroll_of](CpuState& s, GuestMemory&) {
        const auto state = scroll_of(s.x[0]);
        if (state->border_type == 0) return;
        const auto bounds = view_bounds(*view_state(s.x[0]));
        auto& canvas = require_canvas("NSScrollView drawing");
        const auto color = theme_for(s.x[0]).control_border;
        canvas.fill_rect({bounds.origin, {bounds.size.width, 1}}, color);
        canvas.fill_rect({{bounds.origin.x, bounds.origin.y + bounds.size.height - 1}, {bounds.size.width, 1}}, color);
        canvas.fill_rect({bounds.origin, {1, bounds.size.height}}, color);
        canvas.fill_rect({{bounds.origin.x + bounds.size.width - 1, bounds.origin.y}, {1, bounds.size.height}}, color);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(sv, "flashScrollers", [](CpuState&, GuestMemory&) {}, "v@:");

    install_tabs_and_splits();
    install_tables(header_cell_class);
}

// ---- NSIndexSet / NSMutableIndexSet ------------------------------------------------------
void AppKit::install_index_sets() {
    if (objc.find_class("NSMutableIndexSet")) return;   // Foundation provides them
    const auto object = objc.find_class("NSObject");
    // The installer frameworks register a method-less NSIndexSet shell for
    // bind time; it becomes this implementation.
    auto set_class = objc.find_class("NSIndexSet");
    if (!set_class) set_class = objc.register_native_class("NSIndexSet", object, 8);
    const auto mutable_class = objc.register_native_class("NSMutableIndexSet", set_class, 8);
    for (const auto& [name, cls] : {std::pair<const char*, Address>{"NSIndexSet", set_class}, {"NSMutableIndexSet", mutable_class}})
        for (const auto library : {std::string_view(foundation_library), appkit_library, cocoa_library})
            objc.publish_class(name, cls, library);
    const auto values = [this](Address value) -> std::set<std::uint64_t>& { return objc.host_state<IndexSetState>(value)->values; };
    const auto make = [this, values](Address cls, std::set<std::uint64_t> contents) {
        const auto made = objc.allocate(cls);
        values(made) = std::move(contents);
        return made;
    };
    class_method(set_class, "indexSet", [this, make](CpuState& s, GuestMemory&) { s.x[0] = autorelease(make(s.x[0], {})); });
    class_method(set_class, "indexSetWithIndex:", [this, make](CpuState& s, GuestMemory&) { s.x[0] = autorelease(make(s.x[0], {s.x[2]})); }, "@@:Q");
    class_method(set_class, "indexSetWithIndexesInRange:", [this, make](CpuState& s, GuestMemory&) {
        std::set<std::uint64_t> range;
        for (Address i = 0; i < s.x[3] && i < (1u << 24); ++i) range.insert(s.x[2] + i);
        s.x[0] = autorelease(make(s.x[0], std::move(range)));
    }, "@@:{_NSRange=QQ}");
    method(set_class, "init", [values](CpuState& s, GuestMemory&) { values(s.x[0]).clear(); });
    method(set_class, "initWithIndex:", [values](CpuState& s, GuestMemory&) { values(s.x[0]) = {s.x[2]}; }, "@@:Q");
    method(set_class, "initWithIndexesInRange:", [values](CpuState& s, GuestMemory&) {
        auto& set = values(s.x[0]);
        set.clear();
        for (Address i = 0; i < s.x[3] && i < (1u << 24); ++i) set.insert(s.x[2] + i);
    }, "@@:{_NSRange=QQ}");
    method(set_class, "initWithIndexSet:", [values](CpuState& s, GuestMemory&) { values(s.x[0]) = s.x[2] ? values(s.x[2]) : std::set<std::uint64_t>{}; });
    method(set_class, "count", [values](CpuState& s, GuestMemory&) { s.x[0] = values(s.x[0]).size(); }, "Q@:");
    method(set_class, "firstIndex", [values](CpuState& s, GuestMemory&) {
        const auto& set = values(s.x[0]);
        s.x[0] = set.empty() ? not_found : *set.begin();
    }, "Q@:");
    method(set_class, "lastIndex", [values](CpuState& s, GuestMemory&) {
        const auto& set = values(s.x[0]);
        s.x[0] = set.empty() ? not_found : *set.rbegin();
    }, "Q@:");
    method(set_class, "containsIndex:", [values](CpuState& s, GuestMemory&) { return_bool(s, values(s.x[0]).contains(s.x[2])); }, "B@:Q");
    method(set_class, "containsIndexes:", [values](CpuState& s, GuestMemory&) {
        const auto& own = values(s.x[0]);
        bool all = true;
        if (s.x[2]) for (const auto v : values(s.x[2])) all = all && own.contains(v);
        return_bool(s, all);
    }, "B@:@");
    method(set_class, "indexGreaterThanIndex:", [values](CpuState& s, GuestMemory&) {
        const auto& set = values(s.x[0]);
        const auto found = set.upper_bound(s.x[2]);
        s.x[0] = found == set.end() ? not_found : *found;
    }, "Q@:Q");
    method(set_class, "indexLessThanIndex:", [values](CpuState& s, GuestMemory&) {
        const auto& set = values(s.x[0]);
        auto found = set.lower_bound(s.x[2]);
        s.x[0] = found == set.begin() ? not_found : *std::prev(found);
    }, "Q@:Q");
    method(set_class, "isEqualToIndexSet:", [values](CpuState& s, GuestMemory&) { return_bool(s, s.x[2] && values(s.x[0]) == values(s.x[2])); }, "B@:@");
    method(set_class, "isEqual:", [this, values, set_class](CpuState& s, GuestMemory&) {
        return_bool(s, s.x[2] && is_kind(s.x[2], set_class) && values(s.x[0]) == values(s.x[2]));
    }, "B@:@");
    method(set_class, "copyWithZone:", [make, values, set_class](CpuState& s, GuestMemory&) { s.x[0] = make(set_class, values(s.x[0])); });
    method(set_class, "copy", [make, values, set_class](CpuState& s, GuestMemory&) { s.x[0] = make(set_class, values(s.x[0])); });
    method(set_class, "mutableCopyWithZone:", [make, values, mutable_class](CpuState& s, GuestMemory&) { s.x[0] = make(mutable_class, values(s.x[0])); });
    method(set_class, "mutableCopy", [make, values, mutable_class](CpuState& s, GuestMemory&) { s.x[0] = make(mutable_class, values(s.x[0])); });
    method(set_class, "getIndexes:maxCount:inIndexRange:", [values](CpuState& s, GuestMemory& m) {
        const auto& set = values(s.x[0]);
        Address written = 0;
        std::uint64_t low = 0, high = ~0ULL;
        if (s.x[4]) { low = m.read64(s.x[4]); high = low + m.read64(s.x[4] + 8); }
        for (const auto v : set) {
            if (written >= s.x[3]) break;
            if (v < low || v >= high) continue;
            m.write64(s.x[2] + written * 8, v);
            ++written;
        }
        s.x[0] = written;
    }, "Q@:^QQ^{_NSRange=QQ}");
    method(set_class, "enumerateIndexesUsingBlock:", [this, values](CpuState& s, GuestMemory& m) {
        const auto copy = values(s.x[0]);
        const auto stop = process.allocate_guest(8);
        m.write8(stop, 0);
        try {
            for (const auto v : copy) {
                static_cast<void>(invoke_block(s.x[2], {v, stop}));
                if (m.read8(stop)) break;
            }
        } catch (...) { process.release_guest(stop); throw; }
        process.release_guest(stop);
    }, "v@:@?");
    method(set_class, "description", [this, values](CpuState& s, GuestMemory&) {
        std::string text_value = "<NSIndexSet>[number of indexes: " + std::to_string(values(s.x[0]).size()) + "]";
        s.x[0] = auto_string(text_value);
    });
    method(mutable_class, "addIndex:", [values](CpuState& s, GuestMemory&) { values(s.x[0]).insert(s.x[2]); }, "v@:Q");
    method(mutable_class, "removeIndex:", [values](CpuState& s, GuestMemory&) { values(s.x[0]).erase(s.x[2]); }, "v@:Q");
    method(mutable_class, "addIndexes:", [values](CpuState& s, GuestMemory&) { if (s.x[2]) { const auto other = values(s.x[2]); values(s.x[0]).insert(other.begin(), other.end()); } }, "v@:@");
    method(mutable_class, "removeIndexes:", [values](CpuState& s, GuestMemory&) { if (s.x[2]) for (const auto v : std::set(values(s.x[2]))) values(s.x[0]).erase(v); }, "v@:@");
    method(mutable_class, "removeAllIndexes", [values](CpuState& s, GuestMemory&) { values(s.x[0]).clear(); }, "v@:");
    method(mutable_class, "addIndexesInRange:", [values](CpuState& s, GuestMemory&) {
        for (Address i = 0; i < s.x[3] && i < (1u << 24); ++i) values(s.x[0]).insert(s.x[2] + i);
    }, "v@:{_NSRange=QQ}");
    method(mutable_class, "removeIndexesInRange:", [values](CpuState& s, GuestMemory&) {
        for (Address i = 0; i < s.x[3] && i < (1u << 24); ++i) values(s.x[0]).erase(s.x[2] + i);
    }, "v@:{_NSRange=QQ}");
}

// ---- NSTabView, NSSplitView ------------------------------------------------------------------
void AppKit::install_tabs_and_splits() {
    const auto tab_of = [this](Address value) { return extension<TabState>(value); };
    const auto item_of = [this](Address value) { return objc.host_state<TabItemState>(value); };
    // NSTabViewItem.
    const auto ti = tab_view_item;
    method(ti, "initWithIdentifier:", [this, item_of](CpuState& s, GuestMemory&) {
        assign(item_of(s.x[0])->identifier, s.x[2]);
        CallRegs registers;
        put_rect(registers, 0, {});
        item_of(s.x[0])->view = send(send(view, "alloc").x0, "initWithFrame:", registers).x0;
    });
    method(ti, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithIdentifier:", {0}); });
    class_method(ti, "tabViewItemWithViewController:", [this](CpuState& s, GuestMemory&) {
        const auto made = send_id(send(s.x[0], "alloc").x0, "initWithIdentifier:", {0});
        static_cast<void>(send_id(made, "setView:", {send(s.x[2], "view").x0}));
        if (responds(s.x[2], "title")) static_cast<void>(send_id(made, "setLabel:", {send(s.x[2], "title").x0}));
        s.x[0] = autorelease(made);
    });
    method(ti, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<TabItemState>(s.x[0]))
            for (auto* slot : {&state->label, &state->view, &state->identifier}) release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], tab_view_item, sel("dealloc")));
    }, "v@:");
    method(ti, "label", [this, item_of](CpuState& s, GuestMemory&) { const auto l = item_of(s.x[0])->label; s.x[0] = l ? l : auto_string(""); });
    method(ti, "setLabel:", [this, item_of](CpuState& s, GuestMemory&) {
        const auto state = item_of(s.x[0]);
        release(std::exchange(state->label, s.x[2] ? cf.copy(s.x[2]) : 0));
        if (state->tab_view) invalidate_view(state->tab_view);
    }, "v@:@");
    method(ti, "identifier", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->identifier; });
    method(ti, "setIdentifier:", [this, item_of](CpuState& s, GuestMemory&) { assign(item_of(s.x[0])->identifier, s.x[2]); }, "v@:@");
    method(ti, "view", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->view; });
    method(ti, "setView:", [this, item_of, tab_of](CpuState& s, GuestMemory&) {
        const auto state = item_of(s.x[0]);
        const auto owner = state->tab_view;
        const bool shown = owner && state->view && view_state(state->view)->superview == owner;
        if (shown) static_cast<void>(send(state->view, "removeFromSuperview"));
        assign(state->view, s.x[2]);
        if (shown && s.x[2]) {
            static_cast<void>(send_id(owner, "addSubview:", {s.x[2]}));
            CallRegs registers;
            put_rect(registers, 0, result_rect(send(owner, "contentRect")));
            static_cast<void>(send(s.x[2], "setFrame:", registers));
        }
        static_cast<void>(tab_of);
    }, "v@:@");
    method(ti, "tabView", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->tab_view; });
    method(ti, "tabState", [this, item_of, tab_of](CpuState& s, GuestMemory&) {
        const auto owner = item_of(s.x[0])->tab_view;
        if (!owner) { s.x[0] = 2; return; }   // NSBackgroundTab
        const auto state = tab_of(owner);
        const auto& list = state->items;
        const auto at = std::find(list.begin(), list.end(), s.x[0]);
        s.x[0] = at != list.end() && at - list.begin() == state->selected ? 0 : 1;   // NSSelectedTab / NSBackgroundTab
    }, "Q@:");
    method(ti, "initialFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(ti, "setInitialFirstResponder:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(ti, "setToolTip:", [](CpuState&, GuestMemory&) {}, "v@:@");

    // NSTabView.
    const auto tv = tab_view;
    const auto has_tabs = [tab_of](Address self) { const auto type = tab_of(self)->type; return type == 0 || type == 1 || type == 2 || type == 3; };
    method(tv, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(tv, "contentRect", [this, has_tabs, tab_of](CpuState& s, GuestMemory&) {
        const auto bounds = view_bounds(*view_state(s.x[0]));
        if (!has_tabs(s.x[0])) { return_rect(s, tab_of(s.x[0])->type == 6 ? bounds : inset(bounds, 1, 1)); return; }
        return_rect(s, {{bounds.origin.x + 7, bounds.origin.y + 22}, {std::max(0.0, bounds.size.width - 14), std::max(0.0, bounds.size.height - 29)}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    const auto tab_rects = [this, tab_of](Address self) {
        const auto state = tab_of(self);
        const auto bounds = view_bounds(*view_state(self));
        TextStyle style;
        style.font = font_spec(state->font);
        std::vector<Rect> rects;
        double total{};
        std::vector<double> widths;
        for (const auto item : state->items) {
            const auto w = std::ceil(drawing::text_width(style.font, string16(send(item, "label").x0))) + 24;
            widths.push_back(w);
            total += w;
        }
        double x = bounds.origin.x + std::max(0.0, (bounds.size.width - total) / 2);
        for (const auto w : widths) { rects.push_back({{x, bounds.origin.y + 1}, {w, 22}}); x += w; }
        return rects;
    };
    const auto select_index = [this, tab_of, item_of](Address self, std::int64_t index) {
        const auto state = tab_of(self);
        if (index < 0 || index >= static_cast<std::int64_t>(state->items.size()) || index == state->selected) return;
        const auto item = state->items[static_cast<std::size_t>(index)];
        if (state->delegate) {
            CallRegs ask;
            ask.x[2] = self; ask.x[3] = item;
            if (const auto answer = send_optional(state->delegate, "tabView:shouldSelectTabViewItem:", ask); answer && !(answer->x0 & 0xff)) return;
            static_cast<void>(send_optional(state->delegate, "tabView:willSelectTabViewItem:", ask));
        }
        if (state->selected >= 0 && state->selected < static_cast<std::int64_t>(state->items.size()))
            if (const auto old = item_of(state->items[static_cast<std::size_t>(state->selected)])->view)
                if (view_state(old)->superview == self) static_cast<void>(send(old, "removeFromSuperview"));
        state->selected = index;
        if (const auto shown = item_of(item)->view) {
            static_cast<void>(send_id(self, "addSubview:", {shown}));
            CallRegs registers;
            put_rect(registers, 0, result_rect(send(self, "contentRect")));
            static_cast<void>(send(shown, "setFrame:", registers));
        }
        invalidate_view(self);
        if (state->delegate) {
            CallRegs told;
            told.x[2] = self; told.x[3] = item;
            static_cast<void>(send_optional(state->delegate, "tabView:didSelectTabViewItem:", told));
        }
    };
    method(tv, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = find_extension<TabState>(s.x[0])) {
            for (const auto item : std::exchange(state->items, {})) { objc.host_state<TabItemState>(item)->tab_view = 0; release(item); }
            release(std::exchange(state->font, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], tab_view, sel("dealloc")));
    }, "v@:");
    method(tv, "addTabViewItem:", [this](CpuState& s, GuestMemory&) {
        const auto count = send(s.x[0], "numberOfTabViewItems").x0;
        static_cast<void>(send_id(s.x[0], "insertTabViewItem:atIndex:", {s.x[2], count}));
    }, "v@:@");
    method(tv, "insertTabViewItem:atIndex:", [this, tab_of, item_of, select_index](CpuState& s, GuestMemory&) {
        const auto state = tab_of(s.x[0]);
        if (!s.x[2]) return;
        const auto index = std::min<std::size_t>(s.x[3], state->items.size());
        state->items.insert(state->items.begin() + static_cast<std::ptrdiff_t>(index), retain(s.x[2]));
        item_of(s.x[2])->tab_view = s.x[0];
        if (state->selected >= static_cast<std::int64_t>(index) && state->selected >= 0) ++state->selected;
        if (state->selected < 0) select_index(s.x[0], 0);
        invalidate_view(s.x[0]);
        if (state->delegate) {
            CallRegs told;
            told.x[2] = s.x[0];
            static_cast<void>(send_optional(state->delegate, "tabViewDidChangeNumberOfTabViewItems:", told));
        }
    }, "v@:@q");
    method(tv, "removeTabViewItem:", [this, tab_of, item_of, select_index](CpuState& s, GuestMemory&) {
        const auto state = tab_of(s.x[0]);
        const auto found = std::find(state->items.begin(), state->items.end(), s.x[2]);
        if (found == state->items.end()) return;
        const auto index = found - state->items.begin();
        if (const auto shown = item_of(s.x[2])->view) if (view_state(shown)->superview == s.x[0]) static_cast<void>(send(shown, "removeFromSuperview"));
        state->items.erase(found);
        item_of(s.x[2])->tab_view = 0;
        const bool was_selected = index == state->selected;
        if (index < state->selected) --state->selected;
        if (was_selected) { state->selected = -1; select_index(s.x[0], std::min<std::int64_t>(index, static_cast<std::int64_t>(state->items.size()) - 1)); }
        release(s.x[2]);
        invalidate_view(s.x[0]);
    }, "v@:@");
    method(tv, "tabViewItems", [this, tab_of](CpuState& s, GuestMemory&) { s.x[0] = auto_array(tab_of(s.x[0])->items); });
    // Replaces every item (the first new one is selected, as on insertion).
    method(tv, "setTabViewItems:", [this, tab_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto wanted = items(s.x[2]);
        for (const auto value : wanted) static_cast<void>(retain(value));   // they may be the current items
        for (const auto old : std::vector<Address>(tab_of(self)->items)) static_cast<void>(send_id(self, "removeTabViewItem:", {old}));
        for (const auto value : wanted) {
            static_cast<void>(send_id(self, "addTabViewItem:", {value}));
            release(value);
        }
    }, "v@:@");
    method(tv, "controlSize", [tab_of](CpuState& s, GuestMemory&) { s.x[0] = tab_of(s.x[0])->control_size; }, "Q@:");
    method(tv, "setControlSize:", [this, tab_of](CpuState& s, GuestMemory&) {
        tab_of(s.x[0])->control_size = s.x[2];
        invalidate_view(s.x[0]);
    }, "v@:Q");
    method(tv, "numberOfTabViewItems", [tab_of](CpuState& s, GuestMemory&) { s.x[0] = tab_of(s.x[0])->items.size(); }, "q@:");
    method(tv, "tabViewItemAtIndex:", [tab_of](CpuState& s, GuestMemory&) {
        const auto& list = tab_of(s.x[0])->items;
        s.x[0] = s.x[2] < list.size() ? list[s.x[2]] : 0;
    }, "@@:q");
    method(tv, "indexOfTabViewItem:", [tab_of](CpuState& s, GuestMemory&) {
        const auto& list = tab_of(s.x[0])->items;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        s.x[0] = found == list.end() ? not_found : static_cast<Address>(found - list.begin());
    }, "q@:@");
    method(tv, "indexOfTabViewItemWithIdentifier:", [this, tab_of, item_of](CpuState& s, GuestMemory&) {
        const auto& list = tab_of(s.x[0])->items;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto id = item_of(list[i])->identifier;
            if (id == s.x[2] || (id && s.x[2] && send_bool(id, "isEqual:", {s.x[2]}))) { s.x[0] = i; return; }
        }
        s.x[0] = not_found;
    }, "q@:@");
    method(tv, "selectedTabViewItem", [tab_of](CpuState& s, GuestMemory&) {
        const auto state = tab_of(s.x[0]);
        s.x[0] = state->selected >= 0 && state->selected < static_cast<std::int64_t>(state->items.size()) ? state->items[static_cast<std::size_t>(state->selected)] : 0;
    });
    method(tv, "selectTabViewItemAtIndex:", [select_index](CpuState& s, GuestMemory&) { select_index(s.x[0], static_cast<std::int64_t>(s.x[2])); }, "v@:q");
    method(tv, "selectTabViewItem:", [tab_of, select_index](CpuState& s, GuestMemory&) {
        const auto& list = tab_of(s.x[0])->items;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found != list.end()) select_index(s.x[0], found - list.begin());
    }, "v@:@");
    method(tv, "selectTabViewItemWithIdentifier:", [this, select_index](CpuState& s, GuestMemory&) {
        const auto index = send_id(s.x[0], "indexOfTabViewItemWithIdentifier:", {s.x[2]});
        if (index != not_found) select_index(s.x[0], static_cast<std::int64_t>(index));
    }, "v@:@");
    method(tv, "selectFirstTabViewItem:", [select_index](CpuState& s, GuestMemory&) { select_index(s.x[0], 0); }, "v@:@");
    method(tv, "selectNextTabViewItem:", [tab_of, select_index](CpuState& s, GuestMemory&) { select_index(s.x[0], tab_of(s.x[0])->selected + 1); }, "v@:@");
    method(tv, "selectPreviousTabViewItem:", [tab_of, select_index](CpuState& s, GuestMemory&) { select_index(s.x[0], tab_of(s.x[0])->selected - 1); }, "v@:@");
    method(tv, "delegate", [tab_of](CpuState& s, GuestMemory&) { s.x[0] = tab_of(s.x[0])->delegate; });
    method(tv, "setDelegate:", [tab_of](CpuState& s, GuestMemory&) { tab_of(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(tv, "tabViewType", [tab_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(tab_of(s.x[0])->type); }, "Q@:");
    method(tv, "setTabViewType:", [this, tab_of](CpuState& s, GuestMemory&) {
        tab_of(s.x[0])->type = static_cast<std::int64_t>(s.x[2]);
        static_cast<void>(send(s.x[0], "_alchemyLayoutTab"));
    }, "v@:Q");
    method(tv, "font", [tab_of](CpuState& s, GuestMemory&) { s.x[0] = tab_of(s.x[0])->font; });
    method(tv, "setFont:", [this, tab_of](CpuState& s, GuestMemory&) { assign(tab_of(s.x[0])->font, s.x[2]); invalidate_view(s.x[0]); }, "v@:@");
    method(tv, "drawsBackground", [tab_of](CpuState& s, GuestMemory&) { return_bool(s, tab_of(s.x[0])->draws_background); }, "B@:");
    method(tv, "setDrawsBackground:", [tab_of](CpuState& s, GuestMemory&) { tab_of(s.x[0])->draws_background = bool_arg(s, 2); }, "v@:B");
    method(tv, "setAllowsTruncatedLabels:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(tv, "minimumSize", [](CpuState& s, GuestMemory&) { return_size(s, {40, 40}); }, "{CGSize=dd}@:");
    method(tv, "_alchemyLayoutTab", [this, tab_of, item_of](CpuState& s, GuestMemory&) {
        const auto state = tab_of(s.x[0]);
        if (state->selected < 0 || state->selected >= static_cast<std::int64_t>(state->items.size())) return;
        if (const auto shown = item_of(state->items[static_cast<std::size_t>(state->selected)])->view) {
            CallRegs registers;
            put_rect(registers, 0, result_rect(send(s.x[0], "contentRect")));
            static_cast<void>(send(shown, "setFrame:", registers));
        }
        invalidate_view(s.x[0]);
    }, "v@:");
    method(tv, "resizeSubviewsWithOldSize:", [this](CpuState& s, GuestMemory&) { static_cast<void>(send(s.x[0], "_alchemyLayoutTab")); }, "v@:{CGSize=dd}");
    method(tv, "tabViewItemAtPoint:", [this, tab_rects, tab_of](CpuState& s, GuestMemory&) {
        const auto rects = tab_rects(s.x[0]);
        const auto point = point_arg(s);
        for (std::size_t i = 0; i < rects.size(); ++i) if (rect_contains(rects[i], point)) { s.x[0] = tab_of(s.x[0])->items[i]; return; }
        s.x[0] = 0;
    }, "@@:{CGPoint=dd}");
    method(tv, "mouseDown:", [this, has_tabs, tab_rects, select_index](CpuState& s, GuestMemory&) {
        if (!has_tabs(s.x[0])) return;
        const auto point = event_location_in(*this, s.x[0], s.x[2]);
        const auto rects = tab_rects(s.x[0]);
        for (std::size_t i = 0; i < rects.size(); ++i) if (rect_contains(rects[i], point)) { select_index(s.x[0], static_cast<std::int64_t>(i)); return; }
    }, "v@:@");
    method(tv, "drawRect:", [this, has_tabs, tab_rects, tab_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = tab_of(self);
        const auto& th = theme_for(self);
        auto& canvas = require_canvas("NSTabView drawing");
        const auto bounds = view_bounds(*view_state(self));
        if (state->type == 6) return;   // NSNoTabsNoBorder
        const Rect frame = has_tabs(self) ? Rect{{bounds.origin.x, bounds.origin.y + 12}, {bounds.size.width, std::max(0.0, bounds.size.height - 12)}} : bounds;
        draw_bezel(*this, frame, 5, th.dark ? Color::gray(1, 0.04) : Color::gray(0, 0.03), th.separator);
        if (!has_tabs(self)) return;
        const auto rects = tab_rects(self);
        if (rects.empty()) return;
        const Rect strip{rects.front().origin, {rects.back().origin.x + rects.back().size.width - rects.front().origin.x, rects.front().size.height}};
        draw_bezel(*this, strip, 5, th.control_face, th.control_border);
        TextStyle style;
        style.font = font_spec(state->font);
        for (std::size_t i = 0; i < rects.size(); ++i) {
            const bool selected = static_cast<std::int64_t>(i) == state->selected;
            if (selected) {
                drawing::Path path;
                path.add_rounded_rect(inset(rects[i], 1, 1), 4, 4);
                canvas.fill_path(path, th.accent);
            }
            style.color = selected ? th.accent_text : th.text;
            draw_line_in(*this, string16(send(state->items[i], "label").x0), style, rects[i], 2);
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // NSSplitView.
    const auto sp = split_view;
    const auto split_of = [this](Address value) { return extension<SplitState>(value); };
    const auto thickness = [split_of](Address self) { return split_of(self)->divider_style == 2 ? 1.0 : split_of(self)->divider_style == 3 ? 10.0 : 9.0; };
    method(sp, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(sp, "isVertical", [split_of](CpuState& s, GuestMemory&) { return_bool(s, split_of(s.x[0])->vertical); }, "B@:");
    method(sp, "setVertical:", [this, split_of](CpuState& s, GuestMemory&) { split_of(s.x[0])->vertical = bool_arg(s, 2); static_cast<void>(send(s.x[0], "adjustSubviews")); }, "v@:B");
    method(sp, "dividerStyle", [split_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(split_of(s.x[0])->divider_style); }, "q@:");
    method(sp, "setDividerStyle:", [this, split_of](CpuState& s, GuestMemory&) {
        split_of(s.x[0])->divider_style = static_cast<std::int64_t>(s.x[2]);
        static_cast<void>(send(s.x[0], "adjustSubviews"));
    }, "v@:q");
    method(sp, "dividerThickness", [thickness](CpuState& s, GuestMemory&) { set_d(s, 0, thickness(s.x[0])); }, "d@:");
    method(sp, "delegate", [split_of](CpuState& s, GuestMemory&) { s.x[0] = split_of(s.x[0])->delegate; });
    method(sp, "setDelegate:", [split_of](CpuState& s, GuestMemory&) { split_of(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(sp, "arrangedSubviews", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array(view_state(s.x[0])->subviews); });
    method(sp, "addArrangedSubview:", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(s.x[0], "addSubview:", {s.x[2]}));
        static_cast<void>(send(s.x[0], "adjustSubviews"));
    }, "v@:@");
    // Every subview is arranged (arrangesAllSubviews), so arranging is
    // ordering the subviews.
    method(sp, "insertArrangedSubview:atIndex:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], child = s.x[2];
        const auto& panes = view_state(self)->subviews;
        if (s.x[3] > panes.size()) fail("-[NSSplitView insertArrangedSubview:atIndex:] index beyond the arranged subviews (NSRangeException)");
        if (s.x[3] == panes.size()) {
            static_cast<void>(send_id(self, "addSubview:", {child}));
        } else {
            constexpr Address below = static_cast<Address>(-1);   // NSWindowBelow
            static_cast<void>(send_id(self, "addSubview:positioned:relativeTo:", {child, below, panes[s.x[3]]}));
        }
        static_cast<void>(send(self, "adjustSubviews"));
    }, "v@:@q");
    method(sp, "removeArrangedSubview:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2] || view_state(s.x[2])->superview != s.x[0]) return;
        static_cast<void>(send(s.x[2], "removeFromSuperview"));
        static_cast<void>(send(s.x[0], "adjustSubviews"));
    }, "v@:@");
    method(sp, "setAutosaveName:", [this, split_of](CpuState& s, GuestMemory&) { assign(split_of(s.x[0])->autosave_name, s.x[2]); }, "v@:@");
    method(sp, "autosaveName", [split_of](CpuState& s, GuestMemory&) { s.x[0] = split_of(s.x[0])->autosave_name; });
    method(sp, "setArrangesAllSubviews:", [](CpuState& s, GuestMemory&) {
        if (!bool_arg(s, 2)) unsupported("-[NSSplitView setArrangesAllSubviews:NO]");
    }, "v@:B");
    method(sp, "isSubviewCollapsed:", [this](CpuState& s, GuestMemory&) {
        const auto size = view_state(s.x[2])->frame.size;
        return_bool(s, size.width <= 0 || size.height <= 0);
    }, "B@:@");
    // Panes share the length along the split axis in proportion to their
    // current sizes (equally when they have none yet).
    method(sp, "adjustSubviews", [this, split_of, thickness](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto panes = view_state(self)->subviews;
        if (panes.empty()) return;
        const auto vertical = split_of(self)->vertical;
        const auto bounds = view_bounds(*view_state(self));
        const auto total = (vertical ? bounds.size.width : bounds.size.height) - thickness(self) * static_cast<double>(panes.size() - 1);
        double current{};
        for (const auto pane : panes) current += vertical ? view_state(pane)->frame.size.width : view_state(pane)->frame.size.height;
        double offset = vertical ? bounds.origin.x : bounds.origin.y;
        for (std::size_t i = 0; i < panes.size(); ++i) {
            const auto own = vertical ? view_state(panes[i])->frame.size.width : view_state(panes[i])->frame.size.height;
            auto length = current > 0 ? total * own / current : total / static_cast<double>(panes.size());
            if (i + 1 == panes.size()) length = std::max(0.0, (vertical ? bounds.origin.x + bounds.size.width : bounds.origin.y + bounds.size.height) - offset);
            const Rect frame = vertical ? Rect{{offset, bounds.origin.y}, {std::max(0.0, length), bounds.size.height}}
                                        : Rect{{bounds.origin.x, offset}, {bounds.size.width, std::max(0.0, length)}};
            CallRegs registers;
            put_rect(registers, 0, frame);
            static_cast<void>(send(panes[i], "setFrame:", registers));
            offset += length + thickness(self);
        }
        invalidate_view(self);
    }, "v@:");
    // Holding priorities (by subview index): when the split view resizes,
    // the change goes to the panes of the lowest priority (in proportion to
    // their sizes), then to the next priority once those reach zero.
    method(sp, "holdingPriorityForSubviewAtIndex:", [split_of](CpuState& s, GuestMemory&) {
        const auto& list = split_of(s.x[0])->holding;
        set_f(s, 0, s.x[2] < list.size() ? list[s.x[2]] : 250.0f);
    }, "f@:q");
    method(sp, "setHoldingPriority:forSubviewAtIndex:", [split_of](CpuState& s, GuestMemory&) {
        auto& list = split_of(s.x[0])->holding;
        if (s.x[2] >= list.size()) list.resize(s.x[2] + 1, 250.0f);
        list[s.x[2]] = f(s, 0);
    }, "v@:fq");
    method(sp, "resizeSubviewsWithOldSize:", [this, split_of, thickness](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (const auto delegate = split_of(self)->delegate; delegate && responds(delegate, "splitView:resizeSubviewsWithOldSize:")) {
            CallRegs registers;
            registers.x[2] = self;
            put_size(registers, 0, size_arg(s));
            static_cast<void>(send(delegate, "splitView:resizeSubviewsWithOldSize:", registers));
            return;
        }
        const auto panes = view_state(self)->subviews;
        const auto state = split_of(self);
        const auto vertical = state->vertical;
        std::vector<double> sizes;
        double current{};
        for (const auto pane : panes) {
            sizes.push_back(vertical ? view_state(pane)->frame.size.width : view_state(pane)->frame.size.height);
            current += sizes.back();
        }
        if (panes.empty() || !(current > 0)) { static_cast<void>(send(self, "adjustSubviews")); return; }
        const auto bounds = view_bounds(*view_state(self));
        double change = (vertical ? bounds.size.width : bounds.size.height) - thickness(self) * static_cast<double>(panes.size() - 1) - current;
        std::vector<float> levels;
        for (std::size_t i = 0; i < panes.size(); ++i) levels.push_back(i < state->holding.size() ? state->holding[i] : 250.0f);
        std::vector<float> order = levels;
        std::sort(order.begin(), order.end());
        order.erase(std::unique(order.begin(), order.end()), order.end());
        for (const auto level : order) {
            if (std::abs(change) < 1e-9) break;
            double group{};
            for (std::size_t i = 0; i < panes.size(); ++i) if (levels[i] == level) group += sizes[i];
            if (!(group > 0)) continue;   // collapsed panes stay collapsed
            double used{};
            for (std::size_t i = 0; i < panes.size(); ++i) {
                if (levels[i] != level) continue;
                const auto next = std::max(0.0, sizes[i] + change * sizes[i] / group);
                used += next - sizes[i];
                sizes[i] = next;
            }
            change -= used;
        }
        double offset = vertical ? bounds.origin.x : bounds.origin.y;
        for (std::size_t i = 0; i < panes.size(); ++i) {
            const Rect frame = vertical ? Rect{{offset, bounds.origin.y}, {sizes[i], bounds.size.height}}
                                        : Rect{{bounds.origin.x, offset}, {bounds.size.width, sizes[i]}};
            CallRegs registers;
            put_rect(registers, 0, frame);
            static_cast<void>(send(panes[i], "setFrame:", registers));
            offset += sizes[i] + thickness(self);
        }
        invalidate_view(self);
    }, "v@:{CGSize=dd}");
    method(sp, "arrangesAllSubviews", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(sp, "isSpringLoaded", [split_of](CpuState& s, GuestMemory&) { return_bool(s, split_of(s.x[0])->spring_loaded); }, "B@:");
    method(sp, "setSpringLoaded:", [split_of](CpuState& s, GuestMemory&) { split_of(s.x[0])->spring_loaded = bool_arg(s, 2); }, "v@:B");
    // The color dividers are drawn in (-drawDividerInRect:).
    method(sp, "dividerColor", [this, split_of](CpuState& s, GuestMemory&) {
        const auto color_class = objc.find_class("NSColor");
        if (split_of(s.x[0])->divider_style == 2) { s.x[0] = send(color_class, "separatorColor").x0; return; }
        CallRegs registers;
        registers.set_d(0, theme_for(s.x[0]).dark ? 0.15 : 0.9);
        registers.set_d(1, 1.0);
        s.x[0] = send(color_class, "colorWithWhite:alpha:", registers).x0;
    });
    method(sp, "setPosition:ofDividerAtIndex:", [this, split_of, thickness](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto panes = view_state(self)->subviews;
        const auto index = s.x[2];
        if (index + 1 >= panes.size()) return;
        const auto vertical = split_of(self)->vertical;
        auto position = d(s, 0);
        auto first = view_state(panes[index])->frame, second = view_state(panes[index + 1])->frame;
        const auto start = vertical ? first.origin.x : first.origin.y;
        const auto end = vertical ? second.origin.x + second.size.width : second.origin.y + second.size.height;
        position = std::clamp(position, start, end - thickness(self));
        if (vertical) {
            first.size.width = position - start;
            second.origin.x = position + thickness(self);
            second.size.width = end - second.origin.x;
        } else {
            first.size.height = position - start;
            second.origin.y = position + thickness(self);
            second.size.height = end - second.origin.y;
        }
        CallRegs a, b;
        put_rect(a, 0, first);
        put_rect(b, 0, second);
        static_cast<void>(send(panes[index], "setFrame:", a));
        static_cast<void>(send(panes[index + 1], "setFrame:", b));
        invalidate_view(self);
    }, "v@:dq");
    method(sp, "minPossiblePositionOfDividerAtIndex:", [](CpuState& s, GuestMemory&) { set_d(s, 0, 0); }, "d@:q");
    method(sp, "maxPossiblePositionOfDividerAtIndex:", [this](CpuState& s, GuestMemory&) {
        const auto bounds = view_bounds(*view_state(s.x[0]));
        set_d(s, 0, std::max(bounds.size.width, bounds.size.height));
    }, "d@:q");
    method(sp, "drawDividerInRect:", [this, split_of](CpuState& s, GuestMemory&) {
        const auto& th = theme_for(s.x[0]);
        auto& canvas = require_canvas("NSSplitView drawing");
        const auto rect = rect_arg(s);
        if (split_of(s.x[0])->divider_style == 2) { canvas.fill_rect(rect, th.separator); return; }
        canvas.fill_rect(rect, th.dark ? Color::gray(0.15) : Color::gray(0.9));
        const Point center{rect.origin.x + rect.size.width / 2, rect.origin.y + rect.size.height / 2};
        drawing::Path dot;
        dot.add_oval({{center.x - 2, center.y - 2}, {4, 4}});
        canvas.fill_path(dot, th.secondary_text);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto divider_rects = [this, split_of, thickness](Address self) {
        std::vector<Rect> rects;
        const auto panes = view_state(self)->subviews;
        const auto vertical = split_of(self)->vertical;
        const auto bounds = view_bounds(*view_state(self));
        for (std::size_t i = 0; i + 1 < panes.size(); ++i) {
            const auto frame = view_state(panes[i])->frame;
            rects.push_back(vertical ? Rect{{frame.origin.x + frame.size.width, bounds.origin.y}, {thickness(self), bounds.size.height}}
                                     : Rect{{bounds.origin.x, frame.origin.y + frame.size.height}, {bounds.size.width, thickness(self)}});
        }
        return rects;
    };
    method(sp, "drawRect:", [this, divider_rects](CpuState& s, GuestMemory&) {
        for (const auto& rect : divider_rects(s.x[0])) {
            CallRegs registers;
            put_rect(registers, 0, rect);
            static_cast<void>(send(s.x[0], "drawDividerInRect:", registers));
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(sp, "mouseDown:", [this, split_of, divider_rects, thickness](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto start = event_location_in(*this, self, s.x[2]);
        const auto rects = divider_rects(self);
        for (std::size_t i = 0; i < rects.size(); ++i) {
            if (!rect_contains(inset(rects[i], -2, -2), start)) continue;
            const auto vertical = split_of(self)->vertical;
            static_cast<void>(track_mouse(*this, self, s.x[2], [](bool) {}, [&](Point at) {
                // The delegate's limits on the divider (-splitView:constrainMinCoordinate:ofSubviewAt: and the like).
                auto position = vertical ? at.x : at.y;
                if (const auto delegate = split_of(self)->delegate) {
                    const auto panes = view_state(self)->subviews;
                    const auto first = view_state(panes[i])->frame, second = view_state(panes[i + 1])->frame;
                    const auto ask = [&](const char* selector, double proposed) {
                        if (!responds(delegate, selector)) return proposed;
                        CallRegs told;
                        told.x[2] = self;
                        told.set_d(0, proposed);
                        told.x[3] = i;
                        return send(delegate, selector, told).d(0);
                    };
                    const auto low = ask("splitView:constrainMinCoordinate:ofSubviewAt:", vertical ? first.origin.x : first.origin.y);
                    const auto high = ask("splitView:constrainMaxCoordinate:ofSubviewAt:",
                                          (vertical ? second.origin.x + second.size.width : second.origin.y + second.size.height) - thickness(self));
                    position = ask("splitView:constrainSplitPosition:ofSubviewAt:", std::max(low, std::min(position, high)));
                }
                CallRegs registers;
                registers.set_d(0, position);
                registers.x[2] = i;
                static_cast<void>(send(self, "setPosition:ofDividerAtIndex:", registers));
                if (const auto delegate = split_of(self)->delegate) {
                    CallRegs told;
                    told.x[2] = notification("NSSplitViewDidResizeSubviewsNotification", self);
                    static_cast<void>(send_optional(delegate, "splitViewDidResizeSubviews:", told));
                }
            }));
            return;
        }
    }, "v@:@");
}

// ---- NSTableView / NSOutlineView ---------------------------------------------------------------
void AppKit::install_tables(Address header_cell_class) {
    const auto table_of = [this](Address value) {
        const auto state = view_state(value);
        if (!state->extension) state->extension = std::make_shared<TableState>();
        const auto typed = std::dynamic_pointer_cast<TableState>(state->extension);
        if (!typed) fail("NSTableView state of another type");
        return typed;
    };
    const auto column_of = [this](Address value) { return objc.host_state<ColumnState>(value); };

    // NSTableColumn.
    const auto tc = table_column;
    method(tc, "initWithIdentifier:", [this, column_of, header_cell_class](CpuState& s, GuestMemory&) {
        const auto state = column_of(s.x[0]);
        assign(state->identifier, s.x[2]);
        state->header_cell = send_id(send(header_cell_class, "alloc").x0, "initTextCell:", {auto_string("")});
        state->data_cell = send_id(send(text_field_cell, "alloc").x0, "initTextCell:", {auto_string("")});
        const auto data = cell_state(*this, state->data_cell);
        data->bezeled = data->draws_background = data->editable = false;
        const auto header = cell_state(*this, state->header_cell);
        header->bezeled = header->draws_background = header->editable = header->selectable = false;
    });
    method(tc, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithIdentifier:", {auto_string("")}); });
    method(tc, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ColumnState>(s.x[0]))
            for (auto* slot : {&state->identifier, &state->header_cell, &state->data_cell, &state->header_tool_tip, &state->sort_prototype})
                release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], table_column, sel("dealloc")));
    }, "v@:");
    method(tc, "identifier", [column_of](CpuState& s, GuestMemory&) { s.x[0] = column_of(s.x[0])->identifier; });
    method(tc, "setIdentifier:", [this, column_of](CpuState& s, GuestMemory&) { assign(column_of(s.x[0])->identifier, s.x[2]); }, "v@:@");
    method(tc, "tableView", [column_of](CpuState& s, GuestMemory&) { s.x[0] = column_of(s.x[0])->table; });
    method(tc, "setTableView:", [column_of](CpuState& s, GuestMemory&) { column_of(s.x[0])->table = s.x[2]; }, "v@:@");
    const auto column_real = [this, column_of](const char* getter, const char* setter, double ColumnState::*member) {
        method(table_column, getter, [column_of, member](CpuState& s, GuestMemory&) { set_d(s, 0, (*column_of(s.x[0])).*member); }, "d@:");
        method(table_column, setter, [this, column_of, member](CpuState& s, GuestMemory&) {
            const auto state = column_of(s.x[0]);
            (*state).*member = d(s, 0);
            state->width = std::clamp(state->width, state->min_width, state->max_width);
            if (state->table) static_cast<void>(send(state->table, "tile"));
        }, "v@:d");
    };
    column_real("width", "setWidth:", &ColumnState::width);
    column_real("minWidth", "setMinWidth:", &ColumnState::min_width);
    column_real("maxWidth", "setMaxWidth:", &ColumnState::max_width);
    method(tc, "headerCell", [column_of](CpuState& s, GuestMemory&) { s.x[0] = column_of(s.x[0])->header_cell; });
    method(tc, "setHeaderCell:", [this, column_of](CpuState& s, GuestMemory&) { assign(column_of(s.x[0])->header_cell, s.x[2]); }, "v@:@");
    method(tc, "dataCell", [column_of](CpuState& s, GuestMemory&) { s.x[0] = column_of(s.x[0])->data_cell; });
    method(tc, "setDataCell:", [this, column_of](CpuState& s, GuestMemory&) { assign(column_of(s.x[0])->data_cell, s.x[2]); }, "v@:@");
    method(tc, "dataCellForRow:", [this](CpuState& s, GuestMemory&) { s.x[0] = send(s.x[0], "dataCell").x0; }, "@@:q");
    method(tc, "title", [this, column_of](CpuState& s, GuestMemory&) { s.x[0] = send(column_of(s.x[0])->header_cell, "stringValue").x0; });
    method(tc, "setTitle:", [this, column_of](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(column_of(s.x[0])->header_cell, "setStringValue:", {s.x[2]}));
    }, "v@:@");
    method(tc, "isHidden", [column_of](CpuState& s, GuestMemory&) { return_bool(s, column_of(s.x[0])->hidden); }, "B@:");
    method(tc, "setHidden:", [this, column_of](CpuState& s, GuestMemory&) {
        column_of(s.x[0])->hidden = bool_arg(s, 2);
        if (const auto table = column_of(s.x[0])->table) static_cast<void>(send(table, "tile"));
    }, "v@:B");
    method(tc, "isEditable", [column_of](CpuState& s, GuestMemory&) { return_bool(s, column_of(s.x[0])->editable); }, "B@:");
    method(tc, "setEditable:", [column_of](CpuState& s, GuestMemory&) { column_of(s.x[0])->editable = bool_arg(s, 2); }, "v@:B");
    method(tc, "resizingMask", [column_of](CpuState& s, GuestMemory&) { s.x[0] = column_of(s.x[0])->resizing_mask; }, "Q@:");
    method(tc, "setResizingMask:", [column_of](CpuState& s, GuestMemory&) { column_of(s.x[0])->resizing_mask = s.x[2]; }, "v@:Q");
    method(tc, "headerToolTip", [column_of](CpuState& s, GuestMemory&) { s.x[0] = column_of(s.x[0])->header_tool_tip; });
    method(tc, "setHeaderToolTip:", [this, column_of](CpuState& s, GuestMemory&) {
        release(std::exchange(column_of(s.x[0])->header_tool_tip, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(tc, "sortDescriptorPrototype", [column_of](CpuState& s, GuestMemory&) { s.x[0] = column_of(s.x[0])->sort_prototype; });
    method(tc, "setSortDescriptorPrototype:", [this, column_of](CpuState& s, GuestMemory&) {
        release(std::exchange(column_of(s.x[0])->sort_prototype, s.x[2] ? send(s.x[2], "copy").x0 : 0));
    }, "v@:@");
    method(tc, "sizeToFit", [](CpuState&, GuestMemory&) {}, "v@:");

    // NSTableCellView.
    const auto cv = table_cell_view;
    struct CellViewSlots final : ObjectPayload { Address text_field{}, image_view{}, object_value{}; };
    const auto slots = [this](Address value) {
        const auto state = view_state(value);
        if (!state->extension) state->extension = std::make_shared<CellViewSlots>();
        const auto typed = std::dynamic_pointer_cast<CellViewSlots>(state->extension);
        if (!typed) fail("NSTableCellView state of another type");
        return typed;
    };
    method(cv, "textField", [slots](CpuState& s, GuestMemory&) { s.x[0] = slots(s.x[0])->text_field; });
    method(cv, "setTextField:", [slots](CpuState& s, GuestMemory&) { slots(s.x[0])->text_field = s.x[2]; }, "v@:@");
    method(cv, "imageView", [slots](CpuState& s, GuestMemory&) { s.x[0] = slots(s.x[0])->image_view; });
    method(cv, "setImageView:", [slots](CpuState& s, GuestMemory&) { slots(s.x[0])->image_view = s.x[2]; }, "v@:@");
    method(cv, "objectValue", [slots](CpuState& s, GuestMemory&) { s.x[0] = slots(s.x[0])->object_value; });
    method(cv, "setObjectValue:", [this, slots](CpuState& s, GuestMemory&) { assign(slots(s.x[0])->object_value, s.x[2]); }, "v@:@");
    method(cv, "backgroundStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(cv, "setBackgroundStyle:", [](CpuState&, GuestMemory&) {}, "v@:q");
    method(cv, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ViewState>(s.x[0]))
            if (const auto typed = std::dynamic_pointer_cast<CellViewSlots>(state->extension)) release(std::exchange(typed->object_value, 0));
        static_cast<void>(objc.send_super(s.x[0], table_cell_view, sel("dealloc")));
    }, "v@:");

    // NSTableHeaderView.
    const auto hv = table_header_view;
    method(hv, "tableView", [this](CpuState& s, GuestMemory&) {
        const auto state = objc.find_host_state<ViewState>(s.x[0]);
        const auto typed = state ? std::dynamic_pointer_cast<HeaderState>(state->extension) : nullptr;
        s.x[0] = typed ? typed->table : 0;
    });
    method(hv, "setTableView:", [this](CpuState& s, GuestMemory&) { extension<HeaderState>(s.x[0])->table = s.x[2]; }, "v@:@");
    method(hv, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    // Header geometry follows the table's columns and its horizontal scroll.
    const auto header_scroll = [this](Address table) {
        const auto clip = view_state(table)->superview;
        return clip && is_kind(clip, clip_view) ? view_bounds(*view_state(clip)).origin.x : 0.0;
    };
    method(hv, "columnAtPoint:", [this, header_scroll](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto table = send(self, "tableView").x0;
        if (!table) { s.x[0] = static_cast<Address>(-1); return; }
        const auto point = point_arg(s);
        const auto bounds = view_bounds(*view_state(self));
        CallRegs registers;
        put_point(registers, 0, {point.x - bounds.origin.x + header_scroll(table), 0});
        s.x[0] = send(table, "columnAtPoint:", registers).x0;
    }, "q@:{CGPoint=dd}");
    method(hv, "headerRectOfColumn:", [this, header_scroll](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto table = send(self, "tableView").x0;
        const auto bounds = view_bounds(*view_state(self));
        if (!table) { return_rect(s, {}); return; }
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto column = result_rect(send(table, "rectOfColumn:", registers));
        return_rect(s, {{column.origin.x - header_scroll(table) + bounds.origin.x, bounds.origin.y}, {column.size.width, bounds.size.height}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:q");
    method(hv, "drawRect:", [this, table_of, column_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto table = send(self, "tableView").x0;
        const auto bounds = view_bounds(*view_state(self));
        const auto& th = theme_for(self);
        auto& canvas = require_canvas("NSTableHeaderView drawing");
        canvas.fill_rect(bounds, th.header_background);
        canvas.fill_rect({{bounds.origin.x, bounds.origin.y + bounds.size.height - 1}, {bounds.size.width, 1}}, th.separator);
        if (!table) return;
        // Follow the table's horizontal scroll.
        double scroll{};
        if (const auto clip = view_state(table)->superview; clip && is_kind(clip, clip_view)) scroll = view_bounds(*view_state(clip)).origin.x;
        double x = bounds.origin.x - scroll;
        TextStyle style;
        style.font.size = 11;
        style.color = th.secondary_text;
        for (const auto column : table_of(table)->columns) {
            const auto info = column_of(column);
            if (info->hidden) continue;
            draw_line_in(*this, string16(send(info->header_cell, "stringValue").x0), style,
                         {{x + 6, bounds.origin.y}, {std::max(0.0, info->width - 10), bounds.size.height}}, 0);
            x += info->width + table_of(table)->spacing.width;
            canvas.fill_rect({{x - 1, bounds.origin.y + 4}, {1, bounds.size.height - 8}}, th.separator);
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // _NSCornerView: the corner above a table's vertical scroller, drawn as header background.
    const auto corner_class = declare_class("_NSCornerView", "NSView");
    method(corner_class, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(corner_class, "drawRect:", [this](CpuState& s, GuestMemory&) {
        const auto bounds = view_bounds(*view_state(s.x[0]));
        const auto& th = theme_for(s.x[0]);
        auto& canvas = require_canvas("_NSCornerView drawing");
        canvas.fill_rect(bounds, th.header_background);
        canvas.fill_rect({{bounds.origin.x, bounds.origin.y + bounds.size.height - 1}, {bounds.size.width, 1}}, th.separator);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // NSTableView.
    const auto tv = table_view;
    const bool unused_header = false;
    static_cast<void>(unused_header);
    method(tv, "initWithFrame:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        static_cast<void>(table_of(self));   // TableState before NSControl makes a ControlState
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = objc.send_super(self, table_view, sel("initWithFrame:"), registers).x0;
        const auto header = send(send(table_header_view, "alloc").x0, "initWithFrame:", [] { CallRegs r; put_rect(r, 0, {{0, 0}, {100, header_height}}); return r; }()).x0;
        static_cast<void>(send_id(header, "setTableView:", {s.x[0]}));
        table_of(s.x[0])->header_view = header;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(tv, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ViewState>(s.x[0]))
            if (const auto typed = std::dynamic_pointer_cast<TableState>(state->extension)) {
                for (const auto column : std::exchange(typed->columns, {})) release(column);
                release(std::exchange(typed->header_view, 0));
                release(std::exchange(typed->corner_view, 0));
                release(std::exchange(typed->grid_color, 0));
                for (auto& [key, value] : std::exchange(typed->cell_views, {})) release(value);
                for (auto& [row, value] : std::exchange(typed->row_views, {})) release(value);
                for (auto& [key, list] : std::exchange(typed->reuse, {})) for (const auto value : list) release(value);
                for (auto& [key, value] : std::exchange(typed->registered_nibs, {})) release(value);
            }
        static_cast<void>(objc.send_super(s.x[0], table_view, sel("dealloc")));
    }, "v@:");
    method(tv, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(tv, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(tv, "dataSource", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->data_source; });
    method(tv, "setDataSource:", [table_of](CpuState& s, GuestMemory&) { table_of(s.x[0])->data_source = s.x[2]; }, "v@:@");
    method(tv, "delegate", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->table_delegate; });
    method(tv, "setDelegate:", [table_of](CpuState& s, GuestMemory&) { table_of(s.x[0])->table_delegate = s.x[2]; }, "v@:@");
    method(tv, "doubleAction", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->double_action; }, ":@:");
    method(tv, "setDoubleAction:", [table_of](CpuState& s, GuestMemory&) { table_of(s.x[0])->double_action = s.x[2]; }, "v@::");
    method(tv, "clickedRow", [table_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(table_of(s.x[0])->clicked_row); }, "q@:");
    method(tv, "clickedColumn", [table_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(table_of(s.x[0])->clicked_column); }, "q@:");
    method(tv, "headerView", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->header_view; });
    method(tv, "setHeaderView:", [this, table_of](CpuState& s, GuestMemory&) {
        assign(table_of(s.x[0])->header_view, s.x[2]);
        if (s.x[2]) static_cast<void>(send_id(s.x[2], "setTableView:", {s.x[0]}));
        if (const auto scroller_owner = send(s.x[0], "enclosingScrollView").x0) static_cast<void>(send(scroller_owner, "tile"));
    }, "v@:@");
    method(tv, "cornerView", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->corner_view; });
    method(tv, "setCornerView:", [this, table_of](CpuState& s, GuestMemory&) { assign(table_of(s.x[0])->corner_view, s.x[2]); }, "v@:@");
    method(tv, "tableColumns", [this, table_of](CpuState& s, GuestMemory&) { s.x[0] = auto_array(table_of(s.x[0])->columns); });
    method(tv, "numberOfColumns", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->columns.size(); }, "q@:");
    method(tv, "addTableColumn:", [this, table_of, column_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        table_of(s.x[0])->columns.push_back(retain(s.x[2]));
        column_of(s.x[2])->table = s.x[0];
        static_cast<void>(send(s.x[0], "tile"));
    }, "v@:@");
    method(tv, "removeTableColumn:", [this, table_of, column_of](CpuState& s, GuestMemory&) {
        auto& list = table_of(s.x[0])->columns;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        column_of(s.x[2])->table = 0;
        release(s.x[2]);
        static_cast<void>(send(s.x[0], "reloadData"));
    }, "v@:@");
    method(tv, "moveColumn:toColumn:", [this, table_of](CpuState& s, GuestMemory&) {
        auto& list = table_of(s.x[0])->columns;
        if (s.x[2] >= list.size() || s.x[3] >= list.size()) return;
        const auto moved = list[s.x[2]];
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(s.x[2]));
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(s.x[3]), moved);
        static_cast<void>(send(s.x[0], "reloadData"));
    }, "v@:qq");
    method(tv, "tableColumnWithIdentifier:", [this, table_of, column_of](CpuState& s, GuestMemory&) {
        for (const auto column : table_of(s.x[0])->columns) {
            const auto id = column_of(column)->identifier;
            if (id == s.x[2] || (id && s.x[2] && send_bool(id, "isEqual:", {s.x[2]}))) { s.x[0] = column; return; }
        }
        s.x[0] = 0;
    });
    method(tv, "columnWithIdentifier:", [this](CpuState& s, GuestMemory&) {
        const auto column = send_id(s.x[0], "tableColumnWithIdentifier:", {s.x[2]});
        s.x[0] = column ? send_id(s.x[0], "_alchemyIndexOfColumn:", {column}) : static_cast<Address>(-1);
    }, "q@:@");
    method(tv, "_alchemyIndexOfColumn:", [table_of](CpuState& s, GuestMemory&) {
        const auto& list = table_of(s.x[0])->columns;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        s.x[0] = found == list.end() ? static_cast<Address>(-1) : static_cast<Address>(found - list.begin());
    }, "q@:@");
    method(tv, "rowHeight", [table_of](CpuState& s, GuestMemory&) { set_d(s, 0, table_of(s.x[0])->row_height); }, "d@:");
    method(tv, "setRowHeight:", [this, table_of](CpuState& s, GuestMemory&) {
        table_of(s.x[0])->row_height = std::max(1.0, d(s, 0));
        table_of(s.x[0])->row_height_set = true;
        static_cast<void>(send(s.x[0], "tile"));
    }, "v@:d");
    method(tv, "intercellSpacing", [table_of](CpuState& s, GuestMemory&) { return_size(s, table_of(s.x[0])->spacing); }, "{CGSize=dd}@:");
    method(tv, "setIntercellSpacing:", [this, table_of](CpuState& s, GuestMemory&) { table_of(s.x[0])->spacing = size_arg(s); static_cast<void>(send(s.x[0], "tile")); }, "v@:{CGSize=dd}");
    const auto table_bool = [this, table_of](const char* getter, const char* setter, bool TableState::*member) {
        method(table_view, getter, [table_of, member](CpuState& s, GuestMemory&) { return_bool(s, (*table_of(s.x[0])).*member); }, "B@:");
        method(table_view, setter, [this, table_of, member](CpuState& s, GuestMemory&) {
            (*table_of(s.x[0])).*member = bool_arg(s, 2);
            invalidate_view(s.x[0]);
        }, "v@:B");
    };
    table_bool("usesAlternatingRowBackgroundColors", "setUsesAlternatingRowBackgroundColors:", &TableState::alternating);
    table_bool("allowsMultipleSelection", "setAllowsMultipleSelection:", &TableState::multiple);
    table_bool("allowsEmptySelection", "setAllowsEmptySelection:", &TableState::empty_selection);
    table_bool("allowsColumnSelection", "setAllowsColumnSelection:", &TableState::column_selection);
    const auto table_integer = [this, table_of](const char* getter, const char* setter, std::int64_t TableState::*member) {
        method(table_view, getter, [table_of, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>((*table_of(s.x[0])).*member); }, "q@:");
        method(table_view, setter, [this, table_of, member](CpuState& s, GuestMemory&) {
            (*table_of(s.x[0])).*member = static_cast<std::int64_t>(s.x[2]);
            invalidate_view(s.x[0]);
        }, "v@:q");
    };
    table_integer("gridStyleMask", "setGridStyleMask:", &TableState::grid_style);
    table_integer("selectionHighlightStyle", "setSelectionHighlightStyle:", &TableState::highlight_style);
    table_integer("columnAutoresizingStyle", "setColumnAutoresizingStyle:", &TableState::autoresizing_style);
    for (const auto* name : {"setAllowsColumnReordering:", "setAllowsColumnResizing:", "setVerticalMotionCanBeginDrag:", "setAutosaveTableColumns:"})
        method(tv, name, [](CpuState&, GuestMemory&) {}, "v@:B");
    method(tv, "setRowSizeStyle:", [](CpuState&, GuestMemory&) {}, "v@:q");
    table_bool("allowsTypeSelect", "setAllowsTypeSelect:", &TableState::type_select);
    table_bool("floatsGroupRows", "setFloatsGroupRows:", &TableState::floats_group_rows);
    table_bool("usesAutomaticRowHeights", "setUsesAutomaticRowHeights:", &TableState::automatic_row_heights);
    table_integer("style", "setStyle:", &TableState::table_style);
    table_integer("draggingDestinationFeedbackStyle", "setDraggingDestinationFeedbackStyle:", &TableState::feedback_style);
    method(tv, "setAutosaveName:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(tv, "setSortDescriptors:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(tv, "sortDescriptors", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array({}); });
    method(tv, "registerForDraggedTypes:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(tv, "setDraggingSourceOperationMask:forLocal:", [](CpuState&, GuestMemory&) {}, "v@:QB");
    method(tv, "gridColor", [this, table_of](CpuState& s, GuestMemory&) {
        const auto chosen = table_of(s.x[0])->grid_color;
        s.x[0] = chosen ? chosen : send(color, "gridColor").x0;
    });
    method(tv, "setGridColor:", [this, table_of](CpuState& s, GuestMemory&) {
        assign(table_of(s.x[0])->grid_color, s.x[2]);
        invalidate_view(s.x[0]);
    }, "v@:@");
    method(tv, "backgroundColor", [this](CpuState& s, GuestMemory&) { s.x[0] = send(color, "controlBackgroundColor").x0; });
    method(tv, "setBackgroundColor:", [](CpuState&, GuestMemory&) {}, "v@:@");

    // Rows: the data source's count (outline views: the flattened tree).
    method(tv, "numberOfRows", [table_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(table_of(s.x[0])->rows); }, "q@:");
    method(tv, "_alchemyCountRows", [this, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        CallRegs registers;
        registers.x[2] = s.x[0];
        const auto answer = state->data_source ? send_optional(state->data_source, "numberOfRowsInTableView:", registers) : std::nullopt;
        if (!answer && state->table_delegate) static_cast<void>(0);
        state->rows = answer ? static_cast<std::int64_t>(answer->x0) : 0;
    }, "v@:");
    method(tv, "_alchemyObjectValue:row:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        CallRegs registers;
        registers.x[2] = s.x[0]; registers.x[3] = s.x[2]; registers.x[4] = s.x[3];
        const auto answer = state->data_source ? send_optional(state->data_source, "tableView:objectValueForTableColumn:row:", registers) : std::nullopt;
        s.x[0] = answer ? answer->x0 : 0;
    }, "@@:@q");
    method(tv, "_alchemyViewFor:row:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        CallRegs registers;
        registers.x[2] = s.x[0]; registers.x[3] = s.x[2]; registers.x[4] = s.x[3];
        const auto answer = state->table_delegate ? send_optional(state->table_delegate, "tableView:viewForTableColumn:row:", registers) : std::nullopt;
        s.x[0] = answer ? answer->x0 : 0;
    }, "@@:@q");
    method(tv, "_alchemyViewBased", [this, table_of](CpuState& s, GuestMemory&) {
        const auto delegate = table_of(s.x[0])->table_delegate;
        return_bool(s, delegate && responds(delegate, is_kind(s.x[0], outline_view) ? "outlineView:viewForTableColumn:item:" : "tableView:viewForTableColumn:row:"));
    }, "B@:");
    method(tv, "_alchemyHeightOfRow:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        double height = state->row_height;
        if (state->table_delegate && !is_kind(s.x[0], outline_view)) {
            CallRegs registers;
            registers.x[2] = s.x[0]; registers.x[3] = s.x[2];
            if (const auto answer = send_optional(state->table_delegate, "tableView:heightOfRow:", registers)) height = answer->d(0);
        }
        set_d(s, 0, height + state->spacing.height);
    }, "d@:q");
    const auto row_top = [this](Address self, std::int64_t row) {
        double y{};
        for (std::int64_t i = 0; i < row; ++i) { CallRegs r; r.x[2] = static_cast<Address>(i); y += send(self, "_alchemyHeightOfRow:", r).d(0); }
        return y;
    };
    const auto row_height_of = [this](Address self, std::int64_t row) { CallRegs r; r.x[2] = static_cast<Address>(row); return send(self, "_alchemyHeightOfRow:", r).d(0); };
    const auto column_x = [table_of, column_of](Address self, std::size_t index) {
        const auto state = table_of(self);
        double x{};
        for (std::size_t i = 0; i < index && i < state->columns.size(); ++i)
            if (!column_of(state->columns[i])->hidden) x += column_of(state->columns[i])->width + state->spacing.width;
        return x;
    };
    method(tv, "rectOfRow:", [this, row_top, row_height_of](CpuState& s, GuestMemory&) {
        const auto row = static_cast<std::int64_t>(s.x[2]);
        return_rect(s, {{0, row_top(s.x[0], row)}, {view_bounds(*view_state(s.x[0])).size.width, row_height_of(s.x[0], row)}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:q");
    method(tv, "rectOfColumn:", [this, table_of, column_of, column_x](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        if (s.x[2] >= state->columns.size()) { return_rect(s, {}); return; }
        return_rect(s, {{column_x(s.x[0], s.x[2]), 0}, {column_of(state->columns[s.x[2]])->width + state->spacing.width, view_bounds(*view_state(s.x[0])).size.height}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:q");
    method(tv, "frameOfCellAtColumn:row:", [this, table_of, column_of, column_x, row_top, row_height_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        if (s.x[2] >= state->columns.size()) { return_rect(s, {}); return; }
        const auto row = static_cast<std::int64_t>(s.x[3]);
        double indent{};
        if (is_kind(s.x[0], outline_view) && row >= 0 && row < static_cast<std::int64_t>(state->outline_rows.size()) &&
            state->columns[s.x[2]] == (state->outline_column ? state->outline_column : state->columns.front()))
            indent = state->indentation * static_cast<double>(state->outline_rows[static_cast<std::size_t>(row)].level + 1);
        return_rect(s, {{column_x(s.x[0], s.x[2]) + state->spacing.width / 2 + indent, row_top(s.x[0], row) + state->spacing.height / 2},
                        {std::max(0.0, column_of(state->columns[s.x[2]])->width - indent), row_height_of(s.x[0], row) - state->spacing.height}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:qq");
    method(tv, "rowAtPoint:", [this, table_of, row_height_of](CpuState& s, GuestMemory&) {
        const auto point = point_arg(s);
        const auto rows = table_of(s.x[0])->rows;
        double y{};
        for (std::int64_t i = 0; i < rows; ++i) {
            const auto h = row_height_of(s.x[0], i);
            if (point.y >= y && point.y < y + h) { s.x[0] = static_cast<Address>(i); return; }
            y += h;
        }
        s.x[0] = static_cast<Address>(-1);
    }, "q@:{CGPoint=dd}");
    method(tv, "columnAtPoint:", [table_of, column_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        const auto point = point_arg(s);
        double x{};
        for (std::size_t i = 0; i < state->columns.size(); ++i) {
            const auto info = column_of(state->columns[i]);
            if (info->hidden) continue;
            const auto w = info->width + state->spacing.width;
            if (point.x >= x && point.x < x + w) { s.x[0] = i; return; }
            x += w;
        }
        s.x[0] = static_cast<Address>(-1);
    }, "q@:{CGPoint=dd}");
    method(tv, "rowsInRect:", [this](CpuState& s, GuestMemory&) {
        const auto rect = rect_arg(s);
        CallRegs top, bottom;
        put_point(top, 0, rect.origin);
        put_point(bottom, 0, {rect.origin.x, rect.origin.y + rect.size.height - 0.5});
        const auto first = static_cast<std::int64_t>(send(s.x[0], "rowAtPoint:", top).x0);
        auto last = static_cast<std::int64_t>(send(s.x[0], "rowAtPoint:", bottom).x0);
        if (last < 0) last = static_cast<std::int64_t>(send(s.x[0], "numberOfRows").x0) - 1;
        s.x[0] = first < 0 ? 0 : static_cast<Address>(first);
        s.x[1] = first < 0 || last < first ? 0 : static_cast<Address>(last - first + 1);
    }, "{_NSRange=QQ}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // Layout: the table's frame covers its rows and columns; view-based
    // cells are (re)made for every row.
    method(tv, "tile", [this, table_of, column_of, row_top](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        double width{};
        for (const auto column : state->columns) if (!column_of(column)->hidden) width += column_of(column)->width + state->spacing.width;
        const auto height = row_top(self, state->rows);
        auto size = view_state(self)->frame.size;
        size.width = width;
        size.height = height;
        if (const auto parent = view_state(self)->superview; parent && is_kind(parent, clip_view)) {
            const auto visible = view_bounds(*view_state(parent)).size;
            size.width = std::max(size.width, visible.width);
            size.height = std::max(size.height, visible.height);
        }
        CallRegs registers;
        put_size(registers, 0, size);
        static_cast<void>(send(self, "setFrameSize:", registers));
        // View-based cells.
        if (send_bool(self, "_alchemyViewBased")) {
            for (auto& [key, value] : std::exchange(state->cell_views, {})) {
                static_cast<void>(send(value, "removeFromSuperview"));
                release(value);
            }
            for (auto& [row, value] : std::exchange(state->row_views, {})) {
                static_cast<void>(send(value, "removeFromSuperview"));
                const auto delegate = state->table_delegate;
                if (delegate && responds(delegate, "tableView:didRemoveRowView:forRow:"))
                    static_cast<void>(send_id(delegate, "tableView:didRemoveRowView:forRow:", {self, value, static_cast<Address>(-1)}));
                release(value);
            }
            const auto rows = std::min<std::int64_t>(state->rows, 2000);
            for (std::int64_t row = 0; row < rows; ++row) {
                static_cast<void>(send_id(self, "rowViewAtRow:makeIfNecessary:", {static_cast<Address>(row), 1}));
                for (std::size_t c = 0; c < state->columns.size(); ++c) {
                    if (column_of(state->columns[c])->hidden) continue;
                    const auto made = send_id(self, "viewAtColumn:row:makeIfNecessary:", {c, static_cast<Address>(row), 1});
                    static_cast<void>(made);
                }
                const auto found_row = state->row_views.find(row);
                if (found_row != state->row_views.end() && state->table_delegate && responds(state->table_delegate, "tableView:didAddRowView:forRow:"))
                    static_cast<void>(send_id(state->table_delegate, "tableView:didAddRowView:forRow:", {self, found_row->second, static_cast<Address>(row)}));
            }
        }
        if (const auto owner = send(self, "enclosingScrollView").x0) static_cast<void>(send_id(owner, "reflectScrolledClipView:", {view_state(self)->superview}));
        invalidate_view(self);
    }, "v@:");
    method(tv, "viewAtColumn:row:makeIfNecessary:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto column = s.x[2];
        const auto row = static_cast<std::int64_t>(s.x[3]);
        const auto key = std::make_pair(row, static_cast<std::size_t>(column));
        if (const auto found = state->cell_views.find(key); found != state->cell_views.end()) { s.x[0] = found->second; return; }
        if (!bool_arg(s, 4) || column >= state->columns.size() || row < 0 || row >= state->rows) { s.x[0] = 0; return; }
        const auto made = send_id(self, "_alchemyViewFor:row:", {state->columns[column], static_cast<Address>(row)});
        if (!made) { s.x[0] = 0; return; }
        state->cell_views[key] = retain(made);
        CallRegs frame;
        frame.x[2] = column; frame.x[3] = static_cast<Address>(row);
        auto rect = result_rect(send(self, "frameOfCellAtColumn:row:", frame));
        // Cell views live in the view of their row (row views are table subviews).
        const auto row_view = send_id(self, "rowViewAtRow:makeIfNecessary:", {static_cast<Address>(row), 1});
        if (row_view) {
            const auto row_frame = view_state(row_view)->frame;
            rect.origin.x -= row_frame.origin.x;
            rect.origin.y -= row_frame.origin.y;
        }
        CallRegs registers;
        put_rect(registers, 0, rect);
        static_cast<void>(send(made, "setFrame:", registers));
        static_cast<void>(send_id(row_view ? row_view : self, "addSubview:", {made}));
        s.x[0] = made;
    }, "@@:qqB");
    // The row (column) of a cell or row view, or of any view inside one.
    method(tv, "rowForView:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        for (auto current = s.x[2]; current && current != s.x[0]; current = view_state(current)->superview) {
            for (const auto& [key, value] : state->cell_views) if (value == current) { s.x[0] = static_cast<Address>(key.first); return; }
            for (const auto& [row, value] : state->row_views) if (value == current) { s.x[0] = static_cast<Address>(row); return; }
        }
        s.x[0] = static_cast<Address>(-1);
    }, "q@:@");
    method(tv, "columnForView:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        for (auto current = s.x[2]; current && current != s.x[0]; current = view_state(current)->superview)
            for (const auto& [key, value] : state->cell_views) if (value == current) { s.x[0] = key.second; return; }
        s.x[0] = static_cast<Address>(-1);
    }, "q@:@");
    // Row views: the delegate's (tableView:rowViewForRow: / outlineView:rowViewForItem:) or an NSTableRowView.
    method(tv, "rowViewAtRow:makeIfNecessary:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto row = static_cast<std::int64_t>(s.x[2]);
        if (const auto found = state->row_views.find(row); found != state->row_views.end()) { s.x[0] = found->second; return; }
        if (!bool_arg(s, 3) || row < 0 || row >= state->rows || !send_bool(self, "_alchemyViewBased")) { s.x[0] = 0; return; }
        Address made{};
        if (const auto delegate = state->table_delegate) {
            const bool outline = is_kind(self, outline_view);
            CallRegs ask;
            ask.x[2] = self;
            ask.x[3] = outline ? send_id(self, "itemAtRow:", {static_cast<Address>(row)}) : static_cast<Address>(row);
            if (const auto answer = send_optional(delegate, outline ? "outlineView:rowViewForItem:" : "tableView:rowViewForRow:", ask))
                made = retain(answer->x0);
        }
        if (!made) {
            CallRegs zero;
            put_rect(zero, 0, {});
            made = send(send(table_row_view, "alloc").x0, "initWithFrame:", zero).x0;
        }
        const auto info = extension<RowViewState>(made);
        info->table = self;
        info->row = row;
        info->selected = state->selection.contains(row);
        info->highlight_style = state->highlight_style;
        CallRegs rect;
        rect.x[2] = static_cast<Address>(row);
        CallRegs frame;
        put_rect(frame, 0, result_rect(send(self, "rectOfRow:", rect)));
        static_cast<void>(send(made, "setFrame:", frame));
        state->row_views[row] = made;
        static_cast<void>(send_id(self, "addSubview:", {made}));
        s.x[0] = made;
    }, "@@:qB");
    // A reused view of the identifier, else a new one from the nib
    // registered for it (its first top-level view, given the identifier),
    // else nil: the delegate makes its own view.
    method(tv, "makeViewWithIdentifier:owner:", [this, table_of](CpuState& s, GuestMemory& m) {
        const auto state = table_of(s.x[0]);
        const auto key = text(s.x[2]);
        auto& pool = state->reuse[key];
        if (!pool.empty()) { s.x[0] = autorelease(pool.back()); pool.pop_back(); return; }
        const auto found = state->registered_nibs.find(key);
        if (found == state->registered_nibs.end()) { s.x[0] = 0; return; }
        const auto slot = process.allocate_guest(8);
        Address objects{};
        try {
            m.write64(slot, 0);
            static_cast<void>(send_id(found->second, "instantiateWithOwner:topLevelObjects:", {s.x[3], slot}));
            objects = m.read64(slot);
        } catch (...) { process.release_guest(slot); throw; }
        process.release_guest(slot);
        Address made{};
        for (const auto object : objects ? cf.array_values(objects) : std::vector<Address>{}) {
            if (!made && object && is_kind(object, view)) made = object;
            else release(object);   // the load's reference to the other top-level objects
        }
        if (!made) fail("table cell view nib '" + key + "' has no top-level view");
        if (!view_state(made)->identifier) static_cast<void>(send_id(made, "setIdentifier:", {s.x[2]}));
        s.x[0] = autorelease(made);
    });
    method(tv, "registerNib:forIdentifier:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto key = text(s.x[3]);
        auto& slots = table_of(s.x[0])->registered_nibs;
        if (const auto found = slots.find(key); found != slots.end()) { release(found->second); slots.erase(found); }
        if (s.x[2]) slots.emplace(key, retain(s.x[2]));
    }, "v@:@@");
    method(tv, "registeredNibsByIdentifier", [this, table_of](CpuState& s, GuestMemory&) {
        std::vector<std::pair<Address, Address>> entries;
        for (const auto& [key, value] : table_of(s.x[0])->registered_nibs) entries.emplace_back(auto_string(key), value);
        s.x[0] = autorelease(cf.dictionary(entries));
    });
    method(tv, "reloadData", [this, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        static_cast<void>(send(self, "_alchemyCountRows"));
        const auto state = table_of(self);
        for (auto it = state->selection.begin(); it != state->selection.end();)
            it = *it >= state->rows ? state->selection.erase(it) : std::next(it);
        static_cast<void>(send(self, "tile"));
    }, "v@:");
    for (const auto* name : {"noteNumberOfRowsChanged", "beginUpdates", "endUpdates", "sizeLastColumnToFit"})
        method(tv, name, [this, name](CpuState& s, GuestMemory&) {
            if (std::string_view(name) != "beginUpdates") static_cast<void>(send(s.x[0], "reloadData"));
        }, "v@:");
    method(tv, "sizeToFit", [this](CpuState& s, GuestMemory&) { static_cast<void>(send(s.x[0], "tile")); }, "v@:");
    for (const auto* name : {"reloadDataForRowIndexes:columnIndexes:", "insertRowsAtIndexes:withAnimation:", "removeRowsAtIndexes:withAnimation:",
             "noteHeightOfRowsWithIndexesChanged:"})
        method(tv, name, [this](CpuState& s, GuestMemory&) { static_cast<void>(send(s.x[0], "reloadData")); }, "v@:@@");
    // Selection.
    const auto selection_changed = [this, table_of](Address self) {
        invalidate_view(self);
        // Row views follow the selection (-setSelected:, which subclasses override).
        const auto rows_state = table_of(self);
        for (const auto& [row, row_view] : std::map<std::int64_t, Address>(rows_state->row_views)) {
            const bool now = rows_state->selection.contains(row);
            if (const auto info = find_extension<RowViewState>(row_view); info && info->selected != now)
                static_cast<void>(send_id(row_view, "setSelected:", {now ? 1u : 0u}));
        }
        const auto delegate = table_of(self)->table_delegate;
        const bool outline = is_kind(self, outline_view);
        post(outline ? "NSOutlineViewSelectionDidChangeNotification" : "NSTableViewSelectionDidChangeNotification", self, delegate,
             outline ? "outlineViewSelectionDidChange:" : "tableViewSelectionDidChange:");
    };
    method(tv, "selectedRow", [table_of](CpuState& s, GuestMemory&) {
        const auto& selection = table_of(s.x[0])->selection;
        s.x[0] = selection.empty() ? static_cast<Address>(-1) : static_cast<Address>(*selection.rbegin());
    }, "q@:");
    method(tv, "selectedColumn", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(-1); }, "q@:");
    method(tv, "numberOfSelectedRows", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->selection.size(); }, "q@:");
    method(tv, "isRowSelected:", [table_of](CpuState& s, GuestMemory&) { return_bool(s, table_of(s.x[0])->selection.contains(static_cast<std::int64_t>(s.x[2]))); }, "B@:q");
    method(tv, "selectedRowIndexes", [this, table_of](CpuState& s, GuestMemory&) {
        const auto set_class = objc.find_class("NSMutableIndexSet");
        const auto made = send(send(set_class, "alloc").x0, "init").x0;
        for (const auto row : table_of(s.x[0])->selection) static_cast<void>(send_id(made, "addIndex:", {static_cast<Address>(row)}));
        s.x[0] = autorelease(made);
    });
    method(tv, "selectRowIndexes:byExtendingSelection:", [this, table_of, selection_changed](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        const auto before = state->selection;
        if (!bool_arg(s, 3)) state->selection.clear();
        if (s.x[2]) {
            const auto first = send(s.x[2], "firstIndex").x0;
            for (auto index = first; index != not_found; index = send_id(s.x[2], "indexGreaterThanIndex:", {index}))
                if (static_cast<std::int64_t>(index) < state->rows) state->selection.insert(static_cast<std::int64_t>(index));
        }
        if (!state->multiple && state->selection.size() > 1) state->selection = {*state->selection.rbegin()};
        if (state->selection != before) selection_changed(s.x[0]);
    }, "v@:@B");
    method(tv, "selectRow:byExtendingSelection:", [this, table_of, selection_changed](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        const auto before = state->selection;
        if (!bool_arg(s, 3) || !state->multiple) state->selection.clear();
        if (static_cast<std::int64_t>(s.x[2]) < state->rows) state->selection.insert(static_cast<std::int64_t>(s.x[2]));
        if (state->selection != before) selection_changed(s.x[0]);
    }, "v@:qB");
    method(tv, "deselectAll:", [table_of, selection_changed](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        if (state->selection.empty()) return;
        state->selection.clear();
        selection_changed(s.x[0]);
    }, "v@:@");
    method(tv, "deselectRow:", [table_of, selection_changed](CpuState& s, GuestMemory&) {
        if (table_of(s.x[0])->selection.erase(static_cast<std::int64_t>(s.x[2]))) selection_changed(s.x[0]);
    }, "v@:q");
    method(tv, "selectAll:", [table_of, selection_changed](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        if (!state->multiple) return;
        for (std::int64_t i = 0; i < state->rows; ++i) state->selection.insert(i);
        selection_changed(s.x[0]);
    }, "v@:@");
    method(tv, "scrollRowToVisible:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto rect = result_rect(send(s.x[0], "rectOfRow:", registers));
        CallRegs visible;
        put_rect(visible, 0, rect);
        static_cast<void>(send(s.x[0], "scrollRectToVisible:", visible));
    }, "v@:q");
    method(tv, "scrollColumnToVisible:", [](CpuState&, GuestMemory&) {}, "v@:q");
    // In-place editing. A view-based table edits with the cell view's text
    // field; a cell-based one puts the window's field editor over the cell,
    // with the table as the editor's client: the edited text goes to the data
    // source (-tableView:setObjectValue:forTableColumn:row:, the outline
    // view's ...byItem:) when editing ends, unless it was cancelled (Escape).
    method(tv, "editColumn:row:withEvent:select:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto column = static_cast<std::int64_t>(s.x[2]), row = static_cast<std::int64_t>(s.x[3]);
        if (row < 0 || row >= state->rows || column < 0 || column >= static_cast<std::int64_t>(state->columns.size()))
            fail("-[NSTableView editColumn:row:withEvent:select:] cell (" + std::to_string(column) + ", " + std::to_string(row) +
                 ") outside the table (NSInternalInconsistencyException)");
        const bool select = bool_arg(s, 5);
        const auto owner = view_state(self)->window;
        if (!owner) return;
        if (extension<ControlState>(self)->editor) end_field_editing(self, false);
        static_cast<void>(send_id(self, "scrollRowToVisible:", {static_cast<Address>(row)}));
        if (send_bool(self, "_alchemyViewBased")) {
            const auto cell_view = send_id(self, "viewAtColumn:row:makeIfNecessary:", {static_cast<Address>(column), static_cast<Address>(row), 1});
            Address field = cell_view && is_kind(cell_view, text_field) ? cell_view : 0;
            if (!field && cell_view && responds(cell_view, "textField")) field = send_id(cell_view, "textField");
            if (!field && cell_view)
                for (const auto child : all_views(cell_view))
                    if (is_kind(child, text_field) && send_bool(child, "isEditable")) { field = child; break; }
            if (!field) return;   // nothing in the cell view edits text
            state->edited_row = row;
            state->edited_column = column;
            state->edited_field = field;
            static_cast<void>(send_id(owner, "makeFirstResponder:", {field}));
            if (select) static_cast<void>(send_id(field, "selectText:", {self}));
            return;
        }
        const auto table_column = state->columns[static_cast<std::size_t>(column)];
        const auto data_cell = send_id(table_column, "dataCellForRow:", {static_cast<Address>(row)});
        if (!data_cell) return;
        static_cast<void>(send_id(data_cell, "setObjectValue:", {send_id(self, "_alchemyObjectValue:row:", {table_column, static_cast<Address>(row)})}));
        const auto editor = field_editor_for(owner);
        const auto editing = text_state(*this, editor);
        if (editing->client && editing->client != self) end_field_editing(editing->client, false);
        const auto cell_info = cell_state(*this, data_cell);
        editing->value = string16(send(data_cell, "stringValue").x0);
        editing->selection = select ? 0 : editing->value.size();
        editing->selection_length = select ? editing->value.size() : 0;
        editing->anchor = editing->selection;
        editing->marked.reset();
        editing->client = self;
        editing->delegate = self;
        editing->editable = editing->selectable = true;
        editing->secure = false;
        editing->alignment = cell_info->alignment;
        editing->scroll_x = 0;
        editing->draws_background = true;
        assign(editing->font, cell_info->font);
        assign(editing->text_color, cell_info->text_color);
        CallRegs cell;
        cell.x[2] = static_cast<Address>(column);
        cell.x[3] = static_cast<Address>(row);
        CallRegs frame;
        put_rect(frame, 0, result_rect(send(self, "frameOfCellAtColumn:row:", cell)));
        static_cast<void>(send(editor, "setFrame:", frame));
        extension<ControlState>(self)->editor = editor;
        state->edited_row = row;
        state->edited_column = column;
        state->edited_field = 0;
        static_cast<void>(send_id(self, "addSubview:", {editor}));
        window_state(owner)->first_responder = editor;
        invalidate_view(self);
        static_cast<void>(send_id(self, "textDidBeginEditing:", {notification("NSTextDidBeginEditingNotification", editor)}));
    }, "v@:qq@B");
    // The cell being edited (-1 when none): a cell-based table's while its field editor is up, a view-based
    // one's while the cell view's text field is editing.
    const auto editing_now = [this, table_of](Address table) {
        const auto state = table_of(table);
        const auto field = state->edited_field;
        const bool live = field ? extension<ControlState>(field)->editor != 0 : extension<ControlState>(table)->editor != 0;
        if (!live) state->edited_row = state->edited_column = -1;
        return state;
    };
    method(tv, "editedColumn", [editing_now](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(editing_now(s.x[0])->edited_column); }, "q@:");
    method(tv, "editedRow", [editing_now](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(editing_now(s.x[0])->edited_row); }, "q@:");
    // The table answers its field editor as an NSControl does, telling its delegate.
    method(tv, "textShouldBeginEditing:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto delegate = table_of(s.x[0])->table_delegate;
        return_bool(s, !delegate || !responds(delegate, "control:textShouldBeginEditing:") ||
                           send_bool(delegate, "control:textShouldBeginEditing:", {s.x[0], s.x[2]}));
    }, "B@:@");
    method(tv, "textShouldEndEditing:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto delegate = table_of(s.x[0])->table_delegate;
        return_bool(s, !delegate || !responds(delegate, "control:textShouldEndEditing:") ||
                           send_bool(delegate, "control:textShouldEndEditing:", {s.x[0], s.x[2]}));
    }, "B@:@");
    method(tv, "textDidBeginEditing:", [this, table_of](CpuState& s, GuestMemory&) {
        post("NSControlTextDidBeginEditingNotification", s.x[0], table_of(s.x[0])->table_delegate, "controlTextDidBeginEditing:");
    }, "v@:@");
    method(tv, "textDidChange:", [this, table_of](CpuState& s, GuestMemory&) {
        post("NSControlTextDidChangeNotification", s.x[0], table_of(s.x[0])->table_delegate, "controlTextDidChange:");
    }, "v@:@");
    // Editing ended (Return, Tab, a click elsewhere, Escape): the text goes to the data source unless cancelled.
    method(tv, "textDidEndEditing:", [this, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto editor = s.x[2] ? send_id(s.x[2], "object") : 0;
        const bool cancelled = editor && is_kind(editor, text_view) && text_state(*this, editor)->aborting;
        const auto row = std::exchange(state->edited_row, -1), column = std::exchange(state->edited_column, -1);
        state->edited_field = 0;
        if (!cancelled && editor && row >= 0 && column >= 0 && column < static_cast<std::int64_t>(state->columns.size()) && state->data_source) {
            const auto value = send_id(editor, "string");
            const auto table_column = state->columns[static_cast<std::size_t>(column)];
            if (is_kind(self, outline_view)) {
                if (responds(state->data_source, "outlineView:setObjectValue:forTableColumn:byItem:"))
                    static_cast<void>(send_id(state->data_source, "outlineView:setObjectValue:forTableColumn:byItem:",
                                              {self, value, table_column, send_id(self, "itemAtRow:", {static_cast<Address>(row)})}));
            } else if (responds(state->data_source, "tableView:setObjectValue:forTableColumn:row:")) {
                static_cast<void>(send_id(state->data_source, "tableView:setObjectValue:forTableColumn:row:", {self, value, table_column, static_cast<Address>(row)}));
            }
        }
        invalidate_view(self);
        post("NSControlTextDidEndEditingNotification", self, state->table_delegate, "controlTextDidEndEditing:");
    }, "v@:@");
    // Clicks select rows (Command toggles, Shift extends) and send the action;
    // a double click sends the double action.
    method(tv, "mouseDown:", [this, table_of, selection_changed](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto point = event_location_in(*this, self, s.x[2]);
        CallRegs at;
        put_point(at, 0, point);
        const auto row = static_cast<std::int64_t>(send(self, "rowAtPoint:", at).x0);
        const auto column = static_cast<std::int64_t>(send(self, "columnAtPoint:", at).x0);
        state->clicked_row = row;
        state->clicked_column = column;
        if (const auto owner = view_state(self)->window) static_cast<void>(send_id(owner, "makeFirstResponder:", {self}));
        if (is_kind(self, outline_view) && row >= 0 && send_bool(self, "_alchemyToggleAt:", {static_cast<Address>(row), std::bit_cast<Address>(point.x)})) return;
        const auto flags = event_state(s.x[2])->event.modifier_flags;
        const auto before = state->selection;
        bool allowed = row >= 0;
        if (allowed && state->table_delegate) {
            CallRegs ask;
            ask.x[2] = self; ask.x[3] = static_cast<Address>(row);
            const bool outline = is_kind(self, outline_view);
            if (outline) ask.x[3] = send_id(self, "itemAtRow:", {static_cast<Address>(row)});
            if (const auto answer = send_optional(state->table_delegate, outline ? "outlineView:shouldSelectItem:" : "tableView:shouldSelectRow:", ask))
                allowed = (answer->x0 & 0xff) != 0;
        }
        if (row < 0) {
            if (state->empty_selection) state->selection.clear();
        } else if (allowed) {
            if (state->multiple && (flags & modifier::command)) {
                if (!state->selection.erase(row)) state->selection.insert(row);
            } else if (state->multiple && (flags & modifier::shift) && !state->selection.empty()) {
                const auto anchor = *state->selection.begin();
                for (auto i = std::min(anchor, row); i <= std::max(anchor, row); ++i) state->selection.insert(i);
            } else {
                state->selection = {row};
            }
        }
        if (state->selection != before) selection_changed(self);
        const auto clicks = event_state(s.x[2])->event.click_count;
        if (clicks >= 2 && state->double_action) { static_cast<void>(send_action(state->double_action, state->target, self)); return; }
        // Without a double action, a double click edits an editable cell of a cell-based table.
        if (clicks >= 2 && row >= 0 && column >= 0 && column < static_cast<std::int64_t>(state->columns.size()) && !send_bool(self, "_alchemyViewBased")) {
            const auto table_column = state->columns[static_cast<std::size_t>(column)];
            bool editable = send_bool(table_column, "isEditable");
            if (editable && state->table_delegate) {
                const bool outline = is_kind(self, outline_view);
                const char* ask = outline ? "outlineView:shouldEditTableColumn:item:" : "tableView:shouldEditTableColumn:row:";
                if (responds(state->table_delegate, ask))
                    editable = send_bool(state->table_delegate, ask, {self, table_column, outline ? send_id(self, "itemAtRow:", {static_cast<Address>(row)}) : static_cast<Address>(row)});
            }
            if (editable) {
                static_cast<void>(send_id(self, "editColumn:row:withEvent:select:", {static_cast<Address>(column), static_cast<Address>(row), s.x[2], 1}));
                return;
            }
        }
        if (state->action) static_cast<void>(send_action(state->action, state->target, self));
    }, "v@:@");
    method(tv, "keyDown:", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(s.x[0], "interpretKeyEvents:", {auto_array({s.x[2]})}));
    }, "v@:@");
    for (const bool down : {true, false})
        method(tv, down ? "moveDown:" : "moveUp:", [table_of, selection_changed, down](CpuState& s, GuestMemory&) {
            const auto state = table_of(s.x[0]);
            if (!state->rows) return;
            std::int64_t row = state->selection.empty() ? (down ? 0 : state->rows - 1)
                                                        : (down ? *state->selection.rbegin() + 1 : *state->selection.begin() - 1);
            row = std::clamp<std::int64_t>(row, 0, state->rows - 1);
            state->selection = {row};
            selection_changed(s.x[0]);
        }, "v@:@");
    // Drawing: row backgrounds, selection, grid and cell-based cells.
    method(tv, "drawRect:", [this, table_of, column_of, row_height_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        resolve_dynamic_colors(*this, self);
        const auto state = table_of(self);
        const auto& th = theme_for(self);
        auto& canvas = require_canvas("NSTableView drawing");
        const auto bounds = view_bounds(*view_state(self));
        const auto dirty = rect_arg(s);
        canvas.fill_rect(dirty, th.control_background);
        const bool view_based = send_bool(self, "_alchemyViewBased");
        const auto owner = view_state(self)->window;
        const bool focused = owner && window_state(owner)->first_responder == self && window_state(owner)->key;
        double y{};
        for (std::int64_t row = 0; row < state->rows; ++row) {
            const auto h = row_height_of(self, row);
            if (y > dirty.origin.y + dirty.size.height) break;
            if (y + h >= dirty.origin.y) {
                const Rect row_rect{{bounds.origin.x, y}, {bounds.size.width, h}};
                if (state->selection.contains(row) && state->highlight_style != -1)
                    canvas.fill_rect(row_rect, focused ? th.selection : th.unfocused_selection);
                else if (state->alternating && row % 2 == 1)
                    canvas.fill_rect(row_rect, th.alternate_row);
                if (state->grid_style & 2)
                    canvas.fill_rect({{bounds.origin.x, y + h - 1}, {bounds.size.width, 1}}, state->grid_color ? color_value(state->grid_color, th.grid) : th.grid);
                if (!view_based) {
                    for (std::size_t c = 0; c < state->columns.size(); ++c) {
                        const auto column = state->columns[c];
                        const auto info = column_of(column);
                        if (info->hidden) continue;
                        const auto data_cell = send_id(column, "dataCellForRow:", {static_cast<Address>(row)});
                        if (!data_cell) continue;
                        const auto value = is_kind(self, outline_view)
                                               ? send_id(self, "_alchemyObjectValue:row:", {column, static_cast<Address>(row)})
                                               : send_id(self, "_alchemyObjectValue:row:", {column, static_cast<Address>(row)});
                        static_cast<void>(send_id(data_cell, "setObjectValue:", {value}));
                        if (state->selection.contains(row) && focused) {
                            const auto previous = cell_state(*this, data_cell)->text_color;
                            static_cast<void>(previous);
                        }
                        if (state->table_delegate && responds(state->table_delegate, "tableView:willDisplayCell:forTableColumn:row:"))
                            static_cast<void>(send_id(state->table_delegate, "tableView:willDisplayCell:forTableColumn:row:",
                                                      {self, data_cell, column, static_cast<Address>(row)}));
                        CallRegs frame;
                        frame.x[2] = c; frame.x[3] = static_cast<Address>(row);
                        const auto cell_rect = result_rect(send(self, "frameOfCellAtColumn:row:", frame));
                        CallRegs draw;
                        put_rect(draw, 0, cell_rect);
                        draw.x[2] = self;
                        static_cast<void>(send(data_cell, "drawWithFrame:inView:", draw));
                    }
                }
                if (is_kind(self, outline_view)) static_cast<void>(send_id(self, "_alchemyDrawDisclosure:", {static_cast<Address>(row)}));
            }
            y += h;
        }
        if (state->grid_style & 1) {
            double x{};
            for (const auto column : state->columns) {
                const auto info = column_of(column);
                if (info->hidden) continue;
                x += info->width + state->spacing.width;
                canvas.fill_rect({{x - 1, bounds.origin.y}, {1, bounds.size.height}}, state->grid_color ? color_value(state->grid_color, th.grid) : th.grid);
            }
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // NSOutlineView: the flattened tree of expanded items.
    const auto ov = outline_view;
    const auto ask = [this](Address self, Address target, const char* selector, std::initializer_list<Address> arguments) -> std::optional<CallResult> {
        if (!target) return std::nullopt;
        CallRegs registers;
        registers.x[2] = self;
        std::size_t index = 3;
        for (const auto value : arguments) registers.x[index++] = value;
        return send_optional(target, selector, registers);
    };
    method(ov, "_alchemyCountRows", [this, table_of, ask](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        state->outline_rows.clear();
        const std::function<void(Address, std::int64_t)> walk = [&](Address parent, std::int64_t level) {
            const auto count = ask(self, state->data_source, "outlineView:numberOfChildrenOfItem:", {parent});
            const auto n = count ? static_cast<std::int64_t>(count->x0) : 0;
            for (std::int64_t i = 0; i < n && state->outline_rows.size() < 100000; ++i) {
                const auto child = ask(self, state->data_source, "outlineView:child:ofItem:", {static_cast<Address>(i), parent});
                const auto item = child ? child->x0 : 0;
                const auto expandable = ask(self, state->data_source, "outlineView:isItemExpandable:", {item});
                const bool can_expand = expandable && (expandable->x0 & 0xff);
                state->outline_rows.push_back({item, level, can_expand});
                if (can_expand && state->expanded.contains(item)) walk(item, level + 1);
            }
        };
        walk(0, 0);
        state->rows = static_cast<std::int64_t>(state->outline_rows.size());
    }, "v@:");
    method(ov, "_alchemyObjectValue:row:", [table_of, ask](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        const auto row = static_cast<std::int64_t>(s.x[3]);
        if (row < 0 || row >= static_cast<std::int64_t>(state->outline_rows.size())) { s.x[0] = 0; return; }
        const auto answer = ask(s.x[0], state->data_source, "outlineView:objectValueForTableColumn:byItem:", {s.x[2], state->outline_rows[static_cast<std::size_t>(row)].item});
        s.x[0] = answer ? answer->x0 : 0;
    }, "@@:@q");
    method(ov, "_alchemyViewFor:row:", [table_of, ask](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        const auto row = static_cast<std::int64_t>(s.x[3]);
        if (row < 0 || row >= static_cast<std::int64_t>(state->outline_rows.size())) { s.x[0] = 0; return; }
        const auto answer = ask(s.x[0], state->table_delegate, "outlineView:viewForTableColumn:item:", {s.x[2], state->outline_rows[static_cast<std::size_t>(row)].item});
        s.x[0] = answer ? answer->x0 : 0;
    }, "@@:@q");
    method(ov, "itemAtRow:", [table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        const auto row = static_cast<std::int64_t>(s.x[2]);
        s.x[0] = row >= 0 && row < static_cast<std::int64_t>(state->outline_rows.size()) ? state->outline_rows[static_cast<std::size_t>(row)].item : 0;
    }, "@@:q");
    method(ov, "rowForItem:", [table_of](CpuState& s, GuestMemory&) {
        const auto& rows = table_of(s.x[0])->outline_rows;
        for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].item == s.x[2]) { s.x[0] = i; return; }
        s.x[0] = static_cast<Address>(-1);
    }, "q@:@");
    method(ov, "levelForRow:", [table_of](CpuState& s, GuestMemory&) {
        const auto& rows = table_of(s.x[0])->outline_rows;
        s.x[0] = s.x[2] < rows.size() ? static_cast<Address>(rows[s.x[2]].level) : static_cast<Address>(-1);
    }, "q@:q");
    method(ov, "levelForItem:", [this](CpuState& s, GuestMemory&) {
        const auto row = send_id(s.x[0], "rowForItem:", {s.x[2]});
        s.x[0] = static_cast<std::int64_t>(row) < 0 ? static_cast<Address>(-1) : send_id(s.x[0], "levelForRow:", {row});
    }, "q@:@");
    method(ov, "parentForItem:", [table_of](CpuState& s, GuestMemory&) {
        const auto& rows = table_of(s.x[0])->outline_rows;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].item == s.x[2]) {
                for (auto j = i; j-- > 0;) if (rows[j].level == rows[i].level - 1) { s.x[0] = rows[j].item; return; }
                break;
            }
        s.x[0] = 0;
    });
    method(ov, "isItemExpanded:", [table_of](CpuState& s, GuestMemory&) { return_bool(s, table_of(s.x[0])->expanded.contains(s.x[2])); }, "B@:@");
    method(ov, "isExpandable:", [table_of, ask](CpuState& s, GuestMemory&) {
        const auto answer = ask(s.x[0], table_of(s.x[0])->data_source, "outlineView:isItemExpandable:", {s.x[2]});
        return_bool(s, answer && (answer->x0 & 0xff));
    }, "B@:@");
    const auto set_expanded = [this, table_of](Address self, Address item, bool expand, bool children) {
        const auto state = table_of(self);
        const std::function<void(Address)> apply = [&](Address target) {
            if (expand) state->expanded.insert(target); else state->expanded.erase(target);
            if (!children) return;
            static_cast<void>(send(self, "_alchemyCountRows"));
            for (const auto& row : std::vector(state->outline_rows))
                if (row.expandable && (expand != state->expanded.contains(row.item))) {
                    const auto parent = send_id(self, "parentForItem:", {row.item});
                    if (parent == target) apply(row.item);
                }
        };
        if (item) apply(item);
        else for (const auto& row : std::vector(state->outline_rows)) if (row.expandable && row.level == 0) apply(row.item);
        static_cast<void>(send(self, "reloadData"));
        post(expand ? "NSOutlineViewItemDidExpandNotification" : "NSOutlineViewItemDidCollapseNotification", self);
    };
    method(ov, "expandItem:", [set_expanded](CpuState& s, GuestMemory&) { set_expanded(s.x[0], s.x[2], true, false); }, "v@:@");
    method(ov, "expandItem:expandChildren:", [set_expanded](CpuState& s, GuestMemory&) { set_expanded(s.x[0], s.x[2], true, bool_arg(s, 3)); }, "v@:@B");
    method(ov, "collapseItem:", [set_expanded](CpuState& s, GuestMemory&) { set_expanded(s.x[0], s.x[2], false, false); }, "v@:@");
    method(ov, "collapseItem:collapseChildren:", [set_expanded](CpuState& s, GuestMemory&) { set_expanded(s.x[0], s.x[2], false, bool_arg(s, 3)); }, "v@:@B");
    for (const auto* name : {"reloadItem:", "reloadItem:reloadChildren:"})
        method(ov, name, [this](CpuState& s, GuestMemory&) { static_cast<void>(send(s.x[0], "reloadData")); }, "v@:@B");
    method(ov, "indentationPerLevel", [table_of](CpuState& s, GuestMemory&) { set_d(s, 0, table_of(s.x[0])->indentation); }, "d@:");
    method(ov, "setIndentationPerLevel:", [this, table_of](CpuState& s, GuestMemory&) { table_of(s.x[0])->indentation = d(s, 0); invalidate_view(s.x[0]); }, "v@:d");
    method(ov, "outlineTableColumn", [table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        s.x[0] = state->outline_column ? state->outline_column : (state->columns.empty() ? 0 : state->columns.front());
    });
    method(ov, "setOutlineTableColumn:", [table_of](CpuState& s, GuestMemory&) { table_of(s.x[0])->outline_column = s.x[2]; }, "v@:@");
    method(ov, "setIndentationMarkerFollowsCell:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(ov, "setAutoresizesOutlineColumn:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(ov, "setAutosaveExpandedItems:", [](CpuState&, GuestMemory&) {}, "v@:B");
    // Disclosure triangles in the outline column.
    const auto triangle_rect = [this, table_of](Address self, std::int64_t row) {
        const auto state = table_of(self);
        CallRegs registers;
        registers.x[2] = static_cast<Address>(row);
        const auto rect = result_rect(send(self, "rectOfRow:", registers));
        const auto level = state->outline_rows[static_cast<std::size_t>(row)].level;
        return Rect{{rect.origin.x + state->indentation * static_cast<double>(level) + 2, rect.origin.y}, {12, rect.size.height}};
    };
    method(ov, "_alchemyDrawDisclosure:", [this, table_of, triangle_rect](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto row = static_cast<std::int64_t>(s.x[2]);
        if (row < 0 || row >= static_cast<std::int64_t>(state->outline_rows.size()) || !state->outline_rows[static_cast<std::size_t>(row)].expandable) return;
        const auto rect = triangle_rect(self, row);
        const bool open = state->expanded.contains(state->outline_rows[static_cast<std::size_t>(row)].item);
        const Point c{rect.origin.x + rect.size.width / 2, rect.origin.y + rect.size.height / 2};
        drawing::Path arrow;
        if (open) { arrow.move_to({c.x - 4, c.y - 2}); arrow.line_to({c.x, c.y + 2.5}); arrow.line_to({c.x + 4, c.y - 2}); }
        else { arrow.move_to({c.x - 2, c.y - 4}); arrow.line_to({c.x + 2.5, c.y}); arrow.line_to({c.x - 2, c.y + 4}); }
        require_canvas("NSOutlineView drawing").stroke_path(arrow, theme_for(self).secondary_text, 1.5);
    }, "v@:q");
    method(ov, "_alchemyToggleAt:", [this, table_of, triangle_rect](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto row = static_cast<std::int64_t>(s.x[2]);
        const auto x = std::bit_cast<double>(s.x[3]);
        s.x[0] = 0;
        if (row < 0 || row >= static_cast<std::int64_t>(state->outline_rows.size()) || !state->outline_rows[static_cast<std::size_t>(row)].expandable) return;
        const auto rect = triangle_rect(self, row);
        if (x < rect.origin.x - 2 || x > rect.origin.x + rect.size.width + 2) return;
        const auto item = state->outline_rows[static_cast<std::size_t>(row)].item;
        static_cast<void>(send_id(self, state->expanded.contains(item) ? "collapseItem:" : "expandItem:", {item}));
        s.x[0] = 1;
    }, "B@:qQ");
}


// ---- nib decoding of the container views ------------------------------------------------------
// The nib coder's keyed decoding (nib.cpp) through its public NSCoder
// messages. NSView's -initWithCoder: has already decoded the frame and added
// the archived subviews; these adopt the archived parts:
//   NSScrollView  NSContentView (clip view), NSVScroller / NSHScroller,
//                 NSsFlags (bits 0-1 border type, 0x10 / 0x20 has vertical /
//                 horizontal scroller, 0x200 autohides), NSScrollAmts (line
//                 and page amounts, big-endian float32). The archived header
//                 clip view and corner view are removed: -tile places the
//                 table's header view itself and draws no corner view.
//   NSClipView    NSDocView, NSBGColor.
//   NSTableView   NSTableColumns, NSHeaderView, NSCornerView, NSRowHeight,
//                 NSIntercellSpacingWidth/Height, NSBackgroundColor,
//                 NSColumnAutoresizingStyle, NSTvFlags (bit 23 alternating
//                 rows, 26 column selection, 27 multiple selection, 28 empty
//                 selection), NSTableViewArchivedReusableViewsKey (identifier ->
//                 NSNib of a cell view, registered as -registerNib:forIdentifier:).
//   NSOutlineView NSOutlineViewOutlineTableColumnKey, NSOutlineViewIndentationPerLevelKey.
//   NSTableColumn NSIdentifier, NSWidth, NSMinWidth, NSMaxWidth, NSResizingMask,
//                 NSIsEditable, NSHidden, NSHeaderCell, NSDataCell, NSTableView.
//   NSTableHeaderView NSTableView.
void AppKit::install_container_coding() {
    const auto key_of = [this](std::string_view key) { return auto_string(key); };
    const auto contains = [this, key_of](Address coder, std::string_view key) { return send_bool(coder, "containsValueForKey:", {key_of(key)}); };
    const auto object_for = [this, key_of](Address coder, std::string_view key) { return send_id(coder, "decodeObjectForKey:", {key_of(key)}); };
    const auto integer_for = [this, key_of](Address coder, std::string_view key) {
        CallRegs registers;
        registers.x[2] = key_of(key);
        return static_cast<std::int64_t>(send(coder, "decodeIntegerForKey:", registers).x0);
    };
    const auto real_for = [this, key_of](Address coder, std::string_view key) {
        CallRegs registers;
        registers.x[2] = key_of(key);
        return send(coder, "decodeDoubleForKey:", registers).d(0);
    };
    const auto super_init = [this](Address self, Address cls, Address coder) {
        CallRegs registers;
        registers.x[2] = coder;
        return objc.send_super(self, cls, sel("initWithCoder:"), registers).x0;
    };
    const auto scroll_of = [this](Address value) { return extension<ScrollState>(value); };
    const auto clip_of = [this](Address value) { return extension<ClipState>(value); };
    const auto table_of = [this](Address value) { return extension<TableState>(value); };
    const auto column_of = [this](Address value) { return objc.host_state<ColumnState>(value); };

    method(scroll_view, "initWithCoder:", [=, this](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], scroll_view, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const auto coder = s.x[2];
        const auto state = scroll_of(self);
        const auto flags = static_cast<std::uint64_t>(integer_for(coder, "NSsFlags"));
        state->border_type = static_cast<std::int64_t>(flags & 3);
        state->has_vertical = (flags & 0x10) != 0;
        state->has_horizontal = (flags & 0x20) != 0;
        state->autohides = (flags & 0x200) != 0;
        if (const auto clip = object_for(coder, "NSContentView")) {
            if (view_state(clip)->superview != self) static_cast<void>(send_id(self, "addSubview:", {clip}));
            state->clip = clip;
        } else {
            CallRegs zero;
            put_rect(zero, 0, {});
            const auto made = send(send(clip_view, "alloc").x0, "initWithFrame:", zero).x0;
            static_cast<void>(send_id(self, "addSubview:", {made}));
            release(made);
            state->clip = made;
        }
        for (const bool vertical : {true, false}) {
            const auto bar = object_for(coder, vertical ? "NSVScroller" : "NSHScroller");
            if (!bar || !is_kind(bar, scroller)) continue;
            const auto control_state = extension<ControlState>(bar);
            control_state->vertical = vertical;
            control_state->target = self;
            control_state->action = sel("_alchemyScrollerMoved:");
            if (view_state(bar)->superview != self) static_cast<void>(send_id(self, "addSubview:", {bar}));
            (vertical ? state->vertical : state->horizontal) = bar;
        }
        for (const auto* part : {"NSHeaderClipView", "NSCornerView"})
            if (const auto archived = object_for(coder, part); archived && view_state(archived)->superview == self)
                static_cast<void>(send(archived, "removeFromSuperview"));
        if (contains(coder, "NSScrollAmts")) {
            // Raw bytes: vertical line, horizontal line, vertical page, horizontal page.
            const auto length_slot = process.allocate_guest(8);
            std::vector<std::uint8_t> bytes;
            try {
                CallRegs registers;
                registers.x[2] = key_of("NSScrollAmts");
                registers.x[3] = length_slot;
                const auto pointer = send(coder, "decodeBytesForKey:returnedLength:", registers).x0;
                const auto length = memory().read64(length_slot);
                if (pointer && length >= 16) { bytes.resize(16); memory().read_bytes(pointer, bytes); }
            } catch (...) { process.release_guest(length_slot); throw; }
            process.release_guest(length_slot);
            const auto amount = [&bytes](std::size_t index) {
                const auto at = index * 4;
                const std::uint32_t bits = (std::uint32_t{bytes[at]} << 24) | (std::uint32_t{bytes[at + 1]} << 16) | (std::uint32_t{bytes[at + 2]} << 8) | bytes[at + 3];
                return static_cast<double>(std::bit_cast<float>(bits));
            };
            if (bytes.size() == 16) { state->line_scroll = amount(0); state->page_scroll = amount(2); }
        }
        // NSContentInsets: "{top, left, bottom, right}".
        if (const auto insets = object_for(coder, "NSContentInsets"))
            if (const auto n = nib::parse_numbers(cf.utf8(insets)); n && n->size() == 4)
                state->content_insets = {(*n)[0], (*n)[1], (*n)[2], (*n)[3]};
        if (contains(coder, "NSMinMagnification")) state->min_magnification = real_for(coder, "NSMinMagnification");
        if (contains(coder, "NSMaxMagnification")) state->max_magnification = real_for(coder, "NSMaxMagnification");
        if (contains(coder, "NSMagnification") && real_for(coder, "NSMagnification") != 1)
            unsupported("an archived NSScrollView with NSMagnification " + std::to_string(real_for(coder, "NSMagnification")) +
                        " (documents are drawn at magnification 1)");
        static_cast<void>(send(self, "tile"));
    }, "@@:@");

    method(clip_view, "initWithCoder:", [=, this](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], clip_view, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const auto state = clip_of(self);
        if (const auto document = object_for(s.x[2], "NSDocView")) {
            if (view_state(document)->superview != self) static_cast<void>(send_id(self, "addSubview:", {document}));
            state->document = document;
        }
        if (const auto color_value = object_for(s.x[2], "NSBGColor")) assign(state->background_color, color_value);
        // Not read: NSBounds (the scroll position follows the document view
        // and the scroll view's tile), NScvFlags and
        // NSAutomaticallyAdjustsContentInsets (content insets are not adjusted
        // for title bars).
    }, "@@:@");

    // NSScroller: its value and knob size (the scroll view sets them again when it tiles).
    method(scroller, "initWithCoder:", [=, this](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], scroller, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const auto state = extension<ControlState>(self);
        if (contains(s.x[2], "NSCurValue")) state->value = std::clamp(real_for(s.x[2], "NSCurValue"), 0.0, 1.0);
        if (contains(s.x[2], "NSPercent")) state->knob_proportion = std::clamp(real_for(s.x[2], "NSPercent"), 0.0, 1.0);
    }, "@@:@");

    method(table_view, "initWithCoder:", [=, this](CpuState& s, GuestMemory&) {
        static_cast<void>(table_of(s.x[0]));   // TableState before NSControl makes a ControlState
        const auto self = super_init(s.x[0], table_view, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const auto coder = s.x[2];
        const auto state = table_of(self);
        if (const auto columns = object_for(coder, "NSTableColumns"))
            for (const auto column : cf.array_values(columns)) {
                if (!column) continue;
                state->columns.push_back(retain(column));
                column_of(column)->table = self;
            }
        if (const auto header = object_for(coder, "NSHeaderView")) {
            assign(state->header_view, header);
            static_cast<void>(send_id(header, "setTableView:", {self}));
        }
        if (const auto corner = object_for(coder, "NSCornerView")) assign(state->corner_view, corner);
        if (contains(coder, "NSRowHeight")) { state->row_height = real_for(coder, "NSRowHeight"); state->row_height_set = true; }
        if (contains(coder, "NSIntercellSpacingWidth")) state->spacing.width = real_for(coder, "NSIntercellSpacingWidth");
        if (contains(coder, "NSIntercellSpacingHeight")) state->spacing.height = real_for(coder, "NSIntercellSpacingHeight");
        if (const auto background = object_for(coder, "NSBackgroundColor")) {
            CallRegs registers;
            registers.x[2] = background;
            static_cast<void>(send_optional(self, "setBackgroundColor:", registers));
        }
        if (contains(coder, "NSColumnAutoresizingStyle")) state->autoresizing_style = integer_for(coder, "NSColumnAutoresizingStyle");
        if (contains(coder, "NSTvFlags")) {
            const auto flags = static_cast<std::uint32_t>(integer_for(coder, "NSTvFlags"));
            state->alternating = (flags & (1u << 23)) != 0;
            state->column_selection = (flags & (1u << 26)) != 0;
            state->multiple = (flags & (1u << 27)) != 0;
            state->empty_selection = (flags & (1u << 28)) != 0;
        }
        if (const auto reusable = object_for(coder, "NSTableViewArchivedReusableViewsKey"))
            for (const auto& [identifier, template_nib] : cf.dictionary_entries(reusable))
                static_cast<void>(send_id(self, "registerNib:forIdentifier:", {template_nib, identifier}));
        if (is_kind(self, outline_view)) {
            if (const auto column = object_for(coder, "NSOutlineViewOutlineTableColumnKey")) state->outline_column = column;
            if (contains(coder, "NSOutlineViewIndentationPerLevelKey")) state->indentation = real_for(coder, "NSOutlineViewIndentationPerLevelKey");
            // Not read: NSOutlineViewAutoresizesOutlineColumnKey,
            // NSOutlineViewIndentationMarkerFollowsCellKey and
            // NSOutineViewStronglyReferencesItems (the outline column is not
            // resized to fit, the disclosure marker sits at the indentation,
            // items are held as the data source returns them).
        }
        if (contains(coder, "NSGridStyleMask")) state->grid_style = integer_for(coder, "NSGridStyleMask");
        if (const auto grid = object_for(coder, "NSGridColor")) assign(state->grid_color, grid);
        if (contains(coder, "NSTableViewStyle")) state->table_style = integer_for(coder, "NSTableViewStyle");
        if (contains(coder, "NSTableViewUseARH")) state->automatic_row_heights = integer_for(coder, "NSTableViewUseARH") != 0;
        if (contains(coder, "NSTableViewSelectionHighlightStyle")) state->highlight_style = integer_for(coder, "NSTableViewSelectionHighlightStyle");
        if (contains(coder, "NSTableViewDraggingDestinationStyle")) state->feedback_style = integer_for(coder, "NSTableViewDraggingDestinationStyle");
        if (contains(coder, "NSAllowsTypeSelect")) state->type_select = integer_for(coder, "NSAllowsTypeSelect") != 0;
        if (contains(coder, "NSTableViewShouldFloatGroupRows")) state->floats_group_rows = integer_for(coder, "NSTableViewShouldFloatGroupRows") != 0;
        // Not read: NSTableViewGroupRowStyle (group rows draw as rows),
        // NSDraggingSourceMaskForLocal / NSDraggingSourceMaskForNonLocal (drag
        // operations come from the data source).
    }, "@@:@");

    method(table_column, "initWithCoder:", [=, this](CpuState& s, GuestMemory&) {
        const auto coder = s.x[2];
        const auto self = send_id(s.x[0], "initWithIdentifier:", {object_for(coder, "NSIdentifier")});
        s.x[0] = self;
        if (!self) return;
        const auto state = column_of(self);
        if (contains(coder, "NSWidth")) state->width = real_for(coder, "NSWidth");
        if (contains(coder, "NSMinWidth")) state->min_width = real_for(coder, "NSMinWidth");
        if (contains(coder, "NSMaxWidth")) state->max_width = real_for(coder, "NSMaxWidth");
        if (contains(coder, "NSResizingMask")) state->resizing_mask = static_cast<std::uint64_t>(integer_for(coder, "NSResizingMask"));
        if (contains(coder, "NSIsEditable")) state->editable = integer_for(coder, "NSIsEditable") != 0;
        if (contains(coder, "NSHidden")) state->hidden = integer_for(coder, "NSHidden") != 0;
        if (const auto header = object_for(coder, "NSHeaderCell")) assign(state->header_cell, header);
        if (const auto data = object_for(coder, "NSDataCell")) assign(state->data_cell, data);
        if (const auto table = object_for(coder, "NSTableView")) state->table = table;
        if (const auto tip = object_for(coder, "NSHeaderToolTip")) release(std::exchange(state->header_tool_tip, cf.copy(tip)));
        if (const auto prototype = object_for(coder, "NSSortDescriptorPrototype"))
            release(std::exchange(state->sort_prototype, send(prototype, "copy").x0));
        // Archives older than NSResizingMask: NSIsResizeable.
        if (!contains(coder, "NSResizingMask") && contains(coder, "NSIsResizeable"))
            state->resizing_mask = integer_for(coder, "NSIsResizeable") != 0 ? 3 : 0;
    }, "@@:@");

    method(table_header_view, "initWithCoder:", [=, this](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], table_header_view, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        if (const auto table = object_for(s.x[2], "NSTableView")) extension<HeaderState>(self)->table = table;
    }, "@@:@");
}

// ---- additions for QLab (gap group G3: kit/completion/controls/tables.cpp, ruler.cpp) ---------------
// NSTableRowView; NSTableView row views, row moves, drop targets, the edited
// cell and the accessibility getters; NSOutlineView's item-level updates and
// drop targets; NSScroller parts; NSScrollView rulers, insets and floating
// subviews.
void install_container_additions(AppKit& kit) {
    auto* const k = &kit;
    const auto table_of = [k](Address value) { return k->extension<TableState>(value); };
    const auto row_of = [k](Address value) { return k->extension<RowViewState>(value); };
    const auto scroll_of = [k](Address value) { return k->extension<ScrollState>(value); };

    // ---- NSTableRowView ----
    const auto rv = kit.table_row_view;
    kit.method(rv, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    kit.method(rv, "dealloc", [k, rv](CpuState& s, GuestMemory&) {
        if (const auto state = k->find_extension<RowViewState>(s.x[0])) k->release(std::exchange(state->background_color, 0));
        static_cast<void>(k->objc.send_super(s.x[0], rv, k->sel("dealloc")));
    }, "v@:");
    const auto row_flag = [&kit, rv, k, row_of](const char* getter, const char* setter, bool RowViewState::*member) {
        kit.method(rv, getter, [row_of, member](CpuState& s, GuestMemory&) { return_bool(s, (*row_of(s.x[0])).*member); }, "B@:");
        kit.method(rv, setter, [k, row_of, member](CpuState& s, GuestMemory&) {
            (*row_of(s.x[0])).*member = bool_arg(s, 2);
            k->invalidate_view(s.x[0]);
        }, "v@:B");
    };
    row_flag("isSelected", "setSelected:", &RowViewState::selected);
    row_flag("isEmphasized", "setEmphasized:", &RowViewState::emphasized);
    row_flag("isGroupRowStyle", "setGroupRowStyle:", &RowViewState::group_row);
    row_flag("isFloating", "setFloating:", &RowViewState::floating);
    row_flag("isTargetForDropOperation", "setTargetForDropOperation:", &RowViewState::target_for_drop);
    kit.method(rv, "selectionHighlightStyle", [row_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(row_of(s.x[0])->highlight_style); }, "q@:");
    kit.method(rv, "draggingDestinationFeedbackStyle", [row_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(row_of(s.x[0])->feedback_style); }, "q@:");
    kit.method(rv, "setDraggingDestinationFeedbackStyle:", [k, row_of](CpuState& s, GuestMemory&) {
        row_of(s.x[0])->feedback_style = static_cast<std::int64_t>(s.x[2]);
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(rv, "setSelectionHighlightStyle:", [k, row_of](CpuState& s, GuestMemory&) {
        row_of(s.x[0])->highlight_style = static_cast<std::int64_t>(s.x[2]);
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(rv, "backgroundColor", [k, row_of](CpuState& s, GuestMemory&) {
        const auto value = row_of(s.x[0])->background_color;
        s.x[0] = value ? value : k->send(k->color, "controlBackgroundColor").x0;
    });
    kit.method(rv, "setBackgroundColor:", [k, row_of](CpuState& s, GuestMemory&) {
        k->assign(row_of(s.x[0])->background_color, s.x[2]);
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    // NSBackgroundStyle of the row's contents: emphasized (light on the accent) when selected and emphasized.
    kit.method(rv, "interiorBackgroundStyle", [row_of](CpuState& s, GuestMemory&) {
        const auto state = row_of(s.x[0]);
        s.x[0] = state->selected && state->emphasized && state->highlight_style != -1 ? 1 : 0;
    }, "q@:");
    const auto neighbour_selected = [k, table_of, row_of](Address self, std::int64_t offset) {
        const auto state = row_of(self);
        return state->table && table_of(state->table)->selection.contains(state->row + offset);
    };
    kit.method(rv, "isPreviousRowSelected", [neighbour_selected](CpuState& s, GuestMemory&) { return_bool(s, neighbour_selected(s.x[0], -1)); }, "B@:");
    kit.method(rv, "isNextRowSelected", [neighbour_selected](CpuState& s, GuestMemory&) { return_bool(s, neighbour_selected(s.x[0], 1)); }, "B@:");
    kit.method(rv, "numberOfColumns", [k, table_of, row_of](CpuState& s, GuestMemory&) {
        const auto table = row_of(s.x[0])->table;
        s.x[0] = table ? table_of(table)->columns.size() : 0;
        static_cast<void>(k);
    }, "q@:");
    // -viewAtColumn: (its cell views are its subviews, in column order) is
    // windows/window_extras.cpp's.
    kit.method(rv, "drawBackgroundInRect:", [k, row_of](CpuState& s, GuestMemory&) {
        const auto color = row_of(s.x[0])->background_color;
        if (color) k->require_canvas("NSTableRowView drawing").fill_rect(rect_arg(s), k->color_value(color));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(rv, "drawSelectionInRect:", [k, row_of](CpuState& s, GuestMemory&) {
        const auto state = row_of(s.x[0]);
        if (state->highlight_style == -1) return;   // NSTableViewSelectionHighlightStyleNone
        const auto& th = k->theme_for(s.x[0]);
        k->require_canvas("NSTableRowView drawing").fill_rect(rect_arg(s), state->emphasized ? th.selection : th.unfocused_selection);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(rv, "drawSeparatorInRect:", [k](CpuState& s, GuestMemory&) {
        k->require_canvas("NSTableRowView drawing").fill_rect(rect_arg(s), k->theme_for(s.x[0]).grid);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(rv, "drawRect:", [k, row_of, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = row_of(self);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        // The row's emphasis follows its table's focus (first responder of the key window).
        if (state->table) {
            const auto owner = k->view_state(state->table)->window;
            state->emphasized = owner && k->window_state(owner)->key && k->window_state(owner)->first_responder == state->table;
        }
        CallRegs registers;
        put_rect(registers, 0, bounds);
        static_cast<void>(k->send(self, "drawBackgroundInRect:", registers));
        if (state->selected) static_cast<void>(k->send(self, "drawSelectionInRect:", registers));
        if (state->table && (table_of(state->table)->grid_style & 2)) {
            CallRegs line;
            put_rect(line, 0, {{bounds.origin.x, bounds.origin.y + bounds.size.height - 1}, {bounds.size.width, 1}});
            static_cast<void>(k->send(self, "drawSeparatorInRect:", line));
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // ---- NSTableView ----
    const auto tv = kit.table_view;
    // No in-place cell editing is provided (-editColumn:row:withEvent:select:
    // fails), so no cell is ever being edited.
    kit.method(tv, "enumerateAvailableRowViewsUsingBlock:", [k, table_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        for (const auto& [row, row_view] : std::map<std::int64_t, Address>(table_of(s.x[0])->row_views))
            static_cast<void>(k->invoke_block(s.x[2], {row_view, static_cast<Address>(row)}));
    }, "v@:@?");
    // A row moved in the data source: the selection moves with it.
    kit.method(tv, "moveRowAtIndex:toIndex:", [k, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = table_of(self);
        const auto from = static_cast<std::int64_t>(s.x[2]), to = static_cast<std::int64_t>(s.x[3]);
        if (from < 0 || from >= state->rows || to < 0 || to >= state->rows)
            unsupported("-[NSTableView moveRowAtIndex:toIndex:] row beyond the rows (NSRangeException)");
        std::set<std::int64_t> moved;
        for (const auto row : state->selection) {
            if (row == from) { moved.insert(to); continue; }
            auto shifted = row;
            if (from < to && row > from && row <= to) --shifted;
            if (from > to && row >= to && row < from) ++shifted;
            moved.insert(shifted);
        }
        state->selection = std::move(moved);
        static_cast<void>(k->send(self, "reloadData"));
    }, "v@:qq");
    kit.method(tv, "setDropRow:dropOperation:", [k, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        state->drop_row = static_cast<std::int64_t>(s.x[2]);
        state->drop_operation = static_cast<std::int64_t>(s.x[3]);
        for (const auto& [row, row_view] : state->row_views)
            if (const auto info = k->find_extension<RowViewState>(row_view))
                info->target_for_drop = state->drop_operation == 0 /* NSTableViewDropOn */ && row == state->drop_row;
        k->invalidate_view(s.x[0]);
    }, "v@:qQ");
    // Accessibility (stored nowhere: computed from the table).
    const auto rows_in_view = [k, table_of](Address self) {
        const auto visible = k->visible_rect(self);
        CallRegs registers;
        put_rect(registers, 0, visible);
        const auto range = k->send(self, "rowsInRect:", registers);
        return std::pair<std::int64_t, std::int64_t>{static_cast<std::int64_t>(range.x0), static_cast<std::int64_t>(range.x0 + range.x1)};
    };
    kit.method(tv, "accessibilityHeaderGroup", [table_of](CpuState& s, GuestMemory&) { s.x[0] = table_of(s.x[0])->header_view; });
    kit.method(tv, "accessibilityColumnHeaderUIElements", [k, table_of](CpuState& s, GuestMemory&) {
        std::vector<Address> headers;
        for (const auto column : table_of(s.x[0])->columns)
            if (const auto cell = k->send(column, "headerCell").x0) headers.push_back(cell);
        s.x[0] = k->auto_array(headers);
    });
    kit.method(tv, "accessibilityRowHeaderUIElements", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array({}); });   // tables have no row headers
    kit.method(tv, "accessibilityVisibleColumns", [k, table_of](CpuState& s, GuestMemory&) {
        std::vector<Address> columns;
        for (const auto column : table_of(s.x[0])->columns) if (!k->send_bool(column, "isHidden")) columns.push_back(column);
        s.x[0] = k->auto_array(columns);
    });
    kit.method(tv, "accessibilitySelectedColumns", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array({}); });   // column selection is not provided
    kit.method(tv, "accessibilityVisibleRows", [k, table_of, rows_in_view](CpuState& s, GuestMemory&) {
        const auto [first, end] = rows_in_view(s.x[0]);
        std::vector<Address> rows;
        for (auto row = first; row < end; ++row)
            if (const auto view = k->send_id(s.x[0], "rowViewAtRow:makeIfNecessary:", {static_cast<Address>(row), 0})) rows.push_back(view);
        static_cast<void>(table_of);
        s.x[0] = k->auto_array(rows);
    });
    const auto cells_of_rows = [k, table_of](Address self, const std::function<bool(std::int64_t)>& wanted) {
        std::vector<Address> cells;
        for (const auto& [key, view] : table_of(self)->cell_views) if (wanted(key.first)) cells.push_back(view);
        return k->auto_array(cells);
    };
    kit.method(tv, "accessibilityVisibleCells", [cells_of_rows, rows_in_view](CpuState& s, GuestMemory&) {
        const auto [first, end] = rows_in_view(s.x[0]);
        s.x[0] = cells_of_rows(s.x[0], [first, end](std::int64_t row) { return row >= first && row < end; });
    });
    kit.method(tv, "accessibilitySelectedCells", [cells_of_rows, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        s.x[0] = cells_of_rows(s.x[0], [state](std::int64_t row) { return state->selection.contains(row); });
    });

    // ---- NSOutlineView ----
    const auto ov = kit.outline_view;
    const auto ask = [k](Address self, Address target, const char* selector, std::initializer_list<Address> arguments) -> std::optional<CallResult> {
        if (!target) return std::nullopt;
        CallRegs registers;
        registers.x[2] = self;
        std::size_t index = 3;
        for (const auto value : arguments) registers.x[index++] = value;
        return k->send_optional(target, selector, registers);
    };
    kit.method(ov, "numberOfChildrenOfItem:", [ask, table_of](CpuState& s, GuestMemory&) {
        const auto answer = ask(s.x[0], table_of(s.x[0])->data_source, "outlineView:numberOfChildrenOfItem:", {s.x[2]});
        s.x[0] = answer ? answer->x0 : 0;
    }, "q@:@");
    kit.method(ov, "child:ofItem:", [ask, table_of](CpuState& s, GuestMemory&) {
        const auto answer = ask(s.x[0], table_of(s.x[0])->data_source, "outlineView:child:ofItem:", {s.x[2], s.x[3]});
        s.x[0] = answer ? answer->x0 : 0;
    }, "@@:q@");
    kit.method(ov, "childIndexForItem:", [k, ask, table_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto parent = k->send_id(self, "parentForItem:", {s.x[2]});
        const auto count = ask(self, table_of(self)->data_source, "outlineView:numberOfChildrenOfItem:", {parent});
        const auto n = count ? static_cast<std::int64_t>(count->x0) : 0;
        for (std::int64_t i = 0; i < n; ++i) {
            const auto child = ask(self, table_of(self)->data_source, "outlineView:child:ofItem:", {static_cast<Address>(i), parent});
            if (child && child->x0 == s.x[2]) { s.x[0] = static_cast<Address>(i); return; }
        }
        s.x[0] = static_cast<Address>(-1);
    }, "q@:@");
    // Item-level updates: the tree is read again and the selected items stay selected.
    const auto reload_keeping_items = [k, table_of](Address self) {
        const auto state = table_of(self);
        std::vector<Address> items;
        for (const auto row : state->selection)
            if (row >= 0 && row < static_cast<std::int64_t>(state->outline_rows.size())) items.push_back(state->outline_rows[static_cast<std::size_t>(row)].item);
        const auto before = state->selection;
        static_cast<void>(k->send(self, "reloadData"));
        state->selection.clear();
        for (std::size_t i = 0; i < state->outline_rows.size(); ++i)
            if (std::find(items.begin(), items.end(), state->outline_rows[i].item) != items.end()) state->selection.insert(static_cast<std::int64_t>(i));
        if (state->selection != before) {
            k->invalidate_view(self);
            k->post("NSOutlineViewSelectionDidChangeNotification", self, state->table_delegate, "outlineViewSelectionDidChange:");
        }
    };
    for (const auto* name : {"insertItemsAtIndexes:inParent:withAnimation:", "removeItemsAtIndexes:inParent:withAnimation:"})
        kit.method(ov, name, [reload_keeping_items](CpuState& s, GuestMemory&) { reload_keeping_items(s.x[0]); }, "v@:@@Q");
    kit.method(ov, "moveItemAtIndex:inParent:toIndex:inParent:", [reload_keeping_items](CpuState& s, GuestMemory&) { reload_keeping_items(s.x[0]); }, "v@:q@q@");
    kit.method(ov, "setDropItem:dropChildIndex:", [k, table_of](CpuState& s, GuestMemory&) {
        const auto state = table_of(s.x[0]);
        state->drop_item = s.x[2];
        state->drop_child = static_cast<std::int64_t>(s.x[3]);
        k->invalidate_view(s.x[0]);
    }, "v@:@q");

    // ---- NSScroller parts ----
    // NSScrollerPart: no part 0, decrement page 1, knob 2, increment page 3,
    // knob slot 6 (legacy scrollers without arrows: the line parts are empty).
    kit.method(kit.scroller, "rectForPart:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = k->extension<ControlState>(self);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto track = inset(bounds, 3, 3);
        const auto length = state->vertical ? track.size.height : track.size.width;
        const auto knob_length = std::max(18.0, length * state->knob_proportion);
        const auto offset = (length - knob_length) * state->value;
        const auto along = [&](double from, double size) {
            return state->vertical ? Rect{{track.origin.x, track.origin.y + from}, {track.size.width, size}}
                                   : Rect{{track.origin.x + from, track.origin.y}, {size, track.size.height}};
        };
        const bool usable = state->knob_proportion < 1;
        switch (s.x[2]) {
        case 2: return_rect(s, usable ? along(offset, knob_length) : Rect{}); break;
        case 6: return_rect(s, track); break;
        case 1: return_rect(s, usable ? along(0, offset) : Rect{}); break;
        case 3: return_rect(s, usable ? along(offset + knob_length, std::max(0.0, length - offset - knob_length)) : Rect{}); break;
        default: return_rect(s, {}); break;
        }
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:Q");
    kit.method(kit.scroller, "testPart:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto local = k->window_to_view(self, point_arg(s));
        for (const Address part : {2ULL, 1ULL, 3ULL, 6ULL}) {
            CallRegs registers;
            registers.x[2] = part;
            if (rect_contains(result_rect(k->send(self, "rectForPart:", registers)), local)) { s.x[0] = part; return; }
        }
        s.x[0] = 0;
    }, "Q@:{CGPoint=dd}");

    // ---- NSScrollView rulers, insets, floating subviews ----
    const auto sv = kit.scroll_view;
    static Address ruler_class{};
    kit.class_method(sv, "rulerViewClass", [k](CpuState& s, GuestMemory&) { s.x[0] = ruler_class ? ruler_class : k->objc.find_class("NSRulerView"); });
    kit.class_method(sv, "setRulerViewClass:", [](CpuState& s, GuestMemory&) { ruler_class = s.x[2]; }, "v@:#");
    const auto tile = [k](Address self) { static_cast<void>(k->send(self, "tile")); };
    for (const bool horizontal : {true, false}) {
        const std::string axis = horizontal ? "Horizontal" : "Vertical";
        std::string lower = axis;
        lower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[0])));
        const auto slot_of = [scroll_of, horizontal](Address self) -> Address& {
            const auto state = scroll_of(self);
            return horizontal ? state->horizontal_ruler : state->vertical_ruler;
        };
        // The ruler view, made on demand from +rulerViewClass.
        kit.method(sv, lower + "RulerView", [k, slot_of, horizontal](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            auto& slot = slot_of(self);
            if (!slot) {
                const auto cls = k->send(k->objc.object_class(self), "rulerViewClass").x0;
                if (!cls) unsupported("NSScrollView rulers (no NSRulerView class)");
                CallRegs registers;
                registers.x[2] = self;
                registers.x[3] = horizontal ? 0 : 1;   // NSHorizontalRuler / NSVerticalRuler
                slot = k->send(k->send(cls, "alloc").x0, "initWithScrollView:orientation:", registers).x0;
            }
            s.x[0] = slot;
        });
        kit.method(sv, "set" + axis + "RulerView:", [k, slot_of, tile](CpuState& s, GuestMemory&) {
            auto& slot = slot_of(s.x[0]);
            if (slot && k->view_state(slot)->superview == s.x[0]) static_cast<void>(k->send(slot, "removeFromSuperview"));
            k->assign(slot, s.x[2]);
            tile(s.x[0]);
        }, "v@:@");
        kit.method(sv, "has" + axis + "Ruler", [scroll_of, horizontal](CpuState& s, GuestMemory&) {
            const auto state = scroll_of(s.x[0]);
            return_bool(s, horizontal ? state->has_horizontal_ruler : state->has_vertical_ruler);
        }, "B@:");
        kit.method(sv, "setHas" + axis + "Ruler:", [k, scroll_of, horizontal, lower, tile](CpuState& s, GuestMemory&) {
            const auto state = scroll_of(s.x[0]);
            (horizontal ? state->has_horizontal_ruler : state->has_vertical_ruler) = bool_arg(s, 2);
            if (bool_arg(s, 2)) static_cast<void>(k->send(s.x[0], lower + "RulerView"));
            tile(s.x[0]);
        }, "v@:B");
    }
    kit.method(sv, "rulersVisible", [scroll_of](CpuState& s, GuestMemory&) { return_bool(s, scroll_of(s.x[0])->rulers_visible); }, "B@:");
    kit.method(sv, "setRulersVisible:", [scroll_of, tile](CpuState& s, GuestMemory&) {
        scroll_of(s.x[0])->rulers_visible = bool_arg(s, 2);
        tile(s.x[0]);
    }, "v@:B");
    // NSEdgeInsets: four doubles in d0..d3 (top, left, bottom, right).
    for (const bool content : {true, false}) {
        const auto member = content ? &ScrollState::content_insets : &ScrollState::scroller_insets;
        kit.method(sv, content ? "contentInsets" : "scrollerInsets", [scroll_of, member](CpuState& s, GuestMemory&) {
            const auto& insets = (*scroll_of(s.x[0])).*member;
            set_d(s, 0, insets.top); set_d(s, 1, insets.left); set_d(s, 2, insets.bottom); set_d(s, 3, insets.right);
        }, "{NSEdgeInsets=dddd}@:");
        kit.method(sv, content ? "setContentInsets:" : "setScrollerInsets:", [scroll_of, member, tile](CpuState& s, GuestMemory&) {
            (*scroll_of(s.x[0])).*member = {d(s, 0), d(s, 1), d(s, 2), d(s, 3)};
            tile(s.x[0]);
        }, "v@:{NSEdgeInsets=dddd}");
    }
    // Magnification: the bounds and the flag are kept; documents are drawn
    // at magnification 1, so another magnification stops explicitly.
    kit.method(sv, "magnification", [](CpuState& s, GuestMemory&) { set_d(s, 0, 1); }, "d@:");
    for (const auto* name : {"setMagnification:", "setMagnification:centeredAtPoint:"})
        kit.method(sv, name, [name](CpuState& s, GuestMemory&) {
            if (d(s, 0) != 1) unsupported(std::string("-[NSScrollView ") + name + "] other than 1 (documents are drawn at magnification 1)");
        }, std::string(name).find(':') == std::string(name).size() - 1 ? "v@:d" : "v@:d{CGPoint=dd}");
    kit.method(sv, "magnifyToFitRect:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSScrollView magnifyToFitRect:] (documents are drawn at magnification 1)");
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(sv, "minMagnification", [scroll_of](CpuState& s, GuestMemory&) { set_d(s, 0, scroll_of(s.x[0])->min_magnification); }, "d@:");
    kit.method(sv, "setMinMagnification:", [scroll_of](CpuState& s, GuestMemory&) { scroll_of(s.x[0])->min_magnification = d(s, 0); }, "v@:d");
    kit.method(sv, "maxMagnification", [scroll_of](CpuState& s, GuestMemory&) { set_d(s, 0, scroll_of(s.x[0])->max_magnification); }, "d@:");
    kit.method(sv, "setMaxMagnification:", [scroll_of](CpuState& s, GuestMemory&) { scroll_of(s.x[0])->max_magnification = d(s, 0); }, "v@:d");
    kit.method(sv, "allowsMagnification", [scroll_of](CpuState& s, GuestMemory&) { return_bool(s, scroll_of(s.x[0])->allows_magnification); }, "B@:");
    kit.method(sv, "setAllowsMagnification:", [scroll_of](CpuState& s, GuestMemory&) { scroll_of(s.x[0])->allows_magnification = bool_arg(s, 2); }, "v@:B");
    kit.method(sv, "addFloatingSubview:forAxis:", [k, scroll_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = scroll_of(self);
        if (!s.x[2] || !state->clip) return;
        for (const auto& floating : state->floating) if (floating.view == s.x[2]) return;
        state->floating.push_back({k->retain(s.x[2]), static_cast<std::int64_t>(s.x[3]), k->view_state(s.x[2])->frame.origin});
        static_cast<void>(k->send_id(state->clip, "addSubview:", {s.x[2]}));
        static_cast<void>(k->send_id(self, "reflectScrolledClipView:", {state->clip}));
    }, "v@:@q");
}

} // namespace alchemy::gui::appkit
