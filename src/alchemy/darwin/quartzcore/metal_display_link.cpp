#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/call_regs.hpp"

#include "alchemy/gui/layer_contents.hpp"
#include "alchemy/gui/window_server.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <bit>
#include <initializer_list>
#include <string>
#include <utility>
#include <mutex>

// CAMetalDisplayLink (macOS 14) and CAMetalDisplayLinkUpdate (owner: g3,
// Chrome stage 4; moved here from darwin/media/media_gaps.cpp, where the
// class was a property bag). A link added to a run loop fires on that run
// loop at the display's refresh rate (the preferred rate of
// preferredFrameRateRange when one is set, taken when the link is added), as
// a CFRunLoopTimer in the given mode: each firing takes the layer's next
// drawable and sends -metalDisplayLink:needsUpdate: to the delegate with a
// CAMetalDisplayLinkUpdate (drawable, targetTimestamp = now + one period,
// targetPresentationTimestamp = now + preferredFrameLatency periods, on
// CACurrentMediaTime's clock). A paused link, one without a delegate, or one
// whose layer has no drawable now skips the frame. -invalidate removes it
// from every run loop.
//
// CADisplayLink (macOS 14: made by -displayLinkWithTarget:selector: of
// NSView, NSWindow and NSScreen, appkit/view.cpp) works the same way: added
// to a run loop, it sends its selector to its target (retained until
// -invalidate) once per display refresh, with timestamp = the firing time and
// targetTimestamp = one period later. While it is on a run loop the link
// keeps itself alive, as the run loop does on macOS; -invalidate ends that.
//
// Also the CADynamicRange keys of -[CALayer preferredDynamicRange]
// (quartzcore.cpp stores the property; composition is standard dynamic range
// only). Their string values are unverified.
namespace alchemy::gui::support_kit {
namespace {

constexpr std::string_view quartzcore_library = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";
constexpr std::string_view core_foundation_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

struct DisplayLinkState final : ObjectPayload {
    std::mutex mutex;
    Address layer{};      // retained
    Address delegate{};   // weak
    float minimum{}, maximum{}, preferred{};   // CAFrameRateRange (0: the display's rate)
    float latency{2};
    bool paused{};
    bool invalidated{};
    Address timer{};      // +1 CFRunLoopTimerRef while added to a run loop
    double period{1.0 / 60};
};

struct UpdateState final : ObjectPayload {
    Address drawable{};   // retained
    double target{}, presentation{};
};

double display_refresh() {
    if (const auto server = layer_contents::presentation_server())
        for (const auto& screen : server->screens())
            if (screen.refresh_rate >= 1 && screen.refresh_rate <= 1000) return screen.refresh_rate;
    return 60;
}

float single(const CpuState& s, unsigned index) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[index][0])); }
void return_single(CpuState& s, unsigned index, float value) { s.v[index] = {std::bit_cast<std::uint32_t>(value), 0}; }

std::shared_ptr<DisplayLinkState> link_state(Support& support, Address object) {
    auto state = object ? support.objc.find_host_state<DisplayLinkState>(object) : nullptr;
    if (!state) fail("CAMetalDisplayLink: the receiver was not initialized with -initWithMetalLayer:");
    return state;
}

// CACurrentMediaTime's clock (mach_absolute_time in seconds).
double media_time() { return static_cast<double>(host::time::monotonic_ns()) / 1e9; }
// CFAbsoluteTimeGetCurrent's (seconds since 2001-01-01 UTC).
double absolute_time() { return static_cast<double>(host::time::realtime_ns()) / 1e9 - 978307200.0; }

