// The libSystem pieces first written for a renderer plug-in (net/, process/ and misc/libsystem_*.cpp):
// pure helpers and the guest-visible exports (split from the export-fill tests).
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

#include <array>
#include <atomic>
#include <thread>

namespace {

void test_pure() {
    require(vd::test_base_name("/usr/lib/") == "lib" && vd::test_base_name("") == "." && vd::test_base_name("/") == "/" &&
            vd::test_base_name("a") == "a", "basename");
    require(vd::test_directory_name("/usr/lib") == "/usr" && vd::test_directory_name("/usr") == "/" &&
            vd::test_directory_name("a") == "." && vd::test_directory_name("a/b//") == "a" && vd::test_directory_name("//") == "/",
            "dirname");
    require(vd::test_floattidf(0, 1) == 18446744073709551616.0, "2^64");
    require(vd::test_floattidf(~0ULL, ~0ULL) == -1.0, "-1");
    require(vd::test_floattidf(0, 0x8000000000000000ULL) == -std::ldexp(1.0, 127), "INT128_MIN");
    // 2^64 + 2^11 + 1 rounds up (above the halfway point) to 2^64 + 2^12.
    require(vd::test_floattidf((1ULL << 11) + 1, 1) == std::ldexp(1.0, 64) + std::ldexp(1.0, 12), "round up past half");
    require(vd::test_floattidf(1ULL << 11, 1) == std::ldexp(1.0, 64), "ties to even");
    const auto q = vd::test_udivmod(5, 7, 3, 0, false), r = vd::test_udivmod(5, 7, 3, 0, true);
    // (7 * 2^64 + 5) = 3 * q + r
    require(r.first == 0 && r.second == 0, "umodti3");
    require(q.first == 6148914691236517207ULL, "udivti3 low word");
    require(q.second == 2, "udivti3 high word");
}

void test_guest_libsystem(Fixture& f) {
    // __NSGetArgc / __NSGetArgv
    const auto argc = f.sys("__NSGetArgc"), argv = f.sys("__NSGetArgv");
    require(f.engine.read32(argc) == 2 && f.read(f.engine.read64(f.engine.read64(argv) + 8)) == "-version", "_NSGetArgv");
    // argv, NULL, envp (one entry), NULL, apple, NULL: programs walk past envp to the apple strings.
    {
        const auto table = f.engine.read64(argv);
        require(f.engine.read64(table + 16) == 0 && f.read(f.engine.read64(table + 24)) == "ALCHEMY_TEST=1" && f.engine.read64(table + 32) == 0 &&
                f.read(f.engine.read64(table + 40)).starts_with("executable_path=") && f.read(f.engine.read64(table + 40)).ends_with("/tool.bin") &&
                f.engine.read64(table + 48) == 0, "_NSGetArgv vector layout");
    }
    // getifaddrs: a well-formed list containing lo0 with 127.0.0.1.
    const auto list = f.process.allocate_guest(8);
    require(f.sys("_getifaddrs", {list}) == 0, "getifaddrs");
    bool loopback = false;
    int count = 0;
    for (auto entry = f.engine.read64(list); entry; entry = f.engine.read64(entry)) {
        ++count;
        const auto name = f.read(f.engine.read64(entry + 8));
        const auto address = f.engine.read64(entry + 24);
        if (name == "lo0" && address && f.byte(address + 1) == 2 && f.engine.read32(address + 4) == 0x0100007fU) loopback = true;
        require(count < 10000, "getifaddrs list does not terminate");
    }
    require(loopback, "getifaddrs lacks lo0 127.0.0.1");
    f.sys("_freeifaddrs", {f.engine.read64(list)});
    // Named semaphores.
    const auto name = f.text("/alchemy.export_fill.test");
    static_cast<void>(f.sys("_sem_unlink", {name}));
    // sem_open is variadic: mode and value are passed in 8-byte stack slots.
    alchemy::CallRegs open;
    open.x[0] = name; open.x[1] = 0x200 | 0x800;
    open.push_variadic(std::uint64_t{0600});
    open.push_variadic(std::uint64_t{1});
    const auto semaphore = alchemy::engine_call_regs(f.engine, f.symbol("/usr/lib/libSystem.B.dylib", "_sem_open"), open).x0;
    require(semaphore != UINT64_MAX && semaphore != 0, "sem_open O_CREAT");
    require(f.sys("_sem_trywait", {semaphore}) == 0, "sem_trywait with value 1");
    require(f.sys("_sem_trywait", {semaphore}) == UINT64_MAX, "sem_trywait at zero must fail (EAGAIN)");
    require(f.sys("_sem_post", {semaphore}) == 0 && f.sys("_sem_wait", {semaphore}) == 0, "sem_post/sem_wait");
    require(f.sys("_sem_close", {semaphore}) == 0, "sem_close");
    // OSAtomic.
    const auto word = f.process.allocate_guest(8);
    f.engine.write64(word, 40);
    require(static_cast<std::int32_t>(f.sys("_OSAtomicAdd32Barrier", {2, word})) == 42, "OSAtomicAdd32Barrier");
    require(f.sys("_OSAtomicCompareAndSwap64Barrier", {42, 7, word}) == 1 && f.engine.read64(word) == 7, "OSAtomicCompareAndSwap64Barrier");
    require(f.sys("_OSAtomicCompareAndSwap64Barrier", {42, 9, word}) == 0, "CAS with a stale value");
    f.engine.write32(word, 0x0f0);
    require(f.sys("_OSAtomicOr32Barrier", {0x101, word}) == 0x1f1 && f.engine.read32(word) == 0x1f1, "OSAtomicOr32Barrier returns the new value");
    require(f.sys("_OSAtomicAnd32OrigBarrier", {0x0ff, word}) == 0x1f1 && f.engine.read32(word) == 0x0f1, "OSAtomicAnd32OrigBarrier returns the old value");
    require(f.sys("_OSAtomicXor32", {0x001, word}) == 0x0f0 && f.sys("_OSAtomicOr32Orig", {0x100, word}) == 0x0f0 &&
            f.engine.read32(word) == 0x1f0, "OSAtomicXor32 / OSAtomicOr32Orig");
    // 128-bit division through the guest ABI.
    require(f.sys("___udivti3", {10, 1, 2, 0}) == 0x8000000000000005ULL, "__udivti3");
    // strptime.
    const auto tm = f.process.allocate_guest(56);
    f.engine.fill_bytes(tm, 0, 56);
    const auto date = f.text("Fri, 03 Oct 2026 14:05:09 GMT");
    const auto end = f.sys("_strptime", {date, f.text("%a, %d %b %Y %H:%M:%S %Z"), tm});
    require(end == date + 29 && f.engine.read32(tm + 20) == 126 && f.engine.read32(tm + 16) == 9 && f.engine.read32(tm + 12) == 3 &&
            f.engine.read32(tm + 8) == 14 && f.engine.read32(tm) == 9 && f.engine.read32(tm + 24) == 5, "strptime fields");
    // host_info(HOST_BASIC_INFO).
    const auto info = f.process.allocate_guest(64), info_count = f.process.allocate_guest(4);
    f.engine.write32(info_count, 12);
    require(f.sys("_host_info", {f.sys("_mach_host_self"), 1, info, info_count}) == 0 && f.engine.read32(info) >= 1 &&
            f.engine.read32(info + 12) == 0x0100000cU, "host_info");
    // inet_net_pton.
    const auto net = f.process.allocate_guest(16);
    require(f.sys("_inet_net_pton", {2, f.text("10.1.0.0/16"), net, 16}) == 16 && f.byte(net) == 10 && f.byte(net + 1) == 1,
            "inet_net_pton");
    // proc_pidpath for this process.
    const auto path = f.process.allocate_guest(4096);
    const auto path_length = f.sys("_proc_pidpath", {f.sys("_getpid"), path, 4096});
    require(path_length > 0 && f.read(path).size() == path_length && f.read(path).ends_with("/tool.bin"), "proc_pidpath");
    require(f.sys("_proc_pidpath", {f.sys("_getpid"), path, 4}) == 0, "proc_pidpath with a short buffer must fail");
    // vm_page_size, in6addr_any.
    require(f.engine.read64(f.symbol("/usr/lib/libSystem.B.dylib", "_vm_page_size")) == 16384, "vm_page_size");
    // SysV IPC is explicitly unavailable.
    require(f.sys("_shmget", {1, 4096, 0}) == UINT64_MAX, "shmget must fail");
    // System V semaphores.
    {
        const auto sem = [&](std::uint64_t id, std::uint16_t number, std::int16_t op, std::int16_t flags) {
            const auto buffer = f.process.allocate_guest(8);
            f.engine.write_bytes(buffer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(number >> 8),
                static_cast<std::uint8_t>(op), static_cast<std::uint8_t>(static_cast<std::uint16_t>(op) >> 8),
                static_cast<std::uint8_t>(flags), static_cast<std::uint8_t>(static_cast<std::uint16_t>(flags) >> 8)});
            return f.sys("_semop", {id, buffer, 1});
        };
        const auto semctl = [&](std::uint64_t id, std::uint64_t number, std::uint64_t command, std::uint64_t argument) {
            alchemy::CallRegs registers;
            registers.x[0] = id; registers.x[1] = number; registers.x[2] = command;
            registers.push_variadic(argument);
            return alchemy::engine_call_regs(f.engine, f.symbol("/usr/lib/libSystem.B.dylib", "_semctl"), registers).x0;
        };
        const auto id = f.sys("_semget", {0, 2, 0x200 | 0600});
        require(id != UINT64_MAX, "semget IPC_PRIVATE");
        require(semctl(id, 0, 8, 1) == 0 && semctl(id, 0, 5, 0) == 1, "SETVAL/GETVAL");
        require(sem(id, 0, -1, 0x1000) == 0 && semctl(id, 0, 5, 0) == 0, "semop decrement with SEM_UNDO");
        require(sem(id, 0, -1, 0x800) == UINT64_MAX, "semop IPC_NOWAIT on zero must fail (EAGAIN)");
        require(sem(id, 1, 0, 0) == 0, "wait-for-zero on a zero semaphore");
        require(sem(id, 0, 1, 0x1000) == 0 && semctl(id, 0, 5, 0) == 1, "semop increment");
        require(sem(id, 5, 1, 0) == UINT64_MAX, "semaphore number out of range (EFBIG)");
        const auto key = 0x544f4f4c;   // 'TOOL'
        const auto keyed = f.sys("_semget", {key, 1, 0x200 | 0x400 | 0600});
        require(keyed != UINT64_MAX && f.sys("_semget", {key, 1, 0x200 | 0x400 | 0600}) == UINT64_MAX && f.sys("_semget", {key, 1, 0}) == keyed,
                "semget by key");
        require(semctl(keyed, 0, 0, 0) == 0 && f.sys("_semget", {key, 1, 0}) == UINT64_MAX, "IPC_RMID");
        require(semctl(id, 0, 0, 0) == 0 && sem(id, 0, 1, 0) == UINT64_MAX, "operations on a removed set fail");
    }
}

