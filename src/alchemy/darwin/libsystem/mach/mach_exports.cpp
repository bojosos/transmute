#include "alchemy/darwin/libsystem/install.hpp"
#include "alchemy/darwin/libsystem/mach/mach_ipc.hpp"

#include "alchemy/cross_process_memory.hpp"
#include "alchemy/process_tree.hpp"

#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/fill_provider.hpp"
#include "alchemy/mach_kernel_objects.hpp"
#include "alchemy/mach_port_events.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

// libSystem's Mach IPC surface over the in-process space of mach_ipc.hpp
// (Chrome's MessagePumpKqueue, Mojo's ChannelMac and
// base::PlatformSharedMemoryRegion; docs/ALCHEMY_CHROME_AUDIT.md "Process
// model"). Installed before the fill-only machsys provider, whose
// "no namespace" failures then no longer apply to these names.
//
// Real: port rights and names, port sets, attributes, guards, notifications
// (dead-name, no-senders, port-destroyed, send-once, send-possible,
// port-deleted), mach_msg with timeouts, complex messages (port, OOL memory
// and OOL port descriptors) and trailers, mach_msg_destroy, MIG reply ports,
// EVFILT_MACHPORT (kevent/kevent64), task special ports, task_threads,
// identity tokens, an in-process bootstrap registry, and memory entries
// mapped with mach_vm_map as shared aliases (fx shared guest memory).
//
// Explicit failures: messages to kernel ports (no MIG servers:
// MACH_SEND_INVALID_DEST), vouchers (MACH_SEND_INVALID_VOUCHER), guarded port
// descriptors (MACH_SEND_INVALID_TYPE), memory entries over private (non
// vm_allocate) memory or on engines without shared memory
// (KERN_NOT_SUPPORTED), bootstrap services of other processes (unknown
// service; cross-process brokering is stage 5), bootstrap_parent
// (BOOTSTRAP_NOT_PRIVILEGED, as for a non-root caller).
namespace alchemy::libsystem {
namespace {

using namespace mach;
using gui::fill::Provider;
using gui::fill::libsystem;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::uint32_t task_self = 0xa1c1, host_self = 0xa1c3;   // vm/mach_vm.cpp, misc/libsystem_extra.cpp
constexpr std::uint64_t page_size = 16384, page_mask = page_size - 1;
// <servers/bootstrap.h>
constexpr std::uint32_t bootstrap_not_privileged = 1100, bootstrap_unknown_service = 1102, bootstrap_service_active = 1103;
// <mach/memory_object_types.h> MAP_MEM_* and <mach/vm_statistics.h> VM_FLAGS_*
constexpr std::uint32_t map_mem_named_create = 0x020000, map_mem_purgable = 0x040000, map_mem_named_reuse = 0x080000,
    map_mem_vm_copy = 0x200000, map_mem_vm_share = 0x400000;
constexpr std::uint32_t vm_flags_anywhere = 0x1, vm_flags_overwrite = 0x4000;
constexpr std::uint32_t vm_prot_read = 1, vm_prot_write = 2, vm_prot_execute = 4;

struct Bootstrap final : KernelObject {};
struct IdentityToken final : KernelObject {};

// Pages shared by memory entries: the anchor is a hidden alias of the
// memory (fx shared guest memory), released with the last entry.
struct Backing {
    Backing(Address at, std::uint64_t length, std::function<void(Address, std::uint64_t)> releaser)
        : anchor(at), size(length), release(std::move(releaser)) {}
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;
    Address anchor{};
    std::uint64_t size{};
    std::function<void(Address, std::uint64_t)> release;
    ~Backing() {
        if (release) {
            try { release(anchor, size); } catch (...) {}
        }
    }
};
// A fileport: an open file as a Mach send right (fileport_makeport). Its
// description owns a duplicate of the file's host handle in this process; for
// a socket (kind 5) that is a host socket of its own (`socket`), exported for
// each process the port goes to and for each fileport_makefd (a socket blob
// opens once, in the process it was made for).
struct Fileport final : KernelObject {
    PortableDescriptor descriptor;
    host::Handle socket{};
    ~Fileport() override {
        if (descriptor.kind == 1 && descriptor.handle) host::close(host::Handle{static_cast<std::uintptr_t>(descriptor.handle)});
        if (socket) host::net::close_socket(socket);
    }
    // The descriptor for process `target` (0: this one); nullopt when the socket cannot go there.
    std::optional<PortableDescriptor> socket_for(std::uint32_t target) const {
        auto blob = host::net::export_socket(socket, target);
        if (!blob) return std::nullopt;
        PortableDescriptor copy = descriptor;
        copy.blob = std::move(*blob);
        return copy;
    }
};
// A received socket blob, opened here; null when it does not open.
std::shared_ptr<Fileport> socket_fileport(PortableDescriptor descriptor) {
    const auto opened = host::net::import_socket(descriptor.blob);
    if (!opened) return nullptr;
    auto fileport = std::make_shared<Fileport>();
    descriptor.blob.clear();
    fileport->descriptor = std::move(descriptor);
    fileport->socket = *opened;
    return fileport;
}
void write_socket_fields(Writer& out, const PortableDescriptor& descriptor) {
    out.blob(descriptor.blob);
    out.u32(static_cast<std::uint32_t>(descriptor.socket_domain));
    out.u32(static_cast<std::uint32_t>(descriptor.socket_type));
    out.u32(static_cast<std::uint32_t>(descriptor.socket_protocol));
}
void read_socket_fields(Reader& in, PortableDescriptor& descriptor) {
    descriptor.blob = in.blob();
    descriptor.socket_domain = static_cast<std::int32_t>(in.u32());
    descriptor.socket_type = static_cast<std::int32_t>(in.u32());
    descriptor.socket_protocol = static_cast<std::int32_t>(in.u32());
}

struct MemoryEntry final : KernelObject {
    std::shared_ptr<Backing> backing;
    std::uint64_t offset{}, size{};
    std::uint32_t protection{};
};

// Kernel-object ports of other providers (alchemy/mach_kernel_objects.hpp,
// e.g. IOSurfaceCreateMachPort). Another process gets them as 'K' objects
// (serialize_foreign / deserialize_foreign) when they describe themselves.
struct ForeignObject final : KernelObject {
    explicit ForeignObject(std::shared_ptr<MachKernelObject> value) : object(std::move(value)) {}
    std::shared_ptr<MachKernelObject> object;
};
class ForeignPorts final : public MachKernelObjects {
public:
    explicit ForeignPorts(std::weak_ptr<MachIpc> space) : ipc_(std::move(space)) {}
    std::uint32_t make_port(std::shared_ptr<MachKernelObject> object) override {
        const auto ipc = ipc_.lock();
        return ipc && object ? ipc->make_kernel_port(std::make_shared<ForeignObject>(std::move(object))) : 0;
    }
    std::shared_ptr<MachKernelObject> object(std::uint32_t name) override {
        const auto ipc = ipc_.lock();
        if (!ipc) return nullptr;
        const auto found = std::dynamic_pointer_cast<ForeignObject>(ipc->kernel_object(name));
        return found ? found->object : nullptr;
    }
private:
    std::weak_ptr<MachIpc> ipc_;
};

class MachProvider final : public Provider {
public:
    using Provider::Provider;

    std::shared_ptr<MachIpc> ipc;
    std::shared_ptr<ForeignPorts> foreign_ports;       // alchemy/mach_kernel_objects.hpp
    std::mutex mutex;
    std::map<std::uint32_t, Right> special_ports;      // task special ports
    bool inherited_specials{};                          // the parent's posix_spawn special ports were fetched
    std::map<Address, std::vector<std::pair<std::uint32_t, Right>>> spawn_specials;   // posix_spawnattr token -> ports
    std::shared_ptr<Transport> transport;
    std::map<std::uint64_t, Name> reply_ports;         // thread identity -> MIG reply port
    Right bootstrap;
    std::once_flag audit_once;
    std::array<std::uint32_t, 8> audit{};

    // A routine on the task port: other task ports are unknown (a MIG call to
    // MACH_PORT_NULL fails to send).
    static std::optional<std::uint64_t> bad_task(std::uint64_t task) {
        const auto value = static_cast<std::uint32_t>(task);
        if (value == task_self) return std::nullopt;
        return value == 0 ? std::uint64_t{mr::send_invalid_dest} : std::uint64_t{kern::invalid_argument};
    }

    bool kernel_name(Name name) {
        if (name == task_self || name == host_self || name == 0xa1d1 || name == 0xa1d3) return true;
        if (name >= 0x5e03 && (name - 0x5e03) % 4 == 0 && name < 0x1000000) return true;   // semaphores
        const auto threads = process.thread_identities();
        return std::binary_search(threads.begin(), threads.end(), std::uint64_t{name});
    }

    Address vm_allocate(std::uint64_t size) {
        const auto slot = process.allocate_guest(8);
        memory().write64(slot, 0);
        const auto result = call(libsystem, "_vm_allocate", {task_self, slot, size ? size : 1, 1});
        const auto address = memory().read64(slot);
        process.release_guest(slot);
        return result == 0 ? address : 0;
    }
    void vm_deallocate(Address address, std::uint64_t size) {
        if (call(libsystem, "_vm_deallocate", {task_self, address, size}) == 0) return;
        // Not Mach VM memory (an mmap region): munmap's page-rounded range.
        const auto begin = address & ~page_mask;
        static_cast<void>(call(libsystem, "_munmap", {begin, ((address + size + page_mask) & ~page_mask) - begin}));
    }
    // vm_remap(task, &target, size, 0, flags, task, source, FALSE, &cur, &max, VM_INHERIT_SHARE)
    std::uint32_t vm_remap(Address& target, std::uint64_t size, std::uint32_t flags, Address source) {
        const auto block = process.allocate_guest(24);
        memory().write64(block, target);
        memory().write64(block + 8, 0);
        CallRegs registers{};
        registers.x[0] = task_self;
        registers.x[1] = block;
        registers.x[2] = size;
        registers.x[3] = 0;
        registers.x[4] = flags;
        registers.x[5] = task_self;
        registers.x[6] = source;
        registers.x[7] = 0;
        registers.push_stack(static_cast<std::uint64_t>(block + 8));
        registers.push_stack(static_cast<std::uint64_t>(block + 12));
        registers.push_stack(std::uint32_t{0});
        const auto result = static_cast<std::uint32_t>(call_regs(libsystem, "_vm_remap", registers).x0);
        target = memory().read64(block);
        process.release_guest(block);
        return result;
    }
    std::uint32_t vm_protect(Address address, std::uint64_t size, bool maximum, std::uint32_t protection) {
        return static_cast<std::uint32_t>(call(libsystem, "_vm_protect", {task_self, address, size, maximum ? 1U : 0U, protection}));
    }

