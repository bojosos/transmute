#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>

// Mach user-library leftovers (fill-only), over the Mach IPC and VM calls already provided:
//   mach_msg_server_once, mach_msg_server  the MIG server loop: receive on a port, call the
//       demux function with the request and a reply buffer, send the reply unless the demux
//       routine answered MIG_NO_REPLY (or the reply has no destination), destroy a request
//       whose routine failed
//   mig_allocate, mig_deallocate, mig_strncpy  MIG's helpers over vm_allocate/vm_deallocate
//   host_kernel_version  uname's version string
//   clock_sleep        TIME_RELATIVE or TIME_ABSOLUTE (on the clock's own time) over nanosleep
//   mach_task_is_self, mach_vm_behavior_set and vm_behavior_set (madvise's advice)
//   bootstrap_look_up2, bootstrap_register2  bootstrap_look_up / bootstrap_register (the pid
//       and flags select nothing more here)
namespace alchemy::export_fill {
namespace {

// <mach/message.h>, <mach/mig_errors.h>, <mach/kern_return.h>
constexpr std::uint32_t mach_send_msg = 0x1, mach_rcv_msg = 0x2;
constexpr std::uint32_t mach_send_invalid_dest = 0x10000003, mach_send_timed_out = 0x10000004;
constexpr std::uint32_t msgh_bits_complex = 0x80000000U;
constexpr std::int32_t mig_no_reply = -305;
constexpr std::uint64_t max_trailer_size = 68;   // sizeof(mach_msg_max_trailer_t)
constexpr std::uint32_t kern_success = 0, kern_invalid_address = 1, kern_invalid_argument = 4, kern_invalid_value = 18;

} // namespace

void install_libsystem_mach_extra(Provider& p) {
    auto& process = p.process();
    const auto call_address = [&process](std::uint64_t function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    };
    const auto self_task = [&process](GuestMemory& m) -> std::uint32_t {
        const auto cell = process.resolve(libsystem_library, "_mach_task_self_");
        return cell ? m.read32(cell->address) : 0;
    };

    // One receive-demux-reply round of mach_msg_server; KERN_SUCCESS, or the mach_msg error.
    const auto serve_once = [p, call_address](GuestMemory& m, std::uint64_t demux, std::uint32_t max_size, std::uint32_t port,
                                              std::uint32_t options, std::uint64_t request, std::uint64_t reply) -> std::uint32_t {
        const auto size = static_cast<std::uint64_t>(max_size) + max_trailer_size;
        const auto receive = (options & ~mach_send_msg) | mach_rcv_msg;
        auto result = static_cast<std::uint32_t>(p.call(libsystem_library, "_mach_msg", {request, receive, 0, size, port, 0, 0}));
        if (result != kern_success) return result;
        m.fill_bytes(reply, 0, size);
        static_cast<void>(call_address(demux, {request, reply}));
        const auto bits = m.read32(reply);
        const auto code = static_cast<std::int32_t>(m.read32(reply + 32));   // mig_reply_error_t.RetCode
        if (!(bits & msgh_bits_complex) && code != 0) {
            if (code == mig_no_reply) m.write32(reply + 8, 0);
            else {
                // The routine failed: the request's rights and memory are not kept.
                m.write32(request + 8, 0);
                static_cast<void>(p.call(libsystem_library, "_mach_msg_destroy", {request}));
            }
        }
        if (!m.read32(reply + 8)) return kern_success;   // no destination: nothing to send
        result = static_cast<std::uint32_t>(p.call(libsystem_library, "_mach_msg",
            {reply, mach_send_msg | (options & ~(mach_rcv_msg | mach_send_msg)), m.read32(reply + 4), 0, 0, 0, 0}));
        if (result == mach_send_invalid_dest || result == mach_send_timed_out)
            static_cast<void>(p.call(libsystem_library, "_mach_msg_destroy", {reply}));
        return result;
    };
    // mach_msg_return_t mach_msg_server_once(boolean_t (*demux)(mach_msg_header_t*, mach_msg_header_t*),
    //     mach_msg_size_t max_size, mach_port_t rcv_name, mach_msg_options_t options) / mach_msg_server: the loop,
    // until a receive or send fails.
    for (const auto loop : {false, true})
        p.libsystem(loop ? "_mach_msg_server" : "_mach_msg_server_once", [p, serve_once, loop](CpuState& s, GuestMemory& m) -> std::uint64_t {
            if (!s.x[0]) return kern_invalid_argument;
            const auto size = (s.x[1] & 0xffffffffULL) + max_trailer_size;
            const auto request = p.malloc(size), reply = p.malloc(size);
            std::uint32_t result;
            do result = serve_once(m, s.x[0], static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]),
                                   static_cast<std::uint32_t>(s.x[3]), request, reply);
            while (loop && result == kern_success);
            static_cast<void>(p.call(libsystem_library, "_free", {request}));
            static_cast<void>(p.call(libsystem_library, "_free", {reply}));
            return result;
        });

    // void mig_allocate(vm_address_t* address, vm_size_t size): *address 0 when it fails.
    p.libsystem("_mig_allocate", [p, self_task](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) return 0;
        if (p.call(libsystem_library, "_vm_allocate", {self_task(m), s.x[0], s.x[1], 1}) & 0xffffffffULL) m.write64(s.x[0], 0);
        return 0;
    });
    p.libsystem("_mig_deallocate", [p, self_task](CpuState& s, GuestMemory& m) -> std::uint64_t {
        static_cast<void>(p.call(libsystem_library, "_vm_deallocate", {self_task(m), s.x[0], s.x[1]}));
        return 0;
    });
    // int mig_strncpy(char* dest, const char* src, int len): a terminated copy that fits len bytes;
    // the bytes written, the terminating NUL included (0 for len 0).
    p.libsystem("_mig_strncpy", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto length = static_cast<std::int32_t>(s.x[2]);
        if (length <= 0 || !s.x[0]) return 0;
        std::uint32_t at = 0;
        if (s.x[1])
            for (; at + 1 < static_cast<std::uint32_t>(length); ++at) {
                const auto c = m.read8(s.x[1] + at);
                if (!c) break;
                m.write8(s.x[0] + at, c);
            }
        m.write8(s.x[0] + at, 0);
        return at + 1;
    });

    // kern_return_t host_kernel_version(host_t, kernel_version_t): uname's version (512 bytes at most).
    p.libsystem("_host_kernel_version", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1]) return kern_invalid_argument;
        const auto names = p.malloc(5 * 256);
        static_cast<void>(p.call(libsystem_library, "_uname", {names}));
        auto version = m.read_string(names + 3 * 256, 1, 256);
        static_cast<void>(p.call(libsystem_library, "_free", {names}));
        if (version.size() > 511) version.resize(511);
        version.push_back(0);
        m.write_bytes(s.x[1], version);
        return kern_success;
    });

    // kern_return_t clock_sleep(clock_serv_t, sleep_type_t, mach_timespec_t sleep_time, mach_timespec_t* wakeup):
    // mach_timespec_t {unsigned tv_sec; int tv_nsec} arrives in one register.
    p.libsystem("_clock_sleep", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        constexpr std::uint32_t time_absolute = 0, time_relative = 1;
        const auto type = static_cast<std::uint32_t>(s.x[1]);
        const auto seconds = static_cast<std::int64_t>(static_cast<std::uint32_t>(s.x[2]));
        const auto nanoseconds = static_cast<std::int64_t>(static_cast<std::int32_t>(s.x[2] >> 32));
        if ((type != time_absolute && type != time_relative) || nanoseconds < 0 || nanoseconds >= 1'000'000'000) return kern_invalid_value;
        const auto now = p.malloc(16);
        std::int64_t wait = seconds * 1'000'000'000 + nanoseconds;
        if (type == time_absolute) {
            if (const auto result = p.call(libsystem_library, "_clock_get_time", {s.x[0], now}) & 0xffffffffULL) {
                static_cast<void>(p.call(libsystem_library, "_free", {now}));
                return result;
            }
            wait -= static_cast<std::int64_t>(m.read32(now)) * 1'000'000'000 + static_cast<std::int32_t>(m.read32(now + 4));
        }
        if (wait > 0) {
            m.write64(now, static_cast<std::uint64_t>(wait / 1'000'000'000));
            m.write64(now + 8, static_cast<std::uint64_t>(wait % 1'000'000'000));
            static_cast<void>(p.call(libsystem_library, "_nanosleep", {now, 0}));
        }
        if (s.x[3]) static_cast<void>(p.call(libsystem_library, "_clock_get_time", {s.x[0], s.x[3]}));
        static_cast<void>(p.call(libsystem_library, "_free", {now}));
        return kern_success;
    });

    // boolean_t mach_task_is_self(task_t)
    p.libsystem("_mach_task_is_self", [self_task](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return static_cast<std::uint32_t>(s.x[0]) == self_task(m) ? 1 : 0;
    });

    // kern_return_t mach_vm_behavior_set(task, address, size, vm_behavior_t) / vm_behavior_set: this task's
    // range, as madvise's advice (VM_BEHAVIOR_ZERO_WIRED_PAGES has none: no page is wired here).
    for (const auto* name : {"_mach_vm_behavior_set", "_vm_behavior_set"})
        p.libsystem(name, [p, self_task](CpuState& s, GuestMemory& m) -> std::uint64_t {
            if (static_cast<std::uint32_t>(s.x[0]) != self_task(m)) return kern_invalid_argument;
            constexpr std::array<std::int32_t, 12> advice{0, 1, 2, 2, 3, 4, 5, -1, 7, 8, 9, 10};
            const auto behavior = static_cast<std::int32_t>(s.x[3]);
            if (behavior < 0 || behavior >= static_cast<std::int32_t>(advice.size())) return kern_invalid_argument;
            if (advice[static_cast<std::size_t>(behavior)] < 0 || !s.x[2]) return kern_success;
            const auto result = static_cast<std::int32_t>(p.call(libsystem_library, "_madvise",
                {s.x[1], s.x[2], static_cast<std::uint32_t>(advice[static_cast<std::size_t>(behavior)])}));
            if (!result) return kern_success;
            return p.guest_errno() == enomem ? kern_invalid_address : kern_invalid_argument;
        });

    // kern_return_t bootstrap_look_up2(bp, name, mach_port_t* sp, pid_t, uint64_t flags) /
    // bootstrap_register2(bp, name, mach_port_t sp, uint64_t flags)
    p.libsystem("_bootstrap_look_up2", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        return p.call(libsystem_library, "_bootstrap_look_up", {s.x[0], s.x[1], s.x[2]}) & 0xffffffffULL;
    });
    p.libsystem("_bootstrap_register2", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        return p.call(libsystem_library, "_bootstrap_register", {s.x[0], s.x[1], s.x[2]}) & 0xffffffffULL;
    });
}

} // namespace alchemy::export_fill
