#pragma once

// Alchemy main run loop and completion thread (alchemy-fx GUI).
// Implemented by src/alchemy/runloop (A2). Used by AppKit's event pump
// (-[NSApplication nextEventMatchingMask:untilDate:inMode:dequeue:]), NSTimer
// / NSRunLoop / CFRunLoop providers, the window server's modal loops and
// Metal completion handlers.
//
// ---- Model -------------------------------------------------------------------
// One run loop exists: the main thread's (CFRunLoopGetMain). It is the only
// place the main thread blocks while the GUI is idle. One pump step
// (run_once):
//   1. runs due timers of the current mode;
//   2. runs signalled sources of the current mode and the main dispatch
//      queue (the same drain as _dispatch_main_queue_callback_4CF);
//   3. dispatches pending Win32 messages through the registered message
//      dispatcher (WindowServer::dispatch_messages; sink callbacks queue
//      NSEvents or call delegates synchronously);
//   4. if nothing was handled and the deadline is not reached, waits in
//      MsgWaitForMultipleObjectsEx(QS_ALLINPUT) on the wake event and the
//      registered wait handles until the earlier of the deadline and the
//      next timer, then repeats from 1.
// Observers see the CFRunLoopActivity transitions (entry, before timers,
// before sources, before waiting, after waiting, exit).
//
// Modes are CFRunLoop mode names ("kCFRunLoopDefaultMode",
// "NSEventTrackingRunLoopMode", "NSModalPanelRunLoopMode"); registering with
// kCommonModes adds an item to every mode in the common set (default, event
// tracking and modal panel on the main thread, as AppKit configures it;
// default elsewhere), as CFRunLoop does.
//
// ---- Threading ---------------------------------------------------------------
// run_once, stop, the add_/remove_ functions and observers are main-thread
// only (the guest main thread inside a synthetic call). wake, signal_source
// and CompletionThread::post are callable from any thread. Callbacks run on
// the main thread inside run_once; they may call guest code (EngineAdapter /
// ObjcRuntime::send) and may re-enter run_once (nested modal loops); a
// callback's exception stops the step and propagates out of run_once after
// the run loop's bookkeeping is restored.
//
// Win32 modal loops (window size/move, menu tracking, dialog boxes such as
// the MessageBoxW behind -[NSAlert runModal]) own the main thread's message
// queue while they run. The loop keeps servicing the guest inside them:
// size/move and menus through the window server's modal_tick (a
// run_once in NSEventTrackingRunLoopMode), dialog loops through a
// WH_MSGFILTER hook that runs NSModalPanelRunLoopMode steps. A run_once
// nested in such a loop never calls the message dispatcher and does not wait
// for input; the Win32 loop dispatches the messages.
//
// Process stop: run_once returns RunResult::stopped promptly when
// engine_stop_requested is set, so a stopping main thread can unwind.
//
// ---- Time ----------------------------------------------------------------------
// Times are seconds on monotonic_seconds()'s clock (the clock of
// mach_absolute_time and NSEvent timestamps). Providers convert NSDate /
// CFAbsoluteTime fire dates with the wall-clock offset at registration.

#include "alchemy/process_context.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui {

inline constexpr std::string_view default_mode = "kCFRunLoopDefaultMode";
inline constexpr std::string_view event_tracking_mode = "NSEventTrackingRunLoopMode";
inline constexpr std::string_view modal_panel_mode = "NSModalPanelRunLoopMode";
inline constexpr std::string_view common_modes = "kCFRunLoopCommonModes";
inline constexpr double distant_future = std::numeric_limits<double>::infinity();

// Monotonic seconds since an arbitrary epoch (host unbiased interrupt time,
// the same base as Alchemy's mach_absolute_time). Any thread.
[[nodiscard]] double monotonic_seconds() noexcept;

using TimerId = std::uint64_t;      // 0 = none
using SourceId = std::uint64_t;
using ObserverId = std::uint64_t;

// CFRunLoopRunResult values.
enum class RunResult : std::int32_t { finished = 1, stopped = 2, timed_out = 3, handled_source = 4 };

// CFRunLoopActivity bits.
namespace activity {
inline constexpr std::uint32_t entry = 1U << 0, before_timers = 1U << 1, before_sources = 1U << 2,
    before_waiting = 1U << 5, after_waiting = 1U << 6, exit = 1U << 7, all = 0x0fffffffU;
} // namespace activity

class RunLoop {
public:
    virtual ~RunLoop() = default;

