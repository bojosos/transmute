// Linux host backend, area "process".
//
// Children are started with fork + execve, so a suspended start is a gate:
// the child waits on a pipe before it executes, and resume() writes to the
// pipe (Child::thread holds its write end). Handles in Spawn::inherit stay
// open under the same descriptor numbers in the child (their close-on-exec
// flag is cleared there); every other descriptor is closed at exec. A child's
// process Handle is a pidfd, which poll(2) reports readable once the child has
// ended; the child stays unreaped (waitid WNOWAIT) until its last handle is
// closed, so its pid is not reused meanwhile, as on Windows.
//
// Not done yet: end_with_parent (PR_SET_PDEATHSIG fires when the spawning
// thread exits, not the process, so it needs a subreaper), and
// duplicate_into another process (descriptors travel over UNIX sockets).
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/process.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

extern char** environ;

namespace alchemy::host::process {

namespace {

using linux_backend::fd_of;
using linux_backend::handle_of;
using linux_backend::last_error;

std::mutex g_lock;
// pidfd -> pid of the children and processes this backend opened, and the
// exit codes terminate() asked for.
std::map<int, pid_t>& pidfds() {
    static auto* map = new std::map<int, pid_t>;
    return *map;
}
std::map<pid_t, std::uint32_t>& forced_codes() {
    static auto* map = new std::map<pid_t, std::uint32_t>;
    return *map;
}
std::map<int, pid_t>& children() {   // pidfds of processes this process started (reaped by us)
    static auto* map = new std::map<int, pid_t>;
    return *map;
}

int pidfd_open(pid_t pid) { return static_cast<int>(syscall(SYS_pidfd_open, pid, 0)); }

std::optional<pid_t> pid_of(Handle process) {
    std::lock_guard guard(g_lock);
    const auto it = pidfds().find(fd_of(process));
    if (it == pidfds().end()) return std::nullopt;
    return it->second;
}

Handle remember(int pidfd, pid_t pid, bool child) {
    std::lock_guard guard(g_lock);
    pidfds()[pidfd] = pid;
    if (child) children()[pidfd] = pid;
    return handle_of(pidfd);
}

// Field `index` (1-based, after the command) of /proc/<pid>/stat.
std::optional<unsigned long long> stat_field(pid_t pid, int index) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* file = std::fopen(path, "re");
    if (!file) return std::nullopt;
    char buffer[1024];
    const auto n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[n] = 0;
    const char* p = std::strrchr(buffer, ')');
    for (int field = 3; p && field <= index; ++field) {
        p = std::strchr(p + 1, ' ');
        if (p && field == index) return std::strtoull(p + 1, nullptr, 10);
    }
    return std::nullopt;
}

// The wall-clock time of boot, read once so every start time agrees with the others.
std::uint64_t boot_time_ns() {
    static const std::uint64_t boot = [] {
        timespec real{}, boot{};
        clock_gettime(CLOCK_REALTIME, &real);
        clock_gettime(CLOCK_BOOTTIME, &boot);
        return (static_cast<std::uint64_t>(real.tv_sec) - static_cast<std::uint64_t>(boot.tv_sec)) * 1000000000u +
               static_cast<std::uint64_t>(real.tv_nsec) - static_cast<std::uint64_t>(boot.tv_nsec);
    }();
    return boot;
}

std::uint32_t code_of(pid_t pid, const siginfo_t& info) {
    {
        std::lock_guard guard(g_lock);
        if (const auto it = forced_codes().find(pid); it != forced_codes().end()) return it->second;
    }
    if (info.si_code == CLD_EXITED) return static_cast<std::uint32_t>(info.si_status);
    return 128u + static_cast<std::uint32_t>(info.si_status);   // ended by a signal: the shell's convention
}

// Stops this thread for good (suspend_other_threads).
void park(int) {
    for (;;) pause();
}

} // namespace

