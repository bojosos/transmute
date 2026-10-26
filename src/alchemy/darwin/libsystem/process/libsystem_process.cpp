#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/system.hpp"

#include "alchemy/darwin_services.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

// Process, Mach host and dyld queries a renderer plug-in imports: libproc (proc_listpids),
// host_statistics,
// vm_page_size, dyld image lookups by address, plus small POSIX leftovers
// (issetugid, getlogin_r, mktemp, clonefile, freopen, pthread
// scheduling/QoS attributes, __ulock_wait2). _NSGetArgc/_NSGetArgv, host_info
// and termios are misc/crt_and_host_info.cpp's; proc_pidpath and
// execvp are DarwinServices'.
namespace alchemy::export_fill {
namespace {

// <mach/kern_return.h>
constexpr std::uint64_t kern_success = 0, kern_invalid_argument = 4, kern_failure = 5;
// <mach/host_info.h> flavors.
constexpr std::uint32_t host_load_info = 1, host_vm_info = 2, host_cpu_load_info = 3;
// <libproc.h>
constexpr std::uint32_t proc_all_pids = 1, proc_uid_only = 4, proc_ruid_only = 5, proc_ppid_only = 6;
// <pthread/qos.h>
constexpr std::uint64_t qos_classes[] = {0x21, 0x19, 0x15, 0x11, 0x09, 0x00};
constexpr std::uint64_t pthread_attr_magic = 0x5448524441545452ULL;   // darwin_threads' initialized attribute
// <sched.h>
constexpr std::uint32_t sched_other = 1, sched_rr = 2, sched_fifo = 4;
constexpr std::int32_t default_priority = 31;

struct MemoryStatus { std::uint64_t total{}, available{}; };
MemoryStatus memory_status() {
    if (const auto memory = host::system::memory()) return {memory->physical, memory->available};
    return {};
}

} // namespace

void install_libsystem_process(Provider& p) {
    auto& process = p.process();

    // vm_size_t vm_page_size: Darwin arm64 pages are 16 KiB.
    {
        std::array<std::uint8_t, 8> bytes{};
        const auto size = darwin_arm64_page_size;
        std::memcpy(bytes.data(), &size, 8);
        static_cast<void>(p.data(libsystem_library, "_vm_page_size", 8, bytes));
    }

    // int proc_listpids(uint32_t type, uint32_t typeinfo, void* buffer, int size):
    // bytes of pid_t written, or the bytes needed when buffer is NULL.
    p.libsystem("_proc_listpids", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto type = static_cast<std::uint32_t>(s.x[0]), info = static_cast<std::uint32_t>(s.x[1]);
        std::vector<std::uint32_t> pids;
        const auto processes = host::process::list();
        if (!processes) throw Errno{eperm};
        for (const auto& entry : *processes) {
            if (type == proc_ppid_only && entry.parent != info) continue;
            pids.push_back(entry.pid);
        }
        // One user owns every visible process (Alchemy reports a single uid).
        if (type != proc_all_pids && type != proc_uid_only && type != proc_ruid_only && type != proc_ppid_only) throw Errno{einval};
        const auto needed = pids.size() * 4;
        if (!s.x[2]) return needed + 64 * 4;   // room for processes started meanwhile
        const auto capacity = static_cast<std::uint64_t>(std::max<std::int64_t>(0, static_cast<std::int32_t>(s.x[3]))) / 4;
        const auto count = std::min<std::uint64_t>(capacity, pids.size());
        for (std::uint64_t index = 0; index < count; ++index) m.write32(s.x[2] + index * 4, pids[static_cast<std::size_t>(index)]);
        return count * 4;
    }, 0);

