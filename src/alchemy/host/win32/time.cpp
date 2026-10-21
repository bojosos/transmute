#include "alchemy/host/time.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <realtimeapiset.h>

#include <algorithm>

namespace alchemy::host::time {

std::uint64_t monotonic_ns() noexcept {
    ULONGLONG ticks = 0; // 100 ns units, excluding time asleep
    QueryUnbiasedInterruptTimePrecise(&ticks);
    return static_cast<std::uint64_t>(ticks) * 100;
}

std::int64_t realtime_ns() noexcept {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const auto ticks = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    constexpr std::uint64_t unix_epoch = 116444736000000000ULL; // 1970-01-01 in FILETIME ticks
    return (static_cast<std::int64_t>(ticks) - static_cast<std::int64_t>(unix_epoch)) * 100;
}

std::uint32_t tick_ms() noexcept { return GetTickCount(); }

std::uint64_t continuous_ns() noexcept {
    ULONGLONG ticks = 0; // 100 ns units, including time asleep
    QueryInterruptTimePrecise(&ticks);
    return static_cast<std::uint64_t>(ticks) * 100;
}
namespace {
std::uint64_t cpu_ns(const FILETIME& kernel, const FILETIME& user) {
    const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 100;
}
} // namespace
Result<std::uint64_t> process_cpu_ns() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return win32::last_error();
    return cpu_ns(kernel, user);
}
Result<std::uint64_t> thread_cpu_ns() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return win32::last_error();
    return cpu_ns(kernel, user);
}
void sleep_ns(std::uint64_t duration) noexcept {
    if (!duration) return;
    // Sleep rounds up to the timer resolution (15.6 ms by default). A
    // per-thread high-resolution waitable timer sleeps the requested time
    // without raising the system-wide timer resolution.
    struct Precise {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Precise() { if (handle) CloseHandle(handle); }
    };
    thread_local Precise timer;
    if (timer.handle) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(duration / 100, INT64_MAX)));
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) && WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0)
            return;
    }
    const auto milliseconds = (duration + 999'999) / 1'000'000;
    Sleep(milliseconds >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(milliseconds));
}

// ---- Timer (thread-pool timers) ----

struct Timer::State {
    std::function<void()> callback;
    PTP_TIMER timer{};
    ~State() {
        if (!timer) return;
        SetThreadpoolTimer(timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }
};

Result<Timer> Timer::create(std::function<void()> callback) {
    if (!callback) return Error::invalid_argument;
    auto state = std::make_unique<State>();
    state->callback = std::move(callback);
    state->timer = CreateThreadpoolTimer([](PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) { static_cast<State*>(context)->callback(); },
                                         state.get(), nullptr);
    if (!state->timer) return win32::last_error();
    Timer timer;
    timer.state_ = std::move(state);
    return timer;
}

Timer::Timer() = default;
Timer::Timer(Timer&&) noexcept = default;
Timer& Timer::operator=(Timer&&) noexcept = default;
Timer::~Timer() = default;

Status Timer::schedule(std::uint64_t due_ns, std::uint64_t period_ns) {
    if (!state_) return Error::bad_handle;
    // A negative FILETIME is relative, in 100 ns units; the period is in milliseconds.
    const auto due = -static_cast<std::int64_t>(std::min<std::uint64_t>(due_ns / 100, 0x7fffffffffffffffULL));
    FILETIME when{static_cast<DWORD>(due), static_cast<DWORD>(static_cast<std::uint64_t>(due) >> 32)};
    const auto period = static_cast<DWORD>(std::min<std::uint64_t>(period_ns / 1'000'000, 0xffffffffULL));
    SetThreadpoolTimer(state_->timer, &when, period, 0);
    return {};
}

void Timer::cancel() noexcept {
    if (!state_) return;
    SetThreadpoolTimer(state_->timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(state_->timer, TRUE);
}

} // namespace alchemy::host::time
