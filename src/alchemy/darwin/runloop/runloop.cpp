#include "alchemy/darwin/runloop/runloop_internal.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/win32_nesting.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/ui.hpp"
#include "alchemy/perf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <tuple>
#include <utility>


// Main run loop and per-thread run loops (owner: A2; installation: install.cpp).
// See include/alchemy/gui/runloop.hpp for the model; CFRunLoop semantics
// follow Apple's public CFRunLoop and NSRunLoop reference documentation.
namespace alchemy::gui {

double monotonic_seconds() noexcept {
    // The base of Alchemy's mach_absolute_time and dispatch clocks.
    return static_cast<double>(host::time::monotonic_ns()) / 1e9;
}

namespace detail {
namespace {
// A blocked wait wakes at least this often to observe a process stop.
constexpr double stop_poll_seconds = 0.05;
std::size_t maximum_wait_handles() { return host::ui::wait_limit() - 1; }   // the wake event takes one slot

// ---- host modal loops -------------------------------------------------------
// While the host runs its own modal loop on the main thread, the guest's main
// loop must keep servicing timers, the main queue and performs in the mode
// AppKit would run (macOS runs the loop inside every modal session):
//   - window size/move and menu tracking: the window server's modal_tick
//     re-enters run_once in NSEventTrackingRunLoopMode (window.cpp);
//   - dialog loops (the task dialog behind -[NSAlert runModal], other modal
//     dialogs): the host's dialog-loop hook (host::ui; on win32 a
//     WH_MSGFILTER hook arming a thread timer) ticks one
//     NSModalPanelRunLoopMode step from inside that loop.
// In both cases the host loop owns the message queue: a step taken inside it
// never pumps messages itself (removing the size loop's WM_LBUTTONUP or a
// dialog's keys would break the loop), and it never blocks on input.
constexpr std::uint32_t modal_panel_period_ms = 15;

struct ModalPump {        // main thread only
    RunLoopImpl* loop{};  // the main loop that installed the hook
    bool hooked{};
    bool hook_failed{};
    int depth{};          // > 0 while a modal step runs
    std::exception_ptr pending;
};
ModalPump modal_pump;

// The hook ticks while a live main loop has no error waiting.
void update_modal_ticks() noexcept { host::ui::enable_dialog_loop_ticks(modal_pump.loop && !modal_pump.pending); }

void modal_panel_tick() {
    auto* const loop = modal_pump.loop;
    if (!loop || modal_pump.depth || modal_pump.pending) return;
    ++modal_pump.depth;
    try {
        static_cast<void>(loop->run_once(modal_panel_mode, 0, false));
    } catch (...) {
        // Never unwind through the host's dialog loop: the error surfaces from
        // the next main-loop step after the dialog returns.
        modal_pump.pending = std::current_exception();
        update_modal_ticks();
    }
    --modal_pump.depth;
}

// True on the main thread while the host (not the run loop) owns the message
// queue. Only a thread that is already a GUI thread can be in such a loop;
// the check never converts a headless thread.
bool in_win32_modal_loop() {
    if (modal_pump.depth) return true;
    // Modal loops run inside a window procedure (win32_nesting.hpp).
    if (!window_procedure_depth) return false;
    return host::ui::in_modal_loop();
}
} // namespace

RunLoopImpl::RunLoopImpl(ProcessContext& process, bool main)
    : process_(process), main_(main), owner_thread_(host::process::current_thread_id()) {
    const auto event = host::sync::create_event(false, false);
    if (!event) throw std::runtime_error("run loop wake event: " + std::string(host::name(event.error())));
    wake_event_ = std::shared_ptr<void>(reinterpret_cast<void*>(event->value),
                                        [](void* handle) { host::close(host::Handle{reinterpret_cast<std::uintptr_t>(handle)}); });
    common_.emplace(default_mode);
    // AppKit adds its modal and event-tracking modes to the main thread's
    // common set (NSRunLoopCommonModes).
    if (main_) { common_.emplace(event_tracking_mode); common_.emplace(modal_panel_mode); }
    known_modes_ = common_;
}

RunLoopImpl::~RunLoopImpl() {
    if (modal_pump.loop != this) return;
    // Unhooking works only on the hooking thread; elsewhere the hook stays
    // inert (no loop).
    if (on_owner_thread()) {
        host::ui::remove_dialog_loop_hook();
        modal_pump.hooked = false;
    }
    modal_pump.loop = nullptr;
    modal_pump.pending = nullptr;
    update_modal_ticks();
}

// The dialog-loop hook is installed by the main loop's first step on a GUI
// thread (after the guest created a window or otherwise used user32), so a
// headless process never becomes a GUI thread because of the run loop.
void RunLoopImpl::prepare_modal_loops() {
    if (!main_) return;
    if (modal_pump.pending && !modal_pump.depth) {
        auto pending = std::exchange(modal_pump.pending, nullptr);
        update_modal_ticks();
        std::rethrow_exception(pending);
    }
    if (modal_pump.hooked) {   // the newest main loop is the live one
        modal_pump.loop = this;
        update_modal_ticks();
        return;
    }
    if (modal_pump.hook_failed || !host::ui::is_gui_thread()) return;
    if (const auto installed = host::ui::install_dialog_loop_hook(modal_panel_tick, modal_panel_period_ms); !installed) {
        modal_pump.hook_failed = true;
        std::fprintf(stderr, "Alchemy run loop: the dialog-loop hook failed (%s); "
                             "timers do not run while a modal dialog is open\n", std::string(host::name(installed.error())).c_str());
        return;
    }
    modal_pump.hooked = true;
    modal_pump.loop = this;
    update_modal_ticks();
}

bool RunLoopImpl::on_owner_thread() const noexcept { return host::process::current_thread_id() == owner_thread_; }

bool RunLoopImpl::in_mode(const ModeSet& modes, std::string_view mode) const {
    return modes.contains(mode) || (modes.contains(common_modes) && common(mode));
}

// CFRunLoop finishes a mode with no sources and no timers. The main loop is
// never empty in a common mode (the main dispatch queue is a source there)
// nor while the window server's message dispatcher is registered.
bool RunLoopImpl::mode_empty(std::string_view mode) const {
    if (main_ && (common(mode) || dispatcher_)) return false;
    for (const auto& [id, timer] : timers_) if (in_mode(timer.modes, mode)) return false;
    for (const auto& [id, source] : sources_) if (source.handle || in_mode(source.modes, mode)) return false;
    for (const auto& block : blocks_) if (in_mode(block.modes, mode)) return false;
    return true;
}

bool RunLoopImpl::stop_requested() const {
    return process_.requested_exit().has_value() || engine_stop_requested(process_.calling_engine());
}

RunResult RunLoopImpl::run_once(std::string_view mode_view, double timeout, bool return_after_source_handled) {
    if (!on_owner_thread()) throw std::logic_error("a run loop runs only on the thread that owns it");
    if (mode_view == common_modes) throw std::invalid_argument("a run loop cannot run in kCFRunLoopCommonModes");
    if (std::isnan(timeout)) throw std::invalid_argument("run loop timeout is NaN");
    if (stop_requested()) return RunResult::stopped;
    prepare_modal_loops();
    const std::string mode(mode_view);
    const auto start = monotonic_seconds();
    const double deadline = timeout <= 0 ? start : std::isinf(timeout) ? distant_future : start + timeout;
    std::size_t index{};   // this run's frame; stable, nested runs pop before it resumes
    {
        std::lock_guard lock(mutex_);
        known_modes_.insert(mode);
        if (mode_empty(mode)) return RunResult::finished;
        index = frames_.size();
        frames_.push_back({mode, false});
    }
    // Restores the frame stack when a callback's exception leaves the step.
    struct Frame {
        RunLoopImpl& self;
        ~Frame() { self.waiting_ = false; std::lock_guard lock(self.mutex_); self.frames_.pop_back(); }
    } frame{*this};
    notify(activity::entry, mode);
    RunResult result{};
    for (;;) {
        if (take_stop(index)) { result = RunResult::stopped; break; }
        notify(activity::before_timers, mode);
        run_timers(mode);
        notify(activity::before_sources, mode);
        bool handled = run_blocks(mode);
        // Only the first signalled source runs when the caller returns after one.
        handled = run_sources(mode) || handled;
        if (!(handled && return_after_source_handled)) handled = drain_main_queue(mode) || handled;
        if (!(handled && return_after_source_handled)) handled = dispatch_messages() || handled;
        const auto now = monotonic_seconds();
        if (handled && return_after_source_handled) result = RunResult::handled_source;
        else if (take_stop(index) || stop_requested()) result = RunResult::stopped;
        else if (now >= deadline) result = RunResult::timed_out;
        else { std::lock_guard lock(mutex_); if (mode_empty(mode)) result = RunResult::finished; }
        if (result != RunResult{}) break;
        if (handled) continue; // Poll again before blocking, as CFRunLoop does after handling a source.
        const auto until = std::min(deadline, next_timer_fire(mode));
        notify(activity::before_waiting, mode);
        waiting_ = true;
        wait(until);
        waiting_ = false;
        notify(activity::after_waiting, mode);
    }
    notify(activity::exit, mode);
    return result;
}

void RunLoopImpl::wait(double until) {
    // A step nested in a Win32 modal loop cannot dispatch input, so it waits
    // for its handles and timers only (the input waits for the modal loop).
    const bool modal = main_ && in_win32_modal_loop();
    perf::Scope timed(perf::runloop_wait_ticks, perf::runloop_waits);
    for (;;) {
        std::vector<host::Handle> handles{host::Handle{reinterpret_cast<std::uintptr_t>(wake_event_.get())}};
        std::vector<SourceId> owners{0};
        bool messages{};
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, source] : sources_) {
                if (!source.handle) continue;
                if (source.signaled) return; // Already ready; the next step runs it.
                handles.push_back(host::Handle{reinterpret_cast<std::uintptr_t>(source.handle)});
                owners.push_back(id);
            }
            messages = main_ && dispatcher_ && !modal;
        }
        const auto now = monotonic_seconds();
        if (now >= until) return;
        const auto slice = std::min(until - now, stop_poll_seconds);
        const auto milliseconds = static_cast<std::uint32_t>(std::max(1.0, std::ceil(slice * 1000.0)));
        // The main thread also wakes for host input (that arrived before the wait as well).
        const auto result = host::ui::wait_for_input(handles, milliseconds, messages);
        if (!result) {
            if (result.error() == host::Error::timed_out) {
                if (innermost_stopped() || stop_requested()) return;
                continue;
            }
            if (result.error() == host::Error::deadlock) throw std::runtime_error("run loop wait handle is an abandoned mutex");
            throw std::runtime_error("run loop wait failed (" + std::string(host::name(result.error())) + ")");
        }
        if (*result >= 1 && *result < handles.size()) {
            // The wait consumed an auto-reset handle's signal: record it.
            std::lock_guard lock(mutex_);
            if (const auto found = sources_.find(owners[*result]); found != sources_.end()) found->second.signaled = true;
        }
        return;
    }
}

