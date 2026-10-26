// machsys provider tests: the libSystem/libbsm/power-metrics imports of the
// Qt WebEngine images resolve, and each area behaves as documented in
// src/alchemy/darwin/libsystem/mach/machsys.cpp (Mach IPC failures, launchd data objects,
// sandbox answers, task/thread queries, libc leftovers, os_unfair_lock).
#include "alchemy/gui/tests/fill_fixture.hpp"

namespace {
using namespace fill_test;
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";
constexpr std::string_view ncurses = "/usr/lib/libncurses.5.4.dylib";

void imports(Fixture& f) {
    imports_resolve(f, {sys, "/usr/lib/libbsm.0.dylib", "/usr/lib/libpmenergy.dylib", "/usr/lib/libpmsample.dylib", ncurses});
}

void mach(Fixture& f) {
    // Mach IPC is the in-process namespace of src/alchemy/darwin/libsystem/mach
    // (tested there); the machsys fallbacks no longer apply to these names.
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto message = f.block(64);
    f.engine.write32(message, 19);   // MACH_MSG_TYPE_COPY_SEND to MACH_PORT_NULL
    require(f.status(sys, "_mach_msg", {message, 1, 24, 0, 0, 0, 0}) == 0x10000003, "mach_msg send to MACH_PORT_NULL");
    require(f.status(sys, "_mach_msg", {message, 2, 0, 64, 5, 0, 0}) == 0x10004002, "mach_msg receive on an unknown name");
    const auto name = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0 && f.engine.read32(name) != 0, "mach_port_allocate");
    require(f.status(sys, "_mach_port_mod_refs", {self, 0x1234, 1, 1}) == 15, "mach_port_mod_refs invalid name");
    // No exception port is registered.
    const auto count = f.block(4, 0xff);
    require(f.status(sys, "_thread_get_exception_ports", {0, 0x3ffe, f.block(64), count, f.block(64), f.block(64), f.block(64)}) == 0 &&
            f.engine.read32(count) == 0, "thread_get_exception_ports reports none");
    require(f.status(sys, "_thread_set_exception_ports", {0, 0x3ffe, 1, 1, 6}) == 46, "thread_set_exception_ports");
    require(f.read_text(f.call(sys, "_mach_error_string", {46})) == "(os/kern) not supported", "mach_error_string");
    static_cast<void>(f.call(sys, "_mach_error", {f.text("machsys test:"), 46}));   // a line on standard error
    require(f.status(sys, "_thread_set_state", {0, 6, f.block(272), 68}) == 46 && f.status(sys, "_thread_abort_safely", {0}) == 46,
            "another thread's registers are not reachable");
    // vm_read_overwrite copies within this task.
    const auto source = f.text("overwrite me"), target = f.block(16), copied = f.block(8);
    require(f.status(sys, "_vm_read_overwrite", {self, source, 12, target, copied}) == 0 && f.read_text(target).substr(0, 12) == "overwrite me" &&
            f.engine.read64(copied) == 12, "vm_read_overwrite");
    require(f.status(sys, "_mach_vm_read_overwrite", {self + 1000, source, 12, target, copied}) == 4, "another task is KERN_INVALID_ARGUMENT");
    // vm_write / mach_vm_copy within this task, vm_inherit, the page sizes and clocks.
    const auto written = f.block(16), copy_target = f.block(16);
    require(f.status(sys, "_vm_write", {self, written, source, 9}) == 0 && f.read_text(written).substr(0, 9) == "overwrite", "vm_write");
    require(f.status(sys, "_mach_vm_copy", {self, source, 12, copy_target}) == 0 && f.read_text(copy_target).substr(0, 12) == "overwrite me", "mach_vm_copy");
    require(f.status(sys, "_vm_inherit", {self, written, 16, 1}) == 0 && f.status(sys, "_mach_vm_inherit", {self, written, 16, 0}) == 46,
            "vm_inherit: children get copies only");
    require(f.memory().read64(f.symbol(sys, "_vm_kernel_page_size")) == 16384 && f.memory().read32(f.symbol(sys, "_vm_kernel_page_shift")) == 14,
            "vm_kernel_page_size/shift");
    const auto page = f.block(8), times = f.block(32);
    require(f.status(sys, "_host_page_size", {f.call(sys, "_host_self"), page}) == 0 && f.memory().read64(page) == 16384, "host_page_size");
    require(f.status(sys, "_mach_get_times", {times, times + 8, times + 16}) == 0 && f.memory().read64(times) != 0 &&
            f.memory().read64(times + 16) > 0, "mach_get_times");
    require(f.call(sys, "_mach_approximate_time") != 0, "mach_approximate_time");
    require(f.status(sys, "_thread_create", {self, page}) == 46, "Mach threads without pthreads are not supported");
    require(f.call(sys, "_mig_get_reply_port") != 0, "mig_get_reply_port");
    require(f.memory().read8(f.symbol(sys, "_NDR_record") + 4) == 1, "NDR_record int_rep");
}

