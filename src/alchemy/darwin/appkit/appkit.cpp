#include "alchemy/darwin/appkit/c_functions.hpp"
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/registration_audit.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

// AppKit providers over the window server and run loop (owner: A4):
// installation, the service seam, runtime plumbing and NSResponder. See
// appkit_internal.hpp for the object model.
namespace alchemy::gui {
namespace appkit {

namespace {
std::atomic<WindowServer*> window_server_override{};
std::atomic<RunLoop*> run_loop_override{};
} // namespace

void set_window_server_override(WindowServer* server) noexcept { window_server_override.store(server); }
void set_run_loop_override(RunLoop* loop) noexcept { run_loop_override.store(loop); }
WindowServer* window_server_or_null() noexcept {
    if (const auto server = window_server_override.load()) return server;
    if (!window_server_available()) return nullptr;
    try { return &window_server(); } catch (...) { return nullptr; }
}
WindowServer& require_window_server(std::string_view operation) {
    if (const auto server = window_server_or_null()) return *server;
    throw std::runtime_error("Alchemy AppKit: " + std::string(operation) +
                             " needs the window server, which is not available");
}
RunLoop* run_loop_or_null() noexcept {
    if (const auto loop = run_loop_override.load()) return loop;
    if (!main_run_loop_available()) return nullptr;
    try { return &main_run_loop(); } catch (...) { return nullptr; }
}

std::string host_environment(const char* name) {
#ifdef _WIN32
    char buffer[1024];
    std::size_t length{};
    if (getenv_s(&length, buffer, sizeof(buffer), name) != 0 || length == 0) return {};
    return std::string(buffer, length - 1);
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}
void note_trace(std::string_view text) {
    if (!ObjcRuntime::tracing()) return;
    std::fprintf(stderr, "Alchemy AppKit: %.*s\n", static_cast<int>(text.size()), text.data());
}

void fail(std::string_view why) { throw std::runtime_error("Alchemy AppKit: " + std::string(why)); }
void unsupported(std::string_view what) {
    throw std::runtime_error("Alchemy AppKit: unsupported " + std::string(what));
}

// ---- runtime plumbing --------------------------------------------------------------
void AppKit::method(Address cls, std::string name, Handler handler, std::string types, bool class_method,
                    const std::source_location& where) {
    registration_audit::Site site(where);
    // Every handler shares ownership of the provider, which therefore lives
    // as long as the runtime's registered methods.
    static_cast<void>(objc.add_native_method(cls, std::move(name),
        [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
        class_method, std::move(types)));
}
void AppKit::observe_object_lifetimes(std::string domain, ObjectLifetimeObserver observer, const std::source_location& where) {
    std::lock_guard lock(lifetime_mutex);
    for (const auto& entry : lifetime_observers)
        if (entry.first == domain) fail("object lifetimes are already observed for " + domain);
    for (const auto* name : {"-[NSWindow dealloc]", "-[NSMenu dealloc]", "-[NSMenu copyWithZone:]", "-[NSMenuItem dealloc]",
                             "-[NSMenuItem copyWithZone:]", "-[NSBitmapImageRep dealloc]", "-[NSBitmapImageRep copyWithZone:]"})
        registration_audit::record_extension(name, domain, where);
    lifetime_observers.emplace_back(std::move(domain), std::move(observer));
}
void AppKit::set_layout_engine(std::string domain, LayoutEngine engine, const std::source_location& where) {
    if (layout_engine) fail("a layout engine is already installed (" + layout_engine->first + ")");
    for (const auto* name : {"-[NSWindow display]", "-[NSWindow displayIfNeeded]", "-[NSView setAutoresizingMask:]"})
        registration_audit::record_extension(name, domain, where);
    layout_engine.emplace(std::move(domain), std::move(engine));
}
// The observers run outside the lock: they release objects (guest code).
void AppKit::object_deallocated(Address object) {
    std::vector<ObjectLifetimeObserver> observers;
    {
        std::lock_guard lock(lifetime_mutex);
        for (const auto& entry : lifetime_observers) observers.push_back(entry.second);
    }
    for (const auto& observer : observers)
        if (observer.deallocated) observer.deallocated(object);
}
void AppKit::object_copied(Address source, Address copy) {
    std::vector<ObjectLifetimeObserver> observers;
    {
        std::lock_guard lock(lifetime_mutex);
        for (const auto& entry : lifetime_observers) observers.push_back(entry.second);
    }
    for (const auto& observer : observers)
        if (observer.copied) observer.copied(source, copy);
}
void AppKit::function(std::string_view library, std::string symbol, Handler handler, const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(process.install_function(std::move(symbol),
        [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
        std::string(library)));
}
Address AppKit::declare_class(std::string_view name, std::string_view superclass) {
    auto cls = objc.find_class(name);
    if (!cls) {
        const auto parent = objc.find_class(superclass);
        if (!parent) fail("missing superclass " + std::string(superclass));
        cls = objc.register_native_class(std::string(name), parent, 8);
    }
    for (const auto library : {appkit_library, cocoa_library}) objc.publish_class(name, cls, library);
    return cls;
}
void AppKit::string_constant(std::string symbol, std::string_view value, const std::source_location& where) {
    object_constant(std::move(symbol), cf.string(value), where);
}
void AppKit::object_constant(std::string symbol, Address object, const std::source_location& where) {
    registration_audit::Site site(where);
    if (const auto record = process.objects().find(object)) record->immortal = true;
    for (const auto library : {appkit_library, cocoa_library}) {
        const auto slot = process.allocate_guest(8);
        process.main_engine().write64(slot, object);
        process.install_export(std::string(library), symbol, {slot, ExportKind::data});
    }
}
Address AppKit::send_id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments) {
    if (arguments.size() > 6) fail("too many message arguments");
    CallRegs registers;
    std::size_t index = 2;
    for (const auto value : arguments) registers.x[index++] = value;
    return objc.send(receiver, sel(selector), std::move(registers)).x0;
}
bool AppKit::is_kind(Address object, Address cls) {
    if (!object || !cls) return false;
    for (auto current = objc.object_class(object); current; current = superclass_of(current))
        if (current == cls) return true;
    return false;
}
Address AppKit::guest_call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments) {
    const auto found = process.resolve(library, symbol);
    if (!found) fail("missing runtime service " + std::string(symbol));
    return EngineAdapter(process.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
}
Address AppKit::invoke_block(Address block, std::initializer_list<Address> arguments) {
    if (!block) fail("null block");
    std::vector<Address> values{block};
    values.insert(values.end(), arguments.begin(), arguments.end());
    return EngineAdapter(process.calling_engine()).call(memory().read64(block + 16), values);
}
void AppKit::post_block_exit(std::uint32_t status) {
    // exit(3): atexit handlers and stdio flushing run in the guest, as after
    // -[NSApplication terminate:] on macOS.
    static_cast<void>(guest_call(libsystem_library, "_exit", {status}));
}

std::string AppKit::text(Address object) {
    if (!object) return {};
    if (cf.kind(object) == ObjectKind::string) return cf.utf8(object);
    const auto pointer = send(object, "UTF8String").x0;
    if (!pointer) return {};
    bool terminated{};
    const auto bytes = memory().read_string(pointer, 1, 1U << 20, &terminated);
    if (!terminated) fail("unterminated UTF8String");
    return {bytes.begin(), bytes.end()};
}
std::vector<Address> AppKit::items(Address array) {
    std::vector<Address> values;
    if (!array) return values;
    const auto count = send(array, "count").x0;
    if (count > (1U << 20)) fail("array is too large");
    values.reserve(static_cast<std::size_t>(count));
    for (Address index = 0; index < count; ++index) values.push_back(send_id(array, "objectAtIndex:", {index}));
    return values;
}

Address AppKit::notification(std::string_view name, Address object, Address user_info) {
    const auto cls = objc.find_class("NSNotification");
    if (!cls) fail("Foundation NSNotification is missing");
    return send_id(cls, "notificationWithName:object:userInfo:", {auto_string(name), object, user_info});
}
void AppKit::post(std::string_view name, Address object, Address delegate, std::string_view delegate_selector,
                  Address user_info) {
    const auto value = notification(name, object, user_info);
    if (delegate && !delegate_selector.empty()) {
        CallRegs registers;
        registers.x[2] = value;
        static_cast<void>(send_optional(delegate, delegate_selector, std::move(registers)));
    }
    const auto center_class = objc.find_class("NSNotificationCenter");
    if (!center_class) fail("Foundation NSNotificationCenter is missing");
    static_cast<void>(send_id(send(center_class, "defaultCenter").x0, "postNotification:", {value}));
}

// ---- NSResponder ----------------------------------------------------------------------
std::shared_ptr<ResponderState> AppKit::responder_state(Address object) {
    if (is_kind(object, window)) return window_state(object);
    if (is_kind(object, view)) return view_state(object);
    if (is_kind(object, application)) return objc.host_state<ApplicationState>(object);
    if (is_kind(object, window_controller) || is_kind(object, view_controller)) return objc.host_state<ControllerState>(object);
    return objc.host_state<ResponderState>(object);
}
std::shared_ptr<WindowState> AppKit::window_state(Address object) {
    if (!object) fail("nil NSWindow");
    return objc.host_state<WindowState>(object);
}
std::shared_ptr<ViewState> AppKit::view_state(Address object) {
    if (!object) fail("nil NSView");
    return objc.host_state<ViewState>(object);
}
std::shared_ptr<ApplicationState> AppKit::app_state() {
    if (!app) fail("NSApplication has not been created");
    return objc.host_state<ApplicationState>(app);
}
// NSResponder's default for event messages: pass them up the chain; an
// unanswered message ends in -noResponderFor:.
void AppKit::forward(Address responder, std::string_view selector, Address event) {
    const auto next = send(responder, "nextResponder").x0;
    if (next) static_cast<void>(send_id(next, selector, {event}));
    else static_cast<void>(send_id(responder, "noResponderFor:", {sel(selector)}));
}

void AppKit::install_responder() {
    const auto r = responder;
    method(r, "nextResponder", [this](CpuState& s, GuestMemory&) { s.x[0] = responder_state(s.x[0])->next_responder; });
    method(r, "setNextResponder:", [this](CpuState& s, GuestMemory&) { responder_state(s.x[0])->next_responder = s.x[2]; },
           "v@:@");
    method(r, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(r, "becomeFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(r, "resignFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(r, "performKeyEquivalent:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@");
    for (const auto* name : {"mouseDown:", "mouseUp:", "mouseDragged:", "mouseMoved:", "mouseEntered:", "mouseExited:",
             "rightMouseDown:", "rightMouseUp:", "rightMouseDragged:", "otherMouseDown:", "otherMouseUp:",
             "otherMouseDragged:", "scrollWheel:", "keyDown:", "keyUp:", "flagsChanged:", "tabletPoint:",
             "tabletProximity:", "magnifyWithEvent:", "rotateWithEvent:", "swipeWithEvent:", "smartMagnifyWithEvent:",
             "beginGestureWithEvent:", "endGestureWithEvent:", "pressureChangeWithEvent:", "cursorUpdate:",
             "insertText:"}) {
        const std::string selector = name;
        method(r, selector, [this, selector](CpuState& s, GuestMemory&) { forward(s.x[0], selector, s.x[2]); }, "v@:@");
    }
    // -[NSResponder noResponderFor:]: an unhandled key press beeps.
    method(r, "noResponderFor:", [this](CpuState& s, GuestMemory&) {
        if (s.x[2] == sel("keyDown:")) if (const auto server = window_server_or_null()) server->beep();
    }, "v@::");
    method(r, "doCommandBySelector:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], command = s.x[2];
        if (objc.responds(self, command)) { CallRegs registers; static_cast<void>(objc.send(self, command, registers)); return; }
        if (const auto next = send(self, "nextResponder").x0) static_cast<void>(send_id(next, "doCommandBySelector:", {command}));
    }, "v@::");
    method(r, "tryToPerform:with:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], action = s.x[2], argument = s.x[3];
        if (objc.responds(self, action)) {
            CallRegs registers; registers.x[2] = argument;
            static_cast<void>(objc.send(self, action, registers));
            s.x[0] = 1; return;
        }
        const auto next = send(self, "nextResponder").x0;
        s.x[0] = next ? send_bool(next, "tryToPerform:with:", {action, argument}) : 0;
    }, "B@::@");
    method(r, "interpretKeyEvents:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        for (const auto value : items(s.x[2])) interpret_key(self, value);
    }, "v@:@");
    // Key events are delivered as they arrive; nothing is ever buffered.
    method(r, "flushBufferedKeyEvents", [](CpuState&, GuestMemory&) {}, "v@:");
    method(r, "undoManager", [this](CpuState& s, GuestMemory&) {
        const auto next = send(s.x[0], "nextResponder").x0;
        s.x[0] = next ? send(next, "undoManager").x0 : 0;
    });
    // Services: a responder that cannot handle the types asks the next one.
    method(r, "validRequestorForSendType:returnType:", [this](CpuState& s, GuestMemory&) {
        const auto next = send(s.x[0], "nextResponder").x0;
        s.x[0] = next ? send_id(next, "validRequestorForSendType:returnType:", {s.x[2], s.x[3]}) : 0;
    }, "@@:@@");
    // Restorable state: the values at +restorableStateKeyPaths, encoded under
    // their key paths. Alchemy keeps no restoration archive between launches,
    // so -invalidateRestorableState has nothing to schedule.
    class_method(r, "restorableStateKeyPaths", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array({}); });
    method(r, "encodeRestorableStateWithCoder:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], coder = s.x[2];
        for (const auto path : items(send(send(self, "class").x0, "restorableStateKeyPaths").x0))
            static_cast<void>(send_id(coder, "encodeObject:forKey:", {send_id(self, "valueForKeyPath:", {path}), path}));
    }, "v@:@");
    method(r, "restoreStateWithCoder:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], coder = s.x[2];
        for (const auto path : items(send(send(self, "class").x0, "restorableStateKeyPaths").x0)) {
            if (!send_bool(coder, "containsValueForKey:", {path})) continue;
            static_cast<void>(send_id(self, "setValue:forKeyPath:", {send_id(coder, "decodeObjectForKey:", {path}), path}));
        }
    }, "v@:@");
    method(r, "invalidateRestorableState", [](CpuState&, GuestMemory&) {}, "v@:");
}

