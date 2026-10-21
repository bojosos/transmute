#include "alchemy/darwin/network/network_internal.hpp"

#include "alchemy/darwin_threads.hpp"

#include <chrono>

// The guest pump: host threads (Windows DNS-SD callbacks) cannot call guest
// code, so they post small guest jobs here (write a wake-up byte to a dns_sd
// socket, signal an NSNetService run-loop source). The worker is a guest
// dispatch worker started on demand from a guest thread by lease(); it
// sleeps on a host condition variable in 100 ms slices (so a stopping process
// can unwind it) and ends when no lease is held and nothing is queued.
namespace alchemy::gui::network {

namespace {
std::shared_ptr<GuestPump>& pump_instance() {
    static std::shared_ptr<GuestPump> instance;
    return instance;
}
} // namespace

std::shared_ptr<GuestPump> guest_pump() {
    auto& instance = pump_instance();
    if (!instance) instance = std::make_shared<GuestPump>(security::bridge());
    return instance;
}

std::shared_ptr<void> GuestPump::lease() {
    bool start{};
    {
        std::lock_guard lock(mutex_);
        ++leases_;
        if (!running_) { running_ = true; start = true; }
    }
    const auto self = shared_from_this();
    std::shared_ptr<void> handle(nullptr, [self](void*) {
        {
            std::lock_guard lock(self->mutex_);
            --self->leases_;
        }
        self->changed_.notify_all();
    });
    if (start) {
        try {
            const auto queue = b->call(security::libsystem_library, "_dispatch_get_global_queue", {0, 0});
            b->dispatch_async(queue, [self] { self->run(); });
        } catch (...) {
            std::lock_guard lock(mutex_);
            running_ = false;
            throw;
        }
    }
    return handle;
}

void GuestPump::post(std::function<void()> work) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(work));
    }
    changed_.notify_all();
}

void GuestPump::run() {
    for (;;) {
        std::vector<std::function<void()>> batch;
        {
            std::unique_lock lock(mutex_);
            if (queue_.empty()) {
                if (leases_ == 0) { running_ = false; return; }
                changed_.wait_for(lock, std::chrono::milliseconds(100));
            }
            batch.swap(queue_);
            if (batch.empty() && guest_worker_must_stop(b->process)) { running_ = false; throw GuestThreadStop{}; }
        }
        try {
            for (auto& work : batch) work();
        } catch (...) {
            std::lock_guard lock(mutex_);
            running_ = false;   // the next lease restarts the worker
            throw;
        }
    }
}

} // namespace alchemy::gui::network
