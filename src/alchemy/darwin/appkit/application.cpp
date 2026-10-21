#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/perf.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// NSApplication: the shared application, launching, the event pump
// (-nextEventMatchingMask:untilDate:inMode:dequeue:), event routing
// (-sendEvent:), activation, termination and target/action.
namespace alchemy::gui::appkit {
namespace {
// NSApplicationTerminateReply
constexpr Address terminate_cancel = 0, terminate_now = 1, terminate_later = 2;
constexpr std::uint64_t key_modifiers = modifier::shift | modifier::control | modifier::option | modifier::command;
// NSRequestUserAttentionType
constexpr Address critical_request = 0;

std::string lowercase(std::string value) {
    for (auto& character : value) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return value;
}
} // namespace

Address AppKit::shared_application(Address cls) {
    {
        std::lock_guard lock(mutex);
        if (app) return app;
    }
    // +sharedApplication on the principal class (a guest subclass when the
    // guest has one); -init records the instance.
    const auto object = send(send(cls, "alloc").x0, "init").x0;
    std::lock_guard lock(mutex);
    if (!app || app != object) fail("NSApplication initialization did not produce the shared application");
    return app;
}

void AppKit::finish_launching() {
    const auto state = app_state();
    if (state->launched) return;
    state->launched = true;
    perf::phase("-[NSApplication finishLaunching]");
    // Application-wide window-server events (Event::window == 0) reach the
    // same handler as window events through their own sink.
    if (const auto server = window_server_or_null(); server && !application_sink) {
        auto sink = std::make_shared<WindowSink>(weak_from_this());
        try {
            server->set_application_sink(sink);
            application_sink = std::move(sink);
        } catch (const std::runtime_error&) {
            // A window server without application events (test doubles):
            // activation, quit and screen notifications are not delivered.
        }
    }
    {
        std::lock_guard lock(mutex);
        launch_activation_pending = true;
    }
    post(note::will_finish_launching, app);
    post(note::did_finish_launching, app);
    start_autopress();   // diagnostic only (ALCHEMY_GUI_AUTOPRESS / ALCHEMY_CONTENTS_CAPTURE)
}

void AppKit::complete_launch_activation() {
    Address target{};
    {
        std::lock_guard lock(mutex);
        if (!launch_activation_pending) return;
        launch_activation_pending = false;
        target = std::exchange(deferred_key_window, 0);
    }
    if (!target) return;
    const auto state = window_state(target);
    if (!state->closed && state->id && state->visible) set_key_window(target, true);
}

// A delegate observes its object's notifications: AppKit registers it with
// the default notification center (object = the delegating object) for each
// notification whose delegate method it implements, and removes those
// observations when the delegate changes. So a notification that someone
// else posts (Chrome's -terminate: override posts
// NSApplicationWillTerminateNotification itself) reaches the delegate, and
// AppKit's own posts reach it once, through the center.
void AppKit::observe_delegate(Address object, Address previous, Address delegate, std::span<const DelegateNotification> table) {
    if (previous == delegate) return;
    const auto center_class = objc.find_class("NSNotificationCenter");
    if (!center_class) fail("Foundation NSNotificationCenter is missing");
    const auto center = send(center_class, "defaultCenter").x0;
    // The previous delegate is not retained and may already be deallocated
    // (Electron frees its application delegate, then sets nil): it is only
    // compared by address, never messaged.
    if (previous)
        for (const auto& entry : table)
            static_cast<void>(send_id(center, "removeObserver:name:object:", {previous, auto_string(entry.name), object}));
    if (!delegate) return;
    for (const auto& entry : table) {
        if (!objc.responds(delegate, sel(entry.selector))) continue;
        CallRegs registers;
        registers.x[2] = delegate; registers.x[3] = sel(entry.selector); registers.x[4] = auto_string(entry.name); registers.x[5] = object;
        static_cast<void>(send(center, "addObserver:selector:name:object:", registers));
    }
}

// -[NSApplication setDelegate:]: the NSApplicationDelegate notification methods.
void AppKit::set_application_delegate(Address delegate) {
    static constexpr DelegateNotification application_delegate_notifications[] = {
        {note::will_finish_launching, "applicationWillFinishLaunching:"},
        {note::did_finish_launching, "applicationDidFinishLaunching:"},
        {note::will_become_active, "applicationWillBecomeActive:"}, {note::did_become_active, "applicationDidBecomeActive:"},
        {note::will_resign_active, "applicationWillResignActive:"}, {note::did_resign_active, "applicationDidResignActive:"},
        {note::will_hide, "applicationWillHide:"}, {note::did_hide, "applicationDidHide:"},
        {note::will_unhide, "applicationWillUnhide:"}, {note::did_unhide, "applicationDidUnhide:"},
        {note::will_update, "applicationWillUpdate:"}, {note::did_update, "applicationDidUpdate:"},
        {note::will_terminate, "applicationWillTerminate:"},
        {note::screen_parameters, "applicationDidChangeScreenParameters:"},
        {note::did_change_occlusion, "applicationDidChangeOcclusionState:"},
        {note::protected_data_will_become_unavailable, "applicationProtectedDataWillBecomeUnavailable:"},
        {note::protected_data_did_become_available, "applicationProtectedDataDidBecomeAvailable:"},
    };
    const auto state = app_state();
    const auto previous = std::exchange(state->delegate, delegate);   // not retained, as AppKit's delegate
    observe_delegate(app, previous, delegate, application_delegate_notifications);
}

void AppKit::set_active(bool active) {
    const auto state = app_state();
    if (state->active == active) return;
    post(active ? note::will_become_active : note::will_resign_active, app);
    state->active = active;
    if (!active && resign_active_hook) resign_active_hook();
    post(active ? note::did_become_active : note::did_resign_active, app);
}

void AppKit::terminate(Address sender) {
    const auto state = app_state();
    Address reply = terminate_now;
    if (state->delegate) {
        CallRegs registers;
        registers.x[2] = app;
        if (const auto answer = send_optional(state->delegate, "applicationShouldTerminate:", registers)) reply = answer->x0;
    }
    static_cast<void>(sender);
    if (reply == terminate_cancel || reply == terminate_later) return;   // later: -replyToApplicationShouldTerminate:
    post(note::will_terminate, app);
    post_block_exit(0);
}

// ---- event queue -----------------------------------------------------------------------
Address AppKit::take_event(std::uint64_t mask, bool dequeue) {
    std::lock_guard lock(mutex);
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        const auto type = event_state(*it)->event.type;
        if (!(mask & mask_bit(type))) continue;
        const auto found = *it;
        if (dequeue) queue.erase(it);
        return found;
    }
    return 0;
}