void jit(Fixture& f) {
    // MAP_JIT memory is RWX for every thread: the toggle is supported and changes nothing.
    require(f.call(sys, "_pthread_jit_write_protect_supported_np") == 1, "pthread_jit_write_protect_supported_np");
    static_cast<void>(f.call(sys, "_pthread_jit_write_protect_np", {0}));
    static_cast<void>(f.call(sys, "_pthread_jit_write_protect_np", {1}));
}

void tasks(Fixture& f) {
    // Tasks: only the calling task.
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto pid = static_cast<std::uint32_t>(f.call(sys, "_getpid"));
    const auto out = f.block(4);
    require(f.status(sys, "_task_for_pid", {self, pid, out}) == 0 && f.engine.read32(out) == self, "task_for_pid(self)");
    require(f.status(sys, "_task_for_pid", {self, pid + 1, out}) == 5, "task_for_pid(other)");
    require(f.status(sys, "_pid_for_task", {self, out}) == 0 && f.engine.read32(out) == pid, "pid_for_task");
    require(static_cast<std::uint32_t>(f.call(sys, "_task_self_trap")) == self, "task_self_trap");
    const auto policy = f.block(4, 0xff), policy_count = f.block(4), defaulted = f.block(4);
    f.engine.write32(policy_count, 1);
    require(f.status(sys, "_task_policy_get", {self, 1, policy, policy_count, defaulted}) == 0 && f.engine.read32(policy) == 0,
            "task_policy_get(TASK_CATEGORY_POLICY)");
    require(f.status(sys, "_task_suspend", {self}) == 46, "task_suspend");
    require(f.status(sys, "_thread_switch", {0, 0, 0}) == 0, "thread_switch");
}

void vm(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    // VM constants and statistics.
    require(f.engine.read64(f.symbol(sys, "_vm_page_mask")) == 0x3fff && f.engine.read32(f.symbol(sys, "_vm_page_shift")) == 14, "page constants");
    const auto info = f.block(152), info_count = f.block(4);
    f.engine.write32(info_count, 38);
    require(f.status(sys, "_host_statistics64", {0, 4, info, info_count}) == 0 && f.engine.read32(info_count) == 38, "host_statistics64");
    require(f.engine.read32(info) > 0, "free pages reported");
    require(f.status(sys, "_host_statistics64", {0, 99, info, info_count}) == 4, "host_statistics64 unknown flavor");
    const auto zones = f.block(8), zone_count = f.block(4);
    require(f.status(sys, "_malloc_get_all_zones", {self, 0, zones, zone_count}) == 0 && f.engine.read32(zone_count) == 1, "malloc_get_all_zones");
    // mach_vm_read copies memory.
    const auto source = f.text("alchemy"), copy = f.block(8), copy_size = f.block(4);
    require(f.status(sys, "_mach_vm_read", {self, source, 8, copy, copy_size}) == 0 && f.read_text(f.engine.read64(copy)) == "alchemy", "mach_vm_read");
    require(f.status(sys, "_vm_deallocate", {self, f.engine.read64(copy), f.engine.read32(copy_size)}) == 0, "the copy is VM memory");
}

