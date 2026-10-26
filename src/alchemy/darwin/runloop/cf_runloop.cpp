#include "alchemy/darwin/runloop/runloop_internal.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>

// CFRunLoop C API: run loops, observers and version-0 sources (owner: A2).
// Timers are in timers.cpp. Behaviour follows Apple's public CFRunLoop
// reference; version-1 (Mach port) sources fail explicitly.
namespace alchemy::gui::detail {
namespace {
// Callbacks and context of a CFRunLoopObserver or CFRunLoopSource.
struct ContextInfo { std::uint64_t info{}, retain{}, release{}, description{}; };

struct ObserverState final : ObjectPayload {
    std::mutex mutex;
    std::uint32_t activities{};
    bool repeats{}, valid{true};
    std::int64_t order{};
    std::uint64_t callout{}, block{};
    ContextInfo context;
    std::shared_ptr<RunLoopImpl> loop;
    ObserverId id{};
};

struct SourceState final : ObjectPayload {
    std::mutex mutex;
    std::int64_t order{};
    bool valid{true}, signaled{};
    ContextInfo context;
    std::uint64_t equal{}, hash{}, schedule{}, cancel{}, perform{};
    std::shared_ptr<RunLoopImpl> loop;
    SourceId id{};
    ModeSet modes;
};

template <class T>
std::shared_ptr<T> state_of(ObjcRuntime& objc, std::uint64_t object, std::string_view what) {
    if (!object) throw std::invalid_argument("null " + std::string(what));
    const auto state = objc.find_host_state<T>(object);
    if (!state) throw std::runtime_error("object is not a " + std::string(what));
    return state;
}
} // namespace

void Provider::install_cf() {
    cf_run_loop_class = define_class("_AlchemyCFRunLoop", core_foundation_library);
    observer_class = define_class("_AlchemyCFRunLoopObserver", core_foundation_library);
    source_class = define_class("_AlchemyCFRunLoopSource", core_foundation_library);
    const auto function = [this](std::string name, transmute::GuestCallHandler handler) {
        static_cast<void>(process.install_function("_" + name, std::move(handler), core_foundation_library));
    };
    const auto read_context = [this](std::uint64_t address, std::string_view what) {
        ContextInfo context;
        if (!address) return context;
        auto& memory = process.calling_engine();
        if (memory.read64(address) != 0) throw std::runtime_error("unsupported " + std::string(what) + " context version");
        context.info = memory.read64(address + 8); context.retain = memory.read64(address + 16);
        context.release = memory.read64(address + 24); context.description = memory.read64(address + 32);
        if (context.retain && context.info) context.info = call_address(context.retain, {context.info});
        return context;
    };
    const auto write_context = [](transmute::GuestMemory& memory, std::uint64_t address, const ContextInfo& context) {
        if (!address) throw std::invalid_argument("CFRunLoop context pointer is NULL");
        memory.write64(address + 8, context.info); memory.write64(address + 16, context.retain);
        memory.write64(address + 24, context.release); memory.write64(address + 32, context.description);
    };
    const auto release_context = [this](ContextInfo& context) {
        const auto info = std::exchange(context.info, 0), release = std::exchange(context.release, 0);
        if (release && info) static_cast<void>(call_address(release, {info}));
    };

    // ---- run loops ---------------------------------------------------------------
    function("CFRunLoopGetMain", [this](auto& state, auto&) { state.x[0] = cf_object(main); });
    function("CFRunLoopGetCurrent", [this](auto& state, auto&) { state.x[0] = cf_object(current()); });
    function("CFRunLoopRun", [this](auto&, auto&) {
        // Runs the default mode until stopped or out of sources and timers.
        const auto loop = current();
        for (;;) {
            const auto result = loop->run_once(default_mode, distant_future, false);
            if (result == RunResult::stopped || result == RunResult::finished) return;
        }
    });
    function("CFRunLoopRunInMode", [this](auto& state, auto&) {
        const auto result = current()->run_once(mode_name(state.x[0]), d_argument(state, 0), (state.x[1] & 0xff) != 0);
        state.x[0] = static_cast<std::uint32_t>(result);
    });
    function("CFRunLoopStop", [this](auto& state, auto&) { loop_of(state.x[0])->stop(); });
    function("CFRunLoopWakeUp", [this](auto& state, auto&) { loop_of(state.x[0])->wake(); });
    function("CFRunLoopIsWaiting", [this](auto& state, auto&) { state.x[0] = loop_of(state.x[0])->is_waiting(); });
    function("CFRunLoopCopyCurrentMode", [this](auto& state, auto&) {
        const auto mode = loop_of(state.x[0])->current_mode();
        state.x[0] = mode.empty() ? 0 : mode_object(mode); // Immortal: the +1 is implicit.
    });
    function("CFRunLoopCopyAllModes", [this](auto& state, auto&) {
        std::vector<std::uint64_t> objects;
        for (const auto& mode : loop_of(state.x[0])->all_modes()) objects.push_back(mode_object(mode));
        state.x[0] = cf.array(objects);
    });
    function("CFRunLoopAddCommonMode", [this](auto& state, auto&) { loop_of(state.x[0])->add_common_mode(mode_name(state.x[1])); });
    function("CFRunLoopGetNextTimerFireDate", [this](auto& state, auto&) {
        const auto next = loop_of(state.x[0])->next_timer_fire(mode_name(state.x[1]));
        d_result(state, std::isinf(next) ? 0.0 : absolute_from_monotonic(next));
    });
    function("CFRunLoopPerformBlock", [this](auto& state, auto&) {
        const auto loop = loop_of(state.x[0]);
        if (!state.x[2]) throw std::invalid_argument("CFRunLoopPerformBlock requires a block");
        // The mode argument is a mode name or an array of them.
        auto modes = cf.kind(state.x[1]) == ObjectKind::array ? mode_names(state.x[1]) : ModeSet{mode_name(state.x[1])};
        const auto block = block_copy(state.x[2]);
        const std::weak_ptr<Provider> weak = weak_from_this();
        loop->perform(std::move(modes), [weak, block] {
            const auto self = weak.lock();
            if (!self) return;
            struct Release { Provider& self; std::uint64_t block; ~Release() { try { self.block_release(block); } catch (...) {} } } owned{*self, block};
            static_cast<void>(self->call_address(self->process.calling_engine().read64(block + 16), {block}));
        }, false);
    });

    // ---- observers -----------------------------------------------------------------
    const auto create_observer = [this, read_context](std::uint64_t activities, bool repeats, std::int64_t order,
                                                     std::uint64_t callout, std::uint64_t context, std::uint64_t block) {
        const auto object = objc.allocate(observer_class);
        try {
            const auto value = objc.host_state<ObserverState>(object);
            std::lock_guard lock(value->mutex);
            value->activities = static_cast<std::uint32_t>(activities);
            value->repeats = repeats; value->order = order; value->callout = callout;
            value->block = block_copy(block);
            value->context = read_context(context, "CFRunLoopObserver");
        } catch (...) { release(object); throw; }
        return object;
    };
    function("CFRunLoopObserverCreate", [create_observer](auto& state, auto&) {
        if (!state.x[4]) throw std::invalid_argument("CFRunLoopObserverCreate requires a callout");
        state.x[0] = create_observer(state.x[1], state.x[2] & 0xff, std::bit_cast<std::int64_t>(state.x[3]), state.x[4], state.x[5], 0);
    });
    function("CFRunLoopObserverCreateWithHandler", [create_observer](auto& state, auto&) {
        if (!state.x[4]) throw std::invalid_argument("CFRunLoopObserverCreateWithHandler requires a block");
        state.x[0] = create_observer(state.x[1], state.x[2] & 0xff, std::bit_cast<std::int64_t>(state.x[3]), 0, 0, state.x[4]);
    });
    const auto invalidate_observer = [this, release_context](std::uint64_t object) {
        const auto value = state_of<ObserverState>(objc, object, "CFRunLoopObserver");
        std::shared_ptr<RunLoopImpl> loop;
        ObserverId id{};
        std::uint64_t block;
        ContextInfo context;
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            value->valid = false;
            loop = std::move(value->loop); id = std::exchange(value->id, 0);
            block = std::exchange(value->block, 0); context = std::exchange(value->context, {});
        }
        if (loop) loop->remove_observer(id);
        block_release(block);
        release_context(context);
        if (loop) release(object); // The run loop's reference.
    };
    const auto fire_observer = [this, invalidate_observer](std::uint64_t object, std::uint32_t activity) {
        const auto value = objc.find_host_state<ObserverState>(object);
        if (!value) return;
        static_cast<void>(retain(object));
        struct Hold { Provider& self; std::uint64_t object; ~Hold() { try { self.release(object); } catch (...) {} } } hold{*this, object};
        std::uint64_t callout, block, info;
        bool repeats;
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            callout = value->callout; block = value->block; info = value->context.info; repeats = value->repeats;
        }
        if (block) static_cast<void>(call_address(process.calling_engine().read64(block + 16), {block, object, activity}));
        else static_cast<void>(call_address(callout, {object, activity, info}));
        if (!repeats) invalidate_observer(object); // A one-shot observer invalidates itself after firing.
    };
    function("CFRunLoopAddObserver", [this, fire_observer](auto& state, auto&) {
        const auto loop = loop_of(state.x[0]);
        const auto object = state.x[1];
        const auto mode = mode_name(state.x[2]);
        const auto value = state_of<ObserverState>(objc, object, "CFRunLoopObserver");
        ObserverId existing{};
        std::uint32_t activities;
        std::int64_t order;
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            if (value->loop && value->loop != loop) throw std::runtime_error("a CFRunLoopObserver can be added to one run loop only");
            existing = value->id; activities = value->activities; order = value->order;
        }
        if (existing) { loop->add_observer_mode(existing, mode); return; }
        // The loop's own one-shot handling would drop the entry before the
        // observer invalidates; the observer object decides instead.
        const auto id = loop->add_observer(activities, mode, true, [fire_observer, object](std::uint32_t activity) {
            fire_observer(object, activity);
        });
        loop->set_observer_order(id, order);
        { std::lock_guard lock(value->mutex); value->loop = loop; value->id = id; }
        static_cast<void>(retain(object));
    });
    function("CFRunLoopRemoveObserver", [this](auto& state, auto&) {
        const auto loop = loop_of(state.x[0]);
        const auto object = state.x[1];
        const auto value = state_of<ObserverState>(objc, object, "CFRunLoopObserver");
        {
            std::lock_guard lock(value->mutex);
            if (value->loop != loop) return;
            if (loop->remove_observer_mode(value->id, mode_name(state.x[2]))) return;
            loop->remove_observer(value->id);
            value->loop.reset(); value->id = 0;
        }
        release(object);
    });
    function("CFRunLoopContainsObserver", [this](auto& state, auto&) {
        const auto loop = loop_of(state.x[0]);
        const auto value = state_of<ObserverState>(objc, state.x[1], "CFRunLoopObserver");
        const auto mode = mode_name(state.x[2]);
        std::lock_guard lock(value->mutex);
        state.x[0] = value->loop == loop && loop->observer_in_mode(value->id, mode);
    });
    function("CFRunLoopObserverInvalidate", [invalidate_observer](auto& state, auto&) { invalidate_observer(state.x[0]); });
    function("CFRunLoopObserverIsValid", [this](auto& state, auto&) {
        const auto value = state_of<ObserverState>(objc, state.x[0], "CFRunLoopObserver");
        std::lock_guard lock(value->mutex); state.x[0] = value->valid;
    });
    function("CFRunLoopObserverGetActivities", [this](auto& state, auto&) {
        const auto value = state_of<ObserverState>(objc, state.x[0], "CFRunLoopObserver");
        std::lock_guard lock(value->mutex); state.x[0] = value->activities;
    });
    function("CFRunLoopObserverDoesRepeat", [this](auto& state, auto&) {
        const auto value = state_of<ObserverState>(objc, state.x[0], "CFRunLoopObserver");
        std::lock_guard lock(value->mutex); state.x[0] = value->repeats;
    });
    function("CFRunLoopObserverGetOrder", [this](auto& state, auto&) {
        const auto value = state_of<ObserverState>(objc, state.x[0], "CFRunLoopObserver");
        std::lock_guard lock(value->mutex); state.x[0] = std::bit_cast<std::uint64_t>(value->order);
    });
    function("CFRunLoopObserverGetContext", [this, write_context](auto& state, auto& memory) {
        const auto value = state_of<ObserverState>(objc, state.x[0], "CFRunLoopObserver");
        std::lock_guard lock(value->mutex); write_context(memory, state.x[1], value->context);
    });
    method(observer_class, "dealloc", [this, release_context](auto& state, auto&) {
        if (const auto value = objc.find_host_state<ObserverState>(state.x[0])) {
            std::uint64_t block;
            ContextInfo context;
            { std::lock_guard lock(value->mutex); block = std::exchange(value->block, 0); context = std::exchange(value->context, {}); }
            block_release(block);
            release_context(context);
        }
        static_cast<void>(objc.send_super(state.x[0], observer_class, objc.selector("dealloc")));
    }, false, "v@:");

    // ---- version-0 sources ---------------------------------------------------------
    function("CFRunLoopSourceCreate", [this, read_context](auto& state, auto& memory) {
        const auto context = state.x[2];
        if (!context) throw std::invalid_argument("CFRunLoopSourceCreate requires a context");
        if (memory.read64(context) != 0)
            throw std::runtime_error("unsupported CFRunLoopSource version " + std::to_string(memory.read64(context)) +
                                     " (Mach port sources)");
        const auto object = objc.allocate(source_class);
        try {
            const auto value = objc.host_state<SourceState>(object);
            std::lock_guard lock(value->mutex);
            value->order = std::bit_cast<std::int64_t>(state.x[1]);
            // CFRunLoopSourceContext: version, info, retain, release,
            // copyDescription, equal, hash, schedule, cancel, perform.
            value->equal = memory.read64(context + 40); value->hash = memory.read64(context + 48);
            value->schedule = memory.read64(context + 56); value->cancel = memory.read64(context + 64);
            value->perform = memory.read64(context + 72);
            if (!value->perform) throw std::invalid_argument("CFRunLoopSourceContext requires a perform callback");
            value->context = read_context(context, "CFRunLoopSource");
        } catch (...) { release(object); throw; }
        state.x[0] = object;
    });
    const auto perform_source = [this](std::uint64_t object) {
        const auto value = objc.find_host_state<SourceState>(object);
        if (!value) return;
        static_cast<void>(retain(object));
        struct Hold { Provider& self; std::uint64_t object; ~Hold() { try { self.release(object); } catch (...) {} } } hold{*this, object};
        std::uint64_t perform, info;
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            perform = value->perform; info = value->context.info;
        }
        static_cast<void>(call_address(perform, {info}));
    };
    const auto remove_source_mode = [this](std::uint64_t object, const std::shared_ptr<RunLoopImpl>& loop, const std::string& mode) {
        const auto value = state_of<SourceState>(objc, object, "CFRunLoopSource");
        std::uint64_t cancel, info;
        bool last{};
        {
            std::lock_guard lock(value->mutex);
            if (value->loop != loop || !value->modes.erase(mode)) return;
            cancel = value->cancel; info = value->context.info;
            if (!loop->remove_source_mode(value->id, mode)) {
                loop->remove_source(value->id);
                value->loop.reset(); value->id = 0; last = true;
            }
        }
        // cancel(info, rl, mode) runs when the source leaves a mode.
        if (cancel) static_cast<void>(call_address(cancel, {info, cf_object(loop), mode_object(mode)}));
        if (last) release(object);
    };
    function("CFRunLoopAddSource", [this, perform_source](auto& state, auto&) {
        const auto loop = loop_of(state.x[0]);
        const auto object = state.x[1];
        const auto mode = mode_name(state.x[2]);
        const auto value = state_of<SourceState>(objc, object, "CFRunLoopSource");
        SourceId existing{};
        std::uint64_t schedule, info;
        bool signaled;
        std::int64_t order;
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            if (value->loop && value->loop != loop) throw std::runtime_error("a CFRunLoopSource can be added to one run loop only");
            if (!value->modes.insert(mode).second) return;
            existing = value->id; schedule = value->schedule; info = value->context.info;
            signaled = std::exchange(value->signaled, false); order = value->order;
        }
        if (existing) loop->add_source_mode(existing, mode);
        else {
            const auto id = loop->add_source(mode, [perform_source, object] { perform_source(object); });
            loop->set_source_order(id, order);
            { std::lock_guard lock(value->mutex); value->loop = loop; value->id = id; }
            static_cast<void>(retain(object));
            if (signaled) loop->signal_source(id);
        }
        // schedule(info, rl, mode) runs when the source joins a mode.
        if (schedule) static_cast<void>(call_address(schedule, {info, state.x[0], mode_object(mode)}));
    });
    function("CFRunLoopRemoveSource", [this, remove_source_mode](auto& state, auto&) {
        remove_source_mode(state.x[1], loop_of(state.x[0]), mode_name(state.x[2]));
    });
    function("CFRunLoopContainsSource", [this](auto& state, auto&) {
        const auto loop = loop_of(state.x[0]);
        const auto value = state_of<SourceState>(objc, state.x[1], "CFRunLoopSource");
        const auto mode = mode_name(state.x[2]);
        std::lock_guard lock(value->mutex);
        state.x[0] = value->loop == loop && value->modes.contains(mode);
    });
    function("CFRunLoopSourceSignal", [this](auto& state, auto&) {
        const auto value = state_of<SourceState>(objc, state.x[0], "CFRunLoopSource");
        std::lock_guard lock(value->mutex);
        if (!value->valid) return;
        if (value->loop) value->loop->signal_source(value->id);
        else value->signaled = true; // Delivered once the source joins a run loop.
    });
    function("CFRunLoopSourceInvalidate", [this, remove_source_mode, release_context](auto& state, auto&) {
        const auto object = state.x[0];
        const auto value = state_of<SourceState>(objc, object, "CFRunLoopSource");
        std::shared_ptr<RunLoopImpl> loop;
        ModeSet modes;
        {
            std::lock_guard lock(value->mutex);
            if (!value->valid) return;
            loop = value->loop; modes = value->modes;
        }
        if (loop) for (const auto& mode : modes) remove_source_mode(object, loop, mode);
        ContextInfo context;
        { std::lock_guard lock(value->mutex); value->valid = false; context = std::exchange(value->context, {}); }
        release_context(context);
    });
    function("CFRunLoopSourceIsValid", [this](auto& state, auto&) {
        const auto value = state_of<SourceState>(objc, state.x[0], "CFRunLoopSource");
        std::lock_guard lock(value->mutex); state.x[0] = value->valid;
    });
    function("CFRunLoopSourceGetOrder", [this](auto& state, auto&) {
        const auto value = state_of<SourceState>(objc, state.x[0], "CFRunLoopSource");
        std::lock_guard lock(value->mutex); state.x[0] = std::bit_cast<std::uint64_t>(value->order);
    });
    function("CFRunLoopSourceGetContext", [this, write_context](auto& state, auto& memory) {
        const auto value = state_of<SourceState>(objc, state.x[0], "CFRunLoopSource");
        if (!state.x[1] || memory.read64(state.x[1]) != 0) throw std::runtime_error("unsupported CFRunLoopSource context version");
        std::lock_guard lock(value->mutex);
        write_context(memory, state.x[1], value->context);
        memory.write64(state.x[1] + 40, value->equal); memory.write64(state.x[1] + 48, value->hash);
        memory.write64(state.x[1] + 56, value->schedule); memory.write64(state.x[1] + 64, value->cancel);
        memory.write64(state.x[1] + 72, value->perform);
    });
    method(source_class, "dealloc", [this, release_context](auto& state, auto&) {
        if (const auto value = objc.find_host_state<SourceState>(state.x[0])) {
            ContextInfo context;
            { std::lock_guard lock(value->mutex); context = std::exchange(value->context, {}); }
            release_context(context);
        }
        static_cast<void>(objc.send_super(state.x[0], source_class, objc.selector("dealloc")));
    }, false, "v@:");
}