void AppKit::enqueue(Address value, bool at_start) {
    bool wake_pump{};
    {
        std::lock_guard lock(mutex);
        const auto state = event_state(value);
        const auto type = state->event.type;
        const bool motion = type == event_type::mouse_moved || type == event_type::left_mouse_dragged ||
                            type == event_type::right_mouse_dragged || type == event_type::other_mouse_dragged;
        // Mouse coalescing: a motion event replaces a pending one of the same
        // kind for the same window, accumulating its deltas.
        if (motion && coalesce_mouse && !at_start && !queue.empty()) {
            const auto previous = queue.back();
            const auto old = event_state(previous);
            if (old->event.type == type && old->window == state->window) {
                state->event.delta_x += old->event.delta_x;
                state->event.delta_y += old->event.delta_y;
                queue.pop_back();
                release(previous);
            }
        }
        if (at_start) queue.push_front(value);
        else queue.push_back(value);
        wake_pump = pump_depth > 0 && (pump_mask & mask_bit(type));
    }
    // A matching event ends the pump's wait: on the main thread (inside a
    // sink callback) the innermost run_once stops; elsewhere the main thread
    // is woken and re-checks the queue.
    const bool main_thread = &process.calling_engine() == &process.main_engine();
    if (const auto loop = run_loop_or_null()) {
        if (main_thread && wake_pump) loop->stop();
        else if (!main_thread) loop->wake();
    } else if (!main_thread) {
        process.wake_main_thread();
    }
}