void RunLoopImpl::notify(std::uint32_t activity, std::string_view mode) {
    std::vector<std::tuple<std::int64_t, ObserverId, std::shared_ptr<std::function<void(std::uint32_t)>>>> ready;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, observer] : observers_)
            if ((observer.activities & activity) && !observer.firing && in_mode(observer.modes, mode))
                ready.emplace_back(observer.order, id, observer.callback);
    }
    std::sort(ready.begin(), ready.end(), [](const auto& left, const auto& right) {
        return std::tie(std::get<0>(left), std::get<1>(left)) < std::tie(std::get<0>(right), std::get<1>(right));
    });
    for (const auto& [order, id, callback] : ready) {
        static_cast<void>(order);
        {
            std::lock_guard lock(mutex_);
            const auto found = observers_.find(id);
            if (found == observers_.end() || found->second.firing) continue;
            found->second.firing = true;
        }
        struct Done {
            RunLoopImpl& self; ObserverId id;
            ~Done() {
                std::lock_guard lock(self.mutex_);
                const auto found = self.observers_.find(id);
                if (found == self.observers_.end()) return;
                found->second.firing = false;
                if (!found->second.repeats) self.observers_.erase(found);
            }
        } done{*this, id};
        (*callback)(activity);
    }
}

void RunLoopImpl::run_timers(std::string_view mode) {
    const auto now = monotonic_seconds();
    std::vector<std::tuple<double, std::int64_t, TimerId>> due;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, timer] : timers_)
            if (!timer.firing && timer.fire <= now && in_mode(timer.modes, mode)) due.emplace_back(timer.fire, timer.order, id);
    }
    std::sort(due.begin(), due.end());
    for (const auto& [fire, order, id] : due) {
        static_cast<void>(fire); static_cast<void>(order);
        std::shared_ptr<std::function<void(TimerId)>> callback;
        {
            std::lock_guard lock(mutex_);
            const auto found = timers_.find(id);
            if (found == timers_.end() || found->second.firing || found->second.fire > now) continue;
            auto& timer = found->second;
            timer.firing = true;
            timer.rescheduled = false;
            callback = timer.callback;
            // Repeating timers keep their phase and skip missed periods.
            if (timer.interval > 0) timer.fire += (std::floor((now - timer.fire) / timer.interval) + 1) * timer.interval;
        }
        struct Done {
            RunLoopImpl& self; TimerId id;
            ~Done() {
                std::lock_guard lock(self.mutex_);
                const auto found = self.timers_.find(id);
                if (found == self.timers_.end()) return;
                found->second.firing = false;
                if (found->second.interval <= 0 && !found->second.rescheduled) self.timers_.erase(found);
            }
        } done{*this, id};
        (*callback)(id);
    }
}