    std::array<std::uint32_t, 8> audit_token() {
        std::call_once(audit_once, [&] {
            const auto uid = static_cast<std::uint32_t>(call(libsystem, "_getuid", {}));
            const auto gid = static_cast<std::uint32_t>(call(libsystem, "_getgid", {}));
            const auto pid = static_cast<std::uint32_t>(call(libsystem, "_getpid", {}));
            // {auid, euid, egid, ruid, rgid, pid, asid, pidversion}: no audit
            // sessions; the pid's generation from the process tree.
            const auto self = process_tree::self();
            const auto version = self && self->pid == pid ? self->pidversion : process_tree::pidversion(pid);
            audit = {uid, uid, gid, uid, gid, pid, 0, version};
        });
        return audit;
    }

    bool root{};

    // Kernel objects crossing processes (a tag byte, then the object):
    // 'X' a memory entry: {u32 protection, section token} (xmem, the handle
    //     already in the recipient);
    // 'M' a memory entry by value (no section behind it): {u32 protection,
    //     u64 size, contents};
    // 'F' a fileport: its description with the host handle in the recipient;
    // 'O' out-of-line memory in a section: {section token}.
    // A token is {u64 handle, u64 offset, u64 size, u8 writable}.
    static std::uint32_t host_process(std::uint32_t pid) {
        const auto record = process_tree::lookup(pid);
        return record && record->host_pid ? record->host_pid : pid;
    }
    static void write_token(Writer& out, const xmem::Token& token) {
        out.u64(token.handle);
        out.u64(token.offset);
        out.u64(token.size);
        out.u8(token.writable ? 1 : 0);
    }
    static xmem::Token read_token(Reader& in) {
        xmem::Token token;
        token.handle = in.u64();
        token.offset = in.u64();
        token.size = in.u64();
        token.writable = in.u8() != 0;
        return token;
    }

    // Foreign kernel objects ('K', alchemy/mach_kernel_objects.hpp): the
    // receiver kind, the object's bytes, and its shared guest range as a
    // section token (forwarded and discarded like a memory entry's).
    std::optional<std::vector<std::uint8_t>> serialize_foreign(const ForeignObject& foreign, std::uint32_t recipient) {
        const auto wire = foreign.object->wire();
        if (!wire || wire->kind.empty()) return std::nullopt;
        Writer out;
        out.u8('K');
        out.text(wire->kind);
        out.blob(wire->data);
        if (!wire->size) {
            out.u8(0);
            return std::move(out.bytes);
        }
        const auto shared = xmem::section_of(process, wire->address, wire->size);
        if (!shared) return std::nullopt;   // private memory cannot be shared
        const auto token = xmem::export_section(shared->first, shared->second, wire->size, host_process(recipient), true);
        if (!token) return std::nullopt;
        out.u8(1);
        write_token(out, *token);
        return std::move(out.bytes);
    }
    std::shared_ptr<KernelObject> deserialize_foreign(const std::vector<std::uint8_t>& tagged) {
        Reader in{tagged};
        static_cast<void>(in.u8());
        const auto kind = in.text();
        const auto data = in.blob();
        Address at{};
        std::uint64_t size{};
        if (in.u8()) {
            const auto token = read_token(in);
            const auto section = xmem::import_token(token);   // owned here from now on
            if ((token.offset & page_mask) || !token.size || (token.size & page_mask)) return nullptr;
            const auto protection = token.writable ? (vm_prot_read | vm_prot_write) : vm_prot_read;
            std::uint32_t result = kern::success;
            const auto anchor = xmem::map_section(process, section, token.offset, token.size, 0, protection, result, protection);
            if (!anchor) return nullptr;
            at = *anchor;
            size = token.size;
        }
        const auto receiver = mach_kernel_object_receiver(process, kind);
        if (!receiver) {
            if (at) vm_deallocate(at, size);
            notice_once("a Mach port of kind " + kind + " arrived from another process with no receiver here (dead name)");
            return nullptr;
        }
        auto object = receiver(data, at, size);
        return object ? std::make_shared<ForeignObject>(std::move(object)) : nullptr;
    }
    std::optional<std::vector<std::uint8_t>> serialize_object(const std::shared_ptr<KernelObject>& object, std::uint32_t recipient) {
        if (const auto fileport = std::dynamic_pointer_cast<Fileport>(object)) {
            PortableDescriptor copy = fileport->descriptor;
            if (copy.kind == 1) {
                const auto duplicate = host::process::duplicate_into(host::Handle{static_cast<std::uintptr_t>(copy.handle)}, host_process(recipient));
                if (!duplicate) return std::nullopt;
                copy.handle = *duplicate;
            }
            if (copy.kind == 5) {   // a socket: made for the recipient (WSADuplicateSocket)
                auto made = fileport->socket_for(host_process(recipient));
                if (!made) return std::nullopt;
                copy = std::move(*made);
            }
            Writer out;
            out.u8('F');
            out.u32(copy.kind);
            out.u64(copy.handle);
            out.u64(copy.flags);
            out.text(copy.path);
            if (copy.kind == 5) write_socket_fields(out, copy);
            return std::move(out.bytes);
        }
        if (const auto foreign = std::dynamic_pointer_cast<ForeignObject>(object)) return serialize_foreign(*foreign, recipient);
        const auto entry = std::dynamic_pointer_cast<MemoryEntry>(object);
        if (!entry) return std::nullopt;
        if (const auto shared = xmem::section_of(process, entry->backing->anchor + entry->offset, entry->size)) {
            const auto token = xmem::export_section(shared->first, shared->second, entry->size, host_process(recipient),
                                                    (entry->protection & vm_prot_write) != 0);
            if (!token) return std::nullopt;
            Writer out;
            out.u8('X');
            out.u32(entry->protection);
            write_token(out, *token);
            return std::move(out.bytes);
        }
        if (entry->size > (256ULL << 20)) return std::nullopt;
        std::vector<std::uint8_t> data(13 + entry->size);
        data[0] = 'M';
        std::memcpy(data.data() + 1, &entry->protection, 4);
        std::memcpy(data.data() + 5, &entry->size, 8);
        memory().read_bytes(entry->backing->anchor + entry->offset, std::span(data.data() + 13, entry->size));
        notice_once("a Mach memory entry without a host section was sent to another process by value (a copy)");
        return data;
    }
    std::shared_ptr<KernelObject> deserialize_object(const std::vector<std::uint8_t>& tagged) {
        if (tagged.empty()) return nullptr;
        if (tagged[0] == 'F') {
            Reader in{tagged};
            static_cast<void>(in.u8());
            auto fileport = std::make_shared<Fileport>();
            fileport->descriptor.kind = in.u32();
            fileport->descriptor.handle = in.u64();
            fileport->descriptor.flags = in.u64();
            fileport->descriptor.path = in.text();
            if (fileport->descriptor.kind == 5) {
                read_socket_fields(in, fileport->descriptor);
                return socket_fileport(std::move(fileport->descriptor));
            }
            return fileport;
        }
        if (tagged[0] == 'X') {
            Reader in{tagged};
            static_cast<void>(in.u8());
            const auto protection = in.u32() & (vm_prot_read | vm_prot_write);
            const auto token = read_token(in);
            const auto section = xmem::import_token(token);   // owned here from now on
            const auto mapped = token.writable ? protection : protection & vm_prot_read;
            if ((token.offset & page_mask) || !token.size || (token.size & page_mask)) return nullptr;
            std::uint32_t result = kern::success;
            const auto anchor = xmem::map_section(process, section, token.offset, token.size, 0, mapped, result, mapped);
            if (!anchor) return nullptr;
            auto entry = std::make_shared<MemoryEntry>();
            entry->backing = std::make_shared<Backing>(*anchor, token.size, [weak = std::weak_ptr<Provider>(shared_from_this())](Address at, std::uint64_t length) {
                if (const auto self = std::static_pointer_cast<MachProvider>(weak.lock())) self->vm_deallocate(at, length);
            });
            entry->size = token.size;
            entry->protection = mapped;
            return entry;
        }
        if (tagged[0] == 'O') {
            discard_object(tagged);
            return nullptr;
        }
        if (tagged[0] == 'K') return deserialize_foreign(tagged);
        if (tagged[0] != 'M') return nullptr;
        const std::vector<std::uint8_t> data(tagged.begin() + 1, tagged.end());
        if (data.size() < 12) return nullptr;
        std::uint32_t protection{};
        std::uint64_t size{};
        std::memcpy(&protection, data.data(), 4);
        std::memcpy(&size, data.data() + 4, 8);
        if (data.size() != 12 + size || !size) return nullptr;
        const auto anchor = vm_allocate(size);
        if (!anchor) return nullptr;
        if (!engine_is_shared_memory(process.calling_engine(), anchor, size)) { vm_deallocate(anchor, size); return nullptr; }
        memory().write_bytes(anchor, std::span(data.data() + 12, size));
        auto entry = std::make_shared<MemoryEntry>();
        entry->backing = std::make_shared<Backing>(anchor, size, [weak = std::weak_ptr<Provider>(shared_from_this())](Address at, std::uint64_t length) {
            if (const auto self = std::static_pointer_cast<MachProvider>(weak.lock())) self->vm_deallocate(at, length);
        });
        entry->size = size;
        entry->protection = protection;
        return entry;
    }