void test_guest_misc(Fixture& f) {
    const auto system = "/usr/lib/libSystem.B.dylib";
    // difftime / remainder return doubles in d0.
    {
        alchemy::CallRegs args;
        args.x[0] = 1000;
        args.x[1] = 1500;
        const auto result = alchemy::engine_call_regs(f.engine, f.symbol(system, "_difftime"), args);
        require(!result.complete || result.d(0) == -500.0, "difftime");
        alchemy::CallRegs pair;
        pair.set_d(0, 7.0);
        pair.set_d(1, 2.0);
        if (result.complete) {
            const auto r = alchemy::engine_call_regs(f.engine, f.symbol(system, "_remainder"), pair);
            require(r.d(0) == -1.0, "remainder(7, 2) is -1 (ties to even)");
        }
    }
    require(f.sys("_pthread_main_np") == 1, "pthread_main_np on the main thread");
    // Attribute scheduling fields.
    const auto attr = f.process.allocate_guest(64), param = f.process.allocate_guest(8), out = f.process.allocate_guest(8);
    require(f.sys("_pthread_attr_init", {attr}) == 0, "pthread_attr_init");
    require(f.sys("_pthread_attr_getschedpolicy", {attr, out}) == 0 && f.engine.read32(out) == 1, "default policy SCHED_OTHER");
    require(f.sys("_pthread_attr_setschedpolicy", {attr, 4}) == 45, "SCHED_FIFO is ENOTSUP");
    f.engine.write32(param, 40);
    require(f.sys("_pthread_attr_setschedparam", {attr, param}) == 0, "setschedparam");
    require(f.sys("_pthread_attr_getschedparam", {attr, out}) == 0 && f.engine.read32(out) == 40, "getschedparam");
    require(f.sys("_pthread_attr_setinheritsched", {attr, 2}) == 0 && f.sys("_pthread_attr_setinheritsched", {attr, 7}) == 22,
            "setinheritsched");
    require(f.sys("_pthread_attr_destroy", {attr}) == 0, "pthread_attr_destroy");
    // A relative timed wait times out (ETIMEDOUT 60).
    {
        const auto mutex = f.process.allocate_guest(64), cond = f.process.allocate_guest(48), wait = f.process.allocate_guest(16);
        require(f.sys("_pthread_mutex_init", {mutex, 0}) == 0 && f.sys("_pthread_cond_init", {cond, 0}) == 0, "mutex/cond init");
        require(f.sys("_pthread_mutex_lock", {mutex}) == 0, "mutex lock");
        f.engine.write64(wait, 0);
        f.engine.write64(wait + 8, 20'000'000);
        require(f.sys("_pthread_cond_timedwait_relative_np", {cond, mutex, wait}) == 60, "relative wait times out");
        require(f.sys("_pthread_mutex_unlock", {mutex}) == 0, "mutex unlock");
    }
    // A known mutex is not validated again on each lock and unlock (each check was a VirtualQuery under
    // alchemy-fx); a new address still is.
    {
        const auto page = f.sys("_mmap", {0, 4096, 3, 0x1002, UINT64_MAX, 0});   // PROT_READ|WRITE, MAP_PRIVATE|ANON
        require(page != UINT64_MAX, "mmap a page");
        require(f.sys("_pthread_mutex_init", {page, 0}) == 0 && f.sys("_pthread_mutex_lock", {page}) == 0, "lock in the page");
        require(f.sys("_mprotect", {page, 4096, 0}) == 0, "mprotect PROT_NONE");
        require(f.sys("_pthread_mutex_unlock", {page}) == 0 && f.sys("_pthread_mutex_lock", {page}) == 0 &&
                f.sys("_pthread_mutex_unlock", {page}) == 0, "the known mutex works without another check");
        require(f.sys("_pthread_mutex_lock", {page + 64}) == 22, "a new address in an inaccessible page: EINVAL");
        require(f.sys("_munmap", {page, 4096}) == 0, "munmap");
    }
    // proc_name: the executable's file name.
    {
        const auto name = f.process.allocate_guest(64);
        require(f.sys("_proc_name", {f.sys("_getpid"), name, 64}) == 8 && f.read(name) == "tool.bin", "proc_name");
    }
    // ftok of an existing file is stable and carries the id in the top byte.
    {
        const auto file = std::filesystem::temp_directory_path() / "alchemy_export_fill_ftok.txt";
        { std::FILE* handle = std::fopen(file.string().c_str(), "wb"); if (handle) std::fclose(handle); }
        const auto path = f.text(alchemy::guest_path_string(file));
        const auto key = f.sys("_ftok", {path, 0x41});
        require(key != UINT64_MAX && (key >> 24) == 0x41 && key == f.sys("_ftok", {path, 0x41}), "ftok");
        require(f.sys("_ftok", {f.text("/nonexistent/alchemy/ftok"), 1}) == UINT64_MAX, "ftok of a missing file");
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
    // getgrgid_r for the virtual identity's group.
    {
        const auto group = f.process.allocate_guest(32), buffer = f.process.allocate_guest(128), result = f.process.allocate_guest(8);
        require(f.sys("_getgrgid_r", {1000, group, buffer, 128, result}) == 0 && f.engine.read64(result) == group &&
                f.read(f.engine.read64(group)) == "alchemy" && f.engine.read32(group + 16) == 1000, "getgrgid_r 1000");
        require(f.sys("_getgrgid_r", {0, group, buffer, 128, result}) == 0 && f.engine.read64(result) == 0, "getgrgid_r unknown gid");
        require(f.sys("_getgrgid_r", {1000, group, buffer, 4, result}) == 34, "getgrgid_r ERANGE");
    }
    require(f.sys("_waitid", {0, 0, 0, 4}) == UINT64_MAX, "waitid has no children");
    // mlock / munlock on guest memory.
    {
        const auto block = f.process.allocate_guest(4096, 4096);
        require(f.sys("_mlock", {block, 4096}) == 0 && f.sys("_munlock", {block, 4096}) == 0, "mlock/munlock");
    }
    // vm_map of anonymous memory, then vm_deallocate.
    {
        const std::uint64_t task = f.engine.read32(f.symbol(system, "_mach_task_self_"));
        const auto address = f.process.allocate_guest(8);
        f.engine.write64(address, 0);
        alchemy::CallRegs map;
        map.x = {task, address, 65536, 0, 1, 0, 0, 0};
        map.push_stack(std::int32_t{3});
        map.push_stack(std::int32_t{7});
        map.push_stack(std::uint32_t{1});
        // Anonymous memory: libsystem_misc.cpp's internal vm_map, behind mach_exports.cpp's export.
        const auto result = alchemy::engine_call_regs(f.engine, f.symbol("alchemy.internal", "__alchemy_vm_map_anonymous"), map);
        const auto base = f.engine.read64(address);
        require(result.x0 == 0 && base != 0, "vm_map anonymous");
        f.engine.write64(base + 65528, 42);
        require(f.engine.read64(base + 65528) == 42, "vm_map memory is writable");
        require(f.sys("_vm_deallocate", {task, base, 65536}) == 0, "vm_deallocate");
    }
    // dispatch_once runs the block's invoke exactly once.
    {
        static int runs = 0;
        runs = 0;
        const auto invoke = f.process.install_function("_export_fill_test_once", [](transmute::CpuState& s, transmute::GuestMemory&) {
            ++runs;
            s.x[0] = 0;
        }, "/alchemy/test/export_fill");
        const auto block = f.process.allocate_guest(32), predicate = f.process.allocate_guest(8);
        f.engine.fill_bytes(block, 0, 32);
        f.engine.write64(block + 16, invoke);
        f.engine.write64(predicate, 0);
        f.sys("_dispatch_once", {predicate, block});
        f.sys("_dispatch_once", {predicate, block});
        require(runs == 1, "dispatch_once ran the block more than once");
    }
    // host_processor_info / thread_info.
    {
        const auto count = f.process.allocate_guest(4), info = f.process.allocate_guest(8), info_count = f.process.allocate_guest(4);
        require(f.sys("_host_processor_info", {f.sys("_mach_host_self"), 2, count, info, info_count}) == 0, "host_processor_info");
        const auto cpus = f.engine.read32(count);
        require(cpus >= 1 && f.engine.read32(info_count) == cpus * 4, "processor count");
        require(f.engine.read32(f.engine.read64(info) + 8) > 0, "idle ticks");
        static_cast<void>(f.sys("_vm_deallocate", {f.engine.read32(f.symbol(system, "_mach_task_self_")), f.engine.read64(info), cpus * 16ULL}));
        const auto basic = f.process.allocate_guest(64), basic_count = f.process.allocate_guest(4);
        f.engine.write32(basic_count, 10);
        require(f.sys("_thread_info", {f.sys("_mach_thread_self"), 3, basic, basic_count}) == 0 && f.engine.read32(basic_count) == 10,
                "thread_info THREAD_BASIC_INFO");
        require(f.sys("_thread_info", {f.sys("_mach_thread_self") + 1000, 3, basic, basic_count}) == 4, "thread_info of another thread");
    }
    // os_activity objects. os_log objects and os_retain/os_release are
    // misc/oslog.cpp's (QLab runtime tests); these activities are released
    // through the internal retain/release it forwards to.
    {
        const auto activity = f.sys("__os_activity_create", {0, f.text("activity"), 0, 0});
        const auto state = f.process.allocate_guest(16);
        f.sys("_os_activity_scope_enter", {activity, state});
        f.sys("_os_activity_scope_leave", {state});
        const auto retain = f.symbol("alchemy.internal", "__alchemy_os_activity_retain");
        const auto release = f.symbol("alchemy.internal", "__alchemy_os_activity_release");
        const std::array<std::uint64_t, 1> argument{activity};
        require(f.engine.invoke_callback(retain, argument) == activity, "os_activity retain");
        static_cast<void>(f.engine.invoke_callback(release, argument));
        static_cast<void>(f.engine.invoke_callback(release, argument));
    }
    // Activities nest per thread: identifiers, parents, OS_ACTIVITY_CURRENT and the active list.
    {
        const auto current = f.symbol("/usr/lib/libSystem.B.dylib", "__os_activity_current");
        const auto outer = f.sys("__os_activity_create", {0, f.text("outer"), 0, 0});
        const auto outer_state = f.process.allocate_guest(16), inner_state = f.process.allocate_guest(16);
        f.sys("_os_activity_scope_enter", {outer, outer_state});
        const auto inner = f.sys("__os_activity_create", {0, f.text("inner"), current, 0});
        f.sys("_os_activity_scope_enter", {inner, inner_state});
        const auto parent = f.process.allocate_guest(8);
        const auto inner_id = f.sys("_os_activity_get_identifier", {inner, parent});
        const auto outer_id = f.sys("_os_activity_get_identifier", {outer, 0});
        require(inner_id && outer_id && inner_id != outer_id && f.engine.read64(parent) == outer_id, "an activity's parent is the current one");
        require(f.sys("_os_activity_get_identifier", {current, 0}) == inner_id, "OS_ACTIVITY_CURRENT is the innermost");
        const auto ids = f.process.allocate_guest(16), count = f.process.allocate_guest(4);
        f.engine.write32(count, 2);
        require(f.sys("_os_activity_get_active", {ids, count}) == 2 && f.engine.read64(ids) == outer_id && f.engine.read64(ids + 8) == inner_id,
                "os_activity_get_active, outermost first");
        f.sys("_os_activity_scope_leave", {inner_state});
        f.sys("_os_activity_scope_leave", {outer_state});
        require(f.sys("_os_activity_get_identifier", {current, 0}) == 0, "no activity after leaving");
        require(f.sys("_os_activity_get_identifier", {f.symbol("/usr/lib/libSystem.B.dylib", "__os_activity_none"), 0}) == 0, "OS_ACTIVITY_NONE");
        // _os_activity_initiate_f runs the function inside a new activity.
        static Address seen{};
        static Fixture* fixture = &f;
        const auto function = f.process.install_function("_gaps_activity_function", [](transmute::CpuState& s, transmute::GuestMemory&) {
            seen = fixture->sys("_os_activity_get_identifier", {fixture->symbol("/usr/lib/libSystem.B.dylib", "__os_activity_current"), 0});
            s.x[0] = 0;
        });
        f.sys("__os_activity_initiate_f", {0, f.text("initiated"), 0, 0, function});
        require(seen != 0 && seen != outer_id && seen != inner_id, "initiate runs inside a new activity");
    }
}

// compiler-rt builtins (misc/libsystem_runtime.cpp): 128-bit operands in x0:x1 / x2:x3,
// floating-point ones in v registers.
void test_compiler_rt(Fixture& f) {
    const auto call = [&f](std::string_view name, std::initializer_list<Address> x, std::initializer_list<alchemy::VectorRegister> v = {}) {
        alchemy::CallRegs registers;
        std::size_t index = 0;
        for (const auto value : x) registers.x[index++] = value;
        index = 0;
        for (const auto value : v) registers.v[index++] = value;
        return alchemy::engine_call_regs(f.engine, f.symbol("/usr/lib/libSystem.B.dylib", name), registers);
    };
    const auto pair = [](const alchemy::CallResult& r, Address low, Address high) { return r.x0 == low && r.x1 == high; };
    require(pair(call("___multi3", {3, 1, 5, 0}), 15, 5), "__multi3");
    require(pair(call("___multi3", {~0ULL, 0, ~0ULL, 0}), 1, 0xfffffffffffffffeULL), "__multi3 carries into the high word");
    const auto remainder = f.process.allocate_guest(16);
    require(pair(call("___udivmodti4", {1, 1, 2, 0, remainder}), 0x8000000000000000ULL, 0) && f.engine.read64(remainder) == 1 &&
            f.engine.read64(remainder + 8) == 0, "__udivmodti4 quotient and remainder");
    require(pair(call("___ashlti3", {1, 0, 64}), 0, 1), "__ashlti3");
    require(pair(call("___ashrti3", {0, 0x8000000000000000ULL, 127}), ~0ULL, ~0ULL), "__ashrti3 keeps the sign");
    require(pair(call("___lshrti3", {0, 0x8000000000000000ULL, 127}), 1, 0), "__lshrti3");
    require(pair(call("___negti2", {1, 0}), ~0ULL, ~0ULL), "__negti2");
    require(f.sys("___clzti2", {0, 1}) == 63 && f.sys("___ctzti2", {0, 1}) == 64 && f.sys("___popcountti2", {~0ULL, 1}) == 65,
            "128-bit bit counts");
    require(f.sys("___ffsti2", {0, 0}) == 0 && f.sys("___ffsti2", {0, 2}) == 66, "__ffsti2");
    require(f.sys("___cmpti2", {~0ULL, ~0ULL, 1, 0}) == 0 && f.sys("___ucmpti2", {~0ULL, ~0ULL, 1, 0}) == 2 &&
            f.sys("___cmpti2", {5, 0, 5, 0}) == 1, "__cmpti2 / __ucmpti2");
    const auto overflow = f.process.allocate_guest(4);
    f.sys("___mulodi4", {1ULL << 62, 4, overflow});
    require(f.engine.read32(overflow) == 1, "__mulodi4 reports overflow");
    require(f.sys("___mulodi4", {static_cast<Address>(-3), 7, overflow}) == static_cast<Address>(-21) && f.engine.read32(overflow) == 0,
            "__mulodi4 without overflow");
    require(pair(call("___muloti4", {1ULL << 63, 0, 2, 0, overflow}), 0, 1) && f.engine.read32(overflow) == 0, "__muloti4 2^64");
    call("___muloti4", {0, 1ULL << 62, 4, 0, overflow});
    require(f.engine.read32(overflow) == 1, "__muloti4 reports overflow");
    require(f.sys("___divdi3", {static_cast<Address>(-7), 2}) == static_cast<Address>(-3) && f.sys("___udivdi3", {7, 2}) == 3, "__divdi3 / __udivdi3");
    require(f.sys("___bswapsi2", {0x11223344}) == 0x44332211 && f.sys("___bswapdi2", {0x0102030405060708ULL}) == 0x0807060504030201ULL,
            "byte swaps");
    require(f.sys("___paritysi2", {7}) == 1 && f.sys("___paritysi2", {3}) == 0, "__paritysi2");
    const auto real = [](double value) { return alchemy::VectorRegister{std::bit_cast<std::uint64_t>(value), 0}; };
    const auto single = [](float value) { return alchemy::VectorRegister{std::bit_cast<std::uint32_t>(value), 0}; };
    require(pair(call("___fixdfti", {}, {real(-2.5)}), 0xfffffffffffffffeULL, ~0ULL), "__fixdfti truncates");
    require(pair(call("___fixunsdfti", {}, {real(std::ldexp(1.0, 70))}), 0, 64), "__fixunsdfti 2^70");
    require(call("___floattisf", {0, 1}).s(0) == 18446744073709551616.0f, "__floattisf 2^64");
    require(call("___extendhfsf2", {}, {{0x3c00, 0}}).s(0) == 1.0f && call("___gnu_h2f_ieee", {0xc000}).s(0) == -2.0f, "half to float");
    require(call("___truncsfhf2", {}, {single(1.0f / 3.0f)}).v[0][0] == 0x3555 && call("___truncdfhf2", {}, {real(0.5)}).v[0][0] == 0x3800 &&
            call("___gnu_f2h_ieee", {}, {single(-2.0f)}).x0 == 0xc000, "float to half");
    const auto product = call("___muldc3", {}, {real(1), real(2), real(3), real(4)});
    require(product.d(0) == -5.0 && product.d(1) == 10.0, "__muldc3");
    const auto quotient = call("___divdc3", {}, {real(-5), real(10), real(3), real(4)});
    require(std::fabs(quotient.d(0) - 1.0) < 1e-12 && std::fabs(quotient.d(1) - 2.0) < 1e-12, "__divdc3");
    require(f.sys("___isOSVersionAtLeast", {11, 0, 0}) == 1 && f.sys("___isOSVersionAtLeast", {999, 0, 0}) == 0, "__isOSVersionAtLeast");
}

// Blocks introspection (misc/libsystem_extra.cpp).
void test_blocks(Fixture& f) {
    constexpr std::uint32_t has_signature = 1U << 30, extended = 1U << 31, is_global = 1U << 28;
    const auto signature = f.text("v8@?0");
    const auto descriptor = f.process.allocate_guest(32), block = f.process.allocate_guest(32);
    f.engine.write64(descriptor, 0); f.engine.write64(descriptor + 8, 32);
    f.engine.write64(descriptor + 16, signature); f.engine.write64(descriptor + 24, 0x1234);
    f.engine.write64(block, 0); f.engine.write32(block + 8, has_signature); f.engine.write32(block + 12, 0);
    f.engine.write64(block + 16, 0); f.engine.write64(block + 24, descriptor);
    require(f.sys("__Block_has_signature", {block}) == 1 && f.sys("__Block_signature", {block}) == signature, "_Block_signature");
    require(f.sys("__Block_size", {block}) == 32 && f.sys("_Block_size", {block}) == 32, "_Block_size");
    require(f.sys("__Block_layout", {block}) == 0x1234 && f.sys("__Block_extended_layout", {block}) == 0, "the old layout");
    f.engine.write32(block + 8, has_signature | extended);
    require(f.sys("__Block_layout", {block}) == 0 && f.sys("__Block_extended_layout", {block}) == 0x1234, "the extended layout");
    f.engine.write32(block + 8, 0);
    require(f.sys("__Block_has_signature", {block}) == 0 && f.sys("__Block_signature", {block}) == 0, "no signature");
    const auto copy = f.sys("__Block_copy", {block});
    const auto references = f.engine.read32(copy + 8) & 0xfffe;
    require(copy && copy != block && f.sys("__Block_tryRetain", {copy}) == 1 && (f.engine.read32(copy + 8) & 0xfffe) == references + 2,
            "_Block_tryRetain adds a reference to a heap block");
    f.sys("__Block_release", {copy});
    f.sys("__Block_release", {copy});
    f.engine.write32(block + 8, is_global);
    require(f.sys("__Block_tryRetain", {block}) == 1 && f.engine.read32(block + 8) == is_global, "global blocks are not counted");
    f.sys("__Block_use_RR2", {0});
}

// random(3) with initstate/setstate (misc/libsystem_runtime.cpp).
void test_random_states(Fixture& f) {
    f.sys("_srandom", {1});
    require(f.sys("_random") == 1804289383, "srandom(1) gives the BSD/glibc sequence");
    const auto big = f.process.allocate_guest(256), other = f.process.allocate_guest(256), tiny = f.process.allocate_guest(16);
    const auto own = f.sys("_initstate", {9, big, 256});
    require(own != 0, "initstate returns the previous array");
    const auto first = f.sys("_random"), second = f.sys("_random");
    require(f.sys("_initstate", {9, other, 256}) == big && f.sys("_random") == first && f.sys("_random") == second,
            "the same seed and size repeat the sequence");
    require(f.sys("_setstate", {big}) == other && f.sys("_random") != 0, "setstate switches back");
    require(f.sys("_initstate", {5, tiny, 16}) == big && f.sys("_random") == 84035, "a 16-byte array is the Park-Miller generator");
    require(f.sys("_setstate", {own}) == tiny, "back to the process's own array");
    require(f.sys("_initstate", {1, tiny, 4}) == 0 && f.sys("_setstate", {f.process.allocate_guest(8)}) == 0, "bad arrays fail (EINVAL)");
}

// sigsuspend waits for a handler to run (process/process_model.cpp).
void test_sigsuspend(Fixture& f) {
    static std::atomic<int> handled{0};
    static std::atomic<bool> woke{false};
    static Fixture* fixture = &f;
    const auto handler = f.process.install_function("_bsd_test_usr1", [](transmute::CpuState& s, transmute::GuestMemory&) { ++handled; s.x[0] = 0; });
    const auto raiser = f.process.install_function("_bsd_test_raiser", [](transmute::CpuState& s, transmute::GuestMemory&) {
        const auto raise = fixture->symbol("/usr/lib/libSystem.B.dylib", "_raise");
        const std::array<Address, 1> arguments{30};
        for (int round = 0; round < 200 && !woke; ++round) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            static_cast<void>(alchemy::EngineAdapter(fixture->process.calling_engine()).call(raise, arguments));
        }
        s.x[0] = 0;
    });
    f.sys("_signal", {30, handler});
    const auto thread = f.process.allocate_guest(8), mask = f.process.allocate_guest(4);
    f.engine.write32(mask, 0);
    require(f.sys("_pthread_create", {thread, 0, raiser, 0}) == 0, "pthread_create");
    require(f.sys("_sigsuspend", {mask}) == UINT64_MAX && f.engine.read32(f.sys("___error")) == 4 && handled > 0,
            "sigsuspend returns EINTR after the handler ran");
    woke = true;
    require(f.sys("_pthread_join", {f.engine.read64(thread), 0}) == 0, "pthread_join");
    require(f.sys("_pthread_is_threaded_np") == 1, "the process has had a second thread");
    f.sys("_signal", {30, 0});
}

