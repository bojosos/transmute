// Darwin process model of DarwinServices: pipes, the fork/vfork emulation,
// posix_spawn, exec*, wait*, kill, popen, system and identity
// (docs/ALCHEMY_PROCESS.md). Part of DarwinServices::Impl (services_impl.hpp,
// one of its translation units); the host machinery is launch.cpp.
#include "alchemy/darwin/libsystem/services_impl.hpp"

namespace alchemy {

namespace {

constexpr std::uint32_t echild = 10, eagain_error = 35, esrch_error = 3, enotsup_error = 45, eintr_error = 4,
    enoexec_error = 8, eperm_error = 1;

// Darwin signals whose default action is to ignore them / to stop the process.
bool signal_ignored_by_default(std::uint64_t signal) { return signal == 16 || signal == 19 || signal == 20 || signal == 23 || signal == 28 || signal == 29; }
bool signal_stops_by_default(std::uint64_t signal) { return signal == 17 || signal == 18 || signal == 21 || signal == 22; }

// Ends the child side of an emulated fork (after exec or _exit). Not a
// std::exception: no handler between the child's call and its thread catches it.
struct ForkChildEnd {};
struct ForkChildViolation { std::string message; };

std::vector<std::string> host_environment_strings() { return host::process::environment_block(); }

// The host process of a child (launch.cpp hands it over as an opaque pointer).
host::Handle child_process(void* process) { return host::Handle{reinterpret_cast<std::uintptr_t>(process)}; }
// Whether a child's host process has ended, and its exit code.
std::optional<std::uint32_t> ended_child(void* process) {
    if (!process) return std::nullopt;
    const auto code = host::process::exit_code(child_process(process));
    return code ? std::optional<std::uint32_t>(*code) : std::nullopt;
}

// libSystem calls the child side of an emulated fork may make before exec:
// descriptor, directory, signal-disposition and exec operations, plus pure
// memory/string helpers, read-only queries and output.
bool fork_child_may_call(std::string_view symbol) {
    static const std::set<std::string, std::less<>> allowed{
        "___error", "_malloc", "_calloc", "_realloc", "_free", "_malloc_size", "_posix_memalign",
        "_memcpy", "_memmove", "_memset", "_bzero", "___bzero", "_bcopy", "_memcmp", "_memchr", "_memset_pattern16",
        "_strlen", "_strnlen", "_strcmp", "_strncmp", "_strcpy", "_strncpy", "_strcat", "_strlcpy", "_strlcat",
        "_strchr", "_strrchr", "_strstr", "_strdup", "_strerror", "_strerror_r", "_atoi", "_strtol", "_strtoll",
        "_strtoul", "_strtoull", "_snprintf", "_vsnprintf", "_sprintf", "_vsprintf", "_strcasecmp", "_strncasecmp",
        "___memcpy_chk", "___memmove_chk", "___memset_chk", "___strcpy_chk", "___strlcpy_chk", "___strncpy_chk",
        "___strcat_chk", "___snprintf_chk", "___vsnprintf_chk", "___sprintf_chk", "___stack_chk_fail", "___chkstk_darwin",
        "__tlv_bootstrap", "_pthread_self", "_pthread_main_np", "_mach_absolute_time", "_mach_timebase_info",
        "_close", "_close$NOCANCEL", "_dup", "_dup2", "_fcntl", "_fcntl$NOCANCEL", "_open", "_open$NOCANCEL", "_openat",
        "_read", "_read$NOCANCEL", "_write", "_write$NOCANCEL", "_ioctl", "_pipe", "_isatty", "_lseek",
        "_fstat", "_fstat$INODE64", "_fstat64", "_stat", "_stat$INODE64", "_stat64", "_lstat", "_lstat$INODE64",
        "_access", "_faccessat", "_readlink", "_getdtablesize", "_sysconf", "_getrlimit",
        "_opendir", "_readdir", "_readdir$INODE64", "_closedir", "_dirfd", "_fdopendir",
        "_chdir", "_fchdir", "_getcwd", "_umask",
        "_getpid", "_getppid", "_getuid", "_geteuid", "_getgid", "_getegid", "_getpgrp", "_getpgid", "_getsid",
        "_setsid", "_setpgid", "_setpgrp", "_setgroups", "_setuid", "_seteuid", "_setreuid", "_setgid", "_setegid",
        "_setregid", "_initgroups", "_getenv", "_setenv", "_unsetenv",
        "_signal", "_sigaction", "_sigprocmask", "_pthread_sigmask", "_sigemptyset", "_sigfillset", "_sigaddset",
        "_sigdelset", "_sigismember",
        "_execve", "_execv", "_execvp", "_execvP", "_execl", "_execle", "_execlp", "_fork", "_vfork",
        "_posix_spawn", "_posix_spawnp", "_waitpid", "_wait4", "_wait", "_kill",
        "_nanosleep", "_usleep", "_sleep", "_clock_gettime", "_gettimeofday", "_time",
        "_printf", "_fprintf", "_vprintf", "_vfprintf", "_puts", "_fputs", "_fputc", "_putchar", "_fwrite", "_fflush",
        "_fileno", "___swbuf", "__exit", "__Exit"};
    return allowed.contains(symbol) || symbol.starts_with("_posix_spawn_file_actions_") || symbol.starts_with("_posix_spawnattr_");
}

} // namespace

struct DarwinServices::Impl::SpawnFileActions {
    // kind: 0 close, 1 dup2, 2 open, 3 inherit_np, 4 chdir_np, 5 fchdir_np
    struct Action { int kind{}; int fd{}, target{}; std::string path; std::uint64_t flags{}, mode{}; };
    std::vector<Action> actions;
};
struct DarwinServices::Impl::SpawnAttributes {
    std::uint16_t flags{};
    std::uint32_t pgroup{}, sigmask{}, sigdefault{}, qos{};
};

struct DarwinServices::Impl::ForkChild : ForkChildGuard {
    explicit ForkChild(Impl& owner) : impl(owner) {}
    Impl& impl;
    FdOverlay table;
    std::array<SignalAction, 32> actions{};
    std::set<std::uint64_t> allocations;     // child-phase malloc blocks
    std::uint32_t pgid{}, sid{};             // inherited identity
    bool new_group{}, new_session{};         // setpgid(0, 0) / setsid() before exec
    std::uint32_t pseudo_pid{};
    bool exec_attempted{}, spawned_any{};
    std::optional<SpawnedProcess> exec_process;
    std::string exec_program;                // guest path exec ran
    std::optional<std::uint32_t> exit_status;
    // setenv/unsetenv and credential changes before exec: the child's own.
    std::optional<std::vector<std::string>> environment;
    std::optional<std::array<std::uint32_t, 4>> credentials;
    void check_call(std::string_view library, std::string_view symbol) override {
        // Internal entries that stand behind allowed libSystem calls: exit's
        // cleanup, the malloc zone and __tlv_bootstrap's slow path (the first
        // use of a thread-local on the child's thread).
        if (library == "alchemy.internal" && (symbol == "__alchemy_exit_cleanup" || symbol.starts_with("__alchemy_zone_") ||
                                              symbol == "__alchemy_tlv_bootstrap_slow_path")) return;
        if ((library == system_library || library.starts_with("/usr/lib/system/")) && fork_child_may_call(symbol)) return;
        throw ForkChildViolation{"the child called " + std::string(symbol) + (library == system_library ? std::string() : " (" + std::string(library) + ")") +
            " before exec; between fork and exec only descriptor, directory, signal-disposition, wait and exec operations are supported"};
    }
};

void DarwinServices::Impl::note_child_allocation(std::uint64_t address) { fork_child->allocations.insert(address); }

// free/realloc on the child side: true for the child's own blocks (released
// now); a block the parent allocated is left to the parent (the child's copy
// of it ends with the exec, as after a real fork).
bool DarwinServices::Impl::child_owns_block(std::uint64_t address) { return fork_child->allocations.erase(address) != 0; }

std::array<DarwinServices::Impl::SignalAction, 32>& DarwinServices::Impl::signal_table() {
    return fork_child ? fork_child->actions : signal_actions;
}

// Called with `mutex` held.
std::vector<std::string>* DarwinServices::Impl::fork_child_environment(bool create) {
    if (!fork_child) return nullptr;
    if (!fork_child->environment && create) fork_child->environment = context.environment;
    return fork_child->environment ? &*fork_child->environment : nullptr;
}

std::array<std::uint32_t, 4> DarwinServices::Impl::current_credentials() const {
    if (fork_child && fork_child->credentials) return *fork_child->credentials;
    return {credential_ids[0].load(), credential_ids[1].load(), credential_ids[2].load(), credential_ids[3].load()};
}

void DarwinServices::Impl::set_credential(std::size_t index, std::uint32_t value) {
    if (fork_child) {
        if (!fork_child->credentials) fork_child->credentials = current_credentials();
        (*fork_child->credentials)[index] = value;
        return;
    }
    credential_ids[index] = value;
}

std::uint32_t DarwinServices::Impl::process_id() const {
    if (own_pid) return own_pid;
    return host::process::current_pid();
}

bool DarwinServices::Impl::fork_child_handles_exit() const noexcept { return fork_child != nullptr; }

void DarwinServices::Impl::fork_child_exit(std::uint32_t status) {
    if (!fork_child) throw ServiceError{enosys};
    fork_child->exit_status = status & 0xff;
    throw ForkChildEnd{};
}

ExecEnvironment DarwinServices::Impl::exec_environment() {
    static const auto host = host_environment_strings();
    ExecEnvironment result;
    result.map_path = [this](std::string_view value) {
        try { return translate_path(value); }
        catch (const ServiceError& error) { throw ProcessError(error.value, std::string(value) + ": invalid path"); }
    };
    result.main_executable = context.executable;
    result.host_environment = host;
    result.user_id = current_credentials()[1];
    return result;
}

// ---- pipes ----------------------------------------------------------------------------------
std::array<int, 2> DarwinServices::Impl::create_pipe(bool cloexec) {
    const auto ends = host::files::create_pipe();
    if (!ends) throw ServiceError{file_failure(ends.error())};
    const auto read_native = ends->read, write_native = ends->write;
    auto make = [&](host::Handle native, bool reading) {
        auto record = std::make_shared<File>();
        record->native = native; record->pipe = true;
        record->readable = reading; record->writable = !reading; record->flags = reading ? 0 : 1;
        record->cloexec = cloexec;
        record->pipe_status = std::make_shared<std::atomic<std::uint32_t>>(0);
        return record;
    };
    auto reader = make(read_native, true), writer = make(write_native, false);
    const auto first = add_file(std::move(reader));
    try { return {first, add_file(std::move(writer))}; }
    catch (...) { static_cast<void>(close(first)); throw; }
}

std::uint64_t DarwinServices::Impl::pipe_available(const std::shared_ptr<File>& record) {
    std::lock_guard lock(record->mutex);
    if (record->closed || !record->native || !record->readable) return 0;
    const auto available = host::files::pipe_available(record->native);
    return available ? *available : 0;
}

std::uint16_t DarwinServices::Impl::pipe_ready(const std::shared_ptr<File>& record, std::uint16_t events) {
    std::lock_guard lock(record->mutex);
    if (record->closed || !record->native) return 32;   // POLLNVAL
    if (!record->readable) return static_cast<std::uint16_t>(events & (4 | 0x100));   // a write end is writable
    const auto available = host::files::pipe_available(record->native);
    if (!available) return static_cast<std::uint16_t>(16 | (events & (1 | 0x40)));   // every writer closed: POLLHUP, read returns 0
    return *available ? static_cast<std::uint16_t>(events & (1 | 0x40)) : 0;
}

std::uint64_t DarwinServices::Impl::pipe_read(transmute::GuestMemory& memory, const std::shared_ptr<File>& record,
    std::uint64_t address, std::uint64_t size) {
    validate(memory, address, size, true);
    if (!size) return 0;
    auto delay = std::chrono::microseconds(0);
    for (;;) {
        {
            // The descriptor lock is held only while data is ready, never across a
            // wait: close() from another thread must not block behind a reader.
            std::lock_guard lock(record->mutex);
            if (record->closed || !record->native) throw ServiceError{ebadf};
            const auto available = host::files::pipe_available(record->native);
            if (!available) {
                if (available.error() == host::Error::broken_pipe) return 0;
                throw ServiceError{file_failure(available.error())};
            }
            if (*available) {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>({size, *available, 1U << 20})));
                // A pipe whose writers are gone reads end of file.
                const auto received = host::files::read(record->native, std::as_writable_bytes(std::span(bytes.data(), bytes.size())));
                if (!received) throw ServiceError{file_failure(received.error())};
                memory.write_bytes(address, std::span(bytes.data(), *received));
                return *received;
            }
        }
        if (record->pipe_status->load() & 4) throw ServiceError{eagain_error};
        if (process_exiting(context)) throw ServiceError{89};
        if (delay.count()) std::this_thread::sleep_for(delay); else std::this_thread::yield();
        delay = std::min<std::chrono::microseconds>(delay * 2 + std::chrono::microseconds(100), std::chrono::milliseconds(10));
    }
}

