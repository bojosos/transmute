#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/host/sync.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>

// Interprocess primitives a renderer plug-in imports from libSystem.
//
// POSIX named semaphores (sem_open & co.) are host named semaphores in the
// session namespace ("alchemy.psem.<name>"), so two Alchemy processes
// (Blender and the plug-in's server) share them by name. Divergence: sem_unlink
// cannot remove a name that another handle still holds; the object disappears
// with its last handle, so a later O_CREAT|O_EXCL fails with EEXIST until then.
//
// System V semaphores are in libsystem_sysv.cpp (host shared memory; the
// entries below are only their fallback when that table cannot be created).
// POSIX shared memory objects (shm_open/shm_unlink) and mmap(MAP_SHARED) are
// DarwinServices' (host files viewed through fx sections). System V shared
// memory (shmget/shmat) is not available: these calls fail with the
// documented "not available" errors (ENOSYS; shmdt EINVAL because no segment
// can have been attached) and print one diagnostic per interface.
namespace alchemy::export_fill {
namespace {

// <fcntl.h>
constexpr std::uint64_t o_creat = 0x200, o_excl = 0x800;
// <sys/semaphore.h>: SEM_VALUE_MAX
constexpr std::uint32_t sem_value_max = 32767;
constexpr std::size_t psem_name_max = 31;   // PSEMNAMLEN

struct Semaphores {
    std::mutex mutex;
    std::map<std::uint64_t, host::Handle> handles;
    std::set<std::string> reported;
};

// Whether `text` is well-formed UTF-8 (shortest forms, no surrogates).
bool valid_utf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
        if (!length || index + length > text.size()) return false;
        std::uint32_t value = length == 1 ? lead : lead & (0x7fU >> length);
        for (std::size_t next = 1; next < length; ++next) {
            const auto byte = static_cast<unsigned char>(text[index + next]);
            if ((byte >> 6) != 2) return false;
            value = (value << 6) | (byte & 0x3fU);
        }
        constexpr std::uint32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
        if (value < smallest[length] || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        index += length;
    }
    return true;
}
// The host object name: "alchemy.psem.<name>" ('\\' would be a namespace separator: '/').
std::string object_name(std::string name) {
    if (!valid_utf8(name)) throw Errno{einval};
    for (auto& character : name) if (character == '\\') character = '/';
    return "alchemy.psem." + name;
}

std::string semaphore_name(const GuestMemory& memory, std::uint64_t address) {
    auto name = guest_string(memory, address, 1024);
    if (name.size() > psem_name_max) throw Errno{enametoolong};
    if (name.empty()) throw Errno{einval};
    return name;
}

} // namespace

void install_libsystem_ipc(Provider& p) {
    auto& process = p.process();
    auto state = std::make_shared<Semaphores>();
    const auto unavailable = [&process, state](const char* name) {
        std::lock_guard lock(state->mutex);
        if (!state->reported.insert(name).second) return;
        const std::string message = std::string("Alchemy: ") + name +
            " is unsupported (no System V shared memory between Alchemy processes)\n";
        static_cast<void>(process.output(2, std::span(reinterpret_cast<const std::uint8_t*>(message.data()), message.size())));
    };
    const auto handle = [state](std::uint64_t semaphore) {
        std::lock_guard lock(state->mutex);
        const auto found = state->handles.find(semaphore);
        if (found == state->handles.end()) throw Errno{einval};
        return found->second;
    };
    // sem_t* sem_open(const char* name, int oflag, ... /* mode_t mode, unsigned value */)
    p.libsystem("_sem_open", [&process, state](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto name = semaphore_name(m, s.x[0]);
        const auto flags = s.x[1];
        const auto object = object_name(name);
        host::Handle created;
        if (flags & o_creat) {
            const auto value = static_cast<std::uint32_t>(variadic(s, m, 1));
            if (value > sem_value_max) throw Errno{einval};
            const auto semaphore = host::sync::create_semaphore(object, value, sem_value_max, (flags & o_excl) != 0);
            if (!semaphore) {
                const auto error = semaphore.error();
                throw Errno{error == host::Error::exists ? eexist : error == host::Error::access_denied ? eacces : einval};
            }
            created = *semaphore;
        } else {
            const auto semaphore = host::sync::open_semaphore(object);
            if (!semaphore) throw Errno{semaphore.error() == host::Error::access_denied ? eacces : enoent};
            created = *semaphore;
        }
        // The guest sees an opaque sem_t* (one allocated cell per open).
        std::uint64_t cell{};
        try { cell = process.allocate_guest(8); } catch (...) { host::close(created); throw; }
        m.write64(cell, 0);
        std::lock_guard lock(state->mutex);
        state->handles.emplace(cell, created);
        return cell;
    });
    p.libsystem("_sem_close", [&process, state](CpuState& s, GuestMemory&) -> std::uint64_t {
        host::Handle value;
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->handles.find(s.x[0]);
            if (found == state->handles.end()) throw Errno{einval};
            value = found->second;
            state->handles.erase(found);
        }
        host::close(value);
        process.release_guest(s.x[0]);
        return 0;
    });
    p.libsystem("_sem_unlink", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto existing = host::sync::open_semaphore(object_name(semaphore_name(m, s.x[0])));
        if (!existing) throw Errno{enoent};
        host::close(*existing);
        return 0;
    });
    p.libsystem("_sem_post", [handle](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!host::sync::release_semaphore(handle(s.x[0]), 1)) throw Errno{einval};
        return 0;
    });
    p.libsystem("_sem_trywait", [handle](CpuState& s, GuestMemory&) -> std::uint64_t {
        const std::array semaphores{handle(s.x[0])};
        const auto taken = host::sync::wait_any(semaphores, 0);
        if (taken) return 0;
        throw Errno{taken.error() == host::Error::timed_out ? eagain : einval};
    });
    // Blocks in slices so a process exit request interrupts the wait (EINTR).
    p.libsystem("_sem_wait", [&process, handle](CpuState& s, GuestMemory&) -> std::uint64_t {
        const std::array semaphores{handle(s.x[0])};
        for (;;) {
            const auto taken = host::sync::wait_any(semaphores, 20'000'000);
            if (taken) return 0;
            if (taken.error() != host::Error::timed_out) throw Errno{einval};
            if (process_exiting(process)) throw Errno{eintr};
        }
    });
    // semget, semctl and semop: libsystem_sysv.cpp (unavailable there too when
    // its shared table cannot be created).
    for (const auto* name : {"_shmget", "_shmat", "_shmctl"}) {
        p.libsystem(name, [unavailable, name](CpuState&, GuestMemory&) -> std::uint64_t {
            unavailable(name + 1);
            throw Errno{enosys};
        });
    }
    // int shmdt(const void*): shmat never attaches a segment, so no address is one.
    p.libsystem("_shmdt", [](CpuState&, GuestMemory&) -> std::uint64_t { throw Errno{einval}; });
}

} // namespace alchemy::export_fill
