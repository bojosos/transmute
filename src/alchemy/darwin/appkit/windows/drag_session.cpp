#include "alchemy/darwin/appkit/windows/windows_internal.hpp"

#include <algorithm>
#include <set>

// Dragging sources inside the application (QLab gap group G4):
// -[NSView beginDraggingSessionWithItems:event:source:] writes the items
// (NSPasteboardWriting) to a pasteboard of its own and tracks the mouse in the
// event-tracking mode until the button goes up or Escape is pressed. Over
// the application's windows the deepest view registered for one of the
// pasteboard's types (else the window) is the destination: it gets
// -draggingEntered: / -draggingUpdated: / -draggingExited: and, on release,
// -prepareForDragOperation: / -performDragOperation: /
// -concludeDragOperation: with an NSDraggingInfo whose -draggingSource is the
// source. The source hears -draggingSession:willBeginAtPoint:,
// ...movedToPoint:, ...endedAtPoint:operation: and is asked for its operation
// mask within the application. A drop outside the application's windows
// ends with NSDragOperationNone: dragging to other applications (Explorer)
// is not provided.
namespace alchemy::gui::completion_kit::windows {
namespace {
using appkit::put_point;

constexpr std::uint64_t mask_left_up = 1ULL << 2, mask_left_dragged = 1ULL << 6, mask_key_down = 1ULL << 10,
    mask_mouse_moved = 1ULL << 5;
constexpr std::uint16_t escape_key = 0x35;

struct SessionState final : ObjectPayload {
    Address pasteboard{};                     // +1
    Address source{};                         // weak
    std::int64_t formation{}, leader{}, sequence{};
    bool animates_back{true};
    Point location;                           // screen
};
std::int64_t sequence_counter = 0;

std::shared_ptr<SessionState> session_state(Address session) { return kit().objc.host_state<SessionState>(session); }

// Writes the items' pasteboard writers to `pasteboard`.
void write_items(Address pasteboard, const std::vector<Address>& items) {
    auto& k = kit();
    std::vector<Address> types;
    std::vector<std::pair<Address, Address>> values;   // type, value
    for (const auto item : items) {
        const auto writer = k.send(item, "item").x0;
        if (!writer) continue;
        for (const auto type : k.items(k.send_id(writer, "writableTypesForPasteboard:", {pasteboard}))) {
            const auto value = k.send_id(writer, "pasteboardPropertyListForType:", {type});
            if (!value) continue;
            types.push_back(type);
            values.emplace_back(type, value);
        }
    }
    static_cast<void>(k.send_id(pasteboard, "declareTypes:owner:", {k.auto_array(types), 0}));
    for (const auto& [type, value] : values) {
        const auto kind = k.cf.kind(value);
        if (kind == ObjectKind::string) static_cast<void>(k.send_id(pasteboard, "setString:forType:", {value, type}));
        else if (kind == ObjectKind::url) static_cast<void>(k.send_id(pasteboard, "setString:forType:", {k.send(value, "absoluteString").x0, type}));
        else if (k.responds(value, "bytes") && k.responds(value, "length")) static_cast<void>(k.send_id(pasteboard, "setData:forType:", {value, type}));
        else static_cast<void>(k.send_id(pasteboard, "setPropertyList:forType:", {value, type}));
    }
}

// The application window under a screen point, front to back.
Address window_at(Point screen) {
    auto& k = kit();
    std::vector<Address> order;
    {
        std::lock_guard lock(k.mutex);
        order = k.window_order;
    }
    for (const auto window : order) {
        const auto state = k.window_state(window);
        if (!state->visible || state->closed || !state->id) continue;
        const auto content = k.content_rect(window);
        if (appkit::rect_contains(content, screen)) return window;
    }
    return 0;
}

bool registered_for(const std::vector<std::string>& registered, const std::set<std::string>& types) {
    for (const auto& type : registered) if (types.contains(type)) return true;
    return false;
}

Address destination_at(Address window, Point location, const std::set<std::string>& types) {
    auto& k = kit();
    for (auto current = k.hit_test(window, location); current; current = k.view_state(current)->superview)
        if (registered_for(k.view_state(current)->dragged_types, types)) return current;
    return registered_for(k.window_state(window)->dragged_types, types) ? window : 0;
}

std::optional<CallResult> message(Address receiver, std::string_view selector, Address info, Address window) {
    auto& k = kit();
    CallRegs registers;
    registers.x[2] = info;
    auto answer = k.send_optional(receiver, selector, registers);
    if (!answer && receiver == window) {
        const auto delegate = k.window_state(window)->delegate;
        if (delegate) answer = k.send_optional(delegate, selector, registers);
    }
    return answer;
}

Address run_session(Address view, Address items, Address event, Address source) {
    auto& k = kit();
    const auto window = k.view_state(view)->window;
    if (!window) completion_kit::fail("-[NSView beginDraggingSessionWithItems:event:source:]: the view is not in a window");
    if (!source) completion_kit::fail("-[NSView beginDraggingSessionWithItems:event:source:] without a source");
    const auto list = k.items(items);
    if (list.empty()) completion_kit::fail("-[NSView beginDraggingSessionWithItems:event:source:] without items");
    const auto session = k.alloc_init(k.objc.find_class("NSDraggingSession"));
    const auto state = session_state(session);
    state->source = source;
    state->sequence = ++sequence_counter;
    state->pasteboard = k.retain(k.send(k.pasteboard, "pasteboardWithUniqueName").x0);
    write_items(state->pasteboard, list);
    std::set<std::string> types;
    for (const auto type : k.items(k.send(state->pasteboard, "types").x0)) types.insert(k.text(type));
    const auto screen_of = [&](Address event_object) {
        const auto info = k.event_state(event_object);
        const auto origin = info->window ? k.content_rect(info->window).origin : Point{};
        return Point{origin.x + info->event.location.x, origin.y + info->event.location.y};
    };
    state->location = screen_of(event);
    // Within the application (NSDraggingContextWithinApplication).
    CallRegs mask_request;
    mask_request.x[2] = session;
    mask_request.x[3] = 1;
    const auto mask = k.send_optional(source, "draggingSession:sourceOperationMaskForDraggingContext:", mask_request);
    const std::uint64_t source_mask = mask ? mask->x0 : 0;
    CallRegs begin;
    begin.x[2] = session;
    put_point(begin, 0, state->location);
    static_cast<void>(k.send_optional(source, "draggingSession:willBeginAtPoint:", begin));

    const auto info = k.objc.allocate(k.dragging_info);
    const auto drag = k.objc.host_state<appkit::DraggingState>(info);
    drag->pasteboard = k.retain(state->pasteboard);
    drag->source_mask = source_mask;
    drag->sequence = state->sequence;
    drag->source = source;
    Address destination{}, destination_window{};
    std::uint64_t answer{};
    std::uint64_t operation{};
    bool dropped = false, cancelled = false;
    const auto future = k.send(k.objc.find_class("NSDate"), "distantFuture").x0;
    const auto app = k.send(k.application, "sharedApplication").x0;
    const auto leave = [&] {
        if (destination) static_cast<void>(message(destination, "draggingExited:", info, destination_window));
        destination = 0;
        answer = 0;
    };
    try {
        for (;;) {
            const auto next = k.send_id(app, "nextEventMatchingMask:untilDate:inMode:dequeue:",
                                        {mask_left_up | mask_left_dragged | mask_key_down | mask_mouse_moved, future,
                                         k.auto_string(event_tracking_mode), 1});
            if (!next) continue;
            const auto next_state = k.event_state(next);
            if (next_state->event.type == event_type::key_down) {
                if (next_state->event.key_code == escape_key) { cancelled = true; break; }
                continue;
            }
            state->location = screen_of(next);
            const auto target = window_at(state->location);
            Point local{};
            if (target) {
                const auto origin = k.content_rect(target).origin;
                local = {state->location.x - origin.x, state->location.y - origin.y};
            }
            const auto over = target && k.input_allowed_for(target) ? destination_at(target, local, types) : 0;
            drag->window = target;
            drag->location = local;
            if (over != destination) {
                leave();
                if (over) {
                    destination = over;
                    destination_window = target;
                    const auto entered = message(destination, "draggingEntered:", info, target);
                    answer = entered ? entered->x0 : 0;
                }
            } else if (destination) {
                if (const auto updated = message(destination, "draggingUpdated:", info, target)) answer = updated->x0;
            }
            CallRegs moved;
            moved.x[2] = session;
            put_point(moved, 0, state->location);
            static_cast<void>(k.send_optional(source, "draggingSession:movedToPoint:", moved));
            if (next_state->event.type == event_type::left_mouse_up) { dropped = true; break; }
        }
        if (cancelled) leave();
        if (dropped && destination) {
            const auto allowed = answer & source_mask;
            if (allowed) {
                const auto prepared = message(destination, "prepareForDragOperation:", info, destination_window);
                if (!prepared || (prepared->x0 & 0xff)) {
                    const auto performed = message(destination, "performDragOperation:", info, destination_window);
                    if (performed && (performed->x0 & 0xff)) {
                        static_cast<void>(message(destination, "concludeDragOperation:", info, destination_window));
                        // The first operation of the mask both allow (copy, link, generic, private, move, delete).
                        for (const std::uint64_t bit : {1ULL, 2ULL, 4ULL, 8ULL, 16ULL, 32ULL})
                            if (allowed & bit) { operation = bit; break; }
                    }
                }
            } else {
                leave();
            }
            if (destination) static_cast<void>(message(destination, "draggingEnded:", info, destination_window));
        }
    } catch (...) {
        k.release(info);
        k.release(session);
        throw;
    }
    CallRegs ended;
    ended.x[2] = session;
    put_point(ended, 0, state->location);
    ended.x[3] = operation;
    static_cast<void>(k.send_optional(source, "draggingSession:endedAtPoint:operation:", ended));
    k.release(info);
    return k.autorelease(session);
}
} // namespace

void install_drag_session(Support& support) {
    static_cast<void>(support);
    auto& k = kit();
    const auto cls = k.declare_class("NSDraggingSession", "NSObject");
    k.method(cls, "draggingPasteboard", [](CpuState& s, GuestMemory&) { s.x[0] = session_state(s.x[0])->pasteboard; });
    k.method(cls, "draggingSequenceNumber", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(session_state(s.x[0])->sequence); }, "q@:");
    k.method(cls, "draggingLocation", [](CpuState& s, GuestMemory&) { appkit::return_point(s, session_state(s.x[0])->location); }, "{CGPoint=dd}@:");
    k.method(cls, "draggingFormation", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(session_state(s.x[0])->formation); }, "q@:");
    // The formation and the slide back shape the drag images, which this
    // session does not draw.
    k.method(cls, "setDraggingFormation:", [](CpuState& s, GuestMemory&) { session_state(s.x[0])->formation = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    k.method(cls, "animatesToStartingPositionsOnCancelOrFail", [](CpuState& s, GuestMemory&) { appkit::return_bool(s, session_state(s.x[0])->animates_back); }, "B@:");
    k.method(cls, "setAnimatesToStartingPositionsOnCancelOrFail:", [](CpuState& s, GuestMemory&) {
        session_state(s.x[0])->animates_back = appkit::bool_arg(s, 2);
    }, "v@:B");
    k.method(cls, "draggingLeaderIndex", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(session_state(s.x[0])->leader); }, "q@:");
    k.method(cls, "setDraggingLeaderIndex:", [](CpuState& s, GuestMemory&) { session_state(s.x[0])->leader = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    k.method(cls, "dealloc", [cls](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = kk.objc.find_host_state<SessionState>(s.x[0])) kk.release(std::exchange(state->pasteboard, 0));
        static_cast<void>(kk.objc.send_super(s.x[0], cls, kk.sel("dealloc")));
    }, "v@:");
    k.method(k.view, "beginDraggingSessionWithItems:event:source:", [](CpuState& s, GuestMemory&) {
        s.x[0] = run_session(s.x[0], s.x[2], s.x[3], s.x[4]);
    });
}

} // namespace alchemy::gui::completion_kit::windows
