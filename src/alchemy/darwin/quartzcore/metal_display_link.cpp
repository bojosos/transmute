#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/call_regs.hpp"

#include "alchemy/gui/layer_contents.hpp"
#include "alchemy/gui/window_server.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <bit>
#include <initializer_list>
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

} // namespace

void install_metal_display_link(Support& support) {
    auto* s = &support;
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_CADynamicRangeAutomatic", "automatic"}, {"_CADynamicRangeStandard", "standard"},
             {"_CADynamicRangeConstrainedHigh", "constrainedHigh"}, {"_CADynamicRangeHigh", "high"}})
        support.string_constant(quartzcore_library, symbol, value);
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
            const auto refresh = display_refresh();
            float preferred{};
            {
                std::lock_guard lock(value->mutex);
                preferred = value->preferred;
            }
            const double rate = preferred > 0 ? std::min<double>(preferred, refresh) : refresh;
            const auto period = 1.0 / rate;
            const auto callout = s->process.resolve(quartzcore_library, callout_symbol);
            if (!callout) fail("CAMetalDisplayLink: the timer callout is not exported");
            const auto context = s->process.allocate_guest(40, 8);   // CFRunLoopTimerContext
            m.fill_bytes(context, 0, 40);
            m.write64(context + 8, self);                              // info (not retained: -invalidate/-dealloc stop the timer)
            CallRegs create;
            create.x[0] = 0;
            create.set_d(0, absolute_time() + period);   // fireDate
            create.set_d(1, period);
            create.x[1] = 0;   // flags
            create.x[2] = 0;   // order
            create.x[3] = callout->address;
            create.x[4] = context;
            timer = s->call(core_foundation_library, "_CFRunLoopTimerCreate", create).x0;
            s->process.release_guest(context);
            if (!timer) fail("CAMetalDisplayLink: CFRunLoopTimerCreate failed");
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