bool RunLoopImpl::run_blocks(std::string_view mode) {
    std::vector<Block> ready;
    {
        std::lock_guard lock(mutex_);
        for (auto block = blocks_.begin(); block != blocks_.end();) {
            if (in_mode(block->modes, mode)) { ready.push_back(std::move(*block)); block = blocks_.erase(block); }
            else ++block;
        }
    }
    bool counted{};
    for (std::size_t index = 0; index < ready.size(); ++index) {
        try { ready[index].work(); }
        catch (...) {
            // Blocks after the failing one still run later, in order.
            std::lock_guard lock(mutex_);
            blocks_.insert(blocks_.begin(), std::make_move_iterator(ready.begin() + static_cast<std::ptrdiff_t>(index) + 1),
                           std::make_move_iterator(ready.end()));
            throw;
        }
        counted = counted || ready[index].counts;
    }
    return counted;
}

bool RunLoopImpl::run_sources(std::string_view mode) {
    std::vector<std::tuple<std::int64_t, SourceId, std::shared_ptr<std::function<void()>>>> ready;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, source] : sources_) {
            if (source.handle) {
                const host::Handle handle{reinterpret_cast<std::uintptr_t>(source.handle)};
                if (!source.signaled && host::ui::wait_for_input({&handle, 1}, 0, false).value_or(1) == 0) source.signaled = true;
            } else if (!in_mode(source.modes, mode)) {
                continue;
            }
            if (source.signaled) ready.emplace_back(source.order, id, source.perform);
        }
    }
    std::sort(ready.begin(), ready.end(), [](const auto& left, const auto& right) {
        return std::tie(std::get<0>(left), std::get<1>(left)) < std::tie(std::get<0>(right), std::get<1>(right));
    });
    bool handled{};
    for (const auto& [order, id, perform] : ready) {
        static_cast<void>(order);
        {
            std::lock_guard lock(mutex_);
            const auto found = sources_.find(id);
            if (found == sources_.end() || !found->second.signaled) continue;
            found->second.signaled = false;
        }
        handled = true;
        (*perform)();
    }
    return handled;
}