// The timer's callout (CFRunLoopTimerCallBack: timer, info = the link).
void fire(Support& support, Address update_class, Address link) {
    const auto state = support.objc.find_host_state<DisplayLinkState>(link);
    if (!state) return;
    Address layer{}, delegate{};
    double period{};
    float latency{};
    {
        std::lock_guard lock(state->mutex);
        if (state->paused || state->invalidated || !state->delegate) return;
        layer = state->layer;
        delegate = state->delegate;
        period = state->period;
        latency = state->latency;
    }
    const auto drawable = support.objc.send(layer, "nextDrawable").x0;
    if (!drawable) return;
    const auto update = support.objc.allocate(update_class);
    const auto now = media_time();
    {
        const auto value = support.objc.host_state<UpdateState>(update, [] { return std::make_shared<UpdateState>(); });
        value->drawable = support.retain(drawable);
        value->target = now + period;
        value->presentation = now + period * std::max(1.0F, latency);
    }
    CallRegs arguments;
    arguments.x[2] = link;
    arguments.x[3] = update;
    static_cast<void>(support.objc.send(delegate, "metalDisplayLink:needsUpdate:", arguments));
    support.release(update);
}

void stop_timer(Support& support, Address timer) {
    if (!timer) return;
    static_cast<void>(support.call(core_foundation_library, "_CFRunLoopTimerInvalidate", {timer}));
    support.release(timer);
}

// The display's rate, capped by a preferred rate (0: none); and a repeating
// CFRunLoopTimer (+1) at that rate calling `callout_symbol` with `info`.
double link_period(float preferred) {
    const auto refresh = display_refresh();
    return 1.0 / (preferred > 0 ? std::min<double>(preferred, refresh) : refresh);
}
Address make_timer(Support& support, GuestMemory& m, std::string_view callout_symbol, Address info, double period, std::string_view who) {
    const auto callout = support.process.resolve(quartzcore_library, callout_symbol);
    if (!callout) fail(std::string(who) + ": the timer callout is not exported");
    const auto context = support.process.allocate_guest(40, 8);   // CFRunLoopTimerContext
    m.fill_bytes(context, 0, 40);
    m.write64(context + 8, info);
    CallRegs create;
    create.x[0] = 0;
    create.set_d(0, absolute_time() + period);   // fireDate
    create.set_d(1, period);
    create.x[1] = 0;   // flags
    create.x[2] = 0;   // order
    create.x[3] = callout->address;
    create.x[4] = context;
    const auto timer = support.call(core_foundation_library, "_CFRunLoopTimerCreate", create).x0;
    support.process.release_guest(context);
    if (!timer) fail(std::string(who) + ": CFRunLoopTimerCreate failed");
    return timer;
}

// ---- CADisplayLink ----
struct LinkState final : ObjectPayload {
    std::mutex mutex;
    Address target{};     // retained until -invalidate
    Address selector{};
    float minimum{}, maximum{}, preferred{};   // CAFrameRateRange (0: the display's rate)
    bool paused{}, invalidated{};
    Address timer{};      // +1 while on a run loop (the link then holds itself)
    double period{1.0 / 60}, timestamp{}, target_timestamp{};
};

std::shared_ptr<LinkState> display_link_state(Support& support, Address object) {
    auto state = object ? support.objc.find_host_state<LinkState>(object) : nullptr;
    if (!state) fail("CADisplayLink: the receiver was not made by -displayLinkWithTarget:selector:");
    return state;
}

void fire_display_link(Support& support, Address link) {
    const auto state = support.objc.find_host_state<LinkState>(link);
    if (!state) return;
    Address target{}, selector{};
    {
        std::lock_guard lock(state->mutex);
        if (state->paused || state->invalidated || !state->target) return;
        state->timestamp = media_time();
        state->target_timestamp = state->timestamp + state->period;
        target = state->target;
        selector = state->selector;
    }
    CallRegs arguments;
    arguments.x[2] = link;
    static_cast<void>(support.objc.send(target, selector, arguments));
}

// Ends the link: off every run loop, the target released and, when it was
// scheduled, the link's hold on itself dropped (not from -dealloc).
void invalidate_display_link(Support& support, Address link, bool holds_itself) {
    const auto state = display_link_state(support, link);
    Address timer{}, target{};
    {
        std::lock_guard lock(state->mutex);
        state->invalidated = true;
        timer = std::exchange(state->timer, 0);
        target = std::exchange(state->target, 0);
    }
    stop_timer(support, timer);
    support.release(target);
    if (timer && holds_itself) support.release(link);
}