void AppKit::install() {
    object_class = objc.find_class("NSObject");
    responder = declare_class("NSResponder", "NSObject");
    application = declare_class("NSApplication", "NSResponder");
    window = declare_class("NSWindow", "NSResponder");
    view = declare_class("NSView", "NSResponder");
    event = declare_class("NSEvent", "NSObject");
    menu = declare_class("NSMenu", "NSObject");
    menu_item = declare_class("NSMenuItem", "NSObject");
    pasteboard = declare_class("NSPasteboard", "NSObject");
    cursor = declare_class("NSCursor", "NSObject");
    image = declare_class("NSImage", "NSObject");
    image_rep = declare_class("NSImageRep", "NSObject");
    bitmap_rep = declare_class("NSBitmapImageRep", "NSImageRep");
    color = declare_class("NSColor", "NSObject");
    color_space = declare_class("NSColorSpace", "NSObject");
    appearance = declare_class("NSAppearance", "NSObject");
    color_sampler = declare_class("NSColorSampler", "NSObject");
    alert = declare_class("NSAlert", "NSObject");
    workspace = declare_class("NSWorkspace", "NSObject");
    document_controller = declare_class("NSDocumentController", "NSObject");
    screen = declare_class("NSScreen", "NSObject");
    text_input_context = declare_class("NSTextInputContext", "NSObject");
    tracking_area = declare_class("NSTrackingArea", "NSObject");
    bezier_path = declare_class("NSBezierPath", "NSObject");
    dragging_info = declare_class("_AlchemyDraggingInfo", "NSObject");
    event_monitor = declare_class("_AlchemyEventMonitor", "NSObject");
    input_source = objc.find_class("_AlchemyTISInputSource");
    if (!input_source) input_source = objc.register_native_class("_AlchemyTISInputSource", object_class, 8);
    // The unified NSApp cell (darwin_frameworks.cpp binds AppKit's and
    // Cocoa's _NSApp to it).
    const auto slot = process.resolve(appkit_library, "_NSApp");
    if (!slot) fail("the _NSApp cell is missing");
    ns_app_slot = slot->address;

    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, std::string_view>>{
             {"_NSApplicationDidFinishLaunchingNotification", note::did_finish_launching},
             {"_NSApplicationWillFinishLaunchingNotification", note::will_finish_launching},
             {"_NSApplicationDidBecomeActiveNotification", note::did_become_active},
             {"_NSApplicationWillBecomeActiveNotification", note::will_become_active},
             {"_NSApplicationDidResignActiveNotification", note::did_resign_active},
             {"_NSApplicationWillResignActiveNotification", note::will_resign_active},
             {"_NSApplicationWillTerminateNotification", note::will_terminate},
             {"_NSApplicationDidHideNotification", note::did_hide}, {"_NSApplicationWillHideNotification", note::will_hide},
             {"_NSApplicationDidUnhideNotification", note::did_unhide},
             {"_NSApplicationWillUnhideNotification", note::will_unhide},
             {"_NSApplicationDidChangeScreenParametersNotification", note::screen_parameters},
             {"_NSWindowDidResizeNotification", note::window_did_resize}, {"_NSWindowDidMoveNotification", note::window_did_move},
             {"_NSWindowDidBecomeKeyNotification", note::window_did_become_key},
             {"_NSWindowDidResignKeyNotification", note::window_did_resign_key},
             {"_NSWindowDidBecomeMainNotification", note::window_did_become_main},
             {"_NSWindowDidResignMainNotification", note::window_did_resign_main},
             {"_NSWindowDidMiniaturizeNotification", note::window_did_miniaturize},
             {"_NSWindowWillMiniaturizeNotification", note::window_will_miniaturize},
             {"_NSWindowDidDeminiaturizeNotification", note::window_did_deminiaturize},
             {"_NSWindowWillEnterFullScreenNotification", note::window_will_enter_full_screen},
             {"_NSWindowDidEnterFullScreenNotification", note::window_did_enter_full_screen},
             {"_NSWindowWillExitFullScreenNotification", note::window_will_exit_full_screen},
             {"_NSWindowDidExitFullScreenNotification", note::window_did_exit_full_screen},
             {"_NSWindowDidChangeBackingPropertiesNotification", note::window_did_change_backing},
             {"_NSWindowDidChangeScreenNotification", note::window_did_change_screen},
             {"_NSWindowDidChangeOcclusionStateNotification", note::window_did_change_occlusion},
             {"_NSWindowWillStartLiveResizeNotification", note::window_will_start_live_resize},
             {"_NSWindowDidEndLiveResizeNotification", note::window_did_end_live_resize},
             {"_NSWindowDidExposeNotification", note::window_did_expose},
             {"_NSViewFrameDidChangeNotification", note::view_frame_did_change},
             {"_NSAppearanceNameAqua", "NSAppearanceNameAqua"}, {"_NSAppearanceNameDarkAqua", "NSAppearanceNameDarkAqua"},
             {"_NSAppearanceNameAccessibilityHighContrastAqua", "NSAppearanceNameAccessibilityAqua"},
             {"_NSAppearanceNameAccessibilityHighContrastDarkAqua", "NSAppearanceNameAccessibilityDarkAqua"},
             {"_NSPasteboardNameGeneral", "Apple CFPasteboard general"},
             {"_NSPasteboardNameDrag", "Apple CFPasteboard drag"},
             {"_NSStringPboardType", "NSStringPboardType"}, {"_NSTIFFPboardType", "NSTIFFPboardType"},
             {"_NSURLPboardType", "Apple URL pasteboard type"}, {"_NSPasteboardTypeURL", "public.url"},
             {"_NSBackingPropertyOldScaleFactorKey", "NSBackingPropertyOldScaleFactorKey"},
             {"_NSImageNameApplicationIcon", "NSApplicationIcon"},
             {"_NSCalibratedRGBColorSpace", "NSCalibratedRGBColorSpace"}})
        string_constant(symbol, value);
    // The run loop's own mode objects, so that a guest comparing
    // -[NSRunLoop currentMode] or CFRunLoopCopyCurrentMode with these
    // constants by pointer sees them equal (the run loop adopts AppKit's when
    // AppKit is installed first).
    for (const auto& [symbol, mode] : {std::pair{"_NSEventTrackingRunLoopMode", event_tracking_mode},
                                       std::pair{"_NSModalPanelRunLoopMode", modal_panel_mode}}) {
        if (const auto object = run_loop_mode_object(mode)) object_constant(symbol, object);
        else string_constant(symbol, mode);
    }

    install_responder();
    install_application();
    install_window();
    install_window_autosave();
    install_window_geometry();
    install_view();
    install_event();
    install_resources();
    install_alert();
    install_modal();
    install_carbon();
    install_graphics();
    install_controls();
    install_frame_views();   // chrome-s2 w1: NSFrameView family (needs NSButton)
    install_text();
    install_containers();
    install_container_coding();
    install_panels();
    install_window_tabs();   // NSWindow toolbars and tabs (after NSToolbar)
    install_c_functions(*this);   // NSDrawTiledRects, NSRunAlertPanel, NSWindowList, ... (c_functions.cpp)
    install_hitoolbox();
    install_controllers();
    install_documents();
    install_user_defaults_controller();
    install_nib();
}

} // namespace appkit

} // namespace alchemy::gui
