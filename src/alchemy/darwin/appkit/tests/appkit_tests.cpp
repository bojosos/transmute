// AppKit unit tests (owner: A4) against a fake window server: NSApplication
// launching and delegate callbacks, window-server events converted to
// NSEvents, delivery through -sendEvent: and the responder chain to
// subclasses (native classes standing in for guest subclasses, so every
// call goes through ObjcRuntime::send), window delegate notifications,
// NSRect/NSSize/NSPoint marshalling both ways, IME text, pasteboard, cursors,
// screens, images, and the Carbon/CoreGraphics functions.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <commctrl.h>
#undef near   // minwindef.h; the tests use near() for doubles
#undef far
#endif

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
bool same(gui::Rect a, gui::Rect b) {
    return near(a.origin.x, b.origin.x) && near(a.origin.y, b.origin.y) && near(a.size.width, b.size.width) &&
           near(a.size.height, b.size.height);
}
std::string describe(gui::Rect r) {
    return "{" + std::to_string(r.origin.x) + "," + std::to_string(r.origin.y) + "," + std::to_string(r.size.width) + "," +
           std::to_string(r.size.height) + "}";
}

std::vector<std::string> journal;   // calls received by the stand-in subclasses
bool log_has(const std::string& entry) { return std::find(journal.begin(), journal.end(), entry) != journal.end(); }

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

    // `run_loop_first`: the run loop is installed before AppKit, as
    // install_darwin_gui does (the other tests install it later, if at all).
    explicit Fixture(bool run_loop_first = false) {
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        alchemy::install_darwin_frameworks(process, objc, cf);
        gui::appkit::set_window_server_override(&server);
        gui::quartz2d::set_screens_override([this] { return server.screens(); });   // CGDisplay* (darwin/coregraphics/displays.cpp)
        gui::InstallContext context{process, objc, cf};
        if (run_loop_first) gui::install_runloop(context);
        gui::install_appkit_gui(context);
        gui::install_quartzcore(context);   // CAMetalLayer presentation hosts
        gui::install_quartz2d(context);     // AppKit drawing (NSGraphicsContext, NSBezierPath, ...), as in install_darwin_gui
    }
    ~Fixture() {
        gui::appkit::set_window_server_override(nullptr);
        gui::quartz2d::set_screens_override({});
        gui::appkit::set_alert_presenter_override({});
    }

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
    // Delivers the fake server's queued events and dispatches every NSEvent.
    void pump() {
        while (const auto event = next_event()) static_cast<void>(id(app, "sendEvent:", {event}));
    }
    gui::Rect rect_of(const CallResult& r) { return {{r.d(0), r.d(1)}, {r.d(2), r.d(3)}}; }
    Address with_rect(Address receiver, std::string_view selector, gui::Rect rect) {
        CallRegs registers;
        gui::appkit::put_rect(registers, 0, rect);
        return send(receiver, selector, registers).x0;
    }
    std::string symbol_text(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing symbol " + std::string(name));
        return cf.utf8(engine.read64(found->address));
    }
};

