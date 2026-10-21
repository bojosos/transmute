#include "alchemy/engine_adapter.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/object_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

// libSystem calls a license service, an asset-browser service and a login
// token library import beyond the ones in the other export-fill files:
//   ptrace            PT_DENY_ATTACH succeeds (no Darwin debugger can attach to
//                     an Alchemy process); every other request is ENOTSUP.
//   xpc_date_create_from_current / xpc_date_create / xpc_date_get_value:
//                     XPC date objects (class OS_xpc_date).
//   __getdirentries64 directory entries of an open directory descriptor in the
//                     kernel's struct direntry layout.
//   fsctl             ENOTTY: no file-system control requests exist.
//   mach_vm_region    VM_REGION_BASIC_INFO(_64) for the guest region at or
//                     above an address (mapped regions, loaded images).
//   proc_regionfilename  the image file mapped at an address.
namespace alchemy::export_fill {
namespace {

constexpr std::uint64_t kern_success = 0, kern_invalid_address = 1, kern_invalid_argument = 4;

struct XpcDate : ObjectPayload { std::int64_t nanoseconds{}; };

std::int64_t unix_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::uint64_t name_inode(const std::string& directory, const std::string& name) {
    std::uint64_t hash = 1469598103934665603ULL;               // FNV-1a over "<dir>/<name>"
    for (const auto c : directory + "/" + name) { hash ^= static_cast<std::uint8_t>(c); hash *= 1099511628211ULL; }
    return hash ? hash : 1;
}

// One open directory stream of __getdirentries64: the listing taken at the
// first call and the position reached.
struct DirectoryStream { std::string path; std::vector<std::pair<std::string, std::uint8_t>> entries; std::size_t next{}; };

} // namespace

void install_libsystem_services(Provider& p, ObjcRuntime& objc) {
    auto& process = p.process();

    // int ptrace(int request, pid_t, caddr_t, int)
    p.libsystem("_ptrace", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (static_cast<std::int32_t>(s.x[0]) == 31) return 0; // PT_DENY_ATTACH
        throw Errno{enotsup};
    });

    // int fsctl(const char* path, unsigned long request, void* data, unsigned int options)
    p.libsystem("_fsctl", [&process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::error_code error;
        if (!std::filesystem::exists(process.map_guest_path(guest_string(m, s.x[0], 1024)), error)) throw Errno{enoent};
        throw Errno{enotty};
    });

    // XPC connections and dictionaries: explicit stops. No launchd exists, so
    // no Mach service can be looked up, and nothing here builds XPC messages
    // yet. (Squirrel.framework imports them for its ShipIt installer, which
    // only runs to apply an update.)
    for (const auto* name : {"_xpc_connection_create_mach_service", "_xpc_connection_set_event_handler",
                             "_xpc_connection_resume", "_xpc_connection_send_message", "_xpc_connection_send_barrier",
                             "_xpc_dictionary_create"}) {
        if (process.resolve(libsystem_library, name)) continue;   // a real or placeholder owner exists
        p.libsystem(name, [name](CpuState&, GuestMemory&) -> std::uint64_t {
            throw std::runtime_error(std::string("Alchemy: ") + (name + 1) + " is unsupported (no launchd: XPC services cannot be reached)");
        });
        mark_placeholder_export(process, libsystem_library, name);
    }

    // XPC dates.
    auto date_class = objc.find_class("OS_xpc_date");
    if (!date_class) date_class = objc.register_native_class("OS_xpc_date", objc.find_class("NSObject"), 16);
    const auto make_date = [&process, date_class](std::int64_t value) {
        auto payload = std::make_shared<XpcDate>();
        payload->nanoseconds = value;
        return process.objects().create(16, date_class, ObjectKind::instance, std::move(payload));
    };
    p.libsystem("_xpc_date_create_from_current", [make_date](CpuState&, GuestMemory&) -> std::uint64_t {
        return make_date(unix_nanoseconds());
    }, 0);
    p.libsystem("_xpc_date_create", [make_date](CpuState& s, GuestMemory&) -> std::uint64_t {
        return make_date(static_cast<std::int64_t>(s.x[0]));
    }, 0);
    p.libsystem("_xpc_date_get_value", [&process](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto record = s.x[0] ? process.objects().find(s.x[0]) : nullptr;
        const auto value = record ? std::dynamic_pointer_cast<XpcDate>(record->payload) : nullptr;
        return value ? static_cast<std::uint64_t>(value->nanoseconds) : 0;
    }, 0);

