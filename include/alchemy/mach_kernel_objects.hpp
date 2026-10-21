#pragma once

// Kernel-object Mach ports for providers outside libSystem's Mach IPC
// (src/alchemy/darwin/libsystem/mach/, alchemy-fx): an object of another
// framework named by a Mach send right whose receive right the kernel holds,
// like IOSurfaceCreateMachPort's IOSurface port. The Mach provider registers
// itself per process; without one (alchemy.exe) mach_kernel_objects() is
// nullptr and such ports cannot be made. Header-only so the core library
// carries no dependency on the GUI build.
//
// Rights: make_port returns a name holding one send right in the task's
// space; the guest releases it as usual (mach_port_deallocate). The port and
// its object live while send rights to it exist anywhere in the process; the
// object's destructor may run on any thread with the IPC space locked, so it
// must not call guest code or take other host locks (defer such work).
// Other processes: an object travels when it describes itself (wire()) and
// the recipient has a receiver for its kind. Its bytes are copied; its guest
// memory range is shared, not copied (it must be fx shared memory, such as
// vm_allocate'd pages: the recipient maps the same host section). Without
// either, a message carrying the port to another process fails as for any
// kernel object that cannot be sent.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alchemy {

class ProcessContext;

struct MachKernelObject {
    virtual ~MachKernelObject() = default;
    // The object for another process: the receiver kind that makes it there,
    // its bytes, and the guest range [address, address + size) that is shared
    // with it (size 0: none). Called on the sending guest thread without the
    // IPC space locked. nullopt: it cannot travel.
    struct Wire {
        std::string kind;
        std::vector<std::uint8_t> data;
        std::uint64_t address{}, size{};
    };
    [[nodiscard]] virtual std::optional<Wire> wire() const { return std::nullopt; }
};

class MachKernelObjects {
public:
    virtual ~MachKernelObjects() = default;
    // A new kernel-held port for `object` and a send right to it in the
    // task's space (its name; 0 when no name is left).
    [[nodiscard]] virtual std::uint32_t make_port(std::shared_ptr<MachKernelObject> object) = 0;
    // The object a send right in the task's space names; nullptr when `name`
    // is no send right to such a port (or to a dead one).
    [[nodiscard]] virtual std::shared_ptr<MachKernelObject> object(std::uint32_t name) = 0;
};

// Makes `kind` objects that arrive from other processes, on a guest thread of
// this process: `data` is the sender's bytes and [address, address + size)
// the shared range, already mapped read-write here (size 0: none). The
// receiver owns that mapping (vm_deallocate it when it is no longer needed,
// also when it returns nullptr: the port then arrives dead). One receiver per
// kind and process; it may be registered before the Mach provider exists.
using MachKernelObjectReceiver = std::function<std::shared_ptr<MachKernelObject>(
    const std::vector<std::uint8_t>& data, std::uint64_t address, std::uint64_t size)>;

namespace mach_kernel_objects_detail {
struct Registry {
    std::mutex mutex;
    // Weak: the provider is owned by its installed exports.
    std::map<const ProcessContext*, std::weak_ptr<MachKernelObjects>> sources;
    std::map<std::pair<const ProcessContext*, std::string>, MachKernelObjectReceiver> receivers;
};
inline Registry& registry() {
    static Registry instance;
    return instance;
}
} // namespace mach_kernel_objects_detail

inline void set_mach_kernel_objects(const ProcessContext& process, const std::shared_ptr<MachKernelObjects>& source) {
    auto& registry = mach_kernel_objects_detail::registry();
    std::lock_guard lock(registry.mutex);
    if (source) registry.sources[&process] = source;
    else registry.sources.erase(&process);
}

inline void set_mach_kernel_object_receiver(const ProcessContext& process, std::string kind, MachKernelObjectReceiver receiver) {
    auto& registry = mach_kernel_objects_detail::registry();
    std::lock_guard lock(registry.mutex);
    if (receiver) registry.receivers[{&process, std::move(kind)}] = std::move(receiver);
    else registry.receivers.erase({&process, kind});
}

inline MachKernelObjectReceiver mach_kernel_object_receiver(const ProcessContext& process, const std::string& kind) {
    auto& registry = mach_kernel_objects_detail::registry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.receivers.find({&process, kind});
    return found == registry.receivers.end() ? MachKernelObjectReceiver{} : found->second;
}

inline std::shared_ptr<MachKernelObjects> mach_kernel_objects(const ProcessContext& process) {
    auto& registry = mach_kernel_objects_detail::registry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.sources.find(&process);
    return found == registry.sources.end() ? nullptr : found->second.lock();
}

} // namespace alchemy