    // Host-only (transport threads): an object received from another process
    // passed on to `recipient`, its handle moved there.
    static std::optional<std::vector<std::uint8_t>> forward_object(const std::vector<std::uint8_t>& tagged, std::uint32_t recipient) {
        if (tagged.empty()) return std::nullopt;
        Reader in{tagged};
        const auto tag = in.u8();
        if (tag == 'K') {
            Writer out;
            out.u8('K');
            out.text(in.text());
            const auto data = in.blob();
            out.blob(data);
            const auto shared = in.u8();
            out.u8(shared);
            if (!shared) return std::move(out.bytes);
            const auto token = read_token(in);
            const auto section = xmem::import_token(token);   // this copy closes with `section`
            const auto moved = xmem::export_section(section, token.offset, token.size, host_process(recipient), token.writable);
            if (!moved) return std::nullopt;
            write_token(out, *moved);
            return std::move(out.bytes);
        }
        if (tag == 'X' || tag == 'O') {
            const auto protection = tag == 'X' ? in.u32() : 0U;
            const auto token = read_token(in);
            const auto section = xmem::import_token(token);   // this copy closes with `section`
            const auto moved = xmem::export_section(section, token.offset, token.size, host_process(recipient), token.writable);
            if (!moved) return std::nullopt;
            Writer out;
            out.u8(tag);
            if (tag == 'X') out.u32(protection);
            write_token(out, *moved);
            return std::move(out.bytes);
        }
        if (tag == 'F') {
            Writer out;
            out.u8('F');
            const auto kind = in.u32();
            auto handle = in.u64();
            const auto flags = in.u64();
            const auto path = in.text();
            if (kind == 1) {
                // Moved, not copied: this process's handle goes either way.
                const auto source = host::Handle{static_cast<std::uintptr_t>(handle)};
                const auto duplicate = host::process::duplicate_into(source, host_process(recipient));
                host::close(source);
                if (!duplicate) return std::nullopt;
                handle = *duplicate;
            }
            out.u32(kind);
            out.u64(handle);
            out.u64(flags);
            out.text(path);
            if (kind == 5) {
                // A socket made for this process: opened here and made again for the recipient.
                PortableDescriptor descriptor;
                descriptor.kind = kind;
                read_socket_fields(in, descriptor);
                const auto fileport = socket_fileport(std::move(descriptor));
                auto made = fileport ? fileport->socket_for(host_process(recipient)) : std::nullopt;
                if (!made) return std::nullopt;
                write_socket_fields(out, *made);
            }
            return std::move(out.bytes);
        }
        return tagged;   // by value
    }
    // Host-only: an object received from another process, never delivered.
    static void discard_object(const std::vector<std::uint8_t>& tagged) {
        if (tagged.empty()) return;
        Reader in{tagged};
        const auto tag = in.u8();
        if (tag == 'X' || tag == 'O') {
            if (tag == 'X') static_cast<void>(in.u32());
            xmem::discard_token(read_token(in));
        } else if (tag == 'K') {
            static_cast<void>(in.text());
            static_cast<void>(in.blob());
            if (in.u8()) xmem::discard_token(read_token(in));
        } else if (tag == 'F') {
            const auto kind = in.u32();
            const auto handle = in.u64();
            if (kind == 1 && handle) host::close(host::Handle{static_cast<std::uintptr_t>(handle)});
            if (kind == 5) {   // a socket made for this process: opened and closed, so the duplicate does not linger
                static_cast<void>(in.u64());
                static_cast<void>(in.text());
                PortableDescriptor descriptor;
                read_socket_fields(in, descriptor);
                static_cast<void>(socket_fileport(std::move(descriptor)));
            }
        }
    }
    // Host-only: out-of-line bytes in a new section for `recipient` ('O').
    static std::optional<std::vector<std::uint8_t>> export_ool(const std::vector<std::uint8_t>& data, std::uint32_t recipient) {
        const auto section = xmem::create_section(data.size());
        if (!section) return std::nullopt;
        const auto view = host::sections::map(host::Handle{reinterpret_cast<std::uintptr_t>(section->handle.get())}, 0, data.size(), true);
        if (!view) return std::nullopt;
        std::memcpy(reinterpret_cast<void*>(*view), data.data(), data.size());
        static_cast<void>(host::sections::unmap(*view));
        const auto token = xmem::export_section(*section, 0, data.size(), host_process(recipient), false);
        if (!token) return std::nullopt;
        Writer out;
        out.u8('O');
        write_token(out, *token);
        return std::move(out.bytes);
    }
    // Guest thread: out-of-line memory from a section, copied into new memory.
    Address receive_ool(const std::vector<std::uint8_t>& tagged, std::uint64_t size) {
        if (tagged.empty() || tagged[0] != 'O') { discard_object(tagged); return 0; }
        Reader in{tagged};
        static_cast<void>(in.u8());
        const auto token = read_token(in);
        if (token.size != size) { xmem::discard_token(token); return 0; }
        std::uint32_t result = kern::success;
        const auto address = xmem::receive_copy(process, token, result);
        return address ? *address : 0;
    }

    std::shared_ptr<MemoryEntry> memory_entry(Name name) {
        return std::dynamic_pointer_cast<MemoryEntry>(ipc->kernel_object(name));
    }
    bool is_bootstrap(Name name) { return name && std::dynamic_pointer_cast<Bootstrap>(ipc->kernel_object(name)) != nullptr; }

    std::string service_name(GuestMemory& m, Address address) { return read_cstring(m, address, 128); }

    // ---- task policy, thread policy, task_info -------------------------------------
    // <mach/task_policy.h>: what this task's policies were set to (another
    // process sets them through its task port). Scheduling is not changed: the
    // values are hints to the host scheduler that it does not take.
    struct TaskPolicies {
        bool role_set{}, suppression_set{}, base_qos_set{}, override_qos_set{};
        std::int32_t role{};                          // TASK_CATEGORY_POLICY
        std::array<std::uint32_t, 16> suppression{};   // TASK_SUPPRESSION_POLICY
        std::array<std::uint32_t, 2> base_qos{}, override_qos{};   // TASK_BASE_QOS_POLICY, TASK_OVERRIDE_QOS_POLICY
    };
    TaskPolicies task_policies;                       // under `mutex`
    // <mach/thread_policy.h>, per thread port: flavor -> the words set.
    std::map<Name, std::map<std::uint32_t, std::vector<std::uint32_t>>> thread_policies;   // under `mutex`

    static bool latency_tier(std::uint32_t value) { return value == 0 || (value >= 0xff0001 && value <= 0xff0006); }
    static bool throughput_tier(std::uint32_t value) { return value == 0 || (value >= 0xfe0001 && value <= 0xfe0006); }

    // task_policy_set on this task (host-only: no guest calls).
    std::uint32_t set_task_policy(std::uint32_t flavor, const std::vector<std::uint32_t>& words) {
        std::lock_guard lock(mutex);
        switch (flavor) {
        case 1: {   // TASK_CATEGORY_POLICY {task_role_t role}
            if (words.size() < 1) return kern::invalid_argument;
            const auto role = static_cast<std::int32_t>(words[0]);
            // FOREGROUND, BACKGROUND, THROTTLE, NONUI, DEFAULT, DARWINBG; the
            // control and graphics-server roles are not granted.
            if (role != 1 && role != 2 && (role < 5 || role > 8)) return kern::invalid_argument;
            task_policies.role = role;
            task_policies.role_set = true;
            return kern::success;
        }
        case 3:     // TASK_SUPPRESSION_POLICY (16 integers)
            if (words.size() < 16) return kern::invalid_argument;
            std::copy_n(words.begin(), 16, task_policies.suppression.begin());
            task_policies.suppression_set = true;
            return kern::success;
        case 8:     // TASK_BASE_QOS_POLICY {latency tier, throughput tier}
        case 9: {   // TASK_OVERRIDE_QOS_POLICY
            if (words.size() < 2 || !latency_tier(words[0]) || !throughput_tier(words[1])) return kern::invalid_argument;
            auto& slot = flavor == 8 ? task_policies.base_qos : task_policies.override_qos;
            slot = {words[0], words[1]};
            (flavor == 8 ? task_policies.base_qos_set : task_policies.override_qos_set) = true;
            return kern::success;
        }
        default:
            return kern::invalid_argument;
        }
    }
    // task_policy_get on this task: {result, words, default in effect}.
    std::tuple<std::uint32_t, std::vector<std::uint32_t>, bool> get_task_policy(std::uint32_t flavor, std::uint32_t capacity, bool want_default) {
        std::lock_guard lock(mutex);
        switch (flavor) {
        case 1: {
            if (capacity < 1) return {kern::invalid_argument, {}, false};
            const bool fallback = want_default || !task_policies.role_set;
            return {kern::success, {fallback ? 0U : static_cast<std::uint32_t>(task_policies.role)}, fallback};   // TASK_UNSPECIFIED
        }
        case 3: {
            if (capacity < 16) return {kern::invalid_argument, {}, false};
            const bool fallback = want_default || !task_policies.suppression_set;
            std::vector<std::uint32_t> words(16, 0);
            if (!fallback) std::copy(task_policies.suppression.begin(), task_policies.suppression.end(), words.begin());
            return {kern::success, std::move(words), fallback};
        }
        case 8:
        case 9: {
            if (capacity < 2) return {kern::invalid_argument, {}, false};
            const bool set = flavor == 8 ? task_policies.base_qos_set : task_policies.override_qos_set;
            const auto& slot = flavor == 8 ? task_policies.base_qos : task_policies.override_qos;
            const bool fallback = want_default || !set;
            return {kern::success, fallback ? std::vector<std::uint32_t>{0, 0} : std::vector<std::uint32_t>{slot[0], slot[1]}, fallback};
        }
        default:
            return {kern::invalid_argument, {}, false};
        }
    }

    // This process's CPU times and memory (as darwin_libsystem_extra's task_info).
    struct Usage { std::uint64_t user{}, system{}, resident{}, resident_peak{}, footprint{}, virtual_size{}; };
    static Usage local_usage() {
        Usage value;
        if (const auto usage = host::process::usage()) {
            value.user = usage->user_ns; value.system = usage->system_ns;
            value.resident = usage->resident; value.resident_peak = usage->resident_peak;
            value.footprint = usage->private_bytes; value.virtual_size = usage->virtual_size;
        }
        return value;
    }

