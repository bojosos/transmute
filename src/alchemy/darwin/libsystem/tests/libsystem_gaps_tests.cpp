// The libSystem pieces first written for a renderer plug-in (net/, process/ and misc/libsystem_*.cpp):
// pure helpers and the guest-visible exports (split from the export-fill tests).
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

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
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "libSystem gaps tests passed\n";
    return 0;
}
