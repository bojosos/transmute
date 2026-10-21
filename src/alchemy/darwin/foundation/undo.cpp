#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/foundation/undo_core.hpp"

#include <map>
#include <set>
#include <sstream>

// NSUndoManager (Foundation; QLab subclasses it). Stacks and grouping are
// undo_core.cpp's; this file registers and performs the actions:
//   -registerUndoWithTarget:selector:object:  sends selector(object) to the target
//   -registerUndoWithTarget:handler:           calls the block with the target
//   -prepareWithInvocationTarget:              returns a proxy (NSUndoManagerProxy)
//       that records the next message it receives as an NSInvocation.
// As documented, targets are not retained; objects passed with
// registerUndoWithTarget:selector:object:, handler blocks and invocation
// arguments are. groupsByEvent (default YES) opens a group at the first
// registration and closes it when the main run loop next services the main
// dispatch queue (the end of the current event). Notifications
// (NSUndoManagerCheckpointNotification, Will/DidUndoChange, Will/DidRedoChange,
// DidOpenUndoGroup, WillCloseUndoGroup, DidCloseUndoGroup) are posted to the
// default center with the manager as object. Misuse Foundation reports with
// NSInternalInconsistencyException fails explicitly.
//
// The proxy has no message forwarding to rely on: it is a root class that
// gains, for each target class it is prepared with, a recording method per
// instance selector of that class and its superclasses below NSObject (read
// from the runtime's class table). A selector the target's class does not
// implement fails as an unsupported message, as forwarding would.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct Registered {
    enum class Kind { selector, block, invocation } kind{};
    Address selector{}, object{}, block{}, invocation{};
};

struct ManagerState final : ObjectPayload {
    std::recursive_mutex mutex;
    undo::Stack stack;
    std::map<std::uint64_t, Registered> actions;
    std::uint64_t next{1};
    bool groups_by_event{true}, auto_group{}, close_pending{};
    Address modes{};   // +1 NSArray of run loop modes
};

struct ProxyState final : ObjectPayload {
    Address manager{}, target{};
};

struct Undo {
    explicit Undo(Support& value) : support(value) {}
    Support& support;
    Address manager_class{}, proxy_class{}, close_entry{};
    std::mutex proxy_mutex;
    std::set<std::string> proxy_selectors;           // selectors the proxy class implements
    std::set<Address> prepared_classes;              // target classes whose selectors were added

    std::shared_ptr<ManagerState> state_of(Address manager) { return support.objc.host_state<ManagerState>(manager); }

    void post(std::string_view name, Address manager, Address info = 0) { post_notification(support, name, manager, info); }

    void release(const Registered& value) {
        if (value.object) objc_release(support, value.object);
        if (value.block) block_release(support, value.block);
        if (value.invocation) objc_release(support, value.invocation);
    }
    // Takes the registrations of these actions out of the table and releases them.
    void drop(ManagerState& state, const std::vector<undo::Action>& list) {
        std::vector<Registered> gone;
        {
            std::lock_guard lock(state.mutex);
            for (const auto& action : list) {
                const auto found = state.actions.find(action.id);
                if (found == state.actions.end()) continue;
                gone.push_back(found->second);
                state.actions.erase(found);
            }
        }
        for (const auto& value : gone) release(value);
    }

    // The auto group closes at the end of the event: the main run loop's next
    // service of the main dispatch queue.
    void schedule_close(Address manager, ManagerState& state) {
        {
            std::lock_guard lock(state.mutex);
            if (state.close_pending) return;
            state.close_pending = true;
        }
        static_cast<void>(objc_retain(support, manager));
        const auto main_queue = support.process.resolve(libsystem, "__dispatch_main_q");
        if (!main_queue) fail("NSUndoManager groupsByEvent needs the main dispatch queue");
        static_cast<void>(support.call(libsystem, "_dispatch_async_f", {main_queue->address, manager, close_entry}));
    }