    // Routines on this task called through another process's task port
    // (transport threads; host-only). {u8 op, ...}:
    //   1 task_policy_set {u32 flavor, u32 count, words} -> {u32 result}
    //   2 task_policy_get {u32 flavor, u32 capacity, u8 default} -> {u32 result, u8 default, u32 count, words}
    //   3 usage {} -> {u64 user, system, resident, resident peak, footprint, virtual size}
    std::vector<std::uint8_t> answer_task_call(const std::vector<std::uint8_t>& call) {
        Reader in{call};
        Writer out;
        switch (in.u8()) {
        case 1: {
            const auto flavor = in.u32(), count = in.u32();
            std::vector<std::uint32_t> words(std::min<std::uint32_t>(count, 64));
            for (auto& word : words) word = in.u32();
            out.u32(set_task_policy(flavor, words));
            break;
        }
        case 2: {
            const auto flavor = in.u32(), capacity = in.u32();
            const bool want_default = in.u8() != 0;
            const auto [result, words, fallback] = get_task_policy(flavor, capacity, want_default);
            out.u32(result);
            out.u8(fallback ? 1 : 0);
            out.u32(static_cast<std::uint32_t>(words.size()));
            for (const auto word : words) out.u32(word);
            break;
        }
        case 3: {
            const auto usage = local_usage();
            for (const auto value : {usage.user, usage.system, usage.resident, usage.resident_peak, usage.footprint, usage.virtual_size}) out.u64(value);
            break;
        }
        default:
            break;
        }
        return std::move(out.bytes);
    }

    // The process a task port names: 0 for this task, the pid of another
    // process's task port; otherwise the routine's error in `error`.
    std::optional<std::uint32_t> task_target(std::uint64_t task, std::uint64_t& error) {
        const auto name = static_cast<Name>(task);
        if (name == task_self) return 0U;
        if (const auto remote = std::dynamic_pointer_cast<RemoteTask>(ipc->kernel_object(name))) return remote->pid;
        error = name == 0 ? std::uint64_t{mr::send_invalid_dest} : std::uint64_t{kern::invalid_argument};
        return std::nullopt;
    }

    void install();
    void install_ports();
    void install_messages();
    void install_tasks();
    void install_task_policies();
    void install_bootstrap();
    void install_memory_entries();
};

void MachProvider::install() {
    MachIpc::Guest guest;
    std::weak_ptr<MachProvider> weak = std::static_pointer_cast<MachProvider>(shared_from_this());
    guest.allocate = [weak](std::uint64_t size) -> Address { const auto self = weak.lock(); return self ? self->vm_allocate(size) : 0; };
    guest.deallocate = [weak](Address address, std::uint64_t size) { if (const auto self = weak.lock()) self->vm_deallocate(address, size); };
    guest.kernel_name = [weak](Name name) { const auto self = weak.lock(); return self && self->kernel_name(name); };
    guest.interrupted = [weak]() {
        const auto self = weak.lock();
        if (!self || !self->process.requested_exit()) return false;
        if (guest_worker_must_stop(self->process)) throw GuestThreadStop{};
        return true;
    };
    guest.audit_token = [weak]() { const auto self = weak.lock(); return self ? self->audit_token() : std::array<std::uint32_t, 8>{}; };
    guest.task_self_name = task_self;
    guest.serialize_object = [weak](const std::shared_ptr<KernelObject>& object, std::uint32_t recipient) -> std::optional<std::vector<std::uint8_t>> {
        const auto self = weak.lock();
        return self ? self->serialize_object(object, recipient) : std::nullopt;
    };
    guest.deserialize_object = [weak](const std::vector<std::uint8_t>& data) -> std::shared_ptr<KernelObject> {
        const auto self = weak.lock();
        if (!self) { discard_object(data); return nullptr; }
        return self->deserialize_object(data);
    };
    guest.forward_object = &MachProvider::forward_object;
    guest.discard_object = &MachProvider::discard_object;
    guest.ool_section_minimum = 64 * 1024;   // smaller regions travel inline
    guest.export_ool = &MachProvider::export_ool;
    guest.receive_ool = [weak](const std::vector<std::uint8_t>& object, std::uint64_t size) -> Address {
        const auto self = weak.lock();
        if (!self) { discard_object(object); return 0; }
        return self->receive_ool(object, size);
    };
    guest.task_call = [weak](std::uint32_t, const std::vector<std::uint8_t>& call) -> std::vector<std::uint8_t> {
        const auto self = weak.lock();
        if (!self) return {};
        try { return self->answer_task_call(call); } catch (const std::exception&) { return {}; }
    };
    ipc = std::make_shared<MachIpc>(std::move(guest));
    set_mach_port_events(process, ipc);
    foreign_ports = std::make_shared<ForeignPorts>(ipc);
    set_mach_kernel_objects(process, foreign_ports);
    // Across processes: the bootstrap broker is the root Alchemy process of
    // the tree (ALCHEMY_MACH_BROKER, set here by the root, inherited by every
    // descendant through the host environment).
    transport = Transport::create(std::weak_ptr<Transport::Handler>(ipc));
    std::uint32_t broker = 0;
    if (const auto text = host::process::environment("ALCHEMY_MACH_BROKER"))
        broker = static_cast<std::uint32_t>(std::strtoul(text->c_str(), nullptr, 10));
    if (!broker) {
        broker = transport->self();
        static_cast<void>(host::process::set_environment("ALCHEMY_MACH_BROKER", std::to_string(broker)));
    }
    root = broker == transport->self();
    ipc->connect(transport, broker);
    process.at_teardown([transport = transport] { transport->stop(); });
    static_cast<void>(audit_token());   // computed on this (guest-calling) thread, reused by transport threads
    // posix_spawn: the special ports set with posix_spawnattr_setspecialport_np.
    process.spawn_starting = [weak](std::uint64_t) { if (const auto self = weak.lock()) self->ipc->spawn_starting(); };
    process.spawn_started = [weak](std::uint64_t attributes, std::uint32_t pid) {
        const auto self = weak.lock();
        if (!self || !pid) return;   // pid 0: the spawn failed
        std::vector<std::pair<std::uint32_t, Right>> specials;
        {
            std::lock_guard lock(self->mutex);
            if (const auto found = self->spawn_specials.find(attributes); found != self->spawn_specials.end())
                for (const auto& [which, right] : found->second) specials.emplace_back(which, self->ipc->copy_send(right));
        }
        self->ipc->spawn_started(pid, std::move(specials));
    };
    install_ports();
    install_messages();
    install_tasks();
    install_task_policies();
    install_bootstrap();
    install_memory_entries();
}

void MachProvider::install_ports() {
    // Each handler: (task, ...) -> kern_return_t; the task must be mach_task_self.
    const auto port_call = [this](const char* name, auto body) {
        raw(libsystem, name, [this, body](CpuState& s, GuestMemory& m) {
            if (const auto error = bad_task(s.x[0])) { s.x[0] = *error; return; }
            s.x[0] = body(s, m);
        });
    };
    port_call("_mach_port_allocate", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Name name{};
        const auto result = ipc->allocate(static_cast<std::uint32_t>(s.x[1]), name);
        if (s.x[2]) m.write32(s.x[2], name);
        return result;
    });
    port_call("_mach_port_allocate_name", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->allocate_name(static_cast<std::uint32_t>(s.x[1]), static_cast<Name>(s.x[2]));
    });
    // mach_port_construct(task, mach_port_options_t *options, mach_port_context_t context, mach_port_name_t *name)
    // options: {uint32_t flags; mach_port_limits_t mpl; union {uint64_t reserved[2]; ...}}
    port_call("_mach_port_construct", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1] || !s.x[3]) return kern::invalid_argument;
        MachIpc::Options options{m.read32(s.x[1]), m.read32(s.x[1] + 4)};
        Name name{};
        const auto result = ipc->construct(options, s.x[2], name);
        m.write32(s.x[3], name);
        return result;
    });
    port_call("_mach_port_destruct", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->destruct(static_cast<Name>(s.x[1]), static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[2])), s.x[3]);
    });
    // Replaces darwin_libsystem_extra's kernel-name-only mach_port_deallocate.
    static_cast<void>(process.install_function("_mach_port_deallocate", [owner = shared_from_this(), this](CpuState& s, GuestMemory&) {
        if (const auto error = bad_task(s.x[0])) { s.x[0] = *error; return; }
        s.x[0] = ipc->deallocate(static_cast<Name>(s.x[1]));
    }));
    port_call("_mach_port_destroy", [this](CpuState& s, GuestMemory&) -> std::uint64_t { return ipc->destroy(static_cast<Name>(s.x[1])); });
    port_call("_mach_port_mod_refs", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->mod_refs(static_cast<Name>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[3])));
    });
    port_call("_mach_port_get_refs", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::uint32_t refs{};
        const auto result = ipc->get_refs(static_cast<Name>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), refs);
        if (s.x[3]) m.write32(s.x[3], refs);
        return result;
    });
    port_call("_mach_port_type", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::uint32_t bits{};
        const auto result = ipc->type(static_cast<Name>(s.x[1]), bits);
        if (s.x[2]) m.write32(s.x[2], bits);
        return result;
    });
    port_call("_mach_port_insert_right", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->insert_right(static_cast<Name>(s.x[1]), static_cast<Name>(s.x[2]), static_cast<std::uint8_t>(s.x[3]));
    });
    // mach_port_extract_right(task, name, msgt_name, *poly, *polyPoly)
    port_call("_mach_port_extract_right", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Name name{};
        std::uint8_t kind{};
        const auto disposition = static_cast<std::uint32_t>(s.x[2]);
        const auto result = disposition > 0xff ? kern::invalid_value : ipc->extract_right(static_cast<Name>(s.x[1]), static_cast<std::uint8_t>(disposition), name, kind);
        if (s.x[3]) m.write32(s.x[3], name);
        if (s.x[4]) m.write32(s.x[4], kind);
        return result;
    });
    port_call("_mach_port_move_member", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->move_member(static_cast<Name>(s.x[1]), static_cast<Name>(s.x[2]));
    });
    port_call("_mach_port_insert_member", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->insert_member(static_cast<Name>(s.x[1]), static_cast<Name>(s.x[2]));
    });
    port_call("_mach_port_extract_member", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->extract_member(static_cast<Name>(s.x[1]), static_cast<Name>(s.x[2]));
    });
    // Out-of-line arrays come back in vm_allocate'd memory (the caller vm_deallocates them).
    const auto out_array = [this](GuestMemory& m, const std::vector<std::uint32_t>& words, Address pointer, Address count) -> bool {
        Address array = 0;
        if (!words.empty()) {
            array = vm_allocate(words.size() * 4ULL);
            if (!array) return false;
            for (std::size_t index = 0; index < words.size(); ++index) m.write32(array + index * 4, words[index]);
        }
        if (pointer) m.write64(pointer, array);
        if (count) m.write32(count, static_cast<std::uint32_t>(words.size()));
        return true;
    };
    port_call("_mach_port_get_set_status", [this, out_array](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::vector<Name> members;
        const auto result = ipc->set_status(static_cast<Name>(s.x[1]), members);
        if (result) return result;
        return out_array(m, members, s.x[2], s.x[3]) ? kern::success : kern::resource_shortage;
    });
    // mach_port_names(task, names**, namesCnt*, types**, typesCnt*)
    port_call("_mach_port_names", [this, out_array](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::vector<std::pair<Name, std::uint32_t>> list;
        static_cast<void>(ipc->names(list));
        std::vector<std::uint32_t> names, types;
        for (const auto& [name, bits] : list) { names.push_back(name); types.push_back(bits); }
        return out_array(m, names, s.x[1], s.x[2]) && out_array(m, types, s.x[3], s.x[4]) ? kern::success : kern::resource_shortage;
    });
    // mach_port_get_attributes(task, name, flavor, info*, count* (in: capacity in natural_t))
    port_call("_mach_port_get_attributes", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[3] || !s.x[4]) return kern::invalid_argument;
        std::vector<std::uint32_t> words;
        const auto result = ipc->get_attributes(static_cast<Name>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), m.read32(s.x[4]), words);
        if (result) return result;
        for (std::size_t index = 0; index < words.size(); ++index) m.write32(s.x[3] + index * 4, words[index]);
        m.write32(s.x[4], static_cast<std::uint32_t>(words.size()));
        return kern::success;
    });
    port_call("_mach_port_set_attributes", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto count = std::min<std::uint32_t>(static_cast<std::uint32_t>(s.x[4]), 64);
        std::vector<std::uint32_t> words;
        for (std::uint32_t index = 0; s.x[3] && index < count; ++index) words.push_back(m.read32(s.x[3] + index * 4ULL));
        return ipc->set_attributes(static_cast<Name>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), words);
    });
    // mach_port_request_notification(task, name, msgid, sync, notify, notifyPoly, *previous)
    port_call("_mach_port_request_notification", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Name previous{};
        const auto disposition = static_cast<std::uint32_t>(s.x[5]);
        const auto result = disposition > 0xff ? kern::invalid_value
            : ipc->request_notification(static_cast<Name>(s.x[1]), static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[2])),
                                        static_cast<std::uint32_t>(s.x[3]), static_cast<Name>(s.x[4]),
                                        static_cast<std::uint8_t>(disposition), previous);
        if (s.x[6]) m.write32(s.x[6], previous);
        return result;
    });
    port_call("_mach_port_guard", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->guard(static_cast<Name>(s.x[1]), s.x[2], (s.x[3] & 0xff) != 0);
    });
    // mach_port_guard_with_flags(task, name, guard, flags): MPG_STRICT = 1.
    port_call("_mach_port_guard_with_flags", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->guard(static_cast<Name>(s.x[1]), s.x[2], (s.x[3] & 1) != 0);
    });
    port_call("_mach_port_unguard", [this](CpuState& s, GuestMemory&) -> std::uint64_t { return ipc->unguard(static_cast<Name>(s.x[1]), s.x[2]); });
    port_call("_mach_port_swap_guard", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->swap_guard(static_cast<Name>(s.x[1]), s.x[2], s.x[3]);
    });
    port_call("_mach_port_get_context", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::uint64_t context{};
        const auto result = ipc->get_context(static_cast<Name>(s.x[1]), context);
        if (s.x[2]) m.write64(s.x[2], context);
        return result;
    });
    port_call("_mach_port_set_context", [this](CpuState& s, GuestMemory&) -> std::uint64_t { return ipc->set_context(static_cast<Name>(s.x[1]), s.x[2]); });
    port_call("_mach_port_set_mscount", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->set_mscount(static_cast<Name>(s.x[1]), static_cast<std::uint32_t>(s.x[2]));
    });
    port_call("_mach_port_set_seqno", [this](CpuState& s, GuestMemory&) -> std::uint64_t {
        return ipc->set_seqno(static_cast<Name>(s.x[1]), static_cast<std::uint32_t>(s.x[2]));
    });
    // mach_port_peek(task, name, trailer_type, *seqno, *msg_size, *msg_id, trailer_info, *trailer_infoCnt)
    port_call("_mach_port_peek", [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[3]) return kern::invalid_argument;
        std::uint32_t seqno = m.read32(s.x[3]);
        MachIpc::PeekResult peeked;
        const auto result = ipc->peek(static_cast<Name>(s.x[1]), seqno, peeked);
        if (result) return result;
        m.write32(s.x[3], seqno);
        if (s.x[4]) m.write32(s.x[4], peeked.size);
        if (s.x[5]) m.write32(s.x[5], static_cast<std::uint32_t>(peeked.id));
        if (s.x[7]) m.write32(s.x[7], 0);   // no trailer information is returned
        return kern::success;
    });
}

