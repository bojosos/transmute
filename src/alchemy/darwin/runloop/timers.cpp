#include "alchemy/darwin/runloop/runloop_internal.hpp"

#include <cmath>
#include <stdexcept>

// NSTimer and CFRunLoopTimer (toll-free bridged: one host class serves both;
// owner: A2). Behaviour follows Apple's NSTimer and CFRunLoopTimer reference.
namespace alchemy::gui::detail {
namespace {
// NSTimer clamps non-positive repeating intervals to 0.1 ms.
constexpr double minimum_repeat = 0.0001;

struct TimerState final : ObjectPayload {
    std::mutex mutex;
    double fire{};                  // CFAbsoluteTime while unscheduled
    double interval{}, tolerance{};
    std::int64_t order{};
    bool repeats{}, valid{true};
    // NSTimer target-action (target and userInfo retained) or block
    // (copied; NSTimer's and CFRunLoopTimerCreateWithHandler's blocks both
    // take the timer).
    std::uint64_t target{}, selector{}, user_info{}, block{};
    // CFRunLoopTimerCreate callout and CFRunLoopTimerContext.
    std::uint64_t callout{}, info{}, info_retain{}, info_release{}, info_description{};
    std::shared_ptr<RunLoopImpl> loop;   // set while scheduled
    TimerId id{};
};

std::shared_ptr<TimerState> timer_state(ObjcRuntime& objc, std::uint64_t timer) {
    if (!timer) throw std::invalid_argument("nil NSTimer");
    const auto state = objc.find_host_state<TimerState>(timer);
    if (!state) throw std::runtime_error("NSTimer used before initialization");
    return state;
}
} // namespace

std::uint64_t Provider::define_class(std::string_view name, std::string_view library) {
    auto cls = objc.find_class(name);
    if (!cls) cls = objc.register_native_class(std::string(name), objc.find_class("NSObject"), 8);
    objc.publish_class(name, cls, library);
    return cls;
}

void Provider::method(std::uint64_t cls, std::string name, transmute::GuestCallHandler handler, bool class_method,
                      std::string types, const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(objc.add_native_method(cls, std::move(name), std::move(handler), class_method, std::move(types)));
}

void Provider::schedule_timer(std::uint64_t timer, const std::shared_ptr<RunLoopImpl>& loop, std::string_view mode) {
    const auto state = timer_state(objc, timer);
    double fire;
    bool repeats;
    double interval;
    TimerId existing{};
    {
        std::lock_guard lock(state->mutex);
        if (!state->valid) return; // Invalid timers are never scheduled.
        if (state->loop && state->loop != loop) throw std::runtime_error("an NSTimer can be scheduled on one run loop only");
        if (state->loop) existing = state->id;
        fire = state->fire; repeats = state->repeats; interval = state->interval;
    }
    if (existing) { loop->add_timer_mode(existing, mode); return; }
    const std::weak_ptr<Provider> weak = weak_from_this();
    const auto id = loop->add_timer(monotonic_from_absolute(fire), repeats ? interval : 0, mode, [weak, timer](TimerId) {
        if (const auto self = weak.lock()) self->fire_timer(timer);
    });
    {
        std::lock_guard lock(state->mutex);
        state->loop = loop; state->id = id;
    }
    loop->set_timer_order(id, state->order);
    static_cast<void>(retain(timer)); // The run loop's reference.
}

void Provider::unschedule_timer(std::uint64_t timer, const std::shared_ptr<RunLoopImpl>& loop, std::string_view mode) {
    const auto state = timer_state(objc, timer);
    {
        std::lock_guard lock(state->mutex);
        if (state->loop != loop) return;
        if (loop->remove_timer_mode(state->id, mode)) return;
        // Out of every mode: the timer stays valid but leaves the run loop,
        // keeping its next fire date for a later schedule.
        state->fire = absolute_from_monotonic(loop->timer_fire(state->id));
        loop->cancel_timer(state->id);
        state->loop.reset(); state->id = 0;
    }
    release(timer);
}

bool Provider::timer_scheduled(std::uint64_t timer, const std::shared_ptr<RunLoopImpl>& loop, std::string_view mode) {
    const auto state = timer_state(objc, timer);
    std::lock_guard lock(state->mutex);
    return state->loop == loop && loop->timer_in_mode(state->id, mode);
}

void Provider::fire_timer(std::uint64_t timer) {
    const auto state = objc.find_host_state<TimerState>(timer);
    if (!state) return;
    static_cast<void>(retain(timer)); // Alive through the callout even if it invalidates itself.
    struct Hold { Provider& self; std::uint64_t timer; ~Hold() { try { self.release(timer); } catch (...) {} } } hold{*this, timer};
    std::uint64_t block, callout, info, target, selector;
    bool repeats;
    {
        std::lock_guard lock(state->mutex);
        if (!state->valid) return;
        block = state->block; callout = state->callout; info = state->info;
        target = state->target; selector = state->selector; repeats = state->repeats;
    }
    if (block) static_cast<void>(call_address(process.calling_engine().read64(block + 16), {block, timer}));
    else if (callout) static_cast<void>(call_address(callout, {timer, info}));
    else if (target) {
        CallRegs registers;
        registers.x[2] = timer; // - (void)timerFireMethod:(NSTimer *)timer
        static_cast<void>(objc.send(target, selector, std::move(registers)));
    }
    if (!repeats) invalidate_timer(timer); // A non-repeating timer invalidates itself after firing.
}

void Provider::invalidate_timer(std::uint64_t timer) {
    const auto state = timer_state(objc, timer);
    std::shared_ptr<RunLoopImpl> loop;
    TimerId id{};
    std::uint64_t target, user_info, block, info, info_release;
    {
        std::lock_guard lock(state->mutex);
        if (!state->valid) return;
        state->valid = false;
        loop = std::move(state->loop); id = std::exchange(state->id, 0);
        target = std::exchange(state->target, 0); user_info = std::exchange(state->user_info, 0);
        block = std::exchange(state->block, 0); info = std::exchange(state->info, 0);
        info_release = std::exchange(state->info_release, 0);
    }
    if (loop) loop->cancel_timer(id);
    release(target); release(user_info); block_release(block);
    if (info_release && info) static_cast<void>(call_address(info_release, {info}));
    if (loop) release(timer);
}

void Provider::install_timers() {
    timer_class = define_class("NSTimer", foundation_library);
    const auto t = timer_class;
    // Common construction: the object comes from +alloc (guest or host).
    const auto initialize = [this](std::uint64_t timer, double fire, double interval, bool repeats, std::uint64_t target,
                                   std::uint64_t selector, std::uint64_t user_info, std::uint64_t block) {
        if (!block && (!target || !selector)) throw std::invalid_argument("NSTimer requires a target and a selector, or a block");
        const auto state = objc.host_state<TimerState>(timer);
        std::lock_guard lock(state->mutex);
        state->fire = fire;
        state->repeats = repeats;
        state->interval = repeats ? std::max(interval, minimum_repeat) : std::max(interval, 0.0);
        state->target = retain(target); state->selector = selector;
        state->user_info = retain(user_info); state->block = block_copy(block);
    };
    const auto make = [this, initialize](double interval, bool repeats, std::uint64_t target, std::uint64_t selector,
                                         std::uint64_t user_info, std::uint64_t block, bool scheduled) {
        const auto timer = objc.allocate(timer_class);
        try {
            const auto clamped = repeats ? std::max(interval, minimum_repeat) : std::max(interval, 0.0);
            initialize(timer, CoreFoundation::absolute_time_now() + clamped, interval, repeats, target, selector, user_info, block);
            if (scheduled) schedule_timer(timer, current(), default_mode);
        } catch (...) { release(timer); throw; }
        return autorelease(timer);
    };
    method(t, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", [make](auto& state, auto&) {
        state.x[0] = make(d_argument(state, 0), state.x[5] & 0xff, state.x[2], state.x[3], state.x[4], 0, true);
    }, true, "@@:d@:@B");
    method(t, "timerWithTimeInterval:target:selector:userInfo:repeats:", [make](auto& state, auto&) {
        state.x[0] = make(d_argument(state, 0), state.x[5] & 0xff, state.x[2], state.x[3], state.x[4], 0, false);
    }, true, "@@:d@:@B");
    method(t, "scheduledTimerWithTimeInterval:repeats:block:", [make](auto& state, auto&) {
        if (!state.x[3]) throw std::invalid_argument("NSTimer block is nil");
        state.x[0] = make(d_argument(state, 0), state.x[2] & 0xff, 0, 0, 0, state.x[3], true);
    }, true, "@@:dB@?");
    method(t, "timerWithTimeInterval:repeats:block:", [make](auto& state, auto&) {
        if (!state.x[3]) throw std::invalid_argument("NSTimer block is nil");
        state.x[0] = make(d_argument(state, 0), state.x[2] & 0xff, 0, 0, 0, state.x[3], false);
    }, true, "@@:dB@?");
    for (const auto* name : {"scheduledTimerWithTimeInterval:invocation:repeats:", "timerWithTimeInterval:invocation:repeats:"})
        method(t, name, [](auto&, auto&) { throw std::runtime_error("unsupported NSTimer with an NSInvocation"); }, true, "@@:d@B");
    method(t, "initWithFireDate:interval:target:selector:userInfo:repeats:", [this, initialize](auto& state, auto&) {
        const auto fire = state.x[2] ? cf.date_time(state.x[2]) : CoreFoundation::absolute_time_now();
        initialize(state.x[0], fire, d_argument(state, 0), state.x[6] & 0xff, state.x[3], state.x[4], state.x[5], 0);
    }, false, "@@:@d@:@B");
    method(t, "initWithFireDate:interval:repeats:block:", [this, initialize](auto& state, auto&) {
        if (!state.x[4]) throw std::invalid_argument("NSTimer block is nil");
        const auto fire = state.x[2] ? cf.date_time(state.x[2]) : CoreFoundation::absolute_time_now();
        initialize(state.x[0], fire, d_argument(state, 0), state.x[3] & 0xff, 0, 0, 0, state.x[4]);
    }, false, "@@:@dB@?");
    // -fire runs the callout now without changing a repeating timer's schedule.
    method(t, "fire", [this](auto& state, auto&) { fire_timer(state.x[0]); }, false, "v@:");
    method(t, "invalidate", [this](auto& state, auto&) { invalidate_timer(state.x[0]); }, false, "v@:");
    method(t, "isValid", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex); state.x[0] = value->valid;
    }, false, "B@:");
    const auto fire_date = [this](std::uint64_t timer) {
        const auto value = timer_state(objc, timer);
        std::lock_guard lock(value->mutex);
        return value->loop ? absolute_from_monotonic(value->loop->timer_fire(value->id)) : value->fire;
    };
    const auto set_fire_date = [this](std::uint64_t timer, double absolute) {
        const auto value = timer_state(objc, timer);
        std::lock_guard lock(value->mutex);
        value->fire = absolute;
        if (value->loop) value->loop->set_timer_fire(value->id, monotonic_from_absolute(absolute));
    };
    method(t, "fireDate", [this, fire_date](auto& state, auto&) { state.x[0] = date(fire_date(state.x[0])); });
    method(t, "setFireDate:", [this, set_fire_date](auto& state, auto&) {
        if (!state.x[2]) throw std::invalid_argument("NSTimer fire date is nil");
        set_fire_date(state.x[0], cf.date_time(state.x[2]));
    }, false, "v@:@");
    method(t, "timeInterval", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex);
        d_result(state, value->repeats ? value->interval : 0.0);
    }, false, "d@:");
    method(t, "userInfo", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex); state.x[0] = value->user_info;
    });
    method(t, "tolerance", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex); d_result(state, value->tolerance);
    }, false, "d@:");
    // Tolerance only permits late firing; this run loop fires on time.
    method(t, "setTolerance:", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex);
        value->tolerance = std::max(0.0, d_argument(state, 0));
    }, false, "v@:d");
    method(t, "dealloc", [this](auto& state, auto&) {
        const auto timer = state.x[0];
        // A scheduled timer is retained by its run loop, so only an unscheduled
        // timer reaches here: release what invalidate would.
        if (const auto value = objc.find_host_state<TimerState>(timer)) {
            std::uint64_t target, user_info, block, info, info_release;
            {
                std::lock_guard lock(value->mutex);
                target = std::exchange(value->target, 0); user_info = std::exchange(value->user_info, 0);
                block = std::exchange(value->block, 0); info = std::exchange(value->info, 0);
                info_release = std::exchange(value->info_release, 0); value->valid = false;
            }
            release(target); release(user_info); block_release(block);
            if (info_release && info) static_cast<void>(call_address(info_release, {info}));
        }
        static_cast<void>(objc.send_super(timer, timer_class, objc.selector("dealloc")));
    }, false, "v@:");

    // ---- CFRunLoopTimer --------------------------------------------------------
    const auto function = [this](std::string name, transmute::GuestCallHandler handler) {
        static_cast<void>(process.install_function("_" + name, std::move(handler), core_foundation_library));
    };
    // CFRunLoopTimerCreate(allocator, fireDate d0, interval d1, flags, order, callout, context)
    const auto create = [this](double fire, double interval, std::int64_t order, std::uint64_t callout,
                               std::uint64_t context, std::uint64_t block) {
        const auto timer = objc.allocate(timer_class);
        try {
            const auto value = objc.host_state<TimerState>(timer);
            std::lock_guard lock(value->mutex);
            value->fire = fire;
            // A zero or negative interval makes a one-shot timer.
            value->repeats = interval > 0;
            value->interval = value->repeats ? interval : 0;
            value->order = order;
            value->callout = callout;
            value->block = block_copy(block);
            if (context) {
                auto& memory = process.calling_engine();
                if (memory.read64(context) != 0) throw std::runtime_error("unsupported CFRunLoopTimerContext version");
                value->info = memory.read64(context + 8);
                value->info_retain = memory.read64(context + 16);
                value->info_release = memory.read64(context + 24);
                value->info_description = memory.read64(context + 32);
                if (value->info_retain && value->info) value->info = call_address(value->info_retain, {value->info});
            }
        } catch (...) { release(timer); throw; }
        return timer;
    };
    function("CFRunLoopTimerCreate", [create](auto& state, auto&) {
        if (!state.x[3]) throw std::invalid_argument("CFRunLoopTimerCreate requires a callout");
        state.x[0] = create(d_argument(state, 0), d_argument(state, 1), std::bit_cast<std::int64_t>(state.x[2]), state.x[3], state.x[4], 0);
    });
    function("CFRunLoopTimerCreateWithHandler", [create](auto& state, auto&) {
        if (!state.x[3]) throw std::invalid_argument("CFRunLoopTimerCreateWithHandler requires a block");
        state.x[0] = create(d_argument(state, 0), d_argument(state, 1), std::bit_cast<std::int64_t>(state.x[2]), 0, 0, state.x[3]);
    });
    function("CFRunLoopTimerInvalidate", [this](auto& state, auto&) { invalidate_timer(state.x[0]); });
    function("CFRunLoopTimerIsValid", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex); state.x[0] = value->valid;
    });
    function("CFRunLoopTimerGetNextFireDate", [fire_date](auto& state, auto&) { d_result(state, fire_date(state.x[0])); });
    function("CFRunLoopTimerSetNextFireDate", [set_fire_date](auto& state, auto&) { set_fire_date(state.x[0], d_argument(state, 0)); });
    function("CFRunLoopTimerGetInterval", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex);
        d_result(state, value->repeats ? value->interval : 0.0);
    });
    function("CFRunLoopTimerDoesRepeat", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex); state.x[0] = value->repeats;
    });
    function("CFRunLoopTimerGetOrder", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex);
        state.x[0] = std::bit_cast<std::uint64_t>(value->order);
    });
    function("CFRunLoopTimerGetTolerance", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex); d_result(state, value->tolerance);
    });
    function("CFRunLoopTimerSetTolerance", [this](auto& state, auto&) {
        const auto value = timer_state(objc, state.x[0]); std::lock_guard lock(value->mutex);
        value->tolerance = std::max(0.0, d_argument(state, 0));
    });
    function("CFRunLoopTimerGetContext", [this](auto& state, auto& memory) {
        const auto value = timer_state(objc, state.x[0]);
        if (!state.x[1]) throw std::invalid_argument("CFRunLoopTimerGetContext requires a context");
        if (memory.read64(state.x[1]) != 0) throw std::runtime_error("unsupported CFRunLoopTimerContext version");
        std::lock_guard lock(value->mutex);
        memory.write64(state.x[1] + 8, value->info); memory.write64(state.x[1] + 16, value->info_retain);
        memory.write64(state.x[1] + 24, value->info_release); memory.write64(state.x[1] + 32, value->info_description);
    });
    function("CFRunLoopAddTimer", [this](auto& state, auto&) { schedule_timer(state.x[1], loop_of(state.x[0]), mode_name(state.x[2])); });
    function("CFRunLoopRemoveTimer", [this](auto& state, auto&) { unschedule_timer(state.x[1], loop_of(state.x[0]), mode_name(state.x[2])); });
    function("CFRunLoopContainsTimer", [this](auto& state, auto&) {
        state.x[0] = timer_scheduled(state.x[1], loop_of(state.x[0]), mode_name(state.x[2]));
    });
}

} // namespace alchemy::gui::detail
