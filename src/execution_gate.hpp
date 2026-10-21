#pragma once

#include <atomic>
#include <cassert>
#include <mutex>
#include <shared_mutex>

namespace transmute {

// Checked access and mutation acquire this gate before the address-space
// mutex. Native regions borrow stable backing under a shared lease; shared
// scalar stores publish their journal under the short address-space mutex.
class ExecutionGate final {
public:
    class Exclusive final {
    public:
        explicit Exclusive(ExecutionGate& gate)
            : gate_(gate), lock_(gate.mutex_, std::defer_lock), previous_(active_) {
            if (!held(gate)) {
                gate_.pending_.fetch_add(1, std::memory_order_acq_rel);
                try {
                    lock_.lock();
                } catch (...) {
                    gate_.remove_pending_writer();
                    throw;
                }
                gate_.remove_pending_writer();
            }
            active_ = this;
        }
        ~Exclusive() {
            assert(active_ == this);
            active_ = previous_;
        }
        Exclusive(const Exclusive&) = delete;
        Exclusive& operator=(const Exclusive&) = delete;
        [[nodiscard]] static bool held(const ExecutionGate& gate) noexcept {
            for (const auto* owner = active_; owner != nullptr; owner = owner->previous_) {
                if (&owner->gate_ == &gate) return true;
            }
            return false;
        }

    private:
        ExecutionGate& gate_;
        std::unique_lock<std::shared_mutex> lock_;
        Exclusive* previous_;
        inline static thread_local Exclusive* active_{};
    };

    class Read final {
    public:
        explicit Read(ExecutionGate& gate)
            : lock_(gate.mutex_, std::defer_lock) {
            assert(!Exclusive::held(gate));
            if (gate.pending_.load(std::memory_order_acquire) != 0) return;
            lock_.lock();
            // A writer arriving during acquisition is allowed to quiesce the
            // already admitted regions; new regions yield to it.
            if (gate.pending_.load(std::memory_order_acquire) != 0) lock_.unlock();
        }
        Read(const Read&) = delete;
        Read& operator=(const Read&) = delete;
        [[nodiscard]] bool owns_lock() const noexcept { return lock_.owns_lock(); }

    private:
        std::shared_lock<std::shared_mutex> lock_;
    };

    [[nodiscard]] bool has_pending_writer() const noexcept {
        return pending_.load(std::memory_order_acquire) != 0;
    }

    void wait_for_pending_writer() const noexcept {
        // Failed reader admission owns no borrowed memory or gate. Park
        // instead of repeatedly yielding while the queued writers quiesce.
        // Once the queue drains, Read still acquires the backing-lifetime
        // lock and checks a newly arrived writer before admitting a region.
        const auto pending = pending_.load(std::memory_order_acquire);
        if (pending != 0) pending_.wait(pending, std::memory_order_acquire);
    }

private:
    void remove_pending_writer() noexcept {
        if (pending_.fetch_sub(1, std::memory_order_release) == 1)
            pending_.notify_all();
    }
    std::shared_mutex mutex_;
    std::atomic<unsigned> pending_{};
};

} // namespace transmute