void MachProvider::install_messages() {
    // mach_msg(msg, option, send_size, rcv_size, rcv_name, timeout, notify)
    // mach_msg_overwrite(..., notify, rcv_msg, rcv_scatter_size): receives into rcv_msg when non-null.
    const auto transfer = [this](CpuState& s, GuestMemory& m, Address receive_buffer) -> std::uint64_t {
        const auto options = static_cast<std::uint32_t>(s.x[1]);
        const auto timeout = static_cast<std::uint32_t>(s.x[5]);
        if (options & option::send_msg) {
            const auto result = ipc->send(m, s.x[0], options, static_cast<std::uint32_t>(s.x[2]), timeout);
            if (result) return result;
        }
        if (options & option::rcv_msg)
            return ipc->receive(m, receive_buffer, options, static_cast<std::uint32_t>(s.x[3]), static_cast<Name>(s.x[4]), timeout);
        return kern::success;
    };
    raw(libsystem, "_mach_msg", [transfer](CpuState& s, GuestMemory& m) { s.x[0] = transfer(s, m, s.x[0]); });
    raw(libsystem, "_mach_msg_overwrite", [transfer](CpuState& s, GuestMemory& m) { s.x[0] = transfer(s, m, s.x[7] ? s.x[7] : s.x[0]); });
    // mach_msg_send(msg) = mach_msg(msg, MACH_SEND_MSG, msg->msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL)
    raw(libsystem, "_mach_msg_send", [this](CpuState& s, GuestMemory& m) {
        s.x[0] = ipc->send(m, s.x[0], option::send_msg, m.read32(s.x[0] + 4), 0);
    });
    // mach_msg_receive(msg) = mach_msg(msg, MACH_RCV_MSG, 0, msg->msgh_size, msg->msgh_local_port, 0, MACH_PORT_NULL)
    raw(libsystem, "_mach_msg_receive", [this](CpuState& s, GuestMemory& m) {
        s.x[0] = ipc->receive(m, s.x[0], option::rcv_msg, m.read32(s.x[0] + 4), m.read32(s.x[0] + 12), 0);
    });
    // voucher_mach_msg_set(msg): Alchemy has no vouchers, so none is attached
    // and the message is unchanged (false).
    raw(libsystem, "_voucher_mach_msg_set", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    raw(libsystem, "_mach_msg_destroy", [this](CpuState& s, GuestMemory& m) { ipc->destroy_message(m, s.x[0]); });

    // Reply ports: mach_reply_port makes a receive right; MIG's reply port is
    // one per thread, made on first use and replaced after mig_dealloc_reply_port.
    raw(libsystem, "_mach_reply_port", [this](CpuState& s, GuestMemory&) {
        Name name{};
        s.x[0] = ipc->allocate(right::receive, name) == kern::success ? name : 0;
    });
    raw(libsystem, "_mig_get_reply_port", [this](CpuState& s, GuestMemory&) {
        const auto thread = process.current_thread()->identity;
        std::lock_guard lock(mutex);
        auto& slot = reply_ports[thread];
        std::uint32_t refs{};
        if (!slot || ipc->get_refs(slot, right::receive, refs) != kern::success || !refs) {
            if (ipc->allocate(right::receive, slot) != kern::success) slot = 0;
        }
        s.x[0] = slot;
    });
    raw(libsystem, "_mig_put_reply_port", [](CpuState&, GuestMemory&) {});   // the port stays cached
    raw(libsystem, "_mig_dealloc_reply_port", [this](CpuState& s, GuestMemory&) {
        const auto thread = process.current_thread()->identity;
        Name name{};
        {
            std::lock_guard lock(mutex);
            const auto found = reply_ports.find(thread);
            if (found == reply_ports.end()) return;
            name = found->second;
            reply_ports.erase(found);
        }
        static_cast<void>(s);
        static_cast<void>(ipc->mod_refs(name, right::receive, -1));
    });
}

void MachProvider::install_tasks() {
    // Task special ports (<mach/task_special_ports.h>): TASK_KERNEL_PORT 1,
    // TASK_HOST_PORT 2, TASK_NAME_PORT 3 answer the kernel names; the others
    // (TASK_BOOTSTRAP_PORT 4, ...) hold send rights the task set.
    raw(libsystem, "_task_get_special_port", [this](CpuState& s, GuestMemory& m) {
        if (const auto error = bad_task(s.x[0])) { s.x[0] = *error; return; }
        const auto which = static_cast<std::uint32_t>(s.x[1]);
        if (!s.x[2] || which == 0 || which > 12) { s.x[0] = kern::invalid_argument; return; }
        Name name = 0;
        if (which == 1 || which == 3) name = task_self;
        else if (which == 2) name = host_self;
        else {
            // A child: the special ports its parent's posix_spawn attributes set.
            bool fetch = false;
            {
                std::lock_guard lock(mutex);
                fetch = !root && !inherited_specials;
                inherited_specials = true;
            }
            if (fetch) {
                auto inherited = ipc->inherited_special_ports(static_cast<std::uint32_t>(call(libsystem, "_getppid", {})));
                std::lock_guard lock(mutex);
                for (auto& [slot, right] : inherited) {
                    if (const auto previous = special_ports.find(slot); previous != special_ports.end()) ipc->drop(previous->second);
                    special_ports[slot] = std::move(right);
                }
            }
            Right held;
            {
                std::lock_guard lock(mutex);
                const auto found = special_ports.find(which);
                if (found != special_ports.end()) held = found->second;
            }
            if (held.port || held.dead) name = ipc->give(ipc->copy_send(held));
        }
        m.write32(s.x[2], name);
        s.x[0] = kern::success;
    });
    raw(libsystem, "_task_set_special_port", [this](CpuState& s, GuestMemory&) {
        if (const auto error = bad_task(s.x[0])) { s.x[0] = *error; return; }
        const auto which = static_cast<std::uint32_t>(s.x[1]);
        if (which <= 3 || which > 12) { s.x[0] = kern::invalid_argument; return; }
        Right right;
        if (const auto result = ipc->take(static_cast<Name>(s.x[2]), disp::copy_send, right)) { s.x[0] = result; return; }
        Right previous;
        {
            std::lock_guard lock(mutex);
            previous = std::exchange(special_ports[which], right);
        }
        ipc->drop(previous);
        s.x[0] = kern::success;
    });
    // task_threads(task, thread_act_array_t *list, mach_msg_type_number_t *count):
    // thread ports are the thread identities (mach_thread_self, pthread_mach_thread_np).
    raw(libsystem, "_task_threads", [this](CpuState& s, GuestMemory& m) {
        if (const auto error = bad_task(s.x[0])) { s.x[0] = *error; return; }
        if (!s.x[1] || !s.x[2]) { s.x[0] = kern::invalid_argument; return; }
        const auto threads = process.thread_identities();
        const auto array = vm_allocate(std::max<std::size_t>(threads.size(), 1) * 4);
        if (!array) { s.x[0] = kern::resource_shortage; return; }
        for (std::size_t index = 0; index < threads.size(); ++index) m.write32(array + index * 4, static_cast<std::uint32_t>(threads[index]));
        m.write64(s.x[1], array);
        m.write32(s.x[2], static_cast<std::uint32_t>(threads.size()));
        s.x[0] = kern::success;
    });
    // fileport_makeport(fd, fileport_t *port): a send right naming the open
    // file (sockets cannot be wrapped); fileport_makefd(port): a new
    // descriptor for it (O_CLOEXEC is not set, as on Darwin).
    raw(libsystem, "_fileport_makeport", [this](CpuState& s, GuestMemory& m) {
        const auto description = process.export_descriptor
            ? process.export_descriptor(static_cast<int>(static_cast<std::int32_t>(s.x[0])), 0) : std::nullopt;
        // A socket's description is a blob for this process: opened at once into the fileport's own socket.
        auto fileport = !description ? nullptr : description->kind == 5 ? socket_fileport(*description) : std::make_shared<Fileport>();
        if (!fileport) {
            if (s.x[1]) m.write32(s.x[1], 0);
            set_errno(9);   // EBADF
            s.x[0] = gui::fill::status(-1);
            return;
        }
        if (description->kind != 5) fileport->descriptor = *description;
        if (s.x[1]) m.write32(s.x[1], ipc->make_kernel_port(std::move(fileport)));
        s.x[0] = 0;
    });
    raw(libsystem, "_fileport_makefd", [this](CpuState& s, GuestMemory&) {
        const auto fileport = std::dynamic_pointer_cast<Fileport>(ipc->kernel_object(static_cast<Name>(s.x[0])));
        if (!fileport || !process.import_descriptor) { set_errno(22); s.x[0] = gui::fill::status(-1); return; }   // EINVAL
        PortableDescriptor copy = fileport->descriptor;
        if (copy.kind == 1) {
            const auto duplicate = host::duplicate(host::Handle{static_cast<std::uintptr_t>(copy.handle)});
            if (!duplicate) { set_errno(24); s.x[0] = gui::fill::status(-1); return; }   // EMFILE
            copy.handle = duplicate->value;
        } else if (copy.kind == 5) {
            auto made = fileport->socket_for(0);
            if (!made) { set_errno(24); s.x[0] = gui::fill::status(-1); return; }   // EMFILE
            copy = std::move(*made);
        }
        const auto fd = process.import_descriptor(copy);
        if (fd < 0) { set_errno(24); s.x[0] = gui::fill::status(-1); return; }
        s.x[0] = static_cast<std::uint64_t>(fd);
    });
    // posix_spawnattr_setspecialport_np(attr *, port, which): the child's task
    // special port `which` (task_get_special_port in the child).
    raw(libsystem, "_posix_spawnattr_setspecialport_np", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = 22; return; }   // EINVAL
        const auto token = m.read64(s.x[0]);
        const auto which = static_cast<std::uint32_t>(s.x[2]);
        if (!token || which == 0 || which > 12) { s.x[0] = 22; return; }
        Right right;
        if (ipc->take(static_cast<Name>(s.x[1]), disp::copy_send, right) != kern::success) { s.x[0] = 22; return; }
        std::lock_guard lock(mutex);
        spawn_specials[token].emplace_back(which, std::move(right));
        s.x[0] = 0;
    });
    // pid_for_task(task, *pid): this task, or a task port from another process.
    static_cast<void>(process.install_function("_pid_for_task", [owner = shared_from_this(), this](CpuState& s, GuestMemory& m) {
        std::uint32_t pid = 0;
        if (static_cast<std::uint32_t>(s.x[0]) == task_self) pid = static_cast<std::uint32_t>(call(libsystem, "_getpid", {}));
        else if (const auto task = std::dynamic_pointer_cast<RemoteTask>(ipc->kernel_object(static_cast<Name>(s.x[0])))) pid = task->pid;
        else { s.x[0] = kern::failure; return; }
        if (s.x[1]) m.write32(s.x[1], pid);
        s.x[0] = kern::success;
    }));
    // Identity tokens: a send right naming this task.
    raw(libsystem, "_task_create_identity_token", [this](CpuState& s, GuestMemory& m) {
        if (const auto error = bad_task(s.x[0])) { s.x[0] = *error; return; }
        if (!s.x[1]) { s.x[0] = kern::invalid_argument; return; }
        m.write32(s.x[1], ipc->make_kernel_port(std::make_shared<IdentityToken>()));
        s.x[0] = kern::success;
    });
    // task_identity_token_get_task_port(token, flavor, *port): every flavor
    // (CONTROL 0, READ 1, INSPECT 2, NAME 3) answers mach_task_self.
    raw(libsystem, "_task_identity_token_get_task_port", [this](CpuState& s, GuestMemory& m) {
        if (!std::dynamic_pointer_cast<IdentityToken>(ipc->kernel_object(static_cast<Name>(s.x[0]))) ||
            static_cast<std::uint32_t>(s.x[1]) > 3 || !s.x[2]) {
            s.x[0] = kern::invalid_argument;
            return;
        }
        m.write32(s.x[2], task_self);
        s.x[0] = kern::success;
    });
}