std::uint32_t current_pid() noexcept { return static_cast<std::uint32_t>(getpid()); }
std::uint64_t current_thread_id() noexcept { return static_cast<std::uint64_t>(syscall(SYS_gettid)); }

Handle standard_handle(Standard which) noexcept {
    const int fd = which == Standard::input ? 0 : which == Standard::output ? 1 : 2;
    return fcntl(fd, F_GETFD) < 0 ? Handle{} : handle_of(fd);
}

Result<Usage> usage(std::uint32_t host_pid) {
    Usage u;
    if (host_pid == 0 || host_pid == current_pid()) {
        rusage self{};
        getrusage(RUSAGE_SELF, &self);
        u.user_ns = static_cast<std::uint64_t>(self.ru_utime.tv_sec) * 1000000000u + static_cast<std::uint64_t>(self.ru_utime.tv_usec) * 1000u;
        u.system_ns = static_cast<std::uint64_t>(self.ru_stime.tv_sec) * 1000000000u + static_cast<std::uint64_t>(self.ru_stime.tv_usec) * 1000u;
        u.resident_peak = static_cast<std::uint64_t>(self.ru_maxrss) * 1024u;
        u.page_faults = static_cast<std::uint64_t>(self.ru_minflt + self.ru_majflt);
        host_pid = current_pid();
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/status", host_pid);
    FILE* status = std::fopen(path, "re");
    if (!status) return errno == ENOENT ? Error::not_found : last_error();
    char line[256];
    while (std::fgets(line, sizeof(line), status)) {
        unsigned long long kilobytes = 0;
        if (std::sscanf(line, "VmRSS: %llu", &kilobytes) == 1) u.resident = kilobytes * 1024;
        else if (std::sscanf(line, "VmHWM: %llu", &kilobytes) == 1) u.resident_peak = kilobytes * 1024;
        // Committed private memory: the writable private mappings (VmData), as Windows counts commit.
        else if (std::sscanf(line, "VmData: %llu", &kilobytes) == 1) u.private_bytes = kilobytes * 1024;
        else if (host_pid == current_pid() && std::sscanf(line, "VmSize: %llu", &kilobytes) == 1) u.virtual_size = kilobytes * 1024;
    }
    std::fclose(status);
    if (host_pid != current_pid()) {
        const long ticks = sysconf(_SC_CLK_TCK);
        if (const auto user = stat_field(static_cast<pid_t>(host_pid), 14)) u.user_ns = *user * 1000000000ull / static_cast<unsigned long long>(ticks);
        if (const auto system = stat_field(static_cast<pid_t>(host_pid), 15)) u.system_ns = *system * 1000000000ull / static_cast<unsigned long long>(ticks);
    }
    return u;
}

Result<ThreadUsage> thread_usage() {
    rusage self{};
    if (getrusage(RUSAGE_THREAD, &self) != 0) return last_error();
    ThreadUsage t;
    t.user_ns = static_cast<std::uint64_t>(self.ru_utime.tv_sec) * 1000000000u + static_cast<std::uint64_t>(self.ru_utime.tv_usec) * 1000u;
    t.system_ns = static_cast<std::uint64_t>(self.ru_stime.tv_sec) * 1000000000u + static_cast<std::uint64_t>(self.ru_stime.tv_usec) * 1000u;
    return t;
}

Result<MemoryUsage> memory_usage() {
    const auto u = usage(0);
    if (!u) return u.error();
    MemoryUsage m;
    m.private_bytes = u->private_bytes;
    const auto heap = mallinfo2();
    m.heap_committed_bytes = heap.arena + heap.hblkhd;
    return m;
}

Result<int> priority() {
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, 0);
    if (nice == -1 && errno) return last_error();
    return nice;
}

Status set_priority(int nice) {
    if (setpriority(PRIO_PROCESS, 0, nice) != 0) return last_error();
    return {};
}