void bsm(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    // libbsm: audit_token_t val[5] is the pid.
    const auto token = f.block(32);
    f.engine.write32(token + 20, 4321);
    require(f.call("/usr/lib/libbsm.0.dylib", "_audit_token_to_pid", {token}) == 4321, "audit_token_to_pid");
    require(f.status("/usr/lib/libpmsample.dylib", "_pm_sample_task", {self, 0, 0, 0}) == 46, "pm_sample_task");
}

void launchd(Fixture& f) {
    const auto port = f.block(4);
    const auto bootstrap = f.engine.read32(f.symbol(sys, "_bootstrap_port"));
    require(f.status(sys, "_bootstrap_look_up", {bootstrap, f.text("com.example.service"), port}) == 1102, "bootstrap_look_up");
    require(f.read_text(f.call(sys, "_bootstrap_strerror", {1102})) == "Unknown service name", "bootstrap_strerror");
    require(f.call(sys, "_launch_msg", {0}) == 0, "launch_msg fails");
    const auto dictionary = f.call(sys, "_launch_data_alloc", {1});
    const auto value = f.call(sys, "_launch_data_new_string", {f.text("value")});
    require(f.call(sys, "_launch_data_dict_insert", {dictionary, value, f.text("key")}) == 1, "dict_insert");
    require(f.call(sys, "_launch_data_dict_lookup", {dictionary, f.text("key")}) == value, "dict_lookup");
    require(f.call(sys, "_launch_data_get_type", {value}) == 7, "string type");
    const auto array = f.call(sys, "_launch_data_alloc", {2});
    const auto number = f.call(sys, "_launch_data_new_integer", {42});
    require(f.call(sys, "_launch_data_array_set_index", {array, number, 2}) == 1, "array_set_index");
    require(f.call(sys, "_launch_data_array_get_count", {array}) == 3, "array count");
    require(f.call(sys, "_launch_data_get_integer", {f.call(sys, "_launch_data_array_get_index", {array, 2})}) == 42, "array value");
    static_cast<void>(f.call(sys, "_launch_data_dict_insert", {dictionary, array, f.text("list")}));
    static_cast<void>(f.call(sys, "_launch_data_free", {dictionary}));
    require(f.status(sys, "_notify_register_dispatch", {f.text("com.example"), f.block(4), 0, 0}) == 10, "notify_register_dispatch without a queue");
    require(f.status(sys, "_notify_cancel", {1}) == 2, "notify_cancel");
}

void sandbox(Fixture& f) {
    require(f.call(sys, "_sandbox_check", {1, f.text("file-read-data"), 0}) == 0, "unsandboxed: allowed");
    const auto error = f.block(8);
    require(f.status(sys, "_sandbox_init", {f.text("no-network"), 1, error}) == -1, "sandbox_init refused");
    require(!f.read_text(f.engine.read64(error)).empty(), "sandbox_init error text");
    static_cast<void>(f.call(sys, "_sandbox_free_error", {f.engine.read64(error)}));
    require(f.read_text(f.symbol(sys, "_kSBXProfileNoInternet")) == "no-internet", "kSBXProfileNoInternet");
    require(f.read_text(f.engine.read64(f.symbol(sys, "_APP_SANDBOX_READ"))) == "com.apple.app-sandbox.read", "APP_SANDBOX_READ");
}

