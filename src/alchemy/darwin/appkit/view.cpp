#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/gui/layer_contents.hpp"   // -updateLayer views (qt-vfb)
#include "alchemy/darwin/quartzcore/quartzcore.hpp"

#include <algorithm>
#include <cmath>

// NSView (geometry, hierarchy, layers, display, hit testing, tracking areas,
// drag registration), NSTrackingArea, NSTextInputContext with the key
// bindings of -interpretKeyEvents:, and the NSDraggingInfo object of drops.
namespace alchemy::gui::appkit {
namespace {
// NSAutoresizingMaskOptions
constexpr std::uint64_t min_x_margin = 1, width_sizable = 2, max_x_margin = 4, min_y_margin = 8, height_sizable = 16,
    max_y_margin = 32;
// NSTrackingAreaOptions
constexpr std::uint64_t tracking_entered_exited = 0x01, tracking_moved = 0x02, tracking_cursor = 0x04,
    tracking_when_first_responder = 0x10, tracking_in_key_window = 0x20, tracking_in_active_app = 0x40,
    tracking_always = 0x80, tracking_assume_inside = 0x100, tracking_in_visible_rect = 0x200, tracking_during_drag = 0x400;
// NSWindowOrderingMode
constexpr std::int64_t below = -1;

// Mac virtual key codes (Events.h kVK_*) with standard key bindings.
const char* binding(std::uint16_t key_code, bool shift) {
    switch (key_code) {
    case 0x24: case 0x4c: return "insertNewline:";
    case 0x30: return shift ? "insertBacktab:" : "insertTab:";
    case 0x33: return "deleteBackward:";
    case 0x75: return "deleteForward:";
    case 0x35: return "cancelOperation:";
    case 0x7b: return shift ? "moveLeftAndModifySelection:" : "moveLeft:";
    case 0x7c: return shift ? "moveRightAndModifySelection:" : "moveRight:";
    case 0x7d: return shift ? "moveDownAndModifySelection:" : "moveDown:";
    case 0x7e: return shift ? "moveUpAndModifySelection:" : "moveUp:";
    case 0x73: return "scrollToBeginningOfDocument:";
    case 0x77: return "scrollToEndOfDocument:";
    case 0x74: return "scrollPageUp:";
    case 0x79: return "scrollPageDown:";
    default: return nullptr;
    }
}
// Text a key press inserts: no control characters and no function-key
// characters (U+F700..U+F8FF, UTF-8 EF 9C 80 .. EF A3 BF).
bool printable(std::string_view text) {
    if (text.empty()) return false;
    const auto first = static_cast<unsigned char>(text[0]);
    if (first < 0x20 || first == 0x7f) return false;
    if (text.size() >= 3 && first == 0xef) {
        const auto second = static_cast<unsigned char>(text[1]);
        if (second >= 0x9c && second <= 0xa3) return false;
    }
    return true;
}
} // namespace

Rect backing_aligned(Rect rect, double scale, std::uint64_t options) {
    if (!(scale > 0)) scale = 1;
    const bool flipped = (options >> 63) & 1;
    // One axis: bits 0/8/16 the minimum edge, 2/10/18 the maximum edge and
    // 4/12/20 the size (inward / outward / nearest), plus `axis` (0 x, 1 y).
    const auto align = [&](double origin, double length, unsigned axis, double& out_origin, double& out_length) {
        const auto mode = [&](unsigned base) {
            int found = 0, count = 0;
            for (int kind = 0; kind < 3; ++kind)
                if ((options >> (base + axis + 8 * kind)) & 1) { found = kind + 1; ++count; }
            if (count > 1) fail("-backingAlignedRect:options: names one edge or size more than once");
            return found;   // 0 none, 1 inward, 2 outward, 3 nearest
        };
        const int min_mode = mode(0), max_mode = mode(2), size_mode = mode(4);
        if ((min_mode != 0) + (max_mode != 0) + (size_mode != 0) != 2)
            fail("-backingAlignedRect:options: needs exactly two of the minimum edge, the maximum edge and the size per axis");
        // Halves round up, or down along a flipped y axis.
        const bool half_down = flipped && axis == 1;
        const auto nearest = [half_down](double v) { return half_down ? std::ceil(v - 0.5) : std::floor(v + 0.5); };
        const auto edge = [&](double v, int how, bool minimum) {
            v *= scale;
            if (how == 3) return nearest(v) / scale;
            const bool up = (how == 1) == minimum;   // an inward minimum or an outward maximum rounds up
            return (up ? std::ceil(v) : std::floor(v)) / scale;
        };
        const auto size = [&](double v, int how) {
            v *= scale;
            if (how == 3) return std::max(0.0, nearest(v)) / scale;
            return std::max(0.0, how == 1 ? std::floor(v) : std::ceil(v)) / scale;
        };
        if (min_mode && max_mode) {
            out_origin = edge(origin, min_mode, true);
            out_length = std::max(0.0, edge(origin + length, max_mode, false) - out_origin);
        } else if (min_mode) {
            out_origin = edge(origin, min_mode, true);
            out_length = size(length, size_mode);
        } else {
            out_length = size(length, size_mode);
            out_origin = edge(origin + length, max_mode, false) - out_length;
        }
    };
    Rect result;
    align(rect.origin.x, rect.size.width, 0, result.origin.x, result.size.width);
    align(rect.origin.y, rect.size.height, 1, result.origin.y, result.size.height);
    return result;
}

std::vector<Address> AppKit::all_views(Address root) {
    std::vector<Address> result;
    if (!root) return result;
    std::vector<Address> pending{root};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        result.push_back(current);
        const auto& children = view_state(current)->subviews;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}

void AppKit::set_view_window(Address root, Address target) {
    for (const auto value : all_views(root)) {
        const auto state = view_state(value);
        if (state->window == target) continue;
        CallRegs registers;
        registers.x[2] = target;
        static_cast<void>(send_optional(value, "viewWillMoveToWindow:", registers));
        if (state->window) {
            const auto old = window_state(state->window);
            if (old->first_responder == value) old->first_responder = state->window;
            if (old->mouse_down_view == value) old->mouse_down_view = 0;
        }
        state->window = target;
        state->inside_areas.clear();
        if (target && !state->dragged_types.empty()) {
            const auto info = window_state(target);
            if (info->id) require_window_server("-[NSView registerForDraggedTypes:]").set_accepts_file_drops(info->id, true);
        }
        update_layer_host(value);
        // Cursor rectangles belong to a window: they are rebuilt in the new one.
        discard_cursor_rects(value);
        static_cast<void>(send_optional(value, "viewDidMoveToWindow"));
        if (target) static_cast<void>(send(value, "resetCursorRects"));
    }
}

void AppKit::update_layer_host(Address value) {
    const auto state = view_state(value);
    const auto layer = state->layer;
    if (!layer) return;
    // Only CAMetalLayers present; QuartzCore may be absent (AppKit unit tests).
    const auto metal_layer = objc.find_class("CAMetalLayer");
    if (!metal_layer || !is_kind(layer, metal_layer)) return;
    std::optional<quartzcore::LayerHost> host;
    if (state->window) {
        if (const auto id = window_state(state->window)->id) {
            const auto bounds = view_bounds(*state);
            const auto a = view_to_window(value, bounds.origin);
            const auto b = view_to_window(value, {bounds.origin.x + bounds.size.width, bounds.origin.y + bounds.size.height});
            host = quartzcore::LayerHost{
                id, {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::abs(b.x - a.x), std::abs(b.y - a.y)}},
                backing_scale(state->window), nullptr};
        }
    }
    quartzcore::set_layer_host(objc, layer, std::move(host));
}

