#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <cmath>

// NSWindow over one window-server window, and the WindowEventSink that turns
// window-server events into queued NSEvents and synchronous delegate
// notifications.
//
// Window base coordinates are the content area's (origin bottom-left of the
// content, points), the space of event locations; Win32 frames have borders
// on every side, so the frame origin is not the base origin.
namespace alchemy::gui::appkit {
namespace {
// NSWindowOrderingMode
constexpr std::int64_t order_above = 1, order_below = -1, order_out = 0;
constexpr std::uint64_t occlusion_state_visible = 1ULL << 1;   // NSWindowOcclusionStateVisible
constexpr double unlimited_size = 10000;   // AppKit's window size limit
bool same_size(Size a, Size b) { return std::abs(a.width - b.width) < 1e-6 && std::abs(a.height - b.height) < 1e-6; }
bool same_point(Point a, Point b) { return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6; }
// While AppKit orders a window the server reports the change from inside
// the call; the occlusion update waits until the ordering is done.
struct OrderingScope {
    Address& slot;
    Address saved;
    OrderingScope(Address& value, Address window) : slot(value), saved(std::exchange(value, window)) {}
    ~OrderingScope() { slot = saved; }
};
const char* mouse_selector(std::uint32_t type) {
    switch (type) {
    case event_type::left_mouse_down: return "mouseDown:";
    case event_type::left_mouse_up: return "mouseUp:";
    case event_type::left_mouse_dragged: return "mouseDragged:";
    case event_type::right_mouse_down: return "rightMouseDown:";
    case event_type::right_mouse_up: return "rightMouseUp:";
    case event_type::right_mouse_dragged: return "rightMouseDragged:";
    case event_type::other_mouse_down: return "otherMouseDown:";
    case event_type::other_mouse_up: return "otherMouseUp:";
    case event_type::other_mouse_dragged: return "otherMouseDragged:";
    default: return nullptr;
    }
}
const char* view_event_selector(std::uint32_t type) {
    switch (type) {
    case event_type::scroll_wheel: return "scrollWheel:";
    case event_type::tablet_point: return "tabletPoint:";
    case event_type::tablet_proximity: return "tabletProximity:";
    case event_type::magnify: return "magnifyWithEvent:";
    case event_type::rotate: return "rotateWithEvent:";
    case event_type::swipe: return "swipeWithEvent:";
    case event_type::smart_magnify: return "smartMagnifyWithEvent:";
    case event_type::pressure: return "pressureChangeWithEvent:";
    case event_type::begin_gesture: return "beginGestureWithEvent:";
    case event_type::end_gesture: return "endGestureWithEvent:";
    case event_type::cursor_update: return "cursorUpdate:";
    default: return nullptr;
    }
}
} // namespace

void WindowSink::handle_event(const Event& event) {
    if (const auto owner = kit.lock()) owner->handle_window_event(event.window, event);
}
Size WindowSink::will_resize(WindowId window, Size proposed) {
    const auto owner = kit.lock();
    return owner ? owner->will_resize(window, proposed) : proposed;
}
std::optional<Rect> WindowSink::text_caret_rect(WindowId window) {
    const auto owner = kit.lock();
    return owner ? owner->caret_rect(window) : std::nullopt;
}

Address AppKit::window_for_id(WindowId id) {
    std::lock_guard lock(mutex);
    const auto found = windows.find(id);
    return found == windows.end() ? 0 : found->second;
}
Rect AppKit::content_rect(Address target) {
    const auto state = window_state(target);
    if (state->id) if (const auto server = window_server_or_null()) return from_server_content(target, server->content_rect(state->id));
    return state->last_content;
}
double AppKit::backing_scale(Address target) {
    if (target) {
        const auto state = window_state(target);
        if (state->id) if (const auto server = window_server_or_null()) return server->backing_scale(state->id);
    }
    if (const auto server = window_server_or_null()) {
        const auto list = server->screens();
        if (!list.empty()) return list.front().backing_scale;
    }
    return 1;
}

// The window-server caption: the title, and the subtitle after a dash (a
// Win32 title bar has one line).
std::string caption(AppKit& kit, const WindowState& state) {
    auto text = kit.text(state.title);
    if (const auto sub = kit.text(state.subtitle); !sub.empty()) text += (text.empty() ? "" : " \xE2\x80\x94 ") + sub;   // an em dash
    return text;
}

void AppKit::bring_to_front(Address target) {
    std::lock_guard lock(mutex);
    const auto found = std::find(window_order.begin(), window_order.end(), target);
    if (found == window_order.end()) return;
    window_order.erase(found);
    window_order.insert(window_order.begin(), target);
}
void AppKit::remove_from_order(Address target) {
    std::lock_guard lock(mutex);
    window_order.erase(std::remove(window_order.begin(), window_order.end(), target), window_order.end());
}

void AppKit::destroy_server_window(WindowState& state) {
    if (!state.id) return;
    const auto id = std::exchange(state.id, 0);
    end_drag(id);
    release_backing(state);
    update_layer_hosts(state.content_view);   // the window is gone: no presentation host
    {
        std::lock_guard lock(mutex);
        windows.erase(id);
        // A window's own sink callback is still running: destroy it after.
        if (dispatching == id) { pending_destroy.push_back(id); return; }
    }
    if (const auto server = window_server_or_null()) server->destroy_window(id);
}

void AppKit::window_order_front(Address target, bool make_key) {
    const auto state = window_state(target);
    if (!state->id) fail("ordering a closed NSWindow front");
    auto& server = require_window_server("-[NSWindow orderFront:]");
    {
        const OrderingScope ordering(ordering_window, target);
        server.show(state->id, make_key ? ShowCommand::make_key_and_order_front : ShowCommand::order_front);
    }
    state->visible = true;
    state->needs_display = true;
    bring_to_front(target);
    for (const auto child : state->children) {
        const auto info = window_state(child);
        if (info->visible && info->id) server.show(info->id, ShowCommand::order_front);
    }
    if (make_key && send_bool(target, "canBecomeKeyWindow")) set_key_window(target, true);
    schedule_occlusion_update(target);
    tab_window_ordered_in(target);
}

void AppKit::order_window(Address target, std::int64_t place, WindowId relative) {
    const auto state = window_state(target);
    if (place == order_out) {
        if (!state->id || !state->visible) return;
        tab_window_leaving(target);
        {
            const OrderingScope ordering(ordering_window, target);
            require_window_server("-[NSWindow orderOut:]").show(state->id, ShowCommand::order_out);
        }
        state->visible = false;
        set_key_window(target, false);
        schedule_occlusion_update(target);
        return;
    }
    if (!state->id) fail("ordering a closed NSWindow in");
    // Relative to a window that is not on screen (or 0, or an unknown
    // number): above or below every window.
    auto other = relative && relative != state->id ? window_for_id(relative) : 0;
    if (other && !(window_state(other)->visible && window_state(other)->id)) other = 0;
    if (!other && place == order_above) { window_order_front(target, false); return; }
    auto& server = require_window_server("-[NSWindow orderWindow:relativeTo:]");
    {
        const OrderingScope ordering(ordering_window, target);
        if (other) server.order_relative(state->id, window_state(other)->id, place == order_above);
        else server.show(state->id, ShowCommand::order_back);
    }
    state->visible = true;
    state->needs_display = true;
    {
        std::lock_guard lock(mutex);
        window_order.erase(std::remove(window_order.begin(), window_order.end(), target), window_order.end());
        auto at = other ? std::find(window_order.begin(), window_order.end(), other) : window_order.end();
        if (at != window_order.end() && place == order_below) ++at;
        window_order.insert(at, target);
    }
    if (place == order_above)
        for (const auto child : state->children) {
            const auto info = window_state(child);
            if (info->visible && info->id) server.order_relative(info->id, state->id, true);
        }
    schedule_occlusion_update(target);
    tab_window_ordered_in(target);
}

bool AppKit::occlusion_visible(const WindowState& state) const {
    return state.id && !state.closed && state.visible && !state.miniaturized && !state.server_hidden;
}

void AppKit::update_occlusion(Address target) {
    const auto state = window_state(target);
    const bool visible = occlusion_visible(*state);
    if (visible == state->occlusion_posted) return;
    state->occlusion_posted = visible;
    if (!state->closed) post(note::window_did_change_occlusion, target);
}

void AppKit::schedule_occlusion_update(Address target) {
    const auto state = window_state(target);
    if (state->occlusion_pending || target == ordering_window) return;
    const auto loop = run_loop_or_null();
    if (!loop) { update_occlusion(target); return; }
    state->occlusion_pending = true;
    // The window stays alive until the update ran.
    const auto keep = retain(target);
    static_cast<void>(loop->add_timer(monotonic_seconds(), 0, common_modes, [this, keep](TimerId) {
        try {
            window_state(keep)->occlusion_pending = false;
            update_occlusion(keep);
        } catch (...) {
            release(keep);
            throw;
        }
        release(keep);
    }));
}

void AppKit::window_did_resize(Address target, const std::shared_ptr<WindowState>& state, Rect content) {
    const auto previous = state->last_content;
    state->last_content = content;
    const bool resized = !same_size(previous.size, content.size), moved = !same_point(previous.origin, content.origin);
    if (resized) {
        if (state->content_view) {
            CallRegs registers;
            put_size(registers, 0, content.size);
            static_cast<void>(send(state->content_view, "setFrameSize:", registers));
        }
        state->needs_display = true;
        layout_frame_view(target);
        post(note::window_did_resize, target);
    }
    if (moved) post(note::window_did_move, target);
    if ((moved || resized) && state->autosave_name) autosave_frame(target);
}

// The window server may or may not report a frame change synchronously;
// either way the views and delegate see it once.
static void apply_content(AppKit& kit, Address target, Rect content) {
    const auto state = kit.window_state(target);
    if (!state->id) { state->last_content = content; return; }
    auto& server = require_window_server("NSWindow frame change");
    server.set_content_rect(state->id, kit.to_server_content(target, content));
    kit.window_did_resize(target, state, kit.from_server_content(target, server.content_rect(state->id)));
}

void AppKit::close_window(Address target) {
    const auto state = window_state(target);
    if (state->closed) return;
    // The window stays alive across the notifications even when it is
    // released when closed.
    const auto keep = retain(target);
    post(note::window_will_close, target);
    tab_window_leaving(target);
    state->closed = true;
    set_key_window(target, false);
    if (state->id) if (const auto server = window_server_or_null()) {
        if (state->visible) server->show(state->id, ShowCommand::order_out);
    }
    state->visible = false;
    state->occlusion_posted = false;   // a closed window posts no occlusion change
    destroy_server_window(*state);
    remove_from_order(target);
    if (state->parent) {
        auto& siblings = window_state(state->parent)->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), target), siblings.end());
        state->parent = 0;
    }
    // Its children stay, as top-level windows (the server released them).
    for (const auto child : std::exchange(state->children, {})) window_state(child)->parent = 0;
    // A document window: closing the document's last window (or one whose
    // controller -shouldCloseDocument) closes the document; otherwise the
    // controller leaves it.
    if (const auto controller = state->window_controller)
        if (const auto document = send(controller, "document").x0) {
            const auto controllers = send(document, "windowControllers").x0;
            const bool last = !controllers || send(controllers, "count").x0 <= 1;
            if (last || send_bool(controller, "shouldCloseDocument")) static_cast<void>(send(document, "close"));
            else send_id(document, "removeWindowController:", {controller});
        }
    const bool release_now = state->released_when_closed;
    release(keep);
    if (release_now) static_cast<void>(autorelease(target));
    // applicationShouldTerminateAfterLastWindowClosed:
    if (!app) return;
    bool any_visible{};
    {
        std::lock_guard lock(mutex);
        for (const auto value : window_order) any_visible = any_visible || window_state(value)->visible;
    }
    const auto delegate = app_state()->delegate;
    if (any_visible || !delegate) return;
    CallRegs registers;
    registers.x[2] = app;
    if (const auto answer = send_optional(delegate, "applicationShouldTerminateAfterLastWindowClosed:", registers))
        if (answer->x0 & 0xff) static_cast<void>(send_id(app, "terminate:", {0}));
}

