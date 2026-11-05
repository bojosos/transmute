// DarwinServices' process identity: credentials, groups, process groups,
// uname, pathconf/confstr, resource limits, umask, signal masks and actions,
// kill, the explicit failures of unsupported process calls, and sync.
#include "alchemy/darwin/libsystem/services_impl.hpp"

namespace alchemy {

void DarwinServices::Impl::install_process_identity() {
    // Nonprivileged virtual Darwin identity (uid/gid 1000). ALCHEMY_SUPERUSER=1
    // in the guest environment runs the process as root (uid/gid 0), the
    // explicit equivalent of sudo for installers that write into the Alchemy
    // root; the files themselves stay owned by the host user.
    // Credentials: {uid, euid, gid, egid}. A superuser process may switch
    // between root and the virtual user like a setuid-root program.
    // An Alchemy child gets the marker from a parent whose real or effective
    // user is root (process/process_model.cpp, base_request).
    superuser = std::find(context.environment.begin(), context.environment.end(),
                          std::string("ALCHEMY_SUPERUSER=1")) != context.environment.end();
    for (auto& id : credential_ids) id = superuser ? 0U : 1000U;
    constexpr std::array<const char*, 4> getters{"_getuid", "_geteuid", "_getgid", "_getegid"};
    for (std::size_t index = 0; index < getters.size(); ++index)
        install(getters[index], [this, index](auto&, auto&) { return std::uint64_t{current_credentials()[index]}; });
    install("_getgroups", [](auto& s, auto& m) {
        if (s.x[0] > INT_MAX) throw ServiceError{einval};
        if (!s.x[0]) return std::uint64_t{1};
        validate(m, s.x[1], 4, true); m.write32(s.x[1], 1000); return std::uint64_t{1};
    });
    context.install_export(std::string(system_library), "_getgroups$DARWIN_EXTSN", *context.resolve(system_library, "_getgroups"));
    // getppid, getpgrp, getpgid and getsid: install_process_services (process_model.cpp).
    install("_getlogin", [this](auto&, auto& m) {
        const auto value = std::string("alchemy"); const auto address = context.allocate_guest(value.size() + 1);
        put_text(m, address, value); return address;
    }, 0);
    install("_uname", [](auto& s, auto& m) {
        validate(m, s.x[0], 5 * 256, true); m.fill_bytes(s.x[0], 0, 5 * 256);
        const std::array<std::string_view, 5> fields{"Darwin", "alchemy", darwin_release, "Alchemy compatibility profile", "arm64"};
        for (std::size_t index = 0; index < fields.size(); ++index) put_text(m, s.x[0] + index * 256, fields[index]);
        return std::uint64_t{0};
    });
    const auto pathconf = [](std::uint64_t name) -> std::uint64_t {
        switch (name) { case 1: return 1024; case 2: case 3: return 255; case 4: return 255; case 5: return 1024;
        case 6: return 512; case 7: case 8: return 1; case 9: return 255; default: throw ServiceError{einval}; }
    };
    install("_pathconf", [this, pathconf](auto& s, auto& m) {
        if (is_random_device(path(m, s.x[0]))) return pathconf(s.x[1]);
        std::error_code error; if (!std::filesystem::exists(path(m, s.x[0]), error)) throw ServiceError{error ? host_error(error) : enoent};
        return pathconf(s.x[1]);
    });
    install("_fpathconf", [this, pathconf](auto& s, auto&) { static_cast<void>(descriptor_file(static_cast<int>(s.x[0]))); return pathconf(s.x[1]); });
    // _CS_PATH (1), and <unistd.h>'s _CS_DARWIN_USER_DIR (65536),
    // _CS_DARWIN_USER_TEMP_DIR (65537), _CS_DARWIN_USER_CACHE_DIR (65538).
    // macOS answers per-user directories under /var/folders (0/, T/, C/);
    // here all three are the guest's temporary directory (TMPDIR, the host
    // temporary directory), which exists and is private to the user, with
    // the trailing '/' macOS gives. Chrome's sandbox parameters PCHECK them.
    install("_confstr", [this](auto& s, auto& m) {
        std::string value;
        if (s.x[0] == 1) value = "/usr/bin:/bin";
        else if (s.x[0] >= 65536 && s.x[0] <= 65538) value = temporary_directory();
        else throw ServiceError{einval};
        if (s.x[2]) { const auto count = std::min<std::uint64_t>(value.size(), s.x[2] - 1); validate(m, s.x[1], count + 1, true); put_text(m, s.x[1], value.substr(0, static_cast<std::size_t>(count))); }
        return static_cast<std::uint64_t>(value.size() + 1);
    }, 0);
    install("_getrlimit", [this](auto& s, auto& m) {
        if (s.x[0] > 8) throw ServiceError{einval}; validate(m, s.x[1], 16, true);
        const auto resource = s.x[0]; std::array<std::uint64_t, 2> value{INT64_MAX, INT64_MAX};
        if (resource == 3) { const auto bounds = context.calling_engine().stack_bounds(); value = {bounds.second - bounds.first, bounds.second - bounds.first}; }
        else if (resource == 4) value = {0, 0};
        else if (resource == 7) value = {1, 1};
        else if (resource == 8) value = {4096, 4096};
        { std::lock_guard lock(mutex); const auto found = resource_limits.find(resource); if (found != resource_limits.end()) value = found->second; }
        m.write64(s.x[1], value[0]); m.write64(s.x[1] + 8, value[1]); return std::uint64_t{0};
    });
    install("_setrlimit", [this](auto& s, auto& m) {
        if (s.x[0] > 8) throw ServiceError{einval}; validate(m, s.x[1], 16, false);
        const auto soft = m.read64(s.x[1]), hard = m.read64(s.x[1] + 8);
        if (soft > hard) throw ServiceError{einval};
        // Guest resource enforcement cannot be mapped safely to host-global
        // limits. Accept an unchanged query result, reject actual changes.
        const auto temporary = context.allocate_guest(16);
        const std::array<std::uint64_t, 2> query_args{s.x[0], temporary};
        try { static_cast<void>(EngineAdapter(context.calling_engine()).call(context.resolve(system_library, "_getrlimit")->address, query_args));
            const auto unchanged = m.read64(temporary) == soft && m.read64(temporary + 8) == hard;
            context.release_guest(temporary); if (!unchanged) throw ServiceError{enosys};
        } catch (...) { if (m.query_region(temporary)) context.release_guest(temporary); throw; }
        return std::uint64_t{0};
    });
    // getrusage(RUSAGE_SELF 0 / RUSAGE_CHILDREN -1): ru_utime and ru_stime (struct timeval) from this process's
    // CPU time, or from the children it reaped (their CPU time read when wait* reaped them).
    install("_getrusage", [this](auto& s, auto& m) {
        const auto who = static_cast<std::int32_t>(s.x[0]);
        if (who != 0 && who != -1) throw ServiceError{einval};
        validate(m, s.x[1], 144, true); m.fill_bytes(s.x[1], 0, 144);
        std::uint64_t user{}, system{};
        if (who == 0) {
            const auto usage = host::process::usage();
            if (!usage) throw ServiceError{file_failure(usage.error())};
            user = usage->user_ns; system = usage->system_ns;
        } else { std::lock_guard lock(children_mutex); user = reaped_user_ns; system = reaped_system_ns; }
        const auto write_time = [&](std::uint64_t offset, std::uint64_t nanoseconds) {
            m.write64(s.x[1] + offset, nanoseconds / 1'000'000'000);
            m.write32(s.x[1] + offset + 8, static_cast<std::uint32_t>((nanoseconds % 1'000'000'000) / 1000));
        };
        write_time(0, user); write_time(16, system);
        return std::uint64_t{0};
    });
    // clock_t times(struct tms*): tms_utime, tms_stime, tms_cutime, tms_cstime in CLK_TCK (100) ticks, the
    // children's from the ones this process reaped; the result is ticks of the monotonic clock.
    install("_times", [this](auto& s, auto& m) {
        validate(m, s.x[0], 32, true);
        const auto usage = host::process::usage();
        if (!usage) throw ServiceError{file_failure(usage.error())};
        std::uint64_t children_user{}, children_system{};
        { std::lock_guard lock(children_mutex); children_user = reaped_user_ns; children_system = reaped_system_ns; }
        constexpr std::uint64_t tick = 10'000'000;   // 1/100 s in nanoseconds
        m.write64(s.x[0], usage->user_ns / tick); m.write64(s.x[0] + 8, usage->system_ns / tick);
        m.write64(s.x[0] + 16, children_user / tick); m.write64(s.x[0] + 24, children_system / tick);
        return host::time::monotonic_ns() / tick;
    });
    install("_getloadavg", [](auto&, auto&) -> std::uint64_t { throw ServiceError{enosys}; });
    // getpriority/setpriority(which, who, prio) for this process: PRIO_PROCESS (0) is its nice value, -20..20,
    // as the host priority class nearest to it (only root may lower it); PRIO_DARWIN_THREAD (3, the calling
    // thread) and PRIO_DARWIN_PROCESS (4) put it in or out of the host's background mode with PRIO_DARWIN_BG
    // (0x1000) or 0, and getpriority reports 1 or 0. Another process's priority is ENOTSUP; PRIO_PGRP (1) and
    // PRIO_USER (2) would reach other processes and are ENOTSUP too.
    const auto self = [this](std::uint64_t who) {
        const auto id = static_cast<std::uint32_t>(who);
        if (id != 0 && id != process_id()) throw ServiceError{45};   // ENOTSUP
    };
    const auto current_nice = [this]() {
        const auto host_nice = host::process::priority();
        if (!host_nice) throw ServiceError{file_failure(host_nice.error())};
        std::lock_guard lock(mutex);
        // The value last set, while the host class is still the one it chose.
        if (nice_value) {
            const auto mapped = *nice_value <= -15 ? -15 : *nice_value < 0 ? -5 : *nice_value == 0 ? 0 : *nice_value < 15 ? 5 : 15;
            if (mapped == *host_nice) return *nice_value;
        }
        return *host_nice;
    };
    install("_getpriority", [this, self, current_nice](auto& s, auto&) -> std::uint64_t {
        const auto which = static_cast<std::int32_t>(s.x[0]);
        if (which == 1 || which == 2) throw ServiceError{45};
        if (which == 3) {
            if (static_cast<std::uint32_t>(s.x[1]) != 0) throw ServiceError{einval};
            const auto thread = context.current_thread();
            std::lock_guard lock(mutex);
            return background_threads.contains(thread ? thread->identity : 0) ? 1 : 0;
        }
        if (which == 4) { self(s.x[1]); std::lock_guard lock(mutex); return process_background ? 1 : 0; }
        if (which != 0) throw ServiceError{einval};
        self(s.x[1]);
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(current_nice()));
    });
    const auto set_nice = [this, current_nice](int value) {
        value = std::clamp(value, -20, 20);
        if (value < current_nice() && current_credentials()[1] != 0) throw ServiceError{eacces};
        if (const auto set = host::process::set_priority(value); !set) throw ServiceError{file_failure(set.error())};
        std::lock_guard lock(mutex);
        nice_value = value;
        return value;
    };
    install("_setpriority", [this, self, set_nice](auto& s, auto&) -> std::uint64_t {
        const auto which = static_cast<std::int32_t>(s.x[0]);
        const auto value = static_cast<std::int32_t>(s.x[2]);
        if (which == 1 || which == 2) throw ServiceError{45};
        if (which == 3 || which == 4) {
            if (value != 0 && value != 0x1000) throw ServiceError{einval};
            if (which == 3 && static_cast<std::uint32_t>(s.x[1]) != 0) throw ServiceError{einval};
            if (which == 4) self(s.x[1]);
            if (const auto set = host::process::set_background(which == 4, value != 0); !set) throw ServiceError{file_failure(set.error())};
            const auto thread = context.current_thread();
            std::lock_guard lock(mutex);
            if (which == 4) process_background = value != 0;
            else if (value) background_threads.insert(thread ? thread->identity : 0);
            else background_threads.erase(thread ? thread->identity : 0);
            return 0;
        }
        if (which != 0) throw ServiceError{einval};
        self(s.x[1]);
        static_cast<void>(set_nice(value));
        return 0;
    });
    // int nice(int increment): the new nice value (EPERM, not EACCES, when raising the priority is refused).
    install("_nice", [set_nice, current_nice](auto& s, auto&) -> std::uint64_t {
        try {
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(set_nice(current_nice() + static_cast<std::int32_t>(s.x[0]))));
        } catch (const ServiceError& error) {
            if (error.value == eacces) throw ServiceError{eperm};
            throw;
        }
    });
    install("_umask", [this](auto& s, auto&) {
        // Stored per Alchemy process; Windows ACL permission translation remains
        // explicitly limited to the readonly bit for newly opened files.
        if (overlay) { const auto old = overlay->umask; overlay->umask = s.x[0] & 0777; return old; }
        std::lock_guard lock(mutex); const auto old = creation_mask; creation_mask = s.x[0] & 0777; return old;
    });
    install("_sigaction", [this](auto& s, auto& m) {
        if (s.x[0] == 0 || s.x[0] >= signal_actions.size() || (s.x[1] && (s.x[0] == 9 || s.x[0] == 17))) throw ServiceError{einval};
        SignalAction replacement;
        if (s.x[1]) { validate(m, s.x[1], 16, false); replacement = {m.read64(s.x[1]), m.read32(s.x[1] + 8), m.read32(s.x[1] + 12)}; }
        if (s.x[2]) validate(m, s.x[2], 16, true);
        // A fork child changes its own copy of the dispositions (they reach exec).
        std::lock_guard lock(mutex); auto& value = signal_table()[static_cast<std::size_t>(s.x[0])];
        if (s.x[2]) { m.write64(s.x[2], value.handler); m.write32(s.x[2] + 8, value.mask); m.write32(s.x[2] + 12, value.flags); }
        if (s.x[1]) value = replacement;
        if (s.x[1] && replacement.handler == 1) discard_pending_signal(s.x[0]);   // SIG_IGN discards a pending signal
        return std::uint64_t{0};
    });
    install("_signal", [this](auto& s, auto&) {
        if (s.x[0] == 0 || s.x[0] >= signal_actions.size() || s.x[0] == 9 || s.x[0] == 17) throw ServiceError{einval};
        std::lock_guard lock(mutex); auto& value = signal_table()[static_cast<std::size_t>(s.x[0])]; const auto old = value.handler;
        value = {s.x[1], 0, 2};
        if (s.x[1] == 1) discard_pending_signal(s.x[0]);
        return old;
    });
    install("_sigprocmask", [this](auto& s, auto& m) {
        std::uint32_t replacement = 0;
        if (s.x[1]) { if (s.x[0] < 1 || s.x[0] > 3) throw ServiceError{einval}; validate(m, s.x[1], 4, false); replacement = m.read32(s.x[1]) & ~((1U << 8) | (1U << 16)); }
        if (s.x[2]) validate(m, s.x[2], 4, true);
        const auto identity = context.current_thread()->identity;
        std::uint32_t now;
        {
            std::lock_guard lock(mutex); auto& mask = signal_masks[identity];
            if (s.x[2]) m.write32(s.x[2], mask);
            if (s.x[1]) { if (s.x[0] == 1) mask |= replacement; else if (s.x[0] == 2) mask &= ~replacement; else mask = replacement; }
            now = mask;
        }
        // Pending signals the new mask unblocks are delivered before sigprocmask returns.
        if (s.x[1] && deliver_pending_signals) static_cast<void>(deliver_pending_signals(now));
        return std::uint64_t{0};
    });
    for (const auto name : {"_sigemptyset", "_sigfillset"}) install(name, [full = std::string_view(name) == "_sigfillset"](auto& s, auto& m) {
        validate(m, s.x[0], 4, true); m.write32(s.x[0], full ? UINT32_MAX : 0); return std::uint64_t{0};
    });
    for (const auto operation : {0, 1, 2}) install(operation == 0 ? "_sigaddset" : (operation == 1 ? "_sigdelset" : "_sigismember"), [operation](auto& s, auto& m) {
        if (s.x[1] == 0 || s.x[1] > 31) throw ServiceError{einval}; validate(m, s.x[0], 4, operation != 2);
        const auto value = m.read32(s.x[0]), bit = 1U << (s.x[1] - 1);
        if (operation == 2) return static_cast<std::uint64_t>((value & bit) != 0);
        m.write32(s.x[0], operation == 0 ? value | bit : value & ~bit); return std::uint64_t{0};
    });
    // sigpending: the blocked signals pending for the calling thread or for the process.
    install("_sigpending", [this](auto& s, auto& m) {
        validate(m, s.x[0], 4, true);
        const auto identity = context.current_thread()->identity;
        std::lock_guard lock(mutex);
        m.write32(s.x[0], thread_pending[identity] | process_pending);
        return std::uint64_t{0};
    });
    // kill: install_process_services (process_model.cpp).
    // Real process creation, credential changes, terminal objects, and signal
    // delivery are intentionally unavailable. Their ABI imports resolve, then
    // report an explicit Darwin error instead of invoking Windows with guest
    // pointers or falsely claiming successful effects.
    install("_ioctl", [this](auto& s, auto& m) -> std::uint64_t {
        const auto descriptor = static_cast<int>(s.x[0]);
        constexpr std::uint64_t fioclex = 0x20006601, fionclex = 0x20006602;
        if (s.x[1] == fioclex || s.x[1] == fionclex) {
            // IOC_VOID requests take no argument. Do not read a variadic stack
            // slot or forward a guest value to a Windows control operation.
            set_descriptor_cloexec(descriptor, s.x[1] == fioclex); return 0;
        }
        const auto record = descriptor_file(descriptor);
        if (record && record->socket && (s.x[1] == 0x8004667e || s.x[1] == 0x4004667f)) {
            const auto argument = m.read64(s.sp);
            std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
            if (s.x[1] == 0x8004667e) {
                validate(m, argument, 4, false);
                record->socket->status_flags = 2U | (m.read32(argument) ? 4U : 0U);
            } else {
                validate(m, argument, 4, true); std::lock_guard gate(record->socket->gate);
                const auto count = host::net::readable_bytes(record->socket->handle);
                if (!count) throw ServiceError{network_error(count.error())};
                m.write32(argument, static_cast<std::uint32_t>(*count));
            }
            return 0;
        }
        if (record && s.x[1] == 0x8004667e && (record->pipe || record->random_device || record->null_device)) {
            // FIONBIO on a pipe or device: its O_NONBLOCK, as fcntl(F_SETFL) sets it (libuv uses FIONBIO).
            const auto argument = m.read64(s.sp);
            validate(m, argument, 4, false);
            const auto nonblocking = m.read32(argument) ? 4U : 0U;
            std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
            if (record->pipe) record->pipe_status->store(nonblocking);
            else if (record->random_device) *record->random_flags = nonblocking;
            else record->flags = (record->flags & ~4ULL) | nonblocking;
            return 0;
        }
        if (record && record->pipe && s.x[1] == 0x4004667f) {   // FIONREAD
            const auto argument = m.read64(s.sp);
            validate(m, argument, 4, true);
            m.write32(argument, static_cast<std::uint32_t>(std::min<std::uint64_t>(pipe_available(record), INT32_MAX)));
            return 0;
        }
        if (record && record->socket && record->socket->terminal)
            if (const auto handled = terminal_ioctl(m, record, s.x[1], s.sp)) return *handled;
        throw ServiceError{25}; // Request does not apply to the implemented descriptor kind.
    });
    // Process creation, waiting and signals to processes: process/process_model.cpp
    // (install_process_services) replaces fork, exec*, wait*, pipe, raise,
    // killpg, setpgid, setsid, posix_spawn and system.
    // openpty, tcgetpgrp, tcsetpgrp: fs/terminal.cpp.
    // chflags, lchflags, fchflags: fs/files.cpp.
    for (const auto name : {"_forkpty", "_login_tty", "_mkfifo", "_mknod", "_lchmod", "_sigaltstack"})
        install(name, [](auto&, auto&) -> std::uint64_t { throw ServiceError{enosys}; });
    for (const auto name : {"_chown", "_lchown", "_fchown", "_fchownat", "_chroot"})
        install(name, [](auto&, auto&) -> std::uint64_t { throw ServiceError{eperm}; });
    // Credential changes (setuid(2) rules): only a process that started as
    // superuser (ALCHEMY_SUPERUSER=1) may take root or the virtual user
    // (1000) as its real or effective id; others may only re-set their ids.
    // A fork child changes only its own credentials (set_credential).
    const auto allowed = [this](std::uint32_t id, std::size_t real, std::size_t effective) {
        const auto ids = current_credentials();
        return id == ids[real] || id == ids[effective] || (superuser && (id == 0 || id == 1000));
    };
    for (const auto& [name, real, both] : std::array<std::tuple<const char*, std::size_t, bool>, 4>{{
             {"_setuid", 0, true}, {"_seteuid", 0, false}, {"_setgid", 2, true}, {"_setegid", 2, false}}})
        install(name, [this, allowed, real, both](auto& s, auto&) -> std::uint64_t {
            const auto id = static_cast<std::uint32_t>(s.x[0]);
            if (!allowed(id, real, real + 1)) throw ServiceError{eperm};
            const auto ids = current_credentials();
            // setuid by root sets both ids; seteuid sets only the effective one.
            if (both && ids[real + 1] == 0) set_credential(real, id);
            else if (both && id != ids[real] && id != ids[real + 1]) throw ServiceError{eperm};
            set_credential(real + 1, id);
            return 0;
        });
    for (const auto& [name, real] : std::array<std::pair<const char*, std::size_t>, 2>{{{"_setreuid", 0}, {"_setregid", 2}}})
        install(name, [this, allowed, real](auto& s, auto&) -> std::uint64_t {
            const auto first = static_cast<std::uint32_t>(s.x[0]), second = static_cast<std::uint32_t>(s.x[1]);
            if ((first != UINT32_MAX && !allowed(first, real, real + 1)) || (second != UINT32_MAX && !allowed(second, real, real + 1)))
                throw ServiceError{eperm};
            if (first != UINT32_MAX) set_credential(real, first);
            if (second != UINT32_MAX) set_credential(real + 1, second);
            return 0;
        });
    // Supplementary groups stay {1000}; a root process may set them (no effect).
    for (const auto name : {"_initgroups", "_setgroups"})
        install(name, [this](auto&, auto&) -> std::uint64_t { if (current_credentials()[1] != 0) throw ServiceError{eperm}; return 0; });
    // sigwait: process/process_model.cpp.
    // A guest call to another libSystem function.
    const auto guest = [this](std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        const auto function = context.resolve(system_library, name);
        if (!function) throw std::runtime_error("Alchemy libSystem requires " + std::string(name));
        return EngineAdapter(context.calling_engine()).call(function->address, std::span(arguments.begin(), arguments.size()));
    };
    // getpwent walks the password database (the virtual user, uid 1000); setpwent and endpwent rewind it.
    install("_getpwent", [this, guest](auto&, auto&) -> std::uint64_t {
        { std::lock_guard lock(mutex); if (password_cursor > 0) return 0; password_cursor = 1; }
        return guest("_getpwuid", {1000});
    }, 0);
    for (const auto name : {"_setpwent", "_endpwent"})
        install(name, [this](auto&, auto&) -> std::uint64_t { std::lock_guard lock(mutex); password_cursor = 0; return 0; });
    // char* ctermid_r(char* buffer): the controlling terminal's name, "/dev/tty"; NULL for a NULL buffer.
    install("_ctermid_r", [](auto& s, auto& m) -> std::uint64_t {
        if (!s.x[0]) return 0;
        validate(m, s.x[0], 9, true);
        put_text(m, s.x[0], "/dev/tty");
        return s.x[0];
    }, 0);
    // int getgrouplist(const char* name, int basegid, int* groups, int* ngroups): basegid, then the groups of
    // the group database (mach/machsys.cpp: the user's group, then wheel) that list `name` as a member. -1
    // when they do not fit, with *ngroups the number needed.
    install("_getgrouplist", [this, guest](auto& s, auto& m) -> std::uint64_t {
        const auto name = text(m, s.x[0]);
        validate(m, s.x[3], 4, false);
        const auto capacity = static_cast<std::int32_t>(m.read32(s.x[3]));
        std::vector<std::uint32_t> list{static_cast<std::uint32_t>(s.x[1])};
        for (const auto gid : {current_credentials()[2], 0U}) {
            const auto entry = guest("_getgrgid", {gid});
            if (!entry || std::find(list.begin(), list.end(), gid) != list.end()) continue;
            for (auto members = m.read64(entry + 24); members && m.read64(members); members += 8)
                if (text(m, m.read64(members)) == name) { list.push_back(gid); break; }
        }
        const auto fit = std::min<std::size_t>(list.size(), static_cast<std::size_t>(std::max(capacity, 0)));
        if (fit) validate(m, s.x[2], fit * 4, true);
        for (std::size_t index = 0; index < fit; ++index) m.write32(s.x[2] + index * 4, list[index]);
        m.write32(s.x[3], static_cast<std::uint32_t>(list.size()));
        return list.size() <= fit ? 0 : UINT64_MAX;
    });
    // int lockf(int fd, int command, off_t length): F_ULOCK 0, F_LOCK 1 (waits), F_TLOCK 2, F_TEST 3, from the
    // current offset for `length` bytes (0: to the end of the file, negative: before the offset), as fcntl
    // write locks.
    install("_lockf", [this](auto& s, auto& m) -> std::uint64_t {
        const auto command = static_cast<std::int32_t>(s.x[1]);
        if (command < 0 || command > 3) throw ServiceError{einval};
        const auto lock = context.allocate_guest(24);
        m.fill_bytes(lock, 0, 24);
        m.write64(lock + 8, s.x[2]);
        m.write32(lock + 20, (command == 0 ? 2U : 3U) | (1U << 16));   // l_type F_UNLCK / F_WRLCK, l_whence SEEK_CUR
        const auto fcntl_command = command == 3 ? 7U : command == 1 ? 9U : 8U;   // F_GETLK, F_SETLKW, F_SETLK
        // fcntl's third argument is variadic: on the stack in Apple's arm64 ABI.
        CallRegs registers;
        registers.x[0] = s.x[0]; registers.x[1] = fcntl_command;
        registers.push_variadic(lock);
        const auto result = static_cast<std::uint32_t>(
            EngineAdapter(context.calling_engine()).call_regs(context.resolve(system_library, "_fcntl")->address, registers).x0);
        const auto conflict = command == 3 && result == 0 && (m.read32(lock + 20) & 0xffff) != 2;
        context.release_guest(lock);
        if (result != 0) throw ServiceError{context.calling_engine().read32(context.current_thread()->errno_address)};
        if (conflict) throw ServiceError{35};   // EAGAIN
        return 0;
    });
    install("_sync", [this](auto&, auto&) {
        std::vector<std::shared_ptr<File>> pending; { std::lock_guard lock(mutex); for (const auto& [descriptor, record] : files) { static_cast<void>(descriptor); pending.push_back(record); } }
        for (const auto& record : pending) { std::lock_guard lock(record->mutex); if (!record->closed && record->native) {
            if (const auto flushed = host::files::flush(record->native); !flushed) throw ServiceError{crt_failure(flushed.error())};
        } }
        return std::uint64_t{0};
    });
}

} // namespace alchemy