void MachProvider::install_bootstrap() {
    // The task's bootstrap port: a kernel port served in-process. Services
    // checked in by this process are found by look-ups in this process.
    const auto name = ipc->make_kernel_port(std::make_shared<Bootstrap>());
    static_cast<void>(ipc->take(name, disp::copy_send, bootstrap));
    {
        std::lock_guard lock(mutex);
        special_ports[4] = ipc->copy_send(bootstrap);
    }
    std::array<std::uint8_t, 4> value{};
    std::memcpy(value.data(), &name, 4);
    data(libsystem, "_bootstrap_port", value, 4);
    // bootstrap_check_in(bp, service_name, *service_port): the receive right
    // of a new service in the process tree's registry (the root Alchemy
    // process); an active service of that name is refused.
    raw(libsystem, "_bootstrap_check_in", [this](CpuState& s, GuestMemory& m) {
        if (!is_bootstrap(static_cast<Name>(s.x[0]))) { s.x[0] = mr::send_invalid_dest; return; }
        if (!s.x[1] || !s.x[2]) { s.x[0] = kern::invalid_argument; return; }
        Name receive{};
        const auto status = ipc->bootstrap_check_in(service_name(m, s.x[1]), receive);
        m.write32(s.x[2], receive);
        s.x[0] = status;
    });
    // bootstrap_look_up(bp, service_name, *service_port): a send right to a
    // service checked in anywhere in the process tree.
    raw(libsystem, "_bootstrap_look_up", [this](CpuState& s, GuestMemory& m) {
        if (!is_bootstrap(static_cast<Name>(s.x[0]))) { s.x[0] = mr::send_invalid_dest; return; }
        if (!s.x[1] || !s.x[2]) { s.x[0] = kern::invalid_argument; return; }
        Name send{};
        const auto status = ipc->bootstrap_look_up(service_name(m, s.x[1]), send);
        m.write32(s.x[2], send);
        s.x[0] = status;
    });
    // bootstrap_register(bp, service_name, service_port) (deprecated): a send
    // right registered under a name.
    raw(libsystem, "_bootstrap_register", [this](CpuState& s, GuestMemory& m) {
        if (!is_bootstrap(static_cast<Name>(s.x[0]))) { s.x[0] = mr::send_invalid_dest; return; }
        if (!s.x[1]) { s.x[0] = kern::invalid_argument; return; }
        s.x[0] = ipc->bootstrap_register(service_name(m, s.x[1]), static_cast<Name>(s.x[2]));
    });
    // bootstrap_parent(bp, *parent): launchd answers only privileged callers.
    raw(libsystem, "_bootstrap_parent", [this](CpuState& s, GuestMemory& m) {
        if (!is_bootstrap(static_cast<Name>(s.x[0]))) { s.x[0] = mr::send_invalid_dest; return; }
        if (s.x[1]) m.write32(s.x[1], 0);
        s.x[0] = bootstrap_not_privileged;
    });
}