bool AppKit::make_first_responder(Address target, Address responder) {
    const auto state = window_state(target);
    const auto current = state->first_responder ? state->first_responder : target;
    const auto wanted = responder ? responder : target;
    if (current == wanted) return true;
    if (current != target && !send_bool(current, "resignFirstResponder")) return false;
    state->first_responder = target;
    if (wanted != target && send_bool(wanted, "becomeFirstResponder")) {
        state->first_responder = wanted;
        // A text field that started editing hands first-responder status to
        // its field editor (AppKit's field editor model).
        if (text_field && is_kind(wanted, text_field) && objc.find_host_state<ViewState>(wanted))
            if (const auto editor = editing_field_editor(wanted)) state->first_responder = editor;
    }
    update_text_input(target);
    return true;
}

// The active NSTextInputContext is the key window's first responder's
// -inputContext (a guest override may answer nil: no text input there). As
// AppKit does, the previous context gets -deactivate and the new one
// -activate, which switch the window server's text input of their client's
// window (NSTextInputContext, view.cpp).
void AppKit::update_text_input(Address target) {
    const auto server = window_server_or_null();
    const auto state = window_state(target);
    if (!server || !state->id) return;
    const auto client = state->first_responder;
    Address context{};
    if (state->key && client && client != target)
        if (const auto answer = send_optional(client, "inputContext")) context = answer->x0;
    Address previous{};
    {
        std::lock_guard lock(mutex);
        previous = active_input_context;
    }
    const auto context_window = [this](Address value) -> Address {
        const auto info = objc.find_host_state<TextInputState>(value);
        const auto client = info && info->client ? objc.find_host_state<ViewState>(info->client) : nullptr;
        return client ? client->window : 0;
    };
    // A window that is not key only gives up the active context if it is its own.
    if (!state->key && previous && context_window(previous) != target) {
        server->set_text_input_enabled(state->id, false);
        return;
    }
    if (context != previous) {
        static_cast<void>(retain(context));
        {
            std::lock_guard lock(mutex);
            active_input_context = context;
        }
        try {
            if (previous) static_cast<void>(send(previous, "deactivate"));
            if (context) static_cast<void>(send(context, "activate"));
        } catch (...) { release(previous); throw; }
        release(previous);
    }
    if (!context) server->set_text_input_enabled(state->id, false);
}

// -[NSWindow sendEvent:]
void AppKit::window_send_event(Address target, Address value) {
    const auto state = window_state(target);
    const auto info = event_state(value);
    const auto type = info->event.type;
    const auto responder_or_window = [&] { return state->first_responder ? state->first_responder : target; };
    switch (type) {
    case event_type::key_down: static_cast<void>(send_id(responder_or_window(), "keyDown:", {value})); return;
    case event_type::key_up: static_cast<void>(send_id(responder_or_window(), "keyUp:", {value})); return;
    case event_type::flags_changed: static_cast<void>(send_id(responder_or_window(), "flagsChanged:", {value})); return;
    case event_type::left_mouse_down: case event_type::right_mouse_down: case event_type::other_mouse_down: {
        auto hit = hit_test(target, info->event.location);
        if (!hit) hit = state->content_view;
        if (!hit) return;
        if (type == event_type::left_mouse_down && hit != state->first_responder && send_bool(hit, "acceptsFirstResponder"))
            static_cast<void>(send_id(target, "makeFirstResponder:", {hit}));
        state->mouse_down_view = hit;
        static_cast<void>(send_id(hit, mouse_selector(type), {value}));
        return;
    }
    case event_type::left_mouse_up: case event_type::right_mouse_up: case event_type::other_mouse_up:
    case event_type::left_mouse_dragged: case event_type::right_mouse_dragged: case event_type::other_mouse_dragged: {
        update_tracking(target, value);
        auto receiver = state->mouse_down_view;
        if (!receiver) receiver = hit_test(target, info->event.location);
        if (!receiver) receiver = state->content_view;
        if (receiver) static_cast<void>(send_id(receiver, mouse_selector(type), {value}));
        if ((type == event_type::left_mouse_up || type == event_type::right_mouse_up || type == event_type::other_mouse_up) &&
            info->event.pressed_buttons == 0)
            state->mouse_down_view = 0;
        return;
    }
    case event_type::mouse_moved:
        update_tracking(target, value);
        if (state->accepts_mouse_moved) static_cast<void>(send_id(responder_or_window(), "mouseMoved:", {value}));
        return;
    case event_type::mouse_entered: case event_type::mouse_exited:
        // Window-level crossings reach views only through tracking areas.
        if (info->tracking_area) {
            const auto owner = objc.host_state<TrackingAreaState>(info->tracking_area)->owner;
            if (owner) static_cast<void>(send_id(owner, type == event_type::mouse_entered ? "mouseEntered:" : "mouseExited:", {value}));
        } else {
            update_tracking(target, value);
        }
        return;
    default:
        break;
    }
    if (const auto selector = view_event_selector(type)) {
        auto hit = hit_test(target, info->event.location);
        if (!hit) hit = state->content_view;
        if (hit) static_cast<void>(send_id(hit, selector, {value}));
    }
}

// ---- NSDraggingDestination ------------------------------------------------------------
// A file drag from another application (the window server's OLE drop target)
// runs AppKit's destination protocol: draggingEntered: when the pointer
// reaches a destination, draggingUpdated: while it moves (only on changes
// when the destination answers NO to wantsPeriodicDraggingUpdates),
// draggingExited: when it leaves or the drag is cancelled, and on a drop
// that the last answer accepted prepareForDragOperation: ->
// performDragOperation: -> concludeDragOperation:; draggingEnded: closes
// every drag. The destination is the deepest view under the pointer
// registered for a type the drag pasteboard offers, else the window when it
// is registered; a window destination that does not implement a message
// passes it to its delegate (NSWindow's own implementation). The pasteboard
// is the drag pasteboard holding the dragged files (NSFilenamesPboardType,
// NSPasteboardTypeFileURL).
namespace {
constexpr std::string_view drag_pasteboard_name = "Apple CFPasteboard drag";
const std::vector<std::string>& file_drag_types() {
    static const std::vector<std::string> types{"NSFilenamesPboardType", "public.file-url"};
    return types;
}
bool accepts_file_drag(const std::vector<std::string>& registered) {
    for (const auto& type : registered)
        if (std::find(file_drag_types().begin(), file_drag_types().end(), type) != file_drag_types().end()) return true;
    return false;
}
} // namespace

std::uint64_t WindowSink::drag_operation(WindowId window) {
    const auto owner = kit.lock();
    return owner ? owner->drag_answer(window) : gui::drag_operation::none;
}

std::uint64_t AppKit::drag_answer(WindowId id) {
    const auto found = drags.find(id);
    if (found == drags.end() || !found->second->destination) return gui::drag_operation::none;
    return found->second->answer;
}

void AppKit::end_drag(WindowId id) {
    const auto found = drags.find(id);
    if (found == drags.end()) return;
    const auto session = found->second;
    drags.erase(found);
    const auto destination = std::exchange(session->destination, 0), info = std::exchange(session->info, 0);
    release(destination);
    release(info);
}

