// Linux host backend, area "time".
//
// Timers share one service thread that sleeps until the earliest deadline
// and runs the due callbacks there.
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/time.hpp"

#include <time.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace alchemy::host::time {

namespace {

std::uint64_t read(clockid_t clock) noexcept {
    timespec now{};
    clock_gettime(clock, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000u + static_cast<std::uint64_t>(now.tv_nsec);
}

} // namespace

std::uint64_t monotonic_ns() noexcept { return read(CLOCK_MONOTONIC); }
std::uint32_t tick_ms() noexcept { return static_cast<std::uint32_t>(read(CLOCK_MONOTONIC) / 1000000u); }
std::int64_t realtime_ns() noexcept { return static_cast<std::int64_t>(read(CLOCK_REALTIME)); }
std::uint64_t continuous_ns() noexcept { return read(CLOCK_BOOTTIME); }
Result<std::uint64_t> process_cpu_ns() { return read(CLOCK_PROCESS_CPUTIME_ID); }
Result<std::uint64_t> thread_cpu_ns() { return read(CLOCK_THREAD_CPUTIME_ID); }

void sleep_ns(std::uint64_t duration) noexcept {
    timespec rest{static_cast<time_t>(duration / 1000000000u), static_cast<long>(duration % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &rest, &rest) == EINTR) {
    }
}

// ---- timers -------------------------------------------------------------------------

struct Timer::State {
    std::function<void()> callback;
    std::uint64_t period_ns{};
    std::uint64_t generation{};   // bumped by every schedule and cancel
    bool running{};               // the callback is running now
};

namespace {

struct Service {
    std::mutex lock;
    std::condition_variable changed;
    std::multimap<std::uint64_t, std::pair<Timer::State*, std::uint64_t>> due;   // deadline -> (timer, generation)
    bool started{};

    void run() {
        std::unique_lock guard(lock);
        for (;;) {
            if (due.empty()) {
                changed.wait(guard);
                continue;
            }
            const auto now = monotonic_ns();
            auto first = due.begin();
            if (first->first > now) {
                changed.wait_for(guard, std::chrono::nanoseconds(first->first - now));
                continue;
            }
            auto [state, generation] = first->second;
            due.erase(first);
            if (state->generation != generation) continue;   // rescheduled or cancelled meanwhile
            state->running = true;
            if (state->period_ns) due.emplace(now + state->period_ns, std::make_pair(state, generation));
            guard.unlock();
            state->callback();
            guard.lock();
            state->running = false;
            changed.notify_all();
        }
    }
};

Service& service() {
    static auto* s = new Service;   // never destroyed: its thread runs until exit
    std::lock_guard guard(s->lock);
    if (!s->started) {
        std::thread([] { service().run(); }).detach();
        s->started = true;
    }
    return *s;
}

} // namespace

Result<Timer> Timer::create(std::function<void()> callback) {
    Timer timer;
    timer.state_ = std::make_unique<State>();
    timer.state_->callback = std::move(callback);
    return timer;
}

Timer::Timer() = default;
Timer::Timer(Timer&&) noexcept = default;
Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        cancel();
        state_ = std::move(other.state_);
    }
    return *this;
}
Timer::~Timer() { cancel(); }

Status Timer::schedule(std::uint64_t due_ns, std::uint64_t period_ns) {
    if (!state_) return Error::invalid_argument;
    auto& s = service();
    std::lock_guard guard(s.lock);
    state_->period_ns = period_ns;
    ++state_->generation;
    s.due.emplace(monotonic_ns() + due_ns, std::make_pair(state_.get(), state_->generation));
    s.changed.notify_all();
    return {};
}

void Timer::cancel() noexcept {
    if (!state_) return;
    auto& s = service();
    std::unique_lock guard(s.lock);
    ++state_->generation;
    // Forget its pending deadlines: the State may be destroyed next.
    for (auto it = s.due.begin(); it != s.due.end();) {
        if (it->second.first == state_.get()) it = s.due.erase(it);
        else ++it;
    }
    s.changed.wait(guard, [&] { return !state_->running; });
}

} // namespace alchemy::host::time