bool RunLoopImpl::drain_main_queue(std::string_view mode) {
    // The main dispatch queue is a source of the main loop's common modes.
    if (!main_) return false;
    { std::lock_guard lock(mutex_); if (!common(mode)) return false; }
    const auto pending = dispatch_main_queue_length(process_);
    return pending && drain_dispatch_main_queue(process_, pending) != 0;
}

bool RunLoopImpl::dispatch_messages() {
    std::shared_ptr<std::function<bool()>> dispatcher;
    { std::lock_guard lock(mutex_); dispatcher = dispatcher_; }
    // Inside a Win32 modal loop that loop dispatches the messages.
    return main_ && dispatcher && !in_win32_modal_loop() && (*dispatcher)();
}

void RunLoopImpl::stop() {
    {
        std::lock_guard lock(mutex_);
        if (frames_.empty()) return;   // CFRunLoopStop outside a run does nothing
        frames_.back().stopped = true;
    }
    wake();
}

bool RunLoopImpl::take_stop(std::size_t index) {
    std::lock_guard lock(mutex_);
    return index < frames_.size() && std::exchange(frames_[index].stopped, false);
}

bool RunLoopImpl::innermost_stopped() const {
    std::lock_guard lock(mutex_);
    return !frames_.empty() && frames_.back().stopped;
}
void RunLoopImpl::wake() { static_cast<void>(host::sync::set_event(host::Handle{reinterpret_cast<std::uintptr_t>(wake_event_.get())})); }
std::function<void()> RunLoopImpl::waker() const {
    return [event = wake_event_] { static_cast<void>(host::sync::set_event(host::Handle{reinterpret_cast<std::uintptr_t>(event.get())})); };
}

