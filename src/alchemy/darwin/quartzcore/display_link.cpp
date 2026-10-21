#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/darwin/quartzcore/presenter.hpp"
#include "alchemy/gui/layer_contents.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/host/ui.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

// CoreVideo CVDisplayLink. Each link owns a long-lived guest thread (the run
// loop's completion-thread helper, so the callback runs on a real guest
// pthread like CoreVideo's display-link thread). While the link runs, that
// thread waits on a high-resolution waitable timer for the next refresh of
// the link's display (its refresh rate in the screens() of
// layer_contents::presentation_server(); 60 Hz when the host reports none)
// and calls the output callback or handler block with
// CVTimeStamps on the mach_absolute_time clock (nanoseconds). Stopping is
// asynchronous: CVDisplayLinkStop returns at once and the thread skips every
// later frame. The thread ends with the process (or when the link is freed).
// ALCHEMY_TRACE_PRESENT=1 also logs each link's first frames and every 600th
// as "[alchemy display-link] link 0x<ref> display <id> frame <n> <kind>".
namespace alchemy::gui::support_kit {
namespace {

// CVReturn
constexpr std::int32_t cv_success = 0, cv_invalid_argument = -6661, cv_invalid_display = -6670,
    cv_already_running = -6671, cv_not_running = -6672, cv_callbacks_not_set = -6673;
// CVTimeStampFlags
constexpr std::uint64_t ts_video_time = 1U << 0, ts_host_time = 1U << 1, ts_refresh_period = 1U << 3,
    ts_rate_scalar = 1U << 4;
constexpr std::uint64_t cv_time_stamp_size = 80;
constexpr std::int64_t timescale = 1'000'000'000;   // nanoseconds

std::uint64_t cv_status(std::int32_t value) { return static_cast<std::uint32_t>(value); }

struct Link final : ObjectPayload {
    std::mutex mutex;
    std::uint32_t display{};
    double refresh_hz{60};
    Address callback{}, context{};   // CVDisplayLinkOutputCallback
    Address handler{};               // +1 _Block_copy'd CVDisplayLinkOutputHandler
    std::atomic<bool> running{false};
    std::atomic<bool> looping{false};
    std::atomic<std::uint64_t> generation{0};
    std::shared_ptr<CompletionThread> thread;
    Address stamps{};                // guest: now, output (80 bytes each), flags out (8)
    std::uint64_t frame{};
};

std::optional<ScreenInfo> find_screen(std::uint32_t display) {
    const auto server = layer_contents::presentation_server();
    if (!server) return std::nullopt;
    for (const auto& screen : server->screens())
        if (screen.id == display) return screen;
    return std::nullopt;
}

double refresh_of(const ScreenInfo& screen) {
    return screen.refresh_rate >= 1 && screen.refresh_rate <= 1000 ? screen.refresh_rate : 60.0;
}

void write_stamp(GuestMemory& memory, Address at, std::uint64_t host_ns, std::uint64_t frame, std::int64_t period_ns) {
    std::array<std::uint8_t, cv_time_stamp_size> bytes{};
    const auto put32 = [&](std::size_t offset, std::uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    const auto put64 = [&](std::size_t offset, std::uint64_t value) { std::memcpy(bytes.data() + offset, &value, 8); };
    put32(0, 0);                                                    // version
    put32(4, static_cast<std::uint32_t>(timescale));                // videoTimeScale
    put64(8, frame * static_cast<std::uint64_t>(period_ns));        // videoTime
    put64(16, host_ns);                                             // hostTime
    const double rate = 1.0;
    std::memcpy(bytes.data() + 24, &rate, 8);                       // rateScalar
    put64(32, static_cast<std::uint64_t>(period_ns));               // videoRefreshPeriod
    // 40..63 smpteTime (zero)
    put64(64, ts_video_time | ts_host_time | ts_refresh_period | ts_rate_scalar);   // flags
    memory.write_bytes(at, bytes);
}

std::uint64_t host_now_ns() { return static_cast<std::uint64_t>(monotonic_seconds() * 1e9); }

bool stopping(ProcessContext& process) {
    return process.requested_exit().has_value() || engine_stop_requested(process.calling_engine());
}

// The display-link thread's frame loop (one work item per Start; it returns
// when the link stops or the process stops).
void run_frames(Support& support, Address object, std::shared_ptr<Link> link, std::uint64_t generation) {
    auto& process = support.process;
    auto created = alchemy::host::ui::FrameTimer::create();
    if (!created) { link->looping = false; fail("CVDisplayLink: no frame timer (" + std::string(alchemy::host::name(created.error())) + ")"); }
    auto timer = std::move(*created);
    struct Close { Link& link; ~Close() { link.looping = false; } } close{*link};
    auto next = monotonic_seconds();
    for (;;) {
        if (stopping(process) || !link->running.load() || link->generation.load() != generation) return;
        const auto period = 1.0 / std::max(1.0, link->refresh_hz);
        next += period;
        const auto now = monotonic_seconds();
        if (next < now) next = now + period;   // late: skip missed frames
        if (const auto waited = timer.wait(static_cast<std::uint64_t>(std::max(0.0, next - now) * 1e9), 100); !waited)
            fail("CVDisplayLink: the frame timer failed (" + std::string(alchemy::host::name(waited.error())) + ")");
        if (stopping(process) || !link->running.load() || link->generation.load() != generation) return;
        Address callback{}, context{}, handler{};
        {
            std::lock_guard lock(link->mutex);
            callback = link->callback;
            context = link->context;
            handler = link->handler;
        }
        auto& memory = process.calling_engine();
        const auto period_ns = static_cast<std::int64_t>(std::llround(period * 1e9));
        const auto frame = ++link->frame;
        // A late wake-up (a loaded host) moves this refresh to now, so the
        // output time (the next refresh) is always after the current time.
        const auto woke = host_now_ns();
        const auto vsync = std::max(static_cast<std::uint64_t>(next * 1e9), woke);
        next = static_cast<double>(vsync) / 1e9;
        write_stamp(memory, link->stamps, woke, frame, period_ns);
        write_stamp(memory, link->stamps + cv_time_stamp_size, vsync + period_ns, frame + 1, period_ns);
        memory.write64(link->stamps + 2 * cv_time_stamp_size, 0);
        if (quartzcore::present_trace_enabled() && (frame <= 3 || frame % 600 == 0)) {
            std::fprintf(stderr, "[alchemy display-link] link 0x%llx display %u frame %llu %s\n",
                         static_cast<unsigned long long>(object), link->display, static_cast<unsigned long long>(frame),
                         handler ? "handler" : callback ? "callback" : "no callback");
            std::fflush(stderr);
        }
        const auto now_stamp = link->stamps, output_stamp = link->stamps + cv_time_stamp_size,
                   flags_out = link->stamps + 2 * cv_time_stamp_size;
        if (handler) {
            // ^CVReturn(CVDisplayLinkRef, const CVTimeStamp *, const CVTimeStamp *, CVOptionFlags, CVOptionFlags *)
            CallRegs registers;
            registers.x = {handler, object, now_stamp, output_stamp, 0, flags_out, 0, 0};
            static_cast<void>(EngineAdapter(memory).call_regs(memory.read64(handler + 16), registers));
        } else if (callback) {
            CallRegs registers;
            registers.x = {object, now_stamp, output_stamp, 0, flags_out, context, 0, 0};
            static_cast<void>(EngineAdapter(memory).call_regs(callback, registers));
        }
    }
}

std::uint64_t create_link(Support& support, Address cls, std::uint32_t display, Address out) {
    if (!out) return cv_status(cv_invalid_argument);
    const auto screen = find_screen(display);
    if (!screen) return cv_status(cv_invalid_display);
    auto link = std::make_shared<Link>();
    link->display = display;
    link->refresh_hz = refresh_of(*screen);
    link->stamps = support.process.allocate_guest(2 * cv_time_stamp_size + 8, 16);
    std::weak_ptr<Support> owner = support.shared_from_this();
    const auto stamps = link->stamps;
    const auto object = support.create(cls, link, [owner, stamps](Address self) {
        const auto keeper = owner.lock();
        if (!keeper) return;
        if (const auto state = keeper->objc.find_host_state<Link>(self)) {
            state->running = false;
            ++state->generation;
            Address handler{};
            {
                std::lock_guard lock(state->mutex);
                handler = std::exchange(state->handler, 0);
            }
            if (handler) static_cast<void>(keeper->call(libsystem_library, "__Block_release", {handler}));
            // The frame loop may still hold the stamps for one more frame:
            // the block stays allocated (it is small) while a thread exists.
            if (!state->thread) keeper->process.release_guest(stamps);
            state->thread.reset();   // the thread ends on its own (no join from a callback)
        }
    });
    support.memory().write64(out, object);
    return cv_success;
}

} // namespace

void install_display_link(Support& support) {
    const auto lib = core_video_library;
    const auto cls = support.declare_class("CVDisplayLink", "NSObject");
    auto* s = &support;
    const auto link_of = [s](Address object) {
        if (!object) fail("CVDisplayLink: NULL CVDisplayLinkRef");
        const auto link = s->objc.find_host_state<Link>(object);
        if (!link) fail("CVDisplayLink: the argument is not a CVDisplayLinkRef");
        return link;
    };
    support.function(lib, "_CVDisplayLinkGetTypeID", [](CpuState& c, GuestMemory&) { c.x[0] = 0x100; });
    support.function(lib, "_CVDisplayLinkCreateWithCGDisplay", [s, cls](CpuState& c, GuestMemory&) {
        c.x[0] = create_link(*s, cls, static_cast<std::uint32_t>(c.x[0]), c.x[1]);
    });
    support.function(lib, "_CVDisplayLinkCreateWithCGDisplays", [s, cls](CpuState& c, GuestMemory& m) {
        if (!c.x[0] || c.x[1] == 0) { c.x[0] = cv_status(cv_invalid_argument); return; }
        c.x[0] = create_link(*s, cls, m.read32(c.x[0]), c.x[2]);
    });
    support.function(lib, "_CVDisplayLinkCreateWithActiveCGDisplays", [s, cls](CpuState& c, GuestMemory&) {
        const auto server = layer_contents::presentation_server();
        if (!server || server->screens().empty()) { c.x[0] = cv_status(cv_invalid_display); return; }
        c.x[0] = create_link(*s, cls, server->screens().front().id, c.x[0]);
    });
    support.function(lib, "_CVDisplayLinkRetain", [s](CpuState& c, GuestMemory&) { c.x[0] = s->retain(c.x[0]); });
    support.function(lib, "_CVDisplayLinkRelease", [s](CpuState& c, GuestMemory&) { s->release(c.x[0]); });
    support.function(lib, "_CVDisplayLinkSetOutputCallback", [link_of](CpuState& c, GuestMemory&) {
        const auto link = link_of(c.x[0]);
        std::lock_guard lock(link->mutex);
        link->callback = c.x[1];
        link->context = c.x[2];
        c.x[0] = cv_success;
    });
    support.function(lib, "_CVDisplayLinkSetOutputHandler", [s, link_of](CpuState& c, GuestMemory&) {
        const auto link = link_of(c.x[0]);
        const auto copied = c.x[1] ? s->call(libsystem_library, "__Block_copy", {c.x[1]}) : 0;
        Address old{};
        {
            std::lock_guard lock(link->mutex);
            old = std::exchange(link->handler, copied);
        }
        if (old) static_cast<void>(s->call(libsystem_library, "__Block_release", {old}));
        c.x[0] = cv_success;
    });
    support.function(lib, "_CVDisplayLinkStart", [s, link_of](CpuState& c, GuestMemory&) {
        const auto object = c.x[0];
        const auto link = link_of(object);
        {
            std::lock_guard lock(link->mutex);
            if (!link->callback && !link->handler) { c.x[0] = cv_status(cv_callbacks_not_set); return; }
        }
        if (link->running.exchange(true)) { c.x[0] = cv_status(cv_already_running); return; }
        const auto generation = ++link->generation;
        if (!link->thread) link->thread = start_completion_thread(s->process, "com.apple.coreui.CVDisplayLink");
        link->looping = true;
        std::weak_ptr<Link> weak = link;
        link->thread->post([s, object, weak, generation] {
            if (const auto held = weak.lock()) run_frames(*s, object, held, generation);
        });
        c.x[0] = cv_success;
    });
    support.function(lib, "_CVDisplayLinkStop", [link_of](CpuState& c, GuestMemory&) {
        const auto link = link_of(c.x[0]);
        if (!link->running.exchange(false)) { c.x[0] = cv_status(cv_not_running); return; }
        ++link->generation;
        c.x[0] = cv_success;
    });
    support.function(lib, "_CVDisplayLinkIsRunning", [link_of](CpuState& c, GuestMemory&) {
        c.x[0] = link_of(c.x[0])->running.load() ? 1 : 0;
    });
    support.function(lib, "_CVDisplayLinkGetCurrentCGDisplay", [link_of](CpuState& c, GuestMemory&) {
        c.x[0] = link_of(c.x[0])->display;
    });
    support.function(lib, "_CVDisplayLinkSetCurrentCGDisplay", [link_of](CpuState& c, GuestMemory&) {
        const auto link = link_of(c.x[0]);
        const auto screen = find_screen(static_cast<std::uint32_t>(c.x[1]));
        if (!screen) { c.x[0] = cv_status(cv_invalid_display); return; }
        std::lock_guard lock(link->mutex);
        link->display = screen->id;
        link->refresh_hz = refresh_of(*screen);
        c.x[0] = cv_success;
    });
    // CVTime {int64 timeValue; int32 timeScale; int32 flags} in x0/x1.
    support.function(lib, "_CVDisplayLinkGetNominalOutputVideoRefreshPeriod", [link_of](CpuState& c, GuestMemory&) {
        const auto link = link_of(c.x[0]);
        c.x[0] = static_cast<std::uint64_t>(std::llround(1e9 / link->refresh_hz));
        c.x[1] = static_cast<std::uint32_t>(timescale);
    });
    support.function(lib, "_CVDisplayLinkGetOutputVideoLatency", [link_of](CpuState& c, GuestMemory&) {
        static_cast<void>(link_of(c.x[0]));
        c.x[0] = 0;
        c.x[1] = static_cast<std::uint32_t>(timescale);
    });
    support.function(lib, "_CVDisplayLinkGetActualOutputVideoRefreshPeriod", [link_of](CpuState& c, GuestMemory&) {
        const auto link = link_of(c.x[0]);
        return_real(c, 0, link->running.load() ? 1.0 / link->refresh_hz : 0.0);
    });
    support.function(lib, "_CVDisplayLinkGetCurrentTime", [link_of](CpuState& c, GuestMemory& m) {
        const auto link = link_of(c.x[0]);
        if (!c.x[1]) { c.x[0] = cv_status(cv_invalid_argument); return; }
        const auto period_ns = static_cast<std::int64_t>(std::llround(1e9 / link->refresh_hz));
        write_stamp(m, c.x[1], host_now_ns(), link->frame, period_ns);
        c.x[0] = cv_success;
    });
    install_metal_display_link(support);   // CAMetalDisplayLink (metal_display_link.cpp)
}

} // namespace alchemy::gui::support_kit
