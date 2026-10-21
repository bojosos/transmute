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
                 &state.autosave_name})
            release(std::exchange(*slot, 0));
        for (const auto child : std::exchange(state.children, {})) release(child);
        for (const auto object : std::exchange(state.top_level, {})) release(object);
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
    method(wc, "contentViewController", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
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
    method(vc, "dismissController:", [this](CpuState& s, GuestMemory&) {
        const auto target = send(s.x[0], "view").x0;
        const auto owner = target ? view_state(target)->window : 0;
        if (owner) static_cast<void>(send(owner, "close"));
    }, "v@:@");
    for (const auto* name : {"presentViewControllerAsSheet:", "presentViewControllerAsModalWindow:",
             "presentViewController:asPopoverRelativeToRect:ofView:preferredEdge:behavior:"})
        method(vc, name, [selector = std::string(name)](CpuState&, GuestMemory&) {
            unsupported("-[NSViewController " + selector + "] (view controller presentation)");
        });
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