void AppKit::handle_drag(Address target, const Event& event) {
    const auto state = window_state(target);
    const auto id = state->id;
    if (!id) return;
    // An application-modal session refuses drops on the other windows.
    if (!input_allowed_for(target)) { end_drag(id); return; }
    const auto destination_at = [&](Point location) -> Address {
        for (auto current = hit_test(target, location); current; current = view_state(current)->superview)
            if (accepts_file_drag(view_state(current)->dragged_types)) return current;
        return accepts_file_drag(state->dragged_types) ? target : 0;
    };
    // One destination message; nullopt when neither the destination (nor,
    // for the window, its delegate) implements it.
    const auto call = [&](Address receiver, std::string_view selector, Address info) {
        CallRegs registers;
        registers.x[2] = info;
        auto answer = send_optional(receiver, selector, registers);
        if (!answer && receiver == target && state->delegate) answer = send_optional(state->delegate, selector, registers);
        return answer;
    };
    // draggingEntered: of the session's destination; a destination that does
    // not implement it accepts nothing.
    const auto enter = [&](DragSession& session) {
        const auto answer = call(session.destination, "draggingEntered:", session.info);
        session.answer = answer ? answer->x0 : gui::drag_operation::none;
    };
    const auto finish = [&](DragSession& session) {
        if (session.destination && session.info) static_cast<void>(call(session.destination, "draggingEnded:", session.info));
        end_drag(id);
    };

    std::shared_ptr<DragSession> session;
    if (const auto found = drags.find(id); found != drags.end()) session = found->second;
    // A new drag while one is open: the earlier one never ended.
    if (session && event.notification == Notification::drag_entered) {
        if (session->destination) static_cast<void>(call(session->destination, "draggingExited:", session->info));
        finish(*session);
        session.reset();
    }
    if (!session) {
        if (event.notification == Notification::drag_exited) return;
        // A new drag (drag_entered, or a drop without one: the WM_DROPFILES
        // fallback): the drag pasteboard holds the dragged files.
        const auto board = pasteboard_named(drag_pasteboard_name);
        {
            const auto info = objc.host_state<PasteboardState>(board);
            info->file_paths.clear();
            info->strings.clear();
            for (const auto& path : event.paths) info->file_paths.push_back(guest_path_string(utf8_path(path)));
            info->declared = file_drag_types();
            ++info->change_count;
        }
        const auto info = objc.allocate(dragging_info);
        const auto drag = objc.host_state<DraggingState>(info);
        drag->pasteboard = retain(board);
        drag->window = target;
        drag->location = event.location;
        drag->source_mask = event.drag_operations;
        drag->sequence = ++drag_sequence;
        session = std::make_shared<DragSession>();
        session->info = info;
        drags[id] = session;
        if (const auto destination = destination_at(event.location)) {
            session->destination = retain(destination);
            enter(*session);
        }
        if (event.notification == Notification::drag_entered) return;
    }
    if (!session->info) return;   // ended by a destination message (the window closed)

    // The NSDraggingInfo stays alive across the guest's messages even when a
    // message ends the drag (closing the window).
    const auto info = retain(session->info);
    try {
        const auto drag = objc.host_state<DraggingState>(info);
        const bool changed = !same_point(drag->location, event.location) || drag->source_mask != event.drag_operations;
        drag->location = event.location;
        drag->source_mask = event.drag_operations;
        switch (event.notification) {
        case Notification::drag_updated:
        case Notification::files_dropped: {
            // The pointer may have crossed from one destination to another.
            const auto destination = destination_at(event.location);
            if (destination != session->destination) {
                if (session->destination) static_cast<void>(call(session->destination, "draggingExited:", info));
                if (!session->info) break;
                release(std::exchange(session->destination, 0));
                session->answer = gui::drag_operation::none;
                if (destination) {
                    session->destination = retain(destination);
                    enter(*session);
                }
            } else if (session->destination && event.notification == Notification::drag_updated) {
                bool update = true;
                if (!changed) {
                    // Unchanged position and mask: a periodic update, which
                    // the destination may decline.
                    if (const auto wants = send_optional(session->destination, "wantsPeriodicDraggingUpdates"))
                        update = (wants->x0 & 0xff) != 0;
                }
                // A destination without draggingUpdated: keeps its draggingEntered: answer.
                if (update)
                    if (const auto answer = call(session->destination, "draggingUpdated:", info)) session->answer = answer->x0;
            }
            if (event.notification == Notification::drag_updated || !session->info) break;
            // The drop: performed when the destination's last answer allows
            // an operation the source offers; otherwise it is cancelled.
            if (session->destination && (session->answer & drag->source_mask) != 0) {
                const auto prepared = call(session->destination, "prepareForDragOperation:", info);
                if (session->info && (!prepared || (prepared->x0 & 0xff))) {
                    const auto performed = call(session->destination, "performDragOperation:", info);
                    if (session->info && performed && (performed->x0 & 0xff))
                        static_cast<void>(call(session->destination, "concludeDragOperation:", info));
                }
            } else if (session->destination) {
                static_cast<void>(call(session->destination, "draggingExited:", info));
            }
            finish(*session);
            break;
        }
        case Notification::drag_exited:
            if (session->destination) static_cast<void>(call(session->destination, "draggingExited:", info));
            finish(*session);
            break;
        default:
            break;
        }
    } catch (...) {
        try { end_drag(id); release(info); } catch (...) {}
        throw;
    }
    release(info);
}

Size AppKit::will_resize(WindowId id, Size proposed) {
    const auto target = window_for_id(id);
    if (!target) return proposed;
    const auto state = window_state(target);
    const auto strip = state->strip;
    proposed.height = std::max(0.0, proposed.height - strip);
    // The aspect ratio keeps the width the user chose and derives the height.
    if (const auto ratio = state->content_aspect_ratio; ratio.width > 0 && ratio.height > 0) {
        proposed.height = proposed.width * ratio.height / ratio.width;
    } else if (const auto frame_ratio = state->aspect_ratio; frame_ratio.width > 0 && frame_ratio.height > 0) {
        auto& server = require_window_server("-[NSWindow aspectRatio]");
        const auto frame = server.frame_for_content({{0, 0}, {proposed.width, proposed.height + strip}}, state->style).size;
        const double chrome = frame.height - proposed.height;
        proposed.height = std::max(0.0, frame.width * frame_ratio.height / frame_ratio.width - chrome);
    }
    if (!state->delegate || !responds(state->delegate, "windowWillResize:toSize:")) return {proposed.width, proposed.height + strip};
    auto& server = require_window_server("windowWillResize:toSize:");
    // The delegate works in frame sizes; the window server in content sizes.
    const auto frame = server.frame_for_content({{0, 0}, {proposed.width, proposed.height + strip}}, state->style);
    CallRegs registers;
    registers.x[2] = target;
    put_size(registers, 0, frame.size);
    const auto answer = result_size(send(state->delegate, "windowWillResize:toSize:", registers));
    return server.content_for_frame({{0, 0}, answer}, state->style).size;   // the server's content size (with the strip)
}

std::optional<Rect> AppKit::caret_rect(WindowId id) {
    const auto target = window_for_id(id);
    if (!target) return std::nullopt;
    const auto client = window_state(target)->first_responder;
    if (!client || client == target || !responds(client, "firstRectForCharacterRange:actualRange:")) return std::nullopt;
    CallRegs range;
    if (responds(client, "selectedRange")) {
        const auto selected = send(client, "selectedRange");
        range.x[2] = selected.x0; range.x[3] = selected.x1;
    }
    range.x[4] = 0;   // actualRange: NULL
    const auto screen_rect = result_rect(send(client, "firstRectForCharacterRange:actualRange:", range));
    const auto content = content_rect(target);
    return Rect{{screen_rect.origin.x - content.origin.x, screen_rect.origin.y - content.origin.y}, screen_rect.size};
}

void AppKit::handle_window_event(WindowId id, const Event& event) {
    const auto saved = dispatching;
    dispatching = id;
    const auto finish = [&]() {
        dispatching = saved;
        std::vector<WindowId> destroy;
        {
            std::lock_guard lock(mutex);
            for (auto it = pending_destroy.begin(); it != pending_destroy.end();) {
                if (*it != dispatching) { destroy.push_back(*it); it = pending_destroy.erase(it); }
                else ++it;
            }
        }
        if (const auto server = window_server_or_null()) for (const auto value : destroy) server->destroy_window(value);
    };
    const auto target = window_for_id(id);
    try {
        if (event.kind != EventKind::notification && !input_allowed_for(target)) {
            // An application-modal session (-[NSAlert runModal]) discards
            // input for the application's windows, a modal session
            // (-runModalForWindow:, modal.cpp) for the windows other than
            // its own, as AppKit does.
        } else if (event.kind == EventKind::input) {
            // A key press's characters, for the WM_CHAR a translated key
            // message produces right after it (see below).
            const bool keystroke = event.type == event_type::key_down && !event.characters.empty();
            last_keystroke = keystroke ? std::optional<std::pair<WindowId, std::string>>{{id, event.characters}} : std::nullopt;
            if (target || event.window == 0) enqueue(make_event(event, target));
        } else if (event.kind == EventKind::text) {
            // Win32 loops that translate key messages (dialog and size/move
            // loops call TranslateMessage; tools post WM_KEYDOWN + WM_CHAR)
            // follow a key press with a WM_CHAR of the same characters: the
            // key event already carries that keystroke.
            const bool repeated = !event.marked && last_keystroke && last_keystroke->first == id &&
                                  last_keystroke->second == event.text;
            last_keystroke.reset();
            if (target && !repeated) enqueue_text(target, event);
        } else {
            // Application-wide notifications.
            switch (event.notification) {
            case Notification::application_activated: if (app) set_active(true); break;
            case Notification::application_deactivated: if (app) set_active(false); break;
            case Notification::quit_requested: if (app) static_cast<void>(send_id(app, "terminate:", {0})); break;
            case Notification::screens_changed:
                if (const auto server = window_server_or_null()) {
                    std::lock_guard lock(mutex);
                    for (const auto& info : server->screens())
                        if (const auto found = screens.find(info.id); found != screens.end())
                            objc.host_state<ScreenState>(found->second)->info = info;
                }
                if (app) post(note::screen_parameters, app);   // the delegate observes it (setDelegate:)
                break;
            case Notification::keyboard_layout_changed: post(note::keyboard_selection, 0); break;
            default: break;
            }
            if (target) {
                const auto state = window_state(target);
                switch (event.notification) {
                case Notification::close_requested: static_cast<void>(send_id(target, "performClose:", {0})); break;
                case Notification::resized: case Notification::moved: window_did_resize(target, state, from_server_content(target, event.rect)); break;
                case Notification::expose: {
                    post(note::window_did_expose, target);
                    const auto content = state->content_view;
                    if (content && state->visible) {
                        if (window_uses_backing(target)) {
                            // Redraw the exposed part from the views (the
                            // backing store may be stale or reallocated).
                            state->dirty = state->backing ? drawing::unite(state->dirty, event.rect) : Rect{{0, 0}, event.rect.size};
                            if (!state->backing) state->needs_display = true;
                            display_window(target);
                        } else {
                            state->needs_display = true;
                            display_window(target, true);
                        }
                    } else {
                        state->needs_display = true;
                    }
                    break;
                }
                case Notification::became_key: set_key_window(target, true); break;
                case Notification::resigned_key: set_key_window(target, false); break;
                case Notification::miniaturized:
                    post(note::window_will_miniaturize, target);
                    state->miniaturized = true;
                    post(note::window_did_miniaturize, target);
                    schedule_occlusion_update(target);
                    break;
                case Notification::deminiaturized:
                    state->miniaturized = false;
                    post(note::window_did_deminiaturize, target);
                    schedule_occlusion_update(target);
                    break;
                case Notification::entered_full_screen:
                    if (!state->full_screen) {
                        state->full_screen = true;
                        post(note::window_did_enter_full_screen, target);
                    }
                    break;
                case Notification::exited_full_screen:
                    if (state->full_screen) {
                        state->full_screen = false;
                        post(note::window_did_exit_full_screen, target);
                    }
                    break;
                case Notification::backing_changed: {
                    const auto scale = event.backing_scale;
                    for (const auto value : all_views(state->content_view)) {
                        const auto info = view_state(value);
                        if (!info->layer || !info->backing_layer) continue;
                        CallRegs registers;
                        registers.set_d(0, scale);
                        static_cast<void>(send_optional(info->layer, "setContentsScale:", registers));
                    }
                    update_layer_hosts(state->content_view);
                    post(note::window_did_change_backing, target);
                    post(note::window_did_change_screen, target);
                    break;
                }
                case Notification::occlusion_changed:
                    // The server reports while it shows or hides the window,
                    // before -isVisible changes: the state is posted once
                    // both agree.
                    state->server_hidden = !event.visible;
                    schedule_occlusion_update(target);
                    break;
                case Notification::live_resize_started:
                    state->live_resize = true;
                    post(note::window_will_start_live_resize, target);
                    if (state->content_view) static_cast<void>(send_optional(state->content_view, "viewWillStartLiveResize"));
                    break;
                case Notification::live_resize_ended:
                    state->live_resize = false;
                    if (state->content_view) static_cast<void>(send_optional(state->content_view, "viewDidEndLiveResize"));
                    post(note::window_did_end_live_resize, target);
                    if (state->content_view) display_window(target, true);
                    break;
                case Notification::modal_tick:
                    // Inside a Win32 modal loop: timers fire and windows redraw.
                    if (const auto loop = run_loop_or_null()) static_cast<void>(loop->run_once(event_tracking_mode, 0, false));
                    display_pending();
                    break;
                case Notification::drag_entered: case Notification::drag_updated: case Notification::drag_exited:
                case Notification::files_dropped:
                    handle_drag(target, event);
                    break;
                default: break;
                }
            }
        }
    } catch (...) {
        try { finish(); } catch (...) {}
        throw;
    }
    finish();
}