void MachProvider::install_memory_entries() {
    // mach_make_memory_entry_64(task, memory_object_size_t *size, memory_object_offset_t offset,
    //     vm_prot_t permission, mem_entry_name_port_t *object_handle, mem_entry_name_port_t parent)
    const auto make_entry = [this](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (const auto error = bad_task(s.x[0])) return *error;
        if (!s.x[1] || !s.x[4]) return kern::invalid_argument;
        const auto permission = static_cast<std::uint32_t>(s.x[3]);
        const auto protection = permission & (vm_prot_read | vm_prot_write | vm_prot_execute);
        const auto flags = permission & 0x00ffff00U;   // the top byte selects a cache mode
        constexpr auto known = map_mem_named_create | map_mem_purgable | map_mem_named_reuse | map_mem_vm_copy | map_mem_vm_share;
        if (flags & ~known) return kern::not_supported;   // MAP_MEM_ONLY, USE_DATA_ADDR, ledger tags, ...
        const auto size = m.read64(s.x[1]);
        const auto offset = s.x[2];
        const auto parent = static_cast<Name>(s.x[5]);
        auto entry = std::make_shared<MemoryEntry>();
        entry->protection = protection;
        auto& engine = process.calling_engine();
        const auto release = [weak = std::weak_ptr<Provider>(shared_from_this())](Address anchor, std::uint64_t length) {
            if (const auto self = std::static_pointer_cast<MachProvider>(weak.lock())) self->vm_deallocate(anchor, length);
        };
        if (flags & map_mem_named_create) {
            // A new object of `size` bytes (MAP_MEM_PURGABLE objects are
            // created non-volatile).
            if (parent || !size || size > (1ULL << 36)) return kern::invalid_argument;
            const auto length = (size + page_mask) & ~page_mask;
            const auto anchor = vm_allocate(length);
            if (!anchor) return kern::resource_shortage;
            if (!engine_is_shared_memory(engine, anchor, length)) {
                vm_deallocate(anchor, length);
                return kern::not_supported;   // the engine has no shared guest memory
            }
            entry->backing = std::make_shared<Backing>(anchor, length, release);
            entry->size = length;
        } else if (parent) {
            // A sub-range of the parent entry with at most its protection.
            const auto source = memory_entry(parent);
            if (!source) return kern::invalid_argument;
            const auto begin = offset & ~page_mask;
            const auto end = (offset + size + page_mask) & ~page_mask;
            if (!size || end > source->size || end < begin) return kern::invalid_argument;
            if (protection & ~source->protection) return kern::protection_failure;
            entry->backing = source->backing;
            entry->offset = source->offset + begin;
            entry->size = end - begin;
        } else {
            // The pages at `offset` in this task: shared with them
            // (MAP_MEM_VM_SHARE and the default), or a snapshot (MAP_MEM_VM_COPY).
            const auto begin = offset & ~page_mask;
            const auto end = (offset + size + page_mask) & ~page_mask;
            if (!size || end <= begin) return kern::invalid_argument;
            const auto length = end - begin;
            for (auto address = begin; address < end; address += page_size) {
                const auto region = m.query_region(address);
                if (!region || !region->readable) return kern::invalid_address;
                if ((protection & vm_prot_write) && !region->writable) return kern::protection_failure;
            }
            if (flags & map_mem_vm_copy) {
                const auto anchor = vm_allocate(length);
                if (!anchor) return kern::resource_shortage;
                if (!engine_is_shared_memory(engine, anchor, length)) {
                    vm_deallocate(anchor, length);
                    return kern::not_supported;
                }
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
                m.read_bytes(begin, bytes);
                m.write_bytes(anchor, bytes);
                entry->backing = std::make_shared<Backing>(anchor, length, release);
            } else {
                // Private memory (heap, image data, private mmap) cannot be
                // shared on Windows after the fact.
                if (!engine_is_shared_memory(engine, begin, length)) return kern::not_supported;
                Address anchor = 0;
                if (const auto result = vm_remap(anchor, length, vm_flags_anywhere, begin)) return result;
                entry->backing = std::make_shared<Backing>(anchor, length, release);
            }
            entry->size = length;
        }
        m.write64(s.x[1], entry->size);
        m.write32(s.x[4], ipc->make_kernel_port(entry));
        return kern::success;
    };
    raw(libsystem, "_mach_make_memory_entry_64", [make_entry](CpuState& s, GuestMemory& m) { s.x[0] = make_entry(s, m); });
    // mach_make_memory_entry has the same arm64 ABI (vm_size_t is 64-bit).
    raw(libsystem, "_mach_make_memory_entry", [make_entry](CpuState& s, GuestMemory& m) { s.x[0] = make_entry(s, m); });

    // mach_vm_map(task, *address, size, mask, flags, object, offset, copy, cur, max, inherit):
    // cur, max and inherit are packed 4-byte stack arguments (Apple arm64).
    raw(libsystem, "_mach_vm_map", [this](CpuState& s, GuestMemory& m) {
        const auto object = static_cast<Name>(s.x[5]);
        if (object == port_null) {
            // Anonymous memory: vm_map's implementation (the export-fill kit's).
            CallRegs registers{};
            for (unsigned index = 0; index < 8; ++index) registers.x[index] = s.x[index];
            registers.push_stack(m.read32(s.sp));
            registers.push_stack(m.read32(s.sp + 4));
            registers.push_stack(m.read32(s.sp + 8));
            s.x[0] = call_regs(libsystem, "_vm_map", registers).x0;
            return;
        }
        if (const auto error = bad_task(s.x[0])) { s.x[0] = *error; return; }
        const auto entry = memory_entry(object);
        const auto size = s.x[2], mask = s.x[3], offset = s.x[6];
        const auto flags = static_cast<std::uint32_t>(s.x[4]) & 0x00ffffffU;   // the top byte is a VM tag
        const bool copy = (s.x[7] & 0xff) != 0;
        const auto current = m.read32(s.sp) & 7U;
        auto maximum = m.read32(s.sp + 4) & 7U;
        if (!entry || !s.x[1]) { s.x[0] = kern::invalid_argument; return; }
        if (flags & ~(vm_flags_anywhere | vm_flags_overwrite)) { s.x[0] = kern::invalid_argument; return; }
        if (current & ~entry->protection) { s.x[0] = kern::invalid_right; return; }
        maximum &= entry->protection;
        const auto length = (size + page_mask) & ~page_mask;
        if (!size || (offset & page_mask) || offset > entry->size || length > entry->size - offset) { s.x[0] = kern::invalid_argument; return; }
        const auto source = entry->backing->anchor + entry->offset + offset;
        Address target = m.read64(s.x[1]);
        auto remap_flags = flags;
        if ((flags & vm_flags_anywhere) && mask > page_mask) {
            // An alignment wider than a page: find aligned free space.
            const auto alignment = mask + 1;
            auto& engine = process.calling_engine();
            Address candidate = (0x900000000ULL + mask) & ~mask;
            for (int attempt = 0; attempt < 65536 && !engine.is_range_available(candidate, length); ++attempt) candidate += alignment;
            if (!engine.is_range_available(candidate, length)) { s.x[0] = kern::no_space; return; }
            target = candidate;
            remap_flags = 0;
        }
        std::uint32_t result;
        if (copy) {
            // A private copy of the entry's current contents.
            target = remap_flags & vm_flags_anywhere ? 0 : target;
            const auto slot = process.allocate_guest(8);
            m.write64(slot, target);
            result = static_cast<std::uint32_t>(call(libsystem, "_vm_allocate", {task_self, slot, length, remap_flags & vm_flags_anywhere}));
            target = m.read64(slot);
            process.release_guest(slot);
            if (result == kern::success) {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
                m.read_bytes(source, bytes);
                m.write_bytes(target, bytes);
            }
        } else {
            result = vm_remap(target, length, remap_flags, source);
        }
        if (result) { s.x[0] = result; return; }
        if (current != (vm_prot_read | vm_prot_write)) result = vm_protect(target, length, false, current);
        if (!result && (maximum & (vm_prot_write | vm_prot_execute)) != (vm_prot_write | vm_prot_execute))
            result = vm_protect(target, length, true, maximum);
        if (result) {
            vm_deallocate(target, length);
            s.x[0] = result;
            return;
        }
        m.write64(s.x[1], target);
        s.x[0] = kern::success;
    });

    // vm_map has mach_vm_map's arm64 ABI (vm_address_t, vm_size_t and
    // vm_offset_t are 64-bit), and Chrome's shared memory regions call it on
    // memory entries. A memory object maps through mach_vm_map above;
    // anonymous memory goes to libsystem_misc.cpp's internal vm_map, which
    // mach_vm_map also uses (through this export).
    if (const auto anonymous = process.resolve("alchemy.internal", "__alchemy_vm_map_anonymous")) {
        const auto previous = anonymous->address;
        bridge->function(libsystem, "_vm_map", [owner = shared_from_this(), previous](CpuState& s, GuestMemory& m) {
            CallRegs registers{};
            for (unsigned index = 0; index < 8; ++index) registers.x[index] = s.x[index];
            registers.push_stack(m.read32(s.sp));
            registers.push_stack(m.read32(s.sp + 4));
            registers.push_stack(m.read32(s.sp + 8));
            const auto target = static_cast<Name>(s.x[5]) == port_null ? previous : owner->symbol(libsystem, "_mach_vm_map");
            s.x[0] = EngineAdapter(owner->process.calling_engine()).call_regs(target, registers).x0;
        });
    }
}

