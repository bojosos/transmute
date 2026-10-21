#include "alchemy/process_tree.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/sync.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace alchemy::process_tree {
namespace {

// The shared record. `sequence` is odd while a writer updates it.
struct Shared {
    static constexpr std::uint64_t expected_magic = 0x31656572'74636c61ULL;   // "alctree1"
    std::uint64_t magic;
    std::atomic<std::uint32_t> sequence;
    std::uint32_t pid, ppid, pgid, uid, gid, pidversion, host_pid;
    std::uint64_t start_seconds, start_microseconds;
    char path[1024];
    char name[256];
};

void copy_text(char (&target)[1024], std::string_view value) { const auto n = std::min(value.size(), sizeof target - 1); std::memcpy(target, value.data(), n); target[n] = 0; }
void copy_text(char (&target)[256], std::string_view value) { const auto n = std::min(value.size(), sizeof target - 1); std::memcpy(target, value.data(), n); target[n] = 0; }

std::string last_component(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// splitmix64 finaliser: a well-spread 31-bit generation from a creation time.
std::uint32_t generation_of(std::uint64_t created) {
    std::uint64_t z = created + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z ^= z >> 31;
    const auto value = static_cast<std::uint32_t>(z & 0x7fffffffU);
    return value ? value : 1U;
}

std::mutex self_mutex;
std::optional<Record> self_record;

// The session-wide named section of a published record.
std::string object_name(std::uint32_t pid) { return "alchemy-process-" + std::to_string(pid); }

host::Handle self_object;
Shared* self_view{};

// Whether a process handle's process still runs (it is signalled once it ends).
bool handle_running(host::Handle process) {
    const std::array processes{process};
    return host::sync::wait_any(processes, 0).error() == host::Error::timed_out;
}
// Whether the host process `pid` is running.
bool host_running(std::uint32_t pid) {
    const auto running = host::process::is_running(pid);
    return running && *running;
}

std::optional<Record> read_shared(const Shared& shared) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto before = shared.sequence.load(std::memory_order_acquire);
        if (before & 1) { std::this_thread::yield(); continue; }
        Record record;
        record.pid = shared.pid; record.ppid = shared.ppid; record.pgid = shared.pgid; record.uid = shared.uid; record.gid = shared.gid;
        record.pidversion = shared.pidversion; record.host_pid = shared.host_pid;
        record.start_seconds = shared.start_seconds; record.start_microseconds = shared.start_microseconds;
        char path[sizeof shared.path], name[sizeof shared.name];
        std::memcpy(path, shared.path, sizeof path); std::memcpy(name, shared.name, sizeof name);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (shared.sequence.load(std::memory_order_relaxed) != before) continue;
        if (shared.magic != Shared::expected_magic || !record.pid) return std::nullopt;
        path[sizeof path - 1] = 0; name[sizeof name - 1] = 0;
        record.path = path; record.name = name;
        return record;
    }
    return std::nullopt;
}

} // namespace