// misc/bsd_extra.cpp.
void test_bsd_extra(Fixture& f) {
    const auto errno_value = [&f] { return f.engine.read32(f.sys("___error")); };
    const auto text = f.text("abcabc");
    require(f.sys("_memrchr", {text, 'b', 6}) == text + 4 && f.sys("_memrchr", {text, 'z', 6}) == 0 && f.sys("_memrchr", {text, 'a', 0}) == 0, "memrchr");
    require(f.sys("_strchrnul", {text, 'c'}) == text + 2 && f.sys("_strchrnul", {text, 'z'}) == text + 6, "strchrnul");
    f.sys("_explicit_bzero", {text, 3});
    require(f.byte(text) == 0 && f.byte(text + 2) == 0 && f.byte(text + 3) == 'a', "explicit_bzero");
    const auto wide = f.process.allocate_guest(16);
    for (const auto& [index, c] : std::array<std::pair<int, char32_t>, 4>{{{0, U'-'}, {1, U'4'}, {2, U'2'}, {3, 0}}})
        f.engine.write32(wide + static_cast<Address>(index) * 4, static_cast<std::uint32_t>(c));
    require(f.sys("_wcstoimax", {wide, 0, 10}) == static_cast<Address>(-42), "wcstoimax");
    require(f.sys("_wctrans", {f.text("toupper")}) == 2 && f.sys("_towctrans", {'a', 2}) == 'A' && f.sys("_towctrans", {'Q', 1}) == 'q',
            "wctrans / towctrans");
    require(f.sys("_wctrans", {f.text("bogus")}) == 0 && errno_value() == 22, "an unknown mapping is EINVAL");
    // psort: qsort's order.
    const auto compare = f.process.install_function("_bsd_test_compare", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto a = static_cast<std::int32_t>(m.read32(s.x[0])), b = static_cast<std::int32_t>(m.read32(s.x[1]));
        s.x[0] = static_cast<Address>(static_cast<std::int64_t>((a > b) - (a < b)));
    });
    const auto values = f.process.allocate_guest(20);
    const std::array<std::int32_t, 5> unsorted{5, -1, 3, 3, 0};
    for (std::size_t index = 0; index < unsorted.size(); ++index) f.engine.write32(values + index * 4, static_cast<std::uint32_t>(unsorted[index]));
    f.sys("_psort", {values, 5, 4, compare});
    for (std::size_t index = 0; index < 5; ++index)
        require(static_cast<std::int32_t>(f.engine.read32(values + index * 4)) == std::array<std::int32_t, 5>{-1, 0, 3, 3, 5}[index], "psort");
    f.sys("_sranddev");
    f.sys("_arc4random_stir");
    require(f.sys("_rpmatch", {f.text("yes")}) == 1 && f.sys("_rpmatch", {f.text("No")}) == 0 &&
            f.sys("_rpmatch", {f.text("?")}) == static_cast<Address>(-1), "rpmatch");
    // getbsize: 512 without BLOCKSIZE; "1k" is 1024 bytes, "1K-blocks".
    const auto header_length = f.process.allocate_guest(4), block_size = f.process.allocate_guest(8);
    f.sys("_unsetenv", {f.text("BLOCKSIZE")});
    require(f.read(f.sys("_getbsize", {header_length, block_size})) == "512-blocks" && f.engine.read32(header_length) == 10 &&
            f.engine.read64(block_size) == 512, "getbsize default");
    f.sys("_setenv", {f.text("BLOCKSIZE"), f.text("1k"), 1});
    require(f.read(f.sys("_getbsize", {header_length, block_size})) == "1K-blocks" && f.engine.read64(block_size) == 1024, "getbsize 1k");
    f.sys("_unsetenv", {f.text("BLOCKSIZE")});
    // timeoff: 1970-01-01 01:00:00 one hour east of UTC is the epoch.
    const auto tm = f.process.allocate_guest(56);
    f.engine.fill_bytes(tm, 0, 56);
    f.engine.write32(tm + 8, 1); f.engine.write32(tm + 12, 1); f.engine.write32(tm + 20, 70);
    require(f.sys("_timeoff", {tm, 3600}) == 0, "timeoff");
    // pwcache.
    require(f.read(f.sys("_user_from_uid", {424242, 0})) == "424242" && f.sys("_user_from_uid", {424242, 1}) == 0, "user_from_uid of an unknown id");
    const auto id = f.process.allocate_guest(4);
    require(f.sys("_uid_from_user", {f.text("no-such-user"), id}) == static_cast<Address>(-1), "uid_from_user of an unknown name");
    // I/O policies.
    require(f.sys("_getiopolicy_np", {0, 0}) == 0 && f.sys("_setiopolicy_np", {0, 0, 3}) == 0 && f.sys("_getiopolicy_np", {0, 0}) == 3,
            "process I/O policy");
    require(f.sys("_setiopolicy_np", {0, 1, 2}) == 0 && f.sys("_getiopolicy_np", {0, 1}) == 2 && f.sys("_getiopolicy_np", {0, 0}) == 3,
            "thread I/O policy");
    require(f.sys("_setiopolicy_np", {1, 0, 0}) == static_cast<Address>(-1) && errno_value() == 22, "an unknown type is EINVAL");
    // Fortified copies.
    const auto target = f.process.allocate_guest(8);
    f.engine.fill_bytes(target, 0x55, 8);
    require(f.sys("___stpcpy_chk", {target, f.text("abc"), 8}) == target + 3 && f.read(target) == "abc", "__stpcpy_chk");
    f.engine.fill_bytes(target, 0x55, 8);
    require(f.sys("___stpncpy_chk", {target, f.text("ab"), 5, 8}) == target + 2 && f.byte(target + 4) == 0 && f.byte(target + 5) == 0x55,
            "__stpncpy_chk pads to n");
    require(f.sys("___memccpy_chk", {target, f.text("hello"), 'l', 5, 8}) == target + 3, "__memccpy_chk stops after c");
    // fmtcheck.
    const auto suspect = f.text("%d items of %s"), fallback = f.text("%i things of %s, %f");
    require(f.sys("_fmtcheck", {suspect, fallback}) == suspect, "fmtcheck accepts a compatible prefix");
    const auto wrong = f.text("%s items");
    const auto star = f.text("%*d"), plain = f.text("%d");
    require(f.sys("_fmtcheck", {wrong, fallback}) == fallback && f.sys("_fmtcheck", {star, plain}) == plain, "fmtcheck rejects");
    const auto longs = f.text("%lx");
    require(f.sys("_fmtcheck", {longs, f.text("%ld")}) == longs, "fmtcheck: %lx matches %ld");
    // ctype helpers.
    require(f.sys("___istype", {'a', 0x100}) == 1 && f.sys("___istype", {'1', 0x100}) == 0 && f.sys("___isctype", {300, 0x100}) == 0 &&
            f.sys("___tolower_l", {'A', 0}) == 'a' && f.sys("___toupper_l", {'z', 0}) == 'Z', "ctype helpers");
    require(f.engine.read64(f.symbol("/usr/lib/libSystem.B.dylib", "__CurrentRuneLocale")) ==
            f.symbol("/usr/lib/libSystem.B.dylib", "__DefaultRuneLocale"), "_CurrentRuneLocale is the C locale");
}