// A view that lets go of its layer (-setLayer:, -dealloc) stops being its
// delegate, as in AppKit: the layer may outlive the view (Chrome keeps its
// root layer), and its delegate is not retained.
void AppKit::drop_layer(Address value, Address layer) {
    if (!layer) return;
    if (const auto delegate = send_optional(layer, "delegate"); delegate && delegate->x0 == value)
        static_cast<void>(send_optional(layer, "setDelegate:", CallRegs{}));
    const auto metal_layer = objc.find_class("CAMetalLayer");
    if (metal_layer && is_kind(layer, metal_layer)) quartzcore::set_layer_host(objc, layer, std::nullopt);
    release(layer);
}

void AppKit::update_layer_hosts(Address root) {
    for (const auto value : all_views(root)) update_layer_host(value);
}

// Bounds: a view's own coordinates are its frame space translated by the
// bounds origin, scaled by frame.size / bounds.size and rotated by the bounds
// rotation (about the bounds origin).
namespace {
Point rotated(Point p, double degrees) {
    if (degrees == 0) return p;
    const double radians = degrees * 3.14159265358979323846 / 180, c = std::cos(radians), s = std::sin(radians);
    return {p.x * c - p.y * s, p.x * s + p.y * c};
}
} // namespace
Point AppKit::bounds_to_frame(const ViewState& state, Point point) {
    return rotated({(point.x - state.bounds_origin.x) * state.bounds_scale.width,
                    (point.y - state.bounds_origin.y) * state.bounds_scale.height}, state.bounds_rotation);
}
Point AppKit::frame_to_bounds(const ViewState& state, Point point) {
    point = rotated(point, -state.bounds_rotation);
    return {point.x / state.bounds_scale.width + state.bounds_origin.x,
            point.y / state.bounds_scale.height + state.bounds_origin.y};
}
// A rotated bounds coordinate system's bounds rectangle encloses the frame.
Rect AppKit::view_bounds(const ViewState& state) {
    if (state.bounds_rotation == 0)
        return {state.bounds_origin,
                {state.frame.size.width / state.bounds_scale.width, state.frame.size.height / state.bounds_scale.height}};
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto corner : {Point{0, 0}, Point{state.frame.size.width, 0}, Point{0, state.frame.size.height},
                              Point{state.frame.size.width, state.frame.size.height}}) {
        const auto p = frame_to_bounds(state, corner);
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

// View coordinates <-> window base coordinates through the superview chain.
// A frame is in its superview's coordinates; when a view and its superview
// (the window base for the content view, which is never flipped) disagree
// on flipping, the y axis reverses across that edge.
bool AppKit::flipped_against_superview(Address value) {
    const auto parent = view_state(value)->superview;
    return send_bool(value, "isFlipped") != (parent && send_bool(parent, "isFlipped"));
}
Point AppKit::view_to_window(Address value, Point point) {
    for (auto current = value; current;) {
        const auto state = view_state(current);
        point = bounds_to_frame(*state, point);
        if (flipped_against_superview(current)) point.y = state->frame.size.height - point.y;
        point = rotated(point, state->frame_rotation);   // about the frame origin, in the superview's coordinates
        point.x += state->frame.origin.x;
        point.y += state->frame.origin.y;
        current = state->superview;
    }
    return point;
}
Point AppKit::window_to_view(Address value, Point point) {
    std::vector<Address> chain;
    for (auto current = value; current; current = view_state(current)->superview) chain.push_back(current);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto state = view_state(*it);
        point.x -= state->frame.origin.x;
        point.y -= state->frame.origin.y;
        point = rotated(point, -state->frame_rotation);
        if (flipped_against_superview(*it)) point.y = state->frame.size.height - point.y;
        point = frame_to_bounds(*state, point);
    }
    return point;
}

// ---- cursor rectangles ---------------------------------------------------------------
void AppKit::discard_cursor_rects(Address value) {
    const auto state = view_state(value);
    for (auto& entry : std::exchange(state->cursor_rects, {})) release(entry.cursor);
}

void AppKit::update_cursor_rects(Address target, Point location) {
    // Cursor rectangles act in the key window only (AppKit).
    const auto window_info = window_state(target);
    if (!window_info->key || !window_info->cursor_rects_enabled || !window_info->content_view) return;
    Address chosen{};
    bool any{};
    // Pre-order: later views draw above earlier ones, and a view's later
    // rectangles above its earlier ones.
    for (const auto current : all_views(window_info->content_view)) {
        const auto state = view_state(current);
        if (state->cursor_rects.empty() || state->hidden) continue;
        any = true;
        for (const auto& entry : state->cursor_rects) {
            const auto a = view_to_window(current, entry.rect.origin);
            const auto b = view_to_window(current, {entry.rect.origin.x + entry.rect.size.width,
                                                    entry.rect.origin.y + entry.rect.size.height});
            const Rect rect{{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::abs(b.x - a.x), std::abs(b.y - a.y)}};
            if (rect_contains(rect, location)) chosen = entry.cursor;
        }
    }
    if (!any && !cursor_rect_cursor) return;
    if (chosen == cursor_rect_cursor) return;
    if (!chosen) {
        // Leaving every cursor rectangle restores the arrow.
        release(std::exchange(cursor_rect_cursor, 0));
        static_cast<void>(send(make_cursor(SystemCursor::arrow), "set"));
        return;
    }
    assign(cursor_rect_cursor, chosen);
    static_cast<void>(send(chosen, "set"));
}

Address AppKit::hit_test(Address target, Point location) {
    const auto content = window_state(target)->content_view;
    if (!content) return 0;
    CallRegs registers;
    put_point(registers, 0, location);   // the content view's superview space is the window base
    // The top strip (toolbar, tab bar) lies above the content view, in the same space.
    for (const auto strip_view : top_strip_views(target))
        if (const auto hit = send(strip_view, "hitTest:", registers).x0) return hit;
    return send(content, "hitTest:", registers).x0;
}

void AppKit::display_view(Address value) {
    const auto state = view_state(value);
    state->needs_display = false;
    if (state->hidden) return;
    static_cast<void>(send_optional(value, "viewWillDraw"));
    if (layer_contents::view_updates_layer(objc, value)) {
        // -updateLayer / -displayLayer: instead of -drawRect: (layer_contents.hpp, qt-vfb).
        layer_contents::update_view_layer(objc, value);
    } else {
        CallRegs registers;
        put_rect(registers, 0, view_bounds(*state));
        static_cast<void>(send(value, "drawRect:", registers));
    }
    const auto children = state->subviews;
    for (const auto child : children) display_view(child);
}