std::string RunLoopImpl::current_mode() const {
    std::lock_guard lock(mutex_);
    return frames_.empty() ? std::string{} : frames_.back().mode;
}

std::vector<std::string> RunLoopImpl::all_modes() const {
    std::lock_guard lock(mutex_);
    ModeSet modes = known_modes_;
    const auto add = [&](const ModeSet& set) { for (const auto& mode : set) if (mode != common_modes) modes.insert(mode); };
    for (const auto& [id, timer] : timers_) add(timer.modes);
    for (const auto& [id, source] : sources_) add(source.modes);
    for (const auto& [id, observer] : observers_) add(observer.modes);
    return {modes.begin(), modes.end()};
}

void RunLoopImpl::add_common_mode(std::string_view mode) {
    if (mode == common_modes) throw std::invalid_argument("kCFRunLoopCommonModes is not a mode");
    std::lock_guard lock(mutex_);
    common_.emplace(mode);
    known_modes_.emplace(mode);
}

TimerId RunLoopImpl::add_timer(double fire, double interval, std::string_view mode, std::function<void(TimerId)> callback) {
    if (!callback) throw std::invalid_argument("run loop timer without a callback");
    if (std::isnan(fire) || std::isnan(interval)) throw std::invalid_argument("run loop timer time is NaN");
    TimerId id;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        Timer timer;
        timer.fire = fire;
        timer.interval = interval > 0 && std::isfinite(interval) ? interval : 0;
        timer.modes.emplace(mode);
        timer.callback = std::make_shared<std::function<void(TimerId)>>(std::move(callback));
        timers_.emplace(id, std::move(timer));
    }
    wake(); // A blocked loop recomputes its deadline.
    return id;
}

void RunLoopImpl::set_timer_fire(TimerId timer, double fire) {
    if (std::isnan(fire)) throw std::invalid_argument("run loop timer time is NaN");
    {
        std::lock_guard lock(mutex_);
        const auto found = timers_.find(timer);
        if (found == timers_.end()) return;
        found->second.fire = fire;
        found->second.rescheduled = true;
    }
    wake();
}