// Streams without descriptors and the scanf/wide stdio family (fs/memory_streams.cpp,
// fs/files.cpp, misc/native.cpp).
void test_memory_streams(Fixture& f) {
    const auto sys = "/usr/lib/libSystem.B.dylib";
    const auto errno_value = [&f] { return f.engine.read32(f.sys("___error")); };
    const auto variadic = [&f, sys](std::string_view name, std::initializer_list<Address> fixed, std::initializer_list<Address> rest) {
        alchemy::CallRegs registers;
        std::size_t index = 0;
        for (const auto value : fixed) registers.x[index++] = value;
        for (const auto value : rest) registers.push_variadic(value);
        return alchemy::engine_call_regs(f.engine, f.symbol(sys, name), registers).x0;
    };
    const auto wide = [&f](std::u32string_view text) {
        const auto address = f.process.allocate_guest((text.size() + 1) * 4);
        for (std::size_t index = 0; index <= text.size(); ++index)
            f.engine.write32(address + index * 4, index < text.size() ? static_cast<std::uint32_t>(text[index]) : 0);
        return address;
    };
    // fmemopen "w+": printf into the buffer, read it back.
    const auto buffer = f.process.allocate_guest(32), line = f.process.allocate_guest(32);
    f.engine.fill_bytes(buffer, 0x55, 32);
    const auto memory = f.sys("_fmemopen", {buffer, 32, f.text("w+")});
    require(memory != 0 && f.byte(buffer) == 0, "fmemopen w+ starts empty");
    require(variadic("_fprintf", {memory, f.text("x=%d")}, {5}) == 3 && f.read(buffer) == "x=5", "fprintf into a fmemopen buffer");
    require(f.sys("_fseek", {memory, 0, 2}) == 0 && f.sys("_ftell", {memory}) == 3, "SEEK_END is the contents' end");
    f.sys("_rewind", {memory});
    require(f.sys("_fgets", {line, 32, memory}) == line && f.read(line) == "x=5", "fgets reads it back");
    require(f.sys("_fileno", {memory}) == static_cast<Address>(-1) && errno_value() == 9, "no descriptor");
    require(f.sys("_setvbuf", {memory, 0, 1, 0}) == 0 && f.sys("_setlinebuf", {memory}) == 0 && f.sys("_fpurge", {memory}) == 0,
            "buffering modes are accepted");
    require(f.sys("_fclose", {memory}) == 0, "fclose");
    // A full buffer: the write is short.
    const auto small = f.process.allocate_guest(4);
    const auto tight = f.sys("_fmemopen", {small, 4, f.text("w")});
    require(f.sys("_fwrite", {f.text("abcdef"), 1, 6, tight}) == 4 && f.sys("_ferror", {tight}) == 1, "writes past the end are short");
    f.sys("_fclose", {tight});
    // fmemopen "r" with fscanf, and the va_list forms.
    const auto input = f.text("12 abc rest");
    const auto reader = f.sys("_fmemopen", {input, 11, f.text("r")});
    const auto number = f.process.allocate_guest(8), word = f.process.allocate_guest(16), set = f.process.allocate_guest(16);
    require(variadic("_fscanf", {reader, f.text("%d %s")}, {number, word}) == 2 && f.engine.read32(number) == 12 && f.read(word) == "abc",
            "fscanf over a fmemopen stream");
    const auto list = f.process.allocate_guest(16);
    f.engine.write64(list, set);
    require(f.sys("_vfscanf", {reader, f.text(" %[a-s]"), list}) == 1 && f.read(set) == "res", "vfscanf with a scan set");
    f.sys("_fclose", {reader});
    f.engine.write64(list, number); f.engine.write64(list + 8, word);
    require(f.sys("_vsscanf", {f.text("7 eight"), f.text("%d %s"), list}) == 2 && f.engine.read32(number) == 7 && f.read(word) == "eight",
            "vsscanf");
    // open_memstream: *bufp/*sizep follow fflush and fclose.
    const auto cells = f.process.allocate_guest(16);
    const auto dynamic = f.sys("_open_memstream", {cells, cells + 8});
    require(dynamic != 0, "open_memstream");
    f.sys("_fputs", {f.text("hello"), dynamic});
    require(f.sys("_fflush", {dynamic}) == 0 && f.engine.read64(cells + 8) == 5 && f.read(f.engine.read64(cells)) == "hello", "after fflush");
    variadic("_fprintf", {dynamic, f.text("%d")}, {42});
    require(f.sys("_fclose", {dynamic}) == 0 && f.engine.read64(cells + 8) == 7 && f.read(f.engine.read64(cells)) == "hello42", "after fclose");
    f.sys("_free", {f.engine.read64(cells)});
    // open_wmemstream: UTF-8 in, wchar_t out; wide printf and fputws.
    const auto wide_stream = f.sys("_open_wmemstream", {cells, cells + 8});
    f.sys("_fputs", {f.text("h\xc3\xa9"), wide_stream});
    require(f.sys("_fwide", {wide_stream, 1}) == 1 && f.sys("_fwide", {wide_stream, static_cast<Address>(-1)}) == 1, "fwide sets the orientation once");
    f.engine.write64(list, 9);
    require(f.sys("_vfwprintf", {wide_stream, wide(U"n=%d"), list}) == 3, "vfwprintf counts wide characters");
    f.sys("_fputws", {wide(U"!"), wide_stream});
    f.sys("_fclose", {wide_stream});
    const auto text = f.engine.read64(cells);
    require(f.engine.read64(cells + 8) == 6 && f.engine.read32(text) == 'h' && f.engine.read32(text + 4) == 0xe9 &&
            f.engine.read32(text + 8) == 'n' && f.engine.read32(text + 20) == '!' && f.engine.read32(text + 24) == 0, "open_wmemstream");
    f.sys("_free", {text});
    // funopen: the guest's functions with the cookie.
    static std::string written;
    static bool closed = false;
    const auto writer = f.process.install_function("_gaps_funopen_write", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        require(s.x[0] == 0x1234, "the cookie reaches writefn");
        for (std::uint64_t index = 0; index < static_cast<std::uint32_t>(s.x[2]); ++index) written.push_back(static_cast<char>(m.read8(s.x[1] + index)));
        s.x[0] = static_cast<std::uint32_t>(s.x[2]);
    });
    const auto feeder = f.process.install_function("_gaps_funopen_read", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto count = std::min<std::uint64_t>(static_cast<std::uint32_t>(s.x[2]), 4);
        for (std::uint64_t index = 0; index < count; ++index) m.write8(s.x[1] + index, 'Z');
        s.x[0] = count;
    });
    const auto closer = f.process.install_function("_gaps_funopen_close", [](transmute::CpuState& s, transmute::GuestMemory&) { closed = true; s.x[0] = 0; });
    const auto custom = f.sys("_funopen", {0x1234, 0, writer, 0, closer});
    require(f.sys("_fwrite", {f.text("abc"), 1, 3, custom}) == 3 && written == "abc", "funopen writefn");
    require(f.sys("_fseek", {custom, 0, 0}) == static_cast<Address>(-1) && errno_value() == 29, "no seekfn: ESPIPE");
    require(f.sys("_fclose", {custom}) == 0 && closed, "closefn runs at fclose");
    const auto source = f.sys("_funopen", {0, feeder, 0, 0, 0});
    const auto bytes = f.process.allocate_guest(8);
    require(f.sys("_fread", {bytes, 1, 6, source}) == 6 && f.byte(bytes) == 'Z' && f.byte(bytes + 5) == 'Z', "funopen readfn");
    f.sys("_fclose", {source});
    require(f.sys("_funopen", {0, 0, 0, 0, 0}) == 0 && errno_value() == 22, "funopen needs a read or write function");
    require(f.read(f.sys("_ctermid", {0})) == "/dev/tty", "ctermid");
    // tempnam: a name under TMPDIR from the prefix.
    const auto temporary = alchemy::guest_path_string(std::filesystem::temp_directory_path());
    f.sys("_setenv", {f.text("TMPDIR"), f.text(temporary), 1});
    const auto name = f.sys("_tempnam", {0, f.text("abcdefg")});
    require(name && f.read(name).starts_with(temporary) && f.read(name).find("/abcde") != std::string::npos, "tempnam");
    f.sys("_free", {name});
    f.sys("_unsetenv", {f.text("TMPDIR")});
}

