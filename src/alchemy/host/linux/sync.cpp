// Linux host backend, area "sync".
//
// Every waitable object is a descriptor that poll(2) reports readable while
// it is signalled: events, mutexes and unnamed semaphores are eventfds,
// processes are pidfds. wait_any polls them and, for the kinds a wait
// consumes (auto-reset events, mutexes, semaphores), takes the signal with a
// non-blocking read; a waiter that loses that race polls again.
//
// Named mutexes and semaphores (shared by name between this user's
// processes) are FIFOs in $XDG_RUNTIME_DIR/alchemy-sync (else
// /tmp/alchemy-sync.<uid>): the count is the bytes waiting in the FIFO, a
// wait reads one, a release writes them. Every process that has one open
// holds it read-write, so a FIFO nobody holds (whose bytes are gone) is
// recognised and made afresh. A named semaphore's maximum is in a ".max" file
// beside it. A mutex whose owner exits holding it stays locked.
//
// The registry keeps, per descriptor this file made, its kind, the owner of a
// mutex and the count and maximum of an unnamed semaphore.
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/time.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace alchemy::host::sync {

namespace {

using linux_backend::fd_of;
using linux_backend::handle_of;
using linux_backend::last_error;

enum class Kind : std::uint8_t { manual_event, auto_event, mutex, semaphore };

struct Object {
    Kind kind{};
    bool fifo{};                 // a named object: one byte per count
    std::uint64_t owner{};       // a mutex's owning thread (0: unlocked by us)
    std::uint32_t count{};       // an unnamed semaphore's count
    std::uint32_t maximum{};     // a semaphore's maximum (0: none)
};

std::mutex g_lock;
std::map<int, Object>& kinds() {
    static auto* map = new std::map<int, Object>;
    return *map;
}

std::optional<Object> object_of(int fd) {
    std::lock_guard guard(g_lock);
    const auto it = kinds().find(fd);
    if (it == kinds().end()) return std::nullopt;
    return it->second;
}

Result<Handle> make(Kind kind, unsigned initial, std::uint32_t maximum = 0) {
    const int flags = EFD_CLOEXEC | EFD_NONBLOCK | (kind == Kind::semaphore ? EFD_SEMAPHORE : 0);
    const int fd = eventfd(initial, flags);
    if (fd < 0) return last_error();
    std::lock_guard guard(g_lock);
    kinds()[fd] = Object{kind, false, 0, static_cast<std::uint32_t>(initial), maximum};
    return handle_of(fd);
}

// Takes the signal of a consuming object; false if another waiter got it first.
bool take(int fd) {
    const auto object = object_of(fd);
    if (!object || object->kind == Kind::manual_event) return true;
    bool taken = false;
    if (object->fifo) {
        char token = 0;
        taken = ::read(fd, &token, 1) == 1;
    } else {
        std::uint64_t value = 0;
        taken = ::read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
    }
    if (taken) {
        std::lock_guard guard(g_lock);
        if (auto it = kinds().find(fd); it != kinds().end()) {
            if (it->second.kind == Kind::mutex) it->second.owner = process::current_thread_id();
            if (it->second.kind == Kind::semaphore && !it->second.fifo && it->second.count) --it->second.count;
        }
    }
    return taken;
}

Status signal(Handle object, std::uint64_t count) {
    if (::write(fd_of(object), &count, sizeof(count)) != static_cast<ssize_t>(sizeof(count))) return last_error();
    return {};
}

Status put_tokens(int fd, std::uint32_t count) {
    const std::string tokens(count, '\x01');
    if (::write(fd, tokens.data(), tokens.size()) != static_cast<ssize_t>(tokens.size())) return last_error();
    return {};
}

std::string sync_directory() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string directory = runtime && *runtime ? std::string(runtime) + "/alchemy-sync" : "/tmp/alchemy-sync." + std::to_string(getuid());
    mkdir(directory.c_str(), 0700);
    return directory;
}

std::string fifo_path(std::string_view name) {
    std::string file;
    for (const char c : name) file += (c == '/' || c == '\\') ? '_' : c;
    return sync_directory() + "/" + file;
}

// Opens the named FIFO read-write; `created` says whether this call made it
// (a FIFO nobody held counts as made afresh). Error::not_found when it does
// not exist and `create` is false; Error::exists for an exclusive create of
// one in use.
Result<int> open_named(std::string_view name, bool create, bool exclusive, bool& created) {
    const auto path = fifo_path(name);
    created = false;
    // A FIFO with no reader is stale: its bytes went with its last holder.
    const int probe = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    const bool live = probe >= 0;
    if (probe >= 0) ::close(probe);
    if (!live) {
        if (!create) return Error::not_found;
        unlink(path.c_str());
        if (mkfifo(path.c_str(), 0600) != 0 && errno != EEXIST) return last_error();
        created = true;
    } else if (exclusive) {
        return Error::exists;
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return last_error();
    return fd;
}

Result<Handle> make_named(Kind kind, std::string_view name, std::uint32_t initial, std::uint32_t maximum, bool exclusive) {
    bool created = false;
    const auto fd = open_named(name, true, exclusive, created);
    if (!fd) return fd.error();
    const auto max_path = fifo_path(name) + ".max";
    if (created) {
        if (initial) static_cast<void>(put_tokens(*fd, initial));
        std::ofstream(max_path) << maximum;
    } else {
        std::ifstream(max_path) >> maximum;
    }
    std::lock_guard guard(g_lock);
    kinds()[*fd] = Object{kind, true, 0, 0, maximum};
    return handle_of(*fd);
}

struct Watch {
    int cancel{-1};
    std::thread thread;
};
std::map<int, Watch*>& watches() {
    static auto* map = new std::map<int, Watch*>;
    return *map;
}

} // namespace

Result<Handle> create_event(bool manual_reset, bool initially_set) {
    return make(manual_reset ? Kind::manual_event : Kind::auto_event, initially_set ? 1 : 0);
}

Status set_event(Handle event) {
    // An eventfd counts; an event is set or not, so drain before setting.
    std::uint64_t value = 0;
    while (::read(fd_of(event), &value, sizeof(value)) > 0) {
    }
    return signal(event, 1);
}

Status reset_event(Handle event) {
    std::uint64_t value = 0;
    while (::read(fd_of(event), &value, sizeof(value)) > 0) {
    }
    return {};
}

Result<std::size_t> wait_any(std::span<const Handle> handles, std::optional<std::uint64_t> timeout_ns) {
    if (handles.size() > max_wait_handles) return Error::invalid_argument;
    pollfd fds[max_wait_handles];
    const auto deadline = timeout_ns ? time::monotonic_ns() + *timeout_ns : 0;
    for (;;) {
        for (std::size_t i = 0; i < handles.size(); ++i) fds[i] = {fd_of(handles[i]), POLLIN, 0};
        int wait_ms = -1;
        if (timeout_ns) {
            const auto now = time::monotonic_ns();
            if (now >= deadline) wait_ms = 0;
            else wait_ms = static_cast<int>(std::min<std::uint64_t>((deadline - now + 999999) / 1000000, 0x7fffffff));
        }
        const int ready = poll(fds, handles.size(), wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return last_error();
        }
        if (ready == 0) {
            if (timeout_ns && time::monotonic_ns() >= deadline) return Error::timed_out;
            continue;
        }
        for (std::size_t i = 0; i < handles.size(); ++i)
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) && take(fds[i].fd)) return i;
    }
}