// Background mode is a nice value of 10, for this thread or every thread.
// Leaving it lowers the nice value again, which Linux refuses an unprivileged
// process (RLIMIT_NICE; Darwin allows leaving PRIO_DARWIN_BG): that reports
// Error::access_denied rather than pretending.
Status set_background(bool whole_process, bool background) {
    const int nice = background ? 10 : 0;
    const auto apply = [&](id_t thread) { return setpriority(PRIO_PROCESS, thread, nice) == 0; };
    if (!whole_process) {
        if (!apply(static_cast<id_t>(syscall(SYS_gettid)))) return last_error();
        return {};
    }
    DIR* tasks = opendir("/proc/self/task");
    if (!tasks) return last_error();
    Status result;
    while (const dirent* entry = readdir(tasks)) {
        const auto tid = static_cast<id_t>(std::strtol(entry->d_name, nullptr, 10));
        if (tid > 0 && !apply(tid) && result) result = last_error();
    }
    closedir(tasks);
    return result;
}

Result<std::vector<Entry>> list() {
    std::vector<Entry> entries;
    DIR* proc = opendir("/proc");
    if (!proc) return last_error();
    while (const dirent* entry = readdir(proc)) {
        char* end = nullptr;
        const auto pid = std::strtoul(entry->d_name, &end, 10);
        if (!pid || *end) continue;
        const auto parent = stat_field(static_cast<pid_t>(pid), 4);
        entries.push_back({static_cast<std::uint32_t>(pid), static_cast<std::uint32_t>(parent.value_or(0))});
    }
    closedir(proc);
    return entries;
}

std::optional<std::string> environment(std::string_view name) {
    const char* value = std::getenv(std::string(name).c_str());
    if (!value) return std::nullopt;
    return std::string(value);
}

Status set_environment(std::string_view name, std::optional<std::string_view> value) {
    const std::string key(name);
    const int result = value ? setenv(key.c_str(), std::string(*value).c_str(), 1) : unsetenv(key.c_str());
    if (result != 0) return last_error();
    return {};
}

std::vector<std::string> environment_block() {
    std::vector<std::string> block;
    for (char** entry = environ; entry && *entry; ++entry) block.emplace_back(*entry);
    return block;
}