void AppKit::pump(std::string_view mode, double timeout) {
    auto& engine = process.calling_engine();
    if (engine_stop_requested(engine)) EngineAdapter::stop_thread(engine, "the NSApplication event wait");
    if (const auto loop = run_loop_or_null()) {
        const auto result = loop->run_once(mode, timeout, true);
        // A mode without sources returns at once; wait in small slices so a
        // finite deadline does not spin.
        if (result == RunResult::finished && timeout > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(timeout, 0.005)));
        return;
    }
    if (const auto server = window_server_or_null()) {
        // No run loop: dispatch Win32 messages directly, polling in slices.
        if (!server->dispatch_messages() && timeout > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(timeout, 0.005)));
    }
}

Address AppKit::next_event(std::uint64_t mask, double deadline, std::string_view mode, bool dequeue) {
    struct Depth {
        AppKit& kit; unsigned saved_mask_depth; std::uint64_t saved_mask;
        Depth(AppKit& owner, std::uint64_t mask) : kit(owner), saved_mask_depth(owner.pump_depth), saved_mask(owner.pump_mask) {
            ++kit.pump_depth; kit.pump_mask = mask;
        }
        ~Depth() { kit.pump_depth = saved_mask_depth; kit.pump_mask = saved_mask; }
    } depth(*this, mask);
    complete_launch_activation();
    const bool can_wait = run_loop_or_null() || window_server_or_null();
    for (bool pumped = false;; pumped = true) {
        display_pending();
        if (const auto found = take_event(mask, dequeue)) return found;
        const auto remaining = deadline - monotonic_seconds();
        if (pumped && (remaining <= 0 || !can_wait)) return 0;
        if (!can_wait) continue;   // nothing can deliver events; one more queue check
        pump(mode, std::max(0.0, remaining));
    }
}

// -[NSApplication sendEvent:]: key events (window-server text included, as
// key presses) go to the key window, pointer events to their window.
void AppKit::dispatch_event(Address value) {
    {
        std::lock_guard lock(mutex);
        assign(current_event, value);
    }
    value = run_monitors(value);
    if (!value) return;
    const auto state = event_state(value);
    const auto type = state->event.type;
    Address target{};
    switch (type) {
    case event_type::key_down:
        if (state->event.modifier_flags & modifier::command) {
            const auto menu_bar = app_state()->main_menu;
            if (menu_bar && send_bool(menu_bar, "performKeyEquivalent:", {value})) return;
        }
        target = key_window ? key_window : state->window;
        break;
    case event_type::key_up:
        // AppKit does not pass key-up events to the key window while Command
        // is held (applications that need them, like Blender, read them from
        // the event before -sendEvent:).
        if (state->event.modifier_flags & modifier::command) return;
        [[fallthrough]];
    case event_type::flags_changed:
        target = key_window ? key_window : state->window;
        break;
    case event_type::app_kit_defined:
    case event_type::system_defined:
    case event_type::application_defined:
    case event_type::periodic:
        return;
    default:
        if (cursor_hidden_until_moved && (type == event_type::mouse_moved || type == event_type::left_mouse_dragged)) {
            cursor_hidden_until_moved = false;
            if (const auto server = window_server_or_null()) server->set_cursor_visible(true);
        }
        target = state->window;
        if (mouse_down_hook && (type == event_type::left_mouse_down || type == event_type::right_mouse_down ||
                                type == event_type::other_mouse_down))
            mouse_down_hook(target);
        break;
    }
    if (target) static_cast<void>(send_id(target, "sendEvent:", {value}));
}