// ---- NSWindow ---------------------------------------------------------------------------
// NSWindowDelegate's notification methods (NSWindow.h).
const AppKit::DelegateNotification AppKit::window_delegate_notifications[] = {
    {note::window_did_become_key, "windowDidBecomeKey:"}, {note::window_did_resign_key, "windowDidResignKey:"},
    {note::window_did_become_main, "windowDidBecomeMain:"}, {note::window_did_resign_main, "windowDidResignMain:"},
    {note::window_did_resize, "windowDidResize:"}, {note::window_did_move, "windowDidMove:"},
    {note::window_will_move, "windowWillMove:"}, {note::window_will_close, "windowWillClose:"},
    {note::window_did_expose, "windowDidExpose:"}, {note::window_did_update, "windowDidUpdate:"},
    {note::window_will_miniaturize, "windowWillMiniaturize:"}, {note::window_did_miniaturize, "windowDidMiniaturize:"},
    {note::window_did_deminiaturize, "windowDidDeminiaturize:"},
    {note::window_will_enter_full_screen, "windowWillEnterFullScreen:"}, {note::window_did_enter_full_screen, "windowDidEnterFullScreen:"},
    {note::window_will_exit_full_screen, "windowWillExitFullScreen:"}, {note::window_did_exit_full_screen, "windowDidExitFullScreen:"},
    {note::window_did_change_backing, "windowDidChangeBackingProperties:"}, {note::window_did_change_screen, "windowDidChangeScreen:"},
    {note::window_did_change_screen_profile, "windowDidChangeScreenProfile:"},
    {note::window_did_change_occlusion, "windowDidChangeOcclusionState:"},
    {note::window_will_start_live_resize, "windowWillStartLiveResize:"}, {note::window_did_end_live_resize, "windowDidEndLiveResize:"},
    {note::window_will_begin_sheet, "windowWillBeginSheet:"}, {note::window_did_end_sheet, "windowDidEndSheet:"},
};