Record publish(Record record) {
    std::lock_guard lock(self_mutex);
    if (!record.host_pid) record.host_pid = host::process::current_pid();
    if (!record.pid) record.pid = record.host_pid;
    const auto started = host::process::start_time().value_or(0);   // Unix nanoseconds
    if (!record.pidversion) record.pidversion = generation_of(started ^ (static_cast<std::uint64_t>(record.host_pid) << 40));
    if (!self_view || (self_record && self_record->pid != record.pid)) {
        if (self_view) { static_cast<void>(host::sections::unmap(reinterpret_cast<std::uintptr_t>(self_view))); self_view = nullptr; }
        if (self_object) { host::close(self_object); self_object = {}; }
        const auto named = host::sections::open_named(object_name(record.pid), sizeof(Shared));
        const bool existed = named && !named->created;
        if (named) {
            self_object = named->handle;
            if (const auto view = host::sections::map(self_object, 0, sizeof(Shared), true)) self_view = reinterpret_cast<Shared*>(*view);
        }
        // exec in place: the new image keeps the start time of the pid.
        if (self_view && existed && !record.start_seconds)
            if (const auto previous = read_shared(*self_view)) { record.start_seconds = previous->start_seconds; record.start_microseconds = previous->start_microseconds; }
    }
    if (!record.start_seconds) {
        record.start_seconds = started / 1'000'000'000ULL;
        record.start_microseconds = (started % 1'000'000'000ULL) / 1000ULL;
    }
    if (record.name.empty()) record.name = last_component(record.path);
    if (self_view) {
        auto& shared = *self_view;
        shared.sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        shared.magic = Shared::expected_magic;
        shared.pid = record.pid; shared.ppid = record.ppid; shared.pgid = record.pgid; shared.uid = record.uid; shared.gid = record.gid;
        shared.pidversion = record.pidversion; shared.host_pid = record.host_pid;
        shared.start_seconds = record.start_seconds; shared.start_microseconds = record.start_microseconds;
        copy_text(shared.path, record.path); copy_text(shared.name, record.name);
        shared.sequence.fetch_add(1, std::memory_order_release);
    }
    self_record = record;
    return record;
}

std::optional<Record> self() {
    std::lock_guard lock(self_mutex);
    return self_record;
}

std::optional<Record> lookup(std::uint32_t pid) {
    if (!pid) return std::nullopt;
    if (auto own = self(); own && own->pid == pid) return own;
    // Holding the process handle keeps the pid from being reused while the
    // record is read; the record exists only while its publisher runs.
    const auto process = host::process::open(pid);
    if (!process) return std::nullopt;
    std::optional<Record> result;
    if (handle_running(*process)) {
        if (const auto object = host::sections::open_existing(object_name(pid), false)) {
            if (const auto view = host::sections::map(*object, 0, sizeof(Shared), false)) {
                result = read_shared(*reinterpret_cast<const Shared*>(*view));
                static_cast<void>(host::sections::unmap(*view));
            }
            host::close(*object);
        }
        if (result && result->pid != pid) result.reset();
        // An exec'd image is a different host process: it must run too.
        if (result && result->host_pid != pid && !host_running(result->host_pid)) result.reset();
    }
    host::close(*process);
    return result;
}

std::uint32_t pidversion(std::uint32_t pid) {
    const auto record = lookup(pid);
    return record ? record->pidversion : 0;
}

bool alive(std::uint32_t pid, std::uint32_t version) {
    const auto record = lookup(pid);
    return record && (!version || record->pidversion == version);
}

namespace {

struct Watch {
    std::uint64_t token{};
    std::uint32_t pid{};
    host::Handle process, wait;
    std::function<void(std::uint32_t)> on_exit;
    std::atomic<int> state{0};   // 0 armed, 1 fired, 2 cancelled
    std::uint64_t firing_thread{};
    std::mutex done_mutex;
    std::condition_variable done_signal;
    bool done{};
};

std::mutex watches_mutex;
std::map<std::uint64_t, std::shared_ptr<Watch>> watches;
std::uint64_t next_token{};

void process_ended(void* parameter) {
    auto* raw = static_cast<Watch*>(parameter);
    int armed = 0;
    if (!raw->state.compare_exchange_strong(armed, 1)) return;   // cancelled
    raw->firing_thread = host::process::current_thread_id();
    std::shared_ptr<Watch> keep;
    {
        // Taking the lock also waits for the registration to store `wait`.
        std::lock_guard lock(watches_mutex);
        if (const auto found = watches.find(raw->token); found != watches.end()) { keep = found->second; watches.erase(found); }
    }
    if (!keep) return;
    try { keep->on_exit(keep->pid); } catch (...) {}
    host::sync::unwatch(keep->wait, false);   // non-blocking: we are its callback
    host::close(keep->process);
    { std::lock_guard lock(keep->done_mutex); keep->done = true; }
    keep->done_signal.notify_all();
}

} // namespace

std::uint64_t watch_exit(std::uint32_t pid, std::uint32_t version, std::function<void(std::uint32_t pid)> on_exit) {
    const auto opened = host::process::open(pid);
    const auto process = opened ? *opened : host::Handle{};
    // The record is read while `process` pins the pid.
    const auto record = process ? lookup(pid) : std::nullopt;
    if (!process || !record || (version && record->pidversion != version)) {
        if (process) host::close(process);
        on_exit(pid);
        return 0;
    }
    // After exec in place the guest pid's own host process waits for the new
    // image, so it ends with it: waiting on `pid` is enough.
    auto watch = std::make_shared<Watch>();
    watch->pid = pid; watch->process = process; watch->on_exit = std::move(on_exit);
    std::unique_lock lock(watches_mutex);
    watch->token = ++next_token;
    watches[watch->token] = watch;
    const auto registered = host::sync::watch_once(process, process_ended, watch.get());
    if (!registered) {
        watches.erase(watch->token);
        lock.unlock();
        host::close(process);
        watch->on_exit(pid);
        return 0;
    }
    watch->wait = *registered;
    return watch->token;
}

bool unwatch_exit(std::uint64_t token) {
    std::shared_ptr<Watch> watch;
    {
        std::lock_guard lock(watches_mutex);
        const auto found = watches.find(token);
        if (found == watches.end()) return false;
        watch = found->second;
    }
    int armed = 0;
    if (watch->state.compare_exchange_strong(armed, 2)) {
        { std::lock_guard lock(watches_mutex); watches.erase(token); }
        host::sync::unwatch(watch->wait, true);   // waits for a callback that already started
        host::close(watch->process);
        return true;
    }
    // It fired: wait for the callback to finish (unless this is the callback).
    if (watch->firing_thread != host::process::current_thread_id()) {
        std::unique_lock lock(watch->done_mutex);
        watch->done_signal.wait(lock, [&] { return watch->done; });
    }
    return false;
}

} // namespace alchemy::process_tree
