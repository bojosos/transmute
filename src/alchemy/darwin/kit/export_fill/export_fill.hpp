#pragma once

// The export-fill kit: fill-only registration over a ProcessContext (no
// Objective-C), errno conventions and shared helpers of the providers that
// fill process-level gaps (libSystem, libz, CF, Foundation, CoreServices, ...).

#include "alchemy/darwin/kit/export_fill/libraries.hpp"

#include "alchemy/call_regs.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/object_store.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/placeholder_exports.hpp"
#include "alchemy/registration_audit.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <source_location>
#include <vector>

namespace alchemy::export_fill {

using transmute::CpuState;
using transmute::GuestMemory;

// <sys/errno.h> (Darwin numbering).
inline constexpr std::uint32_t eperm = 1, enoent = 2, esrch = 3, eintr = 4, eio = 5, ebadf = 9, enomem = 12,
    eacces = 13, efault = 14, eexist = 17, einval = 22, enotty = 25, erange = 34, eagain = 35, enotsup = 45,
    eafnosupport = 47, emsgsize = 40, enametoolong = 63, enosys = 78, eoverflow = 84;

// Whether `library` already exports `name` for real. A placeholder export (one
// that only fails explicitly when called) counts as missing, so the export-fill
// implementation replaces it.
inline bool provided(ProcessContext& process, std::string_view library, std::string_view name) {
    return process.resolve(library, name) && !take_placeholder_export(process, library, name);
}

// Thrown by a handler to fail with errno (the C convention: -1/NULL result).
struct Errno { std::uint32_t value; };

class Provider {
public:
    explicit Provider(ProcessContext& process) : process_(process) {}
    ProcessContext& process() const noexcept { return process_; }

