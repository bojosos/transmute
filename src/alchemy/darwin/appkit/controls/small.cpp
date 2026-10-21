#include "alchemy/darwin/appkit/controls/g3.hpp"

// Small AppKit / Foundation additions QLab reaches:
// * nib decoding of gesture recognizers (NSGestureRecognizer.* and
//   NSPanGestureRecognizer.* / NSClickGestureRecognizer.* /
//   NSPressGestureRecognizer.* keys, 62 nibs). The recognizers are kept with
//   their target, action and settings; gestures are not recognized from the
//   window server's input (panels.cpp), so they never fire;
// * +[NSButton buttonWithImage:target:action:];
// * -[NSMenu insertItemWithTitle:action:keyEquivalent:atIndex:];
// * -[NSTextInputContext prepareForNewContentsWithOptions:] (drops a
//   composition in progress, as the input method's state for new contents);
// * NSProgress's file-operation properties (kind, cancellable, file operation
//   kind, file counts: the userInfo keys AppKit's Finder progress reads),
//   on Foundation's NSProgress when it has one, else on a minimal NSProgress
//   (unit counts, fraction, cancellation, user info);
// * NSTouch (trackpad touches; the window server delivers none, so no
//   touch object is made, but the class answers its API).
// NSButton / NSImageView -contentTintColor, NSButtonCell -highlightsBy /
// -showsStateBy, -[NSPopUpButtonCell performClickWithFrame:inView:] and
// NSTextFieldCell -sendsActionOnEndEditing are in appkit/controls.cpp.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

constexpr std::string_view objc_runtime = "/usr/lib/libobjc.A.dylib";

// NSProgress state of the minimal class.
struct ProgressState final : ObjectPayload {
    std::int64_t total{}, completed{};
    Address user_info{};   // +1 NSMutableDictionary
    Address parent{};      // not retained
    bool cancelled{}, paused{}, cancellable{true}, pausable{};
};

struct TouchState final : ObjectPayload {
    Address identity{}, device{};   // identity +1
    Point normalized;
    Size device_size;
    std::uint64_t phase{};
    std::int64_t type{};            // NSTouchTypeIndirect 1 (trackpad), direct 0
    bool resting{};
};

} // namespace