Result<std::uint32_t> exit_code(Handle process) {
    const auto pid = pid_of(process);
    if (!pid) return Error::bad_handle;
    siginfo_t info{};
    if (waitid(P_PIDFD, static_cast<id_t>(fd_of(process)), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
        if (errno != ECHILD) return last_error();
        // Not our child: it has ended once the pidfd is readable; its code is not ours to read.
        pollfd fd{fd_of(process), POLLIN, 0};
        return poll(&fd, 1, 0) == 1 ? Result<std::uint32_t>(0u) : Result<std::uint32_t>(Error::busy);
    }
    if (info.si_pid == 0) return Error::busy;
    return code_of(*pid, info);
}

Result<std::uint32_t> wait_exit(Handle process) {
    const auto pid = pid_of(process);
    if (!pid) return Error::bad_handle;
    for (;;) {
        siginfo_t info{};
        if (waitid(P_PIDFD, static_cast<id_t>(fd_of(process)), &info, WEXITED | WNOWAIT) == 0) return code_of(*pid, info);
        if (errno == EINTR) continue;
        if (errno != ECHILD) return last_error();
        pollfd fd{fd_of(process), POLLIN, 0};
        while (poll(&fd, 1, -1) < 0 && errno == EINTR) {
        }
        return 0u;
    }
}

Status terminate(Handle process, std::uint32_t code) {
    const auto pid = pid_of(process);
    if (!pid) return Error::bad_handle;
    {
        std::lock_guard guard(g_lock);
        forced_codes()[*pid] = code;
    }
    if (syscall(SYS_pidfd_send_signal, fd_of(process), SIGKILL, nullptr, 0) != 0) return last_error();
    return {};
}

Status terminate_pid(std::uint32_t host_pid, std::uint32_t code) {
    {
        std::lock_guard guard(g_lock);
        forced_codes()[static_cast<pid_t>(host_pid)] = code;
    }
    if (kill(static_cast<pid_t>(host_pid), SIGKILL) != 0) return last_error();
    return {};
}

void terminate_self(std::uint32_t code) noexcept { _exit(static_cast<int>(code)); }

void suspend_other_threads() noexcept {
    const int parking = SIGRTMIN + 4;
    struct sigaction action {};
    action.sa_handler = &park;
    sigfillset(&action.sa_mask);
    sigaction(parking, &action, nullptr);
    const auto self = static_cast<pid_t>(syscall(SYS_gettid));
    DIR* tasks = opendir("/proc/self/task");
    if (!tasks) return;
    while (const dirent* entry = readdir(tasks)) {
        const auto tid = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
        if (tid > 0 && tid != self) syscall(SYS_tgkill, getpid(), tid, parking);
    }
    closedir(tasks);
}

Result<std::uint64_t> start_time(std::uint32_t host_pid) {
    const auto pid = host_pid ? static_cast<pid_t>(host_pid) : getpid();
    const auto ticks = stat_field(pid, 22);
    if (!ticks) return Error::not_found;
    return boot_time_ns() + *ticks * 1000000000ull / static_cast<unsigned long long>(sysconf(_SC_CLK_TCK));
}

Result<Handle> open(std::uint32_t host_pid) {
    const int fd = pidfd_open(static_cast<pid_t>(host_pid));
    if (fd < 0) return errno == ESRCH ? Error::not_found : last_error();
    return remember(fd, static_cast<pid_t>(host_pid), false);
}

Result<bool> is_running(std::uint32_t host_pid) {
    if (kill(static_cast<pid_t>(host_pid), 0) == 0) {
        // A zombie has ended: it only waits to be reaped.
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%u/stat", host_pid);
        FILE* file = std::fopen(path, "re");
        if (!file) return true;
        char buffer[512];
        const auto n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
        std::fclose(file);
        buffer[n] = 0;
        const char* state = std::strrchr(buffer, ')');
        return !(state && state[1] == ' ' && state[2] == 'Z');
    }
    return errno == ESRCH ? Error::not_found : errno == EPERM ? Result<bool>(true) : last_error();
}

Result<std::string> image_path(std::uint32_t host_pid) {
    char link[64], target[4096];
    std::snprintf(link, sizeof(link), "/proc/%u/exe", host_pid);
    const auto length = readlink(link, target, sizeof(target) - 1);
    if (length <= 0) return errno == ENOENT ? Error::not_found : last_error();
    return std::string(target, static_cast<std::size_t>(length));
}

Result<Child> spawn(const Spawn& options) {
    std::vector<std::string> arguments;
    arguments.push_back(options.program);
    for (const auto& argument : options.arguments) arguments.push_back(argument);
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<int> inherit;
    for (const auto handle : options.inherit) inherit.push_back(fd_of(handle));
    const int standard[3] = {options.standard_input ? fd_of(options.standard_input) : -1,
                             options.standard_output ? fd_of(options.standard_output) : -1,
                             options.standard_error ? fd_of(options.standard_error) : -1};
    // The gate (suspended starts) and the exec-error report, both close-on-exec.
    int gate[2] = {-1, -1}, report[2] = {-1, -1};
    if (options.suspended && pipe2(gate, O_CLOEXEC) != 0) return last_error();
    if (pipe2(report, O_CLOEXEC) != 0) {
        const auto error = last_error();
        if (gate[0] >= 0) { ::close(gate[0]); ::close(gate[1]); }
        return error;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        const auto error = last_error();
        ::close(report[0]); ::close(report[1]);
        if (gate[0] >= 0) { ::close(gate[0]); ::close(gate[1]); }
        return error;
    }
    if (pid == 0) {
        // The child: async-signal-safe calls only until execve.
        const auto fail = [&](int error) {
            static_cast<void>(::write(report[1], &error, sizeof(error)));
            _exit(127);
        };
        if (options.new_group || options.detached) setsid();
        for (int fd = 0; fd < 3; ++fd) {
            if (standard[fd] < 0) {
                const int null_fd = ::open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
                if (null_fd >= 0 && null_fd != fd) { dup2(null_fd, fd); ::close(null_fd); }
            } else if (dup2(standard[fd], fd) < 0) {
                fail(errno);
            }
        }
        syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */);
        for (const int fd : inherit) fcntl(fd, F_SETFD, 0);
        if (!options.working_directory.empty() && chdir(options.working_directory.c_str()) != 0) fail(errno);
        if (gate[0] >= 0) {
            ::close(gate[1]);
            char go = 0;
            while (::read(gate[0], &go, 1) < 0 && errno == EINTR) {
            }
        }
        execve(options.program.c_str(), argv.data(), environ);
        fail(errno);
    }
    ::close(report[1]);
    if (gate[0] >= 0) ::close(gate[0]);
    if (!options.suspended) {
        int error = 0;
        if (::read(report[0], &error, sizeof(error)) == static_cast<ssize_t>(sizeof(error))) {
            ::close(report[0]);
            waitpid(pid, nullptr, 0);
            errno = error;
            return error == E2BIG ? Error::out_of_range : last_error();
        }
    }
    ::close(report[0]);
    const int pidfd = pidfd_open(pid);
    if (pidfd < 0) {
        const auto error = last_error();
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        if (gate[1] >= 0) ::close(gate[1]);
        return error;
    }
    Child child;
    child.pid = static_cast<std::uint32_t>(pid);
    child.process = remember(pidfd, pid, true);
    child.thread = gate[1] >= 0 ? handle_of(gate[1]) : handle_of(fcntl(pidfd, F_DUPFD_CLOEXEC, 0));
    return child;
}