void Provider::install_type_ids() {
    // CF type IDs for the run-loop types, from the provider-wide numbering (gui::fill::TypeId).
    const std::array<std::pair<std::uint64_t, fill::TypeId>, 4> types{{
        {cf_run_loop_class, fill::TypeId::run_loop}, {source_class, fill::TypeId::run_loop_source},
        {observer_class, fill::TypeId::run_loop_observer}, {timer_class, fill::TypeId::run_loop_timer}}};
    for (const auto& [name, id] : std::array<std::pair<const char*, fill::TypeId>, 4>{{
             {"_CFRunLoopGetTypeID", fill::TypeId::run_loop}, {"_CFRunLoopSourceGetTypeID", fill::TypeId::run_loop_source},
             {"_CFRunLoopObserverGetTypeID", fill::TypeId::run_loop_observer}, {"_CFRunLoopTimerGetTypeID", fill::TypeId::run_loop_timer}}})
        static_cast<void>(process.install_function(name, [value = static_cast<std::uint64_t>(id)](transmute::CpuState& s, transmute::GuestMemory&) {
            s.x[0] = value; }, core_foundation_library));
    cf.extend("CFGetTypeID", "CFRunLoop types", [this, types](transmute::CpuState& s, transmute::GuestMemory&) -> std::optional<std::uint64_t> {
        const auto view = s.x[0] ? process.objects().peek(s.x[0]) : std::nullopt;
        if (!view) return std::nullopt;
        for (const auto& [cls, id] : types) if (cls && view->class_address == cls) return static_cast<std::uint64_t>(id);
        return std::nullopt;
    });
}

} // namespace alchemy::gui::detail