    // ssize_t __getdirentries64(int fd, char* buffer, size_t size, off_t* basep):
    // struct direntry {u64 d_ino; u64 d_seekoff; u16 d_reclen; u16 d_namlen;
    // u8 d_type; char d_name[]} records, 8-byte aligned; 0 at the end.
    // The descriptor's guest path comes from fcntl(F_GETPATH); the stream
    // position is kept per descriptor and restarts when it names another path.
    auto streams = std::make_shared<std::pair<std::mutex, std::map<std::uint64_t, DirectoryStream>>>();
    p.libsystem("___getdirentries64", [p, &process, streams](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto descriptor = static_cast<std::uint32_t>(s.x[0]);
        const auto path_buffer = process.allocate_guest(1024);
        // fcntl is variadic: the F_GETPATH buffer is the first stack slot.
        CallRegs registers{};
        registers.x[0] = descriptor; registers.x[1] = 50;
        registers.push_variadic(path_buffer);
        const auto result = p.call_regs(libsystem_library, "_fcntl", registers);
        std::string path;
        if (static_cast<std::int32_t>(result) != -1) path = guest_string(m, path_buffer, 1024);
        process.release_guest(path_buffer);
        if (path.empty()) throw Errno{p.guest_errno() ? p.guest_errno() : ebadf};
        std::lock_guard lock(streams->first);
        auto& stream = streams->second[descriptor];
        if (stream.path != path) {
            stream = DirectoryStream{path, {}, 0};
            const auto host = process.map_guest_path(path);
            std::error_code error;
            if (!std::filesystem::is_directory(host, error)) throw Errno{20}; // ENOTDIR
            stream.entries = {{".", 4}, {"..", 4}};
            for (std::filesystem::directory_iterator it(host, error), end; !error && it != end; it.increment(error)) {
                std::uint8_t type = 8; // DT_REG
                std::error_code kind;
                if (it->is_symlink(kind)) type = 10;
                else if (it->is_directory(kind)) type = 4;
                const auto name = it->path().filename().u8string();
                stream.entries.emplace_back(std::string(name.begin(), name.end()), type);
            }
            if (error) throw Errno{eio};
        }
        if (s.x[3]) m.write64(s.x[3], stream.next);
        std::vector<std::uint8_t> out;
        while (stream.next < stream.entries.size()) {
            const auto& [name, type] = stream.entries[stream.next];
            const auto length = (21 + name.size() + 1 + 7) & ~std::size_t{7};
            if (out.size() + length > s.x[2]) break;
            const auto base = out.size();
            out.resize(base + length, 0);
            const auto inode = name_inode(path, name);
            const auto seek = static_cast<std::uint64_t>(stream.next + 1);
            for (int index = 0; index < 8; ++index) {
                out[base + index] = static_cast<std::uint8_t>(inode >> (index * 8));
                out[base + 8 + index] = static_cast<std::uint8_t>(seek >> (index * 8));
            }
            out[base + 16] = static_cast<std::uint8_t>(length); out[base + 17] = static_cast<std::uint8_t>(length >> 8);
            out[base + 18] = static_cast<std::uint8_t>(name.size()); out[base + 19] = static_cast<std::uint8_t>(name.size() >> 8);
            out[base + 20] = type;
            std::copy(name.begin(), name.end(), out.begin() + static_cast<std::ptrdiff_t>(base + 21));
            ++stream.next;
        }
        if (out.empty() && stream.next < stream.entries.size()) throw Errno{einval}; // buffer too small for one entry
        if (!out.empty()) m.write_bytes(s.x[1], out);
        return out.size();
    });

    // kern_return_t mach_vm_region(vm_map_t, mach_vm_address_t* address, mach_vm_size_t* size,
    //     vm_region_flavor_t, vm_region_info_t info, mach_msg_type_number_t* count, mach_port_t* object_name)
    p.libsystem("_mach_vm_region", [&process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto flavor = static_cast<std::uint32_t>(s.x[3]);
        if (!s.x[1] || !s.x[2] || !s.x[4] || !s.x[5]) return kern_invalid_argument;
        // VM_REGION_BASIC_INFO_64 (9, 9 words) or VM_REGION_BASIC_INFO (10, 8 words).
        const std::uint32_t words = flavor == 9 ? 9 : flavor == 10 ? 8 : 0;
        if (!words || m.read32(s.x[5]) < words) return kern_invalid_argument;
        const auto address = m.read64(s.x[1]);
        // The run of pages with one protection containing the address, or the
        // next one above it (vm/mach_vm.cpp's region walk); bookkeeping-only
        // ranges (__PAGEZERO) are skipped to the next image segment as before.
        auto region = engine_region_from(process.calling_engine(), address);
        if (region && !region->mapped && !(region->readable || region->writable || region->executable)) region.reset();
        if (!region || !(region->readable || region->writable || region->executable || region->mapped)) {
            // The next loaded image segment above the address.
            std::optional<std::uint64_t> next;
            for (const auto& image : process.images())
                for (const auto& section : image.sections)
                    if (section.address > address && (!next || section.address < *next)) next = section.address;
            region = next ? m.query_region(*next) : std::nullopt;
        }
        if (!region || region->base + region->size <= address) return kern_invalid_address;
        const std::uint32_t protection = (region->readable ? 1U : 0U) | (region->writable ? 2U : 0U) | (region->executable ? 4U : 0U);
        m.write64(s.x[1], region->base);
        m.write64(s.x[2], region->size);
        std::vector<std::uint8_t> info(words * 4, 0);
        const auto put32 = [&info](std::size_t offset, std::uint32_t value) {
            for (int index = 0; index < 4; ++index) info[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
        };
        put32(0, protection);
        put32(4, 7);           // max_protection: rwx
        put32(8, 1);           // inheritance: VM_INHERIT_COPY
        // shared, reserved, offset (0), behavior (VM_BEHAVIOR_DEFAULT), user_wired_count stay 0.
        m.write_bytes(s.x[4], info);
        m.write32(s.x[5], words);
        if (s.x[6]) m.write32(s.x[6], 0);
        return kern_success;
    }, kern_invalid_argument);

    // int proc_regionfilename(int pid, uint64_t address, void* buffer, uint32_t size)
    p.libsystem("_proc_regionfilename", [p, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (static_cast<std::uint32_t>(s.x[0]) != static_cast<std::uint32_t>(p.call(libsystem_library, "_getpid", {}))) throw Errno{esrch};
        for (const auto& image : process.images()) {
            if (s.x[1] < image.begin || s.x[1] >= image.end) continue;
            const auto length = std::min<std::uint64_t>(image.name.size(), static_cast<std::uint32_t>(s.x[3]));
            if (length) m.write_bytes(s.x[2], std::span(reinterpret_cast<const std::uint8_t*>(image.name.data()), static_cast<std::size_t>(length)));
            return length;
        }
        return 0;
    }, 0);
}

} // namespace alchemy::export_fill