void install_ca_display_link(Support& support) {
    auto* s = &support;
    const auto link = support.declare_class("CADisplayLink", "NSObject", quartzcore_library);
    constexpr std::string_view callout_symbol = "_AlchemyCADisplayLinkFire";
    support.function(quartzcore_library, std::string(callout_symbol), [s](CpuState& c, GuestMemory&) { fire_display_link(*s, c.x[1]); });
    support.method(link, "displayLinkWithTarget:selector:", [s, link](CpuState& c, GuestMemory&) {
        if (!c.x[2] || !c.x[3]) fail("+[CADisplayLink displayLinkWithTarget:selector:] needs a target and a selector");
        const auto made = s->objc.allocate(link);
        const auto value = s->objc.host_state<LinkState>(made, [] { return std::make_shared<LinkState>(); });
        value->target = s->retain(c.x[2]);
        value->selector = c.x[3];
        c.x[0] = s->objc.send(made, "autorelease").x0;
    }, "@@:@:", true);
    support.method(link, "addToRunLoop:forMode:", [s, callout_symbol](CpuState& c, GuestMemory& m) {
        const auto self = c.x[0], run_loop = c.x[2], mode = c.x[3];
        const auto value = display_link_state(*s, self);
        const auto cf_loop = run_loop ? s->objc.send(run_loop, "getCFRunLoop").x0 : 0;
        if (!cf_loop || !mode) fail("-[CADisplayLink addToRunLoop:forMode:] needs an NSRunLoop and a mode");
        Address timer{};
        float preferred{};
        {
            std::lock_guard lock(value->mutex);
            if (value->invalidated) return;
            timer = value->timer;
            preferred = value->preferred;
        }
        if (!timer) {
            const auto period = link_period(preferred);
            timer = make_timer(*s, m, callout_symbol, self, period, "CADisplayLink");
            static_cast<void>(s->retain(self));   // the run loop's hold
            std::lock_guard lock(value->mutex);
            value->timer = timer;
            value->period = period;
        }
        static_cast<void>(s->call(core_foundation_library, "_CFRunLoopAddTimer", {cf_loop, timer, mode}));
    }, "v@:@@");
    support.method(link, "removeFromRunLoop:forMode:", [s](CpuState& c, GuestMemory&) {
        const auto value = display_link_state(*s, c.x[0]);
        Address timer{};
        {
            std::lock_guard lock(value->mutex);
            timer = value->timer;
        }
        const auto cf_loop = c.x[2] ? s->objc.send(c.x[2], "getCFRunLoop").x0 : 0;
        if (timer && cf_loop && c.x[3]) static_cast<void>(s->call(core_foundation_library, "_CFRunLoopRemoveTimer", {cf_loop, timer, c.x[3]}));
    }, "v@:@@");
    support.method(link, "invalidate", [s](CpuState& c, GuestMemory&) { invalidate_display_link(*s, c.x[0], true); }, "v@:");
    support.method(link, "dealloc", [s, link](CpuState& c, GuestMemory&) {
        const auto self = c.x[0];
        if (s->objc.find_host_state<LinkState>(self)) invalidate_display_link(*s, self, false);
        static_cast<void>(s->objc.send_super(self, link, s->objc.selector("dealloc")));
    }, "v@:");
    const auto real = [s](double LinkState::*member) {
        return [s, member](CpuState& c, GuestMemory&) {
            const auto value = display_link_state(*s, c.x[0]);
            std::lock_guard lock(value->mutex);
            return_real(c, 0, (*value).*member);
        };
    };
    support.method(link, "timestamp", real(&LinkState::timestamp), "d@:");
    support.method(link, "targetTimestamp", real(&LinkState::target_timestamp), "d@:");
    support.method(link, "duration", real(&LinkState::period), "d@:");
    const auto paused = [s](CpuState& c, GuestMemory&) {
        const auto value = display_link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        c.x[0] = value->paused ? 1 : 0;
    };
    support.method(link, "isPaused", paused, "B@:");
    support.method(link, "paused", paused, "B@:");
    support.method(link, "setPaused:", [s](CpuState& c, GuestMemory&) {
        const auto value = display_link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        value->paused = (c.x[2] & 0xff) != 0;
    }, "v@:B");
    // The preferred rate applies from the next -addToRunLoop:forMode: of an
    // unscheduled link.
    support.method(link, "preferredFrameRateRange", [s](CpuState& c, GuestMemory&) {
        const auto value = display_link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        return_single(c, 0, value->minimum);
        return_single(c, 1, value->maximum);
        return_single(c, 2, value->preferred);
    }, "{CAFrameRateRange=fff}@:");
    support.method(link, "setPreferredFrameRateRange:", [s](CpuState& c, GuestMemory&) {
        const auto value = display_link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        value->minimum = single(c, 0);
        value->maximum = single(c, 1);
        value->preferred = single(c, 2);
    }, "v@:{CAFrameRateRange=fff}");
    support.method(link, "preferredFramesPerSecond", [s](CpuState& c, GuestMemory&) {
        const auto value = display_link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        c.x[0] = static_cast<Address>(static_cast<std::int64_t>(value->preferred));
    }, "q@:");
    support.method(link, "setPreferredFramesPerSecond:", [s](CpuState& c, GuestMemory&) {
        const auto value = display_link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        value->preferred = static_cast<float>(static_cast<std::int64_t>(c.x[2]));
        value->minimum = value->maximum = 0;
    }, "v@:q");
}

} // namespace

