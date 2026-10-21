#pragma once

// EVFILT_MACHPORT for kevent/kevent64 (src/alchemy/darwin/libsystem/net/sockets.cpp). The
// Mach IPC provider (src/alchemy/darwin/libsystem/mach/, alchemy-fx) registers
// itself per process; without one, EVFILT_MACHPORT fails with ENOTSUP as
// before. Header-only so the core library carries no dependency on the GUI
// build.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace transmute { class GuestMemory; }

namespace alchemy {

class ProcessContext;

class MachPortEvents {
public:
    // A delivered EVFILT_MACHPORT event (kevent64_s fields the kernel writes).
    struct Event {
        std::uint64_t data{};    // the port with a message (detection) or 0 (direct receive)
        std::uint32_t fflags{};  // the mach_msg_return_t of a direct receive (MACH_RCV_MSG)
        std::uint64_t ext1{};    // the received size (direct receive)
        std::uint64_t stamp{};   // the newest queued message (detection), for EV_CLEAR
    };
    virtual ~MachPortEvents() = default;
    // EV_ADD of `name`: 0 for a receive right or port set of the task, else
    // the errno the kernel reports (ENOENT for no such name, ENOTSUP for
    // other rights).
    virtual std::uint32_t attach(std::uint32_t name) = 0;
    // A message is queued on `name` (or a member of the set). With
    // MACH_RCV_MSG in `fflags` it is received into the buffer ext0 of size
    // ext1 with the trailer that `fflags` requests.
    virtual std::optional<Event> poll(transmute::GuestMemory& memory, std::uint32_t name, std::uint32_t fflags,
                                      std::uint64_t ext0, std::uint64_t ext1) = 0;
    // A message newer than `stamp` (Event::stamp) is queued on `name`.
    virtual bool newer(std::uint32_t name, std::uint64_t stamp) = 0;
    // `name` is a dead name (its port died): DISPATCH_MACH_SEND_DEAD.
    virtual bool dead_name(std::uint32_t name) = 0;
    // A counter that changes whenever a message is queued or a port changes
    // state; wait() returns early when it differs from `seen`.
    virtual std::uint64_t epoch() = 0;
    virtual void wait(std::uint64_t seen, std::chrono::steady_clock::duration limit) = 0;
};

namespace mach_port_events_detail {
struct Registry {
    std::mutex mutex;
    // Weak: the provider is owned by its installed exports.
    std::map<const ProcessContext*, std::weak_ptr<MachPortEvents>> sources;
};
inline Registry& registry() {
    static Registry instance;
    return instance;
}
} // namespace mach_port_events_detail

inline void set_mach_port_events(const ProcessContext& process, const std::shared_ptr<MachPortEvents>& source) {
    auto& registry = mach_port_events_detail::registry();
    std::lock_guard lock(registry.mutex);
    if (source) registry.sources[&process] = source;
    else registry.sources.erase(&process);
}

inline std::shared_ptr<MachPortEvents> mach_port_events(const ProcessContext& process) {
    auto& registry = mach_port_events_detail::registry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.sources.find(&process);
    return found == registry.sources.end() ? nullptr : found->second.lock();
}

} // namespace alchemy
