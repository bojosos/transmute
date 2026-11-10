// linux backend of fx/host/sync.hpp: the futex waits.
#include "fx/host/sync.hpp"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <ctime>

namespace transmute::fx::host {

namespace {

// FUTEX_WAIT while *word == expected, up to `milliseconds` (kWaitForever: no
// limit). False only on a timeout.
bool futex_wait(std::atomic<std::uint32_t>& word, std::uint32_t expected, unsigned long milliseconds) noexcept {
    timespec timeout{};
    timespec* limit = nullptr;
    if (milliseconds != kWaitForever) {
        timeout.tv_sec = static_cast<time_t>(milliseconds / 1000);
        timeout.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
        limit = &timeout;
    }
    const long result = syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected,
                                limit, nullptr, 0);
    return !(result != 0 && errno == ETIMEDOUT);
}

void futex_wake(std::atomic<std::uint32_t>& word, int count) noexcept {
    syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}

} // namespace

bool CondVar::wait(RwLock& lock, unsigned long milliseconds, bool shared) noexcept {
    const std::uint32_t seen = sequence_.load(std::memory_order_acquire);
    if (shared) lock.unlock_shared();
    else lock.unlock();
    const bool woken = futex_wait(sequence_, seen, milliseconds);
    if (shared) lock.lock_shared();
    else lock.lock();
    return woken;
}

void CondVar::notify_one() noexcept {
    sequence_.fetch_add(1, std::memory_order_release);
    futex_wake(sequence_, 1);
}

void CondVar::notify_all() noexcept {
    sequence_.fetch_add(1, std::memory_order_release);
    futex_wake(sequence_, INT_MAX);
}

AutoResetEvent::AutoResetEvent() noexcept = default;
AutoResetEvent::~AutoResetEvent() = default;

void AutoResetEvent::set() noexcept {
    signaled_.store(1, std::memory_order_release);
    futex_wake(signaled_, 1);
}

bool AutoResetEvent::wait_for(unsigned long milliseconds) noexcept {
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        std::uint32_t expected = 1;
        if (signaled_.compare_exchange_strong(expected, 0, std::memory_order_acquire)) return true;
        unsigned long left = milliseconds;
        if (milliseconds != kWaitForever) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const auto spent = static_cast<unsigned long>((now.tv_sec - start.tv_sec) * 1000 +
                                                          (now.tv_nsec - start.tv_nsec) / 1000000);
            if (spent >= milliseconds) return false;
            left = milliseconds - spent;
        }
        futex_wait(signaled_, 0, left);
    }
}

} // namespace transmute::fx::host