void AppKit::install_window() {
    const auto w = window;
    const auto init = [this](Address self, Rect rect, std::uint64_t style) {
        auto& server = require_window_server("NSWindow creation");
        const auto state = window_state(self);
        if (state->id || state->closed) fail("NSWindow initialized twice");
        state->style = style & ~style::full_screen;
        state->last_content = rect;
        state->sink = std::make_shared<WindowSink>(weak_from_this());
        WindowDesc description;
        description.content_rect = rect;
        description.style_mask = state->style;
        description.owner = self;
        state->id = server.create_window(description, state->sink);
        state->last_content = server.content_rect(state->id);
        {
            std::lock_guard lock(mutex);
            windows[state->id] = self;
            window_order.push_back(self);
        }
        // The frame view, then a plain content view filling it.
        update_frame_view(self);
        CallRegs frame;
        put_rect(frame, 0, {{0, 0}, state->last_content.size});
        const auto content = send(send(view, "alloc").x0, "initWithFrame:", frame).x0;
        static_cast<void>(send_id(self, "setContentView:", {content}));
        release(content);
        return self;
    };
    method(w, "initWithContentRect:styleMask:backing:defer:", [init](CpuState& s, GuestMemory&) {
        s.x[0] = init(s.x[0], rect_arg(s), s.x[2]);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}QQB");
    method(w, "initWithContentRect:styleMask:backing:defer:screen:", [this, init](CpuState& s, GuestMemory&) {
        auto rect = rect_arg(s);
        // The rectangle is relative to the given screen.
        if (s.x[5]) {
            const auto origin = objc.host_state<ScreenState>(s.x[5])->info.frame.origin;
            rect.origin.x += origin.x; rect.origin.y += origin.y;
        }
        s.x[0] = init(s.x[0], rect, s.x[2]);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}QQB@");
    method(w, "init", [init](CpuState& s, GuestMemory&) {
        s.x[0] = init(s.x[0], {}, style::titled | style::closable | style::miniaturizable | style::resizable);
    });
    method(w, "dealloc", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        object_deallocated(self);
        if (const auto state = objc.find_host_state<WindowState>(self)) {
            if (const auto delegate = std::exchange(state->delegate, 0)) observe_delegate(self, delegate, 0, window_delegate_notifications);
            // Not retained either way: the parent and children forget it.
            if (const auto parent = std::exchange(state->parent, 0))
                if (const auto info = objc.find_host_state<WindowState>(parent))
                    info->children.erase(std::remove(info->children.begin(), info->children.end(), self), info->children.end());
            for (const auto child : std::exchange(state->children, {}))
                if (const auto info = objc.find_host_state<WindowState>(child)) info->parent = 0;
            destroy_server_window(*state);
            release(std::exchange(state->field_editor, 0));
            {
                std::lock_guard lock(mutex);
                if (key_window == self) key_window = 0;
                if (main_window == self) main_window = 0;
                if (deferred_key_window == self) deferred_key_window = 0;
            }
            remove_from_order(self);
            std::vector<Address> stale;
            {
                std::lock_guard lock(mutex);
                for (auto it = queue.begin(); it != queue.end();) {
                    if (event_state(*it)->window == self) { stale.push_back(*it); it = queue.erase(it); }
                    else ++it;
                }
            }
            for (const auto value : stale) release(value);
            if (const auto content = std::exchange(state->content_view, 0)) {
                detach_from_frame_view(self, content);
                set_view_window(content, 0);
                release(content);
            }
            release_window_tabs(*state);
            if (const auto frame_view = std::exchange(state->frame_view, 0)) { set_view_window(frame_view, 0); release(frame_view); }
            for (auto* slot : {&state->title, &state->represented_filename, &state->background_color, &state->appearance,
                     &state->autosave_name, &state->subtitle, &state->identifier, &state->content_view_controller})
                release(std::exchange(*slot, 0));
            if (const auto handler = std::exchange(state->sheet_handler, 0)) static_cast<void>(guest_call(libsystem_library, "__Block_release", {handler}));
            state->sink.reset();
        }
        static_cast<void>(objc.send_super(self, window, sel("dealloc")));
    }, "v@:");

    method(w, "windowNumber", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->id; }, "q@:");
    method(w, "contentView", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->content_view; });
    method(w, "setContentView:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], value = s.x[2];
        const auto state = window_state(self);
        const auto old = state->content_view;
        if (old == value) return;
        state->content_view = retain(value);
        if (old) {
            if (state->first_responder == old) state->first_responder = self;
            if (state->mouse_down_view == old) state->mouse_down_view = 0;
            view_state(old)->next_responder = 0;
            detach_from_frame_view(self, old);
            set_view_window(old, 0);
            release(old);
        }
        if (!value) return;
        const auto info = view_state(value);
        if (info->superview) static_cast<void>(send(value, "removeFromSuperview"));
        attach_to_frame_view(self, value);
        CallRegs frame;
        put_rect(frame, 0, {{0, 0}, content_rect(self).size});
        static_cast<void>(send(value, "setFrame:", frame));
        info->next_responder = self;
        set_view_window(value, self);
        state->needs_display = true;
    }, "v@:@");
    method(w, "delegate", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->delegate; });
    // The delegate hears the window's notifications through the default
    // center (object = the window), for each method it implements, as in
    // AppKit; it is not retained.
    method(w, "setDelegate:", [this](CpuState& s, GuestMemory&) {
        const auto previous = std::exchange(window_state(s.x[0])->delegate, s.x[2]);
        observe_delegate(s.x[0], previous, s.x[2], window_delegate_notifications);
    }, "v@:@");
    method(w, "title", [this](CpuState& s, GuestMemory&) {
        const auto title = window_state(s.x[0])->title;
        s.x[0] = title ? title : auto_string("");
    });
    method(w, "setTitle:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        const auto copy = s.x[2] ? cf.copy(s.x[2]) : string("");
        release(std::exchange(state->title, copy));
        if (state->id) require_window_server("-[NSWindow setTitle:]").set_title(state->id, caption(*this, *state));
        if (state->tab_group)
            for (const auto member : send_id(state->tab_group, "windows") ? cf.array_values(send_id(state->tab_group, "windows")) : std::vector<Address>{})
                if (const auto bar = window_state(member)->tab_bar_view) invalidate_view(bar);
    }, "v@:@");
    method(w, "representedFilename", [this](CpuState& s, GuestMemory&) {
        const auto value = window_state(s.x[0])->represented_filename;
        s.x[0] = value ? value : auto_string("");
    });
    // Windows has no title-bar proxy icon; the name is kept for the getter.
    method(w, "setRepresentedFilename:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        release(std::exchange(state->represented_filename, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(w, "setTitleWithRepresentedFilename:", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(s.x[0], "setRepresentedFilename:", {s.x[2]}));
        const auto path = text(s.x[2]);
        const auto slash = path.find_last_of('/');
        static_cast<void>(send_id(s.x[0], "setTitle:", {auto_string(slash == std::string::npos ? path : path.substr(slash + 1))}));
    }, "v@:@");
    method(w, "isDocumentEdited", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->document_edited); }, "B@:");
    method(w, "setDocumentEdited:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->document_edited = bool_arg(s, 2); }, "v@:B");

    // ---- geometry ----
    method(w, "frame", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (state->id) return_rect(s, require_window_server("-[NSWindow frame]").frame_rect(state->id));
        else return_rect(s, state->last_content);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(w, "setFrame:display:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        apply_content(*this, s.x[0], from_server_content(s.x[0], require_window_server("-[NSWindow setFrame:display:]").content_for_frame(rect_arg(s), state->style)));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}B");
    method(w, "setFrame:display:animate:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        apply_content(*this, s.x[0], from_server_content(s.x[0], require_window_server("-[NSWindow setFrame:display:animate:]").content_for_frame(rect_arg(s), state->style)));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}BB");
    method(w, "setFrameOrigin:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        auto& server = require_window_server("-[NSWindow setFrameOrigin:]");
        auto frame = state->id ? server.frame_rect(state->id) : state->last_content;
        frame.origin = point_arg(s);
        apply_content(*this, s.x[0], from_server_content(s.x[0], server.content_for_frame(frame, state->style)));
    }, "v@:{CGPoint=dd}");
    method(w, "setFrameTopLeftPoint:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        auto& server = require_window_server("-[NSWindow setFrameTopLeftPoint:]");
        auto frame = state->id ? server.frame_rect(state->id) : state->last_content;
        const auto point = point_arg(s);
        frame.origin = {point.x, point.y - frame.size.height};
        apply_content(*this, s.x[0], from_server_content(s.x[0], server.content_for_frame(frame, state->style)));
    }, "v@:{CGPoint=dd}");
    method(w, "setContentSize:", [this](CpuState& s, GuestMemory&) {
        // The top-left corner stays in place.
        const auto current = content_rect(s.x[0]);
        const auto size = size_arg(s);
        apply_content(*this, s.x[0], {{current.origin.x, current.origin.y + current.size.height - size.height}, size});
    }, "v@:{CGSize=dd}");
    method(w, "center", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        auto& server = require_window_server("-[NSWindow center]");
        const auto screen_id = state->id ? server.window_screen(state->id) : 0;
        const auto list = server.screens();
        if (list.empty()) return;
        auto area = list.front().visible_frame;
        for (const auto& info : list) if (info.id == screen_id) area = info.visible_frame;
        const auto frame = server.frame_rect(state->id);
        Rect centered{{area.origin.x + (area.size.width - frame.size.width) / 2,
                       area.origin.y + (area.size.height - frame.size.height) * 2 / 3}, frame.size};
        apply_content(*this, s.x[0], from_server_content(s.x[0], server.content_for_frame(centered, state->style)));
    }, "v@:");
    method(w, "contentRectForFrameRect:", [this](CpuState& s, GuestMemory&) {
        return_rect(s, from_server_content(s.x[0], require_window_server("-[NSWindow contentRectForFrameRect:]").content_for_frame(rect_arg(s), window_state(s.x[0])->style)));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(w, "frameRectForContentRect:", [this](CpuState& s, GuestMemory&) {
        return_rect(s, require_window_server("-[NSWindow frameRectForContentRect:]").frame_for_content(to_server_content(s.x[0], rect_arg(s)), window_state(s.x[0])->style));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_method(w, "contentRectForFrameRect:styleMask:", [](CpuState& s, GuestMemory&) {
        return_rect(s, require_window_server("+[NSWindow contentRectForFrameRect:styleMask:]").content_for_frame(rect_arg(s), s.x[2]));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}Q");
    class_method(w, "frameRectForContentRect:styleMask:", [](CpuState& s, GuestMemory&) {
        return_rect(s, require_window_server("+[NSWindow frameRectForContentRect:styleMask:]").frame_for_content(rect_arg(s), s.x[2]));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}Q");
    method(w, "backingScaleFactor", [this](CpuState& s, GuestMemory&) { set_d(s, 0, backing_scale(s.x[0])); }, "d@:");
    const auto scaled_rect = [this](CpuState& s, bool to_backing) {
        const auto scale = backing_scale(s.x[0]);
        const auto factor = to_backing ? scale : 1 / scale;
        const auto r = rect_arg(s);
        return_rect(s, {{r.origin.x * factor, r.origin.y * factor}, {r.size.width * factor, r.size.height * factor}});
    };
    const auto scaled_point = [this](CpuState& s, bool to_backing) {
        const auto scale = backing_scale(s.x[0]);
        const auto factor = to_backing ? scale : 1 / scale;
        const auto p = point_arg(s);
        return_point(s, {p.x * factor, p.y * factor});
    };
    method(w, "convertRectToBacking:", [scaled_rect](CpuState& s, GuestMemory&) { scaled_rect(s, true); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(w, "convertRectFromBacking:", [scaled_rect](CpuState& s, GuestMemory&) { scaled_rect(s, false); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(w, "convertPointToBacking:", [scaled_point](CpuState& s, GuestMemory&) { scaled_point(s, true); }, "{CGPoint=dd}@:{CGPoint=dd}");
    method(w, "convertPointFromBacking:", [scaled_point](CpuState& s, GuestMemory&) { scaled_point(s, false); }, "{CGPoint=dd}@:{CGPoint=dd}");
    const auto screen_rect = [this](CpuState& s, double sign) {
        const auto origin = content_rect(s.x[0]).origin;
        const auto r = rect_arg(s);
        return_rect(s, {{r.origin.x + sign * origin.x, r.origin.y + sign * origin.y}, r.size});
    };
    const auto screen_point = [this](CpuState& s, double sign) {
        const auto origin = content_rect(s.x[0]).origin;
        const auto p = point_arg(s);
        return_point(s, {p.x + sign * origin.x, p.y + sign * origin.y});
    };
    method(w, "convertRectToScreen:", [screen_rect](CpuState& s, GuestMemory&) { screen_rect(s, 1); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(w, "convertRectFromScreen:", [screen_rect](CpuState& s, GuestMemory&) { screen_rect(s, -1); },
           "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    for (const auto* name : {"convertPointToScreen:", "convertBaseToScreen:"})
        method(w, name, [screen_point](CpuState& s, GuestMemory&) { screen_point(s, 1); }, "{CGPoint=dd}@:{CGPoint=dd}");
    for (const auto* name : {"convertPointFromScreen:", "convertScreenToBase:"})
        method(w, name, [screen_point](CpuState& s, GuestMemory&) { screen_point(s, -1); }, "{CGPoint=dd}@:{CGPoint=dd}");
    method(w, "mouseLocationOutsideOfEventStream", [this](CpuState& s, GuestMemory&) {
        const auto location = require_window_server("-[NSWindow mouseLocationOutsideOfEventStream]").mouse_location();
        const auto origin = content_rect(s.x[0]).origin;
        return_point(s, {location.x - origin.x, location.y - origin.y});
    }, "{CGPoint=dd}@:");
    method(w, "screen", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        const auto server = window_server_or_null();
        s.x[0] = state->id && server ? screen_object(server->window_screen(state->id)) : main_screen();
    });
    method(w, "deepestScreen", [this](CpuState& s, GuestMemory&) { s.x[0] = send(s.x[0], "screen").x0; });

    // ---- ordering and key state ----
    // -orderWindow:relativeTo: is the primitive; -makeKeyAndOrderFront: sends
    // it, so a subclass that overrides it sees the window come on screen
    // (Chrome's NativeWidgetMacNSWindow tracks its visibility there). The
    // other ordering methods order the window themselves and never send it:
    // such an override may call them (Chrome's headless windows answer
    // -orderWindow:relativeTo: with -orderFront:), which on AppKit does not
    // come back to the override.
    method(w, "orderWindow:relativeTo:", [this](CpuState& s, GuestMemory&) {
        order_window(s.x[0], static_cast<std::int64_t>(s.x[2]), static_cast<WindowId>(s.x[3]));
    }, "v@:qq");
    method(w, "makeKeyAndOrderFront:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        CallRegs registers;
        registers.x[2] = static_cast<Address>(order_above);
        registers.x[3] = 0;
        static_cast<void>(send(self, "orderWindow:relativeTo:", registers));
        if (send_bool(self, "canBecomeKeyWindow")) static_cast<void>(send(self, "makeKeyWindow"));
    }, "v@:@");
    method(w, "orderFront:", [this](CpuState& s, GuestMemory&) { order_window(s.x[0], order_above, 0); }, "v@:@");
    method(w, "orderFrontRegardless", [this](CpuState& s, GuestMemory&) { order_window(s.x[0], order_above, 0); }, "v@:");
    method(w, "orderBack:", [this](CpuState& s, GuestMemory&) { order_window(s.x[0], order_below, 0); }, "v@:@");
    method(w, "orderOut:", [this](CpuState& s, GuestMemory&) { order_window(s.x[0], order_out, 0); }, "v@:@");
    method(w, "setIsVisible:", [this](CpuState& s, GuestMemory&) { order_window(s.x[0], bool_arg(s, 2) ? order_above : order_out, 0); }, "v@:B");
    method(w, "isVisible", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->visible); }, "B@:");
    method(w, "isKeyWindow", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->key); }, "B@:");
    method(w, "isMainWindow", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->main); }, "B@:");
    const auto can_become_key = [this](CpuState& s, GuestMemory&) {
        const auto style = window_state(s.x[0])->style;
        return_bool(s, (style & (style::titled | style::resizable)) != 0);
    };
    method(w, "canBecomeKeyWindow", can_become_key, "B@:");
    method(w, "canBecomeMainWindow", can_become_key, "B@:");
    method(w, "makeKeyWindow", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = window_state(self);
        if (!state->id || !send_bool(self, "canBecomeKeyWindow")) return;
        {
            const OrderingScope ordering(ordering_window, self);
            require_window_server("-[NSWindow makeKeyWindow]").show(state->id, ShowCommand::make_key_and_order_front);
        }
        state->visible = true;
        set_key_window(self, true);
        schedule_occlusion_update(self);
    }, "v@:");
    method(w, "makeMainWindow", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        main_window = s.x[0]; window_state(s.x[0])->main = true;
    }, "v@:");
    // Override points; the notifications are posted by the key-window change.
    for (const auto* name : {"becomeKeyWindow", "resignKeyWindow", "becomeMainWindow", "resignMainWindow"})
        method(w, name, [](CpuState&, GuestMemory&) {}, "v@:");
    method(w, "isOnActiveSpace", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");   // one desktop
    method(w, "occlusionState", [this](CpuState& s, GuestMemory&) {
        s.x[0] = occlusion_visible(*window_state(s.x[0])) ? occlusion_state_visible : 0;
    }, "Q@:");

    // ---- closing, miniaturizing, zooming, full screen ----
    method(w, "close", [this](CpuState& s, GuestMemory&) { close_window(s.x[0]); }, "v@:");
    method(w, "performClose:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = window_state(self);
        if (!(state->style & style::closable)) { if (const auto server = window_server_or_null()) server->beep(); return; }
        CallRegs registers;
        registers.x[2] = self;
        std::optional<CallResult> answer;
        if (state->delegate) answer = send_optional(state->delegate, "windowShouldClose:", registers);
        if (!answer) answer = send_optional(self, "windowShouldClose:", registers);
        if (answer && !(answer->x0 & 0xff)) return;
        static_cast<void>(send(self, "close"));
    }, "v@:@");
    method(w, "miniaturize:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (state->id) require_window_server("-[NSWindow miniaturize:]").miniaturize(state->id, true);
    }, "v@:@");
    method(w, "deminiaturize:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (state->id) require_window_server("-[NSWindow deminiaturize:]").miniaturize(state->id, false);
    }, "v@:@");
    method(w, "performMiniaturize:", [this](CpuState& s, GuestMemory&) {
        if (window_state(s.x[0])->style & style::miniaturizable) static_cast<void>(send_id(s.x[0], "miniaturize:", {s.x[2]}));
        else if (const auto server = window_server_or_null()) server->beep();
    }, "v@:@");
    method(w, "isMiniaturized", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->miniaturized); }, "B@:");
    method(w, "zoom:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = window_state(self);
        if (!state->id) return;
        auto& server = require_window_server("-[NSWindow zoom:]");
        if (state->delegate && responds(state->delegate, "windowShouldZoom:toFrame:")) {
            CallRegs registers;
            registers.x[2] = self;
            const auto list = server.screens();
            const auto screen_id = server.window_screen(state->id);
            Rect standard = list.empty() ? Rect{} : list.front().visible_frame;
            for (const auto& info : list) if (info.id == screen_id) standard = info.visible_frame;
            put_rect(registers, 0, standard);
            if (!(send(state->delegate, "windowShouldZoom:toFrame:", registers).x0 & 0xff)) return;
        }
        server.zoom(state->id);
    }, "v@:@");
    method(w, "performZoom:", [this](CpuState& s, GuestMemory&) {
        if (window_state(s.x[0])->style & style::resizable) static_cast<void>(send_id(s.x[0], "zoom:", {s.x[2]}));
        else if (const auto server = window_server_or_null()) server->beep();
    }, "v@:@");
    method(w, "isZoomed", [this](CpuState& s, GuestMemory&) {
        // Zoomed: the frame fills its screen's visible area (the standard frame).
        const auto state = window_state(s.x[0]);
        const auto server = window_server_or_null();
        if (!state->id || !server) { s.x[0] = 0; return; }
        const auto frame = server->frame_rect(state->id);
        const auto screen_id = server->window_screen(state->id);
        for (const auto& info : server->screens()) {
            if (info.id != screen_id) continue;
            const auto& area = info.visible_frame;
            constexpr double slack = 16;   // Win32 invisible resize borders
            return_bool(s, std::abs(frame.size.width - area.size.width) <= slack &&
                               std::abs(frame.size.height - area.size.height) <= slack);
            return;
        }
        s.x[0] = 0;
    }, "B@:");
    method(w, "toggleFullScreen:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = window_state(self);
        if (!state->id) return;
        const bool entering = !state->full_screen;
        if (entering) post(note::window_will_enter_full_screen, self);
        else post(note::window_will_exit_full_screen, self);
        require_window_server("-[NSWindow toggleFullScreen:]").set_full_screen(state->id, entering);
    }, "v@:@");
    method(w, "styleMask", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        s.x[0] = state->style | (state->full_screen ? style::full_screen : 0);
    }, "Q@:");
    method(w, "setStyleMask:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        state->style = s.x[2] & ~style::full_screen;
        if (state->id) require_window_server("-[NSWindow setStyleMask:]").set_style(state->id, state->style);
        if (state->frame_view) update_frame_view(s.x[0]);
    }, "v@:Q");
    method(w, "level", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(window_state(s.x[0])->level); }, "q@:");
    method(w, "setLevel:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        state->level = static_cast<WindowLevel>(s.x[2]);
        if (state->id) require_window_server("-[NSWindow setLevel:]").set_level(state->id, state->level);
    }, "v@:q");

    // ---- responders and input ----
    method(w, "firstResponder", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        s.x[0] = state->first_responder ? state->first_responder : s.x[0];
    });
    method(w, "makeFirstResponder:", [this](CpuState& s, GuestMemory&) { return_bool(s, make_first_responder(s.x[0], s.x[2])); }, "B@:@");
    // The key view loop: the next / previous valid key view of a view (the
    // first responder, else the initial first responder).
    const auto select_key_view = [this](Address target, Address from, bool forward) {
        const auto state = window_state(target);
        if (!from) from = state->first_responder && is_kind(state->first_responder, view) ? state->first_responder
                                                                                           : state->initial_first_responder;
        if (!from) return;
        const auto chosen = send_id(from, forward ? "nextValidKeyView" : "previousValidKeyView");
        if (!chosen) return;
        // NSSelectingNext 1 / NSSelectingPrevious 2 while the chosen view
        // becomes first responder (its -becomeFirstResponder may ask).
        state->key_view_direction = forward ? 1 : 2;
        struct Reset { std::uint64_t& direction; ~Reset() { direction = 0; } } reset{state->key_view_direction};
        static_cast<void>(make_first_responder(target, chosen));
    };
    method(w, "keyViewSelectionDirection", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->key_view_direction; }, "Q@:");
    method(w, "selectNextKeyView:", [select_key_view](CpuState& s, GuestMemory&) { select_key_view(s.x[0], 0, true); }, "v@:@");
    method(w, "selectPreviousKeyView:", [select_key_view](CpuState& s, GuestMemory&) { select_key_view(s.x[0], 0, false); }, "v@:@");
    method(w, "selectKeyViewFollowingView:", [select_key_view](CpuState& s, GuestMemory&) { select_key_view(s.x[0], s.x[2], true); }, "v@:@");
    method(w, "selectKeyViewPrecedingView:", [select_key_view](CpuState& s, GuestMemory&) { select_key_view(s.x[0], s.x[2], false); }, "v@:@");
    method(w, "autorecalculatesKeyViewLoop", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    method(w, "setAutorecalculatesKeyViewLoop:", [](CpuState& s, GuestMemory&) {
        if (bool_arg(s, 2)) note_trace("-[NSWindow setAutorecalculatesKeyViewLoop:YES]: the loop stays as the application links it");
    }, "v@:B");
    method(w, "recalculateKeyViewLoop", [](CpuState&, GuestMemory&) {}, "v@:");
    method(w, "initialFirstResponder", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->initial_first_responder; });
    method(w, "setInitialFirstResponder:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->initial_first_responder = s.x[2]; }, "v@:@");
    method(w, "sendEvent:", [this](CpuState& s, GuestMemory&) { if (s.x[2]) window_send_event(s.x[0], s.x[2]); }, "v@:@");
    method(w, "acceptsMouseMovedEvents", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->accepts_mouse_moved); }, "B@:");
    method(w, "setAcceptsMouseMovedEvents:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        state->accepts_mouse_moved = bool_arg(s, 2);
        if (state->id) require_window_server("-[NSWindow setAcceptsMouseMovedEvents:]").set_accepts_mouse_moved(state->id, state->accepts_mouse_moved);
    }, "v@:B");
    method(w, "inLiveResize", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->live_resize); }, "B@:");
    method(w, "registerForDraggedTypes:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        for (const auto value : items(s.x[2])) state->dragged_types.push_back(text(value));
        if (state->id) require_window_server("-[NSWindow registerForDraggedTypes:]").set_accepts_file_drops(state->id, true);
    }, "v@:@");
    method(w, "unregisterDraggedTypes", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        state->dragged_types.clear();
        bool views_accept{};
        for (const auto value : all_views(state->content_view)) views_accept = views_accept || !view_state(value)->dragged_types.empty();
        if (state->id && !views_accept) require_window_server("-[NSWindow unregisterDraggedTypes]").set_accepts_file_drops(state->id, false);
    }, "v@:");
    // Cursor rectangles (view.cpp): invalidation discards a view's rectangles
    // and asks it to add them again (-resetCursorRects), at once.
    method(w, "invalidateCursorRectsForView:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        discard_cursor_rects(s.x[2]);
        static_cast<void>(send(s.x[2], "resetCursorRects"));
    }, "v@:@");
    method(w, "discardCursorRects", [this](CpuState& s, GuestMemory&) {
        for (const auto value : all_views(window_state(s.x[0])->content_view)) discard_cursor_rects(value);
    }, "v@:");
    method(w, "resetCursorRects", [this](CpuState& s, GuestMemory&) {
        for (const auto value : all_views(window_state(s.x[0])->content_view)) {
            discard_cursor_rects(value);
            static_cast<void>(send(value, "resetCursorRects"));
        }
    }, "v@:");
    method(w, "areCursorRectsEnabled", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->cursor_rects_enabled); }, "B@:");
    method(w, "enableCursorRects", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->cursor_rects_enabled = true; }, "v@:");
    method(w, "disableCursorRects", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->cursor_rects_enabled = false; }, "v@:");

    // ---- sizes and appearance ----
    const auto limits = [this](Address self) {
        const auto state = window_state(self);
        if (!state->id) return;
        const auto maximum = state->max_content.width > 0 ? state->max_content : Size{unlimited_size, unlimited_size};
        const auto strip = state->strip;
        require_window_server("NSWindow size limits").set_content_limits(state->id, {state->min_content.width, state->min_content.height + strip},
                                                                         {maximum.width, maximum.height + strip});
    };
    const auto frame_to_content = [this](Address self, Size size) {
        return from_server_content(self, require_window_server("NSWindow size limits").content_for_frame({{0, 0}, size}, window_state(self)->style)).size;
    };
    const auto content_to_frame = [this](Address self, Size size) {
        return require_window_server("NSWindow size limits").frame_for_content(to_server_content(self, {{0, 0}, size}), window_state(self)->style).size;
    };
    method(w, "setContentMinSize:", [this, limits](CpuState& s, GuestMemory&) { window_state(s.x[0])->min_content = size_arg(s); limits(s.x[0]); }, "v@:{CGSize=dd}");
    method(w, "setContentMaxSize:", [this, limits](CpuState& s, GuestMemory&) { window_state(s.x[0])->max_content = size_arg(s); limits(s.x[0]); }, "v@:{CGSize=dd}");
    method(w, "setMinSize:", [this, limits, frame_to_content](CpuState& s, GuestMemory&) {
        window_state(s.x[0])->min_content = frame_to_content(s.x[0], size_arg(s)); limits(s.x[0]);
    }, "v@:{CGSize=dd}");
    method(w, "setMaxSize:", [this, limits, frame_to_content](CpuState& s, GuestMemory&) {
        window_state(s.x[0])->max_content = frame_to_content(s.x[0], size_arg(s)); limits(s.x[0]);
    }, "v@:{CGSize=dd}");
    method(w, "contentMinSize", [this](CpuState& s, GuestMemory&) { return_size(s, window_state(s.x[0])->min_content); }, "{CGSize=dd}@:");
    method(w, "contentMaxSize", [this](CpuState& s, GuestMemory&) {
        const auto value = window_state(s.x[0])->max_content;
        return_size(s, value.width > 0 ? value : Size{unlimited_size, unlimited_size});
    }, "{CGSize=dd}@:");
    method(w, "minSize", [this, content_to_frame](CpuState& s, GuestMemory&) {
        return_size(s, content_to_frame(s.x[0], window_state(s.x[0])->min_content));
    }, "{CGSize=dd}@:");
    method(w, "maxSize", [this, content_to_frame](CpuState& s, GuestMemory&) {
        const auto value = window_state(s.x[0])->max_content;
        return_size(s, value.width > 0 ? content_to_frame(s.x[0], value) : Size{unlimited_size, unlimited_size});
    }, "{CGSize=dd}@:");
    method(w, "isReleasedWhenClosed", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->released_when_closed); }, "B@:");
    method(w, "setReleasedWhenClosed:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->released_when_closed = bool_arg(s, 2); }, "v@:B");
    // Title-bar, tabbing and Spaces settings shape macOS chrome that Win32
    // draws itself; they are recorded for the getters.
    method(w, "titlebarAppearsTransparent", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->titlebar_transparent); }, "B@:");
    method(w, "setTitlebarAppearsTransparent:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->titlebar_transparent = bool_arg(s, 2); }, "v@:B");
    method(w, "titleVisibility", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->title_visibility; }, "q@:");
    method(w, "setTitleVisibility:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->title_visibility = s.x[2]; }, "v@:q");
    method(w, "tabbingMode", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->tabbing_mode; }, "q@:");
    method(w, "setTabbingMode:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->tabbing_mode = s.x[2]; }, "v@:q");
    method(w, "collectionBehavior", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->collection_behavior; }, "Q@:");
    method(w, "setCollectionBehavior:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->collection_behavior = s.x[2]; }, "v@:Q");
    class_method(w, "allowsAutomaticWindowTabbing", [this](CpuState& s, GuestMemory&) { return_bool(s, allows_tabbing); }, "B@:");
    class_method(w, "setAllowsAutomaticWindowTabbing:", [this](CpuState& s, GuestMemory&) { allows_tabbing = bool_arg(s, 2); }, "v@:B");
    method(w, "isOpaque", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->opaque); }, "B@:");
    method(w, "setOpaque:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->opaque = bool_arg(s, 2); }, "v@:B");
    method(w, "hasShadow", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->has_shadow); }, "B@:");
    method(w, "setHasShadow:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->has_shadow = bool_arg(s, 2); }, "v@:B");
    method(w, "backgroundColor", [this](CpuState& s, GuestMemory&) {
        const auto value = window_state(s.x[0])->background_color;
        s.x[0] = value ? value : color_object(0.93, 0.93, 0.93, 1);   // windowBackgroundColor
    });
    method(w, "setBackgroundColor:", [this](CpuState& s, GuestMemory&) { assign(window_state(s.x[0])->background_color, s.x[2]); }, "v@:@");
    method(w, "appearance", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->appearance; });
    method(w, "setAppearance:", [this](CpuState& s, GuestMemory&) { assign(window_state(s.x[0])->appearance, s.x[2]); }, "v@:@");
    method(w, "effectiveAppearance", [this](CpuState& s, GuestMemory&) {
        const auto own = window_state(s.x[0])->appearance;
        s.x[0] = own ? own : send(app ? app : shared_application(application), "effectiveAppearance").x0;
    });
    method(w, "windowController", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->window_controller; });
    method(w, "setWindowController:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->window_controller = s.x[2]; }, "v@:@");

    // ---- display (-display, -displayIfNeeded, -flushWindow: graphics.cpp) ----
    method(w, "viewsNeedDisplay", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->needs_display); }, "B@:");
    method(w, "setViewsNeedDisplay:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->needs_display = bool_arg(s, 2); }, "v@:B");
    method(w, "update", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (state->toolbar_view && !view_state(state->toolbar_view)->hidden) {
            static_cast<void>(send(state->toolbar, "validateVisibleItems"));
            invalidate_view(state->toolbar_view);
        }
        post(note::window_did_update, s.x[0]);
    }, "v@:");

    // ---- child windows ----
    // A child window stays above its parent and moves with it: the window
    // server makes the parent its owner (NSWindowAbove). An NSWindowBelow
    // child is ordered below the parent and kept in the lists, but Win32
    // keeps owned windows above their owner, so it is not owned and does not
    // follow the parent's moves.
    method(w, "addChildWindow:ordered:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], child = s.x[2];
        const auto place = static_cast<std::int64_t>(s.x[3]);
        if (!child || child == self) return;
        const auto state = window_state(self), info = window_state(child);
        if (info->parent && info->parent != self) send_id(info->parent, "removeChildWindow:", {child});
        if (std::find(state->children.begin(), state->children.end(), child) == state->children.end()) state->children.push_back(child);
        info->parent = self;
        if (!state->id || !info->id) return;
        auto& server = require_window_server("-[NSWindow addChildWindow:ordered:]");
        server.set_parent(info->id, place == order_below ? 0 : state->id);
        if (info->visible && state->visible) order_window(child, place == order_below ? order_below : order_above, state->id);
    }, "v@:@q");
    method(w, "removeChildWindow:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], child = s.x[2];
        if (!child) return;
        auto& children = window_state(self)->children;
        const auto found = std::find(children.begin(), children.end(), child);
        if (found == children.end()) return;
        children.erase(found);
        const auto info = window_state(child);
        info->parent = 0;
        if (info->id && !info->closed) require_window_server("-[NSWindow removeChildWindow:]").set_parent(info->id, 0);
    }, "v@:@");
    method(w, "childWindows", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array(window_state(s.x[0])->children); });
    method(w, "parentWindow", [this](CpuState& s, GuestMemory&) { s.x[0] = window_state(s.x[0])->parent; });

    // ---- window list ----
    class_method(w, "windowNumbersWithOptions:", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> numbers;
        {
            std::lock_guard lock(mutex);
            for (const auto value : window_order) {
                const auto state = window_state(value);
                if (state->visible && state->id) numbers.push_back(cf.integer(static_cast<std::int64_t>(state->id)));
            }
        }
        const auto array = cf.array(numbers);
        for (const auto value : numbers) process.objects().release(value);
        s.x[0] = autorelease(array);
    }, "@@:Q");
    class_method(w, "windowNumberAtPoint:belowWindowWithWindowNumber:", [this](CpuState& s, GuestMemory&) {
        // Front to back by the last known ordering; frames from the server.
        const auto point = point_arg(s);
        const auto below = static_cast<WindowId>(s.x[2]);
        const auto server = window_server_or_null();
        s.x[0] = 0;
        if (!server) return;
        std::lock_guard lock(mutex);
        bool passed = below == 0;
        for (const auto value : window_order) {
            const auto state = window_state(value);
            if (!state->id || !state->visible) continue;
            if (!passed) { passed = state->id == below; continue; }
            if (rect_contains(server->frame_rect(state->id), point)) { s.x[0] = state->id; return; }
        }
    }, "q@:{CGPoint=dd}q");
}


