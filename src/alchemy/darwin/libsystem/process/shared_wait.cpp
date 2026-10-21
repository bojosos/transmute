#include "alchemy/shared_wait.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/sync.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace alchemy::shared_wait {
namespace {

constexpr auto recheck = std::chrono::milliseconds(5);

constexpr std::uint64_t empty = 0, tombstone = ~0ULL;
constexpr std::size_t slot_count = 4096;

struct Slot {
    std::uint64_t key;      // FNV-1a hash of the key (never empty or tombstone)
    std::uint32_t waiters;
    std::uint32_t reserved;
};
struct Table { Slot slots[slot_count]; };

std::uint64_t hash(std::string_view key) {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    for (const auto byte : key) {
        value ^= static_cast<unsigned char>(byte);
        value *= 0x100000001b3ULL;
    }
    return value == empty || value == tombstone ? 1 : value;
}

// Takes one count of `semaphore` within `timeout_ns` (0: only if one is there).
bool take(host::Handle semaphore, std::uint64_t timeout_ns) {
    const std::array handles{semaphore};
    return static_cast<bool>(host::sync::wait_any(handles, timeout_ns));
}

class Shared {
public:
    Shared(host::Handle mutex, host::Handle mapping) : mutex_(mutex), mapping_(mapping) {
        const auto view = host::sections::map(mapping_, 0, sizeof(Table), true);
        if (!view) throw std::runtime_error("alchemy: cannot map the cross-process wait table");
        table_ = reinterpret_cast<Table*>(*view);
    }
    // Never destroyed: guest threads may wait during process exit.

    struct Lock {
        host::Handle mutex;
        explicit Lock(host::Handle value) : mutex(value) {
            // An abandoned lock (a process died holding it) is still ours.
            const std::array handles{mutex};
            if (!host::sync::wait_any(handles, std::nullopt))
                throw std::runtime_error("alchemy: cannot lock the cross-process wait table");
        }
        ~Lock() { static_cast<void>(host::sync::unlock_mutex(mutex)); }
    };

    host::Handle mutex() const { return mutex_; }

    // Under the lock. Returns the slot index for `key` (claiming one when
    // `claim`), or slot_count.
    std::size_t find(std::uint64_t key, bool claim) {
        std::size_t free = slot_count;
        for (std::size_t probe = 0; probe < slot_count; ++probe) {
            const auto index = (key + probe) % slot_count;
            const auto value = table_->slots[index].key;
            if (value == key) return index;
            if (value == tombstone && free == slot_count) free = index;
            if (value == empty) {
                if (free == slot_count) free = index;
                break;
            }
        }
        if (!claim || free == slot_count) return slot_count;
        table_->slots[free].key = key;
        table_->slots[free].waiters = 0;
        return free;
    }
    Slot& slot(std::size_t index) { return table_->slots[index]; }

    host::Handle semaphore(std::size_t index) {
        std::lock_guard lock(semaphores_mutex_);
        auto& handle = semaphores_[index];
        if (!handle) {
            const auto created =
                host::sync::create_semaphore("alchemy.shared-waits.v1.slot" + std::to_string(index), 0, 0x7fffffff);
            if (!created) throw std::runtime_error("alchemy: cannot create a cross-process wait semaphore");
            handle = *created;
        }
        return handle;
    }

private:
    host::Handle mutex_{};
    host::Handle mapping_{};
    Table* table_{};
    std::mutex semaphores_mutex_;
    std::map<std::size_t, host::Handle> semaphores_;
};

// The table, or null on a host without named objects.
Shared* shared() {
    static Shared* const instance = []() -> Shared* {
        const auto mutex = host::sync::create_mutex("alchemy.shared-waits.v1.lock");
        if (!mutex && mutex.error() == host::Error::unsupported) return nullptr;
        const auto mapping = host::sections::open_named("alchemy.shared-waits.v1.table", sizeof(Table));
        if (!mutex || !mapping) throw std::runtime_error("alchemy: cannot create the cross-process wait table");
        return new Shared(*mutex, mapping->handle);
    }();
    return instance;
}

} // namespace

Result wait(std::string_view key, const std::function<bool()>& blocked, std::chrono::steady_clock::time_point deadline,
            const std::function<bool()>& stop) {
    auto* const table = shared();
    if (!table) {
        // Without a host table: poll the condition.
        for (;;) {
            if (!blocked()) return Result::changed;
            if (stop()) return Result::stopped;
            if (std::chrono::steady_clock::now() >= deadline) return Result::timed_out;
            std::this_thread::sleep_for(recheck);
        }
    }
    const auto value = hash(key);
    std::size_t index;
    {
        Shared::Lock lock(table->mutex());
        index = table->find(value, true);
        if (index == slot_count) throw std::runtime_error("alchemy: the cross-process wait table is full");
        ++table->slot(index).waiters;
    }
    const auto semaphore = table->semaphore(index);
    Result result = Result::timed_out;
    for (;;) {
        if (!blocked()) { result = Result::changed; break; }
        if (stop()) { result = Result::stopped; break; }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { result = Result::timed_out; break; }
        const auto slice = std::min<std::chrono::steady_clock::duration>(recheck, deadline - now);
        const auto milliseconds = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(slice).count()));
        if (take(semaphore, milliseconds * 1'000'000)) { result = Result::woken; break; }
    }
    {
        Shared::Lock lock(table->mutex());
        auto& slot = table->slot(index);
        if (slot.waiters && --slot.waiters == 0) {
            while (take(semaphore, 0)) {}   // leftover permits
            slot.key = tombstone;
        }
    }
    return result;
}

bool wake(std::string_view key, bool all) {
    auto* const table = shared();
    if (!table) return false;
    const auto value = hash(key);
    Shared::Lock lock(table->mutex());
    const auto index = table->find(value, false);
    if (index == slot_count || table->slot(index).waiters == 0) return false;
    const auto count = all ? table->slot(index).waiters : 1U;
    static_cast<void>(host::sync::release_semaphore(table->semaphore(index), count));
    return true;
}

std::uint32_t owner_token() {
    return static_cast<std::uint32_t>(host::process::current_thread_id());
}

} // namespace alchemy::shared_wait