Result<Handle> watch_once(Handle object, void (*callback)(void* context), void* context) {
    const int cancel = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (cancel < 0) return last_error();
    auto* watch = new Watch{cancel, {}};
    const int target = fd_of(object);
    try {
        watch->thread = std::thread([cancel, target, callback, context] {
            pollfd fds[2] = {{target, POLLIN, 0}, {cancel, POLLIN, 0}};
            for (;;) {
                if (poll(fds, 2, -1) < 0) {
                    if (errno == EINTR) continue;
                    return;
                }
                if (fds[1].revents) return;   // unwatched
                if (fds[0].revents && take(target)) break;
            }
            callback(context);
        });
    } catch (...) {
        ::close(cancel);
        delete watch;
        return Error::no_memory;
    }
    std::lock_guard guard(g_lock);
    watches()[cancel] = watch;
    return handle_of(cancel);
}

void unwatch(Handle watch_handle, bool wait_for_callback) noexcept {
    Watch* watch = nullptr;
    {
        std::lock_guard guard(g_lock);
        const auto it = watches().find(fd_of(watch_handle));
        if (it == watches().end()) return;
        watch = it->second;
        watches().erase(it);
    }
    const std::uint64_t one = 1;
    static_cast<void>(::write(watch->cancel, &one, sizeof(one)));
    if (wait_for_callback && watch->thread.get_id() != std::this_thread::get_id()) watch->thread.join();
    else watch->thread.detach();
    ::close(watch->cancel);
    delete watch;
}

Result<Handle> create_mutex(std::string_view name) {
    if (!name.empty()) return make_named(Kind::mutex, name, 1, 1, false);
    return make(Kind::mutex, 1);
}

Status unlock_mutex(Handle mutex) {
    const int fd = fd_of(mutex);
    bool fifo = false;
    {
        std::lock_guard guard(g_lock);
        const auto it = kinds().find(fd);
        if (it == kinds().end() || it->second.kind != Kind::mutex) return Error::invalid_argument;
        if (it->second.owner != process::current_thread_id()) return Error::permission;   // not ours to unlock
        it->second.owner = 0;
        fifo = it->second.fifo;
    }
    return fifo ? put_tokens(fd, 1) : signal(mutex, 1);
}

Result<Handle> create_semaphore(std::string_view name, std::uint32_t initial, std::uint32_t maximum, bool exclusive) {
    if (!name.empty()) return make_named(Kind::semaphore, name, initial, maximum, exclusive);
    return make(Kind::semaphore, initial, maximum);
}

Result<Handle> open_semaphore(std::string_view name) {
    bool created = false;
    const auto fd = open_named(name, false, false, created);
    if (!fd) return fd.error();
    std::uint32_t maximum = 0;
    std::ifstream(fifo_path(name) + ".max") >> maximum;
    std::lock_guard guard(g_lock);
    kinds()[*fd] = Object{Kind::semaphore, true, 0, 0, maximum};
    return handle_of(*fd);
}

Status release_semaphore(Handle semaphore, std::uint32_t count) {
    const int fd = fd_of(semaphore);
    std::unique_lock guard(g_lock);
    const auto it = kinds().find(fd);
    if (it == kinds().end() || it->second.kind != Kind::semaphore) return Error::invalid_argument;
    auto& object = it->second;
    std::uint32_t current = object.count;
    if (object.fifo) {
        int waiting = 0;
        ioctl(fd, FIONREAD, &waiting);
        current = static_cast<std::uint32_t>(waiting);
    }
    if (object.maximum && current + count > object.maximum) return Error::out_of_range;
    if (!object.fifo) object.count += count;
    const bool fifo = object.fifo;
    guard.unlock();
    return fifo ? put_tokens(fd, count) : signal(semaphore, count);
}

} // namespace alchemy::host::sync

namespace alchemy::host::linux_backend {

void forget(int fd) noexcept {
    {
        std::lock_guard guard(sync::g_lock);
        sync::kinds().erase(fd);
    }
    forget_process(fd);
    forget_listener(fd);
}

} // namespace alchemy::host::linux_backend