void install_small_additions(AppKit& kit) {
    auto* const k = &kit;

    // ---- gesture recognizers from nibs ----
    if (const auto recognizer = kit.objc.find_class("NSGestureRecognizer")) {
        kit.method(recognizer, "initWithCoder:", [k](CpuState& s, GuestMemory&) {
            const Decoder in{*k, s.x[2]};
            const auto action = in.string("NSGestureRecognizer.action");
            const auto self = k->send_id(s.x[0], "initWithTarget:action:",
                                         {in.object("NSGestureRecognizer.target"), action ? k->sel(*action) : 0});
            s.x[0] = self;
            if (!self) return;
            if (const auto delegate = in.object("NSGestureRecognizer.delegate")) static_cast<void>(k->send_id(self, "setDelegate:", {delegate}));
            if (in.has("NSGestureRecognizer.allowedTouchTypes"))
                static_cast<void>(k->send_id(self, "setAllowedTouchTypes:", {static_cast<Address>(in.integer("NSGestureRecognizer.allowedTouchTypes"))}));
            if (in.has("NSGestureRecognizer.enabled"))
                static_cast<void>(k->send_id(self, "setEnabled:", {in.flag("NSGestureRecognizer.enabled") ? 1u : 0u}));
            // The subclass keys, prefixed with the class name AppKit archived.
            for (const auto* prefix : {"NSPanGestureRecognizer", "NSClickGestureRecognizer", "NSPressGestureRecognizer"}) {
                const std::string base = prefix;
                if (in.has(base + ".buttonMask") && k->responds(self, "setButtonMask:"))
                    static_cast<void>(k->send_id(self, "setButtonMask:", {static_cast<Address>(in.integer(base + ".buttonMask"))}));
                if (in.has(base + ".numberOfTouchesRequired") && k->responds(self, "setNumberOfTouchesRequired:"))
                    static_cast<void>(k->send_id(self, "setNumberOfTouchesRequired:", {static_cast<Address>(in.integer(base + ".numberOfTouchesRequired"))}));
                if (in.has(base + ".numberOfClicksRequired") && k->responds(self, "setNumberOfClicksRequired:"))
                    static_cast<void>(k->send_id(self, "setNumberOfClicksRequired:", {static_cast<Address>(in.integer(base + ".numberOfClicksRequired"))}));
                if (in.has(base + ".minimumPressDuration") && k->responds(self, "setMinimumPressDuration:")) {
                    CallRegs registers;
                    registers.set_d(0, in.real(base + ".minimumPressDuration"));
                    static_cast<void>(k->send(self, "setMinimumPressDuration:", registers));
                }
            }
        });
    }

    // ---- NSButton ----
    kit.class_method(kit.button, "buttonWithImage:target:action:", [k](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, {{0, 0}, {48, 32}});
        const auto made = k->send(k->send(s.x[0], "alloc").x0, "initWithFrame:", registers).x0;
        static_cast<void>(k->send_id(made, "setTitle:", {k->auto_string("")}));
        static_cast<void>(k->send_id(made, "setImage:", {s.x[2]}));
        static_cast<void>(k->send_id(made, "setImagePosition:", {1}));   // NSImageOnly
        static_cast<void>(k->send_id(made, "setTarget:", {s.x[3]}));
        static_cast<void>(k->send_id(made, "setAction:", {s.x[4]}));
        static_cast<void>(k->send(made, "sizeToFit"));
        s.x[0] = k->autorelease(made);
    });

    // ---- NSMenu ----
    kit.method(kit.menu, "insertItemWithTitle:action:keyEquivalent:atIndex:", [k](CpuState& s, GuestMemory&) {
        const auto item = k->send_id(k->send(k->menu_item, "alloc").x0, "initWithTitle:action:keyEquivalent:", {s.x[2], s.x[3], s.x[4]});
        try { static_cast<void>(k->send_id(s.x[0], "insertItem:atIndex:", {item, s.x[5]})); }
        catch (...) { k->release(item); throw; }
        s.x[0] = k->autorelease(item);
    }, "@@:@:@q");

    // ---- NSTextInputContext ----
    kit.method(kit.text_input_context, "prepareForNewContentsWithOptions:", [k](CpuState& s, GuestMemory&) {
        const auto client = k->send(s.x[0], "client").x0;
        if (client && k->send_bool(client, "hasMarkedText")) static_cast<void>(k->send(client, "unmarkText"));
        if (client && k->is_kind(client, k->view))
            if (const auto window_object = k->view_state(client)->window)
                if (const auto id = k->window_state(window_object)->id)
                    if (const auto server = window_server_or_null()) server->discard_marked_text(id);
    }, "v@:Q");

    // ---- NSProgress ----
    const auto has_method = [k](Address cls, const char* name) {
        return k->guest_call(objc_runtime, "_class_getInstanceMethod", {cls, k->sel(name)}) != 0;
    };
    auto progress = kit.objc.find_class("NSProgress");
    if (!progress) {
        progress = kit.objc.register_native_class("NSProgress", kit.objc.find_class("NSObject"), 8);
        kit.objc.publish_class("NSProgress", progress, "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation");
    }
    if (!has_method(progress, "totalUnitCount")) {
        // A minimal NSProgress (Foundation has none here, or only a method-less shell).
        const auto p = progress;
        const auto pr = [k](Address value) { return k->objc.host_state<ProgressState>(value); };
        const auto user_info = [k, pr](Address self) {
            const auto state = pr(self);
            if (!state->user_info) state->user_info = k->send(k->send(k->objc.find_class("NSMutableDictionary"), "alloc").x0, "init").x0;
            return state->user_info;
        };
        kit.method(p, "initWithParent:userInfo:", [k, p, pr](CpuState& s, GuestMemory&) {
            const auto self = k->objc.send_super(s.x[0], p, k->sel("init")).x0;
            s.x[0] = self;
            const auto state = pr(self);
            state->parent = s.x[2];
            if (s.x[3]) state->user_info = k->send(s.x[3], "mutableCopy").x0;
        });
        kit.method(p, "init", [k](CpuState& s, GuestMemory&) { s.x[0] = k->send_id(s.x[0], "initWithParent:userInfo:", {0, 0}); });
        for (const auto* factory : {"progressWithTotalUnitCount:", "discreteProgressWithTotalUnitCount:"})
            kit.class_method(p, factory, [k](CpuState& s, GuestMemory&) {
                const auto made = k->alloc_init(s.x[0]);
                static_cast<void>(k->send_id(made, "setTotalUnitCount:", {s.x[2]}));
                s.x[0] = k->autorelease(made);
            });
        kit.class_method(p, "currentProgress", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // no implicit progress tree
        kit.method(p, "dealloc", [k, p](CpuState& s, GuestMemory&) {
            if (const auto state = k->objc.find_host_state<ProgressState>(s.x[0])) k->release(std::exchange(state->user_info, 0));
            static_cast<void>(k->objc.send_super(s.x[0], p, k->sel("dealloc")));
        }, "v@:");
        kit.method(p, "totalUnitCount", [pr](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(pr(s.x[0])->total); }, "q@:");
        kit.method(p, "setTotalUnitCount:", [pr](CpuState& s, GuestMemory&) { pr(s.x[0])->total = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
        kit.method(p, "completedUnitCount", [pr](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(pr(s.x[0])->completed); }, "q@:");
        kit.method(p, "setCompletedUnitCount:", [pr](CpuState& s, GuestMemory&) { pr(s.x[0])->completed = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
        kit.method(p, "fractionCompleted", [pr](CpuState& s, GuestMemory&) {
            const auto state = pr(s.x[0]);
            set_d(s, 0, state->total > 0 ? std::clamp(static_cast<double>(state->completed) / static_cast<double>(state->total), 0.0, 1.0) : 0.0);
        }, "d@:");
        kit.method(p, "isIndeterminate", [pr](CpuState& s, GuestMemory&) { return_bool(s, pr(s.x[0])->total <= 0); }, "B@:");
        kit.method(p, "isFinished", [pr](CpuState& s, GuestMemory&) {
            const auto state = pr(s.x[0]);
            return_bool(s, state->total > 0 && state->completed >= state->total);
        }, "B@:");
        kit.method(p, "isCancelled", [pr](CpuState& s, GuestMemory&) { return_bool(s, pr(s.x[0])->cancelled); }, "B@:");
        kit.method(p, "cancel", [pr](CpuState& s, GuestMemory&) { if (pr(s.x[0])->cancellable) pr(s.x[0])->cancelled = true; }, "v@:");
        kit.method(p, "isPaused", [pr](CpuState& s, GuestMemory&) { return_bool(s, pr(s.x[0])->paused); }, "B@:");
        kit.method(p, "pause", [pr](CpuState& s, GuestMemory&) { if (pr(s.x[0])->pausable) pr(s.x[0])->paused = true; }, "v@:");
        kit.method(p, "resume", [pr](CpuState& s, GuestMemory&) { pr(s.x[0])->paused = false; }, "v@:");
        kit.method(p, "isCancellable", [pr](CpuState& s, GuestMemory&) { return_bool(s, pr(s.x[0])->cancellable); }, "B@:");
        kit.method(p, "setCancellable:", [pr](CpuState& s, GuestMemory&) { pr(s.x[0])->cancellable = bool_arg(s, 2); }, "v@:B");
        kit.method(p, "isPausable", [pr](CpuState& s, GuestMemory&) { return_bool(s, pr(s.x[0])->pausable); }, "B@:");
        kit.method(p, "setPausable:", [pr](CpuState& s, GuestMemory&) { pr(s.x[0])->pausable = bool_arg(s, 2); }, "v@:B");
        kit.method(p, "userInfo", [user_info](CpuState& s, GuestMemory&) { s.x[0] = user_info(s.x[0]); });
        kit.method(p, "setUserInfoObject:forKey:", [k, user_info](CpuState& s, GuestMemory&) {
            const auto dictionary = user_info(s.x[0]);
            if (s.x[2]) static_cast<void>(k->send_id(dictionary, "setObject:forKey:", {s.x[2], s.x[3]}));
            else static_cast<void>(k->send_id(dictionary, "removeObjectForKey:", {s.x[3]}));
        }, "v@:@@");
        kit.method(p, "localizedDescription", [k, pr](CpuState& s, GuestMemory&) {
            const auto state = pr(s.x[0]);
            const auto percent = state->total > 0 ? static_cast<int>(100.0 * static_cast<double>(state->completed) / static_cast<double>(state->total)) : 0;
            s.x[0] = k->auto_string(std::to_string(percent) + "% completed");
        });
    }
    // The file-operation properties (Foundation stores them in the user info).
    const auto info_property = [&kit, progress, k](const char* getter, const char* setter, const char* key, bool number) {
        kit.method(progress, getter, [k, key, number](CpuState& s, GuestMemory&) {
            const auto value = k->send_id(k->send(s.x[0], "userInfo").x0, "objectForKey:", {k->auto_string(key)});
            s.x[0] = number ? (value ? static_cast<Address>(static_cast<std::int64_t>(k->cf.number_value(value).value_or(0))) : 0) : value;
        }, number ? "@@:" : "@@:");
        kit.method(progress, setter, [k, key, number](CpuState& s, GuestMemory&) {
            Address value = s.x[2];
            if (number) value = k->autorelease(k->cf.integer(static_cast<std::int64_t>(s.x[2])));
            static_cast<void>(k->send_id(s.x[0], "setUserInfoObject:forKey:", {value, k->auto_string(key)}));
        }, number ? "v@:@" : "v@:@");
    };
    info_property("fileOperationKind", "setFileOperationKind:", "NSProgressFileOperationKindKey", false);
    info_property("fileTotalCount", "setFileTotalCount:", "NSProgressFileTotalCountKey", false);
    info_property("fileCompletedCount", "setFileCompletedCount:", "NSProgressFileCompletedCountKey", false);
    {
        static const auto kind_key = kit.process.allocate_guest(8);
        kit.method(progress, "kind", [k](CpuState& s, GuestMemory&) {
            s.x[0] = k->guest_call(objc_runtime, "_objc_getAssociatedObject", {s.x[0], kind_key});
        });
        kit.method(progress, "setKind:", [k](CpuState& s, GuestMemory&) {
            static_cast<void>(k->guest_call(objc_runtime, "_objc_setAssociatedObject", {s.x[0], kind_key, s.x[2], 3}));
        }, "v@:@");
    }
    if (!has_method(progress, "setCancellable:")) unsupported("Foundation's NSProgress has no -setCancellable:");

    // ---- NSTouch ----
    if (const auto touch = kit.declare_class("NSTouch", "NSObject"); !has_method(touch, "normalizedPosition")) {
        const auto ts = [k](Address value) { return k->objc.host_state<TouchState>(value); };
        kit.method(touch, "identity", [ts](CpuState& s, GuestMemory&) { s.x[0] = ts(s.x[0])->identity; });
        kit.method(touch, "phase", [ts](CpuState& s, GuestMemory&) { s.x[0] = ts(s.x[0])->phase; }, "Q@:");
        kit.method(touch, "normalizedPosition", [ts](CpuState& s, GuestMemory&) { return_point(s, ts(s.x[0])->normalized); }, "{CGPoint=dd}@:");
        kit.method(touch, "isResting", [ts](CpuState& s, GuestMemory&) { return_bool(s, ts(s.x[0])->resting); }, "B@:");
        kit.method(touch, "device", [ts](CpuState& s, GuestMemory&) { s.x[0] = ts(s.x[0])->device; });
        kit.method(touch, "deviceSize", [ts](CpuState& s, GuestMemory&) { return_size(s, ts(s.x[0])->device_size); }, "{CGSize=dd}@:");
        kit.method(touch, "type", [ts](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ts(s.x[0])->type); }, "q@:");
        kit.method(touch, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");
        kit.method(touch, "dealloc", [k, touch](CpuState& s, GuestMemory&) {
            if (const auto state = k->objc.find_host_state<TouchState>(s.x[0])) k->release(std::exchange(state->identity, 0));
            static_cast<void>(k->objc.send_super(s.x[0], touch, k->sel("dealloc")));
        }, "v@:");
    }
}

} // namespace alchemy::gui::completion_kit::controls