// The NSTextInputClient half of a window-server text key press (see
// text_input_key_code), from -interpretKeyEvents: / -handleEvent:.
void AppKit::deliver_text(Address client, const Event& text_event) {
    CallRegs registers;
    registers.x[2] = auto_string(text_event.text);
    if (text_event.marked) {
        if (!responds(client, "setMarkedText:selectedRange:replacementRange:")) return;
        registers.x[3] = text_event.selection_location; registers.x[4] = text_event.selection_length;
        registers.x[5] = not_found; registers.x[6] = 0;
        static_cast<void>(send(client, "setMarkedText:selectedRange:replacementRange:", registers));
    } else if (responds(client, "insertText:replacementRange:")) {
        registers.x[3] = not_found; registers.x[4] = 0;
        static_cast<void>(send(client, "insertText:replacementRange:", registers));
    } else {
        static_cast<void>(send(client, "insertText:", registers));
    }
}

void AppKit::display_pending() {
    if (display_depth) return;
    ++display_depth;
    struct Leave { unsigned& depth; ~Leave() { --depth; } } leave{display_depth};
    std::vector<Address> targets;
    {
        std::lock_guard lock(mutex);
        targets = window_order;
    }
    for (const auto target : targets) {
        const auto state = window_state(target);
        if (state->closed || !state->visible || !state->content_view) continue;
        if (layout_window_hook) layout_window_hook(target);
        display_window(target);
    }
}

void AppKit::set_key_window(Address target, bool key) {
    if (key) {
        Address previous{};
        {
            std::lock_guard lock(mutex);
            if (key_window == target) return;
            if (launch_activation_pending) { deferred_key_window = target; return; }
            previous = key_window;
        }
        if (previous) set_key_window(previous, false);
        const auto state = window_state(target);
        if (state->closed) return;
        {
            std::lock_guard lock(mutex);
            key_window = target; main_window = target;
        }
        state->key = state->main = true;
        bring_to_front(target);
        // The initial first responder takes focus when the window first
        // becomes key.
        if ((!state->first_responder || state->first_responder == target) && state->initial_first_responder)
            static_cast<void>(make_first_responder(target, state->initial_first_responder));
        static_cast<void>(send_optional(target, "becomeKeyWindow"));
        post(note::window_did_become_key, target);
        post(note::window_did_become_main, target);
        update_text_input(target);
        return;
    }
    {
        std::lock_guard lock(mutex);
        if (deferred_key_window == target) deferred_key_window = 0;
        if (key_window != target) return;
        key_window = 0;
        if (main_window == target) main_window = 0;
    }
    const auto state = window_state(target);
    state->key = state->main = false;
    static_cast<void>(send_optional(target, "resignKeyWindow"));
    post(note::window_did_resign_key, target);
    post(note::window_did_resign_main, target);
    update_text_input(target);
}

// ---- target / action ------------------------------------------------------------------
Address AppKit::target_for_action(Address action, Address target) {
    if (!action) return 0;
    if (target) return objc.responds(target, action) ? target : 0;
    const auto chain = [&](Address window_object) -> Address {
        if (!window_object) return 0;
        const auto state = window_state(window_object);
        for (auto current = state->first_responder; current; current = send(current, "nextResponder").x0)
            if (objc.responds(current, action)) return current;
        if (objc.responds(window_object, action)) return window_object;
        if (state->delegate && objc.responds(state->delegate, action)) return state->delegate;
        return 0;
    };
    if (const auto found = chain(key_window)) return found;
    if (main_window != key_window) if (const auto found = chain(main_window)) return found;
    if (objc.responds(app, action)) return app;
    const auto delegate = app_state()->delegate;
    if (delegate && objc.responds(delegate, action)) return delegate;
    return 0;
}
bool AppKit::send_action(Address action, Address target, Address sender) {
    const auto receiver = target_for_action(action, target);
    if (!receiver) return false;
    CallRegs registers;
    registers.x[2] = sender;
    static_cast<void>(objc.send(receiver, action, registers));
    return true;
}