std::uint64_t DarwinServices::Impl::pipe_write(transmute::GuestMemory& memory, const std::shared_ptr<File>& record,
    std::uint64_t address, std::uint64_t size) {
    validate(memory, address, size, false);
    std::uint64_t total = 0;
    std::vector<std::uint8_t> bytes;
    while (total < size) {
        bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(size - total, 1U << 20)));
        memory.read_bytes(address + total, bytes);
        std::lock_guard lock(record->mutex);
        if (record->closed || !record->native) { if (total) break; throw ServiceError{ebadf}; }
        // Anonymous pipes cannot report free buffer space: a write blocks while
        // the pipe is full, also with O_NONBLOCK. A closed read end is EPIPE
        // (SIGPIPE's default action is not delivered).
        const auto written = host::files::write(record->native, std::as_bytes(std::span(bytes.data(), bytes.size())));
        if (!written) {
            if (total) break;
            throw ServiceError{written.error() == host::Error::broken_pipe ? 32U : file_failure(written.error())};
        }
        total += *written;
        if (*written != bytes.size()) break;
    }
    return total;
}

// ---- the process_tree record (proc_pidinfo, sysctl and Mach IPC of other processes) ------------------
void DarwinServices::Impl::publish_identity() {
    process_tree::Record record;
    record.pid = process_id(); record.ppid = own_ppid; record.pgid = own_pgid;
    const auto ids = current_credentials();
    record.uid = ids[1]; record.gid = ids[3];
    if (!context.executable.empty()) record.path = guest_path_string(context.executable);
    if (const auto previous = process_tree::self(); previous && previous->pid == record.pid) {
        record.pidversion = previous->pidversion; record.host_pid = previous->host_pid;
        record.start_seconds = previous->start_seconds; record.start_microseconds = previous->start_microseconds;
    }
    static_cast<void>(process_tree::publish(std::move(record)));
}

// ---- inherited state of an Alchemy child --------------------------------------------------------
void DarwinServices::Impl::install_inherited_descriptors() {
    const auto* launch = context.child_launch.get();
    own_pid = launch && launch->pid ? launch->pid : host::process::current_pid();
    own_ppid = launch ? launch->ppid : 1;
    own_pgid = launch && launch->pgid ? launch->pgid : own_pid;
    own_sid = launch && launch->sid ? launch->sid : own_pid;
    publish_identity();
    if (launch) {
        creation_mask = launch->umask & 0777;
        for (std::size_t signal = 1; signal < signal_actions.size(); ++signal)
            if (launch->ignored_signals & (1U << (signal - 1))) signal_actions[signal] = SignalAction{1, 0, 0};
        signal_masks[context.thread(context.main_engine())->identity] = launch->signal_mask & ~((1U << 8) | (1U << 16));
        std::array<bool, 3> present{};
        for (const auto& fd : launch->fds) {
            if (fd.fd < 0 || fd.fd >= 4096) continue;
            if (fd.kind == ChildFdKind::handle && fd.fd < 3) {
                // Standard streams that are not pipes stay the host standard handles.
                const auto type = host::files::kind(host::Handle{static_cast<std::uintptr_t>(fd.handle)});
                if (type != host::files::Kind::pipe) { present[static_cast<std::size_t>(fd.fd)] = true; continue; }
            }
            if (install_descriptor(fd) && fd.fd < 3) present[static_cast<std::size_t>(fd.fd)] = true;
        }
        for (std::size_t index = 0; index < 3; ++index) if (!present[index]) closed_standard[index] = true;
    }
    // The launcher's --fd descriptors (the top-level process; 3 and up).
    for (const auto& fd : context.host_descriptors)
        if (fd.fd >= 3 && fd.fd < 4096) static_cast<void>(install_descriptor(fd));
}

bool DarwinServices::Impl::install_descriptor(const ChildFd& fd) {
    auto record = std::make_shared<File>();
    record->flags = fd.flags & (3 | 8);
    record->readable = (fd.flags & 3) != 1; record->writable = (fd.flags & 3) != 0;
    if (fd.kind == ChildFdKind::handle) {
        auto handle = host::Handle{static_cast<std::uintptr_t>(fd.handle)};
        const auto type = host::files::kind(handle);
        // A standard pipe stays the host standard handle too (Alchemy's own
        // diagnostics use it after the guest's descriptor table is gone):
        // the guest descriptor owns a duplicate.
        if (fd.fd < 3) {
            const auto duplicated = host::duplicate(handle);
            if (!duplicated) return false;
            handle = *duplicated;
        }
        record->native = handle;   // O_APPEND is in record->flags
        if (type == host::files::Kind::pipe) {
            record->pipe = true;
            record->pipe_status = std::make_shared<std::atomic<std::uint32_t>>(static_cast<std::uint32_t>(fd.flags & 4));
        } else if (type == host::files::Kind::character) {
            record->null_device = true;
            record->path = "/dev/null";
        } else {
            record->path = utf8_path(fd.path);
            record->flock_owner = std::make_shared<FlockOwner>();
        }
    } else if (fd.kind == ChildFdKind::directory) {
        record->directory = true; record->path = utf8_path(fd.path); record->flags = 0x100000; record->readable = true; record->writable = false;
    } else if (fd.kind == ChildFdKind::random) {
        record->random_device = true; record->path = "/dev/urandom"; record->readable = true; record->writable = false;
        record->random_flags = std::make_shared<std::uint64_t>(fd.flags & 4);
    } else if (fd.kind == ChildFdKind::socket) {
        const auto imported = host::net::import_socket(fd.blob);
        if (!imported) return false;
        auto socket = std::make_shared<NetworkSocket>();
        socket->session = network_session;
        socket->handle = *imported;
        socket->domain = fd.socket_domain; socket->type = fd.socket_type; socket->protocol = fd.socket_protocol;
        socket->status_flags = 2U | static_cast<std::uint32_t>(fd.flags & 4);
        { std::lock_guard lock(mutex); socket->identity = ++network_identity; }
        if (fd.terminal) attach_terminal(socket, fd.terminal - 1, fd.terminal_master);
        record->socket = std::move(socket); record->readable = record->writable = true; record->flags = 2;
    } else return false;
    std::lock_guard lock(mutex);
    files[fd.fd] = std::move(record);
    return true;
}