    // Installs `name` in `library` unless a provider already exports it there
    // (base Alchemy services keep precedence; this module only fills gaps).
    // A handler that throws Errno sets errno and returns `failure`.
    template <class Handler>
    void function(std::string_view library, const char* name, Handler handler, std::uint64_t failure = UINT64_MAX,
                  const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const bool skipped = provided(process_, library, name);
        registration_audit::record_fill(library, name, skipped, where);
        if (skipped) return;
        auto& process = process_;
        static_cast<void>(process_.install_function(name, [&process, handler, failure](CpuState& s, GuestMemory& m) {
            try {
                s.x[0] = handler(s, m);
            } catch (const Errno& error) {
                process.set_errno(error.value);
                s.x[0] = failure;
            }
        }, std::string(library)));
    }
    template <class Handler>
    void libsystem(const char* name, Handler handler, std::uint64_t failure = UINT64_MAX,
                   const std::source_location& where = std::source_location::current()) {
        function(libsystem_library, name, std::move(handler), failure, where);
    }
    // A data export (`size` bytes, zero-filled unless `bytes` is given).
    std::uint64_t data(std::string_view library, const char* name, std::uint64_t size,
                       std::span<const std::uint8_t> bytes = {},
                       const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        if (const auto found = process_.resolve(library, name)) {
            registration_audit::record_fill(library, name, true, where);
            return found->address;
        }
        registration_audit::record_fill(library, name, false, where);
        const auto address = process_.allocate_guest(size, 16);
        std::vector<std::uint8_t> zero(static_cast<std::size_t>(size), 0);
        std::copy(bytes.begin(), bytes.end(), zero.begin());
        process_.main_engine().write_bytes(address, zero);
        process_.install_export(std::string(library), name, {address, ExportKind::data});
        return address;
    }
    // Calls an exported guest function (another provider's or a guest image's).
    std::uint64_t call(std::string_view library, std::string_view name, std::initializer_list<std::uint64_t> arguments) const {
        const auto found = process_.resolve(library, name);
        if (!found) throw std::runtime_error("Alchemy export fill: required export " + std::string(name) + " is missing");
        return EngineAdapter(process_.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t call_regs(std::string_view library, std::string_view name, const CallRegs& registers) const {
        const auto found = process_.resolve(library, name);
        if (!found) throw std::runtime_error("Alchemy export fill: required export " + std::string(name) + " is missing");
        return EngineAdapter(process_.calling_engine()).call_regs(found->address, registers).x0;
    }
    // Guest malloc: memory the guest may release with free().
    std::uint64_t malloc(std::uint64_t size) const {
        const auto address = call(libsystem_library, "_malloc", {size ? size : 1});
        if (!address) throw Errno{enomem};
        return address;
    }
    // errno of the calling guest thread (after a nested call failed).
    std::uint32_t guest_errno() const {
        const auto thread = process_.current_thread();
        return thread && thread->errno_address ? process_.calling_engine().read32(thread->errno_address) : 0;
    }

private:
    ProcessContext& process_;
};

// A host network interface under its Darwin name (lo0, en<n>, utun<n>).
struct Interface {
    struct Address {
        std::uint8_t family{};                 // 2 (AF_INET) or 30 (AF_INET6)
        std::array<std::uint8_t, 16> bytes{};  // network order
        unsigned prefix{};
        std::uint32_t scope{};
    };
    std::string name;
    std::uint32_t index{};
    bool up{}, loopback{}, point_to_point{}, multicast{};
    std::uint32_t mtu{};
    std::uint64_t speed{};
    std::vector<std::uint8_t> mac;             // empty or 6 bytes
    std::vector<Address> addresses;
};
std::vector<Interface> host_interfaces();
std::string host_machine_uuid();
// The getifaddrs list for `interfaces` as one block placed at guest `base`.
std::vector<std::uint8_t> serialize_ifaddrs(const std::vector<Interface>& interfaces, std::uint64_t base);

// CFUUIDRef: an immutable 16-byte identifier object (class __NSCFUUID).
struct CfUuid : ObjectPayload { std::array<std::uint8_t, 16> bytes{}; };
inline std::uint64_t cf_uuid_class(ObjcRuntime& objc) {
    if (const auto found = objc.find_class("__NSCFUUID")) return found;
    return objc.register_native_class("__NSCFUUID", objc.find_class("NSObject"), 16);
}

// CFTimeZoneRef / NSTimeZone: instances of __NSTimeZone (a subclass of
// NSTimeZone) naming an IANA zone or a fixed offset ("GMT+0200").
struct CfTimeZone : ObjectPayload { std::uint64_t name{}; std::string identifier; };
std::string default_time_zone();
std::uint64_t time_zone_class(ObjcRuntime& objc);
std::uint64_t make_time_zone(ProcessContext& process, CoreFoundation& cf, ObjcRuntime& objc, const std::string& name);

void install_libsystem_net(Provider& p);
void install_libsystem_ipc(Provider& p);
void install_libsystem_sysv(Provider& p);
void install_libsystem_process(Provider& p);
void install_libsystem_runtime(Provider& p);
void install_libsystem_misc(Provider& p);
void install_libsystem_services(Provider& p, ObjcRuntime& objc);
void install_dispatch_data(Provider& p, ObjcRuntime& objc);
void install_cc_cryptor(Provider& p);
void install_cf_extra(Provider& p, ObjcRuntime& objc, CoreFoundation& cf);
void install_cf_system(Provider& p, CoreFoundation& cf);   // darwin/corefoundation/cf_system.cpp
void install_cf_preferences(Provider& p, ObjcRuntime& objc, CoreFoundation& cf);
void install_cf_locale(Provider& p, ObjcRuntime& objc, CoreFoundation& cf);
void install_foundation_classes(Provider& p, ObjcRuntime& objc, CoreFoundation& cf);
void install_coreservices(Provider& p, ObjcRuntime& objc, CoreFoundation& cf);
void install_iokit_registry(Provider& p, ObjcRuntime& objc, CoreFoundation& cf);

inline std::string guest_string(const GuestMemory& memory, std::uint64_t address, std::size_t maximum = 4096) {
    if (!address) throw Errno{efault};
    bool terminated{};
    const auto bytes = memory.read_string(address, 1, maximum, &terminated);
    if (!terminated) throw Errno{enametoolong};
    return {bytes.begin(), bytes.end()};
}

inline void put_string(GuestMemory& memory, std::uint64_t address, std::string_view value) {
    std::vector<std::uint8_t> bytes(value.begin(), value.end());
    bytes.push_back(0);
    memory.write_bytes(address, bytes);
}

inline void write16(GuestMemory& memory, std::uint64_t address, std::uint16_t value) {
    memory.write8(address, static_cast<std::uint8_t>(value));
    memory.write8(address + 1, static_cast<std::uint8_t>(value >> 8));
}

// Darwin passes variadic arguments in 8-byte stack slots (not x registers).
inline std::uint64_t variadic(const CpuState& state, const GuestMemory& memory, unsigned index) {
    return memory.read64(state.sp + index * 8ULL);
}

} // namespace alchemy::export_fill
