#include "alchemy/darwin/appkit/appkit_internal.hpp"

#include <algorithm>
#include <memory>
#include <mutex>

// NSWindowController and NSViewController: nib-backed controllers (QLab's
// and Sparkle's windows, wx-independent Cocoa code). The window or view is
// loaded from the controller's nib on first use with the controller as the
// File's Owner (the nib connects the `window` / `view` outlet); the
// controller keeps the nib's top-level objects. A controller without a nib
// and without a window / view fails explicitly when one is needed, as
// AppKit raises.
namespace alchemy::gui::appkit {
namespace {
std::string name_of_class(AppKit& kit, Address cls) {
    const auto pointer = kit.guest_call("/usr/lib/libobjc.A.dylib", "_class_getName", {cls});
    if (!pointer) return {};
    bool terminated{};
    const auto bytes = kit.memory().read_string(pointer, 1, 4096, &terminated);
    return terminated ? std::string(bytes.begin(), bytes.end()) : std::string();
}
} // namespace

void AppKit::install_controllers() {
    window_controller = declare_class("NSWindowController", "NSResponder");
    view_controller = declare_class("NSViewController", "NSResponder");
    const auto cs = [this](Address value) { return objc.host_state<ControllerState>(value); };
    const auto release_all = [this](ControllerState& state) {
        for (auto* slot : {&state.window, &state.view, &state.nib_name, &state.nib_bundle, &state.title, &state.represented,
                 &state.autosave_name, &state.identifier, &state.content_view_controller})
            release(std::exchange(*slot, 0));
        for (const auto child : std::exchange(state.children, {})) release(child);
        for (const auto object : std::exchange(state.top_level, {})) release(object);
        for (const auto controller : std::exchange(state.presented, {})) release(controller);
        release(std::exchange(state.presentation_object, 0));
    };
    // Loads nib `name` from `bundle` with `owner` as File's Owner, keeping
    // its top-level objects in the controller.
    const auto load = [this, cs](Address self, Address owner) {
        const auto state = cs(self);
        if (!state->nib_name) return false;
        const auto name = text(state->nib_name);
        const auto bytes = read_nib_file(state->nib_bundle, name);
        if (!bytes) fail("nib '" + name + "' was not found in the controller's bundle");
        state->loading = true;
        std::vector<Address> objects;
        try { objects = load_nib(*bytes, owner ? owner : self); } catch (...) { cs(self)->loading = false; throw; }
        const auto after = cs(self);
        after->loading = false;
        after->top_level.insert(after->top_level.end(), objects.begin(), objects.end());
        return true;
    };

    // ---- NSWindowController ----
    const auto wc = window_controller;
    method(wc, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithWindow:", {0}); });
    method(wc, "initWithWindow:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], window_controller, sel("init")).x0;
        s.x[0] = self;
        static_cast<void>(cs(self));
        if (s.x[2]) static_cast<void>(send_id(self, "setWindow:", {s.x[2]}));
    });
    method(wc, "initWithWindowNibName:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = send_id(s.x[0], "initWithWindowNibName:owner:", {s.x[2], s.x[0]});
    });
    method(wc, "initWithWindowNibName:owner:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = send_id(s.x[0], "initWithWindow:", {0});
        s.x[0] = self;
        const auto state = cs(self);
        release(std::exchange(state->nib_name, s.x[2] ? cf.copy(s.x[2]) : 0));
        state->owner = s.x[3] == self ? 0 : s.x[3];
    });
    method(wc, "initWithCoder:", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithWindow:", {0}); });
    method(wc, "dealloc", [this, release_all](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ControllerState>(s.x[0])) {
            if (state->window && objc.find_host_state<WindowState>(state->window) &&
                window_state(state->window)->window_controller == s.x[0])
                window_state(state->window)->window_controller = 0;
            release_all(*state);
        }
        static_cast<void>(objc.send_super(s.x[0], window_controller, sel("dealloc")));
    }, "v@:");
    method(wc, "windowNibName", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->nib_name; });
    method(wc, "owner", [cs](CpuState& s, GuestMemory&) { const auto state = cs(s.x[0]); s.x[0] = state->owner ? state->owner : s.x[0]; });
    method(wc, "isWindowLoaded", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->window != 0); }, "B@:");
    method(wc, "setWindow:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        if (state->window == s.x[2]) return;
        if (state->window && window_state(state->window)->window_controller == self) window_state(state->window)->window_controller = 0;
        assign(state->window, s.x[2]);
        if (s.x[2]) {
            window_state(s.x[2])->window_controller = self;
            if (!send(s.x[2], "nextResponder").x0) static_cast<void>(send_id(s.x[2], "setNextResponder:", {self}));
            if (const auto pending = state->content_view_controller)
                static_cast<void>(send_id(s.x[2], "setContentViewController:", {pending}));
        }
    }, "v@:@");
    // -window loads the nib (-windowWillLoad, -loadWindow, -windowDidLoad).
    method(wc, "window", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (!cs(self)->window && cs(self)->nib_name && !cs(self)->loading) {
            static_cast<void>(send(self, "windowWillLoad"));
            static_cast<void>(send(self, "loadWindow"));
            if (cs(self)->window) {
                static_cast<void>(send(self, "windowDidLoad"));
                if (cs(self)->document) static_cast<void>(send(self, "synchronizeWindowTitleWithDocumentName"));
            }
        }
        s.x[0] = cs(self)->window;
    });
    method(wc, "loadWindow", [this, load, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (cs(self)->window) return;
        const auto document = cs(self)->document;
        const bool document_owned = document && cs(self)->owner == document;
        if (document_owned) send_id(document, "windowControllerWillLoadNib:", {self});
        static_cast<void>(load(self, cs(self)->owner));
        if (document_owned) send_id(document, "windowControllerDidLoadNib:", {self});
    }, "v@:");
    method(wc, "windowWillLoad", [](CpuState&, GuestMemory&) {}, "v@:");   // override points
    method(wc, "windowDidLoad", [](CpuState&, GuestMemory&) {}, "v@:");
    method(wc, "showWindow:", [this](CpuState& s, GuestMemory&) {
        const auto target = send(s.x[0], "window").x0;
        if (!target) fail("-[NSWindowController showWindow:] without a window");
        static_cast<void>(send_id(target, "makeKeyAndOrderFront:", {s.x[2]}));
    }, "v@:@");
    method(wc, "close", [this, cs](CpuState& s, GuestMemory&) {
        if (const auto target = cs(s.x[0])->window) static_cast<void>(send(target, "close"));
    }, "v@:");
    method(wc, "document", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->document; });
    method(wc, "setDocument:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->document = s.x[2];
        if (s.x[2] && cs(s.x[0])->window) static_cast<void>(send(s.x[0], "synchronizeWindowTitleWithDocumentName"));
    }, "v@:@");
    method(wc, "windowFrameAutosaveName", [this, cs](CpuState& s, GuestMemory&) {
        const auto name = cs(s.x[0])->autosave_name;
        s.x[0] = name ? name : auto_string("");
    });
    method(wc, "setWindowFrameAutosaveName:", [this, cs](CpuState& s, GuestMemory&) {
        release(std::exchange(cs(s.x[0])->autosave_name, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(wc, "shouldCascadeWindows", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->should_cascade); }, "B@:");
    method(wc, "setShouldCascadeWindows:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->should_cascade = bool_arg(s, 2); }, "v@:B");
    method(wc, "shouldCloseDocument", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->should_close_document); }, "B@:");
    method(wc, "setShouldCloseDocument:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->should_close_document = bool_arg(s, 2); }, "v@:B");
    method(wc, "windowTitleForDocumentDisplayName:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2]; });
    // The window's title from the document's display name.
    method(wc, "synchronizeWindowTitleWithDocumentName", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        if (!state->document || !state->window) return;
        const auto name = send(state->document, "displayName").x0;
        const auto title = send_id(s.x[0], "windowTitleForDocumentDisplayName:", {name});
        if (title) send_id(state->window, "setTitle:", {title});
    }, "v@:");
    // The content view controller is the window's (kept until the window exists).
    method(wc, "contentViewController", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        s.x[0] = state->window ? send(state->window, "contentViewController").x0 : state->content_view_controller;
    });
    method(wc, "setContentViewController:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        assign(state->content_view_controller, s.x[2]);
        if (state->window) static_cast<void>(send_id(state->window, "setContentViewController:", {s.x[2]}));
    }, "v@:@");
    method(wc, "setDocumentEdited:", [this, cs](CpuState& s, GuestMemory&) {
        if (const auto target = cs(s.x[0])->window) static_cast<void>(send_id(target, "setDocumentEdited:", {s.x[2] & 0xff}));
    }, "v@:B");
    method(wc, "dismissController:", [this](CpuState& s, GuestMemory&) { static_cast<void>(send(s.x[0], "close")); }, "v@:@");

    // ---- NSViewController ----
    const auto vc = view_controller;
    method(vc, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithNibName:bundle:", {0, 0}); });
    method(vc, "initWithNibName:bundle:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], view_controller, sel("init")).x0;
        s.x[0] = self;
        const auto state = cs(self);
        release(std::exchange(state->nib_name, s.x[2] ? cf.copy(s.x[2]) : 0));
        assign(state->nib_bundle, s.x[3]);
    });
    method(vc, "initWithCoder:", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithNibName:bundle:", {0, 0}); });
    method(vc, "dealloc", [this, release_all](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ControllerState>(s.x[0])) release_all(*state);
        static_cast<void>(objc.send_super(s.x[0], view_controller, sel("dealloc")));
    }, "v@:");
    method(vc, "nibName", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->nib_name; });
    method(vc, "nibBundle", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->nib_bundle; });
    method(vc, "isViewLoaded", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->view != 0); }, "B@:");
    method(vc, "setView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        assign(cs(self)->view, s.x[2]);
        if (s.x[2] && !send(s.x[2], "nextResponder").x0) static_cast<void>(send_id(s.x[2], "setNextResponder:", {self}));
    }, "v@:@");
    method(vc, "view", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (!cs(self)->view && !cs(self)->loading) {
            static_cast<void>(send(self, "loadView"));
            if (cs(self)->view) static_cast<void>(send(self, "viewDidLoad"));
        }
        s.x[0] = cs(self)->view;
    });
    method(vc, "viewIfLoaded", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->view; });
    // -loadView: the nib named by -nibName, else the nib named like the
    // controller's class; no nib is an error (AppKit raises).
    method(vc, "loadView", [this, cs, load](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto named = send(self, "nibName").x0;
        if (named && named != cs(self)->nib_name) release(std::exchange(cs(self)->nib_name, cf.copy(named)));
        if (!cs(self)->nib_name) {
            const auto class_name = name_of_class(*this, objc.object_class(self));
            if (!class_name.empty() && read_nib_file(cs(self)->nib_bundle, class_name)) cs(self)->nib_name = string(class_name);
        }
        if (!cs(self)->nib_name) fail("-[NSViewController loadView] could not load the nil nib (no view and no nib)");
        static_cast<void>(load(self, 0));
        if (!cs(self)->view) fail("-[NSViewController loadView] loaded nib '" + text(cs(self)->nib_name) + "' but the view outlet was not set");
    }, "v@:");
    for (const auto* name : {"viewDidLoad", "viewWillAppear", "viewDidAppear", "viewWillDisappear", "viewDidDisappear",
             "viewWillLayout", "viewDidLayout", "updateViewConstraints"})
        method(vc, name, [](CpuState&, GuestMemory&) {}, "v@:");   // override points
    method(vc, "representedObject", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->represented; });
    method(vc, "setRepresentedObject:", [this, cs](CpuState& s, GuestMemory&) { assign(cs(s.x[0])->represented, s.x[2]); }, "v@:@");
    // NSUserInterfaceItemIdentification.
    method(vc, "identifier", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->identifier; });
    method(vc, "setIdentifier:", [this, cs](CpuState& s, GuestMemory&) {
        release(std::exchange(cs(s.x[0])->identifier, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(vc, "title", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->title; });
    method(vc, "setTitle:", [this, cs](CpuState& s, GuestMemory&) {
        release(std::exchange(cs(s.x[0])->title, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(vc, "preferredContentSize", [cs](CpuState& s, GuestMemory&) { return_size(s, cs(s.x[0])->preferred_content_size); }, "{CGSize=dd}@:");
    method(vc, "setPreferredContentSize:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->preferred_content_size = size_arg(s); }, "v@:{CGSize=dd}");
    method(vc, "childViewControllers", [this, cs](CpuState& s, GuestMemory&) { s.x[0] = auto_array(cs(s.x[0])->children); });
    method(vc, "parentViewController", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->parent; });
    method(vc, "addChildViewController:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], child = s.x[2];
        if (!child || child == self) return;
        if (const auto previous = cs(child)->parent) {
            if (previous == self) return;
            static_cast<void>(send(child, "removeFromParentViewController"));
        }
        cs(self)->children.push_back(retain(child));
        cs(child)->parent = self;
    }, "v@:@");
    method(vc, "insertChildViewController:atIndex:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], child = s.x[2];
        if (!child) return;
        if (cs(child)->parent) static_cast<void>(send(child, "removeFromParentViewController"));
        auto& list = cs(self)->children;
        if (s.x[3] > list.size()) fail("-[NSViewController insertChildViewController:atIndex:] out of range");
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(s.x[3]), retain(child));
        cs(child)->parent = self;
    }, "v@:@q");
    method(vc, "removeFromParentViewController", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto parent = std::exchange(cs(self)->parent, 0);
        if (!parent) return;
        auto& list = cs(parent)->children;
        const auto found = std::find(list.begin(), list.end(), self);
        if (found == list.end()) return;
        list.erase(found);
        release(self);
    }, "v@:");
    method(vc, "removeChildViewControllerAtIndex:", [this, cs](CpuState& s, GuestMemory&) {
        const auto& list = cs(s.x[0])->children;
        if (s.x[2] >= list.size()) fail("-[NSViewController removeChildViewControllerAtIndex:] out of range");
        static_cast<void>(send(list[s.x[2]], "removeFromParentViewController"));
    }, "v@:q");
    // A presented controller is dismissed by its presenter; any other closes its view's window.
    method(vc, "dismissController:", [this, cs](CpuState& s, GuestMemory&) {
        if (const auto presenter = cs(s.x[0])->presenting) {
            static_cast<void>(send_id(presenter, "dismissViewController:", {s.x[0]}));
            return;
        }
        const auto target = send(s.x[0], "view").x0;
        const auto owner = target ? view_state(target)->window : 0;
        if (owner) static_cast<void>(send(owner, "close"));
    }, "v@:@");

    // ---- presentation ----
    // Sheets and modal windows show the controller's view as a window's
    // content (its preferred content size, else the view's size); a modal
    // window runs a modal session (input goes to it alone) without blocking
    // the caller; popovers are NSPopovers; an animator object presents and
    // dismisses through its own -animatePresentationOfViewController:... calls.
    const auto window_for = [this, cs](Address presented, std::uint64_t style) {
        const auto content = send(presented, "view").x0;
        if (!content) fail("presenting a view controller without a view");
        auto size = cs(presented)->preferred_content_size;
        if (!(size.width > 0 && size.height > 0)) size = view_state(content)->frame.size;
        CallRegs init;
        put_rect(init, 0, {{0, 0}, size});
        init.x[2] = style;
        init.x[3] = 2;   // NSBackingStoreBuffered
        init.x[4] = 0;
        const auto made = send(send(window, "alloc").x0, "initWithContentRect:styleMask:backing:defer:", init).x0;
        static_cast<void>(send_id(made, "setReleasedWhenClosed:", {0}));
        if (responds(made, "setContentViewController:")) static_cast<void>(send_id(made, "setContentViewController:", {presented}));
        else static_cast<void>(send_id(made, "setContentView:", {content}));
        if (const auto title = send_id(presented, "title")) static_cast<void>(send_id(made, "setTitle:", {title}));
        return made;   // +1
    };
    const auto record = [this, cs](Address presenter, Address presented, ControllerState::Presentation how, Address object, Address session) {
        if (const auto previous = cs(presented)->presenting; previous && previous != presenter)
            fail("-[NSViewController present...] a view controller that is already presented");
        cs(presenter)->presented.push_back(retain(presented));
        const auto state = cs(presented);
        state->presenting = presenter;
        state->presentation = how;
        release(std::exchange(state->presentation_object, object));
        state->modal_session = session;
    };
    method(vc, "presentViewControllerAsSheet:", [this, window_for, record](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], presented = s.x[2];
        const auto parent_view = send(self, "view").x0;
        const auto parent = parent_view ? view_state(parent_view)->window : 0;
        if (!parent) fail("-[NSViewController presentViewControllerAsSheet:] from a view controller whose view is in no window");
        const auto sheet = window_for(presented, 1);   // titled
        record(self, presented, ControllerState::Presentation::sheet, sheet, 0);
        static_cast<void>(send_id(parent, "beginSheet:completionHandler:", {sheet, 0}));
    }, "v@:@");
    method(vc, "presentViewControllerAsModalWindow:", [this, window_for, record](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], presented = s.x[2];
        const auto modal = window_for(presented, 1 | 2);   // titled, closable
        const auto session = send_id(send_id(application, "sharedApplication"), "beginModalSessionForWindow:", {modal});
        record(self, presented, ControllerState::Presentation::modal_window, modal, session);
    }, "v@:@");
    // (viewController, positioningRect d0-d3, ofView, preferredEdge, behavior)
    method(vc, "presentViewController:asPopoverRelativeToRect:ofView:preferredEdge:behavior:", [this, record](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], presented = s.x[2];
        const auto popover_class = objc.find_class("NSPopover");
        if (!popover_class) unsupported("-[NSViewController presentViewController:asPopoverRelativeToRect:...] (no NSPopover)");
        const auto popover = alloc_init(popover_class);
        static_cast<void>(send_id(popover, "setContentViewController:", {presented}));
        static_cast<void>(send_id(popover, "setBehavior:", {s.x[5]}));
        record(self, presented, ControllerState::Presentation::popover, popover, 0);
        CallRegs show;
        put_rect(show, 0, rect_arg(s));
        show.x[2] = s.x[3];
        show.x[3] = s.x[4];
        static_cast<void>(send(popover, "showRelativeToRect:ofView:preferredEdge:", show));
    }, "v@:@{CGRect={CGPoint=dd}{CGSize=dd}}@QQ");
    method(vc, "presentViewController:animator:", [this, record](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], presented = s.x[2], animator = s.x[3];
        if (!animator) fail("-[NSViewController presentViewController:animator:] without an animator");
        record(self, presented, ControllerState::Presentation::animator, retain(animator), 0);
        static_cast<void>(send_id(animator, "animatePresentationOfViewController:fromViewController:", {presented, self}));
    }, "v@:@@");
    method(vc, "dismissViewController:", [this, cs](CpuState& s, GuestMemory&) {
        auto presenter = s.x[0];
        const auto presented = s.x[2];
        if (!presented) return;
        if (cs(presented)->presenting) presenter = cs(presented)->presenting;
        auto& list = cs(presenter)->presented;
        const auto found = std::find(list.begin(), list.end(), presented);
        if (found == list.end()) return;   // not presented (or dismissed already)
        list.erase(found);
        const auto state = cs(presented);
        const auto object = std::exchange(state->presentation_object, 0);
        const auto how = std::exchange(state->presentation, ControllerState::Presentation::none);
        const auto session = std::exchange(state->modal_session, 0);
        state->presenting = 0;
        switch (how) {
        case ControllerState::Presentation::sheet:
            if (const auto parent = send_id(object, "sheetParent")) static_cast<void>(send_id(parent, "endSheet:", {object}));
            static_cast<void>(send_id(object, "orderOut:", {0}));
            break;
        case ControllerState::Presentation::modal_window:
            if (session) static_cast<void>(send_id(send_id(application, "sharedApplication"), "endModalSession:", {session}));
            static_cast<void>(send_id(object, "orderOut:", {0}));
            break;
        case ControllerState::Presentation::popover:
            static_cast<void>(send_id(object, "close"));
            break;
        case ControllerState::Presentation::animator:
            static_cast<void>(send_id(object, "animateDismissalOfViewController:fromViewController:", {presented, presenter}));
            break;
        case ControllerState::Presentation::none: break;
        }
        release(object);
        release(presented);
    }, "v@:@");
    method(vc, "presentedViewControllers", [this, cs](CpuState& s, GuestMemory&) {
        const auto& list = cs(s.x[0])->presented;
        s.x[0] = list.empty() ? 0 : auto_array(list);
    });
    method(vc, "presentingViewController", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->presenting; });
    // A presented controller's new preferred content size resizes its sheet, window or popover.
    method(vc, "preferredContentSizeDidChangeForViewController:", [this, cs](CpuState& s, GuestMemory&) {
        const auto presented = s.x[2];
        if (!presented || cs(presented)->presenting != s.x[0]) return;
        const auto state = cs(presented);
        const auto size = state->preferred_content_size;
        if (!(size.width > 0 && size.height > 0) || !state->presentation_object) return;
        CallRegs r;
        put_size(r, 0, size);
        // The popover's and the windows' -setContentSize: alike; an animator's presentation is the animator's business.
        if (state->presentation != ControllerState::Presentation::animator) static_cast<void>(send(state->presentation_object, "setContentSize:", r));
    }, "v@:@");

    // ---- the rest of NSViewController ----
    method(vc, "loadViewIfNeeded", [this, cs](CpuState& s, GuestMemory&) {
        if (!cs(s.x[0])->view) static_cast<void>(send(s.x[0], "view"));
    }, "v@:");
    // NSEditor: no bindings editors are registered with a controller, so there is nothing to commit or discard.
    method(vc, "commitEditing", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(vc, "discardEditing", [](CpuState&, GuestMemory&) {}, "v@:");
    // The view's smallest size under its constraints (-fittingSize); no maximum beyond the screen's.
    method(vc, "preferredMinimumSize", [this, cs](CpuState& s, GuestMemory&) {
        const auto view = cs(s.x[0])->view;
        return_size(s, view ? result_size(send(view, "fittingSize")) : Size{});
    }, "{CGSize=dd}@:");
    method(vc, "preferredMaximumSize", [](CpuState& s, GuestMemory&) { return_size(s, {1e7, 1e7}); }, "{CGSize=dd}@:");
    method(vc, "setChildViewControllers:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto wanted = s.x[2] ? items(s.x[2]) : std::vector<Address>{};
        for (const auto child : wanted) static_cast<void>(retain(child));
        for (const auto child : std::vector<Address>(cs(self)->children)) static_cast<void>(send(child, "removeFromParentViewController"));
        for (const auto child : wanted) {
            static_cast<void>(send_id(self, "addChildViewController:", {child}));
            release(child);
        }
    }, "v@:@");
    method(vc, "sourceItemView", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->source_item_view; });
    method(vc, "setSourceItemView:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->source_item_view = s.x[2]; }, "v@:@");
    // Controllers are not loaded from storyboards here (NSStoryboard is not provided).
    method(vc, "storyboard", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // Swaps one child's view for another's in place (the transition options' animations are not run: the change applies at once).
    method(vc, "transitionFromViewController:toViewController:options:completionHandler:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], from = s.x[2], to = s.x[3], completion = s.x[5];
        const auto& children = cs(self)->children;
        if (std::find(children.begin(), children.end(), from) == children.end() || std::find(children.begin(), children.end(), to) == children.end())
            fail("-[NSViewController transitionFromViewController:toViewController:...] between controllers that are not both its children");
        const auto old_view = send(from, "view").x0, new_view = send(to, "view").x0;
        if (const auto parent = old_view ? view_state(old_view)->superview : 0) {
            CallRegs frame;
            put_rect(frame, 0, view_state(old_view)->frame);
            static_cast<void>(send(new_view, "setFrame:", frame));
            static_cast<void>(send_id(parent, "replaceSubview:with:", {old_view, new_view}));
        }
        if (completion) static_cast<void>(invoke_block(completion, {}));
    }, "v@:@@Q@?");
}