    void begin_group(Address manager) {
        const auto state = state_of(manager);
        post("NSUndoManagerCheckpointNotification", manager);
        { std::lock_guard lock(state->mutex); state->stack.begin_group(); }
        post("NSUndoManagerDidOpenUndoGroupNotification", manager);
    }
    void end_group(Address manager) {
        const auto state = state_of(manager);
        bool discardable{};
        {
            std::lock_guard lock(state->mutex);
            if (state->stack.level() == 0) fail("NSInternalInconsistencyException: endUndoGrouping called with no matching begin");
            discardable = state->stack.undo_discardable();
        }
        post("NSUndoManagerCheckpointNotification", manager);
        post("NSUndoManagerWillCloseUndoGroupNotification", manager);
        std::vector<undo::Action> dropped;
        {
            std::lock_guard lock(state->mutex);
            state->stack.end_group(&dropped);
            if (state->stack.level() == 0) state->auto_group = false;
        }
        drop(*state, dropped);
        post("NSUndoManagerDidCloseUndoGroupNotification", manager,
             dictionary(support, {{"NSUndoManagerGroupIsDiscardableKey", boolean(support, discardable)}}));
    }

    void record(Address manager, Address target, Registered value) {
        const auto state = state_of(manager);
        bool open_group{};
        {
            std::lock_guard lock(state->mutex);
            if (!state->stack.registration_enabled()) { release(value); return; }
            if (state->stack.level() == 0) {
                if (!state->groups_by_event) {
                    release(value);
                    fail("NSInternalInconsistencyException: undo registration with no open undo group (groupsByEvent is NO)");
                }
                open_group = true;
            }
        }
        if (open_group) {
            // Through the message, so subclasses see -beginUndoGrouping.
            static_cast<void>(support.send(manager, "beginUndoGrouping"));
            { std::lock_guard lock(state->mutex); state->auto_group = true; }
            schedule_close(manager, *state);
        }
        std::vector<undo::Action> cleared;
        {
            std::lock_guard lock(state->mutex);
            const auto id = state->next++;
            state->actions.emplace(id, value);
            cleared = state->stack.add({target, id});
        }
        drop(*state, cleared);
    }

    void perform(const Registered& value, Address target) {
        switch (value.kind) {
        case Registered::Kind::selector: {
            CallRegs registers;
            registers.x[2] = value.object;
            static_cast<void>(support.objc.send(target, value.selector, registers));
            break;
        }
        case Registered::Kind::block: static_cast<void>(invoke_block(support, value.block, {target})); break;
        case Registered::Kind::invocation: static_cast<void>(support.send(value.invocation, "invoke")); break;
        }
    }

    // undo / redo of the newest group.
    void run(Address manager, bool undoing) {
        const auto state = state_of(manager);
        {
            std::lock_guard lock(state->mutex);
            if (state->stack.mode() != undo::Stack::Mode::normal)
                fail(std::string("NSInternalInconsistencyException: ") + (undoing ? "undo" : "redo") + " called while undoing or redoing");
            if (state->stack.level() > 0)
                fail(std::string("NSInternalInconsistencyException: ") + (undoing ? "undoNestedGroup" : "redo") + " called with open undo groups");
        }
        post("NSUndoManagerCheckpointNotification", manager);
        std::optional<undo::Group> group;
        bool discardable{};
        {
            std::lock_guard lock(state->mutex);
            discardable = undoing ? state->stack.undo_discardable() : state->stack.redo_discardable();
            group = undoing ? state->stack.take_undo() : state->stack.take_redo();
            if (!group) return;
            state->stack.begin_perform(undoing ? undo::Stack::Mode::undoing : undo::Stack::Mode::redoing, group->name);
        }
        post(undoing ? "NSUndoManagerWillUndoChangeNotification" : "NSUndoManagerWillRedoChangeNotification", manager);
        std::vector<undo::Action> dropped;
        try {
            for (auto it = group->actions.rbegin(); it != group->actions.rend(); ++it) {
                Registered value;
                {
                    std::lock_guard lock(state->mutex);
                    const auto found = state->actions.find(it->id);
                    if (found == state->actions.end()) continue;
                    value = found->second;
                    state->actions.erase(found);
                }
                try { perform(value, it->target); } catch (...) { release(value); throw; }
                release(value);
            }
        } catch (...) {
            std::lock_guard lock(state->mutex);
            state->stack.end_perform(&dropped);
            throw;
        }
        { std::lock_guard lock(state->mutex); state->stack.end_perform(&dropped); }
        drop(*state, dropped);
        post(undoing ? "NSUndoManagerDidUndoChangeNotification" : "NSUndoManagerDidRedoChangeNotification", manager,
             dictionary(support, {{"NSUndoManagerGroupIsDiscardableKey", boolean(support, discardable)}}));
    }