void RunLoopImpl::cancel_timer(TimerId timer) { std::lock_guard lock(mutex_); timers_.erase(timer); }

double RunLoopImpl::next_timer_fire(std::string_view mode) const {
    std::lock_guard lock(mutex_);
    double next = distant_future;
    for (const auto& [id, timer] : timers_)
        if (!timer.firing && in_mode(timer.modes, mode)) next = std::min(next, timer.fire);
    return next;
}

void RunLoopImpl::add_timer_mode(TimerId timer, std::string_view mode) {
    { std::lock_guard lock(mutex_); if (const auto found = timers_.find(timer); found != timers_.end()) found->second.modes.emplace(mode); }
    wake();
}

bool RunLoopImpl::remove_timer_mode(TimerId timer, std::string_view mode) {
    std::lock_guard lock(mutex_);
    const auto found = timers_.find(timer);
    if (found == timers_.end()) return false;
    if (const auto entry = found->second.modes.find(mode); entry != found->second.modes.end()) found->second.modes.erase(entry);
    return !found->second.modes.empty();
}

bool RunLoopImpl::timer_in_mode(TimerId timer, std::string_view mode) const {
    std::lock_guard lock(mutex_);
    const auto found = timers_.find(timer);
    return found != timers_.end() && found->second.modes.contains(mode);
}

double RunLoopImpl::timer_fire(TimerId timer) const {
    std::lock_guard lock(mutex_);
    const auto found = timers_.find(timer);
    return found == timers_.end() ? distant_future : found->second.fire;
}

void RunLoopImpl::set_timer_order(TimerId timer, std::int64_t order) {
    std::lock_guard lock(mutex_);
    if (const auto found = timers_.find(timer); found != timers_.end()) found->second.order = order;
}

SourceId RunLoopImpl::add_source(std::string_view mode, std::function<void()> perform) {
    if (!perform) throw std::invalid_argument("run loop source without a perform callback");
    std::lock_guard lock(mutex_);
    const auto id = next_id_++;
    Source source;
    source.modes.emplace(mode);
    source.perform = std::make_shared<std::function<void()>>(std::move(perform));
    sources_.emplace(id, std::move(source));
    return id;
}

void RunLoopImpl::add_source_mode(SourceId source, std::string_view mode) {
    std::lock_guard lock(mutex_);
    const auto found = sources_.find(source);
    if (found == sources_.end()) throw std::invalid_argument("unknown run loop source");
    found->second.modes.emplace(mode);
}

bool RunLoopImpl::remove_source_mode(SourceId source, std::string_view mode) {
    std::lock_guard lock(mutex_);
    const auto found = sources_.find(source);
    if (found == sources_.end()) return false;
    if (const auto entry = found->second.modes.find(mode); entry != found->second.modes.end()) found->second.modes.erase(entry);
    return !found->second.modes.empty();
}

bool RunLoopImpl::source_in_mode(SourceId source, std::string_view mode) const {
    std::lock_guard lock(mutex_);
    const auto found = sources_.find(source);
    return found != sources_.end() && found->second.modes.contains(mode);
}

void RunLoopImpl::set_source_order(SourceId source, std::int64_t order) {
    std::lock_guard lock(mutex_);
    if (const auto found = sources_.find(source); found != sources_.end()) found->second.order = order;
}

void RunLoopImpl::signal_source(SourceId source) {
    {
        std::lock_guard lock(mutex_);
        const auto found = sources_.find(source);
        if (found == sources_.end()) return;
        found->second.signaled = true;
    }
    wake();
}

void RunLoopImpl::remove_source(SourceId source) { std::lock_guard lock(mutex_); sources_.erase(source); }

