#include "alchemy/darwin/appkit/appkit_internal.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <cstdio>
#include <mutex>

#include "alchemy/host/ui.hpp"

// NSAlert: -runModal shows the host's alert (host::ui::show_alert; on win32 a
// task dialog, TaskDialogIndirect, comctl32 v6) with the alert's own button titles in AppKit's order and returns
// NSAlertFirstButtonReturn + index. ALCHEMY_GUI_AUTODISMISS_ALERTS=<index>
// answers every alert with that button without showing anything (unattended
// runs) and logs the alert to stderr.
//
// Key state follows AppKit: the alert panel takes key status from the key
// window for the duration of the modal session and gives it back after
// (-resignKeyWindow / -becomeKeyWindow, the key notifications), whatever
// order the window server reports the activation changes in.
//
// Accessory views: an empty plain NSView is the layout spacer AppKit
// applications use to widen the alert (Blender sets one); its width sizes
// the dialog. Accessory views with content are not provided (explicit
// failure).
namespace alchemy::gui::appkit {
namespace {
// NSAlertStyle
constexpr std::uint64_t alert_warning = 0, alert_critical = 2;   // NSAlertStyleInformational = 1
constexpr Address first_button = 1000;   // NSAlertFirstButtonReturn
constexpr std::string_view autodismiss_variable = "ALCHEMY_GUI_AUTODISMISS_ALERTS";

std::mutex presenter_mutex;
AlertPresenter presenter_override;

std::optional<std::size_t> autodismiss_choice() {
    const auto value = host_environment(autodismiss_variable.data());
    if (value.empty()) return std::nullopt;
    std::size_t index{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), index);
    if (error != std::errc{} || end != value.data() + value.size())
        fail(std::string(autodismiss_variable) + " must be a button index (0 = the first button), not \"" + value + "\"");
    return index;
}

std::string one_line(std::string text) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

} // namespace

void set_alert_presenter_override(AlertPresenter presenter) {
    std::lock_guard lock(presenter_mutex);
    presenter_override = std::move(presenter);
}

std::size_t run_task_dialog(const AlertRequest& request, const std::function<void(void*)>& created) {
    if (request.buttons.empty()) fail("an NSAlert task dialog needs a button");
    host::ui::AlertDialog alert;
    alert.caption = request.title;
    alert.instruction = request.message;
    alert.content = request.informative;
    alert.buttons = request.buttons;
    // AppKit gives a button titled "Cancel" the Escape key equivalent (the
    // last one when several are).
    for (std::size_t index = request.buttons.size(); index-- > 0 && !alert.cancel_button;)
        if (request.buttons[index] == "Cancel") alert.cancel_button = index;
    alert.icon = request.style == alert_critical ? host::ui::AlertIcon::error
               : request.style == alert_warning ? host::ui::AlertIcon::warning : host::ui::AlertIcon::information;
    alert.width = request.width;
    alert.owner = reinterpret_cast<std::uintptr_t>(request.owner);
    // Unattended and test runs (the window server's no-activate mode): the
    // dialog does not take the foreground from other applications.
    if (const auto no_activate = host_environment("ALCHEMY_GUI_NO_ACTIVATE"); !no_activate.empty() && no_activate != "0")
        alert.no_foreground = true;
    alert.verification = request.suppression;
    alert.verification_checked = request.suppression_checked;
    if (created) alert.created = [&created](std::uintptr_t dialog) { created(reinterpret_cast<void*>(dialog)); };
    const auto answer = host::ui::show_alert(alert);
    if (!answer) {
        if (answer.error() == host::Error::unsupported) unsupported("-[NSAlert runModal] without the host's alert dialog");
        fail("-[NSAlert runModal]: the host's alert dialog failed (" + std::string(host::name(answer.error())) + ")");
    }
    if (request.suppression_result) *request.suppression_result = answer->verification_checked;
    return answer->button;
}

