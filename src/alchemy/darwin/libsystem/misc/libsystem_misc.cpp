#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/host/vm.hpp"

#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// libSystem leftovers a renderer plug-in's Qt, Boost and tool images import: difftime,
// remainder, pthread_main_np, pthread attribute scheduling fields,
// pthread_cond_timedwait_relative_np, proc_name, ftok, getgrgid_r,
// mlock/munlock, the anonymous-memory vm_map behind mach_exports.cpp's,
// dispatch_once and atexit_b (blocks), host_processor_info and thread_info
// (CPU accounting), and os_activity (libsystem_trace; os_log is oslog.cpp's).
namespace alchemy::export_fill {
namespace {

// <mach/kern_return.h>
constexpr std::uint64_t kern_success = 0, kern_invalid_argument = 4, kern_no_space = 3, kern_failure = 5;
constexpr std::uint32_t echild = 10;
constexpr std::uint64_t pthread_attr_magic = 0x5448524441545452ULL;   // darwin_threads' initialized attribute
// Scheduling fields kept inside the 64-byte pthread_attr_t, which
// darwin_threads zero-fills and otherwise tracks by address.
constexpr std::uint64_t attr_policy = 40, attr_priority = 44, attr_inherit = 48;
constexpr std::uint32_t sched_other = 1, sched_rr = 2, sched_fifo = 4;
constexpr std::int32_t default_priority = 31;

void return_double(CpuState& state, double value) { state.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; }

bool attribute(const GuestMemory& memory, std::uint64_t address) {
    return address && memory.read64(address) == pthread_attr_magic;
}

std::string base_name(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ---- os_activity -------------------------------------------------------------
// os_activity objects (os_log objects are misc/oslog.cpp's): reference-counted
// handles with no tracing consumer.
struct OsObject { std::uint32_t references{1}; std::string category; std::uint64_t id{}, parent{}; };

class OsObjects {
public:
    explicit OsObjects(ProcessContext& process) : process_(process) {}
    std::uint64_t create(OsObject value) {
        const auto address = process_.allocate_guest(32, 16);
        std::lock_guard lock(mutex_);
        objects_.emplace(address, std::move(value));
        return address;
    }
    bool retain(std::uint64_t address) {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(address);
        if (found == objects_.end()) return false;
        ++found->second.references;
        return true;
    }
    bool release(std::uint64_t address) {
        {
            std::lock_guard lock(mutex_);
            const auto found = objects_.find(address);
            if (found == objects_.end()) return false;
            if (--found->second.references) return true;
            objects_.erase(found);
        }
        process_.release_guest(address);
        return true;
    }
    std::optional<OsObject> find(std::uint64_t address) {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(address);
        if (found == objects_.end()) return std::nullopt;
        return found->second;
    }

private:
    ProcessContext& process_;
    std::mutex mutex_;
    std::map<std::uint64_t, OsObject> objects_;
};

void put_text(GuestMemory& memory, std::uint64_t address, std::string_view text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    memory.write_bytes(address, bytes);
}

// Nanoseconds as Mach time_value_t {seconds, microseconds}.
void time_value(GuestMemory& memory, std::uint64_t address, std::uint64_t nanoseconds) {
    memory.write32(address, static_cast<std::uint32_t>(nanoseconds / 1'000'000'000));
    memory.write32(address + 4, static_cast<std::uint32_t>((nanoseconds / 1'000) % 1'000'000));
}

} // namespace

void install_libsystem_misc(Provider& p) {
    auto& process = p.process();

    // double difftime(time_t, time_t)
    p.libsystem("_difftime", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        return_double(s, static_cast<double>(static_cast<std::int64_t>(s.x[0])) - static_cast<double>(static_cast<std::int64_t>(s.x[1])));
        return s.x[0];
    });
    // double remainder(double, double): IEEE remainder.
    p.libsystem("_remainder", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        return_double(s, std::remainder(std::bit_cast<double>(s.v[0][0]), std::bit_cast<double>(s.v[1][0])));
        return s.x[0];
    });
    // int pthread_main_np(void): the thread that entered main().
    p.libsystem("_pthread_main_np", [&process](CpuState&, GuestMemory&) -> std::uint64_t {
        return &process.calling_engine() == &process.main_engine() ? 1 : 0;
    });

    // Attribute scheduling. SCHED_OTHER with a priority in 0..47 is accepted
    // (advisory: the host scheduler decides, as pthread_setschedparam does);
    // the real-time policies are unavailable (ENOTSUP).
    p.libsystem("_pthread_attr_setschedpolicy", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!attribute(m, s.x[0])) return einval;
        const auto policy = static_cast<std::uint32_t>(s.x[1]);
        if (policy == sched_rr || policy == sched_fifo) return enotsup;
        if (policy != sched_other) return einval;
        m.write32(s.x[0] + attr_policy, policy);
        return 0;
    });
    p.libsystem("_pthread_attr_getschedpolicy", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!attribute(m, s.x[0]) || !s.x[1]) return einval;
        const auto policy = m.read32(s.x[0] + attr_policy);
        m.write32(s.x[1], policy ? policy : sched_other);
        return 0;
    });
    p.libsystem("_pthread_attr_setschedparam", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!attribute(m, s.x[0]) || !s.x[1]) return einval;
        const auto priority = static_cast<std::int32_t>(m.read32(s.x[1]));
        if (priority < 0 || priority > 47) return einval;
        m.write32(s.x[0] + attr_priority, static_cast<std::uint32_t>(priority) | 0x80000000U);
        return 0;
    });
    p.libsystem("_pthread_attr_getschedparam", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!attribute(m, s.x[0]) || !s.x[1]) return einval;
        const auto stored = m.read32(s.x[0] + attr_priority);
        m.write32(s.x[1], stored ? stored & 0x7fffffffU : static_cast<std::uint32_t>(default_priority));
        return 0;
    });
    // PTHREAD_INHERIT_SCHED 1, PTHREAD_EXPLICIT_SCHED 2.
    p.libsystem("_pthread_attr_setinheritsched", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!attribute(m, s.x[0])) return einval;
        const auto value = static_cast<std::uint32_t>(s.x[1]);
        if (value != 1 && value != 2) return einval;
        m.write32(s.x[0] + attr_inherit, value);
        return 0;
    });
    p.libsystem("_pthread_attr_getinheritsched", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!attribute(m, s.x[0]) || !s.x[1]) return einval;
        const auto value = m.read32(s.x[0] + attr_inherit);
        m.write32(s.x[1], value ? value : 1);
        return 0;
    });

    // int pthread_cond_timedwait_relative_np(pthread_cond_t*, pthread_mutex_t*, const struct timespec*)
    // The relative wait becomes pthread_cond_timedwait's absolute
    // CLOCK_REALTIME deadline.
    p.libsystem("_pthread_cond_timedwait_relative_np", [p, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[2]) return p.call(libsystem_library, "_pthread_cond_wait", {s.x[0], s.x[1]});
        const auto seconds = static_cast<std::int64_t>(m.read64(s.x[2]));
        const auto nanos = static_cast<std::int64_t>(m.read64(s.x[2] + 8));
        if (seconds < 0 || nanos < 0 || nanos >= 1'000'000'000) return einval;
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto deadline = now + seconds * 1'000'000'000 + nanos;
        const auto spec = process.allocate_guest(16);
        m.write64(spec, static_cast<std::uint64_t>(deadline / 1'000'000'000));
        m.write64(spec + 8, static_cast<std::uint64_t>(deadline % 1'000'000'000));
        std::uint64_t result{};
        try { result = p.call(libsystem_library, "_pthread_cond_timedwait", {s.x[0], s.x[1], spec}); }
        catch (...) { process.release_guest(spec); throw; }
        process.release_guest(spec);
        return result;
    });

    // int proc_name(int pid, void* buffer, uint32_t size): the process name
    // (the executable's file name, at most 2 * MAXCOMLEN), or 0 with errno.
    p.libsystem("_proc_name", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1] || static_cast<std::uint32_t>(s.x[2]) == 0) throw Errno{einval};
        const auto path = p.process().allocate_guest(4096);
        std::uint64_t length{};
        try { length = p.call(libsystem_library, "_proc_pidpath", {s.x[0], path, 4096}); }
        catch (...) { p.process().release_guest(path); throw; }
        if (static_cast<std::int32_t>(length) <= 0) { p.process().release_guest(path); throw Errno{p.guest_errno() ? p.guest_errno() : esrch}; }
        auto name = base_name(guest_string(m, path, 4096));
        p.process().release_guest(path);
        if (name.size() > 32) name.resize(32);
        const auto size = static_cast<std::uint32_t>(s.x[2]);
        if (name.size() + 1 > size) name.resize(size - 1);
        put_text(m, s.x[1], name);
        return name.size();
    }, 0);

    // key_t ftok(const char* path, int id): Darwin's key from the file's
    // device and inode numbers.
    p.libsystem("_ftok", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto info = p.process().allocate_guest(144);
        std::uint64_t result{};
        try { result = p.call(libsystem_library, "_stat", {s.x[0], info}); }
        catch (...) { p.process().release_guest(info); throw; }
        if (static_cast<std::int32_t>(result) != 0) { p.process().release_guest(info); throw Errno{p.guest_errno()}; }
        const std::uint64_t device = m.read32(info), inode = m.read64(info + 8);
        p.process().release_guest(info);
        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(s.x[1])) << 24) | ((device & 0xffU) << 16) | (inode & 0xffffU);
        return key;
    });

    // int getgrgid_r(gid_t, struct group*, char* buffer, size_t, struct group** result)
    // int getgrnam_r(const char* name, struct group*, char* buffer, size_t, struct group** result)
    // The group database holds the virtual identity's primary group
    // (gid 1000 "alchemy", member "alchemy"), matching getgid/getpwuid.
    const auto group_r = [](bool by_name) {
        return [by_name](CpuState& s, GuestMemory& m) -> std::uint64_t {
            if (!s.x[4] || !s.x[1] || !s.x[2]) return einval;
            m.write64(s.x[4], 0);
            if (by_name ? (!s.x[0] || guest_string(m, s.x[0], 256) != "alchemy") : static_cast<std::uint32_t>(s.x[0]) != 1000) return 0;
            // name, passwd, member, member list (two pointers).
            const std::string name = "alchemy", password = "*";
            const auto needed = name.size() + 1 + password.size() + 1 + name.size() + 1 + 7 + 16;
            if (needed > s.x[3]) return erange;
            const auto name_at = s.x[2], password_at = name_at + name.size() + 1, member_at = password_at + password.size() + 1;
            const auto list_at = (member_at + name.size() + 1 + 7) & ~std::uint64_t{7};
            put_text(m, name_at, name);
            put_text(m, password_at, password);
            put_text(m, member_at, name);
            m.write64(list_at, member_at);
            m.write64(list_at + 8, 0);
            m.write64(s.x[1], name_at);
            m.write64(s.x[1] + 8, password_at);
            m.write32(s.x[1] + 16, 1000);
            m.write32(s.x[1] + 20, 0);
            m.write64(s.x[1] + 24, list_at);
            m.write64(s.x[4], s.x[1]);
            return 0;
        };
    };
    p.libsystem("_getgrgid_r", group_r(false));
    p.libsystem("_getgrnam_r", group_r(true));

    // ssize_t recvmsg_x(int, const struct msghdr_x*, u_int count, int flags)
    // ssize_t sendmsg_x(int, const struct msghdr_x*, u_int count, int flags)
    // Batches of recvmsg/sendmsg: struct msghdr_x is struct msghdr (48 bytes)
    // plus msg_datalen (size_t, offset 48), so each element is handed to the
    // process's own recvmsg/sendmsg. The first message waits as the socket
    // does; later ones are taken only while immediately possible
    // (MSG_DONTWAIT). The result is the number of messages transferred; an
    // error on the first one is the call's error.
    for (const auto sending : {false, true})
        p.libsystem(sending ? "_sendmsg_x" : "_recvmsg_x", [p, sending](CpuState& s, GuestMemory& m) -> std::uint64_t {
            constexpr std::uint64_t msghdr_x_size = 56, msg_dontwait = 0x80;
            const auto count = static_cast<std::uint32_t>(s.x[2]);
            if (!s.x[1] && count) throw Errno{efault};
            std::uint64_t done = 0;
            for (; done < count; ++done) {
                const auto header = s.x[1] + done * msghdr_x_size;
                const auto flags = static_cast<std::uint32_t>(s.x[3]) | (done ? msg_dontwait : 0);
                const auto moved = static_cast<std::int64_t>(p.call(libsystem_library, sending ? "_sendmsg" : "_recvmsg", {s.x[0], header, flags}));
                if (moved < 0) {
                    if (!done) throw Errno{p.guest_errno()};
                    break;
                }
                m.write64(header + 48, static_cast<std::uint64_t>(moved));
            }
            return done;
        });

    // int mlock(const void*, size_t) / munlock: alchemy-fx guest addresses are
    // host addresses, so the host locks the same pages in physical memory.
    // A request beyond the host's limit fails with EAGAIN as Darwin does
    // past its wired-memory limit.
    p.libsystem("_mlock", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1]) return 0;
        static_cast<void>(m.read8(s.x[0]));
        if (const auto locked = host::vm::lock(s.x[0], static_cast<std::size_t>(s.x[1])); !locked)
            throw Errno{locked.error() == host::Error::would_block ? eagain : enomem};
        return 0;
    });
    p.libsystem("_munlock", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1]) return 0;
        static_cast<void>(m.read8(s.x[0]));
        if (!host::vm::unlock(s.x[0], static_cast<std::size_t>(s.x[1]))) throw Errno{enomem};
        return 0;
    });

    // kern_return_t vm_map(task, vm_address_t* address, vm_size_t size, vm_address_t mask,
    //                      int flags, mem_entry_name_port_t object, vm_offset_t offset,
    //                      boolean_t copy, vm_prot_t cur, vm_prot_t max, vm_inherit_t)
    // Anonymous memory (object MEMORY_OBJECT_NULL) through vm_allocate; mapping
    // a memory object is unsupported (KERN_INVALID_ARGUMENT). An internal name:
    // libSystem's vm_map is mach/mach_exports.cpp's, which maps memory
    // entries itself and calls this one for anonymous memory.
    p.function("alchemy.internal", "__alchemy_vm_map_anonymous", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto task = s.x[0], address = s.x[1], size = s.x[2], mask = s.x[3];
        const auto flags = static_cast<std::uint32_t>(s.x[4]);
        const auto object = static_cast<std::uint32_t>(s.x[5]);
        const auto current = static_cast<std::uint32_t>(m.read32(s.sp));
        if (object != 0) return kern_invalid_argument;
        // VM_FLAGS_ANYWHERE; the alias tag (top byte) only labels the region.
        if ((flags & 0x00ffffffU) & ~1U) return kern_invalid_argument;
        const auto result = static_cast<std::uint32_t>(p.call(libsystem_library, "_vm_allocate", {task, address, size, flags & 1U}));
        if (result != kern_success) return result;
        const auto base = m.read64(address);
        if (mask && (base & mask)) {
            static_cast<void>(p.call(libsystem_library, "_vm_deallocate", {task, base, size}));
            return kern_no_space;
        }
        if ((current & 7U) != 3U) {
            const auto protect = static_cast<std::uint32_t>(p.call(libsystem_library, "_vm_protect", {task, base, size, 0, current & 7U}));
            if (protect != kern_success) {
                static_cast<void>(p.call(libsystem_library, "_vm_deallocate", {task, base, size}));
                return protect;
            }
        }
        return kern_success;
    }, kern_failure);

    // void dispatch_once(dispatch_once_t*, dispatch_block_t): dispatch_once_f
    // with the block's invoke function.
    p.libsystem("_dispatch_once", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1]) throw std::runtime_error("Alchemy dispatch_once: null block");
        return p.call(libsystem_library, "_dispatch_once_f", {s.x[0], s.x[1], m.read64(s.x[1] + 16)});
    });
    // int atexit_b(void (^)(void)): a heap copy of the block runs at exit.
    p.libsystem("_atexit_b", [p, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) throw Errno{einval};
        const auto copy = p.call(libsystem_library, "__Block_copy", {s.x[0]});
        if (!copy) throw Errno{enomem};
        process.register_finalizer(m.read64(copy + 16), copy, 0);
        return 0;
    });

    // kern_return_t host_processor_info(host, PROCESSOR_CPU_LOAD_INFO (2),
    //     natural_t* count, processor_info_array_t* info, mach_msg_type_number_t* info_count):
    // per-processor {user, system, idle, nice} ticks (CLK_TCK 100) in memory
    // the caller releases with vm_deallocate.
    p.libsystem("_host_processor_info", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (static_cast<std::uint32_t>(s.x[1]) != 2) return kern_invalid_argument;
        if (!s.x[2] || !s.x[3] || !s.x[4]) return kern_invalid_argument;
        const auto processors = host::system::processor_times();
        if (!processors || processors->empty()) return kern_failure;
        const auto& times = *processors;
        const auto bytes = times.size() * 16;
        const auto slot = p.process().allocate_guest(8);
        // mach_task_self() reads the mach_task_self_ variable.
        const auto task_variable = p.process().resolve(libsystem_library, "_mach_task_self_");
        if (!task_variable) return kern_failure;
        const auto task = m.read32(task_variable->address);
        const auto result = static_cast<std::uint32_t>(p.call(libsystem_library, "_vm_allocate", {task, slot, bytes, 1}));
        const auto array = m.read64(slot);
        p.process().release_guest(slot);
        if (result != kern_success) return result;
        for (std::size_t index = 0; index < times.size(); ++index) {
            const auto& t = times[index];
            const auto ticks = [](std::uint64_t nanoseconds) { return static_cast<std::uint32_t>(nanoseconds / 10'000'000); };
            const auto at = array + index * 16;
            m.write32(at, ticks(t.user_ns));
            m.write32(at + 4, ticks(t.system_ns));
            m.write32(at + 8, ticks(t.idle_ns));
            m.write32(at + 12, 0);
        }
        m.write32(s.x[2], static_cast<std::uint32_t>(times.size()));
        m.write64(s.x[3], array);
        m.write32(s.x[4], static_cast<std::uint32_t>(times.size() * 4));
        return kern_success;
    }, kern_failure);

    // kern_return_t thread_info(thread_act_t, THREAD_BASIC_INFO (3), thread_info_t, count*)
    // for the calling thread (its host thread's CPU times); other threads'
    // accounting is not available (KERN_INVALID_ARGUMENT).
    p.libsystem("_thread_info", [&process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto own = static_cast<std::uint32_t>(process.current_thread()->identity);
        if (static_cast<std::uint32_t>(s.x[0]) != own) return kern_invalid_argument;
        if (static_cast<std::uint32_t>(s.x[1]) != 3 || !s.x[2] || !s.x[3]) return kern_invalid_argument;
        if (m.read32(s.x[3]) < 10) return kern_invalid_argument;
        const auto usage = host::process::thread_usage();
        if (!usage) return kern_failure;
        m.fill_bytes(s.x[2], 0, 40);
        time_value(m, s.x[2], usage->user_ns);
        time_value(m, s.x[2] + 8, usage->system_ns);
        m.write32(s.x[2] + 20, 1);    // policy POLICY_TIMESHARE
        m.write32(s.x[2] + 24, 1);    // run_state TH_STATE_RUNNING (it is running this call)
        m.write32(s.x[3], 10);
        return kern_success;
    }, kern_failure);

    // ---- os_activity ----
    // os_log objects, _os_log_default, os_log_create, os_log_type_enabled,
    // _os_log_impl, os_retain and os_release: misc/oslog.cpp, whose
    // os_retain/os_release hand other os objects (these activities) to the
    // internal retain/release below.
    auto objects = std::make_shared<OsObjects>(process);
    // Activities have identifiers (and their parent's) and nest: each thread keeps a stack of the
    // active ones (os_activity_scope_enter/leave, _os_activity_initiate, os_activity_apply).
    // OS_ACTIVITY_CURRENT (&_os_activity_current) is the calling thread's innermost activity and
    // OS_ACTIVITY_NONE (&_os_activity_none) none. Nothing consumes them (there is no tracing).
    struct Active {
        std::mutex mutex;
        std::map<std::uint64_t, std::vector<std::uint64_t>> threads;   // thread identity -> activity addresses
        std::uint64_t next_id{1};
    };
    const auto active = std::make_shared<Active>();
    const auto current_cell = p.data(libsystem_library, "__os_activity_current", 16);
    const auto none_cell = p.data(libsystem_library, "__os_activity_none", 16);
    const auto thread = [&process] { const auto value = process.current_thread(); return value ? value->identity : std::uint64_t{0}; };
    // The activity an os_activity_t names (0: none).
    const auto resolve = [active, thread, current_cell, none_cell](std::uint64_t activity) -> std::uint64_t {
        if (activity == none_cell) return 0;
        if (activity != current_cell) return activity;
        std::lock_guard lock(active->mutex);
        const auto found = active->threads.find(thread());
        return found == active->threads.end() || found->second.empty() ? 0 : found->second.back();
    };
    const auto identifier = [objects](std::uint64_t activity) -> std::uint64_t {
        const auto found = activity ? objects->find(activity) : std::nullopt;
        return found ? found->id : 0;
    };
    const auto create = [objects, active, resolve, identifier](std::string description, std::uint64_t parent) {
        OsObject value;
        value.category = std::move(description);
        value.parent = identifier(resolve(parent));
        { std::lock_guard lock(active->mutex); value.id = active->next_id++; }
        return objects->create(std::move(value));
    };
    const auto enter = [active, thread](std::uint64_t activity) {
        std::lock_guard lock(active->mutex);
        auto& stack = active->threads[thread()];
        stack.push_back(activity);
        return stack.size();
    };
    const auto leave = [active, thread](std::size_t depth) {
        std::lock_guard lock(active->mutex);
        auto& stack = active->threads[thread()];
        if (depth && stack.size() >= depth) stack.resize(depth - 1);
    };
    const auto run_block = [&process](std::uint64_t block) {
        const std::array<std::uint64_t, 1> arguments{block};
        static_cast<void>(EngineAdapter(process.calling_engine()).call(process.calling_engine().read64(block + 16), arguments));
    };
    // os_activity_t _os_activity_create(void* dso, const char* description, os_activity_t parent, flags)
    p.libsystem("__os_activity_create", [create](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return create(s.x[1] ? guest_string(m, s.x[1], 1024) : std::string(), s.x[2]);
    }, 0);
    // void os_activity_scope_enter(os_activity_t, struct os_activity_scope_state_s* state): the state
    // remembers the activity and the depth it entered at.
    p.libsystem("_os_activity_scope_enter", [resolve, enter](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto activity = resolve(s.x[0]);
        const auto depth = enter(activity);
        if (s.x[1]) { m.write64(s.x[1], activity); m.write64(s.x[1] + 8, depth); }
        return 0;
    });
    p.libsystem("_os_activity_scope_leave", [leave](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[0]) leave(static_cast<std::size_t>(m.read64(s.x[0] + 8)));
        return 0;
    });
    // _os_activity_initiate(dso, description, flags, block) / _f(dso, description, flags, context, function):
    // the work runs synchronously inside a new activity.
    p.libsystem("__os_activity_initiate", [objects, create, enter, leave, run_block](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto activity = create(s.x[1] ? guest_string(m, s.x[1], 1024) : std::string(), 0);
        const auto depth = enter(activity);
        try { run_block(s.x[3]); } catch (...) { leave(depth); static_cast<void>(objects->release(activity)); throw; }
        leave(depth);
        static_cast<void>(objects->release(activity));
        return 0;
    });
    p.libsystem("__os_activity_initiate_f", [&process, objects, create, enter, leave](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto activity = create(s.x[1] ? guest_string(m, s.x[1], 1024) : std::string(), 0);
        const auto depth = enter(activity);
        const std::array<std::uint64_t, 1> arguments{s.x[3]};
        try { static_cast<void>(EngineAdapter(process.calling_engine()).call(s.x[4], arguments)); }
        catch (...) { leave(depth); static_cast<void>(objects->release(activity)); throw; }
        leave(depth);
        static_cast<void>(objects->release(activity));
        return 0;
    });
    // os_activity_apply(activity, block) / os_activity_apply_f(activity, context, function)
    p.libsystem("_os_activity_apply", [resolve, enter, leave, run_block](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto depth = enter(resolve(s.x[0]));
        try { run_block(s.x[1]); } catch (...) { leave(depth); throw; }
        leave(depth);
        return 0;
    });
    p.libsystem("_os_activity_apply_f", [&process, resolve, enter, leave](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto depth = enter(resolve(s.x[0]));
        const std::array<std::uint64_t, 1> arguments{s.x[1]};
        try { static_cast<void>(EngineAdapter(process.calling_engine()).call(s.x[2], arguments)); } catch (...) { leave(depth); throw; }
        leave(depth);
        return 0;
    });
    // os_activity_id_t os_activity_get_identifier(os_activity_t, os_activity_id_t* parent_id)
    p.libsystem("_os_activity_get_identifier", [objects, resolve](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto activity = resolve(s.x[0]);
        const auto found = activity ? objects->find(activity) : std::nullopt;
        if (s.x[1]) m.write64(s.x[1], found ? found->parent : 0);
        return found ? found->id : 0;
    }, 0);
    // unsigned os_activity_get_active(os_activity_id_t* entries, unsigned* count): the calling
    // thread's active activities, outermost first, up to *count; *count becomes their number.
    p.libsystem("_os_activity_get_active", [active, thread, identifier](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::vector<std::uint64_t> stack;
        {
            std::lock_guard lock(active->mutex);
            if (const auto found = active->threads.find(thread()); found != active->threads.end()) stack = found->second;
        }
        const auto room = s.x[1] ? m.read32(s.x[1]) : 0;
        for (std::size_t index = 0; index < stack.size() && index < room && s.x[0]; ++index) m.write64(s.x[0] + index * 8, identifier(stack[index]));
        if (s.x[1]) m.write32(s.x[1], static_cast<std::uint32_t>(stack.size()));
        return stack.size();
    }, 0);
    // _os_activity_label_useraction(const char*): labels the user action an activity belongs to; with
    // no tracing consumer there is nothing to label.
    p.libsystem("__os_activity_label_useraction", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });

    // ---- os_workgroup (<os/workgroup.h>) ----
    // Workgroups are os objects (os_retain/os_release, the activities' store). Threads join and
    // leave with a token (one workgroup per thread at a time: EALREADY); a cancelled workgroup
    // refuses joins (EINVAL) and testcancel reports it. Intervals (OS_CLOCK_MACH_ABSOLUTE_TIME only)
    // track start/finish. The working arena is kept with each joined thread's index. The scheduler
    // gets no hint from membership (Windows has no thread workgroups), so joining only records it.
    struct Group { std::uint64_t port{}; bool cancelled{}; };   // shared by workgroups made from one port
    struct Workgroup {
        std::shared_ptr<Group> group;
        bool interval{}, running{};
        std::uint64_t arena{}, arena_destructor{};
        std::uint32_t max_workers{};
        std::map<std::uint64_t, std::uint32_t> members;   // thread identity -> working-arena index
    };
    struct Workgroups { std::mutex mutex; std::map<std::uint64_t, Workgroup> values; std::map<std::uint64_t, std::shared_ptr<Group>> ports; };
    const auto workgroups = std::make_shared<Workgroups>();
    constexpr std::uint32_t join_token_signature = 0x4a4f494e;   // the token's signature word ("JOIN")
    const auto make_workgroup = [objects, workgroups](std::string name, std::shared_ptr<Group> group, bool interval) {
        OsObject value;
        value.category = std::move(name);
        const auto address = objects->create(std::move(value));
        std::lock_guard lock(workgroups->mutex);
        workgroups->values[address] = Workgroup{group ? std::move(group) : std::make_shared<Group>(), interval};
        return address;
    };
    const auto with = [workgroups](std::uint64_t address, auto action) -> std::uint64_t {
        std::lock_guard lock(workgroups->mutex);
        const auto found = workgroups->values.find(address);
        if (found == workgroups->values.end()) throw std::runtime_error("Alchemy: an os_workgroup call on an object that is not a workgroup");
        return action(found->second);
    };
    // os_workgroup_t os_workgroup_create_with_port(const char* name, mach_port_t) / _copy_port(wg, mach_port_t*)
    p.libsystem("_os_workgroup_create_with_port", [make_workgroup, workgroups](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::shared_ptr<Group> group;
        {
            std::lock_guard lock(workgroups->mutex);
            if (const auto found = workgroups->ports.find(static_cast<std::uint32_t>(s.x[1])); found != workgroups->ports.end()) group = found->second;
        }
        if (!group) throw Errno{einval};   // not a port os_workgroup_copy_port gave out
        return make_workgroup(s.x[0] ? guest_string(m, s.x[0], 256) : std::string(), group, false);
    }, 0);
    p.libsystem("_os_workgroup_copy_port", [p, with, workgroups](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1]) return einval;
        auto port = with(s.x[0], [](Workgroup& w) { return w.group->port; });
        if (!port) {
            // A receive right names the workgroup across copy_port/create_with_port.
            const auto task = m.read32(p.process().resolve(libsystem_library, "_mach_task_self_")->address);
            const auto slot = p.malloc(4);
            m.write32(slot, 0);
            const auto result = p.call(libsystem_library, "_mach_port_allocate", {task, 1, slot});
            port = m.read32(slot);
            static_cast<void>(p.call(libsystem_library, "_free", {slot}));
            if (result != 0 || !port) return einval;
            std::lock_guard lock(workgroups->mutex);
            auto& group = workgroups->values.at(s.x[0]).group;
            if (group->port) port = group->port; else { group->port = port; workgroups->ports[port] = group; }
        }
        m.write32(s.x[1], static_cast<std::uint32_t>(port));
        return 0;
    });
    // os_workgroup_t os_workgroup_create_with_workgroup(const char* name, os_workgroup_t wg): the same group.
    p.libsystem("_os_workgroup_create_with_workgroup", [make_workgroup, with](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::shared_ptr<Group> shared;
        bool interval{};
        static_cast<void>(with(s.x[1], [&](Workgroup& w) { shared = w.group; interval = w.interval; return std::uint64_t{0}; }));
        return make_workgroup(s.x[0] ? guest_string(m, s.x[0], 256) : std::string(), shared, interval);
    }, 0);
    // os_workgroup_interval_t os_workgroup_interval_create(const char* name, os_clockid_t, os_workgroup_attr_t)
    p.libsystem("_os_workgroup_interval_create", [make_workgroup](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (static_cast<std::uint32_t>(s.x[1]) != 32) throw Errno{einval};   // OS_CLOCK_MACH_ABSOLUTE_TIME
        return make_workgroup(s.x[0] ? guest_string(m, s.x[0], 256) : std::string(), nullptr, true);
    }, 0);
    // int os_workgroup_join(os_workgroup_t, os_workgroup_join_token_t) / void os_workgroup_leave(wg, token)
    p.libsystem("_os_workgroup_join", [with, workgroups, thread](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1]) return einval;
        const auto self = thread();
        {
            std::lock_guard lock(workgroups->mutex);
            for (const auto& [address, value] : workgroups->values) if (value.members.contains(self)) return 37;   // EALREADY
        }
        const auto result = with(s.x[0], [self](Workgroup& w) -> std::uint64_t {
            if (w.group->cancelled) return einval;
            w.members.emplace(self, static_cast<std::uint32_t>(w.members.size()));
            return 0;
        });
        if (result) return result;
        m.fill_bytes(s.x[1], 0, 40);   // struct os_workgroup_join_token_opaque_s
        m.write32(s.x[1], join_token_signature);
        m.write64(s.x[1] + 8, s.x[0]);
        m.write64(s.x[1] + 16, self);
        return 0;
    });
    p.libsystem("_os_workgroup_leave", [with, thread](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1] || m.read32(s.x[1]) != join_token_signature || m.read64(s.x[1] + 8) != s.x[0] || m.read64(s.x[1] + 16) != thread())
            throw std::runtime_error("Alchemy: os_workgroup_leave with a token this thread did not get from os_workgroup_join on that workgroup");
        static_cast<void>(with(s.x[0], [&](Workgroup& w) { w.members.erase(thread()); return std::uint64_t{0}; }));
        m.write32(s.x[1], 0);
        return 0;
    });
    p.libsystem("_os_workgroup_cancel", [with](CpuState& s, GuestMemory&) -> std::uint64_t {
        return with(s.x[0], [](Workgroup& w) { w.group->cancelled = true; return std::uint64_t{0}; });
    });
    p.libsystem("_os_workgroup_testcancel", [with](CpuState& s, GuestMemory&) -> std::uint64_t {
        return with(s.x[0], [](Workgroup& w) -> std::uint64_t { return w.group->cancelled ? 1 : 0; });
    }, 0);
    // int os_workgroup_set_working_arena(wg, void* arena, uint32_t max_workers, destructor) /
    // void* os_workgroup_get_working_arena(wg, os_workgroup_index* index_out)
    p.libsystem("_os_workgroup_set_working_arena", [p, with](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::uint64_t previous{}, destructor{};
        const auto result = with(s.x[0], [&](Workgroup& w) -> std::uint64_t {
            if (!w.members.empty()) return 16;   // EBUSY: threads have joined
            previous = std::exchange(w.arena, s.x[1]);
            destructor = std::exchange(w.arena_destructor, s.x[3]);
            w.max_workers = static_cast<std::uint32_t>(s.x[2]);
            return 0;
        });
        if (!result && previous && destructor) {
            const std::array<std::uint64_t, 1> arguments{previous};
            static_cast<void>(EngineAdapter(p.process().calling_engine()).call(destructor, arguments));
        }
        return result;
    });
    p.libsystem("_os_workgroup_get_working_arena", [with, thread](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return with(s.x[0], [&](Workgroup& w) -> std::uint64_t {
            const auto found = w.members.find(thread());
            if (s.x[1]) m.write32(s.x[1], found == w.members.end() ? 0 : found->second);
            return w.arena;
        });
    }, 0);
    // int os_workgroup_max_parallel_threads(wg, os_workgroup_mpt_attr_t): the online processors.
    p.libsystem("_os_workgroup_max_parallel_threads", [p, with](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(with(s.x[0], [](Workgroup&) { return std::uint64_t{0}; }));
        return p.call(libsystem_library, "_sysconf", {58});   // _SC_NPROCESSORS_ONLN
    });
    // int os_workgroup_interval_start(wg, uint64_t start, uint64_t deadline, data) / _finish(wg, data)
    p.libsystem("_os_workgroup_interval_start", [with](CpuState& s, GuestMemory&) -> std::uint64_t {
        return with(s.x[0], [&](Workgroup& w) -> std::uint64_t {
            if (!w.interval || w.running || w.group->cancelled || s.x[2] < s.x[1]) return einval;
            w.running = true;
            return 0;
        });
    });
    p.libsystem("_os_workgroup_interval_finish", [with](CpuState& s, GuestMemory&) -> std::uint64_t {
        return with(s.x[0], [](Workgroup& w) -> std::uint64_t {
            if (!w.interval || !w.running) return einval;
            w.running = false;
            return 0;
        });
    });
    // os_retain / os_release of the activities above (internal names; the
    // libSystem exports are misc/oslog.cpp's, which call these).
    static_cast<void>(process.install_function("__alchemy_os_activity_retain", [objects](CpuState& s, GuestMemory&) {
        if (s.x[0] && !objects->retain(s.x[0]))
            throw std::runtime_error("Alchemy os_retain: not an os_log or os_activity object");
    }, "alchemy.internal"));
    static_cast<void>(process.install_function("__alchemy_os_activity_release", [objects](CpuState& s, GuestMemory&) {
        if (s.x[0] && !objects->release(s.x[0]))
            throw std::runtime_error("Alchemy os_release: not an os_log or os_activity object");
        s.x[0] = 0;
    }, "alchemy.internal"));
}

} // namespace alchemy::export_fill