Status resume(Handle thread) {
    const char go = 1;
    if (::write(fd_of(thread), &go, 1) != 1 && errno != EPIPE) return last_error();
    return {};
}

Result<Handle> inheritable(Handle handle) { return host::duplicate(handle); }

Result<Piped> spawn_piped(std::string_view program, std::span<const std::string> arguments) {
    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC) != 0) return last_error();
    if (pipe2(out, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); return last_error(); }
    if (pipe2(err, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]); return last_error(); }
    Spawn options;
    options.program = std::string(program);
    options.arguments.assign(arguments.begin(), arguments.end());
    options.standard_input = handle_of(in[0]);
    options.standard_output = handle_of(out[1]);
    options.standard_error = handle_of(err[1]);
    auto child = spawn(options);
    ::close(in[0]); ::close(out[1]); ::close(err[1]);
    if (!child) {
        ::close(in[1]); ::close(out[0]); ::close(err[0]);
        return child.error();
    }
    host::close(child->thread);
    return Piped{child->process, child->pid, handle_of(in[1]), handle_of(out[0]), handle_of(err[0])};
}

Result<std::uint64_t> duplicate_into(Handle handle, std::uint32_t target_pid) {
    if (target_pid != 0 && target_pid != current_pid()) return Error::unsupported;
    const auto copy = host::duplicate(handle);
    if (!copy) return copy.error();
    return copy->value;
}

} // namespace alchemy::host::process

namespace alchemy::host::linux_backend {

// A closed child pidfd: reap the child (now or, if it still runs, when it ends).
void forget_process(int fd) noexcept {
    using namespace process;
    pid_t pid = 0;
    {
        std::lock_guard guard(g_lock);
        pidfds().erase(fd);
        const auto it = children().find(fd);
        if (it == children().end()) return;
        pid = it->second;
        children().erase(it);
        forced_codes().erase(pid);
    }
    if (waitpid(pid, nullptr, WNOHANG) == 0) {
        try {
            std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
        } catch (...) {
        }
    }
}

} // namespace alchemy::host::linux_backend
