#include "alchemy/darwin/media/media.hpp"
#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/media_support.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

// CoreMedia clocks, timebases and queues (<CoreMedia/CMSync.h>, <CoreMedia/CMBufferQueue.h>,
// <CoreMedia/CMSimpleQueue.h>, <CoreMedia/CMMemoryPool.h>):
// - CMClock: the host time clock (mach_absolute_time, nanoseconds) and audio clocks, which run on host time too
//   (they never drift from it).
// - CMTimebase: a time that moves at a rate against its source (a clock or another timebase), from an anchor;
//   set time, rate and anchors, effective rates through the chain, and timers: a CFRunLoopTimer added to a
//   timebase is put on its run loop (common modes) and fires when the timebase reaches the timer's fire time
//   (rescheduled whenever the rate or time changes; never while the timebase is stopped). Dispatch-source timers
//   and the timebase notifications are not provided.
// - CMSync: time, relative rate and time conversion between any two clocks or timebases.
// - CMSimpleQueue: a fixed-capacity FIFO of pointers. CMBufferQueue: a queue of CF objects ordered by the
//   callbacks' compare function (or FIFO), with end-of-data marking; the standard callbacks for sample buffers use
//   the CMSampleBuffer getters. Triggers are not provided.
// - CMMemoryPool: allocations come from the default allocator.
namespace alchemy::gui::fill {
namespace {

namespace cm = completion_kit::media;
using cm::CMTime;

struct Timer {
    Address timer{}, run_loop{};       // retained timer; its run loop
    CMTime fire = cm::time_invalid;
};
struct Clock final : ObjectPayload {};
struct Timebase final : ObjectPayload {
    std::mutex mutex;
    Address source{};                  // retained clock or timebase
    bool source_is_timebase{};
    double rate{};
    CMTime anchor_time = cm::time_zero, anchor_source = cm::time_zero;
    std::vector<Timer> timers;
};
struct SimpleQueue final : ObjectPayload {
    std::mutex mutex;
    std::deque<Address> items;
    std::int32_t capacity{};
};
struct BufferQueue final : ObjectPayload {
    std::mutex mutex;
    std::deque<Address> buffers;       // retained
    std::array<std::uint64_t, 9> callbacks{};   // CMBufferCallbacks: version, refcon, then the function pointers
    std::int64_t capacity{};
    bool end_of_data{};
};
struct MemoryPool final : ObjectPayload {};

// CMBufferCallbacks fields.
constexpr std::size_t cb_refcon = 1, cb_decode = 2, cb_presentation = 3, cb_duration = 4, cb_ready = 5, cb_compare = 6, cb_size = 8;

class Clocks final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    Address native(const char* name) {
        auto value = objc.find_class(name);
        if (!value) value = objc.register_native_class(name, objc.find_class("NSObject"), 8);
        return value;
    }
    template <class T> Address make(Address target, std::shared_ptr<T> value) {
        const auto object = objc.allocate(target);
        static_cast<void>(objc.host_state<T>(object, [&] { return value; }));
        return object;
    }
    static CMTime read_time(GuestMemory& m, Address at) {
        return {static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12),
                static_cast<std::int64_t>(m.read64(at + 16))};
    }
    static void write_time(GuestMemory& m, Address at, const CMTime& t) {
        m.write64(at, static_cast<std::uint64_t>(t.value));
        m.write32(at + 8, static_cast<std::uint32_t>(t.timescale));
        m.write32(at + 12, t.flags);
        m.write64(at + 16, static_cast<std::uint64_t>(t.epoch));
    }
    static CMTime host_now() { return cm::make(static_cast<std::int64_t>(completion_kit::media::host_time_now()), 1'000'000'000); }
    // The time of a clock or timebase now, and its rate against host time.
    CMTime now(Address object);
    double effective_rate(Address object);
    CMTime timebase_time(Timebase& tb, const CMTime& source_now) const {
        if (tb.rate == 0 || !cm::is_numeric(tb.anchor_time) || !cm::is_numeric(source_now)) return tb.anchor_time;
        const auto seconds = cm::seconds(tb.anchor_time) + (cm::seconds(source_now) - cm::seconds(tb.anchor_source)) * tb.rate;
        return cm::make_with_seconds(seconds, std::max(tb.anchor_time.timescale, 1'000'000'000));
    }
    // Re-anchors at the current time (before a rate change).
    void reanchor(Address object, Timebase& tb) {
        const auto source_now = now(tb.source);
        std::lock_guard lock(tb.mutex);
        tb.anchor_time = timebase_time(tb, source_now);
        tb.anchor_source = source_now;
        static_cast<void>(object);
    }
    void reschedule(Address object);
    void set_fire_date(Address timer, double absolute);
    double absolute_now() { return call_regs(core_foundation, "_CFAbsoluteTimeGetCurrent", {}).d(0); }
    Address common_modes() {
        const auto found = process.resolve(core_foundation, "_kCFRunLoopCommonModes");
        return found ? memory().read64(found->address) : 0;
    }
    CMTime callback_time(Address function, Address buffer, Address refcon) {
        if (!function) return cm::time_invalid;
        auto& m = memory();
        const auto out = process.allocate_guest(24, 8);
        CallRegs r{};
        r.x[0] = buffer;
        r.x[1] = refcon;
        r.indirect_result = out;
        static_cast<void>(EngineAdapter(process.calling_engine()).call_regs(function, r));
        const auto t = read_time(m, out);
        process.release_guest(out);
        return t;
    }

    Address clock_class_{}, timebase_class_{}, simple_class_{}, buffer_queue_class_{}, pool_class_{};
    Address host_clock_{};
    Address unsorted_callbacks_{}, sorted_callbacks_{};
};

CMTime Clocks::now(Address object) {
    if (!object) return host_now();
    if (const auto tb = objc.find_host_state<Timebase>(object)) {
        Address source{};
        {
            std::lock_guard lock(tb->mutex);
            source = tb->source;
        }
        const auto source_now = now(source);
        std::lock_guard lock(tb->mutex);
        return timebase_time(*tb, source_now);
    }
    return host_now();   // clocks run on host time
}

double Clocks::effective_rate(Address object) {
    const auto tb = object ? objc.find_host_state<Timebase>(object) : nullptr;
    if (!tb) return 1.0;
    Address source{};
    double rate{};
    {
        std::lock_guard lock(tb->mutex);
        source = tb->source;
        rate = tb->rate;
    }
    return rate * effective_rate(source);
}

void Clocks::set_fire_date(Address timer, double absolute) {
    CallRegs r{};
    r.x[0] = timer;
    r.set_d(0, absolute);
    static_cast<void>(call_regs(core_foundation, "_CFRunLoopTimerSetNextFireDate", r));
}

// Each timer fires when the timebase reaches its fire time: now + (fire - time) / rate in host seconds.
void Clocks::reschedule(Address object) {
    const auto tb = objc.find_host_state<Timebase>(object);
    if (!tb) return;
    const auto time = now(object);
    const auto rate = effective_rate(object);
    std::vector<std::pair<Address, double>> dates;
    {
        std::lock_guard lock(tb->mutex);
        for (const auto& t : tb->timers) {
            double delay = std::numeric_limits<double>::infinity();
            if (cm::is_numeric(t.fire) && rate != 0) {
                const auto ahead = (cm::seconds(t.fire) - cm::seconds(time)) / rate;
                delay = ahead < 0 ? 0 : ahead;
            }
            dates.emplace_back(t.timer, delay);
        }
    }
    const auto base = absolute_now();
    for (const auto& [timer, delay] : dates) set_fire_date(timer, std::isinf(delay) ? 1.0e15 : base + delay);
}

void Clocks::install() {
    clock_class_ = native("__AlchemyCMClock");
    timebase_class_ = native("__AlchemyCMTimebase");
    simple_class_ = native("__AlchemyCMSimpleQueue");
    buffer_queue_class_ = native("__AlchemyCMBufferQueue");
    pool_class_ = native("__AlchemyCMMemoryPool");
    register_type(clock_class_, TypeId::cm_clock);
    register_type(timebase_class_, TypeId::cm_timebase);
    register_type(simple_class_, TypeId::cm_simple_queue);
    register_type(buffer_queue_class_, TypeId::cm_buffer_queue);
    register_type(pool_class_, TypeId::cm_memory_pool);
    constant(core_media, "_CMClockGetTypeID", static_cast<Address>(TypeId::cm_clock));
    constant(core_media, "_CMTimebaseGetTypeID", static_cast<Address>(TypeId::cm_timebase));
    constant(core_media, "_CMSimpleQueueGetTypeID", static_cast<Address>(TypeId::cm_simple_queue));
    constant(core_media, "_CMBufferQueueGetTypeID", static_cast<Address>(TypeId::cm_buffer_queue));
    constant(core_media, "_CMMemoryPoolGetTypeID", static_cast<Address>(TypeId::cm_memory_pool));

    // ---- clocks ----
    raw(core_media, "_CMClockGetHostTimeClock", [this](CpuState& s, GuestMemory&) {
        if (!host_clock_) host_clock_ = make(clock_class_, std::make_shared<Clock>());   // lives for the process
        s.x[0] = host_clock_;
    });
    raw(core_media, "_CMAudioClockCreate", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status(-12745); return; }   // kCMClockError_MissingRequiredParameter
        m.write64(s.x[1], make(clock_class_, std::make_shared<Clock>()));
        s.x[0] = 0;
    });
    raw(core_media, "_CMClockGetTime", [](CpuState& s, GuestMemory& m) { write_time(m, s.x[8], host_now()); });
    // OSStatus CMClockGetAnchorTime(clock, CMTime *clockTimeOut, CMTime *referenceClockTimeOut): both now.
    raw(core_media, "_CMClockGetAnchorTime", [](CpuState& s, GuestMemory& m) {
        const auto t = host_now();
        if (s.x[1]) write_time(m, s.x[1], t);
        if (s.x[2]) write_time(m, s.x[2], t);
        s.x[0] = 0;
    });
    raw(core_media, "_CMClockMightDrift", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    raw(core_media, "_CMClockInvalidate", [](CpuState&, GuestMemory&) {});
    // CMClockConvertHostTimeToSystemUnits is media_gaps.cpp's (with the mach timebase).
    raw(core_media, "_CMClockMakeHostTimeFromSystemUnits", [](CpuState& s, GuestMemory& m) {
        write_time(m, s.x[8], cm::make(static_cast<std::int64_t>(s.x[0]), 1'000'000'000));
    });

    // ---- timebases ----
    const auto create = [this](GuestMemory& m, Address source, bool is_timebase, Address out) -> std::int32_t {
        if (!out || !source) {
            if (out) m.write64(out, 0);
            return -12748;   // kCMTimebaseError_MissingRequiredParameter
        }
        auto tb = std::make_shared<Timebase>();
        tb->source = retain(source);
        tb->source_is_timebase = is_timebase;
        tb->anchor_source = now(source);
        m.write64(out, make(timebase_class_, tb));
        return 0;
    };
    for (const auto* name : {"_CMTimebaseCreateWithSourceClock", "_CMTimebaseCreateWithMasterClock"})
        raw(core_media, name, [create](CpuState& s, GuestMemory& m) { s.x[0] = status(create(m, s.x[1], false, s.x[2])); });
    for (const auto* name : {"_CMTimebaseCreateWithSourceTimebase", "_CMTimebaseCreateWithMasterTimebase"})
        raw(core_media, name, [create](CpuState& s, GuestMemory& m) { s.x[0] = status(create(m, s.x[1], true, s.x[2])); });
    static_cast<void>(objc.add_native_method(timebase_class_, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto tb = objc.find_host_state<Timebase>(s.x[0])) {
            release(std::exchange(tb->source, 0));
            for (const auto& t : std::exchange(tb->timers, {})) release(t.timer);
        }
        static_cast<void>(objc.send_super(s.x[0], timebase_class_, objc.selector("dealloc")));
    }, false, "v@:"));
    const auto timebase = [this](Address object) {
        const auto tb = object ? objc.find_host_state<Timebase>(object) : nullptr;
        if (!tb) throw std::runtime_error("Alchemy: the object is not a CMTimebase");
        return tb;
    };
    raw(core_media, "_CMTimebaseGetTime", [this](CpuState& s, GuestMemory& m) { write_time(m, s.x[8], now(s.x[0])); });
    // CMTime CMTimebaseGetTimeWithTimeScale(timebase, CMTimeScale, CMTimeRoundingMethod)
    raw(core_media, "_CMTimebaseGetTimeWithTimeScale", [this](CpuState& s, GuestMemory& m) {
        write_time(m, s.x[8], cm::convert_scale(now(s.x[0]), static_cast<std::int32_t>(s.x[1])));
    });
    // OSStatus CMTimebaseGetTimeAndRate(timebase, CMTime *timeOut, Float64 *rateOut)
    raw(core_media, "_CMTimebaseGetTimeAndRate", [this, timebase](CpuState& s, GuestMemory& m) {
        const auto tb = timebase(s.x[0]);
        if (s.x[1]) write_time(m, s.x[1], now(s.x[0]));
        if (s.x[2]) {
            std::lock_guard lock(tb->mutex);
            m.write64(s.x[2], std::bit_cast<std::uint64_t>(tb->rate));
        }
        s.x[0] = 0;
    });
    raw(core_media, "_CMTimebaseSetTime", [this, timebase](CpuState& s, GuestMemory& m) {
        const auto tb = timebase(s.x[0]);
        const auto time = read_time(m, s.x[1]);
        if (!cm::is_valid(time)) { s.x[0] = status(-12749); return; }   // kCMTimebaseError_InvalidParameter
        const auto source_now = now(tb->source);
        {
            std::lock_guard lock(tb->mutex);
            tb->anchor_time = time;
            tb->anchor_source = source_now;
        }
        reschedule(s.x[0]);
        s.x[0] = 0;
    });
    // OSStatus CMTimebaseSetAnchorTime(timebase, CMTime timebaseTime, CMTime immediateSourceTime): both by reference.
    raw(core_media, "_CMTimebaseSetAnchorTime", [this, timebase](CpuState& s, GuestMemory& m) {
        const auto tb = timebase(s.x[0]);
        {
            std::lock_guard lock(tb->mutex);
            tb->anchor_time = read_time(m, s.x[1]);
            tb->anchor_source = read_time(m, s.x[2]);
        }
        reschedule(s.x[0]);
        s.x[0] = 0;
    });
    raw(core_media, "_CMTimebaseGetRate", [timebase](CpuState& s, GuestMemory&) {
        const auto tb = timebase(s.x[0]);
        std::lock_guard lock(tb->mutex);
        return_real(s, tb->rate);
    });
    raw(core_media, "_CMTimebaseGetEffectiveRate", [this](CpuState& s, GuestMemory&) { return_real(s, effective_rate(s.x[0])); });
    raw(core_media, "_CMTimebaseSetRate", [this, timebase](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto tb = timebase(object);
        const auto rate = real(s, 0);
        reanchor(object, *tb);
        {
            std::lock_guard lock(tb->mutex);
            tb->rate = rate;
        }
        reschedule(object);
        s.x[0] = 0;
    });
    // OSStatus CMTimebaseSetRateAndAnchorTime(timebase, Float64 rate, CMTime timebaseTime, CMTime immediateSourceTime)
    raw(core_media, "_CMTimebaseSetRateAndAnchorTime", [this, timebase](CpuState& s, GuestMemory& m) {
        const auto tb = timebase(s.x[0]);
        {
            std::lock_guard lock(tb->mutex);
            tb->rate = real(s, 0);
            tb->anchor_time = read_time(m, s.x[1]);
            tb->anchor_source = read_time(m, s.x[2]);
        }
        reschedule(s.x[0]);
        s.x[0] = 0;
    });
    // The source (+1): the clock or timebase it was made with; the clock at the root of the chain.
    const auto source_of = [this, timebase](Address object, int want) -> Address {   // 0 any, 1 clock, 2 timebase
        const auto tb = timebase(object);
        Address source{};
        bool is_timebase{};
        {
            std::lock_guard lock(tb->mutex);
            source = tb->source;
            is_timebase = tb->source_is_timebase;
        }
        if (want == 2 && !is_timebase) return 0;
        if (want == 1) {
            while (is_timebase) {
                const auto inner = objc.find_host_state<Timebase>(source);
                if (!inner) return 0;
                std::lock_guard lock(inner->mutex);
                source = inner->source;
                is_timebase = inner->source_is_timebase;
            }
        }
        return retain(source);
    };
    for (const auto* name : {"_CMTimebaseCopySource", "_CMTimebaseCopyMaster"})
        raw(core_media, name, [source_of](CpuState& s, GuestMemory&) { s.x[0] = source_of(s.x[0], 0); });
    for (const auto* name : {"_CMTimebaseCopySourceClock", "_CMTimebaseCopyMasterClock", "_CMTimebaseCopyUltimateSourceClock", "_CMTimebaseCopyUltimateMasterClock"})
        raw(core_media, name, [source_of](CpuState& s, GuestMemory&) { s.x[0] = source_of(s.x[0], 1); });
    for (const auto* name : {"_CMTimebaseCopySourceTimebase", "_CMTimebaseCopyMasterTimebase"})
        raw(core_media, name, [source_of](CpuState& s, GuestMemory&) { s.x[0] = source_of(s.x[0], 2); });
    raw(core_media, "_CMTimebaseNotificationBarrier", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // OSStatus CMTimebaseAddTimer(timebase, CFRunLoopTimerRef, CFRunLoopRef)
    raw(core_media, "_CMTimebaseAddTimer", [this, timebase](CpuState& s, GuestMemory&) {
        const auto tb = timebase(s.x[0]);
        if (!s.x[1] || !s.x[2]) { s.x[0] = status(-12748); return; }
        {
            std::lock_guard lock(tb->mutex);
            if (std::any_of(tb->timers.begin(), tb->timers.end(), [&](const Timer& t) { return t.timer == s.x[1]; })) { s.x[0] = 0; return; }
            tb->timers.push_back({retain(s.x[1]), s.x[2], cm::time_invalid});
        }
        set_fire_date(s.x[1], 1.0e15);   // the distant future, until a fire time is set
        static_cast<void>(call(core_foundation, "_CFRunLoopAddTimer", {s.x[2], s.x[1], common_modes()}));
        s.x[0] = 0;
    });
    raw(core_media, "_CMTimebaseRemoveTimer", [this, timebase](CpuState& s, GuestMemory&) {
        const auto tb = timebase(s.x[0]);
        Timer removed;
        {
            std::lock_guard lock(tb->mutex);
            const auto found = std::find_if(tb->timers.begin(), tb->timers.end(), [&](const Timer& t) { return t.timer == s.x[1]; });
            if (found == tb->timers.end()) { s.x[0] = 0; return; }
            removed = *found;
            tb->timers.erase(found);
        }
        static_cast<void>(call(core_foundation, "_CFRunLoopRemoveTimer", {removed.run_loop, removed.timer, common_modes()}));
        release(removed.timer);
        s.x[0] = 0;
    });
    // OSStatus CMTimebaseSetTimerNextFireTime(timebase, timer, CMTime fireTime, uint32_t flags)
    raw(core_media, "_CMTimebaseSetTimerNextFireTime", [this, timebase](CpuState& s, GuestMemory& m) {
        const auto tb = timebase(s.x[0]);
        {
            std::lock_guard lock(tb->mutex);
            const auto found = std::find_if(tb->timers.begin(), tb->timers.end(), [&](const Timer& t) { return t.timer == s.x[1]; });
            if (found == tb->timers.end()) { s.x[0] = status(-12749); return; }   // kCMTimebaseError_InvalidParameter: not added
            found->fire = read_time(m, s.x[2]);
        }
        reschedule(s.x[0]);
        s.x[0] = 0;
    });
    raw(core_media, "_CMTimebaseSetTimerToFireImmediately", [this, timebase](CpuState& s, GuestMemory&) {
        const auto tb = timebase(s.x[0]);
        {
            std::lock_guard lock(tb->mutex);
            if (std::none_of(tb->timers.begin(), tb->timers.end(), [&](const Timer& t) { return t.timer == s.x[1]; })) { s.x[0] = status(-12749); return; }
        }
        set_fire_date(s.x[1], absolute_now());
        s.x[0] = 0;
    });

    // ---- CMSync ----
    raw(core_media, "_CMSyncGetTime", [this](CpuState& s, GuestMemory& m) { write_time(m, s.x[8], now(s.x[0])); });
    raw(core_media, "_CMSyncGetRelativeRate", [this](CpuState& s, GuestMemory&) {
        const auto b = effective_rate(s.x[1]);
        return_real(s, b != 0 ? effective_rate(s.x[0]) / b : 0.0);
    });
    // CMTime CMSyncConvertTime(CMTime time, fromClockOrTimebase, toClockOrTimebase)
    raw(core_media, "_CMSyncConvertTime", [this](CpuState& s, GuestMemory& m) {
        const auto time = read_time(m, s.x[0]);
        const auto from_now = now(s.x[1]), to_now = now(s.x[2]);
        const auto from_rate = effective_rate(s.x[1]), to_rate = effective_rate(s.x[2]);
        CMTime out = cm::time_invalid;
        if (cm::is_numeric(time) && from_rate != 0) {
            const auto seconds = cm::seconds(to_now) + (cm::seconds(time) - cm::seconds(from_now)) * to_rate / from_rate;
            out = cm::make_with_seconds(seconds, std::max(time.timescale, 1'000'000'000));
        }
        write_time(m, s.x[8], out);
    });
    raw(core_media, "_CMSyncMightDrift", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // every clock is host time

    // ---- CMSimpleQueue ----
    constexpr std::int32_t sq_missing = -12771, sq_out_of_range = -12772, sq_full = -12773;
    raw(core_media, "_CMSimpleQueueCreate", [this](CpuState& s, GuestMemory& m) {
        const auto capacity = static_cast<std::int32_t>(s.x[1]);
        if (!s.x[2]) { s.x[0] = status(sq_missing); return; }
        m.write64(s.x[2], 0);
        if (capacity <= 0) { s.x[0] = status(sq_out_of_range); return; }
        auto q = std::make_shared<SimpleQueue>();
        q->capacity = capacity;
        m.write64(s.x[2], make(simple_class_, q));
        s.x[0] = 0;
    });
    const auto simple = [this](Address object) {
        const auto q = object ? objc.find_host_state<SimpleQueue>(object) : nullptr;
        if (!q) throw std::runtime_error("Alchemy: the object is not a CMSimpleQueue");
        return q;
    };
    raw(core_media, "_CMSimpleQueueEnqueue", [simple](CpuState& s, GuestMemory&) {
        const auto q = simple(s.x[0]);
        std::lock_guard lock(q->mutex);
        if (!s.x[1]) { s.x[0] = status(sq_missing); return; }
        if (static_cast<std::int32_t>(q->items.size()) >= q->capacity) { s.x[0] = status(sq_full); return; }
        q->items.push_back(s.x[1]);
        s.x[0] = 0;
    });
    raw(core_media, "_CMSimpleQueueDequeue", [simple](CpuState& s, GuestMemory&) {
        const auto q = simple(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->items.empty() ? 0 : q->items.front();
        if (!q->items.empty()) q->items.pop_front();
    });
    raw(core_media, "_CMSimpleQueueGetHead", [simple](CpuState& s, GuestMemory&) {
        const auto q = simple(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->items.empty() ? 0 : q->items.front();
    });
    raw(core_media, "_CMSimpleQueueGetCount", [simple](CpuState& s, GuestMemory&) {
        const auto q = simple(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->items.size();
    });
    raw(core_media, "_CMSimpleQueueGetCapacity", [simple](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(simple(s.x[0])->capacity); });
    raw(core_media, "_CMSimpleQueueReset", [simple](CpuState& s, GuestMemory&) {
        const auto q = simple(s.x[0]);
        std::lock_guard lock(q->mutex);
        q->items.clear();
        s.x[0] = 0;
    });

    // ---- CMBufferQueue ----
    constexpr std::int32_t bq_missing = -12761, bq_after_end = -12763, bq_full = -12764;
    raw(core_media, "_CMBufferQueueCreate", [this](CpuState& s, GuestMemory& m) {
        // (allocator, CMItemCount capacity, const CMBufferCallbacks *callbacks, CMBufferQueueRef *out)
        if (!s.x[3] || !s.x[2]) {
            if (s.x[3]) m.write64(s.x[3], 0);
            s.x[0] = status(bq_missing);
            return;
        }
        auto q = std::make_shared<BufferQueue>();
        q->capacity = static_cast<std::int64_t>(s.x[1]);
        for (std::size_t i = 0; i < q->callbacks.size(); ++i) q->callbacks[i] = m.read64(s.x[2] + 8 * i);
        m.write64(s.x[3], make(buffer_queue_class_, q));
        s.x[0] = 0;
    });
    static_cast<void>(objc.add_native_method(buffer_queue_class_, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto q = objc.find_host_state<BufferQueue>(s.x[0]))
            for (const auto b : std::exchange(q->buffers, {})) release(b);
        static_cast<void>(objc.send_super(s.x[0], buffer_queue_class_, objc.selector("dealloc")));
    }, false, "v@:"));
    const auto queue = [this](Address object) {
        const auto q = object ? objc.find_host_state<BufferQueue>(object) : nullptr;
        if (!q) throw std::runtime_error("Alchemy: the object is not a CMBufferQueue");
        return q;
    };
    raw(core_media, "_CMBufferQueueEnqueue", [this, queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        const auto buffer = s.x[1];
        if (!buffer) { s.x[0] = status(bq_missing); return; }
        std::deque<Address> current;
        std::array<std::uint64_t, 9> callbacks{};
        {
            std::lock_guard lock(q->mutex);
            if (q->end_of_data) { s.x[0] = status(bq_after_end); return; }
            if (q->capacity > 0 && static_cast<std::int64_t>(q->buffers.size()) >= q->capacity) { s.x[0] = status(bq_full); return; }
            current = q->buffers;
            callbacks = q->callbacks;
        }
        // The position: after every buffer that does not compare greater (FIFO among equals).
        std::size_t at = current.size();
        if (const auto compare = callbacks[cb_compare]) {
            for (std::size_t i = 0; i < current.size(); ++i) {
                const std::array<Address, 3> arguments{buffer, current[i], callbacks[cb_refcon]};
                if (static_cast<std::int64_t>(bridge->call_address(compare, arguments)) < 0) { at = i; break; }
            }
        }
        static_cast<void>(retain(buffer));
        std::lock_guard lock(q->mutex);
        q->buffers.insert(q->buffers.begin() + static_cast<std::ptrdiff_t>(std::min(at, q->buffers.size())), buffer);
        s.x[0] = 0;
    });
    raw(core_media, "_CMBufferQueueDequeueAndRetain", [queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->buffers.empty() ? 0 : q->buffers.front();   // the queue's reference passes to the caller
        if (!q->buffers.empty()) q->buffers.pop_front();
    });
    raw(core_media, "_CMBufferQueueDequeueIfDataReadyAndRetain", [this, queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        Address head{};
        std::uint64_t ready{}, refcon{};
        {
            std::lock_guard lock(q->mutex);
            if (q->buffers.empty()) { s.x[0] = 0; return; }
            head = q->buffers.front();
            ready = q->callbacks[cb_ready];
            refcon = q->callbacks[cb_refcon];
        }
        if (ready) {
            const std::array<Address, 2> arguments{head, refcon};
            if (!(bridge->call_address(ready, arguments) & 0xff)) { s.x[0] = 0; return; }
        }
        std::lock_guard lock(q->mutex);
        if (q->buffers.empty() || q->buffers.front() != head) { s.x[0] = 0; return; }
        q->buffers.pop_front();
        s.x[0] = head;
    });
    raw(core_media, "_CMBufferQueueGetHead", [queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->buffers.empty() ? 0 : q->buffers.front();
    });
    raw(core_media, "_CMBufferQueueCopyHead", [this, queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        Address head{};
        {
            std::lock_guard lock(q->mutex);
            head = q->buffers.empty() ? 0 : q->buffers.front();
        }
        s.x[0] = retain(head);
    });
    raw(core_media, "_CMBufferQueueIsEmpty", [queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->buffers.empty() ? 1 : 0;
    });
    raw(core_media, "_CMBufferQueueGetBufferCount", [queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->buffers.size();
    });
    raw(core_media, "_CMBufferQueueReset", [this, queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::deque<Address> dropped;
        {
            std::lock_guard lock(q->mutex);
            dropped = std::exchange(q->buffers, {});
            q->end_of_data = false;
        }
        for (const auto b : dropped) release(b);
        s.x[0] = 0;
    });
    raw(core_media, "_CMBufferQueueMarkEndOfData", [queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::lock_guard lock(q->mutex);
        q->end_of_data = true;
        s.x[0] = 0;
    });
    raw(core_media, "_CMBufferQueueContainsEndOfData", [queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->end_of_data ? 1 : 0;
    });
    raw(core_media, "_CMBufferQueueIsAtEndOfData", [queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::lock_guard lock(q->mutex);
        s.x[0] = q->end_of_data && q->buffers.empty() ? 1 : 0;
    });
    // The sum of the buffers' durations (the getDuration callback), and of their sizes (getSize).
    raw(core_media, "_CMBufferQueueGetDuration", [this, queue](CpuState& s, GuestMemory& m) {
        const auto q = queue(s.x[0]);
        std::deque<Address> buffers;
        std::array<std::uint64_t, 9> callbacks{};
        {
            std::lock_guard lock(q->mutex);
            buffers = q->buffers;
            callbacks = q->callbacks;
        }
        CMTime total = cm::time_zero;
        for (const auto b : buffers) {
            const auto d = callback_time(callbacks[cb_duration], b, callbacks[cb_refcon]);
            if (cm::is_numeric(d)) total = cm::add(total, d);
        }
        write_time(m, s.x[8], total);
    });
    raw(core_media, "_CMBufferQueueGetTotalSize", [this, queue](CpuState& s, GuestMemory&) {
        const auto q = queue(s.x[0]);
        std::deque<Address> buffers;
        std::array<std::uint64_t, 9> callbacks{};
        {
            std::lock_guard lock(q->mutex);
            buffers = q->buffers;
            callbacks = q->callbacks;
        }
        std::uint64_t total = 0;
        if (callbacks[cb_size])
            for (const auto b : buffers) {
                const std::array<Address, 2> arguments{b, callbacks[cb_refcon]};
                total += bridge->call_address(callbacks[cb_size], arguments);
            }
        s.x[0] = total;
    });
    // Standard callbacks for CMSampleBuffers (the CMSampleBuffer getters; sorted: by output presentation time).
    raw(core_media, "__AlchemyCMBufferQueueCompareOutputPTS", [this](CpuState& s, GuestMemory&) {
        const auto a = callback_time(symbol(core_media, "_CMSampleBufferGetOutputPresentationTimeStamp"), s.x[0], 0);
        const auto b = callback_time(symbol(core_media, "_CMSampleBufferGetOutputPresentationTimeStamp"), s.x[1], 0);
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(cm::compare(a, b)));
    });
    const auto callbacks_struct = [this](bool sorted) {
        auto& slot = sorted ? sorted_callbacks_ : unsorted_callbacks_;
        if (!slot) {
            slot = process.allocate_guest(72, 8);
            const std::array<Address, 9> fields{0, 0, symbol(core_media, "_CMSampleBufferGetDecodeTimeStamp"),
                                                symbol(core_media, "_CMSampleBufferGetPresentationTimeStamp"), symbol(core_media, "_CMSampleBufferGetDuration"),
                                                symbol(core_media, "_CMSampleBufferDataIsReady"),
                                                sorted ? symbol(core_media, "__AlchemyCMBufferQueueCompareOutputPTS") : 0, 0,
                                                symbol(core_media, "_CMSampleBufferGetTotalSampleSize")};
            for (std::size_t i = 0; i < fields.size(); ++i) process.main_engine().write64(slot + 8 * i, fields[i]);
        }
        return slot;
    };
    raw(core_media, "_CMBufferQueueGetCallbacksForUnsortedSampleBuffers", [callbacks_struct](CpuState& s, GuestMemory&) { s.x[0] = callbacks_struct(false); });
    raw(core_media, "_CMBufferQueueGetCallbacksForSampleBuffersSortedByOutputPTS", [callbacks_struct](CpuState& s, GuestMemory&) { s.x[0] = callbacks_struct(true); });

    // ---- CMMemoryPool ----
    raw(core_media, "_CMMemoryPoolCreate", [this](CpuState& s, GuestMemory&) { s.x[0] = make(pool_class_, std::make_shared<MemoryPool>()); });
    raw(core_media, "_CMMemoryPoolGetAllocator", [this](CpuState& s, GuestMemory&) {
        const auto found = process.resolve(core_foundation, "_kCFAllocatorDefault");
        s.x[0] = found ? memory().read64(found->address) : 0;
    });
    raw(core_media, "_CMMemoryPoolFlush", [](CpuState&, GuestMemory&) {});
    raw(core_media, "_CMMemoryPoolInvalidate", [](CpuState&, GuestMemory&) {});
}

} // namespace

void install_core_media_clocks(InstallContext& context) { std::make_shared<Clocks>(context)->install(); }

} // namespace alchemy::gui::fill