void application(Fixture& f) {
    const auto slot = f.process.resolve(gui::appkit::appkit_library, "_NSApp");
    require(slot && f.engine.read64(slot->address) == 0, "NSApp is set before +sharedApplication");
    f.app = f.id(f.cls("NSApplication"), "sharedApplication");
    require(f.app && f.engine.read64(slot->address) == f.app, "+sharedApplication did not set the unified NSApp cell");
    require(f.id(f.cls("NSApplication"), "sharedApplication") == f.app, "+sharedApplication is not a singleton");
    require(f.symbol_text(gui::appkit::cocoa_library, "_NSApplicationDidFinishLaunchingNotification") ==
                "NSApplicationDidFinishLaunchingNotification", "notification name constant");
    for (const auto library : {gui::appkit::appkit_library, gui::appkit::cocoa_library}) {
        require(f.symbol_text(library, "_NSEventTrackingRunLoopMode") == "NSEventTrackingRunLoopMode", "event tracking mode constant");
        require(f.symbol_text(library, "_NSModalPanelRunLoopMode") == "NSModalPanelRunLoopMode", "modal panel mode constant");
    }

    const auto delegate_class = f.subclass("TestAppDelegate", "NSObject");
    for (const auto* name : {"applicationWillFinishLaunching:", "applicationDidFinishLaunching:", "applicationDidBecomeActive:",
             "observedLaunch:"}) {
        const std::string entry = name;
        f.add(delegate_class, entry, [entry](CpuState& s, GuestMemory&) { journal.push_back(entry); });
    }
    f.add(delegate_class, "applicationShouldTerminate:", [](CpuState& s, GuestMemory&) {
        journal.push_back("applicationShouldTerminate:");
        s.x[0] = 0;   // NSTerminateCancel
    }, "Q@:@");
    const auto delegate = f.id(f.id(delegate_class, "alloc"), "init");
    static_cast<void>(f.id(f.app, "setDelegate:", {delegate}));
    require(f.id(f.app, "delegate") == delegate, "-[NSApplication delegate]");
    // A notification-center observer sees the launch too.
    const auto center = f.id(f.cls("NSNotificationCenter"), "defaultCenter");
    static_cast<void>(f.id(center, "addObserver:selector:name:object:",
        {delegate, f.objc.selector("observedLaunch:"), f.str("NSApplicationDidFinishLaunchingNotification"), 0}));
    require((f.id(f.app, "setActivationPolicy:", {0}) & 0xff) == 1, "-setActivationPolicy: rejected Regular");
    static_cast<void>(f.id(f.app, "finishLaunching"));
    require(journal.size() >= 3 && journal[0] == "applicationWillFinishLaunching:" &&
                log_has("applicationDidFinishLaunching:") && log_has("observedLaunch:"),
            "launch delegate callbacks are missing or out of order");
    static_cast<void>(f.id(f.app, "finishLaunching"));
    require(std::count(journal.begin(), journal.end(), "applicationDidFinishLaunching:") == 1, "-finishLaunching repeated callbacks");

    // Launch activation (GHOST_WindowCocoa): the first window is ordered
    // front and made key before its delegate is set; it becomes key, and the
    // delegate hears windowDidBecomeKey:, when the event loop first runs.
    {
        const auto launch_delegate_class = f.subclass("TestLaunchWindowDelegate", "NSObject");
        f.add(launch_delegate_class, "windowDidBecomeKey:", [](CpuState&, GuestMemory&) { journal.push_back("launch:windowDidBecomeKey:"); });
        CallRegs init;
        gui::appkit::put_rect(init, 0, {{0, 0}, {320, 240}});
        init.x[2] = gui::style::titled;
        init.x[3] = 2;
        const auto first = f.send(f.id(f.cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
        static_cast<void>(f.id(first, "setReleasedWhenClosed:", {0}));
        static_cast<void>(f.id(first, "makeKeyAndOrderFront:", {0}));
        const auto launch_delegate = f.id(f.id(launch_delegate_class, "alloc"), "init");
        static_cast<void>(f.id(first, "setDelegate:", {launch_delegate}));
        require((f.id(first, "isKeyWindow") & 0xff) == 0 && !log_has("launch:windowDidBecomeKey:"),
                "the launch window became key before the event loop ran");
        require(f.next_event() == 0, "launch: unexpected event");
        require((f.id(first, "isKeyWindow") & 0xff) == 1 && log_has("launch:windowDidBecomeKey:"),
                "the launch window did not become key (with its delegate told) when the event loop ran");
        static_cast<void>(f.id(first, "close"));
        static_cast<void>(f.id(first, "release"));
        static_cast<void>(f.id(launch_delegate, "release"));
    }
    // Only the observation added above: -removeObserver: would also drop the
    // delegate's own NSApplication notifications (-setDelegate: registers them).
    static_cast<void>(f.id(center, "removeObserver:name:object:", {delegate, f.str("NSApplicationDidFinishLaunchingNotification"), 0}));

    static_cast<void>(f.id(f.app, "terminate:", {0}));
    require(log_has("applicationShouldTerminate:"), "-terminate: did not ask the delegate");
    require((f.id(f.app, "isActive") & 0xff) == 0, "application is active before activation");
    static_cast<void>(f.id(f.app, "activateIgnoringOtherApps:", {1}));
    require((f.id(f.app, "isActive") & 0xff) == 1 && log_has("applicationDidBecomeActive:"), "activation callbacks");

    // Application-defined events round-trip through -postEvent:atStart:.
    CallRegs other;
    other.x[2] = gui::event_type::application_defined;
    other.x[3] = 0; other.x[4] = 0; other.x[5] = 0; other.x[6] = 7; other.x[7] = 42;
    other.push_stack(std::int64_t{43});
    const auto custom = f.send(f.cls("NSEvent"),
        "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:", other).x0;
    require(custom && f.id(custom, "data1") == 42 && f.id(custom, "data2") == 43 && f.id(custom, "subtype") == 7,
            "+otherEventWithType: arguments");
    static_cast<void>(f.id(f.app, "postEvent:atStart:", {custom, 0}));
    const auto echoed = f.next_event();
    require(echoed == custom && f.id(f.app, "currentEvent") == custom, "posted event did not come back from the queue");
    require(f.next_event() == 0, "event queue is not empty");
}

void windows_and_events(Fixture& f) {
    journal.clear();
    // Stand-ins for guest subclasses: a view (CocoaMetalView) and a window
    // delegate (BlenderWindowDelegate).
    const auto view_class = f.subclass("TestView", "NSView");
    f.add(view_class, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    const auto record_key = [&f](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto code = f.id(event, "keyCode");
        const auto characters = f.text(f.id(event, "characters"));
        journal.push_back("keyDown:" + std::to_string(code) + ":" + characters);
        // Like a text view: input-method key presses go through the key bindings.
        if (code == gui::appkit::text_input_key_code)
            static_cast<void>(f.id(self, "interpretKeyEvents:", {f.cf.array(std::array<Address, 1>{event})}));
    };
    f.add(view_class, "keyDown:", record_key);
    f.add(view_class, "mouseDown:", [&f](CpuState& s, GuestMemory&) {
        const auto r = f.send(s.x[2], "locationInWindow");
        journal.push_back("mouseDown:" + std::to_string(static_cast<int>(r.d(0))) + "," + std::to_string(static_cast<int>(r.d(1))));
    });
    f.add(view_class, "scrollWheel:", [&f](CpuState& s, GuestMemory&) {
        const auto delta = f.send(s.x[2], "scrollingDeltaY").d(0);
        const bool precise = (f.id(s.x[2], "hasPreciseScrollingDeltas") & 0xff) != 0;
        journal.push_back("scrollWheel:" + std::to_string(static_cast<int>(delta)) + (precise ? ":precise" : ""));
    });
    f.add(view_class, "drawRect:", [](CpuState& s, GuestMemory&) {
        journal.push_back("drawRect:" + describe({{gui::appkit::d(s, 0), gui::appkit::d(s, 1)}, {gui::appkit::d(s, 2), gui::appkit::d(s, 3)}}));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    f.add(view_class, "insertText:replacementRange:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("insertText:" + f.text(s.x[2]) + (s.x[3] == gui::appkit::not_found ? ":notfound" : ":range"));
    }, "v@:@{_NSRange=QQ}");
    f.add(view_class, "setMarkedText:selectedRange:replacementRange:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("setMarkedText:" + f.text(s.x[2]) + ":" + std::to_string(s.x[3]) + "," + std::to_string(s.x[4]));
    }, "v@:@{_NSRange=QQ}{_NSRange=QQ}");
    f.add(view_class, "doCommandBySelector:", [&f](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto bytes = m.read_string(s.x[2], 1, 256, &terminated);
        journal.push_back("doCommandBySelector:" + std::string(bytes.begin(), bytes.end()));
    }, "v@::");
    f.add(view_class, "selectedRange", [](CpuState& s, GuestMemory&) { s.x[0] = 3; s.x[1] = 0; }, "{_NSRange=QQ}@:");
    f.add(view_class, "firstRectForCharacterRange:actualRange:", [](CpuState& s, GuestMemory&) {
        journal.push_back("firstRect:" + std::to_string(s.x[2]));
        gui::appkit::return_rect(s, {{150, 160}, {2, 20}});   // screen points
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}");

    const auto delegate_class = f.subclass("TestWindowDelegate", "NSObject");
    for (const auto* name : {"windowDidBecomeKey:", "windowDidResignKey:", "windowDidResize:", "windowDidMove:", "windowWillClose:",
             "windowDidMiniaturize:", "windowWillEnterFullScreen:", "windowDidEnterFullScreen:",
             "windowDidChangeBackingProperties:"}) {
        const std::string entry = name;
        f.add(delegate_class, entry, [entry](CpuState&, GuestMemory&) { journal.push_back(entry); });
    }
    static bool allow_close = false;
    f.add(delegate_class, "windowShouldClose:", [](CpuState& s, GuestMemory&) {
        journal.push_back("windowShouldClose:");
        s.x[0] = allow_close ? 1 : 0;
    }, "B@:@");
    f.add(delegate_class, "windowWillResize:toSize:", [](CpuState& s, GuestMemory&) {
        // Frame sizes: keep the width at least 640.
        const auto width = std::max(640.0, gui::appkit::d(s, 0));
        gui::appkit::set_d(s, 0, width);
    }, "{CGSize=dd}@:@{CGSize=dd}");

    // ---- window creation and geometry ----
    CallRegs init;
    gui::appkit::put_rect(init, 0, {{100, 100}, {800, 600}});
    init.x[2] = gui::style::titled | gui::style::closable | gui::style::resizable | gui::style::miniaturizable;
    init.x[3] = 2;   // NSBackingStoreBuffered
    init.x[4] = 0;
    const auto window = f.send(f.id(f.cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    require(window != 0 && f.server.windows.size() == 1, "NSWindow did not create a server window");
    const auto number = f.id(window, "windowNumber");
    auto& server_window = f.server.at(number);
    require(server_window.desc.owner == window && same(server_window.content, {{100, 100}, {800, 600}}), "server window description");
    static_cast<void>(f.id(window, "setReleasedWhenClosed:", {0}));
    static_cast<void>(f.id(window, "setTitle:", {f.str("Alchemy")}));
    require(server_window.title == "Alchemy" && f.text(f.id(window, "title")) == "Alchemy", "-setTitle:");
    const auto frame = f.rect_of(f.send(window, "frame"));
    require(same(frame, {{100, 100}, {800, 628}}), "-frame is " + describe(frame));
    CallRegs frame_regs;
    gui::appkit::put_rect(frame_regs, 0, frame);
    require(same(f.rect_of(f.send(window, "contentRectForFrameRect:", frame_regs)), {{100, 100}, {800, 600}}), "-contentRectForFrameRect:");
    CallRegs backing;
    gui::appkit::put_rect(backing, 0, {{1, 2}, {10, 20}});
    require(same(f.rect_of(f.send(window, "convertRectToBacking:", backing)), {{2, 4}, {20, 40}}), "-convertRectToBacking:");
    require(near(f.send(window, "backingScaleFactor").d(0), 2), "-backingScaleFactor");
    const auto default_view = f.id(window, "contentView");
    require(default_view && same(f.rect_of(f.send(default_view, "frame")), {{0, 0}, {800, 600}}), "default content view frame");

    const auto view = f.with_rect(f.id(view_class, "alloc"), "initWithFrame:", {{0, 0}, {10, 10}});
    static_cast<void>(f.id(window, "setContentView:", {view}));
    require(f.id(window, "contentView") == view && f.id(view, "window") == window && f.id(view, "nextResponder") == window,
            "-setContentView: hierarchy");
    require(same(f.rect_of(f.send(view, "frame")), {{0, 0}, {800, 600}}), "content view was not sized to the window");
    CallRegs view_backing;
    gui::appkit::put_rect(view_backing, 0, {{0, 0}, {800, 600}});
    require(same(f.rect_of(f.send(view, "convertRectToBacking:", view_backing)), {{0, 0}, {1600, 1200}}), "-[NSView convertRectToBacking:]");
    const auto delegate = f.id(f.id(delegate_class, "alloc"), "init");
    static_cast<void>(f.id(window, "setDelegate:", {delegate}));
    static_cast<void>(f.id(window, "setAcceptsMouseMovedEvents:", {1}));
    require(server_window.accepts_mouse_moved, "-setAcceptsMouseMovedEvents:");

    // ---- key window ----
    static_cast<void>(f.id(window, "setInitialFirstResponder:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    require(f.server.key == number && server_window.visible, "-makeKeyAndOrderFront: did not show and focus the window");
    require(std::count(journal.begin(), journal.end(), "windowDidBecomeKey:") == 1, "windowDidBecomeKey: count");
    require((f.id(window, "isKeyWindow") & 0xff) && f.id(f.app, "keyWindow") == window, "key window state");
    require(f.id(window, "firstResponder") == view, "initial first responder did not take focus");
    require(server_window.text_input, "text input is not enabled for an NSTextInputClient first responder");

    // ---- key event: conversion and delivery ----
    gui::Event key;
    key.type = gui::event_type::key_down;
    key.window = number;
    key.key_code = 0;
    key.characters = "A";
    key.characters_ignoring_modifiers = "a";
    key.modifier_flags = gui::modifier::shift;
    key.location = {5, 6};
    key.timestamp = 12.5;
    f.server.queue(key);
    const auto key_event = f.next_event();
    require(key_event && f.id(key_event, "type") == gui::event_type::key_down, "key event type");
    require(f.id(key_event, "modifierFlags") == gui::modifier::shift && f.id(key_event, "keyCode") == 0 &&
                f.text(f.id(key_event, "charactersIgnoringModifiers")) == "a" && f.id(key_event, "window") == window &&
                f.id(key_event, "windowNumber") == number, "key event accessors");
    const auto location = f.send(key_event, "locationInWindow");
    require(near(location.d(0), 5) && near(location.d(1), 6) && near(f.send(key_event, "timestamp").d(0), 12.5), "NSPoint/double returns");
    static_cast<void>(f.id(f.app, "sendEvent:", {key_event}));
    require(log_has("keyDown:0:A"), "key event did not reach the first responder");

    // ---- mouse, scroll, responder chain ----
    const auto child = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{10, 10}, {100, 100}});
    static_cast<void>(f.id(view, "addSubview:", {child}));
    require(f.id(child, "superview") == view && f.id(child, "window") == window && f.id(child, "nextResponder") == view, "subview hierarchy");
    CallRegs point;
    gui::appkit::put_point(point, 0, {5, 7});
    point.x[2] = child;
    const auto converted = f.send(view, "convertPoint:fromView:", point);
    require(near(converted.d(0), 15) && near(converted.d(1), 17), "-convertPoint:fromView:");
    gui::Event down;
    down.type = gui::event_type::left_mouse_down;
    down.window = number;
    down.location = {300, 200};
    down.click_count = 1;
    down.pressed_buttons = 1;
    f.server.queue(down);
    gui::Event up = down;
    up.type = gui::event_type::left_mouse_up;
    up.pressed_buttons = 0;
    f.server.queue(up);
    gui::Event inside = down;   // over the plain child: forwarded up the chain
    inside.location = {20, 30};
    f.server.queue(inside);
    gui::Event wheel;
    wheel.type = gui::event_type::scroll_wheel;
    wheel.window = number;
    wheel.location = {400, 300};
    wheel.delta_y = 3;
    wheel.precise_scrolling = true;
    f.server.queue(wheel);
    f.pump();
    require(log_has("mouseDown:300,200"), "mouse down did not reach the hit view");
    require(log_has("mouseDown:20,30"), "mouse down on a plain subview was not forwarded to its superview");
    require(log_has("scrollWheel:3:precise"), "scroll event did not reach the view under the pointer");
    // The plain child refuses first responder status; the clicks kept focus.
    require(f.id(window, "firstResponder") == view, "click focus changed to a view that refuses first responder");
    require((f.id(window, "makeFirstResponder:", {child}) & 0xff) == 1 && f.id(window, "firstResponder") == child, "-makeFirstResponder:");
    key.characters = "s"; key.key_code = 1; key.modifier_flags = 0;
    f.server.queue(key);
    f.pump();
    require(log_has("keyDown:1:s"), "key event was not forwarded from the first responder to its next responder");
    static_cast<void>(f.id(window, "makeFirstResponder:", {view}));

    // ---- IME text, key bindings, caret rectangle ----
    gui::Event marked;
    marked.kind = gui::EventKind::text;
    marked.window = number;
    marked.text = "\xe6\x97\xa5";
    marked.marked = true;
    marked.selection_location = 1;
    f.server.queue(marked);
    gui::Event commit = marked;
    commit.marked = false;
    commit.text = "\xe6\x97\xa5\xe6\x9c\xac";
    f.server.queue(commit);
    f.pump();
    // Input-method text arrives inside key presses (kVK_ISO_Section): the
    // composition without characters, the result one character per press.
    require(log_has("keyDown:10:") && log_has("setMarkedText:\xe6\x97\xa5:1,0"), "IME composition delivery");
    require(log_has("keyDown:10:\xe6\x97\xa5") && log_has("keyDown:10:\xe6\x9c\xac") && log_has("insertText:\xe6\x97\xa5:notfound") &&
                log_has("insertText:\xe6\x9c\xac:notfound"), "IME result delivery");
    gui::Event enter = key;
    enter.key_code = 0x24; enter.characters = "\r"; enter.characters_ignoring_modifiers = "\r";
    f.server.queue(enter);
    gui::Event letter = key;
    letter.key_code = 0x0b; letter.characters = "b"; letter.characters_ignoring_modifiers = "b";
    f.server.queue(letter);
    const auto enter_event = f.next_event(), letter_event = f.next_event();
    const auto events = f.cf.array(std::array<Address, 2>{enter_event, letter_event});
    static_cast<void>(f.id(view, "interpretKeyEvents:", {events}));
    require(log_has("doCommandBySelector:insertNewline:") && log_has("insertText:b:notfound"), "-interpretKeyEvents: key bindings");
    const auto caret = server_window.sink->text_caret_rect(number);
    require(caret && same(*caret, {{50, 60}, {2, 20}}) && log_has("firstRect:3"), "IME caret rectangle (NSRect return)");
    const auto context = f.id(view, "inputContext");
    require(context && f.id(context, "client") == view, "-[NSView inputContext]");
    require(f.id(f.cls("NSTextInputContext"), "currentInputContext") == context, "+[NSTextInputContext currentInputContext]");
    static_cast<void>(f.id(context, "discardMarkedText"));
    require(f.server.discards == 1, "-discardMarkedText");

    // ---- window notifications ----
    const auto resized = server_window.sink->will_resize(number, {500, 400});
    require(near(resized.width, 640) && near(resized.height, 400), "windowWillResize:toSize: (frame/content conversion)");
    f.server.set_content_rect(number, {{100, 100}, {1000, 700}});
    require(log_has("windowDidResize:") && same(f.rect_of(f.send(view, "frame")), {{0, 0}, {1000, 700}}), "resize notification");
    CallRegs origin;
    gui::appkit::put_point(origin, 0, {200, 150});
    static_cast<void>(f.send(window, "setFrameOrigin:", origin));
    require(log_has("windowDidMove:") && same(server_window.content, {{200, 150}, {1000, 700}}), "-setFrameOrigin:");
    f.server.invalidate(number, std::nullopt);
    f.pump();
    require(log_has("drawRect:" + describe({{0, 0}, {1000, 700}})), "expose did not draw the content view");
    static_cast<void>(f.id(window, "toggleFullScreen:", {0}));
    require(log_has("windowWillEnterFullScreen:") && log_has("windowDidEnterFullScreen:") &&
                (f.id(window, "styleMask") & gui::style::full_screen), "full screen notifications");
    static_cast<void>(f.id(window, "miniaturize:", {0}));
    require(log_has("windowDidMiniaturize:") && (f.id(window, "isMiniaturized") & 0xff), "miniaturize notification");

    // ---- closing ----
    server_window.sink->handle_event([&] {
        gui::Event close;
        close.kind = gui::EventKind::notification;
        close.notification = gui::Notification::close_requested;
        close.window = number;
        return close;
    }());
    require(log_has("windowShouldClose:") && !log_has("windowWillClose:") && f.server.windows.contains(number),
            "windowShouldClose: NO did not keep the window");
    allow_close = true;
    server_window.sink->handle_event([&] {
        gui::Event close;
        close.kind = gui::EventKind::notification;
        close.notification = gui::Notification::close_requested;
        close.window = number;
        return close;
    }());
    require(log_has("windowWillClose:") && !f.server.windows.contains(number), "close did not destroy the server window");
    require(std::find(f.server.destroyed.begin(), f.server.destroyed.end(), number) != f.server.destroyed.end(), "destroyed list");
    require(f.id(f.app, "keyWindow") == 0, "closed window is still key");
    static_cast<void>(f.id(window, "release"));
}

void resources(Fixture& f) {
    // Pasteboard.
    const auto board = f.id(f.cls("NSPasteboard"), "generalPasteboard");
    static_cast<void>(f.id(board, "clearContents"));
    const auto type = f.str("public.utf8-plain-text");
    require((f.id(board, "setString:forType:", {f.str("clip"), type}) & 0xff) == 1 && f.server.clipboard == "clip", "-setString:forType:");
    f.server.clipboard = "from host";
    require(f.text(f.id(board, "stringForType:", {type})) == "from host", "-stringForType:");
    const auto types = f.id(board, "types");
    require(f.cf.length(types) >= 1 && f.text(f.cf.element(types, 0)) == "public.utf8-plain-text", "-types");

    // Cursors.
    static_cast<void>(f.id(f.id(f.cls("NSCursor"), "IBeamCursor"), "set"));
    require(!f.server.cursors_set.empty() && f.server.cursors_set.back() == 100 + static_cast<int>(gui::SystemCursor::ibeam), "+IBeamCursor set");
    CallRegs bitmap;
    bitmap.x[2] = 0; bitmap.x[3] = 4; bitmap.x[4] = 2; bitmap.x[5] = 8; bitmap.x[6] = 4; bitmap.x[7] = 1;
    bitmap.push_stack(std::uint8_t{0});                  // isPlanar
    bitmap.push_stack(f.str("NSDeviceRGBColorSpace"));   // colorSpaceName
    bitmap.push_stack(std::int64_t{16});                 // bytesPerRow
    bitmap.push_stack(std::int64_t{32});                 // bitsPerPixel
    const auto rep = f.send(f.id(f.cls("NSBitmapImageRep"), "alloc"),
        "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
        bitmap).x0;
    require(rep && f.id(rep, "pixelsWide") == 4 && f.id(rep, "bytesPerRow") == 16 && f.id(rep, "bitmapData") != 0, "NSBitmapImageRep stack arguments");
    CallRegs size;
    gui::appkit::put_size(size, 0, {4, 2});
    const auto picture = f.send(f.id(f.cls("NSImage"), "alloc"), "initWithSize:", size).x0;
    static_cast<void>(f.id(picture, "addRepresentation:", {rep}));
    // NSCopying: the copy's representation is a copy with its own pixels.
    f.engine.write32(f.id(rep, "bitmapData"), 0x11223344);
    const auto picture_copy = f.id(picture, "copy");
    const auto reps_copy = f.id(picture_copy, "representations");
    const auto rep_copy = f.cf.element(reps_copy, 0);
    require(picture_copy != picture && f.cf.length(reps_copy) == 1 && rep_copy != rep && f.id(rep_copy, "pixelsWide") == 4 &&
                f.id(rep_copy, "bitmapData") != f.id(rep, "bitmapData") && f.engine.read32(f.id(rep_copy, "bitmapData")) == 0x11223344,
            "NSImage copy");
    static_cast<void>(f.id(picture_copy, "release"));
    CallRegs hot;
    gui::appkit::put_point(hot, 0, {1, 1});
    hot.x[2] = picture;
    const auto custom = f.send(f.id(f.cls("NSCursor"), "alloc"), "initWithImage:hotSpot:", hot).x0;
    static_cast<void>(f.id(custom, "set"));
    require(f.server.cursors_created.size() == 1 && f.server.cursors_created[0].first.width == 4 &&
                near(f.server.cursors_created[0].second.x, 1), "custom cursor creation");
    static_cast<void>(f.id(f.cls("NSCursor"), "hide"));
    require(f.server.cursor_hide_count == 1, "+[NSCursor hide]");
    static_cast<void>(f.id(f.cls("NSCursor"), "unhide"));

    // Drawing into an image (the dock-icon progress path).
    CallRegs canvas_size;
    gui::appkit::put_size(canvas_size, 0, {16, 16});
    const auto canvas = f.send(f.id(f.cls("NSImage"), "alloc"), "initWithSize:", canvas_size).x0;
    static_cast<void>(f.id(canvas, "lockFocus"));
    static_cast<void>(f.id(f.id(f.cls("NSColor"), "whiteColor"), "setFill"));
    CallRegs path;
    gui::appkit::put_rect(path, 0, {{0, 0}, {8, 16}});
    static_cast<void>(f.id(f.send(f.cls("NSBezierPath"), "bezierPathWithRect:", path).x0, "fill"));
    static_cast<void>(f.id(canvas, "unlockFocus"));
    const auto reps = f.id(canvas, "representations");
    require(f.cf.length(reps) == 1, "-lockFocus did not create a bitmap representation");
    const auto pixels = f.id(f.cf.element(reps, 0), "bitmapData");
    std::array<std::uint8_t, 4> left{}, right{};
    f.engine.read_bytes(pixels, left);
    f.engine.read_bytes(pixels + 15 * 4, right);
    require(left == std::array<std::uint8_t, 4>{255, 255, 255, 255} && right[3] == 0, "-[NSBezierPath fill] pixels");

    // Image files: PNG round trip through the codec, NSImage, NSBitmapImageRep
    // and the pasteboard.
    gui::Image square;
    square.width = 2; square.height = 2;
    square.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 128, 10, 20, 30, 0};
    const auto png = gui::appkit::encode_image(square, gui::appkit::ImageFileType::png);
    require(png && png->size() > 8 && (*png)[1] == 'P', "PNG encoding");
    const auto decoded = gui::appkit::decode_image(*png);
    require(decoded && decoded->pixels.width == 2 && decoded->pixels.rgba == square.rgba, "PNG decoding");
    const auto png_data = f.objc.send(f.cls("NSData"), f.objc.selector("dataWithBytes:length:"), [&] {
        const auto buffer = f.process.allocate_guest(png->size());
        f.engine.write_bytes(buffer, *png);
        CallRegs r; r.x[2] = buffer; r.x[3] = png->size();
        return r;
    }()).x0;
    const auto loaded = f.id(f.id(f.cls("NSImage"), "alloc"), "initWithData:", {png_data});
    require(loaded && near(f.send(loaded, "size").d(0), 2), "-[NSImage initWithData:]");
    const auto loaded_rep = f.cf.element(f.id(loaded, "representations"), 0);
    std::array<std::uint8_t, 4> first{};
    f.engine.read_bytes(f.id(loaded_rep, "bitmapData"), first);
    require(first == std::array<std::uint8_t, 4>{255, 0, 0, 255}, "decoded NSBitmapImageRep pixels");
    require(f.id(loaded_rep, "representationUsingType:properties:", {4, 0}) != 0, "-representationUsingType:properties:");
    const auto truncated = f.id(f.cls("NSData"), "dataWithBytes:length:", {f.id(png_data, "bytes"), 6});
    require(f.id(f.id(f.cls("NSImage"), "alloc"), "initWithData:", {truncated}) == 0, "undecodable data gives a nil image");
    f.server.clipboard_picture = square;
    const auto clip = f.id(board, "dataForType:", {f.str("public.png")});
    require(clip && f.id(clip, "length") > 8, "-dataForType: public.png");
    require((f.id(f.cls("NSImage"), "canInitWithPasteboard:", {board}) & 0xff) == 1, "+canInitWithPasteboard:");
    const auto pasted = f.id(f.id(f.cls("NSImage"), "alloc"), "initWithPasteboard:", {board});
    require(pasted && f.cf.length(f.id(pasted, "representations")) == 1, "-initWithPasteboard:");
    f.server.clipboard_picture.reset();
    static_cast<void>(f.id(board, "clearContents"));
    require((f.id(board, "writeObjects:", {f.cf.array(std::array<Address, 1>{loaded})}) & 0xff) == 1 &&
                f.server.clipboard_picture && f.server.clipboard_picture->rgba == square.rgba, "-writeObjects: image");

    // Screens.
    const auto main = f.id(f.cls("NSScreen"), "mainScreen");
    require(main && same(f.rect_of(f.send(main, "frame")), {{0, 0}, {1440, 900}}) &&
                same(f.rect_of(f.send(main, "visibleFrame")), {{0, 40}, {1440, 860}}) &&
                near(f.send(main, "backingScaleFactor").d(0), 2) && f.id(main, "maximumFramesPerSecond") == 120, "NSScreen geometry");
    const auto description = f.id(main, "deviceDescription");
    const auto number = f.id(description, "objectForKey:", {f.str("NSScreenNumber")});
    require(number && f.id(number, "unsignedIntValue") == 1, "NSScreenNumber");
    require(f.cf.length(f.id(f.cls("NSScreen"), "screens")) == 1, "+screens");

    // Appearance, menus.
    const auto dark = f.id(f.cls("NSAppearance"), "appearanceNamed:", {f.str("NSAppearanceNameDarkAqua")});
    const auto names = f.cf.array(std::array<Address, 2>{f.str("NSAppearanceNameAqua"), f.str("NSAppearanceNameDarkAqua")});
    require(dark && f.text(f.id(dark, "bestMatchFromAppearancesWithNames:", {names})) == "NSAppearanceNameDarkAqua", "NSAppearance");
    const auto bar = f.id(f.id(f.cls("NSMenu"), "alloc"), "init");
    const auto app_menu = f.id(f.id(f.cls("NSMenu"), "alloc"), "initWithTitle:", {f.str("Blender")});
    const auto quit = f.id(app_menu, "addItemWithTitle:action:keyEquivalent:", {f.str("Quit"), f.objc.selector("terminate:"), f.str("q")});
    require(quit && f.id(app_menu, "numberOfItems") == 1 && f.text(f.id(quit, "title")) == "Quit", "NSMenu items");
    const auto holder = f.id(f.id(f.cls("NSMenuItem"), "alloc"), "init");
    static_cast<void>(f.id(holder, "setSubmenu:", {app_menu}));
    static_cast<void>(f.id(bar, "addItem:", {holder}));
    static_cast<void>(f.id(f.app, "setMainMenu:", {bar}));
    require(f.id(f.app, "mainMenu") == bar && f.id(holder, "submenu") == app_menu, "-setMainMenu:");
    // NSResponder's -menu of the application is the main menu (Qt's Cocoa
    // plugin installs its menu with -[NSApp setMenu:]).
    const auto qt_bar = f.id(f.id(f.cls("NSMenu"), "alloc"), "init");
    static_cast<void>(f.id(f.app, "setMenu:", {qt_bar}));
    require(f.id(f.app, "menu") == qt_bar && f.id(f.app, "mainMenu") == qt_bar, "-[NSApp setMenu:] sets the main menu");
    static_cast<void>(f.id(f.app, "setMainMenu:", {bar}));

    // QCocoaNSMenu/QCocoaMenuItem properties: stored and returned, lookups by tag, title and submenu.
    static_cast<void>(f.id(app_menu, "setDelegate:", {holder}));
    require(f.id(app_menu, "delegate") == holder, "NSMenu delegate");
    CallRegs font_size;
    font_size.set_d(0, 13);
    const auto font = f.send(f.cls("NSFont"), "systemFontOfSize:", font_size).x0;
    static_cast<void>(f.id(app_menu, "setFont:", {font}));
    require(f.id(app_menu, "font") == font, "NSMenu font");
    CallRegs width;
    width.set_d(0, 180);
    static_cast<void>(f.send(app_menu, "setMinimumWidth:", width));
    require(near(f.send(app_menu, "minimumWidth").d(0), 180), "NSMenu minimumWidth");
    require((f.id(app_menu, "showsStateColumn") & 0xff) == 1, "NSMenu showsStateColumn default");
    static_cast<void>(f.id(app_menu, "setShowsStateColumn:", {0}));
    require((f.id(app_menu, "showsStateColumn") & 0xff) == 0, "NSMenu setShowsStateColumn:");
    require(f.id(app_menu, "highlightedItem") == 0, "no menu is tracked");
    static_cast<void>(f.id(app_menu, "cancelTracking"));
    static_cast<void>(f.id(quit, "setTag:", {77}));
    const auto about = f.id(app_menu, "addItemWithTitle:action:keyEquivalent:", {f.str("About"), 0, f.str("")});
    require(f.id(app_menu, "indexOfItemWithTag:", {77}) == 0 && f.id(app_menu, "itemWithTag:", {77}) == quit &&
                f.id(app_menu, "itemWithTag:", {5}) == 0 && static_cast<std::int64_t>(f.id(app_menu, "indexOfItemWithTag:", {5})) == -1,
            "NSMenu tag lookup");
    require(f.id(app_menu, "indexOfItemWithTitle:", {f.str("About")}) == 1 &&
                static_cast<std::int64_t>(f.id(app_menu, "indexOfItemWithTitle:", {f.str("Edit")})) == -1, "NSMenu title lookup");
    require(f.id(bar, "indexOfItemWithSubmenu:", {app_menu}) == 0 &&
                static_cast<std::int64_t>(f.id(bar, "indexOfItemWithSubmenu:", {0})) == -1, "NSMenu submenu lookup");
    static_cast<void>(f.id(about, "setToolTip:", {f.str("Shows the version")}));
    require(f.text(f.id(about, "toolTip")) == "Shows the version", "NSMenuItem toolTip");
    static_cast<void>(f.id(about, "setAlternate:", {1}));
    require((f.id(about, "isAlternate") & 0xff) == 1, "NSMenuItem alternate");
    static_cast<void>(f.id(about, "setIndentationLevel:", {40}));
    require(f.id(about, "indentationLevel") == 15, "NSMenuItem indentation is clamped to 15");
    require(f.id(about, "view") == 0 && f.id(about, "image") == 0 && f.id(about, "attributedTitle") == 0, "NSMenuItem unset properties");

    // NSCopying: deep copies of the menu bar (items and submenus), sharing
    // targets, represented objects and the weak delegate.
    static_cast<void>(f.id(quit, "setTarget:", {holder}));
    static_cast<void>(f.id(quit, "setRepresentedObject:", {font}));
    static_cast<void>(f.id(quit, "setState:", {1}));
    static_cast<void>(f.id(quit, "setKeyEquivalentModifierMask:", {(1U << 20) | (1U << 17)}));
    const auto quit_copy = f.id(quit, "copy");
    require(quit_copy && quit_copy != quit && f.id(quit_copy, "menu") == 0, "NSMenuItem copy is in no menu");
    require(f.text(f.id(quit_copy, "title")) == "Quit" && f.text(f.id(quit_copy, "keyEquivalent")) == "q" &&
                f.id(quit_copy, "action") == f.objc.selector("terminate:") && f.id(quit_copy, "target") == holder &&
                f.id(quit_copy, "tag") == 77 && f.id(quit_copy, "state") == 1 && f.id(quit_copy, "representedObject") == font &&
                f.id(quit_copy, "keyEquivalentModifierMask") == ((1U << 20) | (1U << 17)),
            "NSMenuItem copy values");
    static_cast<void>(f.id(quit_copy, "setTitle:", {f.str("Exit")}));
    require(f.text(f.id(quit, "title")) == "Quit", "the copy is independent");
    const auto bar_copy = f.id(bar, "copy");
    require(bar_copy && bar_copy != bar && f.id(bar_copy, "numberOfItems") == 1 && f.id(bar_copy, "supermenu") == 0, "NSMenu copy");
    const auto holder_copy = f.id(bar_copy, "itemAtIndex:", {0});
    const auto menu_copy = f.id(holder_copy, "submenu");
    require(holder_copy != holder && f.id(holder_copy, "menu") == bar_copy && menu_copy && menu_copy != app_menu &&
                f.id(menu_copy, "supermenu") == bar_copy, "the copy's submenu is a copy");
    require(f.text(f.id(menu_copy, "title")) == "Blender" && f.id(menu_copy, "numberOfItems") == 2 && f.id(menu_copy, "delegate") == holder &&
                f.id(menu_copy, "font") == font && (f.id(menu_copy, "showsStateColumn") & 0xff) == 0 &&
                f.id(menu_copy, "itemAtIndex:", {0}) != quit && f.id(f.id(menu_copy, "itemAtIndex:", {0}), "tag") == 77,
            "NSMenu copy values");
    static_cast<void>(f.id(quit_copy, "release"));
    static_cast<void>(f.id(bar_copy, "release"));
}

void carbon(Fixture& f) {
    const auto call = [&](std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        const auto found = f.process.resolve(library, name);
        require(found.has_value(), "missing function " + std::string(name));
        return f.engine.invoke_callback(found->address, std::span(arguments.begin(), arguments.size()));
    };
    const auto source = call(gui::appkit::carbon_library, "_TISCopyCurrentKeyboardLayoutInputSource");
    require(source != 0, "TISCopyCurrentKeyboardLayoutInputSource");
    const auto key = f.engine.read64(f.process.resolve(gui::appkit::carbon_library, "_kTISPropertyUnicodeKeyLayoutData")->address);
    const auto data = call(gui::appkit::carbon_library, "_TISGetInputSourceProperty", {source, key});
    require(data != 0, "kTISPropertyUnicodeKeyLayoutData");
    const auto ascii_key = f.engine.read64(f.process.resolve(gui::appkit::carbon_library, "_kTISPropertyInputSourceIsASCIICapable")->address);
    require(call(gui::appkit::carbon_library, "_TISGetInputSourceProperty", {source, ascii_key}) == f.cf.boolean(true), "ASCII capable");
    const auto bytes = f.id(data, "bytes");
    const auto buffer = f.process.allocate_guest(64);
    const auto dead = buffer, actual = buffer + 8, output = buffer + 16;
    f.engine.write64(dead, 0);
    CallRegs translate;
    translate.x = {bytes, 0x00, 0, 2 /* shiftKey >> 8 */, 40, 1 /* no dead keys */, dead, 4};
    translate.push_stack(actual);
    translate.push_stack(output);
    const auto found = f.process.resolve(gui::appkit::core_services_library, "_UCKeyTranslate");
    const auto status = alchemy::engine_call_regs(f.engine, found->address, translate).x0;
    require(static_cast<std::uint32_t>(status) == 0 && f.engine.read64(actual) == 1 && f.engine.read32(output) == 'A',
            "UCKeyTranslate Shift+A");
    // Option+E then A: the dead key completes to U+00E1.
    translate.x[1] = 0x0e; translate.x[3] = 8 /* optionKey >> 8 */; translate.x[5] = 0;
    require(alchemy::engine_call_regs(f.engine, found->address, translate).x0 == 0 && f.engine.read64(actual) == 0 &&
                f.engine.read32(dead) == 1, "UCKeyTranslate dead key");
    translate.x[1] = 0x00; translate.x[3] = 0;
    require(alchemy::engine_call_regs(f.engine, found->address, translate).x0 == 0 && f.engine.read64(actual) == 1 &&
                (f.engine.read32(output) & 0xffff) == 0xe1, "UCKeyTranslate dead key completion");
    f.process.release_guest(buffer);
    f.process.objects().release(source);

    const auto list = f.process.allocate_guest(16);
    require(call(gui::appkit::core_graphics_library, "_CGGetActiveDisplayList", {4, list, list + 8}) == 0 &&
                f.engine.read32(list + 8) == 1 && f.engine.read32(list) == 1, "CGGetActiveDisplayList");
    require(call(gui::appkit::core_graphics_library, "_CGMainDisplayID") == 1, "CGMainDisplayID");
    CallRegs display;
    display.x[0] = 1;
    const auto bounds = alchemy::engine_call_regs(f.engine, f.process.resolve(gui::appkit::core_graphics_library, "_CGDisplayBounds")->address, display);
    require(same(f.rect_of(bounds), {{0, 0}, {1440, 900}}), "CGDisplayBounds (CGRect return)");
    CallRegs move;
    move.x[0] = 1;
    gui::appkit::put_point(move, 0, {10, 20});
    require(alchemy::engine_call_regs(f.engine, f.process.resolve(gui::appkit::core_graphics_library, "_CGDisplayMoveCursorToPoint")->address, move).x0 == 0 &&
                !f.server.warps.empty() && near(f.server.warps.back().x, 10) && near(f.server.warps.back().y, 880), "CGDisplayMoveCursorToPoint");
    require(call(gui::appkit::core_graphics_library, "_CGAssociateMouseAndMouseCursorPosition", {0}) == 0 && f.server.associated == false,
            "CGAssociateMouseAndMouseCursorPosition");
    f.server.buttons = 5;
    require(call(gui::appkit::carbon_library, "_GetCurrentEventButtonState") == 5, "GetCurrentEventButtonState");
    f.process.release_guest(list);
}

Address make_window(Fixture& f, gui::Rect content) {
    CallRegs init;
    gui::appkit::put_rect(init, 0, content);
    init.x[2] = gui::style::titled | gui::style::closable | gui::style::resizable;
    init.x[3] = 2;
    const auto window = f.send(f.id(f.cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    require(window != 0, "NSWindow creation");
    static_cast<void>(f.id(window, "setReleasedWhenClosed:", {0}));
    return window;
}

std::string describe(const std::optional<gui::quartzcore::LayerHost>& host) {
    if (!host) return "(no host)";
    return "{window " + std::to_string(host->window) + " " + describe(host->frame) + " scale " + std::to_string(host->backing_scale) + "}";
}
bool hosted(const std::optional<gui::quartzcore::LayerHost>& host, gui::WindowId window, gui::Rect frame, double scale) {
    return host && host->window == window && same(host->frame, frame) && near(host->backing_scale, scale);
}

// Application-wide window-server events reach NSApplication through the
// application sink registered by -finishLaunching.
void application_events(Fixture& f) {
    journal.clear();
    require(f.server.application_sink != nullptr, "-finishLaunching did not register the application sink");
    const auto delegate_class = f.cls("TestAppDelegate");
    for (const auto* name : {"applicationDidResignActive:", "applicationDidChangeScreenParameters:"}) {
        const std::string entry = name;
        f.add(delegate_class, entry, [entry](CpuState&, GuestMemory&) { journal.push_back(entry); });
    }
    // -setDelegate: registers the methods a delegate has when it is set.
    const auto delegate = f.id(f.app, "delegate");
    static_cast<void>(f.id(f.app, "setDelegate:", {0}));
    static_cast<void>(f.id(f.app, "setDelegate:", {delegate}));
    require((f.id(f.app, "isActive") & 0xff) == 1, "application should be active here");
    f.server.notify_application(gui::Notification::application_deactivated);
    require((f.id(f.app, "isActive") & 0xff) == 0 && log_has("applicationDidResignActive:"), "application_deactivated");
    gui::Event screens;
    screens.kind = gui::EventKind::notification;
    screens.notification = gui::Notification::screens_changed;
    f.server.queue(screens);   // queued: delivered by the event pump
    f.pump();
    require(log_has("applicationDidChangeScreenParameters:"), "screens_changed through the pump");
    f.server.notify_application(gui::Notification::application_activated);
    require((f.id(f.app, "isActive") & 0xff) == 1, "application_activated");
}

// NSAlert -runModal: request contents, AppKit button order and return codes,
// the spacer accessory view, key-window hand-off, auto-dismissal.
void alerts(Fixture& f) {
    journal.clear();
    const auto window_class = f.subclass("TestKeyWindow", "NSWindow");
    for (const auto* name : {"becomeKeyWindow", "resignKeyWindow"}) {
        const std::string entry = name;
        f.add(window_class, entry, [entry](CpuState&, GuestMemory&) { journal.push_back(entry); }, "v@:");
    }
    CallRegs init;
    gui::appkit::put_rect(init, 0, {{100, 100}, {640, 480}});
    init.x[2] = gui::style::titled | gui::style::closable;
    init.x[3] = 2;
    const auto window = f.send(f.id(window_class, "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    static_cast<void>(f.id(window, "setReleasedWhenClosed:", {0}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    const auto number = f.id(window, "windowNumber");
    require(f.id(f.app, "keyWindow") == window, "alert test window is not key");
    journal.clear();

    std::vector<gui::appkit::AlertRequest> requests;
    bool key_during_modal = true;
    std::size_t answer = 1;
    gui::appkit::set_alert_presenter_override([&](const gui::appkit::AlertRequest& request) {
        requests.push_back(request);
        key_during_modal = (f.id(window, "isKeyWindow") & 0xff) != 0 || f.id(f.app, "keyWindow") == window;
        // Win32 deactivates the owner while the dialog is up; the window
        // server reports it in its own time.
        f.server.key = 0;
        f.server.notify(number, gui::Notification::resigned_key);
        return answer;
    });
    const auto make_alert = [&](bool spacer) {
        const auto value = f.id(f.id(f.cls("NSAlert"), "alloc"), "init");
        if (spacer) {
            const auto accessory = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {500, 0}});
            static_cast<void>(f.id(value, "setAccessoryView:", {accessory}));
            static_cast<void>(f.id(accessory, "release"));
        }
        static_cast<void>(f.id(value, "setAlertStyle:", {0}));
        static_cast<void>(f.id(value, "setMessageText:", {f.str("Blender - Limited Platform Support")}));
        static_cast<void>(f.id(value, "setInformativeText:", {f.str("Your graphics card or driver has limited support.")}));
        static_cast<void>(f.id(value, "addButtonWithTitle:", {f.str("Continue Anyway")}));
        static_cast<void>(f.id(value, "addButtonWithTitle:", {f.str("Find Latest Drivers")}));
        return value;
    };

    const auto alert = make_alert(true);
    require(f.id(alert, "runModal") == 1001, "-runModal did not answer NSAlertSecondButtonReturn");
    require(requests.size() == 1, "presenter calls");
    const auto& request = requests.front();
    require(request.message == "Blender - Limited Platform Support" &&
                request.informative == "Your graphics card or driver has limited support." && request.style == 0 &&
                request.buttons == std::vector<std::string>{"Continue Anyway", "Find Latest Drivers"} && near(request.width, 500),
            "alert request contents");
    require(!key_during_modal, "the key window stayed key during the modal alert");
    require(f.id(f.app, "keyWindow") == window && (f.id(window, "isKeyWindow") & 0xff) == 1 && f.server.key == number,
            "key window was not restored after the alert");
    require(std::count(journal.begin(), journal.end(), "resignKeyWindow") == 1 &&
                std::count(journal.begin(), journal.end(), "becomeKeyWindow") == 1 && journal.front() == "resignKeyWindow",
            "key hand-off: one -resignKeyWindow then one -becomeKeyWindow");
    answer = 0;
    require(f.id(alert, "runModal") == 1000, "-runModal NSAlertFirstButtonReturn");
    static_cast<void>(f.id(alert, "release"));

    // No buttons: AppKit's "OK".
    const auto plain = f.id(f.id(f.cls("NSAlert"), "alloc"), "init");
    static_cast<void>(f.id(plain, "setMessageText:", {f.str("Plain")}));
    require(f.id(plain, "runModal") == 1000 && requests.back().buttons == std::vector<std::string>{"OK"}, "default OK button");
    // Accessory views with content are not provided.
    const auto box = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {100, 20}});
    const auto label = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {100, 20}});
    static_cast<void>(f.id(box, "addSubview:", {label}));
    static_cast<void>(f.id(plain, "setAccessoryView:", {box}));
    bool refused{};
    try { static_cast<void>(f.id(plain, "runModal")); } catch (const std::runtime_error&) { refused = true; }
    require(refused, "an accessory view with content was accepted");
    static_cast<void>(f.id(plain, "release"));

    // ALCHEMY_GUI_AUTODISMISS_ALERTS: no presenter, the chosen button.
    const auto calls = requests.size();
    const auto set_env = [](const char* value) {
#ifdef _WIN32
        _putenv_s("ALCHEMY_GUI_AUTODISMISS_ALERTS", value);
#else
        if (*value) setenv("ALCHEMY_GUI_AUTODISMISS_ALERTS", value, 1); else unsetenv("ALCHEMY_GUI_AUTODISMISS_ALERTS");
#endif
    };
    const auto unattended = make_alert(true);
    set_env("1");
    const auto dismissed = f.id(unattended, "runModal");
    set_env("7");
    bool out_of_range{};
    try { static_cast<void>(f.id(unattended, "runModal")); } catch (const std::runtime_error&) { out_of_range = true; }
    set_env("first");
    bool malformed{};
    try { static_cast<void>(f.id(unattended, "runModal")); } catch (const std::runtime_error&) { malformed = true; }
    set_env("");
    require(dismissed == 1001 && requests.size() == calls, "auto-dismissed alert");
    require(out_of_range && malformed, "bad ALCHEMY_GUI_AUTODISMISS_ALERTS values were accepted");
    static_cast<void>(f.id(unattended, "release"));
    gui::appkit::set_alert_presenter_override({});

    // A cloaked run (ALCHEMY_GUI_CLOAK) refuses the real dialog, which would be
    // its one visible window; ALCHEMY_GUI_AUTODISMISS_ALERTS still answers.
    {
        const auto set_cloak = [](const char* value) {
#ifdef _WIN32
            _putenv_s("ALCHEMY_GUI_CLOAK", value);
#else
            if (*value) setenv("ALCHEMY_GUI_CLOAK", value, 1); else unsetenv("ALCHEMY_GUI_CLOAK");
#endif
        };
        const char* before = std::getenv("ALCHEMY_GUI_CLOAK");
        const std::string saved = before ? before : "";
        const auto cloaked = make_alert(true);
        set_cloak("1");
        std::string why;
        try { static_cast<void>(f.id(cloaked, "runModal")); } catch (const std::exception& error) { why = error.what(); }
        set_env("0");
        const auto answered = f.id(cloaked, "runModal");
        set_env("");
        set_cloak(saved.c_str());
        require(why.find("cloaked run") != std::string::npos, "a cloaked run's NSAlert was not refused: " + why);
        require(answered == 1000, "ALCHEMY_GUI_AUTODISMISS_ALERTS did not answer a cloaked run's alert");
        static_cast<void>(f.id(cloaked, "release"));
    }
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));

#ifdef _WIN32
    // The real task dialog (comctl32 v6), clicked from its creation callback.
    // Needs an interactive window station.
    USEROBJECTFLAGS flags{};
    const bool interactive = GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), nullptr) &&
                             (flags.dwFlags & WSF_VISIBLE);
    if (interactive) {
        gui::appkit::AlertRequest dialog;
        dialog.title = "Alchemy AppKit test";
        dialog.message = "Task dialog";
        dialog.informative = "Clicked by the test";
        dialog.buttons = {"First", "Second & third", "Cancel"};
        dialog.width = 300;
        const auto chosen = gui::appkit::run_task_dialog(dialog, [](void* handle) {
            SetWindowPos(static_cast<HWND>(handle), nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            PostMessageW(static_cast<HWND>(handle), TDM_CLICK_BUTTON, 1001, 0);
        });
        require(chosen == 1, "TaskDialogIndirect button mapping");
        const auto cancelled = gui::appkit::run_task_dialog(dialog, [](void* handle) {
            SetWindowPos(static_cast<HWND>(handle), nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            PostMessageW(static_cast<HWND>(handle), WM_COMMAND, IDCANCEL, 0);   // Escape
        });
        require(cancelled == 2, "Escape (IDCANCEL) did not answer the Cancel button");
    } else {
        std::cout << "task dialog check skipped: no interactive window station\n";
    }
#endif
}

// The AppKit -> QuartzCore seam: where a CAMetalLayer of a view is shown.
void layer_hosts(Fixture& f) {
    namespace qc = gui::quartzcore;
    journal.clear();
    const auto first = make_window(f, {{100, 100}, {800, 600}});
    const auto first_id = f.id(first, "windowNumber");
    const auto delegate_class = f.subclass("TestBackingDelegate", "NSObject");
    f.add(delegate_class, "windowDidChangeBackingProperties:", [](CpuState&, GuestMemory&) { journal.push_back("backing"); });
    const auto delegate = f.id(f.id(delegate_class, "alloc"), "init");
    static_cast<void>(f.id(first, "setDelegate:", {delegate}));

    const auto metal_view = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{10, 20}, {300, 200}});
    static_cast<void>(f.id(metal_view, "setAutoresizingMask:", {2 | 16}));   // width and height sizable
    const auto layer = f.id(f.cls("CAMetalLayer"), "layer");
    require(layer && qc::is_metal_layer(f.objc, layer), "+[CAMetalLayer layer]");
    static_cast<void>(f.id(metal_view, "setWantsLayer:", {1}));
    static_cast<void>(f.id(metal_view, "setLayer:", {layer}));
    require(!qc::layer_host(f.objc, layer), "a layer outside any window has a host");
    static_cast<void>(f.id(f.id(first, "contentView"), "addSubview:", {metal_view}));
    auto host = qc::layer_host(f.objc, layer);
    require(hosted(host, first_id, {{10, 20}, {300, 200}}, 2), "host after entering the window: " + describe(host));

    CallRegs origin;
    gui::appkit::put_point(origin, 0, {30, 40});
    static_cast<void>(f.send(metal_view, "setFrameOrigin:", origin));
    host = qc::layer_host(f.objc, layer);
    require(hosted(host, first_id, {{30, 40}, {300, 200}}, 2), "host after a view move: " + describe(host));

    // Window resize: autoresizing grows the view; the host follows.
    f.server.set_content_rect(first_id, {{100, 100}, {900, 700}});
    host = qc::layer_host(f.objc, layer);
    require(hosted(host, first_id, {{30, 40}, {400, 300}}, 2), "host after a window resize: " + describe(host));
    // A window move changes nothing inside the content area.
    f.server.set_content_rect(first_id, {{150, 120}, {900, 700}});
    require(hosted(qc::layer_host(f.objc, layer), first_id, {{30, 40}, {400, 300}}, 2), "host after a window move");

    // DPI change (WM_DPICHANGED): the backing scale follows.
    f.server.change_backing_scale(first_id, 1.5);
    host = qc::layer_host(f.objc, layer);
    require(hosted(host, first_id, {{30, 40}, {400, 300}}, 1.5) && log_has("backing") &&
                near(f.send(first, "backingScaleFactor").d(0), 1.5), "host after a DPI change: " + describe(host));
    CallRegs backing;
    gui::appkit::put_rect(backing, 0, {{0, 0}, {400, 300}});
    require(same(f.rect_of(f.send(metal_view, "convertRectToBacking:", backing)), {{0, 0}, {600, 450}}), "convertRectToBacking: at 1.5");

    // Moving the view to another window moves the presentation.
    const auto second = make_window(f, {{1000, 100}, {640, 480}});
    const auto second_id = f.id(second, "windowNumber");
    static_cast<void>(f.id(f.id(second, "contentView"), "addSubview:", {metal_view}));
    host = qc::layer_host(f.objc, layer);
    require(hosted(host, second_id, {{30, 40}, {400, 300}}, 2), "host after moving windows: " + describe(host));
    // A nested position through a flipped-free container.
    const auto container = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{100, 50}, {500, 400}});
    static_cast<void>(f.id(f.id(second, "contentView"), "addSubview:", {container}));
    static_cast<void>(f.id(container, "addSubview:", {metal_view}));
    host = qc::layer_host(f.objc, layer);
    require(hosted(host, second_id, {{130, 90}, {400, 300}}, 2), "host inside a container: " + describe(host));
    CallRegs container_origin;
    gui::appkit::put_point(container_origin, 0, {0, 0});
    static_cast<void>(f.send(container, "setFrameOrigin:", container_origin));
    require(hosted(qc::layer_host(f.objc, layer), second_id, {{30, 40}, {400, 300}}, 2), "host after the container moved");

    // -setLayer:nil drops the host; a new layer gets one.
    static_cast<void>(f.id(metal_view, "setLayer:", {0}));
    require(!qc::layer_host(f.objc, layer), "host kept after -setLayer:nil");
    const auto replacement = f.id(f.cls("CAMetalLayer"), "layer");
    static_cast<void>(f.id(metal_view, "setLayer:", {replacement}));
    require(hosted(qc::layer_host(f.objc, replacement), second_id, {{30, 40}, {400, 300}}, 2), "host of a replacement layer");
    // Removal from the window, then window destruction.
    static_cast<void>(f.id(metal_view, "removeFromSuperview"));
    require(!qc::layer_host(f.objc, replacement), "host kept after -removeFromSuperview");
    static_cast<void>(f.id(container, "addSubview:", {metal_view}));
    require(qc::layer_host(f.objc, replacement).has_value(), "no host after re-adding the view");
    static_cast<void>(f.id(second, "close"));
    require(!qc::layer_host(f.objc, replacement), "host kept after the window closed");
    static_cast<void>(f.id(first, "close"));
    static_cast<void>(f.id(first, "release"));
    static_cast<void>(f.id(second, "release"));
}