// NSUserDefaultsController (Cocoa bindings' defaults controller, QLab's
// nibs): a controller over an NSUserDefaults (nil: the standard defaults)
// with optional initial values. -values is a proxy whose -valueForKey: reads
// the defaults (falling back to the initial values) and whose
// -setValue:forKey: writes them inside will/didChangeValueForKey: (key-value
// observers of "values.<key>" see the change). Changes apply immediately: the controller
// never holds unapplied changes, so -save: synchronizes and -revert: has
// nothing to undo. Deferred application (setAppliesImmediately:NO) is not
// provided and fails explicitly.
void AppKit::install_user_defaults_controller() {
    if (!objc.find_class("NSController")) static_cast<void>(declare_class("NSController", "NSObject"));
    const auto udc = declare_class("NSUserDefaultsController", "NSController");
    const auto values_class = declare_class("_AlchemyUserDefaultsValues", "NSObject");
    struct DefaultsControllerState final : ObjectPayload { Address defaults{}, initial{}, values{}; };
    struct ValuesState final : ObjectPayload { Address controller{}; };   // not retained (the controller owns the proxy)
    const auto state_of = [this](Address self) { return objc.host_state<DefaultsControllerState>(self); };
    const auto standard = [this]() {
        const auto defaults_class = objc.find_class("NSUserDefaults");
        if (!defaults_class) unsupported("NSUserDefaultsController without NSUserDefaults");
        return send(defaults_class, "standardUserDefaults").x0;
    };
    const auto initialize = [this, state_of, standard](Address self, Address defaults, Address initial) {
        const auto state = state_of(self);
        release(std::exchange(state->defaults, retain(defaults ? defaults : standard())));
        release(std::exchange(state->initial, initial ? send(initial, "copy").x0 : 0));
        return self;
    };
    method(udc, "initWithDefaults:initialValues:", [this, udc, initialize](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], udc, sel("init")).x0;
        s.x[0] = self ? initialize(self, s.x[2], s.x[3]) : 0;
    }, "@@:@@");
    method(udc, "init", [this, udc, initialize](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], udc, sel("init")).x0;
        s.x[0] = self ? initialize(self, 0, 0) : 0;
    });
    struct Shared { std::mutex mutex; Address controller{}; };
    auto shared = std::make_shared<Shared>();
    const auto shared_controller = [this, udc, shared]() {
        std::lock_guard lock(shared->mutex);
        if (!shared->controller) shared->controller = alloc_init(udc);   // kept for the process
        return shared->controller;
    };
    class_method(udc, "sharedUserDefaultsController", [shared_controller](CpuState& s, GuestMemory&) { s.x[0] = shared_controller(); });
    // Nib decoding: NSSharedInstance selects the shared controller.
    method(udc, "initWithCoder:", [this, udc, initialize, shared_controller](CpuState& s, GuestMemory&) {
        const auto is_shared = send_bool(s.x[2], "decodeBoolForKey:", {auto_string("NSSharedInstance")});
        if (is_shared) {
            release(s.x[0]);
            s.x[0] = retain(shared_controller());
            return;
        }
        const auto self = objc.send_super(s.x[0], udc, sel("init")).x0;
        // Not read: NSDeclaredKeys (Interface Builder's list of bound keys) and
        // NSAppliesImmediately: changes apply immediately even when the archive
        // says NO, since deferred application (-hasUnappliedChanges, -save:,
        // -revert:) is not modelled.
        s.x[0] = self ? initialize(self, 0, 0) : 0;
    }, "@@:@");
    method(udc, "defaults", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->defaults; });
    method(udc, "initialValues", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->initial; });
    method(udc, "setInitialValues:", [this, state_of](CpuState& s, GuestMemory&) {
        release(std::exchange(state_of(s.x[0])->initial, s.x[2] ? send(s.x[2], "copy").x0 : 0));
    }, "v@:@");
    method(udc, "values", [this, state_of, values_class](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        if (!state->values) {
            state->values = alloc_init(values_class);
            objc.host_state<ValuesState>(state->values)->controller = s.x[0];
        }
        s.x[0] = state->values;
    });
    method(udc, "appliesImmediately", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(udc, "setAppliesImmediately:", [](CpuState& s, GuestMemory&) {
        if (!(s.x[2] & 0xff)) unsupported("-[NSUserDefaultsController setAppliesImmediately:NO] (deferred application)");
    }, "v@:B");
    method(udc, "hasUnappliedChanges", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(udc, "save:", [this, state_of](CpuState& s, GuestMemory&) {
        static_cast<void>(send_optional(state_of(s.x[0])->defaults, "synchronize"));
    }, "v@:@");
    method(udc, "revert:", [](CpuState&, GuestMemory&) {}, "v@:@");   // nothing unapplied: changes apply immediately
    // -revertToInitialValues: removes the keys the initial values name from
    // the defaults, so reads fall back to the initial values.
    method(udc, "revertToInitialValues:", [this, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        if (!state->initial) return;
        const auto keys = send(state->initial, "allKeys").x0;
        const auto count = send(keys, "count").x0;
        for (Address i = 0; i < count; ++i) {
            CallRegs index;
            index.x[2] = i;
            send_id(state->defaults, "removeObjectForKey:", {send(keys, "objectAtIndex:", index).x0});
        }
    }, "v@:@");
    method(udc, "dealloc", [this, udc, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        for (auto* slot : {&state->defaults, &state->initial, &state->values}) release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], udc, sel("dealloc")));
    }, "v@:");

    const auto controller_of = [this](Address values) {
        const auto controller = objc.host_state<ValuesState>(values)->controller;
        if (!controller) fail("NSUserDefaultsController values proxy without a controller");
        return objc.host_state<DefaultsControllerState>(controller);
    };
    const auto read = [this, controller_of](Address values, Address key) -> Address {
        const auto state = controller_of(values);
        if (const auto value = send_id(state->defaults, "objectForKey:", {key})) return value;
        return state->initial ? send_id(state->initial, "objectForKey:", {key}) : 0;
    };
    method(values_class, "valueForKey:", [read](CpuState& s, GuestMemory&) { s.x[0] = s.x[2] ? read(s.x[0], s.x[2]) : 0; }, "@@:@");
    method(values_class, "valueForKeyPath:", [this, read](CpuState& s, GuestMemory&) {
        const auto path = text(s.x[2]);
        const auto dot = path.find('.');
        if (dot == std::string::npos) { s.x[0] = read(s.x[0], s.x[2]); return; }
        const auto value = read(s.x[0], auto_string(path.substr(0, dot)));
        s.x[0] = value ? send_id(value, "valueForKeyPath:", {auto_string(path.substr(dot + 1))}) : 0;
    }, "@@:@");
    method(values_class, "setValue:forKey:", [this, controller_of](CpuState& s, GuestMemory&) {
        const auto state = controller_of(s.x[0]);
        static_cast<void>(send_id(s.x[0], "willChangeValueForKey:", {s.x[3]}));
        if (s.x[2]) send_id(state->defaults, "setObject:forKey:", {s.x[2], s.x[3]});
        else send_id(state->defaults, "removeObjectForKey:", {s.x[3]});
        static_cast<void>(send_id(s.x[0], "didChangeValueForKey:", {s.x[3]}));
    }, "v@:@@");
}

} // namespace alchemy::gui::appkit