SourceId RunLoopImpl::add_wait_handle(void* handle, std::function<void()> perform) {
    if (!handle || handle == reinterpret_cast<void*>(~std::uintptr_t{0})) throw std::invalid_argument("run loop wait source needs a handle");
    if (!perform) throw std::invalid_argument("run loop source without a perform callback");
    SourceId id;
    {
        std::lock_guard lock(mutex_);
        const auto count = std::count_if(sources_.begin(), sources_.end(), [](const auto& entry) { return entry.second.handle != nullptr; });
        if (static_cast<std::size_t>(count) >= maximum_wait_handles())
            throw std::runtime_error("run loop supports at most " + std::to_string(maximum_wait_handles()) + " wait handles");
        id = next_id_++;
        Source source;
        source.handle = handle;
        source.perform = std::make_shared<std::function<void()>>(std::move(perform));
        sources_.emplace(id, std::move(source));
    }
    wake();
    return id;
}

ObserverId RunLoopImpl::add_observer(std::uint32_t activities, std::string_view mode, bool repeats,
                                     std::function<void(std::uint32_t)> callback) {
    if (!callback) throw std::invalid_argument("run loop observer without a callback");
    std::lock_guard lock(mutex_);
    const auto id = next_id_++;
    Observer observer;
    observer.activities = activities;
    observer.modes.emplace(mode);
    observer.repeats = repeats;
    observer.callback = std::make_shared<std::function<void(std::uint32_t)>>(std::move(callback));
    observers_.emplace(id, std::move(observer));
    return id;
}

void RunLoopImpl::add_observer_mode(ObserverId observer, std::string_view mode) {
    std::lock_guard lock(mutex_);
    const auto found = observers_.find(observer);
    if (found == observers_.end()) throw std::invalid_argument("unknown run loop observer");
    found->second.modes.emplace(mode);
}

bool RunLoopImpl::remove_observer_mode(ObserverId observer, std::string_view mode) {
    std::lock_guard lock(mutex_);
    const auto found = observers_.find(observer);
    if (found == observers_.end()) return false;
    if (const auto entry = found->second.modes.find(mode); entry != found->second.modes.end()) found->second.modes.erase(entry);
    return !found->second.modes.empty();
}

bool RunLoopImpl::observer_in_mode(ObserverId observer, std::string_view mode) const {
    std::lock_guard lock(mutex_);
    const auto found = observers_.find(observer);
    return found != observers_.end() && found->second.modes.contains(mode);
}

void RunLoopImpl::set_observer_order(ObserverId observer, std::int64_t order) {
    std::lock_guard lock(mutex_);
    if (const auto found = observers_.find(observer); found != observers_.end()) found->second.order = order;
}

void RunLoopImpl::remove_observer(ObserverId observer) { std::lock_guard lock(mutex_); observers_.erase(observer); }

void RunLoopImpl::set_message_dispatcher(std::function<bool()> dispatcher) {
    std::lock_guard lock(mutex_);
    dispatcher_ = dispatcher ? std::make_shared<std::function<bool()>>(std::move(dispatcher)) : nullptr;
}

void RunLoopImpl::perform(ModeSet modes, std::function<void()> work, bool counts_as_source) {
    if (!work) throw std::invalid_argument("run loop perform without work");
    if (modes.empty()) throw std::invalid_argument("run loop perform without modes");
    { std::lock_guard lock(mutex_); blocks_.push_back({std::move(modes), std::move(work), counts_as_source}); }
    wake();
}

// ---- provider ------------------------------------------------------------------
namespace {
struct LoopPayload final : ObjectPayload {
    explicit LoopPayload(std::shared_ptr<RunLoopImpl> value) : loop(std::move(value)) {}
    std::shared_ptr<RunLoopImpl> loop;
};
// CFAbsoluteTime of +[NSDate distantFuture] (0001-01-01 + 4000 years).
constexpr double distant_future_absolute = 63113904000.0;
} // namespace

std::shared_ptr<RunLoopImpl> Provider::current() {
    if (&process.calling_engine() == &process.main_engine()) return main;
    const auto identity = process.current_thread()->identity;
    std::lock_guard lock(mutex);
    auto& slot = threads[identity];
    if (!slot) slot = std::make_shared<RunLoopImpl>(process, false);
    return slot;
}