void AppKit::install_alert() {
    const auto al = alert;
    const auto alert_of = [this](Address value) { return objc.host_state<AlertState>(value); };
    method(al, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<AlertState>(s.x[0])) {
            release(std::exchange(state->accessory, 0));
            release(std::exchange(state->suppression_button, 0));
            release(std::exchange(state->icon, 0));
            for (const auto made : std::exchange(state->button_objects, {})) release(made);
        }
        static_cast<void>(objc.send_super(s.x[0], alert, sel("dealloc")));
    }, "v@:");
    method(al, "messageText", [this, alert_of](CpuState& s, GuestMemory&) { s.x[0] = auto_string(alert_of(s.x[0])->message); });
    method(al, "setMessageText:", [this, alert_of](CpuState& s, GuestMemory&) { alert_of(s.x[0])->message = text(s.x[2]); }, "v@:@");
    method(al, "informativeText", [this, alert_of](CpuState& s, GuestMemory&) { s.x[0] = auto_string(alert_of(s.x[0])->informative); });
    method(al, "setInformativeText:", [this, alert_of](CpuState& s, GuestMemory&) { alert_of(s.x[0])->informative = text(s.x[2]); }, "v@:@");
    method(al, "alertStyle", [alert_of](CpuState& s, GuestMemory&) { s.x[0] = alert_of(s.x[0])->style; }, "Q@:");
    method(al, "setAlertStyle:", [alert_of](CpuState& s, GuestMemory&) {
        if (s.x[2] > alert_critical) fail("-[NSAlert setAlertStyle:] with an unknown style " + std::to_string(s.x[2]));
        alert_of(s.x[0])->style = s.x[2];
    }, "v@:Q");
    // Each button is an NSButton tagged with its response
    // (NSAlertFirstButtonReturn + index); the first answers Return and a
    // "Cancel" button Escape. The task dialog shows the buttons' titles.
    method(al, "addButtonWithTitle:", [this, alert_of](CpuState& s, GuestMemory&) {
        const auto state = alert_of(s.x[0]);
        const auto index = state->button_objects.size();
        const auto made = send(button, "alloc").x0;
        CallRegs frame;
        put_rect(frame, 0, {{0, 0}, {82, 32}});
        const auto value = send(made, "initWithFrame:", frame).x0;
        static_cast<void>(send_id(value, "setTitle:", {s.x[2]}));
        static_cast<void>(send_id(value, "setTag:", {static_cast<Address>(first_button + index)}));
        const auto title = text(s.x[2]);
        if (index == 0) static_cast<void>(send_id(value, "setKeyEquivalent:", {auto_string("\r")}));
        else if (title == "Cancel") static_cast<void>(send_id(value, "setKeyEquivalent:", {auto_string("\x1b")}));
        state->buttons.push_back(title);
        state->button_objects.push_back(value);
        s.x[0] = value;
    });
    method(al, "buttons", [this, alert_of](CpuState& s, GuestMemory&) { s.x[0] = auto_array(alert_of(s.x[0])->button_objects); });
    method(al, "icon", [this, alert_of](CpuState& s, GuestMemory&) {
        const auto value = alert_of(s.x[0])->icon;
        s.x[0] = value ? value : send_id(image, "imageNamed:", {auto_string("NSApplicationIcon")});
    });
    method(al, "setIcon:", [this, alert_of](CpuState& s, GuestMemory&) { assign(alert_of(s.x[0])->icon, s.x[2]); }, "v@:@");
    method(al, "accessoryView", [alert_of](CpuState& s, GuestMemory&) { s.x[0] = alert_of(s.x[0])->accessory; });
    method(al, "setAccessoryView:", [this, alert_of](CpuState& s, GuestMemory&) { assign(alert_of(s.x[0])->accessory, s.x[2]); }, "v@:@");
    // The help button needs a help anchor or delegate; Windows has neither.
    method(al, "showsHelp", [alert_of](CpuState& s, GuestMemory&) { return_bool(s, alert_of(s.x[0])->shows_help); }, "B@:");
    method(al, "setShowsHelp:", [alert_of](CpuState& s, GuestMemory&) {
        if (bool_arg(s, 2)) unsupported("-[NSAlert setShowsHelp:YES] (help anchors)");
        alert_of(s.x[0])->shows_help = false;
    }, "v@:B");
    // The panel exists only while the task dialog is up.
    method(al, "window", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(al, "layout", [](CpuState&, GuestMemory&) {}, "v@:");   // the task dialog lays itself out
    // The suppression checkbox ("Do not show this message again"): the task
    // dialog's verification check box; its state lands in the button.
    method(al, "showsSuppressionButton", [alert_of](CpuState& s, GuestMemory&) { return_bool(s, alert_of(s.x[0])->shows_suppression); }, "B@:");
    method(al, "setShowsSuppressionButton:", [alert_of](CpuState& s, GuestMemory&) { alert_of(s.x[0])->shows_suppression = bool_arg(s, 2); }, "v@:B");
    method(al, "suppressionButton", [this, alert_of](CpuState& s, GuestMemory&) {
        const auto state = alert_of(s.x[0]);
        if (!state->suppression_button) {
            const auto made = send(button, "alloc").x0;
            CallRegs frame;
            put_rect(frame, 0, {{0, 0}, {200, 18}});
            state->suppression_button = send(made, "initWithFrame:", frame).x0;
            static_cast<void>(send_id(state->suppression_button, "setButtonType:", {3}));   // NSButtonTypeSwitch
            static_cast<void>(send_id(state->suppression_button, "setTitle:", {auto_string("Do not show this message again")}));
        }
        s.x[0] = state->suppression_button;
    });
    // +alertWithError:: the error's description, recovery suggestion and
    // recovery options (else one OK button).
    class_method(al, "alertWithError:", [this](CpuState& s, GuestMemory&) {
        const auto error = s.x[2];
        if (!error) fail("+[NSAlert alertWithError:] with nil");
        const auto made = autorelease(alloc_init(alert));
        const auto state = objc.host_state<AlertState>(made);
        state->message = text(send(error, "localizedDescription").x0);
        if (const auto suggestion = send(error, "localizedRecoverySuggestion").x0) state->informative = text(suggestion);
        if (const auto options = send(error, "localizedRecoveryOptions").x0)
            for (const auto option : items(options)) static_cast<void>(send_id(made, "addButtonWithTitle:", {option}));
        state->style = alert_warning;
        s.x[0] = made;
    });
    // A sheet on `window`: the alert is shown (application-modal task dialog
    // owned by the window) once the current event finishes, then `done`
    // gets the response, as an asynchronous sheet does. `cleanup` runs once
    // either way.
    const auto run_as_sheet = [this](Address alert_object, std::string_view operation, std::function<void(Address)> done,
                                     std::function<void()> cleanup) {
        auto* loop = run_loop_or_null();
        if (!loop) { cleanup(); fail(std::string(operation) + " needs the main run loop"); }
        const auto self = retain(alert_object);
        auto source = std::make_shared<SourceId>();
        *source = loop->add_source(common_modes, [this, self, source, loop, done = std::move(done), cleanup = std::move(cleanup)] {
            loop->remove_source(*source);
            try {
                const auto response = send(self, "runModal").x0;
                release(self);
                done(response);
            } catch (...) {
                release(self);
                cleanup();
                throw;
            }
            cleanup();
        });
        loop->signal_source(*source);
    };
    method(al, "beginSheetModalForWindow:completionHandler:", [this, run_as_sheet](CpuState& s, GuestMemory&) {
        const auto handler = s.x[3] ? guest_call(libsystem_library, "__Block_copy", {s.x[3]}) : 0;
        run_as_sheet(s.x[0], "-[NSAlert beginSheetModalForWindow:completionHandler:]",
            [this, handler](Address response) { if (handler) static_cast<void>(invoke_block(handler, {response})); },
            [this, handler] { if (handler) static_cast<void>(guest_call(libsystem_library, "__Block_release", {handler})); });
    }, "v@:@@?");
    // The delegate form: -alertDidEnd:returnCode:contextInfo: (any selector
    // of that shape) goes to the delegate with the alert and the response.
    method(al, "beginSheetModalForWindow:modalDelegate:didEndSelector:contextInfo:", [this, run_as_sheet](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], delegate = s.x[3], selector = s.x[4], context = s.x[5];
        run_as_sheet(self, "-[NSAlert beginSheetModalForWindow:modalDelegate:didEndSelector:contextInfo:]",
            [this, self, delegate, selector, context](Address response) {
                if (!delegate || !selector) return;
                CallRegs registers;
                registers.x[2] = self;
                registers.x[3] = response;
                registers.x[4] = context;
                static_cast<void>(objc.send(delegate, selector, registers));
            }, [] {});
    }, "v@:@@:^v");

    method(al, "runModal", [this, alert_of](CpuState& s, GuestMemory&) {
        const auto state = alert_of(s.x[0]);
        AlertRequest request;
        request.message = state->message;
        request.informative = state->informative;
        request.buttons = state->buttons;
        for (std::size_t index = 0; index < state->button_objects.size() && index < request.buttons.size(); ++index)
            request.buttons[index] = text(send(state->button_objects[index], "title").x0);
        if (request.buttons.empty()) request.buttons.emplace_back("OK");   // AppKit adds "OK"
        request.style = state->style;
        bool suppressed{};
        if (state->shows_suppression) {
            const auto check = send(s.x[0], "suppressionButton").x0;
            request.suppression = text(send(check, "title").x0);
            request.suppression_checked = send(check, "state").x0 == 1;
            suppressed = request.suppression_checked;
            request.suppression_result = &suppressed;
        }
        const auto settle_suppression = [&] {
            if (state->shows_suppression && state->suppression_button)
                static_cast<void>(send_id(state->suppression_button, "setState:", {suppressed ? 1u : 0u}));
        };
        if (const auto accessory = state->accessory) {
            // Only the empty spacer view is understood.
            const auto info = is_kind(accessory, view) ? view_state(accessory) : nullptr;
            if (!info || objc.object_class(accessory) != view || !info->subviews.empty() || info->layer)
                unsupported("NSAlert accessory views with content (only an empty NSView sizing the alert is provided)");
            request.width = info->frame.size.width;
            note_trace("NSAlert accessory view is an empty spacer " + std::to_string(info->frame.size.width) + " points wide");
        }
        {
            const auto bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
            const auto info = bundle ? send(bundle, "infoDictionary").x0 : 0;
            if (info) request.title = text(send_id(info, "objectForKey:", {auto_string("CFBundleName")}));
        }

        if (const auto choice = autodismiss_choice()) {
            if (*choice >= request.buttons.size())
                fail(std::string(autodismiss_variable) + "=" + std::to_string(*choice) + " but the NSAlert \"" +
                     one_line(request.message) + "\" has " + std::to_string(request.buttons.size()) + " button(s)");
            std::string buttons;
            for (std::size_t index = 0; index < request.buttons.size(); ++index)
                buttons += (index ? ", " : "") + std::to_string(index) + "=\"" + request.buttons[index] + "\"";
            std::fprintf(stderr, "Alchemy AppKit: NSAlert auto-dismissed with button %zu \"%s\" (%s=%zu; buttons %s): %s | %s\n",
                         *choice, request.buttons[*choice].c_str(), autodismiss_variable.data(), *choice, buttons.c_str(),
                         one_line(request.message).c_str(), one_line(request.informative).c_str());
            std::fflush(stderr);
            s.x[0] = first_button + *choice;
            return;
        }

        AlertPresenter presenter;
        {
            std::lock_guard lock(presenter_mutex);
            presenter = presenter_override;
        }
        if (!presenter) {
            // Cloaked runs show no windows: the task dialog would be the one
            // visible window (as NSSavePanel / NSOpenPanel, panels.cpp).
            if (const auto cloak = host_environment("ALCHEMY_GUI_CLOAK"); !cloak.empty() && cloak != "0")
                unsupported("-[NSAlert runModal] in a cloaked run (ALCHEMY_GUI_CLOAK: the alert would be visible; " +
                            std::string(autodismiss_variable) + " answers it): " + one_line(request.message));
            presenter = [](const AlertRequest& value) { return run_task_dialog(value); };
        }

        // The modal session: the alert panel is key; the previous key window
        // gets key status back afterwards.
        Address previous{};
        {
            std::lock_guard lock(mutex);
            // During launch activation the window about to become key owns the alert.
            previous = key_window ? key_window : deferred_key_window;
        }
        previous = retain(previous);
        const auto server = window_server_or_null();
        if (previous && server && window_state(previous)->id)
            request.owner = server->native_handle(window_state(previous)->id);
        const auto restore = [&] {
            if (!previous) return;
            const auto info = window_state(previous);
            if (!info->closed && info->id && info->visible) {
                if (server && server->key_window() != info->id) server->show(info->id, ShowCommand::make_key_and_order_front);
                set_key_window(previous, true);
            }
            release(std::exchange(previous, 0));
        };
        // Application-modal: input for every window is discarded
        // (handle_window_event) and, on Win32, the windows other than the
        // owner are disabled, until the alert returns. The run loop keeps
        // running NSModalPanelRunLoopMode steps inside the dialog loop.
        std::vector<std::uintptr_t> handles;
        if (server) {
            std::vector<WindowId> ids;
            {
                std::lock_guard lock(mutex);
                for (const auto& [id, object] : windows) ids.push_back(id);
            }
            for (const auto id : ids) handles.push_back(reinterpret_cast<std::uintptr_t>(server->native_handle(id)));
        }
        const auto disabled = host::ui::disable_windows(handles, reinterpret_cast<std::uintptr_t>(request.owner));
        const auto end_session = [&] {
            --modal_depth;
            host::ui::enable_windows(disabled);
        };
        std::size_t chosen{};
        ++modal_depth;
        try {
            if (previous) set_key_window(previous, false);
            chosen = presenter(request);
            if (chosen >= request.buttons.size()) fail("the NSAlert presenter chose a button that does not exist");
        } catch (...) {
            end_session();
            try { restore(); } catch (...) {}
            throw;
        }
        end_session();
        restore();
        settle_suppression();
        s.x[0] = first_button + chosen;
    }, "q@:");
}

} // namespace alchemy::gui::appkit