// ---- frame autosave --------------------------------------------------------------------------
// Frames are saved in the user defaults under "NSWindow Frame <name>" as
// AppKit's frame strings: the frame then the screen's visible frame,
// "x y w h sx sy sw sh " (integers). A window with an autosave name saves
// its frame whenever it moves or resizes. Names are not checked for use by
// another window (AppKit's -setFrameAutosaveName: returns NO then).
namespace {
std::string frame_defaults_key(std::string_view name) { return "NSWindow Frame " + std::string(name); }

Address user_defaults(AppKit& kit) {
    const auto defaults_class = kit.objc.find_class("NSUserDefaults");
    if (!defaults_class) unsupported("NSWindow frame autosave without NSUserDefaults");
    return kit.send(defaults_class, "standardUserDefaults").x0;
}
} // namespace

std::string AppKit::saved_frame_string(Address target) {
    const auto frame = send(target, "frame");
    Rect visible{};
    if (const auto main_screen = send(screen, "mainScreen").x0) {
        const auto screen_frame = send(main_screen, "visibleFrame");
        visible = {{screen_frame.d(0), screen_frame.d(1)}, {screen_frame.d(2), screen_frame.d(3)}};
    }
    std::string text;
    for (const double value : {frame.d(0), frame.d(1), frame.d(2), frame.d(3), visible.origin.x, visible.origin.y, visible.size.width,
             visible.size.height})
        text += std::to_string(static_cast<long long>(std::lround(value))) + " ";
    return text;
}

