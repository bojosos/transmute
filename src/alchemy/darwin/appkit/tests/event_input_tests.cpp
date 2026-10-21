// AppKit input tests (owner: w2 events) against the fake window server: the
// event paths Chrome's windowed run reaches. NSEvent local monitors,
// NSTrackingArea subclasses and their activity options, enter/exit events
// (+enterExitEventWithType:..., -trackingNumber, -userData), the CGEvent
// bridge (-CGEvent, +eventWithCGEvent:) through CoreGraphics' event records,
// NSCursor subclasses with -push / -pop, NSTextInputContext activation and
// the NSApplication delegate's notification registration. Native classes stand in for the
// guest subclasses, so every override is reached through ObjcRuntime::send.
// Input is synthesized inside the fake window server only.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"
#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/gui/cg_event_record.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace gui = alchemy::gui;
using Address = std::uint64_t;
using alchemy::CallRegs;
using alchemy::CallResult;
using transmute::CpuState;
using transmute::GuestMemory;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

std::vector<std::string> journal;
std::string dump_journal() {
    std::string text;
    for (const auto& entry : journal) text += (text.empty() ? "" : " | ") + entry;
    return text;
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices darwin{process};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    std::unique_ptr<alchemy::FoundationBridge> foundation;
    gui::testing::FakeWindowServer server;
    Address app{};

    Fixture() {
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        alchemy::install_darwin_frameworks(process, objc, cf);
        gui::appkit::set_window_server_override(&server);
        gui::InstallContext context{process, objc, cf};
        gui::install_appkit_gui(context);
        gui::install_quartzcore(context);
        gui::install_quartz2d(context);
        gui::install_security(context);              // the CF bridge the CoreGraphics gaps use
        gui::fill::install_graphics_gaps(context);   // CGEvent objects (-[NSEvent CGEvent])
        app = id(cls("NSApplication"), "sharedApplication");
        static_cast<void>(id(app, "finishLaunching"));
        static_cast<void>(id(app, "activateIgnoringOtherApps:", {1}));
    }
    ~Fixture() { gui::appkit::set_window_server_override(nullptr); }

    Address cls(std::string_view name) {
        const auto value = objc.find_class(name);
        require(value != 0, "missing class " + std::string(name));
        return value;
    }
    CallResult send(Address receiver, std::string_view selector, CallRegs registers = {}) {
        return objc.send(receiver, objc.selector(selector), std::move(registers));
    }
    Address id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        std::size_t index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return send(receiver, selector, registers).x0;
    }
    Address str(std::string_view text) { return cf.string(text); }
    std::string text(Address object) { return object ? cf.utf8(object) : std::string("(nil)"); }
    Address subclass(std::string name, std::string_view super) { return objc.register_native_class(std::move(name), cls(super), 8); }
    void add(Address target, std::string name, transmute::GuestCallHandler handler, std::string types = "v@:@") {
        static_cast<void>(objc.add_native_method(target, std::move(name), std::move(handler), false, std::move(types)));
    }
    Address next_event() { return id(app, "nextEventMatchingMask:untilDate:inMode:dequeue:", {~0ULL, 0, 0, 1}); }
    void pump() {
        while (const auto event = next_event()) static_cast<void>(id(app, "sendEvent:", {event}));
    }
    Address with_rect(Address receiver, std::string_view selector, gui::Rect rect) {
        CallRegs registers;
        gui::appkit::put_rect(registers, 0, rect);
        return send(receiver, selector, registers).x0;
    }
    gui::Point point_of(const CallResult& r) { return {r.d(0), r.d(1)}; }
    Address call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments) {
        const auto found = process.resolve(library, symbol);
        require(found.has_value(), "missing symbol " + std::string(symbol));
        return alchemy::EngineAdapter(engine).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
    // A compiler-emitted (global) block whose invoke runs `handler`.
    Address block(std::string name, transmute::GuestCallHandler handler) {
        const auto invoke = process.install_function(std::move(name), std::move(handler), "alchemy.test");
        const auto value = process.allocate_guest(32), descriptor = process.allocate_guest(16);
        engine.write64(descriptor, 0);
        engine.write64(descriptor + 8, 32);
        engine.write64(value, process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock")->address);
        engine.write32(value + 8, 1U << 28);   // BLOCK_IS_GLOBAL
        engine.write32(value + 12, 0);
        engine.write64(value + 16, invoke);
        engine.write64(value + 24, descriptor);
        return value;
    }
    Address make_window(gui::Rect content) {
        CallRegs init;
        gui::appkit::put_rect(init, 0, content);
        init.x[2] = gui::style::titled | gui::style::closable | gui::style::resizable;
        init.x[3] = 2;
        const auto window = send(id(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
        require(window != 0, "NSWindow creation");
        static_cast<void>(id(window, "setReleasedWhenClosed:", {0}));
        return window;
    }
    gui::Event pointer(Address window, std::uint32_t type, gui::Point location) {
        gui::Event event;
        event.type = type;
        event.window = id(window, "windowNumber");
        event.location = location;
        return event;
    }
    gui::cg_event::Record record_of(Address cg) {
        const auto data = call(gui::appkit::core_graphics_library, "_CGEventCreateData", {0, cg});
        require(data != 0, "CGEventCreateData");
        const auto length = cf.length(data);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        const std::array<Address, 1> arguments{data};
        if (!bytes.empty()) engine.read_bytes(cf.invoke("CFDataGetBytePtr", arguments), bytes);
        static_cast<void>(id(data, "release"));
        const auto record = gui::cg_event::decode(bytes);
        require(record.has_value(), "CGEventCreateData record");
        return *record;
    }
};

bool throws(const std::function<void()>& body) {
    try { body(); } catch (const std::exception&) { return true; }
    return false;
}

// +addLocalMonitorForEventsMatchingMask:handler: (Chrome installs one at
// startup): the handler sees matching events before -sendEvent: dispatches
// them, may swallow them by returning nil, and -removeMonitor: stops it.
void local_monitors(Fixture& f) {
    journal.clear();
    const auto view_class = f.subclass("EventsKeyView", "NSView");
    f.add(view_class, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    f.add(view_class, "keyDown:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("keyDown:" + std::to_string(f.id(s.x[2], "keyCode")));
    });
    const auto window = f.make_window({{100, 100}, {400, 300}});
    const auto view = f.with_rect(f.id(view_class, "alloc"), "initWithFrame:", {{0, 0}, {400, 300}});
    static_cast<void>(f.id(window, "setContentView:", {view}));
    static_cast<void>(f.id(window, "setInitialFirstResponder:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));

    static bool swallow = false;
    const auto handler = f.block("events_monitor_invoke", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("monitor:" + std::to_string(f.id(s.x[1], "type")));
        s.x[0] = swallow ? 0 : s.x[1];
    });
    const std::uint64_t key_down_mask = 1ULL << gui::event_type::key_down;
    const auto monitor = f.id(f.cls("NSEvent"), "addLocalMonitorForEventsMatchingMask:handler:", {key_down_mask, handler});
    require(monitor != 0, "+addLocalMonitorForEventsMatchingMask:handler: returned nil");
    const auto key = [&](std::uint32_t type) {
        gui::Event event;
        event.type = type;
        event.window = f.id(window, "windowNumber");
        event.key_code = 0;
        event.characters = event.characters_ignoring_modifiers = "a";
        return event;
    };
    f.server.queue(key(gui::event_type::key_down));
    f.server.queue(key(gui::event_type::key_up));   // outside the mask
    f.pump();
    require(journal == std::vector<std::string>{"monitor:10", "keyDown:0"}, "monitor before dispatch: " + dump_journal());

    journal.clear();
    swallow = true;
    f.server.queue(key(gui::event_type::key_down));
    f.pump();
    require(journal == std::vector<std::string>{"monitor:10"}, "a nil answer swallows the event: " + dump_journal());

    journal.clear();
    swallow = false;
    static_cast<void>(f.id(f.cls("NSEvent"), "removeMonitor:", {monitor}));
    f.server.queue(key(gui::event_type::key_down));
    f.pump();
    require(journal == std::vector<std::string>{"keyDown:0"}, "-removeMonitor: " + dump_journal());
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}

// NSTrackingArea subclasses (Chrome's CrTrackingArea overrides
// -initWithRect:options:owner:userInfo:), mouse entered / moved / exited to
// the owner, the events' -trackingArea / -trackingNumber / -userData, and the
// activity options.
void tracking_areas(Fixture& f) {
    journal.clear();
    constexpr std::uint64_t entered_exited = 0x01, moved = 0x02, when_first_responder = 0x10, always = 0x80,
        assume_inside = 0x100, during_drag = 0x400;
    const auto area_class = f.subclass("EventsCrTrackingArea", "NSTrackingArea");
    f.add(area_class, "initWithRect:options:owner:userInfo:", [&f, area_class](CpuState& s, GuestMemory&) {
        journal.push_back("init:" + std::to_string(s.x[2]));
        CallRegs registers;
        for (unsigned index = 0; index < 4; ++index) registers.v[index] = s.v[index];
        registers.x[2] = s.x[2]; registers.x[3] = s.x[3]; registers.x[4] = s.x[4];
        s.x[0] = f.objc.send_super(s.x[0], area_class, f.objc.selector("initWithRect:options:owner:userInfo:"), registers).x0;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}Q@@");
    static Address expected_area{}, expected_info{};
    const auto owner_class = f.subclass("EventsTrackingView", "NSView");
    f.add(owner_class, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    for (const auto* name : {"mouseEntered:", "mouseExited:", "mouseMoved:"}) {
        const std::string label(name);
        f.add(owner_class, label, [&f, label](CpuState& s, GuestMemory&) {
            const auto event = s.x[2];
            const auto where = f.point_of(f.send(event, "locationInWindow"));
            std::string entry = label + std::to_string(static_cast<int>(where.x)) + "," + std::to_string(static_cast<int>(where.y));
            if (label != "mouseMoved:") {
                const bool linked = f.id(event, "trackingArea") == expected_area && f.id(event, "trackingNumber") == expected_area &&
                                    f.id(event, "userData") == expected_info;
                entry += linked ? ":area" : ":unlinked";
            }
            journal.push_back(entry);
        });
    }
    const auto window = f.make_window({{100, 100}, {400, 300}});
    const auto content = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {400, 300}});
    static_cast<void>(f.id(window, "setContentView:", {content}));
    const auto view = f.with_rect(f.id(owner_class, "alloc"), "initWithFrame:", {{100, 100}, {100, 100}});
    static_cast<void>(f.id(content, "addSubview:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));

    const auto make_area = [&](std::uint64_t options) {
        CallRegs init;
        gui::appkit::put_rect(init, 0, {{0, 0}, {100, 100}});
        init.x[2] = options; init.x[3] = view; init.x[4] = f.str("info");
        const auto area = f.send(f.id(area_class, "alloc"), "initWithRect:options:owner:userInfo:", init).x0;
        require(area && f.id(area, "options") == options && f.id(area, "owner") == view, "NSTrackingArea subclass init");
        expected_area = area;
        expected_info = f.id(area, "userInfo");
        static_cast<void>(f.id(view, "addTrackingArea:", {area}));
        static_cast<void>(f.id(area, "release"));
        return area;
    };
    const auto move = [&](gui::Point location, std::uint32_t type = gui::event_type::mouse_moved) {
        f.server.queue(f.pointer(window, type, location));
        f.pump();
    };

    // Active always: entered, moved, exited (window coordinates).
    auto area = make_area(entered_exited | moved | always);
    require(journal == std::vector<std::string>{"init:" + std::to_string(entered_exited | moved | always)}, "subclass init: " + dump_journal());
    journal.clear();
    move({150, 150});
    move({160, 170});
    move({20, 20});
    require(journal == std::vector<std::string>{"mouseEntered:150,150:area", "mouseMoved:150,150", "mouseMoved:160,170",
                                                "mouseExited:20,20:area"},
            "active-always area: " + dump_journal());
    static_cast<void>(f.id(view, "removeTrackingArea:", {area}));

    // Mouse drags: no crossing unless NSTrackingEnabledDuringMouseDrag.
    area = make_area(entered_exited | always);
    journal.clear();
    move({150, 150}, gui::event_type::left_mouse_dragged);
    move({20, 20}, gui::event_type::left_mouse_dragged);
    require(journal.empty(), "crossings during a drag: " + dump_journal());
    static_cast<void>(f.id(view, "removeTrackingArea:", {area}));
    area = make_area(entered_exited | always | during_drag);
    journal.clear();
    move({150, 150}, gui::event_type::left_mouse_dragged);
    require(journal == std::vector<std::string>{"mouseEntered:150,150:area"}, "NSTrackingEnabledDuringMouseDrag: " + dump_journal());
    move({20, 20});
    static_cast<void>(f.id(view, "removeTrackingArea:", {area}));

    // Active while the owner is first responder.
    area = make_area(entered_exited | when_first_responder);
    journal.clear();
    move({150, 150});
    move({20, 20});
    require(journal.empty(), "first-responder area while another view is first responder: " + dump_journal());
    static_cast<void>(f.id(window, "makeFirstResponder:", {view}));
    move({150, 150});
    require(journal == std::vector<std::string>{"mouseEntered:150,150:area"}, "first-responder area: " + dump_journal());
    move({20, 20});
    static_cast<void>(f.id(view, "removeTrackingArea:", {area}));

    // NSTrackingAssumeInside: the first crossing is an exit.
    area = make_area(entered_exited | always | assume_inside);
    journal.clear();
    move({20, 20});
    require(journal == std::vector<std::string>{"mouseExited:20,20:area"}, "NSTrackingAssumeInside: " + dump_journal());
    static_cast<void>(f.id(view, "removeTrackingArea:", {area}));
    require(f.cf.length(f.id(view, "trackingAreas")) == 0, "-removeTrackingArea:");
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}

// +enterExitEventWithType:location:modifierFlags:timestamp:windowNumber:
// context:eventNumber:trackingNumber:userData: and -eventRef.
void enter_exit_events(Fixture& f) {
    const auto make = [&](std::uint64_t type) {
        CallRegs registers;
        registers.x[2] = type;
        registers.set_d(0, 5); registers.set_d(1, 6);
        registers.x[3] = gui::modifier::shift;
        registers.set_d(2, 12.5);
        registers.x[4] = 0; registers.x[5] = 0; registers.x[6] = 7; registers.x[7] = 9;
        registers.push_stack(std::uint64_t{0x1234});
        return f.send(f.cls("NSEvent"),
            "enterExitEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:trackingNumber:userData:",
            registers).x0;
    };
    const auto exited = make(gui::event_type::mouse_exited);
    const auto where = f.point_of(f.send(exited, "locationInWindow"));
    require(exited && f.id(exited, "type") == gui::event_type::mouse_exited && near(where.x, 5) && near(where.y, 6) &&
                f.id(exited, "modifierFlags") == gui::modifier::shift && near(f.send(exited, "timestamp").d(0), 12.5) &&
                f.id(exited, "eventNumber") == 7 && f.id(exited, "trackingNumber") == 9 && f.id(exited, "userData") == 0x1234 &&
                f.id(exited, "trackingArea") == 0 && f.id(exited, "window") == 0,
            "+enterExitEventWithType: fields");
    require(f.id(exited, "eventRef") == 0, "-eventRef answers NULL");
    require(throws([&] { static_cast<void>(make(gui::event_type::left_mouse_down)); }),
            "+enterExitEventWithType: accepted a mouse-down type");
    const auto update = make(gui::event_type::cursor_update);
    require(update && f.id(update, "trackingNumber") == 9, "+enterExitEventWithType: of a cursor update");
    require(throws([&] { static_cast<void>(f.id(update, "keyCode")); }), "-keyCode of a cursor-update event");
}

// -[NSEvent CGEvent] and +[NSEvent eventWithCGEvent:]: global display
// coordinates (top-left origin of the 1440x900 primary screen), flags,
// timestamp and the keyboard / mouse / scroll fields, both ways.
void cg_event_bridge(Fixture& f) {
    namespace field = gui::cg_event::field;
    const auto window = f.make_window({{100, 100}, {400, 300}});
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));

    auto key = f.pointer(window, gui::event_type::key_down, {10, 20});
    key.key_code = 0; key.characters = key.characters_ignoring_modifiers = "A"; key.is_repeat = true;
    key.modifier_flags = gui::modifier::shift; key.timestamp = 2.5;
    f.server.queue(key);
    const auto event = f.next_event();
    require(event && f.id(event, "type") == gui::event_type::key_down, "queued key event");
    const auto cg = f.id(event, "CGEvent");
    require(cg != 0 && f.id(event, "CGEvent") == cg, "-CGEvent is made once and kept by the event");
    const auto record = f.record_of(cg);
    require(record.type == gui::event_type::key_down && near(record.x, 110) && near(record.y, 900 - 120) &&
                record.flags == gui::modifier::shift && record.timestamp == 2'500'000'000ULL &&
                record.fields.at(field::keyboard_keycode) == 0 && record.fields.at(field::keyboard_autorepeat) == 1 &&
                record.fields.at(field::keyboard_type) == 40,
            "-CGEvent of a key press");
    const auto back = f.id(f.cls("NSEvent"), "eventWithCGEvent:", {cg});
    const auto where = f.point_of(f.send(back, "locationInWindow"));
    require(back && back != event && f.id(back, "type") == gui::event_type::key_down && f.id(back, "window") == 0 &&
                near(where.x, 110) && near(where.y, 120) && f.id(back, "keyCode") == 0 && (f.id(back, "isARepeat") & 0xff) == 1 &&
                f.text(f.id(back, "characters")) == "A" && f.text(f.id(back, "charactersIgnoringModifiers")) == "A" &&
                f.id(back, "modifierFlags") == gui::modifier::shift && near(f.send(back, "timestamp").d(0), 2.5),
            "+eventWithCGEvent: of a key press");
    require(f.id(f.cls("NSEvent"), "eventWithCGEvent:", {0}) == 0, "+eventWithCGEvent: of NULL");

    auto click = f.pointer(window, gui::event_type::right_mouse_down, {30, 40});
    click.button_number = 1; click.click_count = 2;
    f.server.queue(click);
    const auto mouse = f.next_event();
    const auto mouse_record = f.record_of(f.id(mouse, "CGEvent"));
    require(mouse_record.type == gui::event_type::right_mouse_down && near(mouse_record.x, 130) && near(mouse_record.y, 900 - 140) &&
                mouse_record.fields.at(field::mouse_button_number) == 1 && mouse_record.fields.at(field::mouse_click_state) == 2,
            "-CGEvent of a mouse press");

    auto scroll = f.pointer(window, gui::event_type::scroll_wheel, {50, 60});
    scroll.precise_scrolling = true; scroll.delta_y = 12; scroll.delta_x = -3; scroll.scroll_phase = gui::phase::began;
    f.server.queue(scroll);
    const auto wheel = f.next_event();
    const auto wheel_cg = f.id(wheel, "CGEvent");
    const auto wheel_record = f.record_of(wheel_cg);
    require(wheel_record.fields.at(field::scroll_is_continuous) == 1 && wheel_record.fields.at(field::scroll_point_delta_axis1) == 12 &&
                wheel_record.fields.at(field::scroll_point_delta_axis2) == -3 && wheel_record.fields.at(field::scroll_phase) == 1,
            "-CGEvent of a trackpad scroll");
    const auto wheel_back = f.id(f.cls("NSEvent"), "eventWithCGEvent:", {wheel_cg});
    require(near(f.send(wheel_back, "scrollingDeltaY").d(0), 12) && near(f.send(wheel_back, "scrollingDeltaX").d(0), -3) &&
                (f.id(wheel_back, "hasPreciseScrollingDeltas") & 0xff) == 1 && f.id(wheel_back, "phase") == gui::phase::began,
            "+eventWithCGEvent: of a trackpad scroll");
    for (const auto value : {event, mouse, wheel}) static_cast<void>(f.id(value, "release"));
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}

// NSCursor subclasses: -push and -pop go through -set (an override sees
// them), +currentCursor follows, and the current cursor stays valid after its
// creator releases it.
void cursor_subclass(Fixture& f) {
    journal.clear();
    const auto cursor_class = f.subclass("EventsCrCursor", "NSCursor");
    f.add(cursor_class, "set", [&f, cursor_class](CpuState& s, GuestMemory&) {
        journal.push_back("set");
        static_cast<void>(f.objc.send_super(s.x[0], cursor_class, f.objc.selector("set"), {}));
    }, "v@:");
    const auto arrow = f.id(f.cls("NSCursor"), "arrowCursor");
    static_cast<void>(f.id(arrow, "set"));
    // A 1x1 image cursor of the subclass.
    CallRegs bitmap;
    bitmap.x[2] = 0; bitmap.x[3] = 1; bitmap.x[4] = 1; bitmap.x[5] = 8; bitmap.x[6] = 4; bitmap.x[7] = 1;
    bitmap.push_stack(std::uint8_t{0});
    bitmap.push_stack(f.str("NSDeviceRGBColorSpace"));
    bitmap.push_stack(std::int64_t{4});
    bitmap.push_stack(std::int64_t{32});
    const auto rep = f.send(f.id(f.cls("NSBitmapImageRep"), "alloc"),
        "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
        bitmap).x0;
    CallRegs size;
    gui::appkit::put_size(size, 0, {1, 1});
    const auto picture = f.send(f.id(f.cls("NSImage"), "alloc"), "initWithSize:", size).x0;
    static_cast<void>(f.id(picture, "addRepresentation:", {rep}));
    CallRegs hot;
    gui::appkit::put_point(hot, 0, {0, 0});
    hot.x[2] = picture;
    const auto custom = f.send(f.id(cursor_class, "alloc"), "initWithImage:hotSpot:", hot).x0;
    require(custom != 0 && f.id(custom, "image") == picture, "NSCursor subclass -initWithImage:hotSpot:");

    static_cast<void>(f.id(custom, "push"));
    require(journal == std::vector<std::string>{"set"}, "-push did not send -set: " + dump_journal());
    require(f.id(f.cls("NSCursor"), "currentCursor") == custom, "+currentCursor after -push");
    const auto created = f.server.cursors_created.size();
    static_cast<void>(f.id(custom, "release"));   // the creator lets go while it is shown
    require(f.id(f.cls("NSCursor"), "currentCursor") == custom && f.id(custom, "image") == picture,
            "the current cursor was freed by its creator's release");
    static_cast<void>(f.id(f.cls("NSCursor"), "pop"));
    require(f.id(f.cls("NSCursor"), "currentCursor") == arrow && !f.server.cursors_set.empty() &&
                f.server.cursors_set.back() == 100 + static_cast<int>(gui::SystemCursor::arrow),
            "+pop restores the arrow");
    require(f.server.cursors_created.size() == created, "cursor re-created");
    static_cast<void>(f.id(picture, "release"));
    static_cast<void>(f.id(rep, "release"));
}

// The active NSTextInputContext follows the key window's first responder's
// -inputContext: Chrome's BridgedContentView answers nil while no text field
// has focus, which turns text input (IME) off; the contexts get -activate /
// -deactivate as focus moves.
void text_input_contexts(Fixture& f) {
    journal.clear();
    const auto context_class = f.subclass("EventsTextInputContext", "NSTextInputContext");
    for (const auto* name : {"activate", "deactivate"}) {
        const std::string label(name);
        f.add(context_class, label, [&f, context_class, label](CpuState& s, GuestMemory&) {
            journal.push_back(label);
            static_cast<void>(f.objc.send_super(s.x[0], context_class, f.objc.selector(label), {}));
        }, "v@:");
    }
    static bool editable = false;
    static Address own_context{};
    const auto view_class = f.subclass("EventsTextClientView", "NSView");
    f.add(view_class, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    f.add(view_class, "insertText:replacementRange:", [](CpuState&, GuestMemory&) {}, "v@:@{_NSRange=QQ}");
    f.add(view_class, "inputContext", [](CpuState& s, GuestMemory&) { s.x[0] = editable ? own_context : 0; }, "@@:");
    const auto window = f.make_window({{100, 100}, {400, 300}});
    const auto number = f.id(window, "windowNumber");
    const auto content = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {400, 300}});
    static_cast<void>(f.id(window, "setContentView:", {content}));
    const auto view = f.with_rect(f.id(view_class, "alloc"), "initWithFrame:", {{0, 0}, {200, 100}});
    static_cast<void>(f.id(content, "addSubview:", {view}));
    own_context = f.id(f.id(context_class, "alloc"), "initWithClient:", {view});
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));

    static_cast<void>(f.id(window, "makeFirstResponder:", {view}));
    require(!f.server.at(number).text_input && journal.empty(), "a nil -inputContext enabled text input: " + dump_journal());
    editable = true;
    static_cast<void>(f.id(window, "makeFirstResponder:", {0}));
    static_cast<void>(f.id(window, "makeFirstResponder:", {view}));
    require(f.server.at(number).text_input && journal == std::vector<std::string>{"activate"},
            "the first responder's context was not activated: " + dump_journal());
    static_cast<void>(f.id(window, "makeFirstResponder:", {0}));
    require(!f.server.at(number).text_input && journal == std::vector<std::string>{"activate", "deactivate"},
            "the context was not deactivated: " + dump_journal());
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
    static_cast<void>(f.id(view, "release"));
    static_cast<void>(f.id(own_context, "release"));
}

// -[NSApplication setDelegate:] registers the delegate for the NSApplication
// notifications it implements: one the application posts itself (Chrome's
// -terminate: override posts NSApplicationWillTerminateNotification) reaches
// it, AppKit's own posts reach it once, and a replaced delegate hears nothing.
void application_delegate_notifications(Fixture& f) {
    journal.clear();
    const auto delegate_class = f.subclass("EventsAppDelegate", "NSObject");
    for (const auto* name : {"applicationWillTerminate:", "applicationWillHide:", "applicationDidUnhide:"}) {
        const std::string label(name);
        f.add(delegate_class, label, [&f, label](CpuState& s, GuestMemory&) {
            journal.push_back(label + f.text(f.id(s.x[2], "name")));
        });
    }
    const auto previous = f.id(f.app, "delegate");
    const auto first = f.id(f.id(delegate_class, "alloc"), "init");
    const auto second = f.id(f.id(delegate_class, "alloc"), "init");
    static_cast<void>(f.id(f.app, "setDelegate:", {first}));
    const auto center = f.id(f.cls("NSNotificationCenter"), "defaultCenter");
    static_cast<void>(f.id(center, "postNotificationName:object:", {f.str("NSApplicationWillTerminateNotification"), f.app}));
    require(journal == std::vector<std::string>{"applicationWillTerminate:NSApplicationWillTerminateNotification"},
            "a posted NSApplicationWillTerminateNotification did not reach the delegate: " + dump_journal());
    journal.clear();
    static_cast<void>(f.id(f.app, "hide:", {0}));
    static_cast<void>(f.id(f.app, "unhide:", {0}));
    require(journal == std::vector<std::string>{"applicationWillHide:NSApplicationWillHideNotification",
                                                "applicationDidUnhide:NSApplicationDidUnhideNotification"},
            "AppKit's notifications reach the delegate once: " + dump_journal());
    journal.clear();
    static_cast<void>(f.id(f.app, "setDelegate:", {second}));
    static_cast<void>(f.id(center, "postNotificationName:object:", {f.str("NSApplicationWillTerminateNotification"), f.app}));
    require(journal.size() == 1, "the replaced delegate still observes, or the new one does not: " + dump_journal());
    static_cast<void>(f.id(f.app, "setDelegate:", {previous}));
    journal.clear();
    static_cast<void>(f.id(center, "postNotificationName:object:", {f.str("NSApplicationWillTerminateNotification"), f.app}));
    require(journal.empty(), "a cleared delegate still observes: " + dump_journal());
    static_cast<void>(f.id(first, "release"));
    static_cast<void>(f.id(second, "release"));
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("ALCHEMY_GUI_NO_ACTIVATE", "1");
#endif
    try {
        Fixture fixture;
        local_monitors(fixture);
        tracking_areas(fixture);
        enter_exit_events(fixture);
        cg_event_bridge(fixture);
        cursor_subclass(fixture);
        text_input_contexts(fixture);
        application_delegate_notifications(fixture);
        std::cout << "Alchemy GUI AppKit input tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AppKit input test failed: " << error.what() << '\n';
        return 1;
    }
}
