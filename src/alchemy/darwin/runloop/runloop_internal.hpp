#pragma once

// Run loop internals shared by the run loop core (runloop.cpp), the
// CFRunLoop C API (cf_runloop.cpp), NSRunLoop / NSTimer / NSObject perform
// (ns_runloop.cpp) and the completion thread (completion_thread.cpp).
// Owner: A2. The public contract is include/alchemy/gui/runloop.hpp.

#include "alchemy/core_foundation.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/registration_audit.hpp"

#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::detail {

using ModeSet = std::set<std::string, std::less<>>;

// One CFRunLoop. Every member function is thread-safe; run_once runs only on
// the owning thread (the thread that created the loop; the main loop is
// created by install_runloop on the main thread). Callbacks run on the owning
// thread without the loop's lock held, so they may call back into the loop.
class RunLoopImpl final : public RunLoop {
public:
    RunLoopImpl(ProcessContext& process, bool main);
    ~RunLoopImpl() override;

    RunResult run_once(std::string_view mode, double timeout, bool return_after_source_handled) override;
    void stop() override;
    void wake() override;
    [[nodiscard]] std::string current_mode() const override;
    [[nodiscard]] bool is_waiting() const override { return waiting_.load(); }
    [[nodiscard]] TimerId add_timer(double fire, double interval, std::string_view mode,
                                    std::function<void(TimerId)> callback) override;
    void set_timer_fire(TimerId timer, double fire) override;
    void cancel_timer(TimerId timer) override;
    [[nodiscard]] double next_timer_fire(std::string_view mode) const override;
    [[nodiscard]] SourceId add_source(std::string_view mode, std::function<void()> perform) override;
    void add_source_mode(SourceId source, std::string_view mode) override;
    void signal_source(SourceId source) override;
    void remove_source(SourceId source) override;
    [[nodiscard]] SourceId add_wait_handle(void* handle, std::function<void()> perform) override;
    [[nodiscard]] ObserverId add_observer(std::uint32_t activities, std::string_view mode, bool repeats,
                                          std::function<void(std::uint32_t activity)> callback) override;
    void remove_observer(ObserverId observer) override;
    void set_message_dispatcher(std::function<bool()> dispatcher) override;

    // ---- extensions for the CF / Foundation providers ----------------------
    [[nodiscard]] bool is_main() const noexcept { return main_; }
    [[nodiscard]] bool on_owner_thread() const noexcept;
    // CFRunLoopAddCommonMode; kCFRunLoopCommonModes items join the new mode.
    void add_common_mode(std::string_view mode);
    [[nodiscard]] std::vector<std::string> all_modes() const;
    // Timers in several modes (CFRunLoopAddTimer with a second mode).
    void add_timer_mode(TimerId timer, std::string_view mode);
    // Returns true when the timer is still scheduled in some mode.
    bool remove_timer_mode(TimerId timer, std::string_view mode);
    [[nodiscard]] bool timer_in_mode(TimerId timer, std::string_view mode) const;
    [[nodiscard]] double timer_fire(TimerId timer) const;   // distant_future when unknown
    void set_timer_order(TimerId timer, std::int64_t order);
    bool remove_source_mode(SourceId source, std::string_view mode);
    [[nodiscard]] bool source_in_mode(SourceId source, std::string_view mode) const;
    void set_source_order(SourceId source, std::int64_t order);
    void add_observer_mode(ObserverId observer, std::string_view mode);
    bool remove_observer_mode(ObserverId observer, std::string_view mode);
    [[nodiscard]] bool observer_in_mode(ObserverId observer, std::string_view mode) const;
    void set_observer_order(ObserverId observer, std::int64_t order);
    // CFRunLoopPerformBlock / performSelectorOnMainThread: `work` runs once
    // in the next step of a matching mode. `counts_as_source` makes the step
    // count as a handled source (the source-0 behaviour of
    // performSelector:onThread:), plain blocks do not. Any thread; wakes.
    void perform(ModeSet modes, std::function<void()> work, bool counts_as_source);
    // The waker for ProcessContext::set_main_thread_waker: shares ownership
    // of the wake event, so it stays valid after the loop is destroyed.
    [[nodiscard]] std::function<void()> waker() const;

private:
    struct Timer {
        double fire{}, interval{};
        ModeSet modes;
        std::shared_ptr<std::function<void(TimerId)>> callback;
        std::int64_t order{};
        bool firing{}, rescheduled{};
    };
    struct Source {
        ModeSet modes;
        std::shared_ptr<std::function<void()>> perform;
        void* handle{};            // wait-handle source (every mode)
        std::int64_t order{};
        bool signaled{};
    };
    struct Observer {
        std::uint32_t activities{};
        ModeSet modes;
        bool repeats{}, firing{};
        std::int64_t order{};
        std::shared_ptr<std::function<void(std::uint32_t)>> callback;
    };
    struct Block { ModeSet modes; std::function<void()> work; bool counts{}; };

    // Caller holds mutex_.
    [[nodiscard]] bool in_mode(const ModeSet& modes, std::string_view mode) const;
    [[nodiscard]] bool mode_empty(std::string_view mode) const;
    [[nodiscard]] bool common(std::string_view mode) const { return common_.contains(mode); }

    void notify(std::uint32_t activity, std::string_view mode);
    void run_timers(std::string_view mode);
    bool run_blocks(std::string_view mode);
    bool run_sources(std::string_view mode);
    bool drain_main_queue(std::string_view mode);
    bool dispatch_messages();
    void wait(double seconds);
    [[nodiscard]] bool stop_requested() const;
    // Clears and returns the stop mark of run `index` (frames_).
    bool take_stop(std::size_t index);
    [[nodiscard]] bool innermost_stopped() const;
    // Main loop, owner thread: installs the dialog-loop hook (runloop.cpp,
    // "Win32 modal loops") and rethrows an error a modal step stashed.
    void prepare_modal_loops();

