#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/gui/cg_event_record.hpp"

#include <algorithm>
#include <cmath>

// NSEvent: window-server events as Objective-C objects, the class-level input
// state, programmatic events, local event monitors and the CGEvent bridge
// (-CGEvent, +eventWithCGEvent:) through CoreGraphics' CGEventCreateData /
// CGEventCreateFromData records (alchemy/gui/cg_event_record.hpp).
namespace alchemy::gui::appkit {
namespace {
constexpr std::uint8_t ansi_keyboard_type = 40;   // gestaltUSBAndyANSIKbd, as LMGetKbdType answers

bool key_type(std::uint32_t type) { return type == event_type::key_down || type == event_type::key_up; }
bool enter_exit_type(std::uint32_t type) {
    return type == event_type::mouse_entered || type == event_type::mouse_exited || type == event_type::cursor_update;
}
bool pointer_type(std::uint32_t type) {
    switch (type) {
    case event_type::left_mouse_down: case event_type::left_mouse_up: case event_type::right_mouse_down:
    case event_type::right_mouse_up: case event_type::mouse_moved: case event_type::left_mouse_dragged:
    case event_type::right_mouse_dragged: case event_type::other_mouse_down: case event_type::other_mouse_up:
    case event_type::other_mouse_dragged: case event_type::tablet_point:
        return true;
    default:
        return false;
    }
}

// NSEventPhase <-> CGScrollPhase (kCGScrollPhaseBegan 1, Changed 2, Ended 4,
// Cancelled 8, MayBegin 128; CGEventTypes.h) and CGMomentumScrollPhase
// (None 0, Begin 1, Continue 2, End 3). NSEventPhaseStationary has no
// CGScrollPhase.
std::int64_t scroll_phase_to_cg(std::uint64_t value) {
    switch (value) {
    case phase::began: return 1;
    case phase::changed: return 2;
    case phase::ended: return 4;
    case phase::cancelled: return 8;
    case phase::may_begin: return 128;
    default: return 0;
    }
}
std::uint64_t scroll_phase_from_cg(std::int64_t value) {
    switch (value) {
    case 1: return phase::began;
    case 2: return phase::changed;
    case 4: return phase::ended;
    case 8: return phase::cancelled;
    case 128: return phase::may_begin;
    default: return phase::none;
    }
}
std::int64_t momentum_phase_to_cg(std::uint64_t value) {
    switch (value) {
    case phase::began: return 1;
    case phase::changed: return 2;
    case phase::ended: case phase::cancelled: return 3;
    default: return 0;
    }
}
std::uint64_t momentum_phase_from_cg(std::int64_t value) {
    switch (value) {
    case 1: return phase::began;
    case 2: return phase::changed;
    case 3: return phase::ended;
    default: return phase::none;
    }
}

// The primary screen's height: global display coordinates (CGEvent, origin
// top-left of the primary display) <-> screen points (origin bottom-left).
double primary_screen_height(std::string_view operation) {
    const auto screens = require_window_server(operation).screens();
    if (screens.empty()) fail(std::string(operation) + ": the window server reports no screen");
    return screens.front().frame.size.height;
}
} // namespace

// The CGEventRef of -[NSEvent CGEvent] (+1).
Address AppKit::make_cg_event(Address value) {
    const auto info = event_state(value);
    const auto& source = info->event;
    // Screen points of the event's location: window events convert through
    // their window, the others already carry screen points.
    auto screen = source.location;
    if (info->window) {
        CallRegs registers;
        registers.set_d(0, screen.x);
        registers.set_d(1, screen.y);
        const auto converted = send(info->window, "convertPointToScreen:", registers);
        screen = {converted.d(0), converted.d(1)};
    }
    cg_event::Record record;
    record.type = source.type;
    record.x = screen.x;
    record.y = primary_screen_height("-[NSEvent CGEvent]") - screen.y;
    record.flags = source.modifier_flags;
    record.timestamp = static_cast<std::uint64_t>(std::llround(std::max(0.0, source.timestamp) * 1e9));   // mach time is ns
    auto& fields = record.fields;
    namespace f = cg_event::field;
    if (pointer_type(source.type) || enter_exit_type(source.type)) {
        fields[f::mouse_click_state] = source.click_count;
        fields[f::mouse_button_number] = source.button_number;
        fields[f::mouse_delta_x] = std::llround(source.delta_x);
        fields[f::mouse_delta_y] = std::llround(source.delta_y);
        fields[f::mouse_subtype] = info->subtype;   // CGEventMouseSubtype = NSEventSubtype for mouse events
    } else if (key_type(source.type) || source.type == event_type::flags_changed) {
        fields[f::keyboard_keycode] = source.key_code;
        fields[f::keyboard_autorepeat] = source.is_repeat ? 1 : 0;
        fields[f::keyboard_type] = ansi_keyboard_type;
    } else if (source.type == event_type::scroll_wheel) {
        // Precise (touchpad) deltas are points; wheel deltas are lines.
        fields[f::scroll_is_continuous] = source.precise_scrolling ? 1 : 0;
        if (source.precise_scrolling) {
            fields[f::scroll_point_delta_axis1] = std::llround(source.delta_y);
            fields[f::scroll_point_delta_axis2] = std::llround(source.delta_x);
        } else {
            fields[f::scroll_delta_axis1] = std::llround(source.delta_y);
            fields[f::scroll_delta_axis2] = std::llround(source.delta_x);
        }
        fields[f::scroll_phase] = scroll_phase_to_cg(source.scroll_phase);
        fields[f::scroll_momentum_phase] = momentum_phase_to_cg(source.momentum_phase);
    }
    const auto made = guest_call(core_graphics_library, "_CGEventCreateFromData", {0, make_data(cg_event::encode(record))});
    if (!made) fail("-[NSEvent CGEvent]: CGEventCreateFromData rejected the event record");
    return made;
}

// +[NSEvent eventWithCGEvent:] (+1; window-less, location in screen points).
Address AppKit::make_event_from_cg(Address cg) {
    const auto data = guest_call(core_graphics_library, "_CGEventCreateData", {0, cg});
    if (!data) fail("+[NSEvent eventWithCGEvent:]: CGEventCreateData returned NULL");
    const auto bytes = data_bytes(data);
    release(data);
    const auto record = cg_event::decode(bytes);
    if (!record) fail("+[NSEvent eventWithCGEvent:]: not a CGEvent record");
    const auto field = [&](std::uint32_t id) {
        const auto found = record->fields.find(id);
        return found == record->fields.end() ? std::int64_t{0} : found->second;
    };
    namespace f = cg_event::field;
    Event source;
    source.type = record->type;
    source.location = {record->x, primary_screen_height("+[NSEvent eventWithCGEvent:]") - record->y};
    source.screen_location = source.location;
    source.modifier_flags = record->flags;
    source.timestamp = static_cast<double>(record->timestamp) / 1e9;
    std::int64_t mouse_subtype{};
    if (pointer_type(source.type) || enter_exit_type(source.type)) {
        source.click_count = field(f::mouse_click_state);
        source.button_number = field(f::mouse_button_number);
        source.delta_x = static_cast<double>(field(f::mouse_delta_x));
        source.delta_y = static_cast<double>(field(f::mouse_delta_y));
        mouse_subtype = field(f::mouse_subtype);
    } else if (key_type(source.type) || source.type == event_type::flags_changed) {
        source.key_code = static_cast<std::uint16_t>(field(f::keyboard_keycode));
        source.is_repeat = field(f::keyboard_autorepeat) != 0;
        if (key_type(source.type)) {
            // The characters the current layout produces for the key code.
            auto& server = require_window_server("+[NSEvent eventWithCGEvent:]");
            DeadKeyState dead{};
            dead.no_dead_keys = true;
            source.characters = server.translate_key(source.key_code, source.modifier_flags, dead);
            DeadKeyState plain{};
            plain.no_dead_keys = true;
            source.characters_ignoring_modifiers = server.translate_key(source.key_code, source.modifier_flags & modifier::shift, plain);
        }
    } else if (source.type == event_type::scroll_wheel) {
        source.precise_scrolling = field(f::scroll_is_continuous) != 0;
        source.delta_y = static_cast<double>(field(source.precise_scrolling ? f::scroll_point_delta_axis1 : f::scroll_delta_axis1));
        source.delta_x = static_cast<double>(field(source.precise_scrolling ? f::scroll_point_delta_axis2 : f::scroll_delta_axis2));
        source.scroll_phase = scroll_phase_from_cg(field(f::scroll_phase));
        source.momentum_phase = momentum_phase_from_cg(field(f::scroll_momentum_phase));
    }
    const auto value = make_event(source, 0);
    if (mouse_subtype) event_state(value)->subtype = mouse_subtype;
    return value;
}

std::shared_ptr<EventState> AppKit::event_state(Address value) {
    if (!value) fail("nil NSEvent");
    return objc.host_state<EventState>(value);
}

Address AppKit::make_event(const Event& source, Address target) {
    const auto value = objc.allocate(event);
    const auto state = objc.host_state<EventState>(value);
    state->event = source;
    state->window = target;
    {
        std::lock_guard lock(mutex);
        state->event_number = ++event_number;
    }
    // Pen input arrives as mouse events of the tablet-point subtype.
    const bool pen = source.pointing_device_type == pointing_device::pen || source.pointing_device_type == pointing_device::eraser;
    if (source.type == event_type::tablet_proximity) state->subtype = subtype::tablet_proximity;
    else if (pen && pointer_type(source.type)) state->subtype = subtype::tablet_point;
    return value;
}

void AppKit::enqueue_text(Address target, const Event& source) {
    // One key press (down and up) carrying `text`.
    const auto press = [&](std::string_view characters, std::string_view text, bool marked) {
        Event key = source;
        key.kind = EventKind::input;
        key.key_code = text_input_key_code;
        key.is_repeat = false;
        key.characters = characters;
        key.characters_ignoring_modifiers = characters;
        key.text = text;
        key.marked = marked;
        for (const auto type : {event_type::key_down, event_type::key_up}) {
            key.type = type;
            const auto value = make_event(key, target);
            event_state(value)->text_input = true;
            enqueue(value);
        }
    };
    if (source.marked) {
        press({}, source.text, true);   // a composition update (empty: the composition ended)
        return;
    }
    // Committed text: one press per character (UTF-8 sequence).
    const std::string_view text = source.text;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t length = 1;
        while (at + length < text.size() && (static_cast<unsigned char>(text[at + length]) & 0xc0) == 0x80) ++length;
        const auto character = text.substr(at, length);
        press(character, character, false);
        at += length;
    }
}

