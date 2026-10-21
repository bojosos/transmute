#include "alchemy/darwin/appkit/appkit_internal.hpp"

// NSApplication modal sessions (wxWidgets' wxDialog::ShowModal, an
// installer wizard's dialogs):
//
//   -runModalForWindow:            orders the window front, makes it key and
//                                  runs events in NSModalPanelRunLoopMode
//                                  until -stopModal / -stopModalWithCode: /
//                                  -abortModal; answers the code.
//   -beginModalSessionForWindow:   the same set-up, answering a session
//   -runModalSession:              one non-blocking pass over pending events:
//                                  NSModalResponseContinue (-1002) until the
//                                  session is stopped, then the stop code
//   -endModalSession:              ends it (the window stays where it is)
//   -stopModal (-1000), -stopModalWithCode:, -abortModal (-1001)
//   -modalWindow                   the innermost session's window
//
// While a session runs, input for the application's other windows is
// discarded (handle_window_event, modal_target), as AppKit does; window
// notifications still arrive. Sessions nest. A stop requested while no
// session runs is ignored, as on macOS (with a diagnostic note).
namespace alchemy::gui::appkit {
namespace {
constexpr std::int64_t response_stop = -1000, response_abort = -1001, response_continue = -1002;
}

struct AppKit::ModalSession {
    Address window{};          // +1
    std::int64_t code{response_continue};
    bool stopped{};
    Address previous_key{};    // +1, key window when the session began
};

Address AppKit::modal_target() const {
    return modal_sessions.empty() ? 0 : modal_sessions.back()->window;
}

bool AppKit::input_allowed_for(Address target) {
    if (modal_depth) return false;   // NSAlert (application-modal, no window of ours)
    // A window with a sheet attached takes no input (document-modal); the sheet does.
    if (target && window_state(target)->attached_sheet) return false;
    if (modal_sessions.empty()) return true;
    const auto modal = modal_sessions.back()->window;
    // The modal window, its child windows (sheets, popups) and the menu windows.
    for (auto current = target; current; current = window_state(current)->parent)
        if (current == modal) return true;
    return target && window_state(target)->level >= 101;   // NSPopUpMenuWindowLevel
}

std::shared_ptr<AppKit::ModalSession> AppKit::begin_modal(Address target) {
    if (!target || !is_kind(target, window)) fail("-[NSApplication beginModalSessionForWindow:] needs an NSWindow");
    auto session = std::make_shared<ModalSession>();
    session->window = retain(target);
    {
        std::lock_guard lock(mutex);
        session->previous_key = retain(key_window);
    }
    modal_sessions.push_back(session);
    static_cast<void>(send_id(target, "makeKeyAndOrderFront:", {0}));
    return session;
}

void AppKit::end_modal(const std::shared_ptr<ModalSession>& session) {
    const auto found = std::find(modal_sessions.begin(), modal_sessions.end(), session);
    if (found == modal_sessions.end()) fail("-[NSApplication endModalSession:] with a session that is not running");
    modal_sessions.erase(found);
    const auto previous = std::exchange(session->previous_key, 0);
    if (previous && previous != session->window) {
        const auto info = window_state(previous);
        if (!info->closed && info->visible && info->id) static_cast<void>(send_id(previous, "makeKeyWindow"));
    }
    release(previous);
    release(std::exchange(session->window, 0));
}

void AppKit::stop_modal(std::int64_t code) {
    if (modal_sessions.empty()) { note_trace("-[NSApplication stopModal] with no modal session"); return; }
    const auto& session = modal_sessions.back();
    if (session->stopped) return;
    session->stopped = true;
    session->code = code;
    if (const auto loop = run_loop_or_null()) loop->wake();
}

void AppKit::install_modal() {
    const auto a = application;
    // The session handle is an opaque pointer-sized value (NSModalSession);
    // it indexes the host table.
    const auto handle_of = [this](const std::shared_ptr<ModalSession>& session) {
        const auto value = ++modal_session_serial;
        modal_handles[value] = session;
        return static_cast<Address>(value);
    };
    const auto session_of = [this](Address handle) -> std::shared_ptr<ModalSession> {
        const auto found = modal_handles.find(handle);
        if (found == modal_handles.end()) fail("unknown NSModalSession");
        return found->second;
    };
    const auto one_event = [this](double deadline) {
        const auto pool = guest_call(objc_library, "_objc_autoreleasePoolPush", {});
        try {
            const auto value = next_event(UINT64_MAX, deadline, modal_panel_mode, true);
            if (value) {
                static_cast<void>(send_id(app, "sendEvent:", {value}));
                release(value);
            }
        } catch (...) {
            static_cast<void>(guest_call(objc_library, "_objc_autoreleasePoolPop", {pool}));
            throw;
        }
        static_cast<void>(guest_call(objc_library, "_objc_autoreleasePoolPop", {pool}));
    };
    method(a, "runModalForWindow:", [this, one_event](CpuState& s, GuestMemory&) {
        const auto session = begin_modal(s.x[2]);
        try {
            while (!session->stopped) {
                const auto info = window_state(session->window);
                if (info->closed) { session->stopped = true; session->code = response_abort; break; }
                one_event(monotonic_seconds() + 0.25);
            }
        } catch (...) { end_modal(session); throw; }
        end_modal(session);
        s.x[0] = static_cast<Address>(session->code);
    }, "q@:@");
    method(a, "beginModalSessionForWindow:", [this, handle_of](CpuState& s, GuestMemory&) {
        s.x[0] = handle_of(begin_modal(s.x[2]));
    }, "^v@:@");
    method(a, "runModalSession:", [this, session_of, one_event](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[2]);
        if (modal_sessions.empty() || modal_sessions.back() != session)
            fail("-[NSApplication runModalSession:] for a session that is not the innermost");
        // Pending events only (macOS processes what is queued and returns).
        for (int guard = 0; guard < 256 && !session->stopped; ++guard) {
            const auto pool = guest_call(objc_library, "_objc_autoreleasePoolPush", {});
            Address value{};
            try {
                value = next_event(UINT64_MAX, monotonic_seconds(), modal_panel_mode, true);
                if (value) {
                    static_cast<void>(send_id(app, "sendEvent:", {value}));
                    release(value);
                }
            } catch (...) {
                static_cast<void>(guest_call(objc_library, "_objc_autoreleasePoolPop", {pool}));
                throw;
            }
            static_cast<void>(guest_call(objc_library, "_objc_autoreleasePoolPop", {pool}));
            if (!value) break;
        }
        s.x[0] = static_cast<Address>(session->stopped ? session->code : response_continue);
    }, "q@:^v");
    method(a, "endModalSession:", [this, session_of](CpuState& s, GuestMemory&) {
        const auto session = session_of(s.x[2]);
        modal_handles.erase(s.x[2]);
        end_modal(session);
    }, "v@:^v");
    method(a, "stopModal", [this](CpuState&, GuestMemory&) { stop_modal(response_stop); }, "v@:");
    method(a, "stopModalWithCode:", [this](CpuState& s, GuestMemory&) { stop_modal(static_cast<std::int64_t>(s.x[2])); }, "v@:q");
    method(a, "abortModal", [this](CpuState&, GuestMemory&) { stop_modal(response_abort); }, "v@:");
    method(a, "modalWindow", [this](CpuState& s, GuestMemory&) { s.x[0] = modal_target(); }, "@@:");
}

} // namespace alchemy::gui::appkit