// Bounds transforms and cursor rectangles.
void bounds_and_cursor_rects(Fixture& f) {
    journal.clear();
    const auto window = make_window(f, {{100, 100}, {800, 600}});
    const auto number = f.id(window, "windowNumber");
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    const auto content = f.id(window, "contentView");
    const auto container = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{50, 50}, {200, 100}});
    static_cast<void>(f.id(content, "addSubview:", {container}));
    CallRegs bounds;
    gui::appkit::put_rect(bounds, 0, {{0, 0}, {100, 50}});
    static_cast<void>(f.send(container, "setBounds:", bounds));
    require(same(f.rect_of(f.send(container, "bounds")), {{0, 0}, {100, 50}}) &&
                same(f.rect_of(f.send(container, "frame")), {{50, 50}, {200, 100}}), "-setBounds:");
    const auto child = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{10, 10}, {20, 20}});
    static_cast<void>(f.id(container, "addSubview:", {child}));
    CallRegs to_window;
    gui::appkit::put_rect(to_window, 0, {{0, 0}, {20, 20}});
    to_window.x[2] = 0;
    require(same(f.rect_of(f.send(child, "convertRect:toView:", to_window)), {{70, 70}, {40, 40}}), "scaled -convertRect:toView:nil");
    CallRegs hit;
    gui::appkit::put_point(hit, 0, {75, 75});
    require(f.send(content, "hitTest:", hit).x0 == child, "hit test through a scaled container");
    CallRegs size;
    gui::appkit::put_size(size, 0, {1, 1});
    size.x[2] = 0;
    const auto converted = f.send(child, "convertSize:toView:", size);
    require(near(converted.d(0), 2) && near(converted.d(1), 2), "-convertSize:toView: through a bounds scale");
    // The scale survives a frame size change.
    CallRegs grow;
    gui::appkit::put_size(grow, 0, {400, 200});
    static_cast<void>(f.send(container, "setFrameSize:", grow));
    require(same(f.rect_of(f.send(container, "bounds")), {{0, 0}, {200, 100}}), "bounds after -setFrameSize:");
    CallRegs translate;
    gui::appkit::put_point(translate, 0, {10, 0});
    static_cast<void>(f.send(container, "translateOriginToPoint:", translate));
    require(same(f.rect_of(f.send(container, "bounds")), {{-10, 0}, {200, 100}}), "-translateOriginToPoint:");
    require(same(f.rect_of(f.send(child, "convertRect:toView:", to_window)), {{90, 70}, {40, 40}}), "convert after translation");
    CallRegs unit;
    gui::appkit::put_size(unit, 0, {2, 2});
    static_cast<void>(f.send(container, "scaleUnitSquareToSize:", unit));
    require(same(f.rect_of(f.send(container, "bounds")), {{-5, 0}, {100, 50}}), "-scaleUnitSquareToSize:");

    // Cursor rectangles: a view that adds its bounds with the I-beam cursor.
    const auto cursor_view_class = f.subclass("TestCursorView", "NSView");
    f.add(cursor_view_class, "resetCursorRects", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("resetCursorRects");
        CallRegs registers;
        gui::appkit::put_rect(registers, 0, f.rect_of(f.send(s.x[0], "bounds")));
        registers.x[2] = f.id(f.cls("NSCursor"), "IBeamCursor");
        static_cast<void>(f.send(s.x[0], "addCursorRect:cursor:", registers));
    }, "v@:");
    const auto ibeam = 100 + static_cast<int>(gui::SystemCursor::ibeam), arrow = 100 + static_cast<int>(gui::SystemCursor::arrow);
    f.server.mouse = {100 + 300 + 5, 100 + 300 + 5};   // screen points: inside the view below
    const auto cursor_view = f.with_rect(f.id(cursor_view_class, "alloc"), "initWithFrame:", {{300, 300}, {100, 100}});
    static_cast<void>(f.id(content, "addSubview:", {cursor_view}));
    require(log_has("resetCursorRects"), "entering a window did not build cursor rectangles");
    require(!f.server.cursors_set.empty() && f.server.cursors_set.back() == ibeam, "cursor rectangle under the pointer");
    gui::Event moved;
    moved.type = gui::event_type::mouse_moved;
    moved.window = number;
    moved.location = {10, 10};
    f.server.queue(moved);
    f.pump();
    require(f.server.cursors_set.back() == arrow, "leaving the cursor rectangle did not restore the arrow");
    moved.location = {350, 350};
    f.server.queue(moved);
    f.pump();
    require(f.server.cursors_set.back() == ibeam, "entering the cursor rectangle");
    journal.clear();
    static_cast<void>(f.id(window, "invalidateCursorRectsForView:", {cursor_view}));
    require(std::count(journal.begin(), journal.end(), "resetCursorRects") == 1, "-invalidateCursorRectsForView:");
    static_cast<void>(f.id(cursor_view, "discardCursorRects"));
    moved.location = {360, 360};
    f.server.queue(moved);
    f.pump();
    require(f.server.cursors_set.back() == arrow, "discarded cursor rectangles still set the cursor");
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}