std::optional<std::optional<std::uint32_t>> DarwinServices::Impl::child_status(std::uint32_t pid) {
    std::lock_guard lock(children_mutex);
    const auto found = children.find(pid);
    if (found == children.end()) return std::nullopt;
    const auto& child = found->second;
    if (child.pseudo_status) return std::optional<std::uint32_t>(*child.pseudo_status);
    if (const auto code = ended_child(child.process)) {
        // ALCHEMY_TRACE_PROCESS=1: each child's end, once, with its host exit code.
        static const bool trace = [] { const auto value = host::process::environment("ALCHEMY_TRACE_PROCESS"); return value && value->starts_with("1"); }();
        static std::set<std::uint32_t> reported;
        if (trace && reported.insert(pid).second)
            std::cerr << "[alchemy process] " << process_id() << ": child " << pid << " ended (host exit code 0x" << std::hex << *code << std::dec
                      << ")" << std::endl;
        return std::optional<std::uint32_t>(darwin_wait_status(*code));
    }
    if (child.kill_certain) return std::optional<std::uint32_t>(child.killed_by & 0x7f);
    return std::optional<std::uint32_t>{};
}

// ---- children reaped before their host process has ended ------------------------------------------
// The host handle keeps the pid from being reused and the pid stays out of
// kill(); a host thread-pool callback closes the handle when the process has
// really exited (never on a guest thread, under no lock held across guest
// calls). The child's own process-tree record and Mach broker registration
// end with its process, as for any child.
void DarwinServices::Impl::retire_child_process(void* process, std::uint32_t pid) {
    struct Watch {
        host::Handle process;
        std::uint32_t pid{};
        std::shared_ptr<EndingChildren> ending;
        std::atomic<std::uintptr_t> wait{};
    };
    const auto handle = child_process(process);
    { std::lock_guard lock(ending_children->mutex); ending_children->pids.insert(pid); }
    auto* watch = new Watch{handle, pid, ending_children};
    const auto registered = host::sync::watch_once(handle, [](void* context) {
        auto* watch = static_cast<Watch*>(context);
        // The registration stores `wait` before the process can end it here.
        while (!watch->wait.load()) std::this_thread::yield();
        { std::lock_guard lock(watch->ending->mutex); watch->ending->pids.erase(watch->pid); }
        host::sync::unwatch(host::Handle{watch->wait.load()}, false);
        host::close(watch->process);
        delete watch;
    }, watch);
    if (registered) { watch->wait.store(registered->value); return; }
    // No watch (the host refused it): wait for the end on a thread of its own.
    std::thread([watch] {
        const std::array handles{watch->process};
        static_cast<void>(host::sync::wait_any(handles, std::nullopt));
        { std::lock_guard lock(watch->ending->mutex); watch->ending->pids.erase(watch->pid); }
        host::close(watch->process);
        delete watch;
    }).detach();
}

// ---- fork/vfork emulation -------------------------------------------------------------------------
namespace {
// q0..q31 of the forking thread, when the engine accepts an initial vector state.
template <class Engine>
void set_initial_vectors(Engine& engine, const transmute::CpuState& state) {
    if constexpr (requires { engine.set_initial_vector(0u, std::array<std::uint64_t, 2>{}); })
        for (unsigned index = 0; index < 32; ++index) engine.set_initial_vector(index, state.v[index]);
}
// The table a child starts from: the caller's (process table or overlay).
template <class Impl, class Overlay>
Overlay snapshot_table(Impl& impl, std::filesystem::path cwd, std::uint64_t umask) {
    Overlay result;
    if (Impl::overlay) { result = *Impl::overlay; return result; }
    result.cwd = std::move(cwd); result.umask = umask;
    std::lock_guard lock(impl.mutex);
    for (int index = 0; index < 3; ++index) {
        if (impl.files.contains(index) || impl.closed_standard[static_cast<std::size_t>(index)]) continue;
        auto standard = std::make_shared<typename Impl::File>();
        standard->standard = index; standard->readable = index == 0; standard->writable = index != 0;
        standard->flags = index == 0 ? 0 : 1;
        result.slots[index] = {std::move(standard), impl.standard_cloexec[static_cast<std::size_t>(index)]};
    }
    for (const auto& [descriptor, record] : impl.files) {
        if (record->closed) continue;
        result.slots[descriptor] = {record, record->cloexec};
    }
    return result;
}
} // namespace

namespace {
// Child descriptors for spawn_child from a table: every descriptor without
// close-on-exec (CLOEXEC_DEFAULT: only `inherited` ones).
template <class Overlay>
std::vector<ChildFd> child_descriptors(const Overlay& table, const std::set<int>* inherited = nullptr) {
    std::vector<ChildFd> result;
    for (const auto& [descriptor, slot] : table.slots) {
        if (inherited ? !inherited->contains(descriptor) : slot.cloexec) continue;
        const auto& record = *slot.file;
        if (record.closed) continue;
        ChildFd entry; entry.fd = descriptor;
        if (record.standard >= 0) { entry.kind = ChildFdKind::standard; entry.standard = record.standard; }
        else if (record.socket) {
            entry.kind = ChildFdKind::socket; entry.handle = static_cast<std::uint64_t>(record.socket->handle.value);
            entry.socket_domain = record.socket->domain; entry.socket_type = record.socket->type; entry.socket_protocol = record.socket->protocol;
            entry.flags = record.socket->status_flags.load() & 4;
            if (record.socket->terminal) { entry.terminal = record.socket->terminal->number + 1; entry.terminal_master = record.socket->terminal_master; }
        } else if (record.directory) { entry.kind = ChildFdKind::directory; entry.path = path_text(record.path); }
        else if (record.random_device) { entry.kind = ChildFdKind::random; entry.flags = *record.random_flags & 4; }
        else if (record.queue) continue;   // kqueues are not inherited (as on Darwin)
        else if (record.native) {
            entry.kind = ChildFdKind::handle;
            entry.handle = static_cast<std::uint64_t>(record.native.value);
            entry.flags = (record.readable && record.writable ? 2 : record.writable ? 1 : 0) | (record.flags & 8) |
                (record.pipe ? (record.pipe_status->load() & 4) : 0);
            entry.path = path_text(record.path);
        } else continue;
        result.push_back(std::move(entry));
    }
    return result;
}
} // namespace