std::shared_ptr<RunLoopImpl> Provider::loop_of(std::uint64_t object) {
    if (!object) throw std::invalid_argument("nil run loop");
    const auto payload = objc.find_host_state<LoopPayload>(object);
    if (!payload) throw std::runtime_error("object is not an Alchemy run loop");
    return payload->loop;
}

std::uint64_t Provider::ns_object(const std::shared_ptr<RunLoopImpl>& loop) {
    std::lock_guard lock(mutex);
    auto& slot = ns_objects[loop.get()];
    if (!slot) slot = process.objects().create(16, ns_run_loop_class, ObjectKind::instance, std::make_shared<LoopPayload>(loop), {}, true);
    return slot;
}

std::uint64_t Provider::cf_object(const std::shared_ptr<RunLoopImpl>& loop) {
    std::lock_guard lock(mutex);
    auto& slot = cf_objects[loop.get()];
    if (!slot) slot = process.objects().create(16, cf_run_loop_class, ObjectKind::instance, std::make_shared<LoopPayload>(loop), {}, true);
    return slot;
}

std::string Provider::mode_name(std::uint64_t string) {
    if (!string) throw std::invalid_argument("nil run loop mode");
    return cf.utf8(string);
}

std::uint64_t Provider::mode_object(std::string_view name) {
    {
        std::lock_guard lock(mutex);
        if (const auto found = mode_objects.find(name); found != mode_objects.end()) return found->second;
    }
    const auto object = cf.constant_string(name);
    std::lock_guard lock(mutex);
    return mode_objects.emplace(std::string(name), object).first->second;
}

ModeSet Provider::mode_names(std::uint64_t array) {
    if (!array) throw std::invalid_argument("nil run loop mode array");
    ModeSet modes;
    for (const auto value : cf.array_values(array)) modes.insert(mode_name(value));
    return modes;
}

std::uint64_t Provider::call(std::string_view library, std::string_view symbol, std::initializer_list<std::uint64_t> arguments) {
    const auto service = process.resolve(library, symbol);
    if (!service) throw std::runtime_error("Alchemy run loop requires " + std::string(symbol));
    return call_address(service->address, arguments);
}

std::uint64_t Provider::call_address(std::uint64_t address, std::initializer_list<std::uint64_t> arguments) {
    return EngineAdapter(process.calling_engine()).call(address, std::span(arguments.begin(), arguments.size()));
}

std::uint64_t Provider::retain(std::uint64_t object) { return object ? call(objc_library, "_objc_retain", {object}) : 0; }
void Provider::release(std::uint64_t object) { if (object) static_cast<void>(call(objc_library, "_objc_release", {object})); }
std::uint64_t Provider::autorelease(std::uint64_t object) { return object ? call(objc_library, "_objc_autorelease", {object}) : 0; }
std::uint64_t Provider::block_copy(std::uint64_t block) {
    return block ? call("/usr/lib/libSystem.B.dylib", "__Block_copy", {block}) : 0;
}
void Provider::block_release(std::uint64_t block) {
    if (block) static_cast<void>(call("/usr/lib/libSystem.B.dylib", "__Block_release", {block}));
}

double Provider::monotonic_from_absolute(double absolute) {
    if (std::isnan(absolute)) throw std::invalid_argument("run loop date is NaN");
    const auto delta = absolute - CoreFoundation::absolute_time_now();
    return delta >= never_seconds ? distant_future : monotonic_seconds() + delta;
}

double Provider::monotonic_from_date(std::uint64_t date) {
    return date ? monotonic_from_absolute(cf.date_time(date)) : monotonic_seconds();
}

double Provider::absolute_from_monotonic(double monotonic) {
    if (std::isinf(monotonic)) return distant_future_absolute;
    return CoreFoundation::absolute_time_now() + (monotonic - monotonic_seconds());
}

std::uint64_t Provider::date(double absolute) { return autorelease(cf.date(absolute)); }

} // namespace detail

} // namespace alchemy::gui