    // Closes the event's automatic group before undo/redo (Foundation does).
    void close_auto_group(Address manager) {
        const auto state = state_of(manager);
        bool close{};
        {
            std::lock_guard lock(state->mutex);
            if (state->stack.level() > 1) fail("NSInternalInconsistencyException: undo was called with too many nested undo groups");
            close = state->stack.level() == 1;
        }
        if (close) static_cast<void>(support.send(manager, "endUndoGrouping"));
    }

    // ---- the invocation proxy ----
    // Instance selectors of `cls` and its superclasses below NSObject, from
    // the runtime's class table (ObjcRuntime::dump_json).
    std::vector<std::string> selectors_of(Address cls) {
        std::vector<std::string> names;
        const auto json = support.objc.dump_json();
        std::string name = support.cf.utf8(support.call(foundation_library, "_NSStringFromClass", {cls}));
        const auto list = [](std::string_view line, std::string_view key, std::vector<std::string>& out) {
            const auto start = line.find("\"" + std::string(key) + "\":[");
            if (start == std::string_view::npos) return;
            auto at = start + key.size() + 4;
            while (at < line.size() && line[at] != ']') {
                if (line[at] != '"') { ++at; continue; }
                std::string value;
                ++at;
                while (at < line.size() && line[at] != '"') {
                    if (line[at] == '\\' && at + 1 < line.size()) ++at;
                    value.push_back(line[at++]);
                }
                ++at;
                out.push_back(std::move(value));
            }
        };
        for (int depth = 0; !name.empty() && name != "NSObject" && depth < 64; ++depth) {
            const auto marker = "{\"name\":\"" + name + "\",\"super\":\"";
            const auto at = json.find(marker);
            if (at == std::string::npos) break;
            const auto end = json.find('\n', at);
            const std::string_view line(json.data() + at, (end == std::string::npos ? json.size() : end) - at);
            list(line, "instance", names);
            list(line, "guest_instance", names);
            const auto super_start = at + marker.size();
            name = json.substr(super_start, json.find('"', super_start) - super_start);
        }
        return names;
    }
    void prepare_proxy_class(Address target_class) {
        std::lock_guard lock(proxy_mutex);
        if (!prepared_classes.insert(target_class).second) return;
        for (const auto& name : selectors_of(target_class)) {
            if (!proxy_selectors.insert(name).second) continue;
            const auto selector = support.objc.selector(name);
            support.method(proxy_class, name, [this, selector](CpuState& s, GuestMemory&) {
                const auto proxy = support.objc.find_host_state<ProxyState>(s.x[0]);
                if (!proxy) fail("NSUndoManagerProxy without its undo manager");
                const auto signature = method_signature(support, support.objc.object_class(proxy->target), selector);
                if (!signature) unsupported("-[NSUndoManagerProxy " + selector_name(support, selector) + "]: the target does not implement it");
                const auto invocation = capture_invocation(support, signature, proxy->target, selector, s, false);
                Registered value;
                value.kind = Registered::Kind::invocation;
                value.invocation = invocation;
                record(proxy->manager, proxy->target, value);
                s.x[0] = 0;
                s.x[1] = 0;
            }, "v@:");
        }
    }
};

} // namespace