    // One CFRunLoopRunInMode step: pump until a source is handled (when
    // `return_after_source_handled`), stop() is called, the mode has no
    // sources or timers (finished), or `timeout` seconds pass (timed_out).
    // timeout 0 polls once; distant_future waits indefinitely.
    virtual RunResult run_once(std::string_view mode, double timeout, bool return_after_source_handled) = 0;
    // CFRunLoopStop: the run_once that is innermost at the time of the call
    // returns `stopped` (a run entered later does not consume the stop; with
    // no run active the call does nothing). Any thread.
    virtual void stop() = 0;
    // CFRunLoopWakeUp: the main thread's wait returns. Any thread; cheap; this
    // is what ProcessContext::wake_main_thread calls once the run loop exists.
    virtual void wake() = 0;
    [[nodiscard]] virtual std::string current_mode() const = 0;   // empty outside run_once
    [[nodiscard]] virtual bool is_waiting() const = 0;            // CFRunLoopIsWaiting

    // Timers (NSTimer / CFRunLoopTimer). `fire` is absolute monotonic
    // seconds; interval 0 fires once (then the timer is removed). The callback
    // receives the timer id and may cancel or reschedule it.
    [[nodiscard]] virtual TimerId add_timer(double fire, double interval, std::string_view mode,
                                            std::function<void(TimerId)> callback) = 0;
    virtual void set_timer_fire(TimerId timer, double fire) = 0;
    virtual void cancel_timer(TimerId timer) = 0;
    [[nodiscard]] virtual double next_timer_fire(std::string_view mode) const = 0;   // distant_future when none

    // Version-0 sources (CFRunLoopSource / performSelector:onThread: /
    // NSRunLoop performBlock:). signal_source marks the source; its perform
    // callback runs once on the next step of a mode it is registered in.
    [[nodiscard]] virtual SourceId add_source(std::string_view mode, std::function<void()> perform) = 0;
    virtual void add_source_mode(SourceId source, std::string_view mode) = 0;
    virtual void signal_source(SourceId source) = 0;   // any thread; also wakes
    virtual void remove_source(SourceId source) = 0;
    // A Win32 waitable HANDLE (event, semaphore, timer) whose signalled state
    // runs `perform` on the main thread (in every mode). The run loop does
    // not own the handle; remove it before closing.
    [[nodiscard]] virtual SourceId add_wait_handle(void* handle, std::function<void()> perform) = 0;

    // CFRunLoopObserver.
    [[nodiscard]] virtual ObserverId add_observer(std::uint32_t activities, std::string_view mode, bool repeats,
                                                  std::function<void(std::uint32_t activity)> callback) = 0;
    virtual void remove_observer(ObserverId observer) = 0;

    // The Win32 message hook: called in step 3 and whenever the wait reports
    // input; returns true when it dispatched something. Set once by
    // install_windowserver (WindowServer::dispatch_messages).
    virtual void set_message_dispatcher(std::function<bool()> dispatcher) = 0;
};

// The main run loop. Throws std::runtime_error until install_runloop created
// it. Any thread (the returned object's own threading rules apply).
[[nodiscard]] RunLoop& main_run_loop();
[[nodiscard]] bool main_run_loop_available() noexcept;
// The immortal string object the run loop answers for `mode`
// (CFRunLoopCopyCurrentMode, -[NSRunLoop currentMode]); providers export it
// for their mode constants so guests may compare modes by pointer. 0 until
// install_runloop.
[[nodiscard]] std::uint64_t run_loop_mode_object(std::string_view mode);

// ---- completion thread ---------------------------------------------------------
// A long-lived guest pthread for host-originated callbacks into guest code
// that must not run on the main thread or a borrowed host thread: Metal
// command-buffer completed/scheduled handlers, MTLSharedEvent listeners,
// drawable presented handlers. Started from a guest thread with
// _pthread_create at a synthetic entry (the same pattern as dispatch's
// workers), so it has a real guest engine and ThreadContext; it never
// creates a host std::thread and never uses one dispatch_async per item.
//
// Work items run in post order on that thread. A work item may block (for
// example vkWaitSemaphores with a short timeout in a loop) but must poll
// engine_stop_requested and return when the process stops; after a stop the
// thread drops pending work (releasing posted blocks) and exits.
class CompletionThread {
public:
    virtual ~CompletionThread() = default;
    // Any thread. `work` runs on the completion thread; it may call guest
    // code through EngineAdapter(process.calling_engine()).
    virtual void post(std::function<void()> work) = 0;
    // Any thread. Invokes a guest block (already _Block_copy'd by the caller,
    // ownership passes here) as block->invoke(block, arguments...) with up to
    // 7 integer arguments, then _Block_release's it.
    virtual void post_block(std::uint64_t block, std::vector<std::uint64_t> arguments) = 0;
    // Stops accepting work, drains or drops pending items (see above) and
    // joins the thread. Main thread or any guest thread except the completion
    // thread itself. Idempotent.
    virtual void shutdown() = 0;
};

// Starts a completion thread. Must be called on a guest thread inside a
// synthetic call (it calls _pthread_create through the guest). `name` is the
// thread name (pthread_setname_np) for diagnostics.
[[nodiscard]] std::shared_ptr<CompletionThread> start_completion_thread(ProcessContext& process, std::string name);

} // namespace alchemy::gui