// Synthesizes tracking-area crossings for a pointer event of `target`.
void AppKit::update_tracking(Address target, Address value) {
    const auto window_info = window_state(target);
    const auto location = event_state(value)->event.location;
    update_cursor_rects(target, location);
    const auto type = event_state(value)->event.type;
    const bool dragging = type == event_type::left_mouse_dragged || type == event_type::right_mouse_dragged ||
                          type == event_type::other_mouse_dragged;
    const bool app_active = app_state()->active;
    // NSTrackingAreaOptions' activity: always, in the active application, in
    // the key window, or while the area's view is first responder (an area
    // with none of them is treated as always active).
    const auto area_active = [&](const TrackingAreaState& info, Address area_view) {
        if (info.options & tracking_always) return true;
        if (info.options & tracking_in_key_window) return window_info->key;
        if (info.options & tracking_in_active_app) return app_active;
        if (info.options & tracking_when_first_responder) return window_info->first_responder == area_view;
        return true;
    };
    for (const auto current : all_views(window_info->content_view)) {
        const auto state = view_state(current);
        if (state->tracking_areas.empty()) continue;
        const auto areas = state->tracking_areas;
        for (const auto area : areas) {
            const auto info = objc.host_state<TrackingAreaState>(area);
            if (!area_active(*info, current)) continue;
            auto rect = (info->options & tracking_in_visible_rect) ? view_bounds(*state) : info->rect;
            const auto a = view_to_window(current, rect.origin);
            const auto b = view_to_window(current, {rect.origin.x + rect.size.width, rect.origin.y + rect.size.height});
            rect = {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::abs(b.x - a.x), std::abs(b.y - a.y)}};
            const bool inside = rect_contains(rect, location);
            auto& inside_list = state->inside_areas;
            const bool was = std::find(inside_list.begin(), inside_list.end(), area) != inside_list.end();
            // Entered / exited wait for the end of a mouse drag unless the
            // area asks for them during drags.
            if (inside != was && info->owner && (!dragging || (info->options & tracking_during_drag))) {
                if (inside) inside_list.push_back(area);
                else inside_list.erase(std::remove(inside_list.begin(), inside_list.end(), area), inside_list.end());
                Event crossing = event_state(value)->event;
                crossing.type = inside ? event_type::mouse_entered : event_type::mouse_exited;
                const auto synthesized = make_event(crossing, target);
                event_state(synthesized)->tracking_area = retain(area);
                try {
                    if (info->options & tracking_entered_exited)
                        static_cast<void>(send_id(info->owner, inside ? "mouseEntered:" : "mouseExited:", {synthesized}));
                    if (inside && (info->options & tracking_cursor))
                        static_cast<void>(send_id(info->owner, "cursorUpdate:", {synthesized}));
                } catch (...) { release(synthesized); throw; }
                release(synthesized);
            }
            if (inside && (info->options & tracking_moved) && info->owner && info->owner != window_info->first_responder &&
                event_state(value)->event.type == event_type::mouse_moved)
                static_cast<void>(send_id(info->owner, "mouseMoved:", {value}));
        }
    }
}

// -[NSResponder interpretKeyEvents:] key bindings for one key press.
void AppKit::interpret_key(Address client, Address value) {
    const auto state = event_state(value);
    if (state->event.type != event_type::key_down) return;
    // A key press carrying window-server text: the input method's text, not
    // the key bindings.
    if (state->text_input) { deliver_text(client, state->event); return; }
    const auto flags = state->event.modifier_flags;
    if (const auto command = binding(state->event.key_code, (flags & modifier::shift) != 0)) {
        if (responds(client, "doCommandBySelector:")) static_cast<void>(send_id(client, "doCommandBySelector:", {sel(command)}));
        return;
    }
    const auto& characters = state->event.characters;
    if ((flags & (modifier::command | modifier::control)) || !printable(characters)) return;
    CallRegs registers;
    registers.x[2] = auto_string(characters);
    if (responds(client, "insertText:replacementRange:")) {
        registers.x[3] = not_found; registers.x[4] = 0;
        static_cast<void>(send(client, "insertText:replacementRange:", registers));
    } else {
        static_cast<void>(send(client, "insertText:", registers));
    }
}

Address AppKit::input_context_of(Address responder) {
    if (!responder || !is_kind(responder, view) || !responds(responder, "insertText:replacementRange:")) return 0;
    const auto state = view_state(responder);
    if (!state->input_context) {
        const auto context = send(text_input_context, "alloc").x0;
        state->input_context = send_id(context, "initWithClient:", {responder});
    }
    return state->input_context;
}

Address AppKit::effective_appearance(Address object) {
    for (auto current = object; current && is_kind(current, view); current = view_state(current)->superview) {
        if (const auto own = view_state(current)->appearance) return own;
        if (!view_state(current)->superview) { object = view_state(current)->window; break; }
    }
    if (object && is_kind(object, window)) return send(object, "effectiveAppearance").x0;
    return send(app ? app : shared_application(application), "effectiveAppearance").x0;
}