void install_metal_display_link(Support& support) {
    auto* s = &support;
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_CADynamicRangeAutomatic", "automatic"}, {"_CADynamicRangeStandard", "standard"},
             {"_CADynamicRangeConstrainedHigh", "constrainedHigh"}, {"_CADynamicRangeHigh", "high"}})
        support.string_constant(quartzcore_library, symbol, value);
    install_ca_display_link(support);
    const auto link = support.declare_class("CAMetalDisplayLink", "NSObject", quartzcore_library);
    const auto update = support.declare_class("CAMetalDisplayLinkUpdate", "NSObject", quartzcore_library);
    // The run-loop timer's callout: a private QuartzCore export, so the
    // guest CFRunLoopTimer can call it.
    constexpr std::string_view callout_symbol = "_AlchemyCAMetalDisplayLinkFire";
    support.function(quartzcore_library, std::string(callout_symbol), [s, update](CpuState& c, GuestMemory&) { fire(*s, update, c.x[1]); });

    support.method(link, "initWithMetalLayer:", [s](CpuState& c, GuestMemory&) {
        if (!c.x[2]) fail("-[CAMetalDisplayLink initWithMetalLayer:] with a nil layer");
        const auto value = s->objc.host_state<DisplayLinkState>(c.x[0], [] { return std::make_shared<DisplayLinkState>(); });
        value->layer = s->retain(c.x[2]);
    }, "@@:@");
    support.method(link, "metalLayer", [s](CpuState& c, GuestMemory&) { c.x[0] = link_state(*s, c.x[0])->layer; }, "@@:");
    support.method(link, "delegate", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        c.x[0] = value->delegate;
    }, "@@:");
    support.method(link, "setDelegate:", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        value->delegate = c.x[2];
    }, "v@:@");
    // CAFrameRateRange {float minimum, maximum, preferred}: s0-s2.
    support.method(link, "preferredFrameRateRange", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        return_single(c, 0, value->minimum);
        return_single(c, 1, value->maximum);
        return_single(c, 2, value->preferred);
    }, "{CAFrameRateRange=fff}@:");
    support.method(link, "setPreferredFrameRateRange:", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        value->minimum = single(c, 0);
        value->maximum = single(c, 1);
        value->preferred = single(c, 2);
    }, "v@:{CAFrameRateRange=fff}");
    support.method(link, "preferredFrameLatency", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        return_single(c, 0, value->latency);
    }, "f@:");
    support.method(link, "setPreferredFrameLatency:", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        value->latency = single(c, 0);
    }, "v@:f");
    const auto paused = [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        c.x[0] = value->paused ? 1 : 0;
    };
    support.method(link, "isPaused", paused, "B@:");
    support.method(link, "paused", paused, "B@:");
    support.method(link, "setPaused:", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        std::lock_guard lock(value->mutex);
        value->paused = (c.x[2] & 0xff) != 0;
    }, "v@:B");
    // -addToRunLoop:forMode: (NSRunLoop, NSRunLoopMode): one timer per link;
    // adding it to another run loop or mode adds that timer there too.
    support.method(link, "addToRunLoop:forMode:", [s, callout_symbol](CpuState& c, GuestMemory& m) {
        const auto self = c.x[0], run_loop = c.x[2], mode = c.x[3];
        const auto value = link_state(*s, self);
        const auto cf_loop = run_loop ? s->objc.send(run_loop, "getCFRunLoop").x0 : 0;
        if (!cf_loop || !mode) fail("-[CAMetalDisplayLink addToRunLoop:forMode:] needs an NSRunLoop and a mode");
        Address timer{};
        {
            std::lock_guard lock(value->mutex);
            if (value->invalidated) return;
            timer = value->timer;
        }
        if (!timer) {
            float preferred{};
            {
                std::lock_guard lock(value->mutex);
                preferred = value->preferred;
            }
            const auto period = link_period(preferred);
            // info: the link, not retained (-invalidate / -dealloc stop the timer).
            timer = make_timer(*s, m, callout_symbol, self, period, "CAMetalDisplayLink");
            std::lock_guard lock(value->mutex);
            value->timer = timer;
            value->period = period;
        }
        static_cast<void>(s->call(core_foundation_library, "_CFRunLoopAddTimer", {cf_loop, timer, mode}));
    }, "v@:@@");
    support.method(link, "removeFromRunLoop:forMode:", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        Address timer{};
        {
            std::lock_guard lock(value->mutex);
            timer = value->timer;
        }
        const auto cf_loop = c.x[2] ? s->objc.send(c.x[2], "getCFRunLoop").x0 : 0;
        if (timer && cf_loop && c.x[3]) static_cast<void>(s->call(core_foundation_library, "_CFRunLoopRemoveTimer", {cf_loop, timer, c.x[3]}));
    }, "v@:@@");
    support.method(link, "invalidate", [s](CpuState& c, GuestMemory&) {
        const auto value = link_state(*s, c.x[0]);
        Address timer{};
        {
            std::lock_guard lock(value->mutex);
            value->invalidated = true;
            timer = std::exchange(value->timer, 0);
        }
        stop_timer(*s, timer);
    }, "v@:");
    support.method(link, "dealloc", [s, link](CpuState& c, GuestMemory&) {
        const auto self = c.x[0];
        Address timer{}, layer{};
        if (const auto value = s->objc.find_host_state<DisplayLinkState>(self)) {
            std::lock_guard lock(value->mutex);
            value->invalidated = true;
            timer = std::exchange(value->timer, 0);
            layer = std::exchange(value->layer, 0);
        }
        stop_timer(*s, timer);
        s->release(layer);
        static_cast<void>(s->objc.send_super(self, link, s->objc.selector("dealloc")));
    }, "v@:");

    // CAMetalDisplayLinkUpdate.
    const auto update_state = [s](Address object) {
        auto value = object ? s->objc.find_host_state<UpdateState>(object) : nullptr;
        if (!value) fail("CAMetalDisplayLinkUpdate: not an update of this provider");
        return value;
    };
    support.method(update, "drawable", [update_state](CpuState& c, GuestMemory&) { c.x[0] = update_state(c.x[0])->drawable; }, "@@:");
    support.method(update, "targetTimestamp", [update_state](CpuState& c, GuestMemory&) {
        return_real(c, 0, update_state(c.x[0])->target);
    }, "d@:");
    support.method(update, "targetPresentationTimestamp", [update_state](CpuState& c, GuestMemory&) {
        return_real(c, 0, update_state(c.x[0])->presentation);
    }, "d@:");
    support.method(update, "dealloc", [s, update](CpuState& c, GuestMemory&) {
        const auto self = c.x[0];
        Address drawable{};
        if (const auto value = s->objc.find_host_state<UpdateState>(self)) drawable = std::exchange(value->drawable, 0);
        s->release(drawable);
        static_cast<void>(s->objc.send_super(self, update, s->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::support_kit