void DarwinServices::Impl::install_process_services() {
    // ---- portable descriptors (Mach fileports) ----
    context.export_descriptor = [this](int fd, std::uint32_t target) -> std::optional<PortableDescriptor> {
        std::shared_ptr<File> record;
        try { record = descriptor_file(fd); } catch (const ServiceError&) { return std::nullopt; }
        if (!record || record->closed || record->queue) return std::nullopt;
        PortableDescriptor out;
        if (record->directory) { out.kind = 2; out.path = path_text(record->path); return out; }
        if (record->random_device) { out.kind = 3; out.flags = *record->random_flags & 4; return out; }
        if (record->socket) {
            auto blob = host::net::export_socket(record->socket->handle, target);
            if (!blob) return std::nullopt;
            out.kind = 5;
            out.blob = std::move(*blob);
            out.socket_domain = record->socket->domain; out.socket_type = record->socket->type; out.socket_protocol = record->socket->protocol;
            out.flags = record->socket->status_flags.load() & 4;
            return out;
        }
        host::Handle source;
        using host::process::Standard;
        if (record->standard >= 0)
            source = host::process::standard_handle(record->standard == 0 ? Standard::input : record->standard == 1 ? Standard::output : Standard::error);
        else if (record->native) source = record->native;
        if (!source) return std::nullopt;
        const auto copy = host::process::duplicate_into(source, target);
        if (!copy) return std::nullopt;
        out.kind = 1;
        out.handle = *copy;
        out.flags = (record->readable && record->writable ? 2 : record->writable ? 1 : 0) | (record->flags & 8) |
            (record->pipe && record->pipe_status ? (record->pipe_status->load() & 4) : 0);
        out.path = path_text(record->path);
        return out;
    };
    context.import_descriptor = [this](const PortableDescriptor& in) -> int {
        auto record = std::make_shared<File>();
        record->flags = in.flags & (3 | 8);
        record->readable = (in.flags & 3) != 1; record->writable = (in.flags & 3) != 0;
        if (in.kind == 2) { record->directory = true; record->path = utf8_path(in.path); record->flags = 0x100000; record->readable = true; record->writable = false; }
        else if (in.kind == 3) {
            record->random_device = true; record->path = "/dev/urandom"; record->readable = true; record->writable = false;
            record->random_flags = std::make_shared<std::uint64_t>(in.flags & 4);
        }
        else if (in.kind == 1) {
            const auto handle = host::Handle{static_cast<std::uintptr_t>(in.handle)};
            const auto type = host::files::kind(handle);
            record->native = handle;   // O_APPEND is in record->flags
            if (type == host::files::Kind::pipe) {
                record->pipe = true;
                record->pipe_status = std::make_shared<std::atomic<std::uint32_t>>(static_cast<std::uint32_t>(in.flags & 4));
            } else if (type == host::files::Kind::character) {
                record->null_device = true;
                record->path = "/dev/null";
            } else {
                record->path = utf8_path(in.path);
                record->flock_owner = std::make_shared<FlockOwner>();
            }
        } else if (in.kind == 5) {
            const auto imported = host::net::import_socket(in.blob);
            if (!imported) return -1;
            auto socket = std::make_shared<NetworkSocket>();
            socket->session = network_session;
            socket->handle = *imported;
            socket->domain = in.socket_domain; socket->type = in.socket_type; socket->protocol = in.socket_protocol;
            socket->status_flags = 2U | static_cast<std::uint32_t>(in.flags & 4);
            { std::lock_guard lock(mutex); socket->identity = ++network_identity; }
            record->socket = std::move(socket); record->readable = record->writable = true; record->flags = 2;
        }
        else return -1;
        try { return add_file(std::move(record)); } catch (const ServiceError&) { return -1; }
    };
    // ---- identity ----
    install("_getppid", [this](auto&, auto&) { return static_cast<std::uint64_t>(own_ppid); });
    install("_getpgrp", [this](auto&, auto&) { return static_cast<std::uint64_t>(fork_child ? fork_child->pgid : own_pgid); });
    install("_getpgid", [this](auto& s, auto&) {
        const auto pid = static_cast<std::uint32_t>(s.x[0]);
        if (!pid || pid == process_id()) return static_cast<std::uint64_t>(fork_child ? fork_child->pgid : own_pgid);
        std::lock_guard lock(children_mutex);
        const auto found = children.find(pid);
        if (found == children.end()) throw ServiceError{esrch_error};
        return static_cast<std::uint64_t>(found->second.pgid);
    });
    install("_getsid", [this](auto& s, auto&) {
        const auto pid = static_cast<std::uint32_t>(s.x[0]);
        if (!pid || pid == process_id()) return static_cast<std::uint64_t>(fork_child ? fork_child->sid : own_sid);
        std::lock_guard lock(children_mutex);
        if (!children.contains(pid)) throw ServiceError{esrch_error};
        return static_cast<std::uint64_t>(own_sid);
    });
    install("_setpgid", [this](auto& s, auto&) {
        const auto pid = static_cast<std::uint32_t>(s.x[0]), group = static_cast<std::uint32_t>(s.x[1]);
        if (static_cast<std::int32_t>(s.x[1]) < 0) throw ServiceError{einval};
        if (fork_child && !pid) {
            // Recorded for exec: 0 makes the child its own group leader.
            if (!group) fork_child->new_group = true; else { fork_child->pgid = group; fork_child->new_group = false; }
            return std::uint64_t{0};
        }
        if (!pid || pid == process_id()) { own_pgid = group ? group : process_id(); publish_identity(); return std::uint64_t{0}; }
        std::lock_guard lock(children_mutex);
        const auto found = children.find(pid);
        if (found == children.end()) throw ServiceError{esrch_error};
        found->second.pgid = group ? group : pid;   // bookkeeping for kill(-pgid) and waitpid(-pgid)
        return std::uint64_t{0};
    });
    install("_setpgrp", [this](auto&, auto&) {
        if (fork_child) { fork_child->new_group = true; return std::uint64_t{0}; }
        own_pgid = process_id(); publish_identity(); return std::uint64_t{0};
    });
    install("_setsid", [this](auto&, auto&) {
        if (fork_child) {
            // The new session's id is the child's pid, unknown until exec creates it.
            fork_child->new_session = true; fork_child->new_group = true;
            return static_cast<std::uint64_t>(fork_child->pseudo_pid);
        }
        if (own_pgid == process_id()) throw ServiceError{eperm_error};   // a group leader cannot start a session
        own_sid = own_pgid = process_id();
        publish_identity();
        return static_cast<std::uint64_t>(own_sid);
    });

    // ---- pipes ----
    install("_pipe", [this](auto& s, auto& m) {
        validate(m, s.x[0], 8, true);
        const auto ends = create_pipe(false);
        m.write32(s.x[0], static_cast<std::uint32_t>(ends[0])); m.write32(s.x[0] + 4, static_cast<std::uint32_t>(ends[1]));
        return std::uint64_t{0};
    });

    // ---- child table helpers ----
    const auto register_child = [this](const SpawnedProcess& spawned, std::uint32_t pgid, std::string program, bool waitable) {
        std::lock_guard lock(children_mutex);
        Child child; child.process = spawned.process; child.pid = spawned.pid; child.pgid = pgid ? pgid : spawned.pid;
        child.program = std::move(program); child.waitable = waitable;
        children[spawned.pid] = std::move(child);
    };
    const auto read_strings = [](transmute::GuestMemory& m, std::uint64_t vector) {
        std::vector<std::string> result;
        if (!vector) return result;
        for (std::uint64_t index = 0; index < 65536; ++index) {
            validate(m, vector + index * 8, 8, false);
            const auto item = m.read64(vector + index * 8);
            if (!item) return result;
            result.push_back(text(m, item));
        }
        throw ServiceError{7};   // E2BIG
    };
    const auto current_environment = [this] {
        std::lock_guard lock(mutex);
        if (fork_child && fork_child->environment) return *fork_child->environment;
        return context.environment;
    };
    // The request every spawn path shares; the caller sets the target and descriptors.
    const auto base_request = [this](const std::vector<std::string>& envp) {
        SpawnRequest request;
        request.envp = envp;
        request.cwd = overlay ? overlay->cwd : [&] { std::lock_guard lock(mutex); return context.working_directory; }();
        request.ppid = process_id();
        request.pgid = fork_child ? fork_child->pgid : own_pgid;
        request.sid = fork_child ? fork_child->sid : own_sid;
        { std::lock_guard lock(mutex); request.umask = static_cast<std::uint32_t>(overlay ? overlay->umask : creation_mask); }
        const auto& table = signal_table();
        for (std::size_t signal = 1; signal < table.size(); ++signal)
            if (table[signal].handler == 1) request.ignored_signals |= 1U << (signal - 1);
        { std::lock_guard lock(mutex); request.signal_mask = signal_masks[context.current_thread()->identity]; }
        // Identity: a child of a process whose real or effective user is root
        // starts as root (ALCHEMY_SUPERUSER=1), whatever environment exec passes.
        std::erase_if(request.envp, [](const std::string& entry) { return entry.starts_with("ALCHEMY_SUPERUSER="); });
        if (const auto ids = current_credentials(); superuser && (ids[0] == 0 || ids[1] == 0))
            request.envp.push_back("ALCHEMY_SUPERUSER=1");
        request.launch_options = context.launch_options;
        if (const auto root = configured_alchemy_root()) {
            request.launch_options.push_back("--root");
            request.launch_options.push_back(path_text(*root));
        }
        return request;
    };
    const auto resolve = [this](const std::string& path, std::vector<std::string> argv, std::vector<std::string>& envp) {
        try {
            auto target = resolve_exec(path, std::move(argv), envp, exec_environment());
            if (target.tool == "env") {
                // env with no utility prints the environment: the shell tool does it.
                std::vector<std::string> argument_vector{"sh", "-c", "env"};
                target.argv = std::move(argument_vector);
                target.tool = "sh";
            }
            if (target.tool == "sh") target.host_program = context.executable;   // python alias hint for the shell
            return target;
        } catch (const ProcessError& error) {
            if (error.error() == enoexec_error || error.error() == eacces)
                std::cerr << "alchemy: exec " << path << ": " << error.what() << std::endl;
            throw ServiceError{error.error()};
        }
    };
    // Replaces this process image: the new image keeps this pid (spec pid);
    // every other thread stops; this process waits for it and exits with its status.
    const auto exec_in_place = [this](SpawnRequest request) -> std::uint64_t {
        request.pid = process_id(); request.ppid = own_ppid; request.pgid = own_pgid; request.sid = own_sid;
        SpawnedProcess spawned;
        try { spawned = spawn_child(request); }
        catch (const ProcessError& error) { throw ServiceError{error.error()}; }
        std::cout.flush(); std::cerr.flush();
        // Other threads of the old image never run again.
        host::process::suspend_other_threads();
        const std::array processes{child_process(spawned.process)};
        static_cast<void>(host::sync::wait_any(processes, std::nullopt));
        host::process::terminate_self(host::process::exit_code(processes[0]).value_or(0));
    };
    // Spawns `request` from the current table (process or fork child) and
    // returns the pid; registers it. In a fork child it is the exec.
    const auto start = [this, register_child](SpawnRequest& request, const std::string& program) {
        // Every child: services children reach (the Mach bootstrap broker) start listening first.
        if (context.spawn_starting) context.spawn_starting(0);
        SpawnedProcess spawned;
        try { spawned = spawn_child(request); }
        catch (const ProcessError& error) {
            std::cerr << "alchemy: cannot start " << program << ": " << error.what() << std::endl;
            throw ServiceError{error.error()};
        }
        const auto pgid = request.new_group ? spawned.pid : request.pgid;
        register_child(spawned, pgid, program, fork_child == nullptr);
        if (fork_child) fork_child->spawned_any = true;
        return spawned;
    };

    // ---- exec ----
    const auto execute = [this, read_strings, base_request, resolve, exec_in_place, start](transmute::GuestMemory& m,
        const std::string& path, std::vector<std::string> argv, std::vector<std::string> envp) -> std::uint64_t {
        if (fork_child) fork_child->exec_attempted = true;   // a failed exec may end in _exit
        auto request = base_request(envp);
        request.target = resolve(path, std::move(argv), request.envp);
        if (fork_child) {
            // The child side of an emulated fork: the real child process starts here.
            request.fds = child_descriptors(fork_child->table);
            request.new_group = fork_child->new_group; request.new_session = fork_child->new_session;
            if (fork_child->new_group) request.pgid = 0;
            if (fork_child->new_session) request.sid = 0;
            auto spawned = start(request, path);
            // start() registered it as a grandchild; the fork parent owns it.
            { std::lock_guard lock(children_mutex); children.erase(spawned.pid); }
            fork_child->exec_process = spawned;
            fork_child->exec_program = path;
            throw ForkChildEnd{};
        }
        static_cast<void>(m);
        FdOverlay table = snapshot_table<Impl, FdOverlay>(*this, request.cwd, request.umask);
        request.fds = child_descriptors(table);
        return exec_in_place(std::move(request));
    };
    install("_execve", [this, read_strings, execute](auto& s, auto& m) -> std::uint64_t {
        const auto path = text(m, s.x[0]);
        auto envp = s.x[2] ? read_strings(m, s.x[2]) : std::vector<std::string>{};
        return execute(m, path, read_strings(m, s.x[1]), std::move(envp));
    });
    install("_execv", [this, read_strings, execute, current_environment](auto& s, auto& m) -> std::uint64_t {
        return execute(m, text(m, s.x[0]), read_strings(m, s.x[1]), current_environment());
    });
    for (const auto name : {"_execvp", "_execvP"}) install(name, [this, read_strings, execute, current_environment,
        searched = std::string_view(name) == "_execvP"](auto& s, auto& m) -> std::uint64_t {
        auto envp = current_environment();
        const auto file = text(m, s.x[0]);
        auto search = envp;
        if (searched) { std::erase_if(search, [](const std::string& entry) { return entry.starts_with("PATH="); }); search.push_back("PATH=" + text(m, s.x[1])); }
        std::string path;
        try { path = search_path(file, search, exec_environment()); }
        catch (const ProcessError& error) { throw ServiceError{error.error()}; }
        return execute(m, path, read_strings(m, s.x[searched ? 2 : 1]), std::move(envp));
    });
    // execl(path, arg0, ...), execle(path, arg0, ..., NULL, envp), execlp(file, arg0, ...):
    // Apple ARM64 passes the variadic arguments in 8-byte stack slots.
    for (const auto name : {"_execl", "_execle", "_execlp"}) install(name, [this, read_strings, execute, current_environment,
        kind = std::string_view(name)](auto& s, auto& m) -> std::uint64_t {
        std::vector<std::string> argv;
        if (s.x[1]) argv.push_back(text(m, s.x[1]));
        std::uint64_t slot = s.sp;
        if (s.x[1]) for (unsigned index = 0; index < 4096; ++index, slot += 8) {
            const auto item = m.read64(slot);
            if (!item) break;
            argv.push_back(text(m, item));
        }
        auto envp = current_environment();
        if (kind == "_execle") envp = read_strings(m, m.read64(slot + 8));
        auto path = text(m, s.x[0]);
        if (kind == "_execlp") {
            try { path = search_path(path, envp, exec_environment()); }
            catch (const ProcessError& error) { throw ServiceError{error.error()}; }
        }
        return execute(m, path, std::move(argv), std::move(envp));
    });

    // ---- fork / vfork ----
    const auto fork = [this, register_child](transmute::CpuState& s, transmute::GuestMemory& m) -> std::uint64_t {
        auto& parent_engine = context.calling_engine();
        // The stack above the caller's SP is restored after the child: the
        // child's writes to the frames it shares with the parent stay private.
        const auto region = m.query_region(s.sp);
        if (!region || s.sp < region->base || s.sp > region->base + region->size) throw ServiceError{enosys};
        const auto top = region->base + region->size;
        std::vector<std::uint8_t> stack(static_cast<std::size_t>(top - s.sp));
        m.read_bytes(s.sp, stack);

        auto child = std::make_shared<ForkChild>(*this);
        {
            std::filesystem::path cwd; std::uint64_t mask;
            { std::lock_guard lock(mutex); cwd = context.working_directory; mask = creation_mask; }
            child->table = snapshot_table<Impl, FdOverlay>(*this, cwd, mask);
        }
        { std::lock_guard lock(mutex); child->actions = signal_table(); }
        child->pgid = fork_child ? fork_child->pgid : own_pgid;
        child->sid = fork_child ? fork_child->sid : own_sid;
        { std::lock_guard lock(children_mutex); child->pseudo_pid = next_pseudo_pid; next_pseudo_pid += 2; }

        auto worker = EngineAdapter(parent_engine).create_worker(s.x[30], 0, 256 * 1024);
        worker->set_instruction_limit(context.instruction_limit);
        for (unsigned index = 1; index < 31; ++index) worker->set_initial_register(index, s.x[index]);
        worker->set_initial_register(0, 0);   // fork() returns 0 in the child
        try { worker->set_initial_stack_pointer(s.sp); }
        catch (const std::runtime_error&) {
            // The reference engine keeps every thread on its own stack.
            std::cerr << "alchemy: fork emulation needs an engine that runs a thread on its parent's stack "
                         "(alchemy-fx); fork() fails with ENOSYS" << std::endl;
            throw ServiceError{enosys};
        }
        set_initial_vectors(*worker, s);
        std::string failure;
        bool stopped = false;
        std::thread host([&] {
            const auto previous_overlay = overlay;
            const auto previous_child = fork_child;
            auto& guard = ProcessContext::fork_child_guard();
            const auto previous_guard = guard;
            overlay = &child->table; fork_child = child.get(); guard = child.get();
            try {
                static_cast<void>(worker->run());
                if (!child->exec_process && !child->exit_status && !engine_stop_requested(*worker))
                    failure = "the child returned from the function that called fork without exec";
                stopped = engine_stop_requested(*worker);
            } catch (const ForkChildEnd&) {
            } catch (const ForkChildViolation& violation) {
                failure = violation.message;
            } catch (const std::exception& error) {
                failure = std::string("the child failed before exec: ") + error.what();
            } catch (...) {
                stopped = true;   // process stop (the engine unwinds the thread)
            }
            overlay = previous_overlay; fork_child = previous_child; guard = previous_guard;
            if (const auto thread = context.thread(*worker))
                for (const auto& [module, address] : thread->tls_blocks) { static_cast<void>(module); try { context.release_guest(address); } catch (...) {} }
            context.release_thread(*worker);
        });
        host.join();
        worker.reset();
        m.write_bytes(s.sp, stack);
        for (const auto address : child->allocations) { try { release(address); } catch (...) {} }
        if (stopped) throw ServiceError{89};
        if (!failure.empty() || (child->exit_status && !child->exec_attempted && !child->spawned_any)) {
            if (failure.empty()) failure = "the child called _exit before any exec (a fork without exec is not supported)";
            std::cerr << "alchemy: fork emulation: " << failure << "; fork() fails with ENOSYS" << std::endl;
            throw ServiceError{enosys};
        }
        if (child->exec_process) {
            const auto& spawned = *child->exec_process;
            register_child(spawned, child->new_group ? spawned.pid : child->pgid, child->exec_program, true);
            if (fork_child) fork_child->spawned_any = true;
            return spawned.pid;
        }
        // _exit after a failed exec (or after starting a grandchild): a child
        // that has already ended with that status.
        std::lock_guard lock(children_mutex);
        Child pseudo; pseudo.pid = child->pseudo_pid; pseudo.pgid = child->pgid;
        pseudo.pseudo_status = (*child->exit_status & 0xff) << 8;
        children[pseudo.pid] = std::move(pseudo);
        return child->pseudo_pid;
    };
    install("_fork", fork);
    install("_vfork", fork);

    // ---- posix_spawn ----
    install("_posix_spawn_file_actions_init", [this](auto& s, auto& m) -> std::uint64_t {
        validate(m, s.x[0], 8, true);
        const auto token = context.allocate_guest(16);
        { std::lock_guard lock(mutex); spawn_actions[token] = std::make_shared<SpawnFileActions>(); }
        m.write64(s.x[0], token); return 0;
    }, enomem);
    install("_posix_spawn_file_actions_destroy", [this](auto& s, auto& m) -> std::uint64_t {
        validate(m, s.x[0], 8, true);
        const auto token = m.read64(s.x[0]);
        { std::lock_guard lock(mutex); if (!spawn_actions.erase(token)) return einval; }
        context.release_guest(token); m.write64(s.x[0], 0); return 0;
    }, einval);
    const auto actions_of = [this](transmute::GuestMemory& m, std::uint64_t address) {
        validate(m, address, 8, false);
        std::lock_guard lock(mutex);
        const auto found = spawn_actions.find(m.read64(address));
        if (found == spawn_actions.end()) throw ServiceError{einval};
        return found->second;
    };
    // Error-number convention: these return the error instead of setting errno.
    const auto add_action = [this, actions_of](auto& s, auto& m, int kind) -> std::uint64_t {
        try {
            const auto actions = actions_of(m, s.x[0]);
            SpawnFileActions::Action action; action.kind = kind; action.fd = static_cast<std::int32_t>(s.x[1]);
            if (kind != 4 && (action.fd < 0 || action.fd >= 4096)) return ebadf;
            if (kind == 1) { action.target = static_cast<std::int32_t>(s.x[2]); if (action.target < 0 || action.target >= 4096) return ebadf; }
            if (kind == 2) { action.path = text(m, s.x[2]); action.flags = s.x[3]; action.mode = s.x[4]; }
            if (kind == 4) { action.path = text(m, s.x[1]); action.fd = -1; }
            std::lock_guard lock(mutex);
            actions->actions.push_back(std::move(action));
            return 0;
        } catch (const ServiceError& error) { return error.value; }
    };
    install("_posix_spawn_file_actions_addclose", [add_action](auto& s, auto& m) { return add_action(s, m, 0); });
    install("_posix_spawn_file_actions_adddup2", [add_action](auto& s, auto& m) { return add_action(s, m, 1); });
    install("_posix_spawn_file_actions_addopen", [add_action](auto& s, auto& m) { return add_action(s, m, 2); });
    install("_posix_spawn_file_actions_addinherit_np", [add_action](auto& s, auto& m) { return add_action(s, m, 3); });
    install("_posix_spawn_file_actions_addchdir_np", [add_action](auto& s, auto& m) { return add_action(s, m, 4); });
    install("_posix_spawn_file_actions_addfchdir_np", [add_action](auto& s, auto& m) { return add_action(s, m, 5); });

    install("_posix_spawnattr_init", [this](auto& s, auto& m) -> std::uint64_t {
        validate(m, s.x[0], 8, true);
        const auto token = context.allocate_guest(16);
        { std::lock_guard lock(mutex); spawn_attributes[token] = std::make_shared<SpawnAttributes>(); }
        m.write64(s.x[0], token); return 0;
    }, enomem);
    install("_posix_spawnattr_destroy", [this](auto& s, auto& m) -> std::uint64_t {
        validate(m, s.x[0], 8, true);
        const auto token = m.read64(s.x[0]);
        { std::lock_guard lock(mutex); if (!spawn_attributes.erase(token)) return einval; }
        context.release_guest(token); m.write64(s.x[0], 0); return 0;
    }, einval);
    const auto attributes_of = [this](transmute::GuestMemory& m, std::uint64_t address) {
        validate(m, address, 8, false);
        std::lock_guard lock(mutex);
        const auto found = spawn_attributes.find(m.read64(address));
        if (found == spawn_attributes.end()) throw ServiceError{einval};
        return found->second;
    };
    // posix_spawnattr_{set,get}{flags,pgroup,sigmask,sigdefault} and the Darwin extensions.
    const auto attribute = [this, attributes_of](const char* name, auto operation) {
        install(name, [attributes_of, operation](auto& s, auto& m) -> std::uint64_t {
            try { return operation(*attributes_of(m, s.x[0]), s, m); }
            catch (const ServiceError& error) { return error.value; }
        }, einval);
    };
    attribute("_posix_spawnattr_setflags", [](SpawnAttributes& a, auto& s, auto&) -> std::uint64_t {
        constexpr std::uint32_t known = 0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x80 | 0x100 | 0x400 | 0x4000;
        if (static_cast<std::uint16_t>(s.x[1]) & ~known) return einval;
        a.flags = static_cast<std::uint16_t>(s.x[1]); return 0; });
    attribute("_posix_spawnattr_getflags", [](SpawnAttributes& a, auto& s, auto& m) -> std::uint64_t {
        validate(m, s.x[1], 2, true); m.write8(s.x[1], static_cast<std::uint8_t>(a.flags)); m.write8(s.x[1] + 1, static_cast<std::uint8_t>(a.flags >> 8)); return 0; });
    attribute("_posix_spawnattr_setpgroup", [](SpawnAttributes& a, auto& s, auto&) -> std::uint64_t { a.pgroup = static_cast<std::uint32_t>(s.x[1]); return 0; });
    attribute("_posix_spawnattr_getpgroup", [](SpawnAttributes& a, auto& s, auto& m) -> std::uint64_t { validate(m, s.x[1], 4, true); m.write32(s.x[1], a.pgroup); return 0; });
    attribute("_posix_spawnattr_setsigmask", [](SpawnAttributes& a, auto& s, auto& m) -> std::uint64_t { validate(m, s.x[1], 4, false); a.sigmask = m.read32(s.x[1]); return 0; });
    attribute("_posix_spawnattr_getsigmask", [](SpawnAttributes& a, auto& s, auto& m) -> std::uint64_t { validate(m, s.x[1], 4, true); m.write32(s.x[1], a.sigmask); return 0; });
    attribute("_posix_spawnattr_setsigdefault", [](SpawnAttributes& a, auto& s, auto& m) -> std::uint64_t { validate(m, s.x[1], 4, false); a.sigdefault = m.read32(s.x[1]); return 0; });
    attribute("_posix_spawnattr_getsigdefault", [](SpawnAttributes& a, auto& s, auto& m) -> std::uint64_t { validate(m, s.x[1], 4, true); m.write32(s.x[1], a.sigdefault); return 0; });
    attribute("_posix_spawnattr_set_qos_class_np", [](SpawnAttributes& a, auto& s, auto&) -> std::uint64_t { a.qos = static_cast<std::uint32_t>(s.x[1]); return 0; });
    attribute("_posix_spawnattr_get_qos_class_np", [](SpawnAttributes& a, auto& s, auto& m) -> std::uint64_t { validate(m, s.x[1], 4, true); m.write32(s.x[1], a.qos); return 0; });
    // Binary preferences: an arm64 (or any) slice is what Alchemy runs.
    attribute("_posix_spawnattr_setbinpref_np", [](SpawnAttributes&, auto& s, auto& m) -> std::uint64_t {
        const auto count = s.x[1];
        for (std::uint64_t index = 0; index < count && index < 4; ++index) {
            const auto type = m.read32(s.x[2] + index * 4);
            if (type != 0x0100000cU && type != 0xffffffffU && type != 0) return enotsup_error;
        }
        if (s.x[3]) m.write64(s.x[3], count); return 0; });
    attribute("_posix_spawnattr_getbinpref_np", [](SpawnAttributes&, auto& s, auto& m) -> std::uint64_t {
        if (s.x[1]) m.write32(s.x[2], 0x0100000cU);
        if (s.x[3]) m.write64(s.x[3], s.x[1] ? 1 : 0); return 0; });

    for (const auto searching : {false, true}) install(searching ? "_posix_spawnp" : "_posix_spawn",
        [this, searching, read_strings, base_request, resolve, exec_in_place, start, actions_of, attributes_of, current_environment](auto& s, auto& m) -> std::uint64_t {
        try {
            SpawnFileActions actions; SpawnAttributes attributes;
            if (s.x[2]) { const auto value = actions_of(m, s.x[2]); std::lock_guard lock(mutex); actions = *value; }
            if (s.x[3]) attributes = *attributes_of(m, s.x[3]);
            if (attributes.flags & 0x80) {   // POSIX_SPAWN_START_SUSPENDED
                std::cerr << "alchemy: posix_spawn: POSIX_SPAWN_START_SUSPENDED is unsupported" << std::endl;
                return enotsup_error;
            }
            auto envp = s.x[5] ? read_strings(m, s.x[5]) : current_environment();
            auto path = text(m, s.x[1]);
            if (searching) {
                try { path = search_path(path, current_environment(), exec_environment()); }
                catch (const ProcessError& error) { return error.error(); }
            }
            auto request = base_request(envp);
            // File actions run, in order, on a copy of the caller's table.
            FdOverlay table = snapshot_table<Impl, FdOverlay>(*this, request.cwd, request.umask);
            std::set<int> inherited;
            {
                struct Scope { FdOverlay*& slot; FdOverlay* previous; ~Scope() { slot = previous; } } scope{overlay, overlay};
                overlay = &table;
                for (const auto& action : actions.actions) {
                    switch (action.kind) {
                    case 0: static_cast<void>(close(action.fd)); inherited.erase(action.fd); break;
                    case 1:
                        if (action.fd == action.target) set_descriptor_cloexec(action.target, false);
                        else static_cast<void>(duplicate(action.fd, 0, action.target));
                        inherited.insert(action.target); break;
                    case 2: {
                        const auto opened = open_path(translate_path(action.path), action.flags, action.mode);
                        if (opened != action.fd) { static_cast<void>(duplicate(opened, 0, action.fd)); static_cast<void>(close(opened)); }
                        inherited.insert(action.fd); break;
                    }
                    case 3: set_descriptor_cloexec(action.fd, false); inherited.insert(action.fd); break;
                    case 4: {
                        const auto directory = translate_path(action.path);
                        std::error_code error;
                        if (!std::filesystem::is_directory(directory, error)) throw ServiceError{enotdir};
                        table.cwd = directory; break;
                    }
                    case 5: {
                        const auto record = file(action.fd);
                        if (!record->directory) throw ServiceError{enotdir};
                        table.cwd = record->path; break;
                    }
                    default: break;
                    }
                }
            }
            request.cwd = table.cwd;
            request.fds = child_descriptors(table, (attributes.flags & 0x4000) ? &inherited : nullptr);
            for (std::size_t signal = 1; signal < 32; ++signal)
                if ((attributes.flags & 0x4) && (attributes.sigdefault & (1U << (signal - 1)))) request.ignored_signals &= ~(1U << (signal - 1));
            if (attributes.flags & 0x8) request.signal_mask = attributes.sigmask;
            if (attributes.flags & 0x2) { if (attributes.pgroup) request.pgid = attributes.pgroup; else { request.new_group = true; request.pgid = 0; } }
            if (attributes.flags & 0x400) { request.new_session = request.new_group = true; request.pgid = 0; request.sid = 0; }
            request.target = resolve(path, read_strings(m, s.x[4]), request.envp);
            if (attributes.flags & 0x40) return exec_in_place(std::move(request));   // POSIX_SPAWN_SETEXEC
            const auto attributes_token = s.x[3] ? m.read64(s.x[3]) : 0;
            const auto spawned = start(request, path);
            if (context.spawn_started) context.spawn_started(attributes_token, spawned.pid);
            if (s.x[0]) { validate(m, s.x[0], 4, true); m.write32(s.x[0], spawned.pid); }
            return 0;
        } catch (const ServiceError& error) { return error.value; }
    }, einval);

    // ---- wait ----
    // Selects a child: pid > 0 that pid, -1 any, 0 this group, < -1 group -pid.
    const auto wait_child = [this](std::int64_t selector, std::uint64_t options, bool consume) -> std::pair<std::uint32_t, std::uint32_t> {
        const auto matches = [&](const Child& child) {
            if (!child.waitable) return false;
            if (selector > 0) return child.pid == static_cast<std::uint32_t>(selector);
            if (selector == -1) return true;
            const auto group = selector == 0 ? (fork_child ? fork_child->pgid : own_pgid) : static_cast<std::uint32_t>(-selector);
            return child.pgid == group;
        };
        for (;;) {
            std::vector<host::Handle> pending;
            {
                std::lock_guard lock(children_mutex);
                bool any = false;
                for (auto it = children.begin(); it != children.end(); ++it) {
                    auto& child = it->second;
                    if (!matches(child)) continue;
                    any = true;
                    std::optional<std::uint32_t> status = child.pseudo_status;
                    bool ended = !child.process;
                    if (!status) if (const auto code = ended_child(child.process)) { status = darwin_wait_status(*code); ended = true; }
                    // A child this process ended with SIGKILL (see signal_child) is
                    // reported at once with the signal its exit code carries,
                    // WIFSIGNALED / WTERMSIG = killed_by; wait4's rusage is zero
                    // then (as always here). Alchemy delivers no SIGCHLD, so
                    // nothing else can report the child a second time.
                    if (!status && child.kill_certain) status = child.killed_by & 0x7f;
                    if (!status) { if (child.process) pending.push_back(child_process(child.process)); continue; }
                    const std::pair<std::uint32_t, std::uint32_t> result{child.pid, *status};
                    if (consume) {
                        if (child.process && ended) host::close(child_process(child.process));
                        else if (child.process) retire_child_process(child.process, child.pid);
                        children.erase(it);
                    }
                    return result;
                }
                if (!any) throw ServiceError{echild};
            }
            if (options & 1) return {0, 0};   // WNOHANG
            if (process_exiting(context)) throw ServiceError{eintr_error};
            if (pending.size() > host::sync::max_wait_handles) pending.resize(host::sync::max_wait_handles);
            if (pending.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            else static_cast<void>(host::sync::wait_any(pending, 50'000'000));
        }
    };
    const auto wait_common = [wait_child](auto& s, auto& m, std::int64_t selector, std::uint64_t status_address,
                                          std::uint64_t options, std::uint64_t usage) -> std::uint64_t {
        if (options & ~(1ULL | 2ULL | 0x10ULL)) throw ServiceError{einval};   // WNOHANG, WUNTRACED, WCONTINUED
        if (status_address) validate(m, status_address, 4, true);
        if (usage) validate(m, usage, 144, true);
        static_cast<void>(s);
        const auto [pid, status] = wait_child(selector, options, true);
        if (pid && status_address) m.write32(status_address, status);
        if (pid && usage) m.fill_bytes(usage, 0, 144);
        return pid;
    };
    install("_waitpid", [wait_common](auto& s, auto& m) { return wait_common(s, m, static_cast<std::int32_t>(s.x[0]), s.x[1], s.x[2], 0); });
    install("_wait4", [wait_common](auto& s, auto& m) { return wait_common(s, m, static_cast<std::int32_t>(s.x[0]), s.x[1], s.x[2], s.x[3]); });
    install("_wait", [wait_common](auto& s, auto& m) { return wait_common(s, m, -1, s.x[0], 0, 0); });
    install("_wait3", [wait_common](auto& s, auto& m) { return wait_common(s, m, -1, s.x[0], s.x[1], s.x[2]); });
    for (const auto alias : {"_waitpid$NOCANCEL", "_wait4$NOCANCEL", "_wait$NOCANCEL", "_wait3$NOCANCEL"})
        context.install_export(std::string(system_library), alias,
            *context.resolve(system_library, std::string_view(alias).substr(0, std::string_view(alias).find('$'))));
    // int waitid(idtype_t, id_t, siginfo_t*, int): P_ALL 0, P_PID 1, P_PGID 2;
    // WEXITED 4, WNOHANG 1, WNOWAIT 0x20.
    install("_waitid", [wait_child](auto& s, auto& m) -> std::uint64_t {
        const std::uint64_t type = s.x[0], options = s.x[3];
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        if (type > 2 || !(options & 4) || (options & ~(1ULL | 2ULL | 4ULL | 8ULL | 0x10ULL | 0x20ULL))) throw ServiceError{einval};
        const std::int64_t selector = type == 0 ? -1 : type == 1 ? static_cast<std::int64_t>(id) : -static_cast<std::int64_t>(id);
        if (type == 2 && id <= 1) throw ServiceError{einval};
        const auto [pid, status] = wait_child(selector, options, !(options & 0x20));
        if (s.x[2]) {
            validate(m, s.x[2], 104, true); m.fill_bytes(s.x[2], 0, 104);
            if (pid) {
                const bool signaled = (status & 0x7f) != 0;
                m.write32(s.x[2], 20);                              // si_signo SIGCHLD
                m.write32(s.x[2] + 8, signaled ? 2U : 1U);          // si_code CLD_KILLED / CLD_EXITED
                m.write32(s.x[2] + 12, pid); m.write32(s.x[2] + 16, 1000);
                m.write32(s.x[2] + 20, signaled ? (status & 0x7f) : ((status >> 8) & 0xff));
            }
        }
        return 0;
    });

    // ---- signals to processes ----
    // The calling thread receives a signal it sends to its own process:
    // SIG_IGN and ignored-by-default signals do nothing, a handler runs now,
    // the default action of a terminating signal ends the process.
    const auto deliver_self = [this](std::uint64_t signal) -> std::uint64_t {
        if (!signal) return 0;
        if (context.signal_observed) context.signal_observed(static_cast<int>(signal));
        SignalAction action;
        { std::lock_guard lock(mutex); action = signal_table()[static_cast<std::size_t>(signal)]; }
        if (action.handler == 1 && signal != 9 && signal != 17) return 0;
        if (action.handler > 1 && signal != 9 && signal != 17) {
            if (action.flags & 4) { std::lock_guard lock(mutex); signal_table()[static_cast<std::size_t>(signal)] = SignalAction{}; }   // SA_RESETHAND
            if (action.flags & 0x40) {   // SA_SIGINFO: (signal, siginfo_t*, ucontext*)
                const auto info = context.allocate_guest(104);
                auto& memory = context.calling_engine();
                memory.fill_bytes(info, 0, 104);
                memory.write32(info, static_cast<std::uint32_t>(signal)); memory.write32(info + 12, process_id()); memory.write32(info + 16, 1000);
                try {
                    const std::array<std::uint64_t, 3> arguments{signal, info, 0};
                    static_cast<void>(EngineAdapter(context.calling_engine()).call(action.handler, arguments));
                } catch (...) { context.release_guest(info); throw; }
                context.release_guest(info);
            } else {
                const std::array<std::uint64_t, 1> arguments{signal};
                static_cast<void>(EngineAdapter(context.calling_engine()).call(action.handler, arguments));
            }
            {
                std::lock_guard lock(mutex);
                ++handled_signals;
                last_handled_signal = static_cast<std::uint32_t>(signal);
            }
            signal_handled.notify_all();
            return 0;
        }
        if (signal_ignored_by_default(signal)) return 0;
        if (signal_stops_by_default(signal)) {
            std::cerr << "alchemy: stopping a process (signal " << signal << ") is unsupported" << std::endl;
            throw ServiceError{enotsup_error};
        }
        const auto reason = "Darwin signal " + std::to_string(signal) + " sent to the process itself (default action)";
        if constexpr (!engine_can_stop<transmute::ReferenceEngine>) {
            // The reference engine cannot carry a signal termination out of
            // run(): end the process here with the signal's status.
            std::cout.flush();
            std::cerr << "alchemy: guest terminated by signal " << signal << ": " << reason << std::endl;
            host::process::terminate_self(context.child_launch ? child_signal_exit_marker | static_cast<std::uint32_t>(signal)
                                                               : 128U + static_cast<std::uint32_t>(signal));
        }
        terminate_by_signal(static_cast<int>(signal), reason);
    };
    const auto signal_child = [this](Child& child, std::uint64_t signal) -> std::uint64_t {
        if (!signal || child.pseudo_status || signal_ignored_by_default(signal)) return 0;
        if (signal_stops_by_default(signal)) {
            std::cerr << "alchemy: stopping a child process (signal " << signal << ") is unsupported" << std::endl;
            throw ServiceError{enotsup_error};
        }
        // A child cannot run a handler for a signal sent from outside it
        // (no asynchronous delivery): its default action ends it, with
        // TerminateProcess. A later signal to a child already being ended
        // does nothing (as to a Darwin zombie); its exit code keeps the first.
        //
        // When the wait status is reported early: only after TerminateProcess
        // succeeded, and only once SIGKILL has been sent, which no process can
        // catch, block or ignore. Then no guest code of the child runs again
        // and waitpid reports it at once, as Darwin reaps a killed child at
        // once, instead of after the host's teardown of a large process
        // (seconds). Other terminating signals (SIGTERM, ...) keep waiting for
        // the host process to end: a Darwin child could have caught or ignored
        // them, and this process does not know the child's dispositions.
        if (!child.killed_by && child.process && !ended_child(child.process) &&
            host::process::terminate(child_process(child.process), child_signal_exit_marker | static_cast<std::uint32_t>(signal)))
            child.killed_by = static_cast<std::uint32_t>(signal);
        if (child.killed_by && signal == 9) child.kill_certain = true;
        return 0;
    };
    const auto kill = [this, deliver_self, signal_child](std::int64_t pid, std::uint64_t signal) -> std::uint64_t {
        if (signal > 31) throw ServiceError{einval};
        if (pid > 0) {
            if (static_cast<std::uint32_t>(pid) == process_id() && !fork_child) return deliver_self(signal);
            {
                std::lock_guard lock(children_mutex);
                const auto found = children.find(static_cast<std::uint32_t>(pid));
                if (found != children.end()) return signal_child(found->second, signal);
            }
            // A child already reaped while the host still ends its process.
            {
                std::lock_guard lock(ending_children->mutex);
                if (ending_children->pids.contains(static_cast<std::uint32_t>(pid))) throw ServiceError{esrch_error};
            }
            // Another process of the Alchemy tree (process_tree.hpp): it cannot
            // run a handler for a signal from outside, so a signal whose default
            // action ends a process ends it (its image after exec in place too).
            if (const auto record = process_tree::lookup(static_cast<std::uint32_t>(pid))) {
                if (!signal || signal_ignored_by_default(signal)) return 0;
                if (signal_stops_by_default(signal)) {
                    std::cerr << "alchemy: stopping another process (signal " << signal << ") is unsupported" << std::endl;
                    throw ServiceError{enotsup_error};
                }
                bool ended = false;
                for (const auto host_pid : {record->host_pid, record->pid}) {
                    ended = host::process::terminate_pid(host_pid, child_signal_exit_marker | static_cast<std::uint32_t>(signal)) || ended;
                    if (host_pid == record->pid) break;
                }
                if (!ended) throw ServiceError{eperm_error};
                return 0;
            }
            // Another host process: existence check only.
            const auto running = host::process::is_running(static_cast<std::uint32_t>(pid));
            if (!running) throw ServiceError{running.error() == host::Error::access_denied ? eperm_error : esrch_error};
            if (!*running) throw ServiceError{esrch_error};
            if (signal) throw ServiceError{eperm_error};
            return 0;
        }
        // Process groups: 0 the caller's, -1 every child, < -1 group -pid.
        const auto group = pid == 0 ? own_pgid : static_cast<std::uint32_t>(-pid);
        bool any = false;
        {
            std::lock_guard lock(children_mutex);
            for (auto& [identity, child] : children) {
                static_cast<void>(identity);
                if (child.pseudo_status || !child.waitable) continue;
                if (pid != -1 && child.pgid != group) continue;
                any = true;
                static_cast<void>(signal_child(child, signal));
            }
        }
        if (pid != -1 && group == own_pgid && !fork_child) return deliver_self(signal);
        if (!any) throw ServiceError{esrch_error};
        return 0;
    };
    install("_kill", [kill](auto& s, auto&) { return kill(static_cast<std::int32_t>(s.x[0]), s.x[1]); });
    install("_killpg", [kill](auto& s, auto&) {
        if (static_cast<std::int32_t>(s.x[0]) <= 1) throw ServiceError{einval};
        return kill(-static_cast<std::int64_t>(static_cast<std::int32_t>(s.x[0])), s.x[1]);
    });
    install("_raise", [deliver_self](auto& s, auto&) {
        if (s.x[0] == 0 || s.x[0] > 31) throw ServiceError{einval};
        return deliver_self(s.x[0]);
    });
    // sigsuspend(mask) / pause() / sigpause(signal): the thread's mask becomes `mask` (pause
    // keeps it; sigpause unblocks `signal`) until a handler for a signal outside it has run
    // (raise or kill from any thread of this process), then the old mask is back and the call
    // fails with EINTR, as it always does. Alchemy delivers no asynchronous signals from
    // outside the process (SIGCHLD, SIGALRM, terminal signals), so only those wake it.
    const auto suspend = [this](std::optional<std::uint32_t> mask) -> std::uint64_t {
        const auto identity = context.current_thread()->identity;
        std::unique_lock lock(mutex);
        auto& current = signal_masks[identity];
        const auto previous = current;
        const auto waiting = mask ? *mask & ~((1U << 8) | (1U << 16)) : previous;
        current = waiting;
        const auto seen = handled_signals;
        while (true) {
            if (handled_signals != seen && last_handled_signal && !(waiting & (1U << (last_handled_signal - 1)))) break;
            if (context.requested_exit()) break;
            signal_handled.wait_for(lock, std::chrono::milliseconds(50));
        }
        signal_masks[identity] = previous;
        throw ServiceError{eintr_error};
    };
    for (const auto name : {"_sigsuspend", "_sigsuspend$NOCANCEL"})
        install(name, [suspend](auto& s, auto& m) -> std::uint64_t {
            validate(m, s.x[0], 4, false);
            return suspend(m.read32(s.x[0]));
        });
    install("_pause", [suspend](auto&, auto&) -> std::uint64_t { return suspend(std::nullopt); });
    install("_sigpause", [this, suspend](auto& s, auto&) -> std::uint64_t {
        if (s.x[0] == 0 || s.x[0] > 31) throw ServiceError{einval};
        std::uint32_t mask;
        { std::lock_guard lock(mutex); mask = signal_masks[context.current_thread()->identity]; }
        return suspend(mask & ~(1U << (s.x[0] - 1)));
    });

    // ---- libproc for this process and its children ----
    install("_proc_pidpath", [this](auto& s, auto& m) -> std::uint64_t {
        const auto pid = static_cast<std::uint32_t>(s.x[0]);
        if (!s.x[1] || !static_cast<std::uint32_t>(s.x[2])) throw ServiceError{einval};
        std::optional<std::string> path;
        if (pid == process_id()) { if (!context.executable.empty()) path = guest_path_string(context.executable); }
        else {
            // A running Alchemy process names its own image; else the program a child was started with.
            if (const auto record = process_tree::lookup(pid); record && !record->path.empty()) path = record->path;
            if (!path) {
                std::lock_guard lock(children_mutex);
                const auto found = children.find(pid);
                if (found != children.end() && !found->second.program.empty()) path = found->second.program;
            }
            if (!path) if (const auto image = host::process::image_path(pid)) path = guest_path_string(utf8_path(*image));
        }
        if (!path) throw ServiceError{esrch_error};
        if (path->size() + 1 > std::min<std::uint64_t>(static_cast<std::uint32_t>(s.x[2]), 4096)) throw ServiceError{enomem};
        validate(m, s.x[1], path->size() + 1, true);
        put_text(m, s.x[1], *path);
        return path->size();
    }, 0);

    // ---- popen, pclose, system: the built-in shell ----
    install("_popen", [this, base_request, start, current_environment](auto& s, auto& m) -> std::uint64_t {
        const auto command = text(m, s.x[0]);
        const auto mode = text(m, s.x[1], 8);
        const bool reading = !mode.empty() && mode[0] == 'r';
        if (mode.empty() || (mode[0] != 'r' && mode[0] != 'w') || mode.find_first_not_of("rwe", 1) != std::string::npos ||
            mode.find_first_of("rw", 1) != std::string::npos) throw ServiceError{einval};
        const auto ends = create_pipe(true);
        const int ours = reading ? ends[0] : ends[1], theirs = reading ? ends[1] : ends[0];
        try {
            auto request = base_request(current_environment());
            FdOverlay table = snapshot_table<Impl, FdOverlay>(*this, request.cwd, request.umask);
            {
                struct Scope { FdOverlay*& slot; FdOverlay* previous; ~Scope() { slot = previous; } } scope{overlay, overlay};
                overlay = &table;
                static_cast<void>(duplicate(theirs, 0, reading ? 1 : 0));
            }
            request.fds = child_descriptors(table);
            std::vector<std::string> argv{"sh", "-c", command};
            request.target = ExecTarget{"sh", context.executable, "/bin/sh", std::move(argv)};
            const auto spawned = start(request, "/bin/sh");
            static_cast<void>(close(theirs));
            if (mode.find('e') == std::string::npos) set_descriptor_cloexec(ours, false);
            const auto stream = new_stream(ours);
            std::lock_guard lock(children_mutex);
            popen_streams[stream] = spawned.pid;
            return stream;
        } catch (...) {
            try { static_cast<void>(close(theirs)); } catch (...) {}
            try { static_cast<void>(close(ours)); } catch (...) {}
            throw;
        }
    }, 0);
    install("_pclose", [this, wait_child](auto& s, auto&) -> std::uint64_t {
        std::uint32_t pid;
        {
            std::lock_guard lock(children_mutex);
            const auto found = popen_streams.find(s.x[0]);
            if (found == popen_streams.end()) throw ServiceError{echild};
            pid = found->second; popen_streams.erase(found);
        }
        const auto value = stream(s.x[0]);
        { std::lock_guard stream_lock(*value.lock); static_cast<void>(close(value.descriptor)); }
        { std::lock_guard lock(mutex); streams.erase(s.x[0]); }
        context.release_guest(s.x[0]);
        return wait_child(pid, 0, true).second;
    });
    install("_system", [this, base_request, start, wait_child, current_environment](auto& s, auto& m) -> std::uint64_t {
        if (!s.x[0]) return 1;   // a command processor is available (the built-in shell)
        auto request = base_request(current_environment());
        FdOverlay table = snapshot_table<Impl, FdOverlay>(*this, request.cwd, request.umask);
        request.fds = child_descriptors(table);
        std::vector<std::string> argv{"sh", "-c", text(m, s.x[0])};
        request.target = ExecTarget{"sh", context.executable, "/bin/sh", std::move(argv)};
        try {
            const auto spawned = start(request, "/bin/sh");
            return wait_child(spawned.pid, 0, true).second;
        } catch (const ServiceError&) { return 127U << 8; }
    }, UINT32_MAX);
}

} // namespace alchemy