// ---- Blender's input paths ---------------------------------------------------------
// Stand-ins for Blender's GHOST classes (CocoaMetalView, BlenderWindow), with
// the same AppKit-facing logic, driven through the fake window server: the
// paths GHOST_SystemCocoa / GHOST_WindowCocoa take on keyboard, text, IME,
// pointer, tablet and drag-and-drop input.

Address make_window_of(Fixture& f, Address cls, gui::Rect content) {
    CallRegs init;
    gui::appkit::put_rect(init, 0, content);
    init.x[2] = gui::style::titled | gui::style::closable | gui::style::resizable | gui::style::miniaturizable;
    init.x[3] = 2;
    const auto window = f.send(f.id(cls, "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    require(window != 0, "NSWindow subclass creation");
    static_cast<void>(f.id(window, "setReleasedWhenClosed:", {0}));
    return window;
}

std::string ns_text(Fixture& f, Address value) {
    // insertText: / setMarkedText: take an NSString or NSAttributedString.
    if (!value) return "(nil)";
    return f.text(value);
}

// The view's text state (GHOST_WindowViewCocoa.hh: composing plus the IME
// state flags of WITH_INPUT_IME).
struct GhostTextState {
    bool composing{};
    bool ime_enabled{};       // GHOST_IME_ENABLED (a text field is focused and the input source is an IME)
    bool ime_composing{};     // GHOST_IME_COMPOSING
    bool composition_event{}, result_event{};
    std::string combined;
};
GhostTextState ghost;

Address make_ghost_view_class(Fixture& f) {
    const auto view_class = f.subclass("TestGhostView", "NSView");
    f.add(view_class, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    f.add(view_class, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    // -keyDown: as Blender implements it.
    f.add(view_class, "keyDown:", [&f](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto code = f.id(event, "keyCode");
        const auto flags = f.id(event, "modifierFlags");
        const auto characters = f.text(f.id(event, "characters"));
        const auto ignoring = f.text(f.id(event, "charactersIgnoringModifiers"));
        // checkKeyCodeIsControlChar: shortcuts and editing keys bypass the IME.
        const bool control = (flags & (gui::modifier::command | gui::modifier::control)) != 0 ||
                             code == 0x24 || code == 0x33 || code == 0x75 || code == 0x35 || code == 0x30 ||
                             (code >= 0x7b && code <= 0x7e);
        const bool ime_process = ghost.ime_enabled && (ghost.ime_composing || !control);
        if (!ime_process) journal.push_back("key:" + std::to_string(code) + ":" + characters);   // handleKeyEvent
        if (characters.empty() || ignoring.empty() || ghost.composing || ime_process) {
            ghost.composing = true;
            journal.push_back("interpret");
            static_cast<void>(f.id(self, "interpretKeyEvents:", {f.cf.array(std::array<Address, 1>{event})}));
            ghost.composition_event = ghost.result_event = false;
            ghost.combined.clear();
        }
    });
    f.add(view_class, "keyUp:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("keyUp:" + std::to_string(f.id(s.x[2], "keyCode")));
    });
    f.add(view_class, "flagsChanged:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("flagsChanged:" + std::to_string(f.id(s.x[2], "modifierFlags")));
    });
    f.add(view_class, "insertText:replacementRange:", [&f](CpuState& s, GuestMemory&) {
        ghost.composing = false;
        if (!ghost.ime_enabled) return;
        if (!ghost.ime_composing) journal.push_back("ime:start");
        const auto text = ns_text(f, s.x[2]);
        ghost.combined = ghost.result_event ? ghost.combined + text : text;
        if (!(ghost.composition_event || ghost.result_event)) journal.push_back("ime:result:" + ghost.combined);
        ghost.result_event = true;
        journal.push_back("ime:end");
        ghost.ime_composing = false;
    }, "v@:@{_NSRange=QQ}");
    f.add(view_class, "setMarkedText:selectedRange:replacementRange:", [&f](CpuState& s, GuestMemory&) {
        ghost.composing = false;
        const auto text = ns_text(f, s.x[2]);
        if (text.empty()) {
            if (ghost.ime_composing) { journal.push_back("ime:cancel"); ghost.ime_composing = false; }
            return;
        }
        ghost.composing = true;
        if (!ghost.ime_enabled) return;
        if (!ghost.ime_composing) { ghost.ime_composing = true; journal.push_back("ime:start"); }
        if (!(ghost.composition_event || ghost.result_event)) {
            ghost.composition_event = true;
            journal.push_back("ime:composition:" + text + ":" + std::to_string(s.x[3]) + "," + std::to_string(s.x[4]));
        }
    }, "v@:@{_NSRange=QQ}{_NSRange=QQ}");
    f.add(view_class, "unmarkText", [](CpuState&, GuestMemory&) { ghost.composing = false; }, "v@:");
    f.add(view_class, "hasMarkedText", [](CpuState& s, GuestMemory&) { s.x[0] = ghost.composing ? 1 : 0; }, "B@:");
    f.add(view_class, "doCommandBySelector:", [](CpuState&, GuestMemory&) {}, "v@::");
    // Pointer and tablet input (handleMouseEvent / handleTabletEvent).
    const auto pointer = [&f](std::string name) {
        return [&f, name](CpuState& s, GuestMemory&) {
            const auto event = s.x[2];
            const auto location = f.send(event, "locationInWindow");
            std::string entry = name + ":" + std::to_string(static_cast<int>(location.d(0))) + "," +
                                std::to_string(static_cast<int>(location.d(1)));
            const auto type = f.id(event, "type");
            if (type == gui::event_type::other_mouse_down) entry += ":button" + std::to_string(f.id(event, "buttonNumber"));
            if (type != gui::event_type::scroll_wheel && type != gui::event_type::magnify && f.id(event, "subtype") == 1) {   // NSEventSubtypeTabletPoint
                entry += ":pen:" + std::to_string(static_cast<int>(f.send(event, "pressure").s(0) * 100));
                const auto tilt = f.send(event, "tilt");
                entry += ":tilt" + std::to_string(static_cast<int>(tilt.d(0) * 10)) + "," + std::to_string(static_cast<int>(tilt.d(1) * 10));
            }
            if (type == gui::event_type::mouse_moved)
                entry += ":delta" + std::to_string(static_cast<int>(f.send(event, "deltaX").d(0))) + "," +
                         std::to_string(static_cast<int>(f.send(event, "deltaY").d(0)));
            if (type == gui::event_type::scroll_wheel)
                entry += ":phase" + std::to_string(f.id(event, "phase")) + ":dy" +
                         std::to_string(static_cast<int>(f.send(event, "scrollingDeltaY").d(0))) +
                         ((f.id(event, "hasPreciseScrollingDeltas") & 0xff) ? ":precise" : "");
            if (type == gui::event_type::magnify)
                entry += ":magnification" + std::to_string(static_cast<int>(f.send(event, "magnification").d(0) * 100));
            if (type == gui::event_type::tablet_proximity)
                entry += ":entering" + std::to_string(f.id(event, "isEnteringProximity") & 0xff) + ":device" +
                         std::to_string(f.id(event, "pointingDeviceType"));
            journal.push_back(entry);
        };
    };
    for (const auto* name : {"mouseDown:", "mouseUp:", "mouseDragged:", "mouseMoved:", "rightMouseDown:", "rightMouseUp:",
             "otherMouseDown:", "otherMouseUp:", "scrollWheel:", "magnifyWithEvent:", "tabletPoint:", "tabletProximity:"}) {
        std::string selector = name;
        f.add(view_class, selector, pointer(selector.substr(0, selector.size() - 1)));
    }
    return view_class;
}

gui::Event key_event(gui::WindowId window, std::uint32_t type, std::uint16_t code, std::string characters,
                     std::string ignoring, std::uint64_t flags = 0) {
    gui::Event key;
    key.type = type;
    key.window = window;
    key.key_code = code;
    key.characters = std::move(characters);
    key.characters_ignoring_modifiers = std::move(ignoring);
    key.modifier_flags = flags;
    return key;
}
gui::Event text_event(gui::WindowId window, std::string text, bool marked, std::uint64_t selection = 0) {
    gui::Event event;
    event.kind = gui::EventKind::text;
    event.window = window;
    event.text = std::move(text);
    event.marked = marked;
    event.selection_location = selection;
    return event;
}
std::string dump_journal() {
    std::string text;
    for (const auto& entry : journal) text += (text.empty() ? "" : " | ") + entry;
    return text;
}

void blender_keyboard_and_text(Fixture& f) {
    journal.clear();
    ghost = {};
    const auto view_class = make_ghost_view_class(f);
    const auto window = make_window(f, {{100, 100}, {800, 600}});
    const auto number = f.id(window, "windowNumber");
    const auto view = f.with_rect(f.id(view_class, "alloc"), "initWithFrame:", {{0, 0}, {800, 600}});
    // GHOST_WindowCocoa: contentView, initialFirstResponder, makeKeyAndOrderFront:.
    static_cast<void>(f.id(window, "setContentView:", {view}));
    static_cast<void>(f.id(window, "setInitialFirstResponder:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.id(window, "setAcceptsMouseMovedEvents:", {1}));
    require(f.id(window, "firstResponder") == view, "Blender's view did not become first responder through initialFirstResponder");
    require(f.server.at(number).text_input, "IME is not enabled for Blender's NSTextInputClient view");

    // A typed letter: handleKeyEvent with the characters, no key bindings.
    // The WM_CHAR a translated (or posted) key message adds repeats the key
    // press and is dropped.
    f.server.queue(key_event(number, gui::event_type::key_down, 0x00, "a", "a"));
    f.server.queue(text_event(number, "a", false));
    f.server.queue(key_event(number, gui::event_type::key_up, 0x00, "a", "a"));
    f.pump();
    require(journal == std::vector<std::string>{"key:0:a", "keyUp:0"}, "typed letter: " + dump_journal());

    // A dead key (Option+E) then a letter: the dead key has no characters,
    // so Blender composes and interprets; the letter brings the accented
    // character and ends the composition through -insertText:.
    journal.clear();
    f.server.queue(key_event(number, gui::event_type::key_down, 0x0e, "", "\xc2\xb4", gui::modifier::option));
    f.server.queue(key_event(number, gui::event_type::key_down, 0x00, "\xc3\xa1", "a"));
    f.server.queue(key_event(number, gui::event_type::key_down, 0x01, "s", "s"));
    f.pump();
    require(journal == std::vector<std::string>{"key:14:", "interpret", "key:0:\xc3\xa1", "interpret", "key:1:s"} && !ghost.composing,
            "dead key composition: " + dump_journal());

    // Injected Unicode (SendInput KEYEVENTF_UNICODE, WM_CHAR) and an IME
    // result outside Blender's IME mode: key presses carrying one character
    // each, which Blender turns into text events of an unknown key.
    journal.clear();
    f.server.queue(text_event(number, "\xc3\xa9", false));
    f.server.queue(text_event(number, "\xe6\x97\xa5\xe6\x9c\xac", false));
    f.pump();
    const std::uint16_t text_key = gui::appkit::text_input_key_code;
    const auto key = [&](const std::string& characters) { return "key:" + std::to_string(text_key) + ":" + characters; };
    const auto up = "keyUp:" + std::to_string(text_key);
    require(journal == std::vector<std::string>{key("\xc3\xa9"), up, key("\xe6\x97\xa5"), up, key("\xe6\x9c\xac"), up},
            "text outside IME mode: " + dump_journal());

    // A composition outside IME mode: a press without characters (like a
    // dead key) whose -interpretKeyEvents: sets the marked text.
    journal.clear();
    f.server.queue(text_event(number, "\xe3\x81\xab", true, 1));
    f.server.queue(text_event(number, "\xe4\xba\x8c", false));
    f.pump();
    require(journal == std::vector<std::string>{key(""), "interpret", up, key("\xe4\xba\x8c"), "interpret", up} && !ghost.composing,
            "composition outside IME mode: " + dump_journal());

    // Blender's IME mode (beginIME on a focused text field with an input
    // method): every update and result is its own key press, so Blender's
    // per-keyDown event flags reset and each update reaches the UI.
    journal.clear();
    ghost.ime_enabled = true;
    f.server.queue(text_event(number, "\xe3\x81\xab", true, 1));          // に
    f.server.queue(text_event(number, "\xe3\x81\xab\xe3\x81\xbb", true, 2));   // にほ
    f.server.queue(text_event(number, "\xe6\x97\xa5\xe6\x9c\xac", false));    // 日本
    f.server.queue(key_event(number, gui::event_type::key_down, 0x24, "\r", "\r"));   // Return after the result
    f.pump();
    require(journal == std::vector<std::string>{
                "interpret", "ime:start", "ime:composition:\xe3\x81\xab:1,0", up,
                "interpret", "ime:composition:\xe3\x81\xab\xe3\x81\xbb:2,0", up,
                "interpret", "ime:result:\xe6\x97\xa5", "ime:end", up,
                "interpret", "ime:start", "ime:result:\xe6\x9c\xac", "ime:end", up,
                "key:36:\r"},
            "IME mode: " + dump_journal());
    // Cancelling a composition: an empty marked text.
    journal.clear();
    f.server.queue(text_event(number, "\xe3\x81\x82", true, 1));
    f.server.queue(text_event(number, "", true));
    f.pump();
    require(journal == std::vector<std::string>{"interpret", "ime:start", "ime:composition:\xe3\x81\x82:1,0", up,
                                                "interpret", "ime:cancel", up},
            "IME cancel: " + dump_journal());
    ghost.ime_enabled = false;

    // Modifiers reach -flagsChanged:; Command key-up events are not passed
    // to the window (AppKit), Blender reads them before -sendEvent:.
    journal.clear();
    auto flags = key_event(number, gui::event_type::flags_changed, 0x37, "", "", gui::modifier::command);
    f.server.queue(flags);
    f.server.queue(key_event(number, gui::event_type::key_up, 0x0d, "w", "w", gui::modifier::command));
    flags.modifier_flags = 0;
    f.server.queue(flags);
    f.server.queue(key_event(number, gui::event_type::key_up, 0x0d, "w", "w"));
    f.pump();
    require(journal == std::vector<std::string>{"flagsChanged:" + std::to_string(gui::modifier::command), "flagsChanged:0", "keyUp:13"},
            "flagsChanged / Command key-up: " + dump_journal());
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}

void blender_pointer_input(Fixture& f) {
    journal.clear();
    const auto view_class = f.objc.find_class("TestGhostView");
    require(view_class != 0, "TestGhostView");
    const auto window = make_window(f, {{100, 100}, {800, 600}});
    const auto number = f.id(window, "windowNumber");
    const auto view = f.with_rect(f.id(view_class, "alloc"), "initWithFrame:", {{0, 0}, {800, 600}});
    static_cast<void>(f.id(window, "setContentView:", {view}));
    static_cast<void>(f.id(window, "setInitialFirstResponder:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.id(window, "setAcceptsMouseMovedEvents:", {1}));

    const auto pointer = [&](std::uint32_t type, gui::Point location) {
        gui::Event event;
        event.type = type;
        event.window = number;
        event.location = location;
        return event;
    };
    auto moved = pointer(gui::event_type::mouse_moved, {10, 20});
    moved.delta_x = 3; moved.delta_y = -4;   // Apple deltas: y grows downward
    f.server.queue(moved);
    auto middle = pointer(gui::event_type::other_mouse_down, {30, 40});
    middle.button_number = 2; middle.pressed_buttons = 4; middle.click_count = 1;
    f.server.queue(middle);
    auto middle_up = middle;
    middle_up.type = gui::event_type::other_mouse_up; middle_up.pressed_buttons = 0;
    f.server.queue(middle_up);
    auto right = pointer(gui::event_type::right_mouse_down, {50, 60});
    right.button_number = 1; right.pressed_buttons = 2;
    f.server.queue(right);
    auto right_up = right;
    right_up.type = gui::event_type::right_mouse_up; right_up.pressed_buttons = 0;
    f.server.queue(right_up);
    // A pen: proximity, then a press of the tablet-point subtype.
    auto proximity = pointer(gui::event_type::tablet_proximity, {70, 80});
    proximity.entering_proximity = true;
    proximity.pointing_device_type = gui::pointing_device::pen;
    f.server.queue(proximity);
    auto pen = pointer(gui::event_type::left_mouse_down, {70, 80});
    pen.pointing_device_type = gui::pointing_device::pen;
    pen.pressure = 0.5f; pen.tilt = {0.2, -0.3}; pen.pressed_buttons = 1; pen.click_count = 1;
    f.server.queue(pen);
    auto pen_drag = pen;
    pen_drag.type = gui::event_type::left_mouse_dragged; pen_drag.location = {75, 85}; pen_drag.pressure = 0.75f;
    f.server.queue(pen_drag);
    auto pen_up = pen;
    pen_up.type = gui::event_type::left_mouse_up; pen_up.pressure = 0; pen_up.pressed_buttons = 0;
    f.server.queue(pen_up);
    // Trackpad: a precise scroll gesture and a pinch; a wheel notch.
    auto scroll = pointer(gui::event_type::scroll_wheel, {90, 100});
    scroll.precise_scrolling = true; scroll.scroll_phase = gui::phase::began; scroll.delta_y = 12;
    f.server.queue(scroll);
    auto wheel = pointer(gui::event_type::scroll_wheel, {90, 100});
    wheel.delta_y = -1;
    f.server.queue(wheel);
    auto pinch = pointer(gui::event_type::magnify, {90, 100});
    pinch.magnification = 0.25;
    f.server.queue(pinch);
    f.pump();
    const std::vector<std::string> expected{
        "mouseMoved:10,20:delta3,-4",
        "otherMouseDown:30,40:button2", "otherMouseUp:30,40",
        "rightMouseDown:50,60", "rightMouseUp:50,60",
        "tabletProximity:70,80:entering1:device1",
        "mouseDown:70,80:pen:50:tilt2,-3", "mouseDragged:75,85:pen:75:tilt2,-3", "mouseUp:70,80:pen:0:tilt2,-3",
        "scrollWheel:90,100:phase1:dy12:precise", "scrollWheel:90,100:phase0:dy-1",
        "magnifyWithEvent:90,100:magnification25"};
    require(journal == expected, "pointer input: " + dump_journal());

    // Continuous grab (GHOST_kGrabWrap / Hide): cursor hidden, warped and
    // dissociated from the mouse through CoreGraphics.
    const auto hidden = f.server.cursor_hide_count;
    static_cast<void>(f.id(f.cls("NSCursor"), "hide"));
    require(f.server.cursor_hide_count == hidden + 1, "+[NSCursor hide]");
    static_cast<void>(f.id(f.cls("NSCursor"), "unhide"));
    require(f.server.cursor_hide_count == hidden, "+[NSCursor unhide]");
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}

// NSDraggingDestination: BlenderWindow's drag methods over the window
// server's drag notifications.
void blender_drag_and_drop(Fixture& f) {
    journal.clear();
    static bool accept = true;
    const auto window_class = f.subclass("TestBlenderWindow", "NSWindow");
    const auto location_of = [&f](Address info) {
        const auto point = f.send(info, "draggingLocation");
        return std::to_string(static_cast<int>(point.d(0))) + "," + std::to_string(static_cast<int>(point.d(1)));
    };
    f.add(window_class, "draggingEntered:", [&f, location_of](CpuState& s, GuestMemory&) {
        const auto info = s.x[2];
        const auto board = f.id(info, "draggingPasteboard");
        const auto types = f.id(board, "types");
        // Blender: TIFF, then file names, then strings.
        bool files{};
        for (std::size_t index = 0; index < f.cf.length(types); ++index)
            files = files || f.text(f.cf.element(types, index)) == "NSFilenamesPboardType";
        journal.push_back("entered:" + location_of(info) + (files ? ":files" : ":other") + ":window" +
                          std::to_string(f.id(info, "draggingDestinationWindow") == s.x[0]));
        s.x[0] = files ? 1 : 0;   // NSDragOperationCopy
    }, "Q@:@");
    f.add(window_class, "wantsPeriodicDraggingUpdates", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    f.add(window_class, "draggingUpdated:", [location_of](CpuState& s, GuestMemory&) {
        journal.push_back("updated:" + location_of(s.x[2]));
        s.x[0] = accept ? 1 : 0;
    }, "Q@:@");
    f.add(window_class, "draggingExited:", [](CpuState&, GuestMemory&) { journal.push_back("exited"); });
    f.add(window_class, "prepareForDragOperation:", [](CpuState& s, GuestMemory&) {
        journal.push_back("prepare");
        s.x[0] = accept ? 1 : 0;
    }, "B@:@");
    f.add(window_class, "performDragOperation:", [&f, location_of](CpuState& s, GuestMemory&) {
        const auto info = s.x[2];
        const auto board = f.id(info, "draggingPasteboard");
        const auto names = f.id(board, "propertyListForType:", {f.str("NSFilenamesPboardType")});
        std::string entry = "perform:" + location_of(info) + ":mask" + std::to_string(f.id(info, "draggingSourceOperationMask"));
        for (std::size_t index = 0; names && index < f.cf.length(names); ++index) {
            const auto path = f.text(f.cf.element(names, index));
            entry += ":" + path.substr(path.find_last_of('/') + 1);
        }
        const auto url = f.id(board, "stringForType:", {f.str("public.file-url")});
        entry += url && f.text(url).starts_with("file://") ? ":url" : ":nourl";
        journal.push_back(entry);
        s.x[0] = 1;
    }, "B@:@");

    const auto window = make_window_of(f, window_class, {{100, 100}, {800, 600}});
    const auto number = f.id(window, "windowNumber");
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    const auto types = f.cf.array(std::array<Address, 3>{f.str("NSFilenamesPboardType"), f.str("public.utf8-plain-text"), f.str("public.tiff")});
    static_cast<void>(f.id(window, "registerForDraggedTypes:", {types}));
    require(f.server.at(number).accepts_drops, "-registerForDraggedTypes: did not make the window a drop target");

    using gui::Notification;
    namespace op = gui::drag_operation;
    const std::vector<std::string> paths{"C:\\scenes\\cube.blend", "C:\\scenes\\tex.png"};
    const auto mask = op::copy | op::move | op::generic;
    // Enter, a periodic update (same position: declined), a move, leave.
    require(f.server.drag(number, Notification::drag_entered, {10, 20}, paths, mask) == op::copy, "draggingEntered: answer");
    require(f.server.drag(number, Notification::drag_updated, {10, 20}, paths, mask) == op::copy, "periodic update answer");
    require(f.server.drag(number, Notification::drag_updated, {30, 40}, paths, mask) == op::copy, "draggingUpdated: answer");
    static_cast<void>(f.server.drag(number, Notification::drag_exited, {30, 40}, paths, mask));
    require(journal == std::vector<std::string>{"entered:10,20:files:window1", "updated:30,40", "exited"},
            "drag enter/update/exit: " + dump_journal());
    require(f.server.at(number).sink->drag_operation(number) == op::none, "the answer outlived the drag");

    // A drop: prepare -> perform with the files.
    journal.clear();
    static_cast<void>(f.server.drag(number, Notification::drag_entered, {50, 60}, paths, mask));
    static_cast<void>(f.server.drag(number, Notification::files_dropped, {55, 65}, paths, mask));
    require(journal == std::vector<std::string>{"entered:50,60:files:window1", "prepare",
                                                "perform:55,65:mask" + std::to_string(mask) + ":cube.blend:tex.png:url"},
            "drop: " + dump_journal());

    // A drop the destination refused in its last update: cancelled.
    journal.clear();
    accept = false;
    static_cast<void>(f.server.drag(number, Notification::drag_entered, {50, 60}, paths, mask));
    require(f.server.drag(number, Notification::drag_updated, {70, 80}, paths, mask) == op::none, "refusing draggingUpdated:");
    static_cast<void>(f.server.drag(number, Notification::files_dropped, {70, 80}, paths, mask));
    require(journal == std::vector<std::string>{"entered:50,60:files:window1", "updated:70,80", "exited"}, "refused drop: " + dump_journal());
    accept = true;

    // A drop without a drag (the WM_DROPFILES fallback).
    journal.clear();
    static_cast<void>(f.server.drag(number, Notification::files_dropped, {5, 6}, {"C:\\a.blend"}, op::copy));
    require(journal == std::vector<std::string>{"entered:5,6:files:window1", "prepare", "perform:5,6:mask1:a.blend:url"},
            "drop without a drag: " + dump_journal());

    // A registered subview is the destination under the pointer; leaving it
    // for the window moves the drag to the window.
    journal.clear();
    const auto view_class = f.subclass("TestDropView", "NSView");
    f.add(view_class, "draggingEntered:", [](CpuState& s, GuestMemory&) { journal.push_back("view:entered"); s.x[0] = op::link; }, "Q@:@");
    f.add(view_class, "draggingExited:", [](CpuState&, GuestMemory&) { journal.push_back("view:exited"); });
    f.add(view_class, "draggingEnded:", [](CpuState&, GuestMemory&) { journal.push_back("view:ended"); });
    const auto drop_view = f.with_rect(f.id(view_class, "alloc"), "initWithFrame:", {{100, 100}, {100, 100}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {drop_view}));
    static_cast<void>(f.id(drop_view, "registerForDraggedTypes:", {f.cf.array(std::array<Address, 1>{f.str("public.file-url")})}));
    require(f.server.drag(number, Notification::drag_entered, {150, 150}, paths, op::every) == op::link, "view destination answer");
    require(f.server.drag(number, Notification::drag_updated, {20, 20}, paths, op::every) == op::copy, "window destination answer");
    static_cast<void>(f.server.drag(number, Notification::drag_exited, {20, 20}, paths, op::every));
    require(journal == std::vector<std::string>{"view:entered", "view:exited", "entered:20,20:files:window1", "exited"},
            "destination change: " + dump_journal());
    // A view that only implements draggingEntered: is offered the drop
    // (-performDragOperation: is optional: nothing is performed) and ended.
    journal.clear();
    static_cast<void>(f.server.drag(number, Notification::drag_entered, {150, 150}, paths, op::every));
    static_cast<void>(f.server.drag(number, Notification::files_dropped, {150, 150}, paths, op::every));
    require(journal == std::vector<std::string>{"view:entered", "view:ended"}, "view drop: " + dump_journal());
    static_cast<void>(f.id(drop_view, "release"));
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}

#ifdef _WIN32
bool interactive_desktop() {
    USEROBJECTFLAGS flags{};
    return GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), nullptr) &&
           (flags.dwFlags & WSF_VISIBLE);
}
// Task dialogs of the tests are shown off screen (the desktop is shared).
void move_off_screen(void* dialog) {
    SetWindowPos(static_cast<HWND>(dialog), nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
#endif

// NSEventTrackingRunLoopMode / NSModalPanelRunLoopMode are the run loop's
// mode objects, whichever was installed first: guests compare
// -[NSRunLoop currentMode] with them by pointer.
void run_loop_mode_constants(Fixture& f) {
    gui::InstallContext context{f.process, f.objc, f.cf};
    if (!gui::main_run_loop_available()) gui::install_runloop(context);
    for (const auto mode : {gui::event_tracking_mode, gui::modal_panel_mode}) {
        const auto object = gui::run_loop_mode_object(mode);
        require(object != 0 && f.cf.utf8(object) == mode, "the run loop's mode object for " + std::string(mode));
        for (const auto library : {gui::appkit::appkit_library, gui::appkit::cocoa_library}) {
            const auto cell = f.process.resolve(library, "_" + std::string(mode));
            require(cell && f.engine.read64(cell->address) == object,
                    "AppKit's " + std::string(mode) + " is not the run loop's mode object");
        }
    }
}

// -[NSAlert runModal] is application-modal over the real task dialog and the
// real run loop: the run loop's dialog-loop hook keeps NSModalPanelRunLoopMode
// steps (timers) running, the other windows are disabled, input for the
// application's windows is discarded while notifications (redraws) still
// arrive, and everything is restored when the alert returns.
void modal_alert_session(Fixture& f) {
#ifdef _WIN32
    if (!interactive_desktop() || !IsGUIThread(TRUE)) {
        std::cout << "modal alert session check skipped: no interactive window station\n";
        return;
    }
    journal.clear();
    gui::InstallContext context{f.process, f.objc, f.cf};
    gui::install_runloop(context);
    auto& loop = gui::main_run_loop();
    loop.set_message_dispatcher([&f] { return f.server.dispatch_messages(); });
    static_cast<void>(loop.run_once(gui::default_mode, 0, false));   // installs the dialog-loop hook

    // Blender's main window and its Preferences window, with real (never
    // shown) top-level HWNDs standing in for the window server's.
    const auto view_class = f.subclass("TestModalView", "NSView");
    f.add(view_class, "mouseDown:", [](CpuState&, GuestMemory&) { journal.push_back("mouseDown"); });
    f.add(view_class, "keyDown:", [](CpuState&, GuestMemory&) { journal.push_back("keyDown"); });
    f.add(view_class, "drawRect:", [](CpuState&, GuestMemory&) { journal.push_back("drawRect"); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto main_window = make_window(f, {{100, 100}, {800, 600}});
    const auto preferences = make_window(f, {{300, 300}, {400, 300}});
    const auto main_id = f.id(main_window, "windowNumber"), preferences_id = f.id(preferences, "windowNumber");
    const auto preferences_view = f.with_rect(f.id(view_class, "alloc"), "initWithFrame:", {{0, 0}, {400, 300}});
    static_cast<void>(f.id(preferences, "setContentView:", {preferences_view}));
    static_cast<void>(f.id(preferences, "orderFront:", {0}));
    static_cast<void>(f.id(main_window, "makeKeyAndOrderFront:", {0}));
    const auto make_hwnd = [] {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Alchemy AppKit modal test", WS_POPUP,
                               -32000, -32000, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    };
    const auto main_hwnd = make_hwnd(), preferences_hwnd = make_hwnd();
    require(main_hwnd && preferences_hwnd, "test HWND creation");
    f.server.native[main_id] = main_hwnd;
    f.server.native[preferences_id] = preferences_hwnd;
    journal.clear();

    HWND dialog{};
    int ticks = 0;
    bool preferences_disabled{}, owner_disabled{}, modal_mode{true};
    const auto ticker = loop.add_timer(gui::monotonic_seconds() + 0.01, 0.01, gui::common_modes, [&](gui::TimerId) {
        modal_mode = modal_mode && loop.current_mode() == gui::modal_panel_mode;
        if (!dialog) return;
        if (++ticks == 2) {
            preferences_disabled = !IsWindowEnabled(preferences_hwnd);
            owner_disabled = !IsWindowEnabled(main_hwnd);
            // The dialog loop dispatches messages of the other windows: a
            // click and a key press (discarded) and a repaint (delivered).
            auto click = key_event(preferences_id, gui::event_type::left_mouse_down, 0, "", "");
            click.location = {10, 10}; click.click_count = 1; click.pressed_buttons = 1;
            f.server.at(preferences_id).sink->handle_event(click);
            f.server.at(main_id).sink->handle_event(key_event(main_id, gui::event_type::key_down, 0, "a", "a"));
            f.server.at(main_id).sink->handle_event(text_event(main_id, "x", false));
            gui::Event expose;
            expose.kind = gui::EventKind::notification;
            expose.notification = gui::Notification::expose;
            expose.window = preferences_id;
            expose.rect = {{0, 0}, {400, 300}};
            f.server.at(preferences_id).sink->handle_event(expose);
            // A drag over a window during the session is refused.
            require(f.server.drag(preferences_id, gui::Notification::drag_entered, {5, 5}, {"C:\\a.blend"}, gui::drag_operation::copy) ==
                        gui::drag_operation::none, "a drag was accepted during the modal session");
        }
        if (ticks == 3) PostMessageW(dialog, TDM_CLICK_BUTTON, 1000, 0);
    });
    gui::appkit::set_alert_presenter_override([&](const gui::appkit::AlertRequest& request) {
        require(request.owner == main_hwnd, "the alert's owner is not the key window's HWND");
        return gui::appkit::run_task_dialog(request, [&](void* handle) {
            move_off_screen(handle);
            dialog = static_cast<HWND>(handle);
        });
    });
    std::atomic<bool> returned{};
    const auto thread = GetCurrentThreadId();
    std::thread watchdog([&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!returned && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        while (!returned) {
            EnumThreadWindows(thread, [](HWND window, LPARAM) -> BOOL {
                wchar_t name[16]{};
                GetClassNameW(window, name, 16);
                if (std::wstring_view(name) == L"#32770") PostMessageW(window, TDM_CLICK_BUTTON, 1000, 0);
                return TRUE;
            }, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    const auto alert = f.id(f.id(f.cls("NSAlert"), "alloc"), "init");
    static_cast<void>(f.id(alert, "setMessageText:", {f.str("Alchemy modal session")}));
    static_cast<void>(f.id(alert, "addButtonWithTitle:", {f.str("OK")}));
    static_cast<void>(f.id(alert, "addButtonWithTitle:", {f.str("Cancel")}));
    Address answer{};
    try {
        answer = f.id(alert, "runModal");
    } catch (...) {
        returned = true;
        watchdog.join();
        throw;
    }
    returned = true;
    watchdog.join();
    loop.cancel_timer(ticker);
    gui::appkit::set_alert_presenter_override({});
    require(answer == 1000, "the modal alert did not answer its first button");
    require(ticks >= 3 && modal_mode, "timers did not run in NSModalPanelRunLoopMode inside the task dialog");
    require(preferences_disabled && owner_disabled, "the application's windows were not disabled during the modal session");
    require(IsWindowEnabled(preferences_hwnd) && IsWindowEnabled(main_hwnd), "windows were not re-enabled after the alert");
    require(journal == std::vector<std::string>{"drawRect"}, "during the session: " + dump_journal());
    f.pump();
    require(!log_has("mouseDown") && !log_has("keyDown"), "input of the modal session was delivered afterwards: " + dump_journal());
    require(f.id(f.app, "keyWindow") == main_window, "key window after the modal session");
    // Input flows again.
    auto click = key_event(preferences_id, gui::event_type::left_mouse_down, 0, "", "");
    click.location = {10, 10}; click.click_count = 1; click.pressed_buttons = 1;
    f.server.queue(click);
    f.pump();
    require(log_has("mouseDown"), "input after the modal session");

    static_cast<void>(f.id(alert, "release"));
    loop.set_message_dispatcher({});
    f.server.native.clear();
    DestroyWindow(main_hwnd);
    DestroyWindow(preferences_hwnd);
    for (const auto value : {main_window, preferences}) {
        static_cast<void>(f.id(value, "close"));
        static_cast<void>(f.id(value, "release"));
    }
    static_cast<void>(f.id(preferences_view, "release"));
#else
    static_cast<void>(f);
#endif
}

} // namespace

// The key view loop: -keyViewSelectionDirection is NSSelectingNext while
// -selectNextKeyView: makes the next view first responder, NSDirectSelection otherwise.
std::vector<std::uint64_t> key_view_directions;
void key_view_loop(Fixture& f) {
    const auto probe = f.subclass("AlchemyKeyViewProbe", "NSView");
    static_cast<void>(f.objc.add_native_method(probe, "acceptsFirstResponder", [](transmute::CpuState& s, transmute::GuestMemory&) { s.x[0] = 1; }, false, "B@:"));
    static_cast<void>(f.objc.add_native_method(probe, "becomeFirstResponder", [&f](transmute::CpuState& s, transmute::GuestMemory&) {
        key_view_directions.push_back(f.id(f.id(s.x[0], "window"), "keyViewSelectionDirection"));
        s.x[0] = 1;
    }, false, "B@:"));
    const auto window = make_window(f, {{100, 100}, {300, 200}});
    const auto container = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    const auto first = f.with_rect(f.id(probe, "alloc"), "initWithFrame:", {{10, 10}, {50, 20}});
    const auto second = f.with_rect(f.id(probe, "alloc"), "initWithFrame:", {{10, 40}, {50, 20}});
    static_cast<void>(f.id(container, "addSubview:", {first}));
    static_cast<void>(f.id(container, "addSubview:", {second}));
    static_cast<void>(f.id(window, "setContentView:", {container}));
    static_cast<void>(f.id(first, "setNextKeyView:", {second}));
    key_view_directions.clear();
    require((f.id(window, "makeFirstResponder:", {first}) & 0xff) == 1, "first view is first responder");
    static_cast<void>(f.id(window, "selectNextKeyView:", {0}));
    require(f.id(window, "firstResponder") == second, "selectNextKeyView: moves to the next key view");
    require(key_view_directions == std::vector<std::uint64_t>{0, 1}, "NSDirectSelection, then NSSelectingNext during the loop");
    require(f.id(window, "keyViewSelectionDirection") == 0, "NSDirectSelection after the loop");
}

// ---- the frame view (chrome-s2 w1) ----
// The content view's superview is the window's frame view
// (+frameViewClassForStyleMask:, a subclass may override it), sized as
// AppKit's window frame, with the standard window buttons laid out by the
// frame's title-bar queries; -setStyleMask: swaps the class when it changes.
bool frame_rtl = false;
void frame_views(Fixture& f) {
    using gui::style::titled; using gui::style::closable; using gui::style::resizable;
    const auto frame_of = [&f](Address value) { return f.rect_of(f.send(value, "frame")); };
    const auto kind = [&f](Address value, std::string_view name) { return value && (f.id(value, "isKindOfClass:", {f.cls(name)}) & 0xff); };

    const auto window = make_window(f, {{100, 100}, {400, 300}});   // titled, closable, resizable
    const auto content = f.id(window, "contentView");
    const auto frame = f.id(content, "superview");
    require(kind(frame, "NSThemeFrame"), "a titled window's frame view is an NSThemeFrame");
    require(same(frame_of(frame), {{0, 0}, {400, 328}}), "frame view " + describe(frame_of(frame)));
    require(same(frame_of(content), {{0, 0}, {400, 300}}), "the content view at the frame view's origin");
    require(f.id(frame, "window") == window && f.id(frame, "superview") == 0, "the frame view is the window's root view");
    require(f.id(content, "nextResponder") == window, "the content view's next responder is the window");
    const auto close = f.id(window, "standardWindowButton:", {0});
    const auto miniaturize = f.id(window, "standardWindowButton:", {1});
    const auto zoom = f.id(window, "standardWindowButton:", {2});
    require(kind(close, "_NSThemeWidget") && kind(miniaturize, "_NSThemeWidget") && kind(zoom, "_NSThemeWidget"), "standard window buttons");
    require(f.id(window, "standardWindowButton:", {3}) == 0, "no toolbar button");
    require(f.id(close, "superview") == frame && f.id(close, "window") == window, "buttons are frame-view subviews");
    require(same(frame_of(close), {{7, 306}, {14, 16}}) && same(frame_of(miniaturize), {{27, 306}, {14, 16}}) &&
                same(frame_of(zoom), {{47, 306}, {14, 16}}),
            "button layout " + describe(frame_of(close)));
    require((f.id(close, "isEnabled") & 0xff) && !(f.id(miniaturize, "isEnabled") & 0xff) && (f.id(zoom, "isEnabled") & 0xff),
            "buttons enabled by the style");
    require(f.id(close, "target") == window && f.id(close, "action") == f.objc.selector("performClose:"), "the close button's action");
    // The full-screen title bar's reveal amount and -_inFullScreen (Chrome's kiosk mode asks both).
    require((f.id(close, "respondsToSelector:", {f.objc.selector("setRevealAmount:")}) & 0xff) && near(f.send(close, "revealAmount").d(0), 1),
            "-revealAmount starts at 1");
    CallRegs reveal;
    reveal.set_d(0, 0.25);
    static_cast<void>(f.send(zoom, "setRevealAmount:", reveal));
    require(near(f.send(zoom, "revealAmount").d(0), 0.25) && near(f.send(close, "revealAmount").d(0), 1), "-setRevealAmount: per button");
    require(!(f.id(frame, "_inFullScreen") & 0xff), "-_inFullScreen is NO outside full screen");
    // A subview converts to window coordinates as before (the frame view sits at the window base).
    const auto inner = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{10, 20}, {50, 50}});
    static_cast<void>(f.id(content, "addSubview:", {inner}));
    CallRegs point;
    gui::appkit::put_point(point, 0, {1, 2});
    point.x[2] = 0;
    const auto converted = f.send(inner, "convertPoint:toView:", point);
    require(near(converted.d(0), 11) && near(converted.d(1), 22), "-convertPoint:toView:nil through the frame view");

    // The frame view follows a resize.
    CallRegs size;
    gui::appkit::put_size(size, 0, {500, 350});
    static_cast<void>(f.send(window, "setContentSize:", size));
    require(same(frame_of(frame), {{0, 0}, {500, 378}}) && same(frame_of(close), {{7, 356}, {14, 16}}), "after a resize");
    require(same(f.rect_of(f.send(window, "contentLayoutRect")), {{0, 0}, {500, 350}}), "-contentLayoutRect");

    // A full-size content view fills the frame view; the title bar overlays it.
    static_cast<void>(f.id(window, "setStyleMask:", {titled | closable | resizable | gui::style::full_size_content_view}));
    require(f.id(content, "superview") == frame, "the same frame class keeps the frame view");
    require(same(frame_of(frame), {{0, 0}, {500, 350}}) && same(frame_of(close), {{7, 328}, {14, 16}}), "full-size content");
    require(same(f.rect_of(f.send(window, "contentLayoutRect")), {{0, 0}, {500, 322}}), "-contentLayoutRect leaves out the title bar");

    // Borderless: an NSNextStepFrame without buttons; the content view moves over.
    static_cast<void>(f.id(frame, "retain"));
    static_cast<void>(f.id(window, "setStyleMask:", {0}));
    const auto plain = f.id(content, "superview");
    require(plain != frame && kind(plain, "NSNextStepFrame") && !kind(plain, "NSThemeFrame"), "a borderless window's frame view");
    require(f.id(window, "standardWindowButton:", {0}) == 0, "no buttons without a title bar");
    require(same(frame_of(plain), {{0, 0}, {500, 350}}), "borderless frame view " + describe(frame_of(plain)));
    require(f.id(frame, "window") == 0 && f.cf.length(f.id(frame, "subviews")) == 3, "the old frame view left the window with its buttons");
    require(f.id(content, "window") == window && f.id(inner, "window") == window, "the content view stays in the window");
    static_cast<void>(f.id(frame, "release"));

    // A subclass's frame class: its title-bar height, centring and right-to-left layout.
    const auto custom_frame = f.subclass("AlchemyTestThemeFrame", "NSThemeFrame");
    f.add(custom_frame, "_titlebarHeight", [](CpuState& s, GuestMemory&) { gui::appkit::set_d(s, 0, 40); }, "d@:");
    f.add(custom_frame, "_shouldCenterTrafficLights", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    const auto custom_window = f.subclass("AlchemyTestFramedWindow", "NSWindow");
    static_cast<void>(f.objc.add_native_method(custom_window, "frameViewClassForStyleMask:", [custom_frame](CpuState& s, GuestMemory&) {
        s.x[0] = custom_frame;
    }, true, "#@:Q"));
    f.add(custom_window, "windowTitlebarLayoutDirection", [](CpuState& s, GuestMemory&) { s.x[0] = frame_rtl ? 1 : 0; }, "q@:");
    const auto framed = make_window_of(f, custom_window, {{100, 100}, {400, 300}});
    const auto custom = f.id(f.id(framed, "contentView"), "superview");
    require(f.objc.object_class(custom) == custom_frame, "+frameViewClassForStyleMask: of the subclass");
    require(same(frame_of(custom), {{0, 0}, {400, 340}}), "the subclass's title-bar height");
    const auto custom_close = f.id(framed, "standardWindowButton:", {0});
    require(same(frame_of(custom_close), {{7, 312}, {14, 16}}), "centred in the title bar " + describe(frame_of(custom_close)));
    frame_rtl = true;
    static_cast<void>(f.id(custom, "_tileTitlebarAndRedisplay:", {0}));
    require(same(frame_of(custom_close), {{379, 312}, {14, 16}}) &&
                same(frame_of(f.id(framed, "standardWindowButton:", {2})), {{339, 312}, {14, 16}}),
            "mirrored for right-to-left " + describe(frame_of(custom_close)));
    frame_rtl = false;

    for (const auto value : {window, framed}) {
        static_cast<void>(f.id(value, "close"));
        static_cast<void>(f.id(value, "release"));
    }
    static_cast<void>(f.id(inner, "release"));
}

// ---- the window delegate through the notification center (chrome-s2 w1) ----
// -setDelegate: registers the delegate with the default center (object =
// the window) for the NSWindowDelegate methods it implements: it hears the
// window's notifications once, whoever posts them, and none after it leaves.
std::vector<std::string> delegate_log;
void window_delegate_notifications(Fixture& f) {
    const auto delegate_class = f.subclass("AlchemyCenterWindowDelegate", "NSObject");
    for (const auto* name : {"windowDidResize:", "windowDidMove:", "windowWillClose:", "windowDidUpdate:"}) {
        const std::string entry = name;
        f.add(delegate_class, entry, [&f, entry](CpuState& s, GuestMemory&) {
            delegate_log.push_back(entry + std::to_string(f.id(s.x[2], "object")));
        });
    }
    const auto delegate = f.id(f.id(delegate_class, "alloc"), "init");
    const auto window = make_window(f, {{100, 100}, {300, 200}});
    const auto other = make_window(f, {{500, 100}, {300, 200}});
    const auto tag = [](std::string_view name, Address object) { return std::string(name) + std::to_string(object); };
    static_cast<void>(f.id(window, "setDelegate:", {delegate}));
    require(f.id(window, "delegate") == delegate, "-delegate");

    delegate_log.clear();
    CallRegs size;
    gui::appkit::put_size(size, 0, {320, 240});
    static_cast<void>(f.send(window, "setContentSize:", size));
    // -setContentSize: keeps the top-left corner: the window resizes and moves.
    require(delegate_log == std::vector<std::string>{tag("windowDidResize:", window), tag("windowDidMove:", window)},
            "windowDidResize: and windowDidMove: once each, through the center");
    delegate_log.clear();
    static_cast<void>(f.send(other, "setContentSize:", size));
    require(delegate_log.empty(), "another window's notification does not reach the delegate");
    // A notification someone else posts for the window reaches the delegate too.
    const auto center = f.id(f.cls("NSNotificationCenter"), "defaultCenter");
    static_cast<void>(f.id(center, "postNotificationName:object:", {f.str("NSWindowDidMoveNotification"), window}));
    require(delegate_log == std::vector<std::string>{tag("windowDidMove:", window)}, "a posted NSWindowDidMoveNotification");
    delegate_log.clear();
    static_cast<void>(f.id(window, "update"));
    require(delegate_log == std::vector<std::string>{tag("windowDidUpdate:", window)}, "-update posts NSWindowDidUpdateNotification");

    // Replaced and cleared: the old delegate hears nothing more.
    static_cast<void>(f.id(window, "setDelegate:", {0}));
    delegate_log.clear();
    static_cast<void>(f.send(window, "setContentSize:", size));
    gui::appkit::put_size(size, 0, {330, 250});
    static_cast<void>(f.send(window, "setContentSize:", size));
    static_cast<void>(f.id(center, "postNotificationName:object:", {f.str("NSWindowDidMoveNotification"), window}));
    require(delegate_log.empty(), "no notifications after -setDelegate:nil");

    // windowWillClose: once on -close.
    static_cast<void>(f.id(other, "setDelegate:", {delegate}));
    delegate_log.clear();
    static_cast<void>(f.id(other, "close"));
    require(delegate_log == std::vector<std::string>{tag("windowWillClose:", other)}, "windowWillClose: once");
    static_cast<void>(f.id(other, "setDelegate:", {0}));
    static_cast<void>(f.id(window, "close"));
    for (const auto value : {window, other, delegate}) static_cast<void>(f.id(value, "release"));
}

// ---- a view lets go of its layer (chrome-s2 w1) ----
// The layer's delegate is the view and not retained: when the view drops the
// layer (-setLayer:, -dealloc) it clears the delegate if it is still the
// view, so a layer that outlives the view never messages it.
void layer_outlives_view(Fixture& f) {
    const auto layer_class = f.cls("CALayer");
    const auto view = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {50, 50}});
    const auto layer = f.id(f.id(layer_class, "layer"), "retain");
    static_cast<void>(f.id(view, "setLayer:", {layer}));
    require(f.id(layer, "delegate") == view, "the view is its layer's delegate");
    static_cast<void>(f.id(view, "release"));
    require(f.id(layer, "delegate") == 0, "a layer that outlives its view has no delegate");
    // -setLayer: with another layer: the old one's delegate is cleared only
    // when it is still the view.
    const auto second = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {50, 50}});
    const auto other_delegate = f.id(f.id(f.cls("NSObject"), "alloc"), "init");
    static_cast<void>(f.id(second, "setLayer:", {layer}));
    static_cast<void>(f.id(second, "setLayer:", {f.id(layer_class, "layer")}));
    require(f.id(layer, "delegate") == 0, "-setLayer: clears the old layer's delegate");
    const auto kept = f.id(f.id(layer_class, "layer"), "retain");
    static_cast<void>(f.id(second, "setLayer:", {kept}));
    static_cast<void>(f.id(kept, "setDelegate:", {other_delegate}));
    static_cast<void>(f.id(second, "setLayer:", {0}));
    require(f.id(kept, "delegate") == other_delegate, "another delegate stays");
    for (const auto value : {second, layer, kept, other_delegate}) static_cast<void>(f.id(value, "release"));
}

// ---- child windows (chrome-s2 w1) ----
// -addChildWindow:ordered: makes the window server parent the child (it stays
// above and moves with it) and orders it next to the parent; NSWindowBelow
// orders it below without owning it; a window closing frees its children.
void child_windows(Fixture& f) {
    const auto parent = make_window(f, {{100, 100}, {400, 300}});
    const auto child = make_window(f, {{150, 150}, {100, 80}});
    const auto other = make_window(f, {{600, 100}, {200, 200}});
    const auto parent_id = f.id(parent, "windowNumber"), child_id = f.id(child, "windowNumber");
    for (const auto value : {parent, child, other}) static_cast<void>(f.id(value, "orderFront:", {0}));
    const auto position = [&f](gui::WindowId id) {
        return static_cast<std::size_t>(std::find(f.server.z_order.begin(), f.server.z_order.end(), id) - f.server.z_order.begin());
    };
    static_cast<void>(f.id(parent, "addChildWindow:ordered:", {child, 1}));
    require(f.server.parents.count(child_id) && f.server.parents.at(child_id) == parent_id, "the server parents the child");
    require(f.id(child, "parentWindow") == parent && f.cf.length(f.id(parent, "childWindows")) == 1, "-parentWindow / -childWindows");
    require(position(child_id) + 1 == position(parent_id), "the child is right above its parent");
    static_cast<void>(f.id(parent, "removeChildWindow:", {child}));
    require(!f.server.parents.count(child_id) && f.id(child, "parentWindow") == 0, "-removeChildWindow:");
    static_cast<void>(f.id(parent, "addChildWindow:ordered:", {child, static_cast<Address>(-1)}));
    require(!f.server.parents.count(child_id) && position(child_id) == position(parent_id) + 1,
            "NSWindowBelow: ordered right below the parent, not owned");
    // Another parent takes it over.
    static_cast<void>(f.id(other, "addChildWindow:ordered:", {child, 1}));
    require(f.id(child, "parentWindow") == other && f.cf.length(f.id(parent, "childWindows")) == 0, "a new parent takes the child");
    static_cast<void>(f.id(other, "close"));
    require(f.id(child, "parentWindow") == 0 && f.cf.length(f.id(other, "childWindows")) == 0, "a closed parent frees its children");
    for (const auto value : {parent, child}) static_cast<void>(f.id(value, "close"));
    for (const auto value : {parent, child, other}) static_cast<void>(f.id(value, "release"));
}

// ---- ordering and occlusion (chrome-s2 w1) ----
// -makeKeyAndOrderFront: reaches -orderWindow:relativeTo: through the
// subclass (Chrome's NativeWidgetMacNSWindow tracks its visibility in its
// override); the other ordering methods order the window themselves, so an
// override that calls them does not recurse (Chrome's headless windows);
// NSWindowDidChangeOcclusionStateNotification is posted once
// -occlusionState and -isVisible agree, not while the window server shows the
// window (Win32 reports WM_SHOWWINDOW from inside ShowWindow).
std::vector<std::string> order_log;
std::string flag(Fixture& f, Address window) { return (f.id(window, "isVisible") & 0xff) ? "1" : "0"; }
Address order_probe_class(Fixture& f) {
    if (const auto found = f.objc.find_class("AlchemyOrderProbeWindow")) return found;
    const auto probe = f.subclass("AlchemyOrderProbeWindow", "NSWindow");
    static_cast<void>(f.objc.add_native_method(probe, "orderWindow:relativeTo:", [&f, probe](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        order_log.push_back("order:" + std::to_string(static_cast<std::int64_t>(s.x[2])) + ":" + std::to_string(s.x[3]));
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        static_cast<void>(f.objc.send_super(self, probe, f.objc.selector("orderWindow:relativeTo:"), registers));
        order_log.push_back("visible:" + flag(f, self));
    }, false, "v@:qq"));
    const auto delegate = f.subclass("AlchemyOcclusionDelegate", "NSObject");
    f.add(delegate, "windowDidChangeOcclusionState:", [&f](CpuState& s, GuestMemory&) {
        const auto window = f.id(s.x[2], "object");
        order_log.push_back("occlusion:" + std::to_string(f.id(window, "occlusionState")) + ":" + flag(f, window));
    });
    return probe;
}
std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const auto& item : items) text += (text.empty() ? "" : " ") + item;
    return text;
}
void expect_log(const std::vector<std::string>& expected, const std::string& what) {
    require(order_log == expected, what + ": got [" + joined(order_log) + "], expected [" + joined(expected) + "]");
    order_log.clear();
}

void window_ordering_and_occlusion(Fixture& f) {
    const auto probe = order_probe_class(f);
    const auto delegate = f.id(f.id(f.cls("AlchemyOcclusionDelegate"), "alloc"), "init");
    const auto window = make_window_of(f, probe, {{100, 100}, {400, 300}});
    static_cast<void>(f.id(window, "setDelegate:", {delegate}));
    const auto id = f.id(window, "windowNumber");
    require(f.id(window, "occlusionState") == 0 && flag(f, window) == "0", "a new window is not visible");

    order_log.clear();
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    expect_log({"order:1:0", "occlusion:2:1", "visible:1"}, "-makeKeyAndOrderFront:");
    require((f.id(window, "isKeyWindow") & 0xff) && f.server.key == id, "-makeKeyAndOrderFront: makes the window key");
    require(f.id(window, "occlusionState") == 2, "NSWindowOcclusionStateVisible once shown");

    static_cast<void>(f.id(window, "orderOut:", {0}));
    expect_log({"occlusion:0:0"}, "-orderOut:");
    require(!(f.id(window, "isKeyWindow") & 0xff) && flag(f, window) == "0", "-orderOut: hides the window and resigns key");
    static_cast<void>(f.id(window, "orderOut:", {0}));
    expect_log({}, "a second -orderOut: changes nothing");
    static_cast<void>(f.id(window, "orderFront:", {0}));
    expect_log({"occlusion:2:1"}, "-orderFront:");
    static_cast<void>(f.id(window, "orderFrontRegardless"));
    expect_log({}, "-orderFrontRegardless of a visible window");
    static_cast<void>(f.id(window, "setIsVisible:", {0}));
    expect_log({"occlusion:0:0"}, "-setIsVisible:NO");
    static_cast<void>(f.id(window, "setIsVisible:", {1}));
    expect_log({"occlusion:2:1"}, "-setIsVisible:YES");
    static_cast<void>(f.id(window, "orderWindow:relativeTo:", {0, 0}));
    expect_log({"order:0:0", "occlusion:0:0", "visible:0"}, "-orderWindow:NSWindowOut relativeTo:0");
    static_cast<void>(f.id(window, "orderWindow:relativeTo:", {1, 0}));
    expect_log({"order:1:0", "occlusion:2:1", "visible:1"}, "-orderWindow:NSWindowAbove relativeTo:0");

    // The window server hides the window on its own (an owner minimized):
    // occluded while -isVisible stays YES, visible again when it shows it.
    f.server.set_visible(id, false);
    expect_log({"occlusion:0:1"}, "server hide");
    f.server.set_visible(id, true);
    expect_log({"occlusion:2:1"}, "server show");
    // Miniaturized: not visible to the user.
    f.server.notify(id, gui::Notification::miniaturized);
    expect_log({"occlusion:0:1"}, "miniaturized");
    f.server.notify(id, gui::Notification::deminiaturized);
    expect_log({"occlusion:2:1"}, "deminiaturized");

    // Relative ordering: below / above another window, and to the back.
    const auto other = make_window_of(f, probe, {{200, 200}, {300, 200}});
    const auto other_id = f.id(other, "windowNumber");
    static_cast<void>(f.id(other, "orderFront:", {0}));
    order_log.clear();
    require(f.server.z_order.size() >= 2 && f.server.z_order[0] == other_id && f.server.z_order[1] == id, "z-order after -orderFront:");
    static_cast<void>(f.id(window, "orderWindow:relativeTo:", {1, other_id}));
    expect_log({"order:1:" + std::to_string(other_id), "visible:1"}, "-orderWindow:NSWindowAbove relativeTo:");
    require(f.server.z_order[0] == id && f.server.z_order[1] == other_id, "above the other window");
    static_cast<void>(f.id(window, "orderWindow:relativeTo:", {static_cast<Address>(-1), other_id}));
    require(f.server.z_order[0] == other_id && f.server.z_order[1] == id, "below the other window");
    const auto numbers = f.id(f.cls("NSWindow"), "windowNumbersWithOptions:", {0});
    require(f.cf.length(numbers) >= 2 && f.id(f.cf.element(numbers, 0), "longLongValue") == other_id &&
                f.id(f.cf.element(numbers, 1), "longLongValue") == id,
            "+windowNumbersWithOptions: follows the relative order");
    static_cast<void>(f.id(other, "orderBack:", {0}));
    require(f.server.z_order.back() == other_id, "-orderBack: puts the window at the back");
    order_log.clear();

    // A hidden window ordered relative to a hidden window: above everything.
    static_cast<void>(f.id(other, "orderOut:", {0}));
    static_cast<void>(f.id(window, "orderWindow:relativeTo:", {1, other_id}));
    require(f.server.z_order.front() == id, "relative to a window that is not on screen");
    order_log.clear();

    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(other, "close"));
    expect_log({}, "closing posts no occlusion change");
    static_cast<void>(f.id(window, "setDelegate:", {0}));
    static_cast<void>(f.id(window, "release"));
    static_cast<void>(f.id(other, "release"));
    static_cast<void>(f.id(delegate, "release"));
}

// A subclass whose -orderWindow:relativeTo: and -makeKeyAndOrderFront:
// answer with -orderFront: (Chrome's headless windows) does not recurse.
void ordering_override_calls_order_front(Fixture& f) {
    const auto headless = f.subclass("AlchemyHeadlessOrderWindow", "NSWindow");
    static int depth = 0, deepest = 0;
    const auto front = [&f](CpuState& s, GuestMemory&) {
        deepest = std::max(deepest, ++depth);
        if (depth < 8) static_cast<void>(f.id(s.x[0], "orderFront:", {0}));
        --depth;
    };
    static_cast<void>(f.objc.add_native_method(headless, "orderWindow:relativeTo:", front, false, "v@:qq"));
    static_cast<void>(f.objc.add_native_method(headless, "makeKeyAndOrderFront:", front, false, "v@:@"));
    const auto window = make_window_of(f, headless, {{100, 100}, {300, 200}});
    deepest = 0;
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    require(deepest == 1 && flag(f, window) == "1", "-orderFront: from an ordering override does not come back (depth " + std::to_string(deepest) + ")");
    deepest = 0;
    static_cast<void>(f.id(window, "orderWindow:relativeTo:", {0, 0}));
    require(deepest == 1, "-orderWindow:relativeTo: override calling -orderFront:");
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "release"));
}

// With a run loop, the occlusion notification waits for the run loop's next
// turn, as AppKit's comes after the window server's report.
void occlusion_after_run_loop(Fixture& f) {
    const auto loop = gui::appkit::run_loop_or_null();
    if (!loop) {
        std::cout << "deferred occlusion check skipped: no run loop\n";
        return;
    }
    const auto probe = order_probe_class(f);
    const auto delegate = f.id(f.id(f.cls("AlchemyOcclusionDelegate"), "alloc"), "init");
    const auto window = make_window_of(f, probe, {{100, 100}, {400, 300}});
    static_cast<void>(f.id(window, "setDelegate:", {delegate}));
    order_log.clear();
    static_cast<void>(f.id(window, "orderFront:", {0}));
    expect_log({}, "-orderFront: with a run loop");
    static_cast<void>(loop->run_once(gui::default_mode, 0, false));
    expect_log({"occlusion:2:1"}, "posted on the next run-loop turn");
    static_cast<void>(f.id(window, "orderOut:", {0}));
    static_cast<void>(f.id(window, "orderFront:", {0}));
    static_cast<void>(loop->run_once(gui::default_mode, 0, false));
    expect_log({}, "out and in again before the run loop: no change to post");
    static_cast<void>(f.id(window, "close"));
    static_cast<void>(f.id(window, "setDelegate:", {0}));
    static_cast<void>(f.id(window, "release"));
    static_cast<void>(f.id(delegate, "release"));
}

int main() {
#ifdef _WIN32
    // The desktop may be shared: task dialogs never take the foreground.
    _putenv_s("ALCHEMY_GUI_NO_ACTIVATE", "1");
#endif
    try {
      {
        Fixture fixture;
        application(fixture);
        windows_and_events(fixture);
        resources(fixture);
        carbon(fixture);
        application_events(fixture);
        alerts(fixture);
        layer_hosts(fixture);
        bounds_and_cursor_rects(fixture);
        blender_keyboard_and_text(fixture);
        blender_pointer_input(fixture);
        blender_drag_and_drop(fixture);
        key_view_loop(fixture);
        window_ordering_and_occlusion(fixture);
        ordering_override_calls_order_front(fixture);
        window_delegate_notifications(fixture);
        layer_outlives_view(fixture);
        child_windows(fixture);
        frame_views(fixture);
        modal_alert_session(fixture);   // installs the real run loop
        occlusion_after_run_loop(fixture);
        run_loop_mode_constants(fixture);   // the run loop installed after AppKit
      }
      {
        Fixture fixture(true);
        run_loop_mode_constants(fixture);   // the run loop installed first
      }
        std::cout << "Alchemy GUI AppKit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AppKit test failed: " << error.what() << '\n';
        return 1;
    }
}
