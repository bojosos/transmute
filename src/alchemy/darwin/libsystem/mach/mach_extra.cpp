#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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
//   exc_server, exception_raise, exception_raise_state, exception_raise_state_identity  the exc
//       subsystem's MIG server and client routines (install_libsystem_exc)
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

void install_libsystem_exc(Provider& p);

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
    // The deprecated server and subset calls: bootstrap_create_server(bp, cmd, uid, on_demand, port*),
    // bootstrap_create_service(bp, name, port*) and bootstrap_subset(bp, requestor, port*). Alchemy's bootstrap
    // namespace launches no servers on demand and makes no subsets: BOOTSTRAP_NOT_PRIVILEGED, as launchd answers a
    // caller it does not let do so; *port stays MACH_PORT_NULL.
    constexpr std::uint32_t bootstrap_not_privileged = 1100;
    for (const auto& [name, out] : std::array<std::pair<const char*, int>, 3>{{
             {"_bootstrap_create_server", 4}, {"_bootstrap_create_service", 2}, {"_bootstrap_subset", 2}}})
        p.libsystem(name, [out = out, refused = bootstrap_not_privileged](CpuState& s, GuestMemory& m) -> std::uint64_t {
            if (const auto port = s.x[static_cast<std::size_t>(out)]) m.write32(port, 0);
            return refused;
        });

    install_libsystem_exc(p);
}