bool AppKit::set_frame_from_string(Address target, std::string_view text) {
    std::vector<double> numbers;
    std::string token;
    for (const char c : std::string(text) + " ") {
        if (c == ' ' || c == '\t') {
            if (!token.empty()) {
                try { numbers.push_back(std::stod(token)); } catch (const std::exception&) { return false; }
                token.clear();
            }
        } else token.push_back(c);
    }
    if (numbers.size() < 4 || numbers[2] <= 0 || numbers[3] <= 0) return false;
    CallRegs registers;
    put_rect(registers, 0, {{numbers[0], numbers[1]}, {numbers[2], numbers[3]}});
    registers.x[2] = 1;   // display
    static_cast<void>(send(target, "setFrame:display:", registers));
    return true;
}

namespace {
// The screen a window is on (its server window's), else the main screen; nullopt without screens.
std::optional<ScreenInfo> window_screen_info(AppKit& kit, Address target) {
    auto& server = require_window_server("NSWindow screen geometry");
    const auto list = server.screens();
    if (list.empty()) return std::nullopt;
    const auto state = kit.window_state(target);
    const auto id = state->id ? server.window_screen(state->id) : 0;
    for (const auto& info : list) if (info.id == id) return info;
    return list.front();
}
} // namespace

void AppKit::install_window_geometry() {
    const auto w = window;
    // Frame constraints: the top edge stays on the screen's visible frame and
    // a frame taller than it is shortened to fit.
    method(w, "constrainFrameRect:toScreen:", [this](CpuState& s, GuestMemory&) {
        auto frame = rect_arg(s);
        Rect visible;
        if (const auto screen_object = s.x[2]) {
            visible = result_rect(send(screen_object, "visibleFrame"));
        } else if (const auto info = window_screen_info(*this, s.x[0])) {
            visible = info->visible_frame;
        } else {
            return_rect(s, frame);
            return;
        }
        const double top = visible.origin.y + visible.size.height;
        if (frame.size.height > visible.size.height) {
            frame.origin.y += frame.size.height - visible.size.height;
            frame.size.height = visible.size.height;
        }
        if (frame.origin.y + frame.size.height > top) frame.origin.y = top - frame.size.height;
        return_rect(s, frame);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    // Cascading: the window's top-left goes to the point ({0, 0}: where it
    // is), kept on the visible frame, and the next window's point is one
    // titlebar height to the right and down.
    method(w, "cascadeTopLeftFromPoint:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto frame = result_rect(send(self, "frame"));
        const auto content = content_rect(self);
        const double step = std::max(frame.size.height - content.size.height, 16.0);
        auto point = point_arg(s);
        if (point.x == 0 && point.y == 0) point = {frame.origin.x, frame.origin.y + frame.size.height};
        if (const auto info = window_screen_info(*this, self)) {
            const auto& visible = info->visible_frame;
            const double top = visible.origin.y + visible.size.height, right = visible.origin.x + visible.size.width;
            if (point.y > top) point.y = top;
            if (point.x < visible.origin.x) point.x = visible.origin.x;
            // A window that would leave the visible frame starts again at its top-left corner.
            if (point.y - frame.size.height < visible.origin.y || point.x + frame.size.width > right)
                point = {visible.origin.x, top};
        }
        CallRegs registers;
        put_point(registers, 0, point);
        static_cast<void>(send(self, "setFrameTopLeftPoint:", registers));
        return_point(s, {point.x + step, point.y - step});
    }, "{CGPoint=dd}@:{CGPoint=dd}");
    // Pixel alignment in screen points at the window's backing scale.
    method(w, "backingAlignedRect:options:", [this](CpuState& s, GuestMemory&) {
        return_rect(s, backing_aligned(rect_arg(s), backing_scale(s.x[0]), s.x[2]));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}Q");
    // The time a -setFrame:display:animate: to `frame` takes: the
    // NSWindowResizeTime default (0.2 s) per 150 points of height change.
    method(w, "animationResizeTime:", [this](CpuState& s, GuestMemory&) {
        const auto current = result_rect(send(s.x[0], "frame"));
        const auto wanted = rect_arg(s);
        double per_step = 0.2;
        if (const auto defaults_class = objc.find_class("NSUserDefaults")) {
            const auto defaults = send(defaults_class, "standardUserDefaults").x0;
            const auto key = auto_string("NSWindowResizeTime");
            if (defaults && send_id(defaults, "objectForKey:", {key})) {
                CallRegs registers;
                registers.x[2] = key;
                per_step = send(defaults, "doubleForKey:", registers).d(0);
            }
        }
        set_d(s, 0, per_step * std::abs(wanted.size.height - current.size.height) / 150.0);
    }, "d@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // Aspect ratios (enforced by will_resize while the user resizes).
    method(w, "aspectRatio", [this](CpuState& s, GuestMemory&) { return_size(s, window_state(s.x[0])->aspect_ratio); }, "{CGSize=dd}@:");
    method(w, "setAspectRatio:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        state->aspect_ratio = size_arg(s);
        state->content_aspect_ratio = {};
    }, "v@:{CGSize=dd}");
    method(w, "contentAspectRatio", [this](CpuState& s, GuestMemory&) { return_size(s, window_state(s.x[0])->content_aspect_ratio); }, "{CGSize=dd}@:");
    method(w, "setContentAspectRatio:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        state->content_aspect_ratio = size_arg(s);
        state->aspect_ratio = {};
    }, "v@:{CGSize=dd}");
    method(w, "canHide", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->can_hide); }, "B@:");
    method(w, "setCanHide:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->can_hide = bool_arg(s, 2); }, "v@:B");
    method(w, "toolbarStyle", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(window_state(s.x[0])->toolbar_style); }, "q@:");
    method(w, "setToolbarStyle:", [this](CpuState& s, GuestMemory&) { window_state(s.x[0])->toolbar_style = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // The window's place among the application's visible windows (0 front).
    method(w, "orderedIndex", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> order;
        { std::lock_guard lock(mutex); order = window_order; }
        std::int64_t index = 0;
        for (const auto target : order) {
            if (target == s.x[0]) { s.x[0] = static_cast<Address>(index); return; }
            if (window_state(target)->visible) ++index;
        }
        s.x[0] = static_cast<Address>(-1);
    }, "q@:");
    method(w, "setOrderedIndex:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto wanted = static_cast<std::int64_t>(s.x[2]);
        std::vector<Address> visible;
        {
            std::lock_guard lock(mutex);
            for (const auto target : window_order)
                if (target != self && window_state(target)->visible) visible.push_back(target);
        }
        if (wanted <= 0 || visible.empty()) { order_window(self, order_above, 0); return; }
        const auto below = visible[std::min<std::size_t>(static_cast<std::size_t>(wanted), visible.size()) - 1];
        order_window(self, order_below, window_state(below)->id);
    }, "v@:q");
    // The event queue is the application's.
    method(w, "currentEvent", [this](CpuState& s, GuestMemory&) { s.x[0] = app ? send(app, "currentEvent").x0 : 0; });
    method(w, "postEvent:atStart:", [this](CpuState& s, GuestMemory&) {
        if (!app) fail("-[NSWindow postEvent:atStart:] before NSApplication exists");
        static_cast<void>(send_id(app, "postEvent:atStart:", {s.x[2], s.x[3] & 0xff}));
    }, "v@:@B");
    method(w, "discardEventsMatchingMask:beforeEvent:", [this](CpuState& s, GuestMemory&) {
        if (app) static_cast<void>(send_id(app, "discardEventsMatchingMask:beforeEvent:", {s.x[2], s.x[3]}));
    }, "v@:Q@");
}