void install_undo_manager(Support& support) {
    auto self = std::make_shared<Undo>(support);
    const auto cls = self->manager_class = support.declare_class("NSUndoManager", "NSObject", foundation_library);
    for (const auto* name : {"NSUndoManagerCheckpointNotification", "NSUndoManagerWillUndoChangeNotification",
                             "NSUndoManagerWillRedoChangeNotification", "NSUndoManagerDidUndoChangeNotification",
                             "NSUndoManagerDidRedoChangeNotification", "NSUndoManagerDidOpenUndoGroupNotification",
                             "NSUndoManagerWillCloseUndoGroupNotification", "NSUndoManagerDidCloseUndoGroupNotification",
                             "NSUndoManagerGroupIsDiscardableKey"})
        if (!support.process.resolve(foundation_library, "_" + std::string(name))) support.string_constant(foundation_library, "_" + std::string(name), name);
    {
        // NSUndoCloseGroupingRunLoopOrdering is an NSUInteger constant (350000).
        const auto slot = support.process.allocate_guest(8);
        support.process.main_engine().write64(slot, 350000);
        support.process.install_export(std::string(foundation_library), "_NSUndoCloseGroupingRunLoopOrdering", {slot, ExportKind::data});
    }

    self->close_entry = support.process.install_function("___alchemy_completion_undo_close_group", [self](CpuState& s, GuestMemory&) {
        const auto manager = s.x[0];
        const auto state = self->state_of(manager);
        bool close{};
        {
            std::lock_guard lock(state->mutex);
            state->close_pending = false;
            close = state->auto_group && state->groups_by_event && state->stack.level() == 1 &&
                    state->stack.mode() == undo::Stack::Mode::normal;
        }
        if (close) static_cast<void>(self->support.send(manager, "endUndoGrouping"));
        objc_release(self->support, manager);
        s.x[0] = 0;
    }, "alchemy.internal");

    support.method(cls, "init", [self](CpuState& s, GuestMemory&) { static_cast<void>(self->state_of(s.x[0])); }, "@@:");
    support.method(cls, "dealloc", [self, cls](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<ManagerState>(s.x[0])) {
            std::vector<undo::Action> all;
            Address modes{};
            { std::lock_guard lock(state->mutex); all = state->stack.clear(); modes = std::exchange(state->modes, 0); }
            self->drop(*state, all);
            objc_release(self->support, modes);
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], cls, self->support.objc.selector("dealloc")));
    }, "v@:");

    // ---- registration ----
    support.method(cls, "registerUndoWithTarget:selector:object:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) fail("NSInvalidArgumentException: -registerUndoWithTarget:selector:object: needs a target and a selector");
        Registered value;
        value.kind = Registered::Kind::selector;
        value.selector = s.x[3];
        value.object = objc_retain(self->support, s.x[4]);
        self->record(s.x[0], s.x[2], value);
    }, "v@:@:@");
    support.method(cls, "registerUndoWithTarget:handler:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) fail("NSInvalidArgumentException: -registerUndoWithTarget:handler: needs a target and a handler");
        Registered value;
        value.kind = Registered::Kind::block;
        value.block = block_copy(self->support, s.x[3]);
        self->record(s.x[0], s.x[2], value);
    }, "v@:@@?");
    support.method(cls, "prepareWithInvocationTarget:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: -prepareWithInvocationTarget: with a nil target");
        self->prepare_proxy_class(self->support.objc.object_class(s.x[2]));
        auto state = std::make_shared<ProxyState>();
        state->manager = s.x[0];
        state->target = s.x[2];
        s.x[0] = autoreleased(self->support, self->support.create(self->proxy_class, state));
    }, "@@:@");

    // ---- grouping ----
    support.method(cls, "beginUndoGrouping", [self](CpuState& s, GuestMemory&) { self->begin_group(s.x[0]); }, "v@:");
    support.method(cls, "endUndoGrouping", [self](CpuState& s, GuestMemory&) { self->end_group(s.x[0]); }, "v@:");
    support.method(cls, "groupingLevel", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = static_cast<Address>(state->stack.level());
    }, "q@:");
    support.method(cls, "groupsByEvent", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->groups_by_event;
    }, "B@:");
    support.method(cls, "setGroupsByEvent:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->groups_by_event = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    support.method(cls, "levelsOfUndo", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->stack.levels();
    }, "Q@:");
    support.method(cls, "setLevelsOfUndo:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::vector<undo::Action> dropped;
        { std::lock_guard lock(state->mutex); state->stack.set_levels(s.x[2], &dropped); }
        self->drop(*state, dropped);
    }, "v@:Q");
    support.method(cls, "runLoopModes", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        Address modes;
        { std::lock_guard lock(state->mutex); modes = state->modes; }
        if (!modes) {
            const auto mode = string(self->support, "kCFRunLoopDefaultMode");
            const std::array<Address, 1> values{mode};
            modes = array(self->support, values);
        }
        s.x[0] = modes;
    }, "@@:");
    support.method(cls, "setRunLoopModes:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        const auto copy = s.x[2] ? self->support.send_id(s.x[2], "copy") : 0;
        Address old;
        { std::lock_guard lock(state->mutex); old = std::exchange(state->modes, copy); }
        objc_release(self->support, old);
    }, "v@:@");

    // ---- undo and redo ----
    support.method(cls, "undo", [self](CpuState& s, GuestMemory&) {
        self->close_auto_group(s.x[0]);
        static_cast<void>(self->support.send(s.x[0], "undoNestedGroup"));
    }, "v@:");
    support.method(cls, "undoNestedGroup", [self](CpuState& s, GuestMemory&) { self->run(s.x[0], true); }, "v@:");
    support.method(cls, "redo", [self](CpuState& s, GuestMemory&) {
        self->close_auto_group(s.x[0]);
        self->run(s.x[0], false);
    }, "v@:");
    const auto flag = [self](auto read) {
        return [self, read](CpuState& s, GuestMemory&) {
            const auto state = self->state_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = read(*state) ? 1 : 0;
        };
    };
    support.method(cls, "canUndo", flag([](ManagerState& v) { return v.stack.can_undo(); }), "B@:");
    support.method(cls, "canRedo", flag([](ManagerState& v) { return v.stack.can_redo(); }), "B@:");
    support.method(cls, "isUndoing", flag([](ManagerState& v) { return v.stack.mode() == undo::Stack::Mode::undoing; }), "B@:");
    support.method(cls, "isRedoing", flag([](ManagerState& v) { return v.stack.mode() == undo::Stack::Mode::redoing; }), "B@:");
    support.method(cls, "isUndoRegistrationEnabled", flag([](ManagerState& v) { return v.stack.registration_enabled(); }), "B@:");
    support.method(cls, "undoActionIsDiscardable", flag([](ManagerState& v) { return v.stack.undo_discardable(); }), "B@:");
    support.method(cls, "redoActionIsDiscardable", flag([](ManagerState& v) { return v.stack.redo_discardable(); }), "B@:");
    support.method(cls, "disableUndoRegistration", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->stack.disable();
    }, "v@:");
    support.method(cls, "enableUndoRegistration", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        if (!state->stack.enable()) fail("NSInternalInconsistencyException: enableUndoRegistration without a matching disableUndoRegistration");
    }, "v@:");
    support.method(cls, "removeAllActions", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::vector<undo::Action> all;
        { std::lock_guard lock(state->mutex); all = state->stack.clear(); state->auto_group = false; }
        self->drop(*state, all);
    }, "v@:");
    support.method(cls, "removeAllActionsWithTarget:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::vector<undo::Action> removed;
        { std::lock_guard lock(state->mutex); removed = state->stack.remove_target(s.x[2]); }
        self->drop(*state, removed);
    }, "v@:@");

    // ---- action names ----
    support.method(cls, "setActionName:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;   // nil is ignored, as documented
        const auto name = self->support.cf.utf8(s.x[2]);
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->stack.set_name(name);
    }, "v@:@");
    support.method(cls, "setActionIsDiscardable:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->stack.set_discardable((s.x[2] & 0xff) != 0);
    }, "v@:B");
    const auto name_of = [self](Address manager, bool undoing) {
        const auto state = self->state_of(manager);
        std::lock_guard lock(state->mutex);
        return undoing ? state->stack.undo_name() : state->stack.redo_name();
    };
    support.method(cls, "undoActionName", [self, name_of](CpuState& s, GuestMemory&) { s.x[0] = string(self->support, name_of(s.x[0], true)); }, "@@:");
    support.method(cls, "redoActionName", [self, name_of](CpuState& s, GuestMemory&) { s.x[0] = string(self->support, name_of(s.x[0], false)); }, "@@:");
    const auto title = [](std::string_view verb, const std::string& name) { return name.empty() ? std::string(verb) : std::string(verb) + " " + name; };
    support.method(cls, "undoMenuItemTitle", [self, name_of](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.send_id(s.x[0], "undoMenuTitleForUndoActionName:", {string(self->support, name_of(s.x[0], true))});
    }, "@@:");
    support.method(cls, "redoMenuItemTitle", [self, name_of](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.send_id(s.x[0], "redoMenuTitleForUndoActionName:", {string(self->support, name_of(s.x[0], false))});
    }, "@@:");
    support.method(cls, "undoMenuTitleForUndoActionName:", [self, title](CpuState& s, GuestMemory&) {
        s.x[0] = string(self->support, title("Undo", text(self->support, s.x[2])));
    }, "@@:@");
    support.method(cls, "redoMenuTitleForUndoActionName:", [self, title](CpuState& s, GuestMemory&) {
        s.x[0] = string(self->support, title("Redo", text(self->support, s.x[2])));
    }, "@@:@");

    // ---- the invocation proxy (a root class: NSObject's methods are not recorded) ----
    self->proxy_class = support.objc.find_class("NSUndoManagerProxy");
    if (!self->proxy_class) self->proxy_class = support.objc.register_native_class("NSUndoManagerProxy", 0, 16);
    const auto proxy = self->proxy_class;
    const auto own = [self, proxy](std::string name, Handler handler, std::string types) {
        self->proxy_selectors.insert(name);
        self->support.method(proxy, std::move(name), std::move(handler), std::move(types));
    };
    own("retain", [self](CpuState& s, GuestMemory&) { s.x[0] = self->support.process.objects().retain(s.x[0]); }, "@@:");
    own("release", [self](CpuState& s, GuestMemory&) { self->support.process.objects().release(s.x[0]); }, "v@:");
    own("autorelease", [self](CpuState& s, GuestMemory&) {
        const auto thread = self->support.process.current_thread();
        if (thread->autorelease_pools.empty()) thread->unpooled_autoreleases.push_back(s.x[0]);
        else thread->autorelease_pools.back().objects.push_back(s.x[0]);
    }, "@@:");
    own("isProxy", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    own("respondsToSelector:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->support.objc.find_host_state<ProxyState>(s.x[0]);
        s.x[0] = state && self->support.objc.responds(state->target, s.x[2]);
    }, "B@::");
    // +initialize and +class for the runtime's own use of the class.
    support.method(proxy, "initialize", [](CpuState&, GuestMemory&) {}, "v@:", true);
    support.method(proxy, "class", [](CpuState&, GuestMemory&) {}, "#@:", true);
}

} // namespace alchemy::gui::completion_kit