// The exc subsystem (exc.defs, MIG ids 2401-2403): its server routine exc_server, which calls the program's
// own catch_exception_raise, catch_exception_raise_state and catch_exception_raise_state_identity, and its
// client routines exception_raise, exception_raise_state and exception_raise_state_identity, which send the
// request and wait for the reply. The layouts are MIG's for exc.defs on arm64 (32-bit codes,
// exception_data_t integer_t[2], thread_state_t natural_t[THREAD_STATE_MAX]).
void install_libsystem_exc(Provider& p) {
    auto& process = p.process();
    constexpr std::uint32_t raise_id = 2401, state_id = 2402, identity_id = 2403, reply_offset = 100;
    constexpr std::int32_t mig_bad_id = -303, mig_bad_arguments = -304, mig_reply_mismatch = -301, mig_array_too_large = -307;
    constexpr std::uint32_t state_max = 1296;   // THREAD_STATE_MAX (natural_t)
    constexpr std::uint32_t copy_send = 19, make_send_once = 21, port_descriptor = 0;
    constexpr std::array<std::uint8_t, 8> ndr{0, 0, 0, 0, 1, 0, 0, 0};   // NDR_record: little-endian integers
    const auto write_ndr = [ndr](GuestMemory& m, std::uint64_t at) { m.write_bytes(at, ndr); };
    // The program's catch_* routine (dlsym RTLD_DEFAULT, as dyld binds exc_server's references).
    const auto handler = [p](GuestMemory& m, std::string_view name) -> std::uint64_t {
        const std::string text(name);
        const auto string = p.malloc(text.size() + 1);
        m.write_bytes(string, std::span(reinterpret_cast<const std::uint8_t*>(text.c_str()), text.size() + 1));
        const auto address = p.call(libsystem_library, "_dlsym", {static_cast<std::uint64_t>(-2), string});
        static_cast<void>(p.call(libsystem_library, "_free", {string}));
        if (!address) throw std::runtime_error("exc_server: the program defines no " + text);
        return address;
    };

    // boolean_t exc_server(mach_msg_header_t* InHeadP, mach_msg_header_t* OutHeadP)
    p.libsystem("_exc_server", [&process, handler, write_ndr](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto in = s.x[0], out = s.x[1];
        if (!in || !out) return 0;
        const auto bits = m.read32(in), size = m.read32(in + 4), id = m.read32(in + 20);
        // The reply's header: the request's reply right back to its sender, id + 100.
        m.write32(out, bits & 0x1f);
        m.write32(out + 4, 36);
        m.write32(out + 8, m.read32(in + 8));   // a received request's remote port is the sender's reply port
        m.write32(out + 12, 0);
        m.write32(out + 16, 0);
        m.write32(out + 20, id + reply_offset);
        write_ndr(m, out + 24);
        const auto fail = [&](std::int32_t code) { m.write32(out + 32, static_cast<std::uint32_t>(code)); };
        if (id < raise_id || id > identity_id) { fail(mig_bad_id); return 0; }
        const auto port = m.read32(in + 12);   // msgh_local_port: the exception port the request came to
        const bool identity = id == identity_id, complex = id != state_id;
        // Complex requests carry the thread and task port descriptors before the NDR record.
        const std::uint64_t args = in + (complex ? 60 : 32);
        if ((complex && (!(bits & msgh_bits_complex) || m.read32(in + 24) != 2)) || size < (args - in) + 8) { fail(mig_bad_arguments); return 1; }
        const auto thread = complex ? m.read32(in + 28) : 0, task = complex ? m.read32(in + 40) : 0;
        const auto exception = m.read32(args), code_count = m.read32(args + 4);
        const auto code = args + 8;
        if (code_count > 2) { fail(mig_bad_arguments); return 1; }
        if (id == raise_id) {
            if (size != (code - in) + 4 * code_count) { fail(mig_bad_arguments); return 1; }
            const auto result = EngineAdapter(process.calling_engine()).call(handler(m, "catch_exception_raise"),
                std::array<std::uint64_t, 6>{port, thread, task, exception, code, code_count});
            fail(static_cast<std::int32_t>(result));
            return 1;
        }
        const auto flavor_at = code + 4 * code_count;
        const std::uint32_t old_count = m.read32(flavor_at + 4);
        const std::uint64_t old_state = flavor_at + 8;
        if (old_count > state_max || size != (old_state - in) + 4 * old_count) { fail(mig_bad_arguments); return 1; }
        // Reply: RetCode, flavor (in/out), new_stateCnt (in: its capacity), new_state.
        m.write32(out + 36, m.read32(flavor_at));
        m.write32(out + 40, state_max);
        CallRegs registers;
        if (identity) {
            registers.x = {port, thread, task, exception, code, code_count, out + 36, old_state};
            registers.push_stack(old_count);
            registers.push_stack(out + 44);
            registers.push_stack(out + 40);
        } else {
            registers.x = {port, exception, code, code_count, out + 36, old_state, old_count, out + 44};
            registers.push_stack(out + 40);
        }
        const auto result = static_cast<std::int32_t>(EngineAdapter(process.calling_engine())
            .call_regs(handler(m, identity ? "catch_exception_raise_state_identity" : "catch_exception_raise_state"), registers).x0);
        fail(result);
        if (result != 0) return 1;   // an error reply: RetCode only
        const auto new_count = std::min(m.read32(out + 40), std::uint32_t{state_max});
        m.write32(out + 40, new_count);
        m.write32(out + 4, 44 + 4 * new_count);
        return 1;
    });

    // The client routines: the request to the exception port, the reply on the thread's MIG reply port.
    const auto exchange = [p, write_ndr](GuestMemory& m, std::uint32_t exception_port, std::uint32_t id, std::uint32_t thread,
                                          std::uint32_t task, std::uint32_t exception, std::uint64_t code, std::uint32_t code_count,
                                          std::uint64_t flavor, std::uint64_t old_state, std::uint32_t old_count,
                                          std::uint64_t new_state, std::uint64_t new_count) -> std::uint32_t {
        if (code_count > 2 || old_count > state_max || (code_count && !code) || (old_count && !old_state))
            return static_cast<std::uint32_t>(mig_bad_arguments);
        const bool complex = id != state_id, state = id != raise_id;
        const auto capacity = static_cast<std::uint64_t>(44 + 4 * state_max) + max_trailer_size;
        const auto buffer = p.malloc(std::max<std::uint64_t>(capacity, 76 + 4 * (2 + state_max)));
        const auto reply_port = static_cast<std::uint32_t>(p.call(libsystem_library, "_mig_get_reply_port", {}));
        m.fill_bytes(buffer, 0, 76);
        m.write32(buffer, copy_send | (make_send_once << 8) | (complex ? msgh_bits_complex : 0));
        m.write32(buffer + 8, exception_port);
        m.write32(buffer + 12, reply_port);
        m.write32(buffer + 20, id);
        std::uint64_t at = buffer + 24;
        if (complex) {
            m.write32(at, 2);   // two port descriptors: the thread and the task, copied send rights
            for (const auto name : {thread, task}) {
                m.write32(at + 4, name);
                m.write32(at + 8, 0);
                m.write8(at + 14, static_cast<std::uint8_t>(copy_send));
                m.write8(at + 15, static_cast<std::uint8_t>(port_descriptor));
                at += 12;
            }
            at += 4;
        }
        write_ndr(m, at);
        m.write32(at + 8, exception);
        m.write32(at + 12, code_count);
        at += 16;
        for (std::uint32_t index = 0; index < code_count; ++index, at += 4) m.write32(at, m.read32(code + 4 * index));
        if (state) {
            m.write32(at, flavor ? m.read32(flavor) : 0);
            m.write32(at + 4, old_count);
            at += 8;
            for (std::uint32_t index = 0; index < old_count; ++index, at += 4) m.write32(at, m.read32(old_state + 4 * index));
        }
        const auto send_size = static_cast<std::uint32_t>(at - buffer);
        auto result = static_cast<std::uint32_t>(p.call(libsystem_library, "_mach_msg",
            {buffer, mach_send_msg | mach_rcv_msg, send_size, capacity, reply_port, 0, 0}));
        if (result == kern_success) {
            const auto reply_id = m.read32(buffer + 20), code_value = m.read32(buffer + 32);
            if (reply_id != id + reply_offset) result = static_cast<std::uint32_t>(mig_reply_mismatch);
            else if (code_value != 0 || !state) result = code_value;
            else {
                const auto count = m.read32(buffer + 40), room = new_count ? m.read32(new_count) : 0;
                if (flavor) m.write32(flavor, m.read32(buffer + 36));
                const auto copied = std::min(count, room);
                for (std::uint32_t index = 0; index < copied; ++index) m.write32(new_state + 4 * index, m.read32(buffer + 44 + 4 * index));
                if (new_count) m.write32(new_count, count);
                result = count > room ? static_cast<std::uint32_t>(mig_array_too_large) : kern_success;
            }
        } else if (result != mach_send_invalid_dest && result != mach_send_timed_out) {
            // A receive failure leaves the reply port in an unknown state: MIG drops it.
            static_cast<void>(p.call(libsystem_library, "_mig_dealloc_reply_port", {reply_port}));
        }
        static_cast<void>(p.call(libsystem_library, "_free", {buffer}));
        return result;
    };
    // kern_return_t exception_raise(mach_port_t exception_port, mach_port_t thread, mach_port_t task,
    //     exception_type_t exception, exception_data_t code, mach_msg_type_number_t codeCnt)
    p.libsystem("_exception_raise", [exchange](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return exchange(m, static_cast<std::uint32_t>(s.x[0]), raise_id, static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]),
                        static_cast<std::uint32_t>(s.x[3]), s.x[4], static_cast<std::uint32_t>(s.x[5]), 0, 0, 0, 0, 0);
    });
    // kern_return_t exception_raise_state(mach_port_t exception_port, exception_type_t exception,
    //     const exception_data_t code, mach_msg_type_number_t codeCnt, int* flavor, const thread_state_t old_state,
    //     mach_msg_type_number_t old_stateCnt, thread_state_t new_state, mach_msg_type_number_t* new_stateCnt [stack])
    p.libsystem("_exception_raise_state", [exchange](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return exchange(m, static_cast<std::uint32_t>(s.x[0]), state_id, 0, 0, static_cast<std::uint32_t>(s.x[1]), s.x[2],
                        static_cast<std::uint32_t>(s.x[3]), s.x[4], s.x[5], static_cast<std::uint32_t>(s.x[6]), s.x[7], m.read64(s.sp));
    });
    // kern_return_t exception_raise_state_identity(exception_port, thread, task, exception, code, codeCnt, int* flavor,
    //     old_state, old_stateCnt [stack, 4 bytes], new_state [stack, 8], new_stateCnt* [stack, 8])
    p.libsystem("_exception_raise_state_identity", [exchange](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return exchange(m, static_cast<std::uint32_t>(s.x[0]), identity_id, static_cast<std::uint32_t>(s.x[1]),
                        static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]), s.x[4],
                        static_cast<std::uint32_t>(s.x[5]), s.x[6], s.x[7], m.read32(s.sp), m.read64(s.sp + 8), m.read64(s.sp + 16));
    });
}

} // namespace alchemy::export_fill