void libc(Fixture& f) {
    // termios speeds (c_ispeed at 56, c_ospeed at 64).
    const auto termios = f.block(72);
    require(f.call(sys, "_cfsetispeed", {termios, 9600}) == 0 && f.call(sys, "_cfgetispeed", {termios}) == 9600, "cfsetispeed");
    require(f.call(sys, "_cfsetospeed", {termios, 19200}) == 0 && f.call(sys, "_cfgetospeed", {termios}) == 19200, "cfsetospeed");
    // Generic atomics.
    const auto from = f.text("0123456789abcdefghijklm"), to = f.block(24);
    static_cast<void>(f.call(sys, "___atomic_load", {24, from, to, 5}));
    require(f.read_text(to) == "0123456789abcdefghijklm", "__atomic_load");
    // Mach-O section lookups in a synthesized header.
    const auto header = f.block(32 + 72 + 80);
    f.engine.write32(header, 0xfeedfacf);
    f.engine.write32(header + 16, 1);
    f.engine.write32(header + 20, 72 + 80);
    const auto segment = header + 32;
    f.engine.write32(segment, 0x19);
    f.engine.write32(segment + 4, 72 + 80);
    f.engine.write_bytes(segment + 8, std::vector<std::uint8_t>{'_', '_', 'T', 'E', 'X', 'T'});
    f.engine.write32(segment + 64, 1);
    const auto section = segment + 72;
    f.engine.write_bytes(section, std::vector<std::uint8_t>{'_', '_', 't', 'e', 'x', 't'});
    f.engine.write64(section + 32, 0x100004000);
    f.engine.write64(section + 40, 0x1234);
    require(f.call(sys, "_getsectbynamefromheader_64", {header, f.text("__TEXT"), f.text("__text")}) == section, "getsectbynamefromheader_64");
    const auto size = f.block(8);
    require(f.call(sys, "_getsectdatafromheader_64", {header, f.text("__TEXT"), f.text("__text"), size}) == 0x100004000 &&
            f.engine.read64(size) == 0x1234, "getsectdatafromheader_64");
    require(f.call(sys, "_getsectbynamefromheader_64", {header, f.text("__DATA"), f.text("__data")}) == 0, "missing section");
    // ilogbf (float in s0).
    alchemy::CallRegs registers{};
    registers.set_s(0, 8.0f);
    require(f.regs(sys, "_ilogbf", registers).x0 == 3, "ilogbf");
    // Frame records: {previous fp, lr}; PAC bits stripped.
    const auto frame = f.block(16);
    f.engine.write64(frame, 0x1000);
    f.engine.write64(frame + 8, 0x8012000100002000ULL);
    const auto return_address = f.block(8);
    require(f.call(sys, "_pthread_stack_frame_decode_np", {frame, return_address}) == 0x1000 &&
            f.engine.read64(return_address) == 0x0000000100002000ULL, "pthread_stack_frame_decode_np");
    const auto attribute = f.block(16), protocol = f.block(4, 0xff);
    require(f.call(sys, "_pthread_mutexattr_setprotocol", {attribute, 1}) == 0 &&
            f.call(sys, "_pthread_mutexattr_getprotocol", {attribute, protocol}) == 0 && f.engine.read32(protocol) == 1,
            "PTHREAD_PRIO_INHERIT is a stored attribute");
    require(f.call(sys, "_pthread_mutexattr_setprotocol", {attribute, 3}) == 22, "unknown protocol");
    require(f.memory().read8(f.symbol(sys, "_in6addr_loopback") + 15) == 1, "in6addr_loopback");
    // Groups.
    const auto own = f.call(sys, "_getgrgid", {f.call(sys, "_getgid")});
    require(own && !f.read_text(f.engine.read64(own)).empty(), "getgrgid(own gid)");
    const auto wheel = f.call(sys, "_getgrnam", {f.text("wheel")});
    require(wheel && f.engine.read32(wheel + 16) == 0, "getgrnam(wheel)");
    require(f.call(sys, "_getgrnam", {f.text("no-such-group")}) == 0, "unknown group");
    // getgrent walks the user's group, then wheel; setgrent rewinds.
    static_cast<void>(f.call(sys, "_setgrent"));
    require(f.call(sys, "_getgrent") == own && f.call(sys, "_getgrent") == wheel && f.call(sys, "_getgrent") == 0, "getgrent");
    static_cast<void>(f.call(sys, "_endgrent"));
    require(f.call(sys, "_getgrent") == own && f.call(sys, "_setgroupent", {1}) == 1 && f.call(sys, "_getgrent") == own, "endgrent/setgroupent rewind");
    // termios on a descriptor that is not a terminal.
    const auto pipe = f.block(8);
    require(f.status(sys, "_pipe", {pipe}) == 0, "pipe");
    const auto reader = f.engine.read32(pipe);
    require(f.status(sys, "_tcflow", {reader, 1}) == -1 && f.status(sys, "_tcsendbreak", {reader, 0}) == -1 &&
            f.status(sys, "_tcgetsid", {reader}) == -1, "tcflow/tcsendbreak/tcgetsid need a terminal");
    static_cast<void>(f.call(sys, "_close", {reader}));
    static_cast<void>(f.call(sys, "_close", {f.engine.read32(pipe + 4)}));
    // Guarded descriptors.
    const auto guard = f.block(8);
    f.engine.write64(guard, 0xfeed);
    require(f.status(sys, "_change_fdguard_np", {77, 0, 0, guard, 1, 0}) == 0, "guard set");
    const auto wrong = f.block(8);
    f.engine.write64(wrong, 0xbad);
    require(f.status(sys, "_guarded_close_np", {77, wrong}) == -1, "guard mismatch refuses close");
}