void AppKit::autosave_frame(Address target) {
    const auto state = window_state(target);
    if (!state->autosave_name) return;
    const auto name = text(state->autosave_name);
    if (name.empty()) return;
    send_id(user_defaults(*this), "setObject:forKey:", {auto_string(saved_frame_string(target)), auto_string(frame_defaults_key(name))});
}

void AppKit::install_window_autosave() {
    const auto w = window;
    method(w, "stringWithSavedFrame", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_string(saved_frame_string(s.x[0])); });
    method(w, "setFrameFromString:", [this](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(set_frame_from_string(s.x[0], text(s.x[2])));
    }, "v@:@");
    const auto restore = [this](Address target, Address name_object) {
        const auto name = text(name_object);
        if (name.empty()) return false;
        const auto saved = send_id(user_defaults(*this), "stringForKey:", {auto_string(frame_defaults_key(name))});
        return saved && set_frame_from_string(target, text(saved));
    };
    method(w, "setFrameUsingName:", [restore](CpuState& s, GuestMemory&) { return_bool(s, s.x[2] && restore(s.x[0], s.x[2])); }, "B@:@");
    method(w, "setFrameUsingName:force:", [restore](CpuState& s, GuestMemory&) { return_bool(s, s.x[2] && restore(s.x[0], s.x[2])); }, "B@:@B");
    method(w, "saveFrameUsingName:", [this](CpuState& s, GuestMemory&) {
        const auto name = text(s.x[2]);
        if (name.empty()) return;
        send_id(user_defaults(*this), "setObject:forKey:", {auto_string(saved_frame_string(s.x[0])), auto_string(frame_defaults_key(name))});
    }, "v@:@");
    class_method(w, "removeFrameUsingName:", [this](CpuState& s, GuestMemory&) {
        const auto name = text(s.x[2]);
        if (!name.empty()) send_id(user_defaults(*this), "removeObjectForKey:", {auto_string(frame_defaults_key(name))});
    }, "v@:@");
    method(w, "frameAutosaveName", [this](CpuState& s, GuestMemory&) {
        const auto name = window_state(s.x[0])->autosave_name;
        s.x[0] = name ? name : auto_string("");
    });
    // A saved frame is applied; otherwise the current frame is saved.
    method(w, "setFrameAutosaveName:", [this, restore](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        release(std::exchange(state->autosave_name, s.x[2] ? cf.copy(s.x[2]) : 0));
        if (state->autosave_name && !restore(s.x[0], state->autosave_name)) autosave_frame(s.x[0]);
        return_bool(s, true);
    }, "B@:@");
}

} // namespace alchemy::gui::appkit