// -[NSMenu performKeyEquivalent:]: an enabled item whose key equivalent and
// modifier mask match the key press sends its action.
bool AppKit::perform_key_equivalent(Address target_menu, Address value) {
    const auto event_info = event_state(value);
    const auto pressed = lowercase(event_info->event.characters_ignoring_modifiers);
    const auto flags = event_info->event.modifier_flags & key_modifiers;
    for (const auto item : objc.host_state<MenuState>(target_menu)->items) {
        const auto entry = objc.host_state<MenuItemState>(item);
        if (entry->submenu) {
            if (send_bool(entry->submenu, "performKeyEquivalent:", {value})) return true;
            continue;
        }
        if (!entry->enabled || entry->separator || !entry->key_equivalent || !entry->action) continue;
        const auto key = text(entry->key_equivalent);
        if (key.empty()) continue;
        auto mask = entry->modifier_mask & key_modifiers;
        if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') mask |= modifier::shift;   // uppercase implies Shift
        if (lowercase(key) != pressed || mask != flags) continue;
        return send_action(entry->action, entry->target, item);
    }
    return false;
}

void AppKit::install_application() {
    const auto a = application;
    class_method(a, "sharedApplication", [this](CpuState& s, GuestMemory&) { s.x[0] = shared_application(s.x[0]); });
    method(a, "init", [this](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(mutex);
        if (app && app != s.x[0]) fail("a second NSApplication instance was initialized");
        static_cast<void>(objc.host_state<ApplicationState>(s.x[0]));
        app = s.x[0];
        // NSApp is never deallocated.
        if (const auto record = process.objects().find(app)) record->immortal = true;
        m.write64(ns_app_slot, app);
    });
    method(a, "delegate", [this](CpuState& s, GuestMemory&) { s.x[0] = app_state()->delegate; });
    method(a, "setDelegate:", [this](CpuState& s, GuestMemory&) { set_application_delegate(s.x[2]); }, "v@:@");
    method(a, "setActivationPolicy:", [this](CpuState& s, GuestMemory&) {
        const auto policy = static_cast<std::int64_t>(s.x[2]);
        if (policy < 0 || policy > 2) { s.x[0] = 0; return; }
        app_state()->activation_policy = policy; s.x[0] = 1;
    }, "B@:q");
    method(a, "activationPolicy", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(app_state()->activation_policy); }, "q@:");
    method(a, "finishLaunching", [this](CpuState&, GuestMemory&) { finish_launching(); }, "v@:");
    method(a, "isRunning", [this](CpuState& s, GuestMemory&) { return_bool(s, app_state()->running); }, "B@:");
    method(a, "run", [this](CpuState&, GuestMemory&) {
        // -stop: sent while launching (applicationDidFinishLaunching:, the
        // wxWidgets OnInit idiom) ends -run after the next event, as on macOS:
        // the flag is cleared before launching and checked after each event.
        const auto state = app_state();
        state->running = true; state->stop_requested = false;
        finish_launching();
        do {
            const auto pool = guest_call(objc_library, "_objc_autoreleasePoolPush", {});
            try {
                const auto value = next_event(UINT64_MAX, distant_future, default_mode, true);
                if (value) {
                    static_cast<void>(send_id(app, "sendEvent:", {value}));
                    release(value);
                } else {
                    // Without a window server or run loop nothing can post an
                    // event except other threads (-postEvent:atStart:).
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            } catch (...) {
                static_cast<void>(guest_call(objc_library, "_objc_autoreleasePoolPop", {pool}));
                state->running = false;
                throw;
            }
            static_cast<void>(guest_call(objc_library, "_objc_autoreleasePoolPop", {pool}));
        } while (!state->stop_requested);
        state->running = false;
    }, "v@:");
    // -stop: ends -run after the current event, as on macOS.
    method(a, "stop:", [this](CpuState&, GuestMemory&) { app_state()->stop_requested = true; }, "v@:@");
    method(a, "terminate:", [this](CpuState& s, GuestMemory&) { terminate(s.x[2]); }, "v@:@");
    method(a, "replyToApplicationShouldTerminate:", [this](CpuState& s, GuestMemory&) {
        if (!bool_arg(s, 2)) return;
        const auto state = app_state();
        post(note::will_terminate, app);
        post_block_exit(0);
    }, "v@:B");
    method(a, "nextEventMatchingMask:untilDate:inMode:dequeue:", [this](CpuState& s, GuestMemory&) {
        const auto mask = s.x[2], date = s.x[3], mode_object = s.x[4];
        const bool dequeue = bool_arg(s, 5);
        // NSDate is wall clock; the pump runs on the monotonic clock.
        double deadline = monotonic_seconds();
        if (date) {
            const auto seconds = cf.date_time(date) - CoreFoundation::absolute_time_now();
            deadline = seconds > 1e9 ? distant_future : deadline + seconds;
        }
        const auto mode = mode_object ? text(mode_object) : std::string(default_mode);
        const auto value = next_event(mask, deadline, mode, dequeue);
        if (!value) { s.x[0] = 0; return; }
        {
            std::lock_guard lock(mutex);
            assign(current_event, value);
        }
        // Dequeued events pass the queue's reference to the pool.
        s.x[0] = dequeue ? autorelease(value) : autorelease(retain(value));
    }, "@@:Q@@B");
    method(a, "discardEventsMatchingMask:beforeEvent:", [this](CpuState& s, GuestMemory&) {
        const auto mask = s.x[2], before = s.x[3];
        std::vector<Address> dropped;
        {
            std::lock_guard lock(mutex);
            for (auto it = queue.begin(); it != queue.end();) {
                if (before && *it == before) break;
                if (mask & mask_bit(event_state(*it)->event.type)) { dropped.push_back(*it); it = queue.erase(it); }
                else ++it;
            }
        }
        for (const auto value : dropped) release(value);
    }, "v@:Q@");
    method(a, "postEvent:atStart:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !is_kind(s.x[2], event)) fail("-[NSApplication postEvent:atStart:] needs an NSEvent");
        enqueue(retain(s.x[2]), bool_arg(s, 3));
    }, "v@:@B");
    method(a, "sendEvent:", [this](CpuState& s, GuestMemory&) { if (s.x[2]) dispatch_event(s.x[2]); }, "v@:@");
    method(a, "currentEvent", [this](CpuState& s, GuestMemory&) { std::lock_guard lock(mutex); s.x[0] = current_event; });
    method(a, "mainMenu", [this](CpuState& s, GuestMemory&) { s.x[0] = app_state()->main_menu; });
    method(a, "setMainMenu:", [this](CpuState& s, GuestMemory&) { assign(app_state()->main_menu, s.x[2]); }, "v@:@");
    // NSResponder's menu on the application object is the main menu (Qt's
    // Cocoa plugin installs its menu loader's menu with -[NSApp setMenu:]).
    method(a, "menu", [this](CpuState& s, GuestMemory&) { s.x[0] = app_state()->main_menu; });
    method(a, "setMenu:", [this](CpuState& s, GuestMemory&) { assign(app_state()->main_menu, s.x[2]); }, "v@:@");
    method(a, "windowsMenu", [this](CpuState& s, GuestMemory&) { s.x[0] = app_state()->windows_menu; });
    method(a, "setWindowsMenu:", [this](CpuState& s, GuestMemory&) { assign(app_state()->windows_menu, s.x[2]); }, "v@:@");
    method(a, "servicesMenu", [this](CpuState& s, GuestMemory&) { s.x[0] = app_state()->services_menu; });
    method(a, "setServicesMenu:", [this](CpuState& s, GuestMemory&) { assign(app_state()->services_menu, s.x[2]); }, "v@:@");
    method(a, "helpMenu", [this](CpuState& s, GuestMemory&) { s.x[0] = app_state()->help_menu; });
    method(a, "setHelpMenu:", [this](CpuState& s, GuestMemory&) { assign(app_state()->help_menu, s.x[2]); }, "v@:@");
    method(a, "windows", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        { std::lock_guard lock(mutex); values = window_order; }
        s.x[0] = auto_array(values);
    });
    method(a, "orderedWindows", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        {
            std::lock_guard lock(mutex);
            for (const auto value : window_order) if (window_state(value)->visible) values.push_back(value);
        }
        s.x[0] = auto_array(values);
    });
    method(a, "windowWithWindowNumber:", [this](CpuState& s, GuestMemory&) { s.x[0] = window_for_id(s.x[2]); }, "@@:q");
    method(a, "keyWindow", [this](CpuState& s, GuestMemory&) { std::lock_guard lock(mutex); s.x[0] = key_window; });
    method(a, "mainWindow", [this](CpuState& s, GuestMemory&) { std::lock_guard lock(mutex); s.x[0] = main_window; });
    method(a, "isActive", [this](CpuState& s, GuestMemory&) { return_bool(s, app_state()->active); }, "B@:");
    const auto activate = [this](CpuState&, GuestMemory&) {
        set_active(true);
        Address front{};
        { std::lock_guard lock(mutex); front = key_window ? key_window : (window_order.empty() ? 0 : window_order.front()); }
        if (front && window_state(front)->visible) window_order_front(front, true);
    };
    method(a, "activateIgnoringOtherApps:", activate, "v@:B");
    method(a, "activate", activate, "v@:");
    method(a, "deactivate", [this](CpuState&, GuestMemory&) { set_active(false); }, "v@:");
    method(a, "hide:", [this](CpuState&, GuestMemory&) {
        const auto state = app_state();
        if (state->hidden) return;
        post(note::will_hide, app);
        std::vector<Address> targets;
        { std::lock_guard lock(mutex); targets = window_order; }
        for (const auto target : targets) {
            const auto info = window_state(target);
            if (!info->visible) continue;
            info->hidden_by_application = true;
            static_cast<void>(send_id(target, "orderOut:", {0}));
        }
        state->hidden = true;
        set_active(false);
        post(note::did_hide, app);
    }, "v@:@");
    const auto unhide = [this](CpuState&, GuestMemory&) {
        const auto state = app_state();
        if (!state->hidden) return;
        post(note::will_unhide, app);
        std::vector<Address> targets;
        { std::lock_guard lock(mutex); targets = window_order; }
        for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
            const auto info = window_state(*it);
            if (!std::exchange(info->hidden_by_application, false)) continue;
            static_cast<void>(send_id(*it, "orderFront:", {0}));
        }
        state->hidden = false;
        post(note::did_unhide, app);
    };
    method(a, "unhide:", unhide, "v@:@");
    method(a, "unhideWithoutActivation", unhide, "v@:");
    method(a, "isHidden", [this](CpuState& s, GuestMemory&) { return_bool(s, app_state()->hidden); }, "B@:");
    // Other applications' windows are not Alchemy's to hide.
    method(a, "hideOtherApplications:", [](CpuState&, GuestMemory&) { unsupported("-[NSApplication hideOtherApplications:] (other processes' windows)"); }, "v@:@");
    method(a, "unhideAllApplications:", [](CpuState&, GuestMemory&) { unsupported("-[NSApplication unhideAllApplications:] (other processes' windows)"); }, "v@:@");
    // Presentation options control the Dock and menu bar, which Windows does
    // not have; they are recorded and reported back.
    method(a, "setPresentationOptions:", [this](CpuState& s, GuestMemory&) { app_state()->presentation_options = static_cast<std::int64_t>(s.x[2]); }, "v@:Q");
    method(a, "presentationOptions", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(app_state()->presentation_options); }, "Q@:");
    method(a, "currentSystemPresentationOptions", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(app_state()->presentation_options); }, "Q@:");
    method(a, "requestUserAttention:", [this](CpuState& s, GuestMemory&) {
        const auto state = app_state();
        if (state->active) { s.x[0] = 0; return; }   // no request while active
        require_window_server("-[NSApplication requestUserAttention:]").request_attention(s.x[2] == critical_request);
        s.x[0] = static_cast<Address>(++state->attention_requests);
    }, "q@:Q");
    // FlashWindowEx stops on its own once the application is activated.
    method(a, "cancelUserAttentionRequest:", [](CpuState&, GuestMemory&) {}, "v@:q");
    // The application's icon unless one was set: the named image NSApplicationIcon.
    method(a, "applicationIconImage", [this](CpuState& s, GuestMemory&) {
        const auto icon = app_state()->icon_image;
        s.x[0] = icon ? icon : send_id(image, "imageNamed:", {auto_string("NSApplicationIcon")});
    });
    method(a, "setApplicationIconImage:", [this](CpuState& s, GuestMemory&) { assign(app_state()->icon_image, s.x[2]); }, "v@:@");
    method(a, "appearance", [this](CpuState& s, GuestMemory&) { s.x[0] = app_state()->appearance; });
    method(a, "setAppearance:", [this](CpuState& s, GuestMemory&) { assign(app_state()->appearance, s.x[2]); }, "v@:@");
    method(a, "effectiveAppearance", [this](CpuState& s, GuestMemory&) {
        const auto chosen = app_state()->appearance;
        s.x[0] = chosen ? chosen : appearance_named(system_appearance_name());
    });
    method(a, "userInterfaceLayoutDirection", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");   // left to right
    method(a, "isFullKeyboardAccessEnabled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(a, "occlusionState", [this](CpuState& s, GuestMemory&) {
        bool visible{};
        { std::lock_guard lock(mutex); for (const auto value : window_order) visible = visible || window_state(value)->visible; }
        s.x[0] = visible ? 2 : 0;   // NSApplicationOcclusionStateVisible
    }, "Q@:");
    method(a, "sendAction:to:from:", [this](CpuState& s, GuestMemory&) { return_bool(s, send_action(s.x[2], s.x[3], s.x[4])); }, "B@::@@");
    method(a, "targetForAction:", [this](CpuState& s, GuestMemory&) { s.x[0] = target_for_action(s.x[2], 0); }, "@@::");
    method(a, "targetForAction:to:from:", [this](CpuState& s, GuestMemory&) { s.x[0] = target_for_action(s.x[2], s.x[3]); }, "@@::@@");
    method(a, "updateWindows", [this](CpuState&, GuestMemory&) {
        std::vector<Address> targets;
        { std::lock_guard lock(mutex); targets = window_order; }
        for (const auto target : targets) static_cast<void>(send_optional(target, "update"));
    }, "v@:");
    method(a, "setWindowsNeedUpdate:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(a, "orderFrontStandardAboutPanel:", [this](CpuState&, GuestMemory&) {
        // The standard About panel: the bundle's name and version.
        const auto bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
        const auto info = bundle ? send(bundle, "infoDictionary").x0 : 0;
        const auto lookup = [&](std::string_view key) {
            return info ? text(send_id(info, "objectForKey:", {auto_string(key)})) : std::string{};
        };
        auto name = lookup("CFBundleName");
        if (name.empty()) name = "Application";
        const auto version = lookup("CFBundleShortVersionString");
        const auto panel = alloc_init(alert);
        static_cast<void>(send_id(panel, "setMessageText:", {auto_string(name)}));
        static_cast<void>(send_id(panel, "setInformativeText:", {auto_string(version.empty() ? "" : "Version " + version)}));
        static_cast<void>(send(panel, "runModal"));
        release(panel);
    }, "v@:@");
}

} // namespace alchemy::gui::appkit