    // int proc_listallpids(void* buffer, int size) and proc_listchildpids(pid, buffer, size): the
    // number of pids (not bytes) written, or the number there are when buffer is NULL.
    const auto list_pids = [](std::optional<std::uint32_t> parent, std::uint64_t buffer, std::uint64_t size, GuestMemory& m) -> std::uint64_t {
        const auto processes = host::process::list();
        if (!processes) throw Errno{eperm};
        std::vector<std::uint32_t> pids;
        for (const auto& entry : *processes) if (!parent || entry.parent == *parent) pids.push_back(entry.pid);
        if (!buffer) return pids.size() + (parent ? 0 : 64);   // room for processes started meanwhile
        const auto capacity = static_cast<std::uint64_t>(std::max<std::int64_t>(0, static_cast<std::int32_t>(size))) / 4;
        const auto count = std::min<std::uint64_t>(capacity, pids.size());
        for (std::uint64_t index = 0; index < count; ++index) m.write32(buffer + index * 4, pids[static_cast<std::size_t>(index)]);
        return count;
    };
    p.libsystem("_proc_listallpids", [list_pids](CpuState& s, GuestMemory& m) { return list_pids(std::nullopt, s.x[0], s.x[1], m); });
    p.libsystem("_proc_listchildpids", [list_pids](CpuState& s, GuestMemory& m) {
        return list_pids(static_cast<std::uint32_t>(s.x[0]), s.x[1], s.x[2], m);
    });