void MachProvider::install_task_policies() {
    // task_policy_set(task, flavor, policy_info, count) and
    // task_policy_get(task, flavor, policy_info, *count, *get_default): this
    // task's policies, or another process's through its task port.
    static_cast<void>(process.install_function("_task_policy_set", [owner = shared_from_this(), this](CpuState& s, GuestMemory& m) {
        std::uint64_t error = 0;
        const auto target = task_target(s.x[0], error);
        if (!target) { s.x[0] = error; return; }
        const auto flavor = static_cast<std::uint32_t>(s.x[1]);
        const auto count = std::min<std::uint32_t>(static_cast<std::uint32_t>(s.x[3]), 64);
        if (count && !s.x[2]) { s.x[0] = kern::invalid_argument; return; }
        std::vector<std::uint32_t> words(count);
        for (std::uint32_t index = 0; index < count; ++index) words[index] = m.read32(s.x[2] + index * 4ULL);
        if (!*target) { s.x[0] = set_task_policy(flavor, words); return; }
        Writer call;
        call.u8(1);
        call.u32(flavor);
        call.u32(count);
        for (const auto word : words) call.u32(word);
        const auto answer = ipc->task_call(*target, call.bytes);
        if (!answer || answer->size() < 4) { s.x[0] = kern::invalid_argument; return; }   // the task is gone
        Reader in{*answer};
        s.x[0] = in.u32();
    }));
    static_cast<void>(process.install_function("_task_policy_get", [owner = shared_from_this(), this](CpuState& s, GuestMemory& m) {
        std::uint64_t error = 0;
        const auto target = task_target(s.x[0], error);
        if (!target) { s.x[0] = error; return; }
        if (!s.x[2] || !s.x[3]) { s.x[0] = kern::invalid_argument; return; }
        const auto flavor = static_cast<std::uint32_t>(s.x[1]);
        const auto capacity = m.read32(s.x[3]);
        const bool want_default = s.x[4] && m.read32(s.x[4]) != 0;
        std::uint32_t result = kern::invalid_argument;
        std::vector<std::uint32_t> words;
        bool fallback = false;
        if (!*target) {
            std::tie(result, words, fallback) = get_task_policy(flavor, capacity, want_default);
        } else {
            Writer call;
            call.u8(2);
            call.u32(flavor);
            call.u32(capacity);
            call.u8(want_default ? 1 : 0);
            const auto answer = ipc->task_call(*target, call.bytes);
            if (answer && answer->size() >= 9) {
                Reader in{*answer};
                result = in.u32();
                fallback = in.u8() != 0;
                words.resize(std::min<std::uint32_t>(in.u32(), capacity));
                for (auto& word : words) word = in.u32();
            }
        }
        if (result == kern::success) {
            for (std::size_t index = 0; index < words.size(); ++index) m.write32(s.x[2] + index * 4, words[index]);
            m.write32(s.x[3], static_cast<std::uint32_t>(words.size()));
            if (s.x[4]) m.write32(s.x[4], fallback ? 1U : 0U);
        }
        s.x[0] = result;
    }));

    // task_info(task, flavor, info, *count) for this task and for another
    // process's task port (its own figures, asked over the Mach transport):
    // MACH_TASK_BASIC_INFO 20, TASK_BASIC_INFO_64 5, TASK_THREAD_TIMES_INFO 3,
    // TASK_VM_INFO 22 (revisions 0-2) and TASK_POWER_INFO 21. CPU times are
    // the host process's; wakeup counters are 0 (the host keeps none).
    static_cast<void>(process.install_function("_task_info", [owner = shared_from_this(), this](CpuState& s, GuestMemory& m) {
        std::uint64_t error = 0;
        const auto target = task_target(s.x[0], error);
        if (!target) { s.x[0] = error; return; }
        if (!s.x[2] || !s.x[3]) { s.x[0] = kern::invalid_argument; return; }
        const auto flavor = static_cast<std::uint32_t>(s.x[1]), capacity = m.read32(s.x[3]);
        const auto output = s.x[2];
        Usage usage;
        if (!*target) usage = local_usage();
        else {
            Writer call;
            call.u8(3);
            const auto answer = ipc->task_call(*target, call.bytes);
            if (!answer || answer->size() < 48) { s.x[0] = kern::invalid_argument; return; }   // the task is gone
            Reader in{*answer};
            usage.user = in.u64(); usage.system = in.u64(); usage.resident = in.u64();
            usage.resident_peak = in.u64(); usage.footprint = in.u64(); usage.virtual_size = in.u64();
        }
        const auto seconds = [&](Address address, std::uint64_t nanos) {   // time_value_t
            m.write32(address, static_cast<std::uint32_t>(nanos / 1'000'000'000));
            m.write32(address + 4, static_cast<std::uint32_t>(nanos / 1000 % 1'000'000));
        };
        std::uint32_t count = 0;
        switch (flavor) {
        case 20:   // MACH_TASK_BASIC_INFO
            count = 12;
            if (capacity < count) { s.x[0] = kern::invalid_argument; return; }
            m.write64(output, usage.virtual_size); m.write64(output + 8, usage.resident); m.write64(output + 16, usage.resident_peak);
            seconds(output + 24, usage.user); seconds(output + 32, usage.system);
            m.write32(output + 40, 1);   // POLICY_TIMESHARE
            m.write32(output + 44, 0);   // suspend count
            break;
        case 5:    // TASK_BASIC_INFO_64
            count = 10;
            if (capacity < count) { s.x[0] = kern::invalid_argument; return; }
            m.write32(output, 0); m.write64(output + 4, usage.virtual_size); m.write64(output + 12, usage.resident);
            seconds(output + 20, usage.user); seconds(output + 28, usage.system); m.write32(output + 36, 1);
            break;
        case 3:    // TASK_THREAD_TIMES_INFO
            count = 4;
            if (capacity < count) { s.x[0] = kern::invalid_argument; return; }
            seconds(output, usage.user); seconds(output + 8, usage.system);
            break;
        case 22:   // TASK_VM_INFO, revisions 0 through 2
            if (capacity < 36) { s.x[0] = kern::invalid_argument; return; }
            count = std::min<std::uint32_t>(capacity, 42);
            m.fill_bytes(output, 0, count * 4ULL);
            m.write64(output, usage.virtual_size); m.write32(output + 12, static_cast<std::uint32_t>(page_size));
            m.write64(output + 16, usage.resident); m.write64(output + 24, usage.resident_peak);
            m.write64(output + 48, usage.footprint); m.write64(output + 56, usage.footprint);
            if (count >= 38) m.write64(output + 144, usage.footprint);   // phys_footprint
            if (count >= 42) { m.write64(output + 152, 0x100000000ULL); m.write64(output + 160, 0x7ffffffff000ULL); }
            break;
        case 21:   // TASK_POWER_INFO {total_user, total_system (mach time = ns), four wakeup counters}
            count = 12;
            if (capacity < count) { s.x[0] = kern::invalid_argument; return; }
            m.fill_bytes(output, 0, count * 4ULL);
            m.write64(output, usage.user);
            m.write64(output + 8, usage.system);
            break;
        default:
            s.x[0] = kern::invalid_argument;
            return;
        }
        m.write32(s.x[3], count);
        s.x[0] = kern::success;
    }));

    // thread_policy_set(thread, flavor, policy_info, count) and
    // thread_policy_get(thread, flavor, policy_info, *count, *get_default) on
    // this task's threads: STANDARD 0, EXTENDED 1, TIME_CONSTRAINT 2,
    // PRECEDENCE 3, BACKGROUND 5, LATENCY_QOS 7, THROUGHPUT_QOS 8 are stored
    // and read back (scheduling hints the host does not take); AFFINITY 4 is
    // not supported (as on arm64 macOS).
    const auto thread_known = [this](Name thread) {
        const auto threads = process.thread_identities();
        return std::find(threads.begin(), threads.end(), std::uint64_t{thread}) != threads.end();
    };
    const auto policy_words = [](std::uint32_t flavor) -> std::uint32_t {
        switch (flavor) {
        case 0: return 0;
        case 1: case 3: case 5: case 7: case 8: return 1;
        case 2: return 4;
        default: return 0xffffffffU;
        }
    };
    static_cast<void>(process.install_function("_thread_policy_set", [owner = shared_from_this(), this, thread_known, policy_words](CpuState& s, GuestMemory& m) {
        const auto thread = static_cast<Name>(s.x[0]);
        const auto flavor = static_cast<std::uint32_t>(s.x[1]), count = static_cast<std::uint32_t>(s.x[3]);
        if (!thread) { s.x[0] = mr::send_invalid_dest; return; }
        if (!thread_known(thread)) { s.x[0] = kern::invalid_argument; return; }
        if (flavor == 4) { s.x[0] = kern::not_supported; return; }
        const auto needed = policy_words(flavor);
        if (needed == 0xffffffffU || count < needed || (needed && !s.x[2])) { s.x[0] = kern::invalid_argument; return; }
        std::vector<std::uint32_t> words(needed);
        for (std::uint32_t index = 0; index < needed; ++index) words[index] = m.read32(s.x[2] + index * 4ULL);
        if (flavor == 2 && words[2] < words[1]) { s.x[0] = kern::invalid_argument; return; }   // constraint >= computation
        if (flavor == 5 && words[0] != 0 && words[0] != 0x1000) { s.x[0] = kern::invalid_argument; return; }   // THREAD_BACKGROUND_POLICY_DARWIN_BG
        if ((flavor == 7 && !latency_tier(words[0])) || (flavor == 8 && !throughput_tier(words[0]))) { s.x[0] = kern::invalid_argument; return; }
        std::lock_guard lock(mutex);
        auto& policies = thread_policies[thread];
        if (flavor == 0) policies.clear();   // back to the standard policy
        else {
            if (flavor == 1) policies.erase(2);   // timeshare and real time exclude each other
            if (flavor == 2) policies.erase(1);
            policies[flavor] = std::move(words);
        }
        s.x[0] = kern::success;
    }));
    static_cast<void>(process.install_function("_thread_policy_get", [owner = shared_from_this(), this, thread_known, policy_words](CpuState& s, GuestMemory& m) {
        const auto thread = static_cast<Name>(s.x[0]);
        const auto flavor = static_cast<std::uint32_t>(s.x[1]);
        if (!thread) { s.x[0] = mr::send_invalid_dest; return; }
        if (!thread_known(thread)) { s.x[0] = kern::invalid_argument; return; }
        if (flavor == 4) { s.x[0] = kern::not_supported; return; }
        const auto needed = policy_words(flavor);
        if (needed == 0xffffffffU || !s.x[3] || m.read32(s.x[3]) < needed || (needed && !s.x[2])) { s.x[0] = kern::invalid_argument; return; }
        const bool want_default = s.x[4] && m.read32(s.x[4]) != 0;
        // Defaults: timeshare TRUE; time constraint {0, 0, 0, preemptible}; the others 0.
        std::vector<std::uint32_t> words = flavor == 1 ? std::vector<std::uint32_t>{1}
                                         : flavor == 2 ? std::vector<std::uint32_t>{0, 0, 0, 1}
                                                       : std::vector<std::uint32_t>(needed, 0);
        bool fallback = true;
        if (!want_default) {
            std::lock_guard lock(mutex);
            if (const auto found = thread_policies.find(thread); found != thread_policies.end()) {
                if (const auto policy = found->second.find(flavor); policy != found->second.end()) {
                    words = policy->second;
                    fallback = false;
                } else if (flavor == 1 && found->second.contains(2)) {
                    words = {0};   // real time: not timeshare
                    fallback = false;
                }
            }
        }
        for (std::size_t index = 0; index < words.size(); ++index) m.write32(s.x[2] + index * 4, words[index]);
        m.write32(s.x[3], static_cast<std::uint32_t>(words.size()));
        if (s.x[4]) m.write32(s.x[4], fallback ? 1U : 0U);
        s.x[0] = kern::success;
    }));
}

} // namespace

void install_mach_ipc(gui::InstallContext& context) {
    const auto provider = std::make_shared<MachProvider>(context);
    provider->install();
}

} // namespace alchemy::libsystem