Address AppKit::run_monitors(Address value) {
    std::vector<Address> list;
    {
        std::lock_guard lock(mutex);
        list = monitors;
    }
    for (const auto monitor : list) {
        const auto info = objc.host_state<MonitorState>(monitor);
        if (!info->local || !(info->mask & mask_bit(event_state(value)->event.type))) continue;
        // The handler returns the event to dispatch, possibly another one, or nil.
        value = invoke_block(info->block, {value});
        if (!value) return 0;
        if (!is_kind(value, event)) fail("an NSEvent local monitor returned a non-event object");
    }
    return value;
}

void AppKit::install_event() {
    const auto e = event;
    const auto state = [this](Address value) { return event_state(value); };
    method(e, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto info = objc.find_host_state<EventState>(s.x[0]))
            for (auto* slot : {&info->characters, &info->characters_ignoring, &info->tracking_area, &info->cg_event})
                release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], event, sel("dealloc")));
    }, "v@:");
    method(e, "type", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.type; }, "Q@:");
    method(e, "modifierFlags", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.modifier_flags; }, "Q@:");
    method(e, "timestamp", [state](CpuState& s, GuestMemory&) { set_d(s, 0, state(s.x[0])->event.timestamp); }, "d@:");
    method(e, "window", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->window; });
    method(e, "windowNumber", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.window; }, "q@:");
    method(e, "locationInWindow", [state](CpuState& s, GuestMemory&) { return_point(s, state(s.x[0])->event.location); }, "{CGPoint=dd}@:");
    method(e, "eventNumber", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event_number; }, "q@:");
    method(e, "context", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(e, "subtype", [state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state(s.x[0])->subtype); }, "s@:");
    method(e, "data1", [state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state(s.x[0])->data1); }, "q@:");
    method(e, "data2", [state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state(s.x[0])->data2); }, "q@:");
    // Keyboard.
    method(e, "keyCode", [state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!key_type(info->event.type) && info->event.type != event_type::flags_changed)
            fail("-[NSEvent keyCode] sent to a non-key event");
        s.x[0] = info->event.key_code;
    }, "S@:");
    method(e, "isARepeat", [state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!key_type(info->event.type)) fail("-[NSEvent isARepeat] sent to a non-key event");
        return_bool(s, info->event.is_repeat);
    }, "B@:");
    method(e, "characters", [this, state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!key_type(info->event.type)) fail("-[NSEvent characters] sent to a non-key event");
        if (!info->characters) info->characters = string(info->event.characters);
        s.x[0] = info->characters;
    });
    method(e, "charactersIgnoringModifiers", [this, state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!key_type(info->event.type)) fail("-[NSEvent charactersIgnoringModifiers] sent to a non-key event");
        if (!info->characters_ignoring) info->characters_ignoring = string(info->event.characters_ignoring_modifiers);
        s.x[0] = info->characters_ignoring;
    });
    method(e, "charactersByApplyingModifiers:", [this, state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!key_type(info->event.type)) fail("-[NSEvent charactersByApplyingModifiers:] sent to a non-key event");
        DeadKeyState dead{};
        s.x[0] = auto_string(require_window_server("-[NSEvent charactersByApplyingModifiers:]")
                                 .translate_key(info->event.key_code, s.x[2], dead));
    }, "@@:Q");
    // Pointer.
    method(e, "clickCount", [state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state(s.x[0])->event.click_count); }, "q@:");
    method(e, "buttonNumber", [state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state(s.x[0])->event.button_number); }, "q@:");
    method(e, "buttonMask", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.pressed_buttons; }, "Q@:");
    method(e, "deltaX", [state](CpuState& s, GuestMemory&) { set_d(s, 0, state(s.x[0])->event.delta_x); }, "d@:");
    method(e, "deltaY", [state](CpuState& s, GuestMemory&) { set_d(s, 0, state(s.x[0])->event.delta_y); }, "d@:");
    method(e, "deltaZ", [state](CpuState& s, GuestMemory&) { set_d(s, 0, state(s.x[0])->event.delta_z); }, "d@:");
    method(e, "scrollingDeltaX", [state](CpuState& s, GuestMemory&) { set_d(s, 0, state(s.x[0])->event.delta_x); }, "d@:");
    method(e, "scrollingDeltaY", [state](CpuState& s, GuestMemory&) { set_d(s, 0, state(s.x[0])->event.delta_y); }, "d@:");
    method(e, "hasPreciseScrollingDeltas", [state](CpuState& s, GuestMemory&) { return_bool(s, state(s.x[0])->event.precise_scrolling); }, "B@:");
    method(e, "phase", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.scroll_phase; }, "Q@:");
    method(e, "momentumPhase", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.momentum_phase; }, "Q@:");
    method(e, "isDirectionInvertedFromDevice", [state](CpuState& s, GuestMemory&) { return_bool(s, state(s.x[0])->event.direction_inverted); }, "B@:");
    method(e, "magnification", [state](CpuState& s, GuestMemory&) { set_d(s, 0, state(s.x[0])->event.magnification); }, "d@:");
    method(e, "rotation", [state](CpuState& s, GuestMemory&) { set_f(s, 0, state(s.x[0])->event.rotation); }, "f@:");
    method(e, "stage", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(e, "trackingArea", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->tracking_area; });
    // Events of an NSTrackingArea: the area is the tracking number and its
    // userInfo the user data; +enterExitEventWithType:... events carry their own.
    method(e, "trackingNumber", [state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!enter_exit_type(info->event.type)) fail("-[NSEvent trackingNumber] sent to a non-enter/exit event");
        s.x[0] = info->tracking_area ? info->tracking_area : static_cast<Address>(info->tracking_number);
    }, "q@:");
    method(e, "userData", [this, state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!enter_exit_type(info->event.type)) fail("-[NSEvent userData] sent to a non-enter/exit event");
        s.x[0] = info->tracking_area ? objc.host_state<TrackingAreaState>(info->tracking_area)->user_info : info->user_data;
    }, "^v@:");
    // Tablet.
    method(e, "pressure", [state](CpuState& s, GuestMemory&) { set_f(s, 0, state(s.x[0])->event.pressure); }, "f@:");
    method(e, "tangentialPressure", [](CpuState& s, GuestMemory&) { set_f(s, 0, 0); }, "f@:");
    method(e, "tilt", [state](CpuState& s, GuestMemory&) { return_point(s, state(s.x[0])->event.tilt); }, "{CGPoint=dd}@:");
    method(e, "pointingDeviceType", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.pointing_device_type; }, "Q@:");
    for (const auto* name : {"pointingDeviceID", "deviceID", "tabletID", "systemTabletID"})
        method(e, name, [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->event.device_id; }, "Q@:");
    method(e, "isEnteringProximity", [state](CpuState& s, GuestMemory&) { return_bool(s, state(s.x[0])->event.entering_proximity); }, "B@:");
    method(e, "capabilityMask", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");
    for (const auto* name : {"absoluteX", "absoluteY", "absoluteZ"})
        method(e, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    // The CGEventRef stays owned by the event (made once, released with it).
    method(e, "CGEvent", [this, state](CpuState& s, GuestMemory&) {
        const auto info = state(s.x[0]);
        if (!info->cg_event) info->cg_event = make_cg_event(s.x[0]);
        s.x[0] = info->cg_event;
    }, "^{__CGEvent=}@:");
    // Carbon EventRefs do not exist under Alchemy: NULL, the documented
    // answer when none can be created.
    method(e, "eventRef", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "r^v@:");

    // ---- class methods ----
    class_method(e, "mouseLocation", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        return_point(s, server ? server->mouse_location() : Point{});
    }, "{CGPoint=dd}@:");
    class_method(e, "pressedMouseButtons", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        s.x[0] = server ? server->pressed_mouse_buttons() : 0;
    }, "Q@:");
    class_method(e, "modifierFlags", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        s.x[0] = server ? server->modifier_flags() : 0;
    }, "Q@:");
    class_method(e, "doubleClickInterval", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        set_d(s, 0, server ? server->double_click_interval() : 0.5);
    }, "d@:");
    class_method(e, "keyRepeatDelay", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        set_d(s, 0, server ? server->key_repeat_delay() : 0.5);
    }, "d@:");
    class_method(e, "keyRepeatInterval", [](CpuState& s, GuestMemory&) {
        const auto server = window_server_or_null();
        set_d(s, 0, server ? server->key_repeat_interval() : 0.033);
    }, "d@:");
    class_method(e, "isMouseCoalescingEnabled", [this](CpuState& s, GuestMemory&) { return_bool(s, coalesce_mouse); }, "B@:");
    class_method(e, "setMouseCoalescingEnabled:", [this](CpuState& s, GuestMemory&) { coalesce_mouse = bool_arg(s, 2); }, "v@:B");
    class_method(e, "isSwipeTrackingFromScrollEventsEnabled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    class_method(e, "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:",
                 [this](CpuState& s, GuestMemory& m) {
        Event source;
        source.type = static_cast<std::uint32_t>(s.x[2]);
        source.location = point_arg(s);
        source.modifier_flags = s.x[3];
        source.timestamp = d(s, 2);
        source.window = s.x[4];
        const auto value = make_event(source, source.window ? window_for_id(source.window) : 0);
        const auto info = event_state(value);
        info->subtype = static_cast<std::int16_t>(s.x[6]);
        info->data1 = static_cast<std::int64_t>(s.x[7]);
        info->data2 = static_cast<std::int64_t>(m.read64(s.sp));
        s.x[0] = autorelease(value);
    }, "@@:Q{CGPoint=dd}Qdq@sqq");
    class_method(e, "mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:",
                 [this](CpuState& s, GuestMemory&) {
        Event source;
        source.type = static_cast<std::uint32_t>(s.x[2]);
        source.location = point_arg(s);
        source.modifier_flags = s.x[3];
        source.timestamp = d(s, 2);
        source.window = s.x[4];
        source.click_count = static_cast<std::int64_t>(s.x[7]);
        source.pressure = f(s, 3);
        const auto value = make_event(source, source.window ? window_for_id(source.window) : 0);
        event_state(value)->event_number = s.x[6];
        s.x[0] = autorelease(value);
    }, "@@:Q{CGPoint=dd}Qdq@qqf");
    class_method(e, "keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:charactersIgnoringModifiers:isARepeat:keyCode:",
                 [this](CpuState& s, GuestMemory& m) {
        Event source;
        source.type = static_cast<std::uint32_t>(s.x[2]);
        source.location = point_arg(s);
        source.modifier_flags = s.x[3];
        source.timestamp = d(s, 2);
        source.window = s.x[4];
        source.characters = text(s.x[6]);
        source.characters_ignoring_modifiers = text(s.x[7]);
        // Apple ARM64 packs the BOOL and the unsigned short on the stack.
        std::array<std::uint8_t, 4> packed{};
        m.read_bytes(s.sp, packed);
        source.is_repeat = packed[0] != 0;
        source.key_code = static_cast<std::uint16_t>(packed[2] | (packed[3] << 8));
        s.x[0] = autorelease(make_event(source, source.window ? window_for_id(source.window) : 0));
    }, "@@:Q{CGPoint=dd}Qdq@@@BS");
    class_method(e, "eventWithCGEvent:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? autorelease(make_event_from_cg(s.x[2])) : 0;
    }, "@@:^{__CGEvent=}");
    class_method(e, "enterExitEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:trackingNumber:userData:",
                 [this](CpuState& s, GuestMemory& m) {
        Event source;
        source.type = static_cast<std::uint32_t>(s.x[2]);
        if (!enter_exit_type(source.type))
            fail("+[NSEvent enterExitEventWithType:...] needs NSEventTypeMouseEntered, MouseExited or CursorUpdate");
        source.location = point_arg(s);
        source.modifier_flags = s.x[3];
        source.timestamp = d(s, 2);
        source.window = s.x[4];
        const auto value = make_event(source, source.window ? window_for_id(source.window) : 0);
        const auto info = event_state(value);
        info->event_number = s.x[6];
        info->tracking_number = static_cast<std::int64_t>(s.x[7]);
        info->user_data = m.read64(s.sp);
        s.x[0] = autorelease(value);
    }, "@@:Q{CGPoint=dd}Qdq@qq^v");
    const auto add_monitor = [this](CpuState& s, bool local) {
        if (!s.x[3]) fail("event monitor without a handler");
        const auto block = guest_call(libsystem_library, "__Block_copy", {s.x[3]});
        const auto monitor = objc.allocate(event_monitor);
        const auto info = objc.host_state<MonitorState>(monitor);
        info->mask = s.x[2]; info->block = block; info->local = local;
        std::lock_guard lock(mutex);
        monitors.push_back(monitor);   // the list owns the monitor until -removeMonitor:
        s.x[0] = monitor;
    };
    class_method(e, "addLocalMonitorForEventsMatchingMask:handler:", [add_monitor](CpuState& s, GuestMemory&) { add_monitor(s, true); }, "@@:Q@?");
    // Global monitors observe other applications' events, which Alchemy's
    // process never receives, so their handlers are never called.
    class_method(e, "addGlobalMonitorForEventsMatchingMask:handler:", [add_monitor](CpuState& s, GuestMemory&) { add_monitor(s, false); }, "@@:Q@?");
    class_method(e, "removeMonitor:", [this](CpuState& s, GuestMemory&) {
        {
            std::lock_guard lock(mutex);
            const auto found = std::find(monitors.begin(), monitors.end(), s.x[2]);
            if (found == monitors.end()) return;
            monitors.erase(found);
        }
        const auto info = objc.host_state<MonitorState>(s.x[2]);
        if (const auto block = std::exchange(info->block, 0)) static_cast<void>(guest_call(libsystem_library, "__Block_release", {block}));
        release(s.x[2]);
    }, "v@:@");
    class_method(e, "startPeriodicEventsAfterDelay:withPeriod:", [](CpuState&, GuestMemory&) {
        unsupported("+[NSEvent startPeriodicEventsAfterDelay:withPeriod:] (periodic events)");
    }, "v@:dd");
    class_method(e, "stopPeriodicEvents", [](CpuState&, GuestMemory&) {}, "v@:");
}

} // namespace alchemy::gui::appkit