    // int proc_pidfdinfo(pid, fd, flavor, buffer, size): this process's descriptors, flavors
    // PROC_PIDFDVNODEINFO (1, struct vnode_fdinfo, 176 bytes) and PROC_PIDFDVNODEPATHINFO
    // (2, struct vnode_fdinfowithpath, 1200 bytes): struct proc_fileinfo (24 bytes: fi_openflags,
    // fi_status, fi_offset, fi_type, fi_guardflags), then vnode_info (vinfo_stat 136, vi_type,
    // vi_pad, vi_fsid) and the path. Other processes' descriptors are not visible (EPERM); the
    // socket, pipe, kqueue and other flavors are not offered. Returns the bytes written, 0 on error.
    p.libsystem("_proc_pidfdinfo", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto pid = static_cast<std::uint32_t>(s.x[0]), flavor = static_cast<std::uint32_t>(s.x[2]);
        const std::uint64_t fd = s.x[1];
        const std::uint64_t buffer = s.x[3];
        const auto size = static_cast<std::int32_t>(s.x[4]);
        if (pid != static_cast<std::uint32_t>(p.call(libsystem_library, "_getpid", {}))) throw Errno{eperm};
        if (flavor != 1 && flavor != 2) throw Errno{enotsup};
        const std::uint64_t needed = flavor == 2 ? 1200 : 176;
        if (!buffer || size < static_cast<std::int32_t>(needed)) throw Errno{enomem};
        auto& process = p.process();
        const auto info = process.allocate_guest(144), path = process.allocate_guest(1024);
        const auto release = [&] { process.release_guest(info); process.release_guest(path); };
        try {
            if (static_cast<std::int32_t>(p.call(libsystem_library, "_fstat", {fd, info})) != 0) throw Errno{p.guest_errno()};
            const auto mode = static_cast<std::uint16_t>(m.read8(info + 4) | m.read8(info + 5) << 8);
            const auto type = mode & 0170000;
            if (type == 0140000 || type == 0010000) throw Errno{ebadf};   // a socket or pipe is not a vnode
            CallRegs flags{}; flags.x[0] = fd; flags.x[1] = 3;   // F_GETFL
            const auto open_flags = p.call_regs(libsystem_library, "_fcntl", flags);
            const auto offset = p.call(libsystem_library, "_lseek", {fd, 0, 1});   // SEEK_CUR
            m.fill_bytes(buffer, 0, needed);
            m.write32(buffer, static_cast<std::uint32_t>(open_flags) + 1);   // fi_openflags: FREAD/FWRITE are O_ACCMODE + 1
            m.write64(buffer + 8, static_cast<std::int64_t>(offset) < 0 ? 0 : offset);
            m.write32(buffer + 16, 1);   // fi_type DTYPE_VNODE
            // vinfo_stat from struct stat (64-bit inodes): st_dev @0, st_mode @4, st_nlink @6,
            // st_ino @8, st_uid @16, st_gid @20, st_rdev @24, times @32 (atime, mtime, ctime,
            // birthtime timespecs), st_size @96, st_blocks @104, st_blksize @112, st_flags @116, st_gen @120.
            const auto stat = buffer + 24;
            m.write32(stat, m.read32(info));
            m.write32(stat + 4, m.read32(info + 4));   // vst_mode, vst_nlink
            m.write64(stat + 8, m.read64(info + 8));
            m.write32(stat + 16, m.read32(info + 16)); m.write32(stat + 20, m.read32(info + 20));
            for (unsigned index = 0; index < 8; ++index) m.write64(stat + 24 + index * 8, m.read64(info + 32 + index * 8));
            m.write64(stat + 88, m.read64(info + 96)); m.write64(stat + 96, m.read64(info + 104));
            m.write32(stat + 104, m.read32(info + 112)); m.write32(stat + 108, m.read32(info + 116));
            m.write32(stat + 112, m.read32(info + 120)); m.write32(stat + 116, m.read32(info + 24));
            // vi_type: VREG 1, VDIR 2, VBLK 3, VCHR 4, VLNK 5, VFIFO 7.
            const std::uint32_t vnode_type = type == 0100000 ? 1 : type == 0040000 ? 2 : type == 0060000 ? 3 : type == 0020000 ? 4 : type == 0120000 ? 5 : 0;
            m.write32(stat + 136, vnode_type);
            if (flavor == 2) {
                CallRegs query{}; query.x[0] = fd; query.x[1] = 50; query.push_variadic(path);   // F_GETPATH
                if (static_cast<std::int32_t>(p.call_regs(libsystem_library, "_fcntl", query)) == 0) {
                    const auto text = guest_string(m, path, 1024);
                    put_string(m, buffer + 176, text);
                }
            }
        } catch (...) { release(); throw; }
        release();
        return needed;
    }, 0);

    // int proc_signal_with_audittoken(audit_token_t*, int signal): kill(2) to the token's pid
    // (val[5]); 0 or the errno value.
    p.libsystem("_proc_signal_with_audittoken", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) return einval;
        const auto pid = m.read32(s.x[0] + 20);
        if (static_cast<std::int32_t>(p.call(libsystem_library, "_kill", {pid, s.x[1]})) != 0) return p.guest_errno();
        return 0;
    });

    // kern_return_t host_statistics(host_t, host_flavor_t, host_info_t, mach_msg_type_number_t*)
    p.libsystem("_host_statistics", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto flavor = static_cast<std::uint32_t>(s.x[1]);
        const auto out = s.x[2], count_address = s.x[3];
        if (!out || !count_address) return kern_invalid_argument;
        const auto capacity = m.read32(count_address);
        std::vector<std::uint32_t> words;
        if (flavor == host_load_info) {
            words.assign(6, 0);   // avenrun[3], mach_factor[3] (LOAD_SCALE 1000)
        } else if (flavor == host_vm_info) {
            const auto status = memory_status();
            const auto page = darwin_arm64_page_size;
            const auto free_pages = static_cast<std::uint32_t>(status.available / page);
            const auto used_pages = static_cast<std::uint32_t>((status.total - status.available) / page);
            // vm_statistics: free, active, inactive, wire, zero_fill, reactivations, pageins,
            // pageouts, faults, cow_faults, lookups, hits, purgeable, purges, speculative.
            words = {free_pages, used_pages, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        } else if (flavor == host_cpu_load_info) {
            const auto times = host::system::cpu_times().value_or(host::system::CpuTimes{});
            // cpu_ticks[USER, SYSTEM, IDLE, NICE] in 10 ms ticks.
            const auto tick = 10'000'000ULL;
            words = {static_cast<std::uint32_t>(times.user_ns / tick), static_cast<std::uint32_t>(times.system_ns / tick),
                     static_cast<std::uint32_t>(times.idle_ns / tick), 0};
        } else return kern_invalid_argument;
        if (capacity < words.size()) return kern_failure;
        for (std::size_t index = 0; index < words.size(); ++index) m.write32(out + index * 4, words[index]);
        m.write32(count_address, static_cast<std::uint32_t>(words.size()));
        return kern_success;
    });

    // dyld lookups by address over the loaded images.
    auto names = std::make_shared<std::pair<std::mutex, std::map<std::uint64_t, std::uint64_t>>>();
    p.libsystem("__dyld_get_image_header_containing_address", [&process](CpuState& s, GuestMemory&) -> std::uint64_t {
        for (const auto& image : process.images())
            if (s.x[0] >= image.begin && s.x[0] < image.end) return image.header;
        return 0;
    }, 0);
    p.libsystem("_dyld_image_path_containing_address", [&process, names](CpuState& s, GuestMemory&) -> std::uint64_t {
        for (const auto& image : process.images()) {
            if (s.x[0] < image.begin || s.x[0] >= image.end) continue;
            std::lock_guard lock(names->first);
            auto& text = names->second[image.header];
            if (!text) {
                text = process.allocate_guest(image.name.size() + 1);
                put_string(process.main_engine(), text, image.name);
            }
            return text;
        }
        return 0;
    }, 0);

    // A process that is never set-user-ID or set-group-ID.
    p.libsystem("_issetugid", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });

    // int getlogin_r(char* name, size_t size): the getlogin() name.
    p.libsystem("_getlogin_r", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto name = p.call(libsystem_library, "_getlogin", {});
        if (!name) return enoent;
        const auto text = guest_string(m, name, 256);
        if (text.size() + 1 > s.x[1]) return erange;
        put_string(m, s.x[0], text);
        return 0;
    });

    // char* mktemp(char* template): trailing X characters become a name that does not exist.
    p.libsystem("_mktemp", [&process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        auto text = guest_string(m, s.x[0], 1024);
        const auto end = text.size();
        auto start = end;
        while (start > 0 && text[start - 1] == 'X') --start;
        static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device device;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            for (auto index = start; index < end; ++index) text[index] = alphabet[device() % (sizeof(alphabet) - 1)];
            std::error_code error;
            if (!std::filesystem::exists(process.map_guest_path(text), error) && !error) {
                put_string(m, s.x[0], text);
                return s.x[0];
            }
            if (start == end) break;
        }
        throw Errno{eexist};
    }, 0);

    // int clonefile(const char*, const char*, int): host volumes (NTFS/ReFS
    // through Alchemy) are not APFS, so cloning is not supported (ENOTSUP),
    // which callers treat as "copy instead".
    p.libsystem("_clonefile", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        static_cast<void>(guest_string(m, s.x[0], 1024));
        static_cast<void>(guest_string(m, s.x[1], 1024));
        throw Errno{enotsup};
    });
    // int fclonefileat(int srcfd, int dst_dirfd, const char* dst, int flags): the same.
    p.libsystem("_fclonefileat", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        static_cast<void>(guest_string(m, s.x[2], 1024));
        throw Errno{enotsup};
    });


    // FILE* freopen(const char* path, const char* mode, FILE* stream): the
    // stream's descriptor is replaced by the newly opened file (dup2), as
    // Darwin does for a named path. A NULL path (mode change) is unsupported.
    p.libsystem("_freopen", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) throw Errno{einval};
        const auto mode = guest_string(m, s.x[1], 16);
        std::uint64_t flags = 0;
        const bool plus = mode.find('+') != std::string::npos;
        if (mode.starts_with('r')) flags = plus ? 2 : 0;
        else if (mode.starts_with('w')) flags = (plus ? 2 : 1) | 0x200 | 0x400;
        else if (mode.starts_with('a')) flags = (plus ? 2 : 1) | 0x200 | 0x8;
        else throw Errno{einval};
        if (mode.find('x') != std::string::npos) flags |= 0x800;
        static_cast<void>(p.call(libsystem_library, "_fflush", {s.x[2]}));
        const auto target = static_cast<std::int32_t>(p.call(libsystem_library, "_fileno", {s.x[2]}));
        if (target < 0) throw Errno{ebadf};
        CallRegs open;
        open.x[0] = s.x[0]; open.x[1] = flags;
        open.push_variadic(std::uint64_t{0666});
        const auto descriptor = static_cast<std::int32_t>(p.call_regs(libsystem_library, "_open", open));
        if (descriptor < 0) throw Errno{p.guest_errno()};
        if (descriptor != target) {
            const auto duplicated = static_cast<std::int32_t>(p.call(libsystem_library, "_dup2", {static_cast<std::uint32_t>(descriptor), static_cast<std::uint32_t>(target)}));
            const auto error = p.guest_errno();
            static_cast<void>(p.call(libsystem_library, "_close", {static_cast<std::uint32_t>(descriptor)}));
            if (duplicated < 0) throw Errno{error};
        }
        static_cast<void>(p.call(libsystem_library, "_clearerr", {s.x[2]}));
        return s.x[2];
    }, 0);

    // pthread attributes and scheduling (error-number return convention).
    // QoS classes and guard sizes are scheduling hints Alchemy's workers do not model; they are
    // kept inside the zero-filled 64-byte pthread_attr_t (after misc/libsystem_misc.cpp's
    // scheduling fields at 40..51) so the getters read them back: the QoS class at 52, its
    // relative priority at 56, the guard size at 60 (16 KB pages plus one; 0 is the default page).
    constexpr std::uint64_t attr_qos = 52, attr_relative = 56, attr_guard = 60;
    constexpr std::uint64_t guard_page = 16384;
    p.libsystem("_pthread_attr_set_qos_class_np", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || m.read64(s.x[0]) != pthread_attr_magic) return einval;
        const auto relative = static_cast<std::int32_t>(s.x[2]);
        if (std::find(std::begin(qos_classes), std::end(qos_classes), static_cast<std::uint32_t>(s.x[1])) == std::end(qos_classes) ||
            relative > 0 || relative < -15) return einval;
        m.write32(s.x[0] + attr_qos, static_cast<std::uint32_t>(s.x[1]));
        m.write32(s.x[0] + attr_relative, static_cast<std::uint32_t>(relative));
        return 0;
    });
    // pthread_attr_get_qos_class_np(attr, qos_class_t*, int* relative): QOS_CLASS_UNSPECIFIED, 0 until set.
    p.libsystem("_pthread_attr_get_qos_class_np", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || m.read64(s.x[0]) != pthread_attr_magic) return einval;
        if (s.x[1]) m.write32(s.x[1], m.read32(s.x[0] + attr_qos));
        if (s.x[2]) m.write32(s.x[2], m.read32(s.x[0] + attr_relative));
        return 0;
    });
    // The guard size rounds up to whole pages.
    p.libsystem("_pthread_attr_setguardsize", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || m.read64(s.x[0]) != pthread_attr_magic) return einval;
        const auto pages = (s.x[1] + guard_page - 1) / guard_page;
        if (pages >= 0xffffffffULL) return einval;
        m.write32(s.x[0] + attr_guard, static_cast<std::uint32_t>(pages + 1));
        return 0;
    });
    p.libsystem("_pthread_attr_getguardsize", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || !s.x[1] || m.read64(s.x[0]) != pthread_attr_magic) return einval;
        const auto stored = m.read32(s.x[0] + attr_guard);
        m.write64(s.x[1], stored ? (stored - 1ULL) * guard_page : guard_page);
        return 0;
    });
    p.libsystem("_pthread_getschedparam", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[1]) m.write32(s.x[1], sched_other);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(default_priority));
        return 0;
    });
    // SCHED_OTHER requests within its priority range succeed (the host
    // scheduler decides); real-time policies are not available (ENOTSUP).
    p.libsystem("_pthread_setschedparam", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto policy = static_cast<std::uint32_t>(s.x[1]);
        if (!s.x[2]) return einval;
        const auto priority = static_cast<std::int32_t>(m.read32(s.x[2]));
        if (policy == sched_rr || policy == sched_fifo) return enotsup;
        if (policy != sched_other || priority < 0 || priority > 47) return einval;
        return 0;
    });

    // int __ulock_wait2(uint32_t op, void* addr, uint64_t value, uint64_t timeout_ns, uint64_t value2):
    // __ulock_wait with the timeout in nanoseconds (rounded up to microseconds).
    p.libsystem("___ulock_wait2", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto nanos = s.x[3];
        const auto micros = nanos == 0 ? 0 : std::min<std::uint64_t>((nanos + 999) / 1000, UINT32_MAX);
        return p.call(libsystem_library, "___ulock_wait", {s.x[0], s.x[1], s.x[2], micros});
    });

    // os_sync (<os/os_sync_wait_on_address.h>): waits on a 4- or 8-byte value, over the ulock
    // calls (UL_COMPARE_AND_WAIT 1, UL_COMPARE_AND_WAIT64 5, ULF_WAKE_ALL 0x100; shared file memory
    // is matched across processes by the ulock itself). Waits return the number of waiters left,
    // or -1 with errno (ETIMEDOUT, EINVAL); wakes return 0, or -1 with ENOENT when none waited.
    const auto operation = [](std::uint64_t size) -> std::optional<std::uint64_t> {
        if (size == 4) return 1;
        if (size == 8) return 5;
        return std::nullopt;
    };
    // int os_sync_wait_on_address(void* addr, uint64_t value, size_t size, os_sync_wait_on_address_flags_t)
    p.libsystem("_os_sync_wait_on_address", [p, operation](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto op = operation(s.x[2]);
        if (!op || !s.x[0] || (s.x[3] & ~1ULL)) throw Errno{einval};
        return p.call(libsystem_library, "___ulock_wait", {*op, s.x[0], s.x[1], 0});
    });
    // ..._with_timeout(addr, value, size, flags, os_clockid_t clock, uint64_t timeout_ns) and
    // ..._with_deadline(addr, value, size, flags, clock, uint64_t deadline): the clock is
    // OS_CLOCK_MACH_ABSOLUTE_TIME (32), whose units are nanoseconds here (mach_timebase_info 1/1).
    for (const auto* name : {"_os_sync_wait_on_address_with_timeout", "_os_sync_wait_on_address_with_deadline"})
        p.libsystem(name, [p, operation, deadline = std::string_view(name).ends_with("deadline")](CpuState& s, GuestMemory&) -> std::uint64_t {
            const auto op = operation(s.x[2]);
            if (!op || !s.x[0] || (s.x[3] & ~1ULL) || s.x[4] != 32) throw Errno{einval};
            std::uint64_t nanos = s.x[5];
            if (deadline) {
                const auto now = p.call(libsystem_library, "_mach_absolute_time", {});
                nanos = s.x[5] > now ? s.x[5] - now : 0;
            }
            if (nanos == 0) {
                // No time to wait: a value that already differs returns at once, else it times out.
                const auto current = s.x[2] == 4 ? static_cast<std::uint64_t>(p.process().calling_engine().read32(s.x[0]))
                                                 : p.process().calling_engine().read64(s.x[0]);
                if (current != (s.x[2] == 4 ? static_cast<std::uint32_t>(s.x[1]) : s.x[1])) return 0;
                throw Errno{60};   // ETIMEDOUT
            }
            return p.call(libsystem_library, "___ulock_wait2", {*op, s.x[0], s.x[1], nanos, 0});
        });
    // int os_sync_wake_by_address_any / _all(void* addr, size_t size, os_sync_wake_by_address_flags_t)
    for (const auto* name : {"_os_sync_wake_by_address_any", "_os_sync_wake_by_address_all"})
        p.libsystem(name, [p, operation, all = std::string_view(name).ends_with("all")](CpuState& s, GuestMemory&) -> std::uint64_t {
            const auto op = operation(s.x[1]);
            if (!op || !s.x[0] || (s.x[2] & ~1ULL)) throw Errno{einval};
            return p.call(libsystem_library, "___ulock_wake", {*op | (all ? 0x100 : 0), s.x[0], 0});
        });
}

} // namespace alchemy::export_fill