void AppKit::install_view() {
    const auto v = view;
    method(v, "initWithFrame:", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->frame = rect_arg(s); },
           "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(v, "init", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, {});
        s.x[0] = send(s.x[0], "initWithFrame:", registers).x0;
    });
    // NSUserInterfaceItemIdentification.
    method(v, "identifier", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->identifier; });
    method(v, "setIdentifier:", [this](CpuState& s, GuestMemory&) {
        release(std::exchange(view_state(s.x[0])->identifier, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(v, "dealloc", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (const auto state = objc.find_host_state<ViewState>(self)) {
            for (const auto child : std::exchange(state->subviews, {})) {
                const auto info = view_state(child);
                info->superview = 0; info->next_responder = 0;
                set_view_window(child, 0);
                release(child);
            }
            for (const auto area : std::exchange(state->tracking_areas, {})) release(area);
            for (auto& entry : std::exchange(state->cursor_rects, {})) release(entry.cursor);
            drop_layer(self, std::exchange(state->layer, 0));
            release(std::exchange(state->identifier, 0));
            release(std::exchange(state->shadow, 0));
            release(std::exchange(state->tool_tip, 0));
            release(std::exchange(state->appearance, 0));
            release(std::exchange(state->accessibility_overrides, 0));
            for (const auto constraint : std::exchange(state->constraints, {})) release(constraint);
            for (const auto guide : std::exchange(state->layout_guides, {})) {
                if (const auto info = objc.find_host_state<GuideState>(guide)) info->owner = 0;
                release(guide);
            }
            // The context does not retain its client and may outlive the view
            // (the active input context is retained).
            if (const auto context = std::exchange(state->input_context, 0)) {
                if (const auto info = objc.find_host_state<TextInputState>(context); info && info->client == self) info->client = 0;
                release(context);
            }
        }
        static_cast<void>(objc.send_super(self, view, sel("dealloc")));
    }, "v@:");

    // ---- geometry ----
    method(v, "frame", [this](CpuState& s, GuestMemory&) { return_rect(s, view_state(s.x[0])->frame); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(v, "bounds", [this](CpuState& s, GuestMemory&) { return_rect(s, view_bounds(*view_state(s.x[0]))); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    // -visibleRect, display and drawing: graphics.cpp.
    method(v, "setFrame:", [this](CpuState& s, GuestMemory&) {
        const auto rect = rect_arg(s);
        CallRegs origin, size;
        put_point(origin, 0, rect.origin);
        put_size(size, 0, rect.size);
        static_cast<void>(send(s.x[0], "setFrameOrigin:", origin));
        static_cast<void>(send(s.x[0], "setFrameSize:", size));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(v, "setFrameOrigin:", [this](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        const auto origin = point_arg(s);
        if (origin.x == state->frame.origin.x && origin.y == state->frame.origin.y) return;
        state->frame.origin = origin;
        update_layer_hosts(s.x[0]);
        if (frame_changed_hook) frame_changed_hook(s.x[0]);
        if (state->posts_frame_changes) post(note::view_frame_did_change, s.x[0]);
    }, "v@:{CGPoint=dd}");
    method(v, "setFrameSize:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = view_state(self);
        const auto size = size_arg(s);
        const auto old = state->frame.size;
        if (size.width == old.width && size.height == old.height) return;
        state->frame.size = size;
        state->needs_display = true;
        if (state->layer) {
            CallRegs registers;
            put_rect(registers, 0, {{0, 0}, size});
            static_cast<void>(send_optional(state->layer, "setFrame:", registers));
        }
        if (state->autoresizes_subviews && !state->subviews.empty()) {
            CallRegs registers;
            put_size(registers, 0, old);
            static_cast<void>(send(self, "resizeSubviewsWithOldSize:", registers));
        }
        update_layer_hosts(self);
        if (frame_changed_hook) frame_changed_hook(self);
        // Rectangles usually follow the bounds: a resized view rebuilds them.
        if (!state->cursor_rects.empty()) {
            discard_cursor_rects(self);
            static_cast<void>(send(self, "resetCursorRects"));
        }
        if (state->posts_frame_changes) post(note::view_frame_did_change, self);
    }, "v@:{CGSize=dd}");
    method(v, "resizeSubviewsWithOldSize:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_size(registers, 0, size_arg(s));
        const auto children = view_state(s.x[0])->subviews;
        for (const auto child : children) static_cast<void>(send(child, "resizeWithOldSuperviewSize:", registers));
    }, "v@:{CGSize=dd}");
    method(v, "resizeWithOldSuperviewSize:", [this](CpuState& s, GuestMemory&) {
        // Springs and struts: the superview's size change is shared evenly
        // between the flexible margins and dimension of each axis.
        const auto state = view_state(s.x[0]);
        const auto mask = state->autoresizing_mask;
        if (!mask || !state->superview) return;
        const auto old = size_arg(s);
        const auto now = view_state(state->superview)->frame.size;
        auto frame = state->frame;
        const auto share = [](double delta, bool low, bool size, bool high, double& origin, double& extent) {
            const int parts = (low ? 1 : 0) + (size ? 1 : 0) + (high ? 1 : 0);
            if (!parts) return;
            const auto piece = delta / parts;
            if (low) origin += piece;
            if (size) extent += piece;
        };
        share(now.width - old.width, mask & min_x_margin, mask & width_sizable, mask & max_x_margin, frame.origin.x, frame.size.width);
        share(now.height - old.height, mask & min_y_margin, mask & height_sizable, mask & max_y_margin, frame.origin.y, frame.size.height);
        CallRegs registers;
        put_rect(registers, 0, frame);
        static_cast<void>(send(s.x[0], "setFrame:", registers));
    }, "v@:{CGSize=dd}");
    method(v, "autoresizingMask", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->autoresizing_mask; }, "Q@:");
    method(v, "setAutoresizingMask:", [this](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        if (state->autoresizing_mask == s.x[2]) return;
        state->autoresizing_mask = s.x[2];
        if (layout_engine) layout_engine->second.autoresizing_changed(s.x[0]);
    }, "v@:Q");
    method(v, "autoresizesSubviews", [this](CpuState& s, GuestMemory&) { return_bool(s, view_state(s.x[0])->autoresizes_subviews); }, "B@:");
    method(v, "setAutoresizesSubviews:", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->autoresizes_subviews = bool_arg(s, 2); }, "v@:B");
    method(v, "postsFrameChangedNotifications", [this](CpuState& s, GuestMemory&) { return_bool(s, view_state(s.x[0])->posts_frame_changes); }, "B@:");
    method(v, "setPostsFrameChangedNotifications:", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->posts_frame_changes = bool_arg(s, 2); }, "v@:B");
    // ---- bounds (translation and scale; rotation is not provided) ----
    const auto bounds_changed = [this](Address self) {
        const auto state = view_state(self);
        state->needs_display = true;
        update_layer_hosts(self);   // subviews moved within the window
        if (state->posts_bounds_changes) post(note::view_bounds_did_change, self);
    };
    const auto scale_for = [](std::string_view selector, double frame, double bounds) {
        if (!(bounds > 0) || !std::isfinite(bounds)) unsupported("-[NSView " + std::string(selector) + "] with a non-positive bounds size");
        return frame > 0 ? frame / bounds : 1.0;   // an empty frame has no scale yet
    };
    method(v, "setBoundsOrigin:", [this, bounds_changed](CpuState& s, GuestMemory&) {
        view_state(s.x[0])->bounds_origin = point_arg(s);
        bounds_changed(s.x[0]);
    }, "v@:{CGPoint=dd}");
    method(v, "setBoundsSize:", [this, bounds_changed, scale_for](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        const auto size = size_arg(s);
        state->bounds_scale = {scale_for("setBoundsSize:", state->frame.size.width, size.width),
                               scale_for("setBoundsSize:", state->frame.size.height, size.height)};
        bounds_changed(s.x[0]);
    }, "v@:{CGSize=dd}");
    method(v, "setBounds:", [this](CpuState& s, GuestMemory&) {
        const auto rect = rect_arg(s);
        CallRegs origin, size;
        put_point(origin, 0, rect.origin);
        put_size(size, 0, rect.size);
        static_cast<void>(send(s.x[0], "setBoundsOrigin:", origin));
        static_cast<void>(send(s.x[0], "setBoundsSize:", size));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // The bounds origin moves by -point: what was at `point` is the new origin.
    method(v, "translateOriginToPoint:", [this, bounds_changed](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        const auto point = point_arg(s);
        state->bounds_origin = {state->bounds_origin.x - point.x, state->bounds_origin.y - point.y};
        bounds_changed(s.x[0]);
    }, "v@:{CGPoint=dd}");
    // One old unit becomes `size` units: bounds origin and size divide by it.
    method(v, "scaleUnitSquareToSize:", [this, bounds_changed](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        const auto unit = size_arg(s);
        if (!(unit.width > 0) || !(unit.height > 0)) unsupported("-[NSView scaleUnitSquareToSize:] with a non-positive size");
        state->bounds_origin = {state->bounds_origin.x / unit.width, state->bounds_origin.y / unit.height};
        state->bounds_scale = {state->bounds_scale.width * unit.width, state->bounds_scale.height * unit.height};
        bounds_changed(s.x[0]);
    }, "v@:{CGSize=dd}");
    method(v, "postsBoundsChangedNotifications", [this](CpuState& s, GuestMemory&) { return_bool(s, view_state(s.x[0])->posts_bounds_changes); }, "B@:");
    method(v, "setPostsBoundsChangedNotifications:", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->posts_bounds_changes = bool_arg(s, 2); }, "v@:B");
    // Rotation. The frame rotates about its origin (or, through
    // -setFrameCenterRotation:, about its centre: the origin moves to keep the
    // centre in place); the bounds about the bounds origin. Drawing, the
    // conversions and hit testing follow; a layer-backed view's layer does not
    // rotate with it, so rotating one stops explicitly.
    const auto rotation_changed = [this](Address self) {
        const auto state = view_state(self);
        if (state->layer) unsupported("-[NSView set...Rotation:] of a layer-backed view (its layer is not rotated)");
        state->needs_display = true;
        if (state->superview) invalidate_view(state->superview);
        else invalidate_view(self);
    };
    method(v, "frameRotation", [this](CpuState& s, GuestMemory&) { set_d(s, 0, view_state(s.x[0])->frame_rotation); }, "d@:");
    method(v, "setFrameRotation:", [this, rotation_changed](CpuState& s, GuestMemory&) {
        view_state(s.x[0])->frame_rotation = d(s, 0);
        rotation_changed(s.x[0]);
    }, "v@:d");
    method(v, "frameCenterRotation", [this](CpuState& s, GuestMemory&) { set_d(s, 0, view_state(s.x[0])->frame_rotation); }, "d@:");
    method(v, "setFrameCenterRotation:", [this, rotation_changed](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        const Point half{state->frame.size.width / 2, state->frame.size.height / 2};
        const auto before = rotated(half, state->frame_rotation);
        const Point centre{state->frame.origin.x + before.x, state->frame.origin.y + before.y};
        state->frame_rotation = d(s, 0);
        const auto after = rotated(half, state->frame_rotation);
        state->frame.origin = {centre.x - after.x, centre.y - after.y};
        rotation_changed(s.x[0]);
    }, "v@:d");
    method(v, "boundsRotation", [this](CpuState& s, GuestMemory&) { set_d(s, 0, view_state(s.x[0])->bounds_rotation); }, "d@:");
    method(v, "setBoundsRotation:", [this, rotation_changed](CpuState& s, GuestMemory&) {
        view_state(s.x[0])->bounds_rotation = d(s, 0);
        rotation_changed(s.x[0]);
    }, "v@:d");
    method(v, "rotateByAngle:", [this, rotation_changed](CpuState& s, GuestMemory&) {
        view_state(s.x[0])->bounds_rotation += d(s, 0);
        rotation_changed(s.x[0]);
    }, "v@:d");
    // Whether the view or a superview is rotated (or, for the second, scaled) from the window's base coordinates.
    const auto from_base = [this](Address self, bool scaled_too) {
        for (auto current = self; current; current = view_state(current)->superview) {
            const auto state = view_state(current);
            if (state->frame_rotation != 0 || state->bounds_rotation != 0) return true;
            if (scaled_too && (state->bounds_scale.width != 1 || state->bounds_scale.height != 1)) return true;
        }
        return false;
    };
    method(v, "isRotatedFromBase", [from_base](CpuState& s, GuestMemory&) { return_bool(s, from_base(s.x[0], false)); }, "B@:");
    method(v, "isRotatedOrScaledFromBase", [from_base](CpuState& s, GuestMemory&) { return_bool(s, from_base(s.x[0], true)); }, "B@:");
    method(v, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(v, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@");
    method(v, "isHidden", [this](CpuState& s, GuestMemory&) { return_bool(s, view_state(s.x[0])->hidden); }, "B@:");
    method(v, "setHidden:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = view_state(self);
        const bool hidden = bool_arg(s, 2);
        if (state->hidden == hidden) return;
        state->hidden = hidden;
        if (!hidden) state->needs_display = true;
        // -viewDidHide / -viewDidUnhide go to the view and to the descendants
        // whose visibility changed with it (not those hidden themselves),
        // unless an ancestor still hides them.
        bool ancestor_hidden{};
        for (auto current = state->superview; current && !ancestor_hidden; current = view_state(current)->superview)
            ancestor_hidden = view_state(current)->hidden;
        if (ancestor_hidden) return;
        const char* selector = hidden ? "viewDidHide" : "viewDidUnhide";
        std::vector<Address> pending{self};
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            static_cast<void>(send(current, selector));
            for (const auto child : view_state(current)->subviews)
                if (!view_state(child)->hidden) pending.push_back(child);
        }
    }, "v@:B");
    method(v, "viewDidHide", [](CpuState&, GuestMemory&) {}, "v@:");
    method(v, "viewDidUnhide", [](CpuState&, GuestMemory&) {}, "v@:");
    method(v, "isHiddenOrHasHiddenAncestor", [this](CpuState& s, GuestMemory&) {
        bool hidden{};
        for (auto current = s.x[0]; current && !hidden; current = view_state(current)->superview) hidden = view_state(current)->hidden;
        return_bool(s, hidden);
    }, "B@:");
    method(v, "inLiveResize", [this](CpuState& s, GuestMemory&) {
        const auto owner = view_state(s.x[0])->window;
        return_bool(s, owner && window_state(owner)->live_resize);
    }, "B@:");
    // Touch types matter to NSTouch delivery, which Win32 pointer input does
    // not produce; the value is kept for the getter.
    method(v, "allowedTouchTypes", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->allowed_touch_types; }, "Q@:");
    method(v, "setAllowedTouchTypes:", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->allowed_touch_types = s.x[2]; }, "v@:Q");
    method(v, "userInterfaceLayoutDirection", [this](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(view_state(s.x[0])->layout_direction);
    }, "q@:");
    method(v, "setUserInterfaceLayoutDirection:", [this](CpuState& s, GuestMemory&) {
        view_state(s.x[0])->layout_direction = static_cast<std::int64_t>(s.x[2]);
        invalidate_view(s.x[0]);
    }, "v@:q");
    // Vibrancy needs a visual effect view behind; plain views do not ask for it.
    method(v, "allowsVibrancy", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // A view that is not a scroll view has nothing to reflect.
    method(v, "reflectScrolledClipView:", [](CpuState&, GuestMemory&) {}, "v@:@");
    // NSAnimatablePropertyContainer: the view's own animation for a key,
    // else its class's default.
    method(v, "animationForKey:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], key = s.x[2];
        const auto own = key ? send_id(send(self, "animations").x0, "objectForKey:", {key}) : 0;
        s.x[0] = own ? own : send_id(send(self, "class").x0, "defaultAnimationForKey:", {key});
    });
    // The safe area: the part of the view not under the titlebar of a
    // window whose content view extends below it (full-size content view),
    // within its superview's safe area, less the additional insets
    // (-additionalSafeAreaInsets) of the view and its superviews. Insets
    // are {top, left, bottom, right}; a view without a window has its own
    // additional insets.
    const auto safe_insets = [this](Address self) {
        const auto state = view_state(self);
        const auto owner = state->window;
        if (!owner) return state->additional_safe_area;
        // The safe rectangle in window base coordinates (y up), from the outermost superview down.
        const auto content = content_rect(owner);
        Rect safe{{0, 0}, content.size};
        constexpr std::uint64_t full_size_content = 1ULL << 15;
        if (window_state(owner)->style & full_size_content) safe.size.height = std::max(0.0, safe.size.height - std::max(0.0, titlebar_height(owner)));
        std::vector<Address> chain;
        for (auto current = self; current; current = view_state(current)->superview) chain.push_back(current);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto rect = view_rect_in_window(*it, view_bounds(*view_state(*it)));
            const auto& extra = view_state(*it)->additional_safe_area;
            const double left = std::max(safe.origin.x, rect.origin.x) + extra[1];
            const double bottom = std::max(safe.origin.y, rect.origin.y) + extra[2];
            const double right = std::min(safe.origin.x + safe.size.width, rect.origin.x + rect.size.width) - extra[3];
            const double top = std::min(safe.origin.y + safe.size.height, rect.origin.y + rect.size.height) - extra[0];
            safe = {{left, bottom}, {std::max(0.0, right - left), std::max(0.0, top - bottom)}};
        }
        const auto rect = view_rect_in_window(self, view_bounds(*state));
        return std::array<double, 4>{
            std::clamp(rect.origin.y + rect.size.height - (safe.origin.y + safe.size.height), 0.0, rect.size.height),
            std::clamp(safe.origin.x - rect.origin.x, 0.0, rect.size.width),
            std::clamp(safe.origin.y - rect.origin.y, 0.0, rect.size.height),
            std::clamp(rect.origin.x + rect.size.width - (safe.origin.x + safe.size.width), 0.0, rect.size.width)};
    };
    method(v, "safeAreaInsets", [safe_insets](CpuState& s, GuestMemory&) {
        const auto insets = safe_insets(s.x[0]);
        for (unsigned i = 0; i < 4; ++i) set_d(s, i, insets[i]);
    }, "{NSEdgeInsets=dddd}@:");
    method(v, "safeAreaRect", [this, safe_insets](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto insets = safe_insets(self);
        auto rect = view_bounds(*view_state(self));
        const bool flipped = send_bool(self, "isFlipped");
        rect.origin.x += insets[1];
        rect.size.width = std::max(0.0, rect.size.width - insets[1] - insets[3]);
        rect.origin.y += flipped ? insets[0] : insets[2];
        rect.size.height = std::max(0.0, rect.size.height - insets[0] - insets[2]);
        return_rect(s, rect);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    // Pixel alignment in the window's backing store.
    method(v, "backingAlignedRect:options:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto owner = view_state(self)->window;
        const double scale = owner ? backing_scale(owner) : 1.0;
        const auto in_window = view_rect_in_window(self, rect_arg(s));
        return_rect(s, window_rect_in_view(self, backing_aligned(in_window, scale, s.x[2])));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}Q");
    // OpenGL surfaces are not provided (the GPU path is Metal); the request
    // for a full-resolution surface is kept for the getter.
    method(v, "wantsBestResolutionOpenGLSurface", [this](CpuState& s, GuestMemory&) { return_bool(s, view_state(s.x[0])->best_resolution_gl); }, "B@:");
    method(v, "setWantsBestResolutionOpenGLSurface:", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->best_resolution_gl = bool_arg(s, 2); }, "v@:B");

    // ---- hierarchy ----
    method(v, "window", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->window; });
    method(v, "superview", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->superview; });
    method(v, "subviews", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array(view_state(s.x[0])->subviews); });
    // ---- the key view loop (weak links, as in AppKit) ----
    method(v, "nextKeyView", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->next_key_view; });
    method(v, "previousKeyView", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->previous_key_view; });
    method(v, "setNextKeyView:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], next = s.x[2];
        const auto state = view_state(self);
        if (const auto old = state->next_key_view; old && old != next)
            if (const auto info = objc.find_host_state<ViewState>(old); info && info->previous_key_view == self) info->previous_key_view = 0;
        state->next_key_view = next;
        if (next && is_kind(next, view)) view_state(next)->previous_key_view = self;
    }, "v@:@");
    const auto can_become_key = [this](Address value) {
        if (!value || !view_state(value)->window) return false;
        for (auto current = value; current; current = view_state(current)->superview)
            if (view_state(current)->hidden) return false;
        return send_bool(value, "acceptsFirstResponder");
    };
    method(v, "canBecomeKeyView", [can_become_key](CpuState& s, GuestMemory&) { return_bool(s, can_become_key(s.x[0])); }, "B@:");
    method(v, "needsPanelToBecomeKey", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    const auto valid_key_view = [this](Address start, bool forward) -> Address {
        Address current = start;
        for (int guard = 0; guard < 4096; ++guard) {
            current = forward ? view_state(current)->next_key_view : view_state(current)->previous_key_view;
            if (!current || current == start) return 0;
            if (send_bool(current, "canBecomeKeyView")) return current;
        }
        return 0;
    };
    method(v, "nextValidKeyView", [valid_key_view](CpuState& s, GuestMemory&) { s.x[0] = valid_key_view(s.x[0], true); });
    method(v, "previousValidKeyView", [valid_key_view](CpuState& s, GuestMemory&) { s.x[0] = valid_key_view(s.x[0], false); });

    // -sortSubviewsUsingFunction:context: with the guest comparator
    // (NSComparisonResult (*)(id, id, void*)); a stable sort, then the view
    // redraws (the drawing order changed).
    method(v, "sortSubviewsUsingFunction:context:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], function = s.x[2], context = s.x[3];
        if (!function) fail("-[NSView sortSubviewsUsingFunction:context:] needs a function");
        auto list = view_state(self)->subviews;
        EngineAdapter engine(process.calling_engine());
        std::stable_sort(list.begin(), list.end(), [&](Address a, Address b) {
            const std::array<Address, 3> arguments{a, b, context};
            return static_cast<std::int64_t>(engine.call(function, std::span(arguments))) < 0;
        });
        view_state(self)->subviews = std::move(list);
        invalidate_view(self);
    }, "v@:^v^v");
    const auto add = [this](Address self, Address child, std::int64_t place, Address relative) {
        if (!child || child == self) fail("-[NSView addSubview:] needs another view");
        const auto keep = retain(child);
        if (view_state(child)->superview) static_cast<void>(send(child, "removeFromSuperview"));
        const auto state = view_state(self);
        auto& list = state->subviews;
        auto at = list.end();
        if (relative) {
            at = std::find(list.begin(), list.end(), relative);
            if (at != list.end() && place != below) ++at;
        } else if (place == below) {
            at = list.begin();
        }
        list.insert(at, keep);
        const auto info = view_state(child);
        info->superview = self;
        info->next_responder = self;
        set_view_window(child, state->window);
        static_cast<void>(send_optional(child, "viewDidMoveToSuperview"));
        if (hierarchy_changed_hook) hierarchy_changed_hook(child, 0);
    };
    method(v, "addSubview:", [add](CpuState& s, GuestMemory&) { add(s.x[0], s.x[2], 1, 0); }, "v@:@");
    method(v, "addSubview:positioned:relativeTo:", [add](CpuState& s, GuestMemory&) {
        add(s.x[0], s.x[2], static_cast<std::int64_t>(s.x[3]), s.x[4]);
    }, "v@:@q@");
    method(v, "removeFromSuperview", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = view_state(self);
        const auto parent = std::exchange(state->superview, 0);
        if (!parent) return;
        auto& list = view_state(parent)->subviews;
        list.erase(std::remove(list.begin(), list.end(), self), list.end());
        state->next_responder = 0;
        if (hierarchy_changed_hook) hierarchy_changed_hook(self, parent);
        set_view_window(self, 0);
        static_cast<void>(send_optional(self, "viewDidMoveToSuperview"));
        release(self);
    }, "v@:");
    method(v, "isDescendantOf:", [this](CpuState& s, GuestMemory&) {
        for (auto current = s.x[0]; current; current = view_state(current)->superview)
            if (current == s.x[2]) { s.x[0] = 1; return; }
        s.x[0] = 0;
    }, "B@:@");
    for (const auto* name : {"viewWillMoveToWindow:", "viewDidMoveToWindow", "viewDidMoveToSuperview", "viewWillStartLiveResize",
             "viewDidEndLiveResize", "updateTrackingAreas", "resetCursorRects"})
        method(v, name, [](CpuState&, GuestMemory&) {}, "v@:");   // override points
    // -viewWillDraw resolves the dynamic system colours (controls/appearance.cpp).

    // ---- cursor rectangles: the cursor shown while the pointer is over a
    // rectangle of a view in the key window (update_cursor_rects) ----
    const auto refresh_cursor = [this](Address self) {
        const auto owner = view_state(self)->window;
        const auto server = window_server_or_null();
        if (!owner || !server || !window_state(owner)->id) return;
        const auto mouse = server->mouse_location();
        const auto origin = content_rect(owner).origin;
        update_cursor_rects(owner, {mouse.x - origin.x, mouse.y - origin.y});
    };
    method(v, "addCursorRect:cursor:", [this, refresh_cursor](CpuState& s, GuestMemory&) {
        const auto cursor_object = s.x[2];
        if (!cursor_object || !is_kind(cursor_object, cursor)) fail("-[NSView addCursorRect:cursor:] needs an NSCursor");
        const auto rect = rect_arg(s);
        if (rect_empty(rect)) return;
        view_state(s.x[0])->cursor_rects.push_back({rect, retain(cursor_object)});
        refresh_cursor(s.x[0]);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(v, "removeCursorRect:cursor:", [this, refresh_cursor](CpuState& s, GuestMemory&) {
        const auto rect = rect_arg(s);
        auto& list = view_state(s.x[0])->cursor_rects;
        for (auto it = list.begin(); it != list.end(); ++it) {
            const auto& r = it->rect;
            if (it->cursor == s.x[2] && r.origin.x == rect.origin.x && r.origin.y == rect.origin.y &&
                r.size.width == rect.size.width && r.size.height == rect.size.height) {
                const auto released = it->cursor;
                list.erase(it);
                release(released);
                refresh_cursor(s.x[0]);
                return;
            }
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(v, "discardCursorRects", [this](CpuState& s, GuestMemory&) { discard_cursor_rects(s.x[0]); }, "v@:");
    method(v, "hitTest:", [this](CpuState& s, GuestMemory&) {
        // The point is in the superview's coordinates.
        const auto self = s.x[0];
        auto point = point_arg(s);
        s.x[0] = 0;
        if (send_bool(self, "isHidden")) return;
        const auto frame = result_rect(send(self, "frame"));
        // In the frame's own (unrotated) space: a rotated frame contains the points of its rotated rectangle.
        point = rotated({point.x - frame.origin.x, point.y - frame.origin.y}, -view_state(self)->frame_rotation);
        if (!rect_contains({{0, 0}, frame.size}, point)) return;
        if (flipped_against_superview(self)) point.y = frame.size.height - point.y;
        point = frame_to_bounds(*view_state(self), point);
        const auto children = view_state(self)->subviews;
        CallRegs registers;
        put_point(registers, 0, point);
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            if (const auto hit = send(*it, "hitTest:", registers).x0) { s.x[0] = hit; return; }
        s.x[0] = self;
    }, "@@:{CGPoint=dd}");

    // ---- coordinate conversion ----
    const auto convert_point = [this](Address self, Point point, Address other, bool from) {
        if (from) return window_to_view(self, other ? view_to_window(other, point) : point);
        const auto base = view_to_window(self, point);
        return other ? window_to_view(other, base) : base;
    };
    const auto convert_rect = [convert_point](Address self, Rect rect, Address other, bool from) {
        const auto a = convert_point(self, rect.origin, other, from);
        const auto b = convert_point(self, {rect.origin.x + rect.size.width, rect.origin.y + rect.size.height}, other, from);
        return Rect{{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::abs(b.x - a.x), std::abs(b.y - a.y)}};
    };
    method(v, "convertPoint:fromView:", [convert_point](CpuState& s, GuestMemory&) {
        return_point(s, convert_point(s.x[0], point_arg(s), s.x[2], true));
    }, "{CGPoint=dd}@:{CGPoint=dd}@");
    method(v, "convertPoint:toView:", [convert_point](CpuState& s, GuestMemory&) {
        return_point(s, convert_point(s.x[0], point_arg(s), s.x[2], false));
    }, "{CGPoint=dd}@:{CGPoint=dd}@");
    method(v, "convertRect:fromView:", [convert_rect](CpuState& s, GuestMemory&) {
        return_rect(s, convert_rect(s.x[0], rect_arg(s), s.x[2], true));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(v, "convertRect:toView:", [convert_rect](CpuState& s, GuestMemory&) {
        return_rect(s, convert_rect(s.x[0], rect_arg(s), s.x[2], false));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    // Sizes scale by the bounds scales between the views (window base: 1).
    const auto window_scale = [this](Address self) {
        Size total{1, 1};
        for (auto current = self; current; current = view_state(current)->superview) {
            const auto scale = view_state(current)->bounds_scale;
            total = {total.width * scale.width, total.height * scale.height};
        }
        return total;
    };
    const auto convert_size = [window_scale](Address self, Size size, Address other, bool from) {
        const auto own = window_scale(self), theirs = other ? window_scale(other) : Size{1, 1};
        const auto ratio = from ? Size{theirs.width / own.width, theirs.height / own.height}
                                : Size{own.width / theirs.width, own.height / theirs.height};
        return Size{size.width * ratio.width, size.height * ratio.height};
    };
    method(v, "convertSize:fromView:", [convert_size](CpuState& s, GuestMemory&) {
        return_size(s, convert_size(s.x[0], size_arg(s), s.x[2], true));
    }, "{CGSize=dd}@:{CGSize=dd}@");
    method(v, "convertSize:toView:", [convert_size](CpuState& s, GuestMemory&) {
        return_size(s, convert_size(s.x[0], size_arg(s), s.x[2], false));
    }, "{CGSize=dd}@:{CGSize=dd}@");
    // Backing store pixels: the view's units scaled to window points, then
    // by the window's backing scale factor.
    const auto backing_factor = [this, window_scale](Address self, bool to) {
        const auto points = window_scale(self);
        const auto scale = backing_scale(view_state(self)->window);
        const Size factor{points.width * scale, points.height * scale};
        return to ? factor : Size{1 / factor.width, 1 / factor.height};
    };
    for (const bool to : {true, false}) {
        const std::string direction = to ? "ToBacking:" : "FromBacking:";
        method(v, "convertRect" + direction, [backing_factor, to](CpuState& s, GuestMemory&) {
            const auto factor = backing_factor(s.x[0], to);
            const auto r = rect_arg(s);
            return_rect(s, {{r.origin.x * factor.width, r.origin.y * factor.height},
                            {r.size.width * factor.width, r.size.height * factor.height}});
        }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
        method(v, "convertPoint" + direction, [backing_factor, to](CpuState& s, GuestMemory&) {
            const auto factor = backing_factor(s.x[0], to);
            const auto p = point_arg(s);
            return_point(s, {p.x * factor.width, p.y * factor.height});
        }, "{CGPoint=dd}@:{CGPoint=dd}");
        method(v, "convertSize" + direction, [backing_factor, to](CpuState& s, GuestMemory&) {
            const auto factor = backing_factor(s.x[0], to);
            const auto value = size_arg(s);
            return_size(s, {value.width * factor.width, value.height * factor.height});
        }, "{CGSize=dd}@:{CGSize=dd}");
    }

    // ---- layers and display ----
    method(v, "wantsLayer", [this](CpuState& s, GuestMemory&) { return_bool(s, view_state(s.x[0])->wants_layer); }, "B@:");
    method(v, "setWantsLayer:", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->wants_layer = bool_arg(s, 2); }, "v@:B");
    method(v, "wantsUpdateLayer", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(v, "makeBackingLayer", [this](CpuState& s, GuestMemory&) {
        const auto layer_class = objc.find_class("CALayer");
        if (!layer_class) fail("QuartzCore CALayer is missing");
        s.x[0] = send(layer_class, "layer").x0;
    });
    method(v, "layer", [this](CpuState& s, GuestMemory&) {
        // A layer-backed view creates its backing layer on first use.
        const auto self = s.x[0];
        const auto state = view_state(self);
        if (!state->layer && state->wants_layer) {
            const auto created = send(self, "makeBackingLayer").x0;
            if (created) {
                static_cast<void>(send_id(self, "setLayer:", {created}));
                state->backing_layer = true;
                CallRegs registers;
                registers.set_d(0, backing_scale(state->window));
                static_cast<void>(send_optional(created, "setContentsScale:", registers));
            }
        }
        s.x[0] = state->layer;
    });
    method(v, "setLayer:", [this](CpuState& s, GuestMemory&) {
        // The view is its layer's delegate (QuartzCore finds the window, and
        // through -windowNumber the window-server window, this way).
        const auto self = s.x[0], value = s.x[2];
        const auto state = view_state(self);
        if (state->layer == value) return;
        const auto old = state->layer;
        state->layer = retain(value);
        state->backing_layer = false;
        drop_layer(self, old);
        if (!value) return;
        CallRegs delegate;
        delegate.x[2] = self;
        static_cast<void>(send_optional(value, "setDelegate:", delegate));
        CallRegs frame;
        put_rect(frame, 0, {{0, 0}, state->frame.size});
        static_cast<void>(send_optional(value, "setFrame:", frame));
        update_layer_host(self);
    }, "v@:@");
    method(v, "needsDisplay", [this](CpuState& s, GuestMemory&) { return_bool(s, view_state(s.x[0])->needs_display); }, "B@:");
    // -setNeedsDisplay:, -display..., -drawRect: (NSView draws nothing
    // itself; subclasses override it): graphics.cpp.

    // ---- tracking areas, dragging, text input ----
    method(v, "addTrackingArea:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto info = objc.host_state<TrackingAreaState>(s.x[2]);
        info->view = s.x[0];
        const auto state = view_state(s.x[0]);
        state->tracking_areas.push_back(retain(s.x[2]));
        // NSTrackingAssumeInside: the first crossing is an exit.
        if (info->options & tracking_assume_inside) state->inside_areas.push_back(s.x[2]);
    }, "v@:@");
    method(v, "removeTrackingArea:", [this](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        auto& list = state->tracking_areas;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        state->inside_areas.erase(std::remove(state->inside_areas.begin(), state->inside_areas.end(), s.x[2]), state->inside_areas.end());
        release(s.x[2]);
    }, "v@:@");
    method(v, "trackingAreas", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array(view_state(s.x[0])->tracking_areas); });
    method(v, "registerForDraggedTypes:", [this](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        for (const auto value : items(s.x[2])) state->dragged_types.push_back(text(value));
        if (state->window) {
            const auto info = window_state(state->window);
            if (info->id) require_window_server("-[NSView registerForDraggedTypes:]").set_accepts_file_drops(info->id, true);
        }
    }, "v@:@");
    method(v, "unregisterDraggedTypes", [this](CpuState& s, GuestMemory&) { view_state(s.x[0])->dragged_types.clear(); }, "v@:");
    method(v, "registeredDraggedTypes", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> names;
        for (const auto& name : view_state(s.x[0])->dragged_types) names.push_back(auto_string(name));
        s.x[0] = auto_array(names);
    });
    method(v, "inputContext", [this](CpuState& s, GuestMemory&) { s.x[0] = input_context_of(s.x[0]); });
    // NSAppearanceCustomization.
    method(v, "appearance", [this](CpuState& s, GuestMemory&) { s.x[0] = view_state(s.x[0])->appearance; });
    method(v, "setAppearance:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (view_state(self)->appearance == s.x[2]) return;
        assign(view_state(self)->appearance, s.x[2]);
        for (const auto current : all_views(self)) invalidate_view(current);
        static_cast<void>(send_optional(self, "viewDidChangeEffectiveAppearance"));
    }, "v@:@");
    method(v, "effectiveAppearance", [this](CpuState& s, GuestMemory&) { s.x[0] = effective_appearance(s.x[0]); });
    method(v, "viewDidChangeEffectiveAppearance", [](CpuState&, GuestMemory&) {}, "v@:");   // override point

    // ---- NSTrackingArea ----
    const auto t = tracking_area;
    method(t, "initWithRect:options:owner:userInfo:", [this](CpuState& s, GuestMemory&) {
        const auto state = objc.host_state<TrackingAreaState>(s.x[0]);
        state->rect = rect_arg(s);
        state->options = s.x[2];
        state->owner = s.x[3];
        state->user_info = retain(s.x[4]);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}Q@@");
    method(t, "rect", [this](CpuState& s, GuestMemory&) { return_rect(s, objc.host_state<TrackingAreaState>(s.x[0])->rect); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(t, "options", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<TrackingAreaState>(s.x[0])->options; }, "Q@:");
    method(t, "owner", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<TrackingAreaState>(s.x[0])->owner; });
    method(t, "userInfo", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<TrackingAreaState>(s.x[0])->user_info; });
    method(t, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<TrackingAreaState>(s.x[0])) release(std::exchange(state->user_info, 0));
        static_cast<void>(objc.send_super(s.x[0], tracking_area, sel("dealloc")));
    }, "v@:");

    // ---- NSTextInputContext ----
    const auto c = text_input_context;
    method(c, "initWithClient:", [this](CpuState& s, GuestMemory&) { objc.host_state<TextInputState>(s.x[0])->client = s.x[2]; });
    method(c, "client", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<TextInputState>(s.x[0])->client; });
    class_method(c, "currentInputContext", [this](CpuState& s, GuestMemory&) {
        Address responder{};
        {
            std::lock_guard lock(mutex);
            if (key_window) responder = window_state(key_window)->first_responder;
        }
        s.x[0] = responder ? send(responder, "inputContext").x0 : 0;
    });
    method(c, "handleEvent:", [this](CpuState& s, GuestMemory&) {
        const auto client = objc.host_state<TextInputState>(s.x[0])->client;
        if (!client || !s.x[2] || event_state(s.x[2])->event.type != event_type::key_down) { s.x[0] = 0; return; }
        interpret_key(client, s.x[2]);
        s.x[0] = 1;
    }, "B@:@");
    const auto client_window = [this](Address context) -> WindowId {
        const auto client = objc.host_state<TextInputState>(context)->client;
        if (!client || !is_kind(client, view)) return 0;
        const auto owner = view_state(client)->window;
        return owner ? window_state(owner)->id : 0;
    };
    method(c, "discardMarkedText", [client_window](CpuState& s, GuestMemory&) {
        if (const auto id = client_window(s.x[0])) require_window_server("-[NSTextInputContext discardMarkedText]").discard_marked_text(id);
    }, "v@:");
    // The window server asks for the caret rectangle whenever it places the
    // candidate window, so there is no cached geometry to drop.
    method(c, "invalidateCharacterCoordinates", [](CpuState&, GuestMemory&) {}, "v@:");
    method(c, "activate", [this, client_window](CpuState& s, GuestMemory&) {
        objc.host_state<TextInputState>(s.x[0])->active = true;
        if (const auto id = client_window(s.x[0])) require_window_server("-[NSTextInputContext activate]").set_text_input_enabled(id, true);
    }, "v@:");
    method(c, "deactivate", [this, client_window](CpuState& s, GuestMemory&) {
        objc.host_state<TextInputState>(s.x[0])->active = false;
        if (const auto id = client_window(s.x[0])) require_window_server("-[NSTextInputContext deactivate]").set_text_input_enabled(id, false);
    }, "v@:");
    method(c, "selectedKeyboardInputSource", [this](CpuState& s, GuestMemory&) {
        s.x[0] = auto_string(require_window_server("-[NSTextInputContext selectedKeyboardInputSource]").keyboard_layout().identifier);
    });
    method(c, "keyboardInputSources", [this](CpuState& s, GuestMemory&) {
        const auto name = auto_string(require_window_server("-[NSTextInputContext keyboardInputSources]").keyboard_layout().identifier);
        s.x[0] = auto_array({name});
    });
    method(c, "acceptsGlyphInfo", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(c, "setAcceptsGlyphInfo:", [](CpuState&, GuestMemory&) {}, "v@:B");

    // ---- NSDraggingInfo of file drops ----
    const auto g = dragging_info;
    const auto drag = [this](Address object) { return objc.host_state<DraggingState>(object); };
    method(g, "draggingPasteboard", [drag](CpuState& s, GuestMemory&) { s.x[0] = drag(s.x[0])->pasteboard; });
    method(g, "draggingLocation", [drag](CpuState& s, GuestMemory&) { return_point(s, drag(s.x[0])->location); }, "{CGPoint=dd}@:");
    method(g, "draggedImageLocation", [drag](CpuState& s, GuestMemory&) { return_point(s, drag(s.x[0])->location); }, "{CGPoint=dd}@:");
    method(g, "draggingDestinationWindow", [drag](CpuState& s, GuestMemory&) { s.x[0] = drag(s.x[0])->window; });
    method(g, "draggingSourceOperationMask", [drag](CpuState& s, GuestMemory&) { s.x[0] = drag(s.x[0])->source_mask; }, "Q@:");
    method(g, "draggingSource", [drag](CpuState& s, GuestMemory&) { s.x[0] = drag(s.x[0])->source; });   // 0: another process
    method(g, "draggedImage", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(g, "draggingSequenceNumber", [drag](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(drag(s.x[0])->sequence); }, "q@:");
    method(g, "numberOfValidItemsForDrop", [this, drag](CpuState& s, GuestMemory&) {
        const auto state = drag(s.x[0]);
        s.x[0] = state->valid_items >= 0 ? static_cast<Address>(state->valid_items)
                                         : objc.host_state<PasteboardState>(state->pasteboard)->file_paths.size();
    }, "q@:");
    method(g, "setNumberOfValidItemsForDrop:", [drag](CpuState& s, GuestMemory&) {
        drag(s.x[0])->valid_items = static_cast<std::int64_t>(s.x[2]);
    }, "v@:q");
    method(g, "springLoadingHighlight", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");   // NSSpringLoadingHighlightNone
    method(g, "resetSpringLoading", [](CpuState&, GuestMemory&) {}, "v@:");
    method(g, "animatesToDestination", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(g, "setAnimatesToDestination:", [](CpuState&, GuestMemory&) {}, "v@:B");
    // -draggingFormation / -setDraggingFormation: are windows/workspace_dragging.cpp's.
    method(g, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<DraggingState>(s.x[0])) release(std::exchange(state->pasteboard, 0));
        static_cast<void>(objc.send_super(s.x[0], dragging_info, sel("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::appkit