void unfair_lock(Fixture& f) {
    const auto lock = f.block(4);
    static_cast<void>(f.call(sys, "_os_unfair_lock_lock", {lock}));
    require(f.call(sys, "_os_unfair_lock_trylock", {lock}) == 0, "held lock refuses trylock");
    static_cast<void>(f.call(sys, "_os_unfair_lock_unlock", {lock}));
    require(f.call(sys, "_os_unfair_lock_trylock", {lock}) == 1, "free lock trylock");
    static_cast<void>(f.call(sys, "_os_unfair_lock_unlock", {lock}));
    bool refused = false;
    try { static_cast<void>(f.call(sys, "_os_unfair_lock_unlock", {lock})); } catch (const std::exception&) { refused = true; }
    require(refused, "unlocking an unowned os_unfair_lock is fatal");
    const auto self = f.call(sys, "_pthread_self");
    const auto port = f.call(sys, "_pthread_mach_thread_np", {self});
    require(f.call(sys, "_pthread_from_mach_thread_np", {port}) == self, "pthread_from_mach_thread_np");
    require(f.call(sys, "_pthread_from_mach_thread_np", {0x7fff}) == 0, "unknown thread port");
}

void terminfo(Fixture& f) {
    // No terminfo database: setupterm fails with *errret = -1, nothing is current.
    const auto errret = f.block(4, 0x55);
    require(f.status(ncurses, "_setupterm", {f.text("xterm-256color"), 1, errret}) == -1 && static_cast<std::int32_t>(f.engine.read32(errret)) == -1,
            "setupterm reports no database");
    require(static_cast<std::int32_t>(f.call(ncurses, "_tigetnum", {f.text("colors")})) == -2, "tigetnum without a terminal");
    require(f.call(ncurses, "_set_curterm", {0x1000}) == 0 && f.call(ncurses, "_set_curterm", {0}) == 0x1000, "set_curterm swaps cur_term");
    require(f.status(ncurses, "_del_curterm", {0}) == -1, "del_curterm(NULL)");
}

} // namespace

int main() {
    return run("machsys", {{"imports", imports}, {"mach", mach}, {"tasks", tasks}, {"jit", jit}, {"vm", vm}, {"bsm", bsm}, {"launchd", launchd}, {"sandbox", sandbox}, {"libc", libc},
                           {"os_unfair_lock", unfair_lock}, {"terminfo", terminfo}});
}