    ProcessContext& process_;
    const bool main_;
    const std::uint64_t owner_thread_;
    std::shared_ptr<void> wake_event_;
    mutable std::mutex mutex_;
    ModeSet common_;
    ModeSet known_modes_;
    std::map<TimerId, Timer> timers_;
    std::map<SourceId, Source> sources_;
    std::map<ObserverId, Observer> observers_;
    std::vector<Block> blocks_;
    std::shared_ptr<std::function<bool()>> dispatcher_;
    // The active (nested) run_once calls, innermost last. CFRunLoopStop marks
    // the innermost run active at the time of the call (CFRunLoop's per-run
    // data): a run entered later never consumes it, and a stop with no run
    // active is dropped.
    struct ActiveRun { std::string mode; bool stopped{}; };
    std::vector<ActiveRun> frames_;
    std::uint64_t next_id_{1};
    std::atomic<bool> waiting_{false};
};

// The process's run loops and their guest objects (install_runloop).
struct Provider : std::enable_shared_from_this<Provider> {
    Provider(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf) : process(process), objc(objc), cf(cf) {}
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    std::shared_ptr<RunLoopImpl> main;
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<RunLoopImpl>> threads;   // thread identity -> loop (not main)
    std::map<const RunLoopImpl*, std::uint64_t> ns_objects, cf_objects;
    std::map<std::string, std::uint64_t, std::less<>> mode_objects;  // mode name -> immortal NSString
    std::uint64_t ns_run_loop_class{}, cf_run_loop_class{}, timer_class{}, observer_class{}, source_class{};

    // The calling thread's run loop (created on first use).
    std::shared_ptr<RunLoopImpl> current();
    std::shared_ptr<RunLoopImpl> loop_of(std::uint64_t object);   // NSRunLoop or CFRunLoop object
    std::uint64_t ns_object(const std::shared_ptr<RunLoopImpl>& loop);
    std::uint64_t cf_object(const std::shared_ptr<RunLoopImpl>& loop);
    // Mode strings: guest NSString/CFString -> name, and name -> the
    // process-wide constant object (pointer-equal to kCFRunLoopDefaultMode &c).
    std::string mode_name(std::uint64_t string);
    std::uint64_t mode_object(std::string_view name);
    ModeSet mode_names(std::uint64_t array);   // NSArray/CFArray of modes

    // Guest services.
    std::uint64_t call(std::string_view library, std::string_view symbol, std::initializer_list<std::uint64_t> arguments);
    std::uint64_t call_address(std::uint64_t address, std::initializer_list<std::uint64_t> arguments);
    std::uint64_t retain(std::uint64_t object);
    void release(std::uint64_t object);
    std::uint64_t autorelease(std::uint64_t object);
    std::uint64_t block_copy(std::uint64_t block);
    void block_release(std::uint64_t block);
    // NSDate (CFDate) <-> monotonic seconds.
    double monotonic_from_date(std::uint64_t date);          // nil -> now
    double monotonic_from_absolute(double absolute);
    double absolute_from_monotonic(double monotonic);
    std::uint64_t date(double absolute);                     // +0 autoreleased NSDate

    // A host class registered under `name` (an existing class is reused) and
    // published as _OBJC_CLASS_$_<name> by `library`. method() takes its
    // caller's location for the registration audit.
    std::uint64_t define_class(std::string_view name, std::string_view library);
    void method(std::uint64_t cls, std::string name, transmute::GuestCallHandler handler, bool class_method = false,
                std::string types = "@@:", const std::source_location& where = std::source_location::current());

    // NSTimer / CFRunLoopTimer, toll-free bridged (timers.cpp). A scheduled
    // timer is retained by its run loop until it is invalidated or removed
    // from its last mode.
    void install_timers();
    void schedule_timer(std::uint64_t timer, const std::shared_ptr<RunLoopImpl>& loop, std::string_view mode);
    void unschedule_timer(std::uint64_t timer, const std::shared_ptr<RunLoopImpl>& loop, std::string_view mode);
    [[nodiscard]] bool timer_scheduled(std::uint64_t timer, const std::shared_ptr<RunLoopImpl>& loop, std::string_view mode);
    void fire_timer(std::uint64_t timer);
    void invalidate_timer(std::uint64_t timer);

    // -performSelector:withObject:afterDelay: requests (ns_runloop.cpp).
    struct DelayedPerform { std::uint64_t target{}, selector{}, argument{}; std::shared_ptr<RunLoopImpl> loop; TimerId timer{}; };
    std::map<std::uint64_t, DelayedPerform> delayed;
    std::uint64_t next_delayed{1};
    // -[NSRunLoop performSelector:target:argument:order:modes:] requests
    // (timer unused), cancelled by -cancelPerformSelector(s)....
    std::map<std::uint64_t, DelayedPerform> loop_performs;

    void install_cf();
    void install_foundation();
};

// Guest double arguments and results (d registers).
inline double d_argument(const transmute::CpuState& state, unsigned index) { return std::bit_cast<double>(state.v[index][0]); }
inline void d_result(transmute::CpuState& state, double value) { state.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; }

// Seconds that a CFAbsoluteTime / NSDate interval of this size or more means
// "never" (distantFuture is ~63e9 s after the reference date).
inline constexpr double never_seconds = 1e9;

} // namespace alchemy::gui::detail