// net/netdb_extra.cpp.
void test_netdb_extra(Fixture& f) {
    const auto errno_value = [&f] { return f.engine.read32(f.sys("___error")); };
    const auto address = f.process.allocate_guest(16), out = f.process.allocate_guest(64);
    f.engine.write_bytes(address, std::vector<std::uint8_t>{10, 1, 0x1f, 0});
    const auto net = [&](Address family, Address bits, Address size = 64) { return f.sys("_inet_net_ntop", {family, address, bits, out, size}); };
    require(net(2, 16) == out && f.read(out) == "10.1/16", "inet_net_ntop /16");
    require(net(2, 20) == out && f.read(out) == "10.1.16/20", "inet_net_ntop /20 masks the partial octet");
    require(net(2, 0) == out && f.read(out) == "0/0", "inet_net_ntop /0");
    require(net(2, 33) == 0 && errno_value() == 22, "inet_net_ntop bits past 32");
    require(net(2, 16, 4) == 0 && errno_value() == 40, "inet_net_ntop with a short buffer");
    f.engine.write_bytes(address, std::vector<std::uint8_t>{0x20, 0x01, 0x0d, 0xb8, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    require(net(30, 32) == out && f.read(out) == "2001:db8::/32", "inet_net_ntop AF_INET6");
    const auto ether = f.process.allocate_guest(8), host = f.process.allocate_guest(256);
    require(f.sys("_ether_line", {f.text("00:1a:2B:3c:4d:5e  host-a # comment"), ether, host}) == 0 && f.read(host) == "host-a" &&
            f.byte(ether + 1) == 0x1a && f.byte(ether + 2) == 0x2b && f.byte(ether + 5) == 0x5e, "ether_line");
    require(f.sys("_ether_line", {f.text("zz:00 host"), ether, host}) == static_cast<Address>(-1), "ether_line rejects a bad address");
    const auto v4 = f.sys("_gethostbyname2", {f.text("127.0.0.1"), 2});
    require(v4 && f.engine.read32(v4 + 16) == 2 && f.engine.read32(v4 + 20) == 4 &&
            f.engine.read32(f.engine.read64(f.engine.read64(v4 + 24))) == 0x0100007fU, "gethostbyname2 AF_INET");
    const auto v6 = f.sys("_gethostbyname2", {f.text("::1"), 30});
    require(v6 && f.engine.read32(v6 + 16) == 30 && f.engine.read32(v6 + 20) == 16 &&
            f.byte(f.engine.read64(f.engine.read64(v6 + 24)) + 15) == 1, "gethostbyname2 AF_INET6");
    require(f.sys("_gethostbyname2", {f.text("127.0.0.1"), 99}) == 0, "gethostbyname2 with another family");
    const auto error = f.process.allocate_guest(4);
    const auto mapped = f.sys("_getipnodebyname", {f.text("127.0.0.1"), 30, 0x800, error});
    require(mapped && f.engine.read32(error) == 0 && f.engine.read32(mapped + 16) == 30 && f.engine.read32(mapped + 20) == 16, "getipnodebyname AI_V4MAPPED");
    const auto bytes = f.engine.read64(f.engine.read64(mapped + 24));
    require(f.byte(bytes + 10) == 0xff && f.byte(bytes + 11) == 0xff && f.byte(bytes + 12) == 127 && f.byte(bytes + 15) == 1, "::ffff:127.0.0.1");
    f.sys("_freehostent", {mapped});
    // The /etc databases: whatever the guest file system has (none here is fine).
    f.sys("_setservent", {0});
    for (int index = 0; index < 4 && f.sys("_getservent"); ++index) {}
    f.sys("_endservent");
    require(f.sys("_getnetbyname", {f.text("no-such-network")}) == 0, "getnetbyname of an unknown name");
}

void test_os_extras(Fixture& f) {
    // os_sync: a wait on a value that differs returns at once; a timeout expires; wakes without waiters are ENOENT.
    const auto word = f.process.allocate_guest(8);
    f.engine.write32(word, 7);
    require(f.sys("_os_sync_wait_on_address", {word, 6, 4, 0}) == 0, "a different value does not wait");
    require(f.sys("_os_sync_wait_on_address", {word, 7, 3, 0}) == UINT64_MAX, "size 3 is EINVAL");
    require(f.sys("_os_sync_wait_on_address_with_timeout", {word, 7, 4, 0, 32, 1000000}) == UINT64_MAX, "the timeout expires");
    require(f.sys("_os_sync_wait_on_address_with_timeout", {word, 7, 4, 0, 1, 1000000}) == UINT64_MAX, "only the mach clock");
    require(f.sys("_os_sync_wake_by_address_any", {word, 4, 0}) == UINT64_MAX, "no waiter: ENOENT");
    // A waiter on another guest thread is woken.
    static std::atomic<int> woke{0};
    static Fixture* fixture = &f;
    static Address shared_word{};
    shared_word = word;
    const auto waiter = f.process.install_function("_gaps_os_sync_waiter", [](transmute::CpuState& s, transmute::GuestMemory&) {
        // On this guest thread's engine (the fixture's calls run on the main engine).
        const std::array<std::uint64_t, 4> arguments{shared_word, 7, 4, 0};
        const auto result = alchemy::EngineAdapter(fixture->process.calling_engine())
                                .call(fixture->symbol("/usr/lib/libSystem.B.dylib", "_os_sync_wait_on_address"), arguments);
        if (result != UINT64_MAX) ++woke;
        s.x[0] = 0;
    });
    const auto thread = f.process.allocate_guest(8);
    require(f.sys("_pthread_create", {thread, 0, waiter, 0}) == 0, "pthread_create");
    std::uint64_t woken = UINT64_MAX;
    for (int attempt = 0; attempt < 500 && woken == UINT64_MAX; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        woken = f.sys("_os_sync_wake_by_address_all", {word, 4, 0});
    }
    require(woken == 0, "the waiter was woken");
    require(f.sys("_pthread_join", {f.engine.read64(thread), 0}) == 0 && woke == 1, "the wait returned");
    // Workgroups: join/leave, cancel, intervals, the working arena.
    const auto group = f.sys("_os_workgroup_interval_create", {f.text("render"), 32, 0});
    require(group != 0 && f.sys("_os_workgroup_interval_create", {f.text("bad"), 1, 0}) == 0, "os_workgroup_interval_create");
    const auto token = f.process.allocate_guest(40), other = f.process.allocate_guest(40);
    require(f.sys("_os_workgroup_join", {group, token}) == 0, "join");
    require(f.sys("_os_workgroup_join", {group, other}) == 37, "one workgroup per thread (EALREADY)");
    require(f.sys("_os_workgroup_set_working_arena", {group, 0x1000, 4, 0}) == 16, "no arena change while joined (EBUSY)");
    f.sys("_os_workgroup_leave", {group, token});
    require(f.sys("_os_workgroup_set_working_arena", {group, 0x1000, 4, 0}) == 0, "set_working_arena");
    require(f.sys("_os_workgroup_join", {group, token}) == 0, "join again");
    const auto index = f.process.allocate_guest(4);
    require(f.sys("_os_workgroup_get_working_arena", {group, index}) == 0x1000 && f.engine.read32(index) == 0, "get_working_arena");
    require(f.sys("_os_workgroup_interval_start", {group, 10, 20, 0}) == 0 && f.sys("_os_workgroup_interval_start", {group, 10, 20, 0}) == 22,
            "an interval starts once");
    require(f.sys("_os_workgroup_interval_finish", {group, 0}) == 0 && f.sys("_os_workgroup_interval_finish", {group, 0}) == 22, "finish");
    f.sys("_os_workgroup_leave", {group, token});
    // (copy_port / create_with_port need Mach IPC: mach_tests.)
    const auto twin = f.sys("_os_workgroup_create_with_workgroup", {f.text("twin"), group});
    require(twin != 0, "create_with_workgroup");
    f.sys("_os_workgroup_cancel", {group});
    require(f.sys("_os_workgroup_testcancel", {twin}) == 1 && f.sys("_os_workgroup_join", {twin, token}) == 22, "cancelling cancels the group");
    require(static_cast<std::int64_t>(f.sys("_os_workgroup_max_parallel_threads", {group, 0})) >= 1, "max_parallel_threads");
    // dnsinfo: no configuration snapshot; configd's notify key.
    require(f.sys("_dns_configuration_copy") == 0, "dns_configuration_copy is NULL without configd");
    require(f.read(f.sys("_dns_configuration_notify_key")) == "com.apple.system.SystemConfiguration.dns_configuration", "dns_configuration_notify_key");
}

void test_guest_services(Fixture& f) {
    const auto cf = alchemy::core_foundation_library;
    const auto system = "/usr/lib/libSystem.B.dylib";
    // ptrace, xpc dates, fsctl (notify(3) is mach/notify.cpp's).
    {
        require(f.sys("_ptrace", {31, 0, 0, 0}) == 0, "PT_DENY_ATTACH");
        require(f.sys("_ptrace", {0, 0, 0, 0}) == UINT64_MAX, "PT_TRACE_ME fails");
        const auto date = f.sys("_xpc_date_create_from_current");
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto value = static_cast<std::int64_t>(f.sys("_xpc_date_get_value", {date}));
        require(date && value > 0 && now - value < 60'000'000'000LL && now >= value, "xpc_date_create_from_current");
        const auto directory = f.text(alchemy::guest_path_string(std::filesystem::temp_directory_path()));
        require(f.sys("_fsctl", {directory, 0x4004, 0, 0}) == UINT64_MAX, "fsctl fails");
    }
    // __getdirentries64 over a directory descriptor.
    {
        const auto root = std::filesystem::temp_directory_path() / "alchemy_export_fill_dirents";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root / "sub");
        { std::ofstream file(root / "alpha.txt"); file << "a"; }
        const auto descriptor = f.sys("_open", {f.text(alchemy::guest_path_string(root)), 0x100000, 0}); // O_DIRECTORY
        require(static_cast<std::int32_t>(descriptor) >= 0, "open directory");
        const auto buffer = f.process.allocate_guest(4096), base = f.process.allocate_guest(8);
        std::map<std::string, std::uint8_t> seen;
        for (;;) {
            const auto count = f.sys("___getdirentries64", {descriptor, buffer, 64, base});
            require(count != UINT64_MAX, "__getdirentries64");
            if (!count) break;
            for (std::uint64_t offset = 0; offset < count;) {
                const auto length = f.engine.read32(buffer + offset + 16) & 0xffff;
                const auto name_length = f.engine.read32(buffer + offset + 18) & 0xffff;
                const auto name = guest_bytes(f, buffer + offset + 21, name_length);
                require(f.engine.read64(buffer + offset) != 0 && length % 8 == 0 && length >= 21 + name_length + 1, "direntry layout");
                seen[std::string(name.begin(), name.end())] = f.byte(buffer + offset + 20);
                offset += length;
            }
        }
        require(seen.size() == 4 && seen["."] == 4 && seen[".."] == 4 && seen["sub"] == 4 && seen["alpha.txt"] == 8, "directory entries");
        f.sys("_close", {descriptor});
        std::filesystem::remove_all(root, ignored);
    }
    // mach_vm_region / proc_regionfilename.
    {
        const auto block = f.process.allocate_guest(4096);
        const auto address = f.process.allocate_guest(8), size = f.process.allocate_guest(8);
        const auto info = f.process.allocate_guest(64), count = f.process.allocate_guest(4), object = f.process.allocate_guest(4);
        f.engine.write64(address, block + 8);
        f.engine.write32(count, 9);
        require(f.sys("_mach_vm_region", {f.engine.read32(f.symbol(system, "_mach_task_self_")), address, size, 9, info, count, object}) == 0,
                "mach_vm_region");
        const auto region = f.engine.read64(address), length = f.engine.read64(size);
        require(region <= block + 8 && block + 8 < region + length && (f.engine.read32(info) & 3) == 3 && f.engine.read32(count) == 9,
                "mach_vm_region basic info");
        const auto name = f.process.allocate_guest(256);
        require(f.sys("_proc_regionfilename", {f.sys("_getpid"), block, name, 256}) == 0, "anonymous region has no file");
    }
}

// Names real apps import that were missing (VS Code's libSystem names, Blender's Python
// stdlib modules): rand48, ndbm, NIS/RPC, inet_aton and friends, libproc, mount info,
// copyfile states, multibyte, uuid v1, __clear_cache.
void test_threads_extra(Fixture& f) {
    require(f.sys("_qos_class_main") == 0x21, "qos_class_main is USER_INTERACTIVE");
    const auto attr = f.process.allocate_guest(64), out = f.process.allocate_guest(16);
    require(f.sys("_pthread_attr_init", {attr}) == 0, "pthread_attr_init");
    require(f.sys("_pthread_attr_setstack", {attr, out, 0x10000}) == 45, "caller-provided stacks are ENOTSUP");
    require(f.sys("_pthread_attr_getstack", {attr, out, out + 8}) == 0 && f.engine.read64(out) == 0 && f.engine.read64(out + 8) != 0,
            "pthread_attr_getstack: no address, the default size");
    require(f.sys("_pthread_setconcurrency", {4}) == 0 && f.sys("_pthread_getconcurrency") == 4, "the concurrency level is kept");
    const auto mutexattr = f.process.allocate_guest(16);
    require(f.sys("_pthread_mutexattr_setprioceiling", {mutexattr, 30}) == 0 && f.sys("_pthread_mutexattr_getprioceiling", {mutexattr, out}) == 0 &&
            f.engine.read32(out) == 30, "priority ceilings are kept");
    require(f.sys("_sem_init", {out, 0, 1}) == UINT64_MAX, "Darwin has no unnamed semaphores");
    static Address jitted = 0;
    const auto callback = f.process.install_function("_gaps_jit_callback", [](transmute::CpuState& s, transmute::GuestMemory&) { jitted = s.x[0]; s.x[0] = 7; });
    require(f.sys("_pthread_jit_write_with_callback_np", {callback, 5}) == 7 && jitted == 5, "pthread_jit_write_with_callback_np runs the callback");
    const auto rwattr = f.process.allocate_guest(16), rwlock = f.process.allocate_guest(200);
    require(f.sys("_pthread_rwlockattr_init", {rwattr}) == 0 && f.sys("_pthread_rwlockattr_setpshared", {rwattr, 1}) == 45 &&
            f.sys("_pthread_rwlockattr_getpshared", {rwattr, out}) == 0 && f.engine.read32(out) == 2, "rwlock attributes: process-private");
    require(f.sys("_pthread_rwlock_init", {rwlock, rwattr}) == 0 && f.sys("_pthread_rwlock_destroy", {rwlock}) == 0 &&
            f.sys("_pthread_rwlockattr_destroy", {rwattr}) == 0, "pthread_rwlock_init with attributes");
    // QoS classes and guard sizes are kept on the attribute.
    const auto qos = f.process.allocate_guest(8);
    require(f.sys("_pthread_attr_get_qos_class_np", {attr, qos, qos + 4}) == 0 && f.engine.read32(qos) == 0 && f.engine.read32(qos + 4) == 0,
            "no QoS class until one is set");
    require(f.sys("_pthread_attr_set_qos_class_np", {attr, 0x19, static_cast<Address>(-3)}) == 0 &&
            f.sys("_pthread_attr_get_qos_class_np", {attr, qos, qos + 4}) == 0 && f.engine.read32(qos) == 0x19 &&
            static_cast<std::int32_t>(f.engine.read32(qos + 4)) == -3, "QoS class round trip");
    require(f.sys("_pthread_attr_getguardsize", {attr, out}) == 0 && f.engine.read64(out) == 16384, "one guard page by default");
    require(f.sys("_pthread_attr_setguardsize", {attr, 20000}) == 0 && f.sys("_pthread_attr_getguardsize", {attr, out}) == 0 &&
            f.engine.read64(out) == 32768, "guard sizes round up to whole pages");
}

void test_app_imports(Fixture& f) {
    // rand48: srand48(1) gives glibc's well-known first values.
    f.sys("_srand48", {1});
    require(f.sys("_lrand48") == 89400484 && f.sys("_lrand48") == 976015093 && f.sys("_lrand48") == 1792756325, "lrand48 after srand48(1)");
    f.sys("_srand48", {1});
    require(f.sys("_mrand48") == 178800969, "mrand48 after srand48(1)");
    {
        // nrand48 advances its own xsubi[3] and leaves the process generator alone.
        const auto xsubi = f.process.allocate_guest(8);
        f.engine.write32(xsubi, 0x0001330E); f.engine.write32(xsubi + 4, 0);   // {0x330E, 1, 0}: srand48(1)'s X
        require(f.sys("_nrand48", {xsubi}) == 89400484 && f.sys("_nrand48", {xsubi}) == 976015093, "nrand48 with its own state");
        f.sys("_srand48", {1});
        require(f.sys("_lrand48") == 89400484, "the process generator is independent");
        // seed48 returns the previous X and sets a new one.
        const auto seed = f.process.allocate_guest(8);
        f.engine.write32(seed, 0x0001330E); f.engine.write32(seed + 4, 0);
        const auto previous = f.sys("_seed48", {seed});
        require(previous && f.sys("_lrand48") == 89400484, "seed48");
        require(f.sys("_crypt", {f.text("x"), f.text("ab")}) == 0, "crypt answers ENOSYS");
    }
    // ndbm: store, fetch, replace, insert-exists, delete, iterate, reopen.
    {
        const auto root = std::filesystem::temp_directory_path() / "alchemy-ndbm-test";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
        const auto name = f.text(alchemy::guest_path_string(root / "db"));
        const auto open = [&](std::uint64_t flags) { return f.sys("_dbm_open", {name, flags, 0644}); };
        auto db = open(0x202);   // O_RDWR | O_CREAT
        require(db != 0, "dbm_open creates the database");
        const auto key = f.text("alpha"), value = f.text("one"), other = f.text("second value");
        require(f.sys("_dbm_store", {db, key, 5, value, 3, 0}) == 0, "dbm_store insert");
        require(f.sys("_dbm_store", {db, key, 5, other, 12, 0}) == 1, "dbm_store DBM_INSERT keeps an existing key");
        require(f.sys("_dbm_store", {db, key, 5, other, 12, 1}) == 0, "dbm_store DBM_REPLACE");
        const auto beta = f.text("beta");
        require(f.sys("_dbm_store", {db, beta, 4, value, 3, 1}) == 0, "second key");
        const auto fetched = f.sys("_dbm_fetch", {db, key, 5});
        require(fetched && guest_bytes(f, fetched, 12) == bytes("second value"), "dbm_fetch");
        require(f.sys("_dbm_fetch", {db, f.text("gamma"), 5}) == 0, "dbm_fetch of a missing key");
        const auto first = f.sys("_dbm_firstkey", {db});
        require(first && guest_bytes(f, first, 5) == bytes("alpha"), "dbm_firstkey in byte order");
        const auto second = f.sys("_dbm_nextkey", {db});
        require(second && guest_bytes(f, second, 4) == bytes("beta") && f.sys("_dbm_nextkey", {db}) == 0, "dbm_nextkey");
        require(f.sys("_dbm_delete", {db, beta, 4}) == 0 && f.sys("_dbm_delete", {db, beta, 4}) == UINT64_MAX, "dbm_delete");
        require(f.sys("_dbm_error", {db}) == 0 && f.sys("_dbm_clearerr", {db}) == 0, "dbm_error");
        f.sys("_dbm_close", {db});
        db = open(0);   // O_RDONLY: the log is replayed
        require(db != 0, "dbm_open reopens");
        const auto again = f.sys("_dbm_fetch", {db, key, 5});
        require(again && guest_bytes(f, again, 12) == bytes("second value") && f.sys("_dbm_fetch", {db, beta, 4}) == 0, "the database persists");
        require(f.sys("_dbm_store", {db, beta, 4, value, 3, 1}) == UINT64_MAX, "a read-only database refuses stores");
        f.sys("_dbm_close", {db});
        { std::ofstream bad(root / "bad.db"); bad << "not a database"; }
        require(f.sys("_dbm_open", {f.text(alchemy::guest_path_string(root / "bad")), 0, 0}) == 0, "a foreign file is refused (EFTYPE)");
        std::filesystem::remove_all(root, ignored);
    }
    // NIS and Sun RPC are unavailable.
    {
        const auto domain = f.process.allocate_guest(8);
        require(f.sys("_yp_get_default_domain", {domain}) == 12, "YPERR_NODOM");
        require(f.sys("_yp_match", {0, 0, 0, 0, 0, 0}) == 10, "YPERR_YPBIND");
        require(f.read(f.sys("_yperr_string", {12})) == "Local domain name not set", "yperr_string");
        require(f.sys("_clnt_create", {f.text("localhost"), 100004, 2, f.text("udp")}) == 0, "clnt_create fails");
        require(f.read(f.sys("_clnt_spcreateerror", {f.text("nis")})).starts_with("nis: RPC: Port mapper failure"), "clnt_spcreateerror");
    }
    // inet_aton and the classful helpers (network-order results).
    {
        const auto address = f.process.allocate_guest(4);
        require(f.sys("_inet_aton", {f.text("127.0.0.1"), address}) == 1 && f.engine.read32(address) == 0x0100007fU, "inet_aton dotted quad");
        require(f.sys("_inet_aton", {f.text("10.1"), address}) == 1 && f.engine.read32(address) == 0x0100000aU, "inet_aton a.b");
        require(f.sys("_inet_aton", {f.text("0x7f.1"), address}) == 1 && f.engine.read32(address) == 0x0100007fU, "inet_aton hex part");
        require(f.sys("_inet_aton", {f.text("256.1.1.1"), address}) == 0 && f.sys("_inet_aton", {f.text("1.2.3.4.5"), address}) == 0 &&
                f.sys("_inet_aton", {f.text("1.2.3.x"), address}) == 0, "inet_aton rejects malformed addresses");
        require(static_cast<std::uint32_t>(f.sys("_inet_network", {f.text("192.168.1")})) == 0xc0a801U, "inet_network");
        require(static_cast<std::uint32_t>(f.sys("_inet_makeaddr", {10, 5})) == 0x0500000aU, "inet_makeaddr class A");
        require(static_cast<std::uint32_t>(f.sys("_inet_netof", {0x0101a8c0U})) == 0xc0a801U &&
                static_cast<std::uint32_t>(f.sys("_inet_lnaof", {0x0101a8c0U})) == 1, "inet_netof/inet_lnaof class C");
    }
    // libproc: pid lists count pids, fdinfo describes this process's vnodes.
    {
        const auto pid = f.sys("_getpid");
        const auto total = static_cast<std::int32_t>(f.sys("_proc_listallpids", {0, 0}));
        require(total > 0, "proc_listallpids counts");
        const auto pids = f.process.allocate_guest(static_cast<std::uint64_t>(total) * 4);
        const auto listed = static_cast<std::int32_t>(f.sys("_proc_listallpids", {pids, static_cast<std::uint64_t>(total) * 4}));
        require(listed > 0 && listed <= total, "proc_listallpids lists");
        const auto path = std::filesystem::temp_directory_path() / "alchemy-fdinfo.txt";
        { std::ofstream file(path, std::ios::binary); file << "0123456789"; }
        const auto fd = f.sys("_open", {f.text(alchemy::guest_path_string(path)), 0, 0});
        require(static_cast<std::int32_t>(fd) >= 0, "open for fdinfo");
        const auto info = f.process.allocate_guest(1200);
        require(f.sys("_proc_pidfdinfo", {pid, fd, 2, info, 1200}) == 1200, "proc_pidfdinfo PROC_PIDFDVNODEPATHINFO");
        require(f.engine.read32(info + 16) == 1 && f.engine.read32(info + 24 + 136) == 1 && f.engine.read64(info + 24 + 88) == 10, "vnode info");
        require(f.read(info + 176) == alchemy::guest_path_string(path), "the descriptor's path");
        require(f.sys("_proc_pidfdinfo", {pid, fd, 3, info, 1200}) == 0, "socket flavors are not offered");
        require(f.sys("_proc_pidfdinfo", {pid, fd, 2, info, 100}) == 0, "a short buffer fails");
        f.sys("_close", {fd});
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        const auto token = f.process.allocate_guest(32);
        f.engine.fill_bytes(token, 0, 32); f.engine.write32(token + 20, static_cast<std::uint32_t>(pid));
        require(f.sys("_proc_signal_with_audittoken", {token, 0}) == 0, "signal 0 to self");
        require(f.sys("_proc_signal_with_audittoken", {0, 0}) == 22, "a NULL token is EINVAL");
    }
    // Mounted file systems: "/" first, every record a statfs.
    {
        const auto count = static_cast<std::int32_t>(f.sys("_getfsstat", {0, 0, 2}));
        require(count >= 1, "getfsstat counts");
        const auto buffer = f.process.allocate_guest(static_cast<std::uint64_t>(count) * 2168);
        require(static_cast<std::int32_t>(f.sys("_getfsstat", {buffer, static_cast<std::uint64_t>(count) * 2168, 2})) == count, "getfsstat fills");
        require(f.read(buffer + 88) == "/", "the first file system is mounted on /");
        const auto out = f.process.allocate_guest(8);
        require(static_cast<std::int32_t>(f.sys("_getmntinfo", {out, 2})) == count && f.read(f.engine.read64(out) + 88) == "/", "getmntinfo");
        require(static_cast<std::int32_t>(f.sys("_getmntinfo_r_np", {out, 2})) == count, "getmntinfo_r_np");
        f.sys("_free", {f.engine.read64(out)});
    }
    // copyfile states record what was copied.
    {
        const auto root = std::filesystem::temp_directory_path() / "alchemy-copyfile-state";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
        { std::ofstream file(root / "from.txt", std::ios::binary); file << "twelve bytes"; }
        const auto state = f.sys("_copyfile_state_alloc");
        require(state != 0, "copyfile_state_alloc");
        const auto from = f.text(alchemy::guest_path_string(root / "from.txt")), to = f.text(alchemy::guest_path_string(root / "to.txt"));
        require(f.sys("_copyfile", {from, to, state, 1 << 3}) == 0, "copyfile COPYFILE_DATA");
        const auto value = f.process.allocate_guest(8);
        require(f.sys("_copyfile_state_get", {state, 8, value}) == 0 && f.engine.read64(value) == 12, "COPYFILE_STATE_COPIED");
        require(f.sys("_copyfile_state_get", {state, 2, value}) == 0 && f.read(f.engine.read64(value)) == alchemy::guest_path_string(root / "from.txt"),
                "COPYFILE_STATE_SRC_FILENAME");
        require(f.sys("_copyfile_state_get", {state, 10, value}) == 0 && f.byte(value) == 0, "nothing is cloned");
        require(f.sys("_copyfile_state_set", {state, 7, 0x1234}) == 0 && f.sys("_copyfile_state_get", {state, 7, value}) == 0 &&
                f.engine.read64(value) == 0x1234, "COPYFILE_STATE_STATUS_CTX round trip");
        require(f.sys("_copyfile_state_get", {state, 99, value}) == UINT64_MAX, "an unknown flag is EINVAL");
        require(f.sys("_copyfile_state_free", {state}) == 0, "copyfile_state_free");
        std::filesystem::remove_all(root, ignored);
    }
    // getdomainname (no NIS domain), fchflags (as chflags), multibyte, uuid v1, __clear_cache.
    {
        const auto name = f.process.allocate_guest(64);
        static_cast<transmute::GuestMemory&>(f.engine).write8(name, 'x');
        require(f.sys("_getdomainname", {name, 64}) == 0 && f.byte(name) == 0, "getdomainname is empty");
        require(f.sys("_fchflags", {0, 0}) == UINT64_MAX, "fchflags fails as chflags does");
        const auto out = f.process.allocate_guest(8);
        require(f.sys("_wctomb", {0, 'a'}) == 0 && f.sys("_wctomb", {out, 'a'}) == 1 && f.byte(out) == 'a', "wctomb");
        require(f.sys("_wctomb", {out, 0x263a}) == UINT64_MAX, "wctomb outside the C locale");
        require(f.sys("_mblen", {f.text("abc"), 3}) == 1 && f.sys("_mblen", {0, 0}) == 0, "mblen");
        const auto first = f.process.allocate_guest(16), second = f.process.allocate_guest(16);
        require(f.sys("_uuid_generate_time", {first}) == 0 && f.sys("_uuid_generate_time", {second}) == 0, "uuid_generate_time");
        require((f.byte(first + 6) >> 4) == 1 && (f.byte(first + 8) & 0xc0) == 0x80, "version 1, RFC 4122 variant");
        require(guest_bytes(f, first, 16) != guest_bytes(f, second, 16) && guest_bytes(f, first + 10, 6) == guest_bytes(f, second + 10, 6),
                "time-ordered UUIDs share the node");
        const auto code = f.process.allocate_guest(64);
        require(f.sys("___clear_cache", {code, code + 64}) == 0, "__clear_cache");
    }
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("pure"); test_pure();
        stage("fixture"); Fixture f;
        stage("guest libSystem"); test_guest_libsystem(f);
        stage("guest misc"); test_guest_misc(f);
        stage("guest services"); test_guest_services(f);
        stage("app imports"); test_app_imports(f);
        stage("threads extra"); test_threads_extra(f);
        stage("os extras"); test_os_extras(f);
        stage("compiler-rt"); test_compiler_rt(f);
        stage("blocks"); test_blocks(f);
        stage("random states"); test_random_states(f);
        stage("sigsuspend"); test_sigsuspend(f);
        stage("bsd extra"); test_bsd_extra(f);
        stage("netdb extra"); test_netdb_extra(f);
        stage("memory streams"); test_memory_streams(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "libSystem gaps tests passed\n";
    return 0;
}
