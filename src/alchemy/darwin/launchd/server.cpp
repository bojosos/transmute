#include "alchemy/darwin/launchd/client.hpp"
#include "alchemy/darwin/launchd/job.hpp"
#include "alchemy/darwin/launchd/protocol.hpp"

#include "alchemy/alchemy_root.hpp"
#include "alchemy/darwin_process.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/ipc.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/path.hpp"
#include "alchemy/process_tree.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <thread>

// The session launchd (client.hpp). One scheduler thread owns the job table's
// timers and the session's lifetime; one thread per connection answers
// requests. Job processes are Alchemy children of this process (they end with
// it); their exits and the end of client process trees arrive as events from
// host watches.
namespace alchemy::launchd {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t guest_uid = 1000;
constexpr std::int32_t spawn_failed_status = 78 << 8;   // EX_CONFIG, as launchd reports a job it could not start

std::chrono::seconds seconds_setting(const char* name, std::int64_t fallback) {
    if (const auto text = host::process::environment(name); text && !text->empty()) {
        char* end = nullptr;
        const auto value = std::strtoll(text->c_str(), &end, 10);
        if (end && *end == 0 && value >= 0) return std::chrono::seconds(value);
    }
    return std::chrono::seconds(fallback);
}

// Local calendar time of `seconds` (false when the C library cannot convert it).
bool local_time(std::time_t seconds, std::tm& out) {
#ifdef _WIN32
    return localtime_s(&out, &seconds) == 0;
#else
    return localtime_r(&seconds, &out) != nullptr;
#endif
}

void log(const std::string& line) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::time_t now = std::time(nullptr);
    char stamp[32] = "";
    std::tm local{};
    if (local_time(now, local)) std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    std::cerr << stamp << " alchemy launchd[" << host::process::current_pid() << "]: " << line << std::endl;
}

// A guest path on the host, as the built-in shell maps it (absolute paths only).
std::filesystem::path host_path(std::string_view guest) {
    const auto text = std::string(guest);
    if (text == "/tmp" || text.starts_with("/tmp/")) {
        auto temporary = std::filesystem::temp_directory_path();
        if (text.size() > 5) temporary /= utf8_path(text.substr(5));
        return temporary.lexically_normal();
    }
    if (text.starts_with("/Volumes/") && text.size() >= 10) {
        std::string native{text[9], ':', '/'};
        if (text.size() > 11) native += text.substr(11);
        return utf8_path(native).lexically_normal();
    }
    const auto normal = utf8_path(text).lexically_normal().generic_u8string();
    const std::string generic(reinterpret_cast<const char*>(normal.data()), normal.size());
    if (auto in_root = guest_root_host_path(generic)) return in_root->lexically_normal();
    return (std::filesystem::current_path().root_path() / utf8_path(generic).relative_path()).lexically_normal();
}

std::string guest_home() {
    for (const auto* name : {"HOME", "USERPROFILE"}) {
        const auto value = host::process::environment(name);
        if (!value || value->empty()) continue;
        if (value->starts_with('/')) return *value;
        return guest_path_string(utf8_path(*value));
    }
    return "/";
}

bool signal_ignored_by_default(std::uint32_t signal) { return signal == 16 || signal == 19 || signal == 20 || signal == 23 || signal == 28 || signal == 29; }
bool signal_stops(std::uint32_t signal) { return signal == 17 || signal == 18 || signal == 21 || signal == 22; }

// launchctl list's status column: the exit status, or minus the signal.
std::int32_t list_status(std::int32_t wait_status) {
    if ((wait_status & 0x7f) == 0) return (wait_status >> 8) & 0xff;
    return -(wait_status & 0x7f);
}

// The next local minute at or after `from` that matches one of the entries.
std::optional<std::chrono::system_clock::time_point> next_calendar(const std::vector<CalendarInterval>& entries,
                                                                   std::chrono::system_clock::time_point from) {
    if (entries.empty()) return std::nullopt;
    auto seconds = std::chrono::system_clock::to_time_t(from);
    seconds += 60 - seconds % 60;   // the next whole minute
    for (int minute = 0; minute < 366 * 24 * 60; ++minute, seconds += 60) {
        std::tm value{};
        if (!local_time(seconds, value)) return std::nullopt;
        const std::tm* local = &value;
        for (const auto& entry : entries) {
            const auto match = [](const std::optional<int>& want, int have) { return !want || *want == have; };
            const auto weekday_match = !entry.weekday || *entry.weekday % 7 == local->tm_wday;
            if (match(entry.minute, local->tm_min) && match(entry.hour, local->tm_hour) && match(entry.day, local->tm_mday) &&
                match(entry.month, local->tm_mon + 1) && weekday_match)
                return std::chrono::system_clock::from_time_t(seconds);
        }
    }
    return std::nullopt;
}

struct Job {
    JobSpec spec;
    std::string path;                         // guest path of its plist ("" when submitted)
    std::uint32_t pid{};                      // 0: not running
    std::optional<std::int32_t> last_status;  // wait status of the last run
    std::uint64_t runs{};
    Clock::time_point last_start{};
    std::optional<Clock::time_point> next_interval, restart_at;
    std::optional<std::chrono::system_clock::time_point> next_calendar_time;
};

struct Process {
    host::Handle handle, watch;
    std::string label;
};

class Server {
public:
    Server(std::string name, std::vector<std::string> launch_options)
        : name_(std::move(name)), launch_options_(std::move(launch_options)) {
        grace_ = seconds_setting("ALCHEMY_LAUNCHD_GRACE", 300);
        linger_ = seconds_setting("ALCHEMY_LAUNCHD_LINGER", 3);
        if (const auto root = configured_alchemy_root())
            overrides_path_ = guest_root_host_path("/private/var/db/com.apple.xpc.launchd/disabled." + std::to_string(guest_uid) + ".plist");
        read_overrides();
    }

    int run(host::Handle first_listener) {
        log("started for " + name_ + " (grace " + std::to_string(grace_.count()) + " s)");
        std::thread([this, first_listener] { accept_loop(first_listener); }).detach();
        if (const auto agents = host::process::environment("ALCHEMY_LAUNCHD_AGENTS"); agents && *agents == "1") load_agents();
        std::unique_lock lock(mutex_);
        // A client that started this server says hello at once.
        linger_until_ = Clock::now() + std::max(linger_, std::chrono::seconds(10));
        for (;;) {
            drain_events(lock);
            const auto now = Clock::now();
            const bool any_running = std::any_of(jobs_.begin(), jobs_.end(), [](const auto& item) { return item.second.pid != 0; });
            if (!sessions_.empty()) {
                grace_until_.reset();
                linger_until_.reset();
            } else if (any_running) {
                if (!grace_until_ && !ending_) {
                    grace_until_ = now + grace_;
                    log("no application uses the session: jobs end in " + std::to_string(grace_.count()) + " s");
                }
                linger_until_.reset();
            } else if (!linger_until_) {
                linger_until_ = now + linger_;
            }
            if (grace_until_ && now >= *grace_until_ && !ending_) {
                ending_ = true;
                log("grace window over: ending the jobs");
                for (auto& [label, job] : jobs_) if (job.pid) terminate(job, 15);
            }
            if (linger_until_ && now >= *linger_until_ && sessions_.empty() && !any_running) break;
            if (active()) run_timers(now);
            cv_.wait_until(lock, next_deadline(now));
        }
        log("session ended: exiting");
        stopping_ = true;
        lock.unlock();
        host::ipc::cancel(listener_);
        std::cerr.flush();
        return 0;
    }

private:
    // ---- connections -------------------------------------------------------------------------
    void accept_loop(host::Handle listener) {
        for (;;) {
            { std::lock_guard lock(mutex_); if (stopping_) return; listener_ = listener; }
            if (!host::ipc::accept(listener)) { host::close(listener); return; }
            std::thread([this, listener] { serve(listener); }).detach();
            auto next = host::ipc::listen(name_, false);
            if (!next) { log("cannot listen again (" + std::string(host::name(next.error())) + ")"); return; }
            listener = *next;
        }
    }

    void serve(host::Handle channel) {
        Caller caller;
        while (auto body = wire::receive(channel)) {
            wire::Writer answer;
            try {
                wire::Reader in(std::move(*body));
                const auto op = static_cast<wire::Op>(in.u32());
                if (op == wire::Op::hello) {
                    caller.pid = in.u32(); caller.tree_root = in.u32(); caller.tree_version = in.u32();
                    hello(caller);
                } else {
                    handle(op, in, answer);
                }
            } catch (const std::exception& error) {
                log(std::string("bad request: ") + error.what());
                break;
            }
            if (!wire::send(channel, answer.bytes)) break;
        }
        host::close(channel);
    }

    // A client's process tree keeps the session alive until it ends, unless
    // it is one of the session's own jobs.
    void hello(const Caller& caller) {
        {
            std::lock_guard lock(mutex_);
            if (!caller.tree_root || sessions_.contains(caller.tree_root) || job_pids_.contains(caller.tree_root)) return;
            sessions_.insert(caller.tree_root);
            log("application tree " + std::to_string(caller.tree_root) + " joined (pid " + std::to_string(caller.pid) + ")");
        }
        const auto ended = [this](std::uint32_t pid) {
            { std::lock_guard events(events_mutex_); ended_trees_.push_back(pid); }
            wake();
        };
        if (caller.tree_version) {
            static_cast<void>(process_tree::watch_exit(caller.tree_root, caller.tree_version, ended));
            return;
        }
        // Not a published Alchemy process: watch the host process itself.
        auto process = host::process::open(caller.tree_root);
        if (!process) { ended(caller.tree_root); return; }
        auto* context = new std::pair<Server*, std::pair<std::uint32_t, host::Handle>>{this, {caller.tree_root, *process}};
        const auto watched = host::sync::watch_once(*process, [](void* raw) {
            auto* item = static_cast<std::pair<Server*, std::pair<std::uint32_t, host::Handle>>*>(raw);
            { std::lock_guard events(item->first->events_mutex_); item->first->ended_trees_.push_back(item->second.first); }
            item->first->wake();
            host::close(item->second.second);
            delete item;
        }, context);
        if (!watched) { host::close(*process); delete context; ended(caller.tree_root); }
    }

    void wake() {
        { std::lock_guard lock(mutex_); }
        cv_.notify_all();
    }

    void handle(wire::Op op, wire::Reader& in, wire::Writer& out) {
        std::unique_lock lock(mutex_);
        switch (op) {
        case wire::Op::load: {
            const auto xml = in.text();
            const auto path = in.text();
            const auto flags = in.u8();
            std::string message;
            const auto status = load(std::span(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()), path, flags, message);
            out.i32(status).text(message);
            break;
        }
        case wire::Op::remove: {
            std::string message;
            const auto status = remove(lock, in.text(), message);
            out.i32(status).text(message);
            break;
        }
        case wire::Op::list:
            out.u32(static_cast<std::uint32_t>(jobs_.size()));
            for (const auto& [label, job] : jobs_)
                out.i64(job.pid ? static_cast<std::int64_t>(job.pid) : -1).i32(job.last_status ? list_status(*job.last_status) : 0).text(label);
            break;
        case wire::Op::info: {
            const auto found = jobs_.find(in.text());
            if (found == jobs_.end()) { out.i32(wire::status_no_such_service); break; }
            out.i32(0).text(to_xml(dictionary_of(found->second))).text(to_xml(state_of(found->second)));
            break;
        }
        case wire::Op::kickstart: {
            const auto label = in.text();
            const bool kill_first = in.u8() != 0;
            std::string message;
            std::uint32_t pid = 0;
            const auto status = kickstart(lock, label, kill_first, pid, message);
            out.i32(status).text(message).u32(pid);
            break;
        }
        case wire::Op::signal: {
            const auto label = in.text();
            const auto signal = in.u32();
            std::string message;
            const auto status = send_signal(label, signal, message);
            out.i32(status).text(message);
            break;
        }
        case wire::Op::enable: {
            const auto label = in.text();
            const bool enabled = in.u8() != 0;
            overrides_[label] = !enabled;
            write_overrides();
            out.i32(0).text("");
            break;
        }
        case wire::Op::overrides:
            out.u32(static_cast<std::uint32_t>(overrides_.size()));
            for (const auto& [label, disabled] : overrides_) out.text(label).u8(disabled ? 1 : 0);
            break;
        case wire::Op::setenv: {
            const auto name = in.text();
            environment_[name] = in.text();
            break;
        }
        case wire::Op::unsetenv:
            environment_.erase(in.text());
            break;
        case wire::Op::getenv: {
            const auto found = environment_.find(in.text());
            if (found == environment_.end()) out.i32(wire::status_no_such_service);
            else out.i32(0).text(found->second);
            break;
        }
        case wire::Op::start: {
            std::string message;
            std::uint32_t pid = 0;
            const auto status = kickstart(lock, in.text(), false, pid, message);
            out.i32(status).text(message);
            break;
        }
        case wire::Op::stop: {
            const auto found = jobs_.find(in.text());
            if (found == jobs_.end()) { out.i32(wire::status_no_such_service).text("Could not find specified service"); break; }
            if (found->second.pid) terminate(found->second, 15);
            out.i32(0).text("");
            break;
        }
        default:
            throw wire::WireError("unknown request " + std::to_string(static_cast<std::uint32_t>(op)));
        }
    }

    // ---- the job table -----------------------------------------------------------------------
    bool active() const { return !sessions_.empty() && !ending_; }

    bool disabled(const JobSpec& spec) const {
        const auto found = overrides_.find(spec.label);
        return found != overrides_.end() ? found->second : spec.disabled;
    }

    std::int32_t load(std::span<const std::uint8_t> bytes, const std::string& path, std::uint8_t flags, std::string& message) {
        JobSpec spec;
        try {
            const auto value = parse_plist(bytes);
            spec = parse_job(value);
            if (!loads_in_session(value)) {
                message = spec.label + ": the job is limited to a session type other than Aqua";
                return wire::status_io;
            }
        } catch (const std::exception& error) {
            message = std::string("Invalid property list: ") + error.what();
            return wire::status_invalid_plist;
        }
        if (jobs_.contains(spec.label)) {
            message = "service already loaded";
            return wire::status_already_loaded;
        }
        if (flags & wire::load_write) {
            overrides_[spec.label] = false;
            write_overrides();
        }
        if (!(flags & wire::load_force) && disabled(spec)) {
            message = "Service is disabled";
            return wire::status_service_disabled;
        }
        if (ending_) {
            message = "the session is ending";
            return wire::status_io;
        }
        const auto label = spec.label;
        auto& job = jobs_[label];
        job.spec = std::move(spec);
        job.path = path;
        const auto now = Clock::now();
        if (job.spec.start_interval) job.next_interval = now + std::chrono::seconds(*job.spec.start_interval);
        job.next_calendar_time = next_calendar(job.spec.calendar, std::chrono::system_clock::now());
        log("loaded " + label + (path.empty() ? std::string(" (submitted)") : " from " + path) +
            (job.spec.ignored_keys.empty() ? std::string() : " (keys without effect here: " + join(job.spec.ignored_keys) + ")"));
        // RunAtLoad, and KeepAlive conditions that hold now (SuccessfulExit implies a first run).
        const auto& keep = job.spec.keep_alive;
        if (job.spec.run_at_load || keep.always || keep.successful_exit || keep_alive_holds(job, std::nullopt)) {
            if (active()) static_cast<void>(start(job, message));
            else job.restart_at = now;   // when an application joins
        }
        message.clear();
        return 0;
    }

    std::int32_t remove(std::unique_lock<std::mutex>& lock, const std::string& label, std::string& message) {
        const auto found = jobs_.find(label);
        if (found == jobs_.end()) { message = "Could not find specified service"; return wire::status_no_such_service; }
        const auto pid = found->second.pid;
        if (pid) terminate(found->second, 15);
        jobs_.erase(found);
        log("removed " + label);
        // bootout returns once the job's process has ended.
        if (pid && !wait_exit(lock, pid)) log("removed " + label + ": its process " + std::to_string(pid) + " has not ended yet");
        return 0;
    }

    // Waits until a terminated job process has ended (the host may take a
    // while to tear down a large process).
    bool wait_exit(std::unique_lock<std::mutex>& lock, std::uint32_t pid) {
        return cv_.wait_for(lock, std::chrono::seconds(60), [&] { return !processes_.contains(pid); });
    }

    std::int32_t kickstart(std::unique_lock<std::mutex>& lock, const std::string& label, bool kill_first, std::uint32_t& pid, std::string& message) {
        auto found = jobs_.find(label);
        if (found == jobs_.end()) { message = "Could not find specified service"; return wire::status_no_such_service; }
        if (found->second.pid) {
            if (!kill_first) { pid = found->second.pid; return 0; }
            const auto old = found->second.pid;
            terminate(found->second, 9);
            if (!wait_exit(lock, old)) {
                message = "the job's process " + std::to_string(old) + " has not ended yet";
                return 16;   // EBUSY
            }
            found = jobs_.find(label);
            if (found == jobs_.end()) { message = "Could not find specified service"; return wire::status_no_such_service; }
            found->second.restart_at.reset();
        }
        if (ending_) { message = "the session is ending"; return wire::status_io; }
        const auto status = start(found->second, message);
        pid = found->second.pid;
        return status;
    }

    std::int32_t send_signal(const std::string& label, std::uint32_t signal, std::string& message) {
        const auto found = jobs_.find(label);
        if (found == jobs_.end()) { message = "Could not find specified service"; return wire::status_no_such_service; }
        if (signal > 31) { message = "bad signal"; return 22; }
        if (!found->second.pid) { message = "No process"; return 3; }
        if (!signal || signal_ignored_by_default(signal)) return 0;
        if (signal_stops(signal)) { message = "stopping a process is not supported under Alchemy"; return wire::status_unsupported; }
        // A process of another tree cannot run a handler for a signal sent from
        // outside (as kill(2) between Alchemy processes): the default action ends it.
        terminate(found->second, signal);
        return 0;
    }

    void terminate(Job& job, std::uint32_t signal) {
        const auto process = processes_.find(job.pid);
        if (process == processes_.end()) return;
        static_cast<void>(host::process::terminate(process->second.handle, child_signal_exit_marker | signal));
    }

    std::vector<std::string> environment_of(const Job& job) const {
        std::map<std::string, std::string> values{
            {"PATH", std::string(default_guest_path)}, {"HOME", guest_home()}, {"USER", "alchemy"}, {"LOGNAME", "alchemy"},
            {"SHELL", "/bin/zsh"}, {"TMPDIR", guest_path_string(std::filesystem::temp_directory_path()) + "/"},
            {"XPC_SERVICE_NAME", job.spec.label}, {"XPC_FLAGS", "0x0"}, {"__CF_USER_TEXT_ENCODING", "0x3E8:0x0:0x0"}};
        if (values["TMPDIR"].ends_with("//")) values["TMPDIR"].pop_back();
        for (const auto& [name, value] : environment_) values[name] = value;
        for (const auto& [name, value] : job.spec.environment) values[name] = value;
        std::vector<std::string> out;
        for (const auto& [name, value] : values) out.push_back(name + "=" + value);
        return out;
    }

    // Opens a StandardIn/Out/ErrorPath for the job (/dev/null when unset).
    static ChildFd descriptor(int fd, const std::optional<std::string>& path, std::vector<host::OwnedHandle>& keep) {
        ChildFd entry;
        entry.fd = fd;
        entry.kind = ChildFdKind::handle;
        const bool input = fd == 0;
        entry.flags = input ? 0 : (1 | 8);   // O_RDONLY / O_WRONLY | O_APPEND
        host::Result<host::Handle> opened = host::Error::not_found;
        if (path && *path != "/dev/null") {
            const auto native = path_utf8(host_path(*path));
            if (!input) {
                std::error_code ignored;
                std::filesystem::create_directories(utf8_path(native).parent_path(), ignored);
            }
            opened = input ? host::files::open(native, host::files::Access::read, host::files::Create::open_existing)
                           : host::files::open(native, host::files::Access::write, host::files::Create::open_or_create, host::files::open_append);
            if (opened) entry.path = native;
            else log("cannot open " + *path + " for descriptor " + std::to_string(fd) + " (" + std::string(host::name(opened.error())) + "): /dev/null instead");
        }
        if (!opened) opened = host::files::open_null(input ? host::files::Access::read : host::files::Access::write);
        if (!opened) return entry;
        keep.emplace_back(*opened);
        entry.handle = static_cast<std::uint64_t>(opened->value);
        return entry;
    }

    std::int32_t start(Job& job, std::string& message) {
        job.restart_at.reset();
        if (job.pid) return 0;
        job.last_start = Clock::now();
        ++job.runs;
        try {
            ExecEnvironment environment;
            environment.map_path = [](std::string_view value) { return host_path(value); };
            environment.host_environment = host::process::environment_block();
            environment.user_id = guest_uid;
            SpawnRequest request;
            request.envp = environment_of(job);
            const auto program = job.spec.program.starts_with('/') ? job.spec.program : search_path(job.spec.program, request.envp, environment);
            request.target = resolve_exec(program, job.spec.arguments, request.envp, environment);
            if (job.spec.working_directory) request.cwd = host_path(*job.spec.working_directory);
            else if (const auto root = configured_alchemy_root()) request.cwd = *root;
            std::vector<host::OwnedHandle> keep;
            request.fds.push_back(descriptor(0, job.spec.standard_in, keep));
            request.fds.push_back(descriptor(1, job.spec.standard_out, keep));
            request.fds.push_back(descriptor(2, job.spec.standard_error ? job.spec.standard_error : job.spec.standard_out, keep));
            request.ppid = 1;   // launchd
            request.new_session = true;
            request.launch_options = launch_options_;
            if (const auto root = configured_alchemy_root()) {
                request.launch_options.push_back("--root");
                request.launch_options.push_back(path_utf8(*root));
            }
            const auto spawned = spawn_child(request);
            const host::Handle handle{reinterpret_cast<std::uintptr_t>(spawned.process)};
            auto* context = new std::pair<Server*, std::uint32_t>{this, spawned.pid};
            const auto watch = host::sync::watch_once(handle, [](void* raw) {
                auto* item = static_cast<std::pair<Server*, std::uint32_t>*>(raw);
                { std::lock_guard events(item->first->events_mutex_); item->first->exited_.push_back(item->second); }
                item->first->wake();
                delete item;
            }, context);
            if (!watch) {
                delete context;
                static_cast<void>(host::process::terminate(handle, 1));
                host::close(handle);
                throw ProcessError(5, "cannot watch the job's process");
            }
            processes_[spawned.pid] = Process{handle, *watch, job.spec.label};
            job_pids_.insert(spawned.pid);
            job.pid = spawned.pid;
            log("started " + job.spec.label + " as pid " + std::to_string(spawned.pid) + ": " + job.spec.program);
            return 0;
        } catch (const ProcessError& error) {
            job.last_status = spawn_failed_status;
            message = error.what();
            log("cannot start " + job.spec.label + ": " + message);
            return static_cast<std::int32_t>(error.error());
        } catch (const std::exception& error) {
            job.last_status = spawn_failed_status;
            message = error.what();
            log("cannot start " + job.spec.label + ": " + message);
            return wire::status_io;
        }
    }

    bool keep_alive_holds(const Job& job, std::optional<std::int32_t> status) const {
        const auto& keep = job.spec.keep_alive;
        if (keep.always) return true;
        if (status) {
            const bool signaled = (*status & 0x7f) != 0;
            const bool success = !signaled && ((*status >> 8) & 0xff) == 0;
            if (keep.successful_exit && !signaled && success == *keep.successful_exit) return true;
            if (keep.crashed && signaled == *keep.crashed) return true;
        }
        for (const auto& [path, wanted] : keep.path_state) {
            std::error_code ignored;
            if (std::filesystem::exists(host_path(path), ignored) == wanted) return true;
        }
        for (const auto& [label, wanted] : keep.other_job_enabled)
            if (jobs_.contains(label) == wanted) return true;
        return false;
    }

    void drain_events(std::unique_lock<std::mutex>&) {
        std::vector<std::uint32_t> exited, ended;
        {
            std::lock_guard events(events_mutex_);
            exited.swap(exited_);
            ended.swap(ended_trees_);
        }
        for (const auto pid : ended) {
            if (sessions_.erase(pid)) log("application tree " + std::to_string(pid) + " ended");
        }
        for (const auto pid : exited) {
            const auto process = processes_.find(pid);
            if (process == processes_.end()) continue;
            const auto code = host::process::exit_code(process->second.handle);
            const auto status = static_cast<std::int32_t>(darwin_wait_status(code ? *code : 1));
            host::sync::unwatch(process->second.watch, false);
            host::close(process->second.handle);
            const auto label = process->second.label;
            processes_.erase(process);
            job_pids_.erase(pid);
            const auto found = jobs_.find(label);
            if (found == jobs_.end() || found->second.pid != pid) continue;
            auto& job = found->second;
            job.pid = 0;
            job.last_status = status;
            log(label + " (pid " + std::to_string(pid) + ") ended with " +
                ((status & 0x7f) ? "signal " + std::to_string(status & 0x7f) : "status " + std::to_string((status >> 8) & 0xff)));
            if (!ending_ && keep_alive_holds(job, status)) {
                const auto earliest = job.last_start + std::chrono::seconds(job.spec.throttle_interval);
                job.restart_at = std::max(Clock::now(), earliest);
            }
        }
        cv_.notify_all();
    }

    void run_timers(Clock::time_point now) {
        const auto wall = std::chrono::system_clock::now();
        std::string ignored;
        for (auto& [label, job] : jobs_) {
            bool due = job.restart_at && now >= *job.restart_at;
            if (job.next_interval && now >= *job.next_interval) {
                job.next_interval = now + std::chrono::seconds(*job.spec.start_interval);
                due = true;
            }
            if (job.next_calendar_time && wall >= *job.next_calendar_time) {
                job.next_calendar_time = next_calendar(job.spec.calendar, wall);
                due = true;
            }
            // PathState / OtherJobEnabled conditions are polled.
            if (!job.pid && !job.restart_at && (!job.spec.keep_alive.path_state.empty() || !job.spec.keep_alive.other_job_enabled.empty()) &&
                keep_alive_holds(job, std::nullopt) && now >= job.last_start + std::chrono::seconds(job.spec.throttle_interval))
                due = true;
            if (due && !job.pid) static_cast<void>(start(job, ignored));
            else if (due) job.restart_at.reset();
        }
    }

    Clock::time_point next_deadline(Clock::time_point now) const {
        auto next = now + std::chrono::seconds(60);
        const auto earlier = [&](std::optional<Clock::time_point> at) { if (at && *at < next) next = *at; };
        earlier(grace_until_);
        earlier(linger_until_);
        if (!active()) return std::max(next, now + std::chrono::milliseconds(10));
        const auto wall = std::chrono::system_clock::now();
        for (const auto& [label, job] : jobs_) {
            earlier(job.restart_at);
            earlier(job.next_interval);
            if (job.next_calendar_time) earlier(now + std::chrono::duration_cast<Clock::duration>(*job.next_calendar_time - wall));
            if (!job.spec.keep_alive.path_state.empty() || !job.spec.keep_alive.other_job_enabled.empty()) earlier(now + std::chrono::seconds(5));
        }
        return std::max(next, now + std::chrono::milliseconds(10));
    }

    // ---- reports -----------------------------------------------------------------------------
    static std::string join(const std::vector<std::string>& items) {
        std::string out;
        for (const auto& item : items) out += (out.empty() ? "" : ", ") + item;
        return out;
    }

    // As SMJobCopyDictionary and `launchctl list <label>` report a job.
    Value dictionary_of(const Job& job) const {
        auto value = Value::of_dictionary();
        value.set("Label", Value::of_string(job.spec.label));
        value.set("LimitLoadToSessionType", Value::of_string("Aqua"));
        value.set("OnDemand", Value::of_boolean(!job.spec.keep_alive.always));
        value.set("LastExitStatus", Value::of_integer(job.last_status ? *job.last_status : 0));
        if (job.pid) value.set("PID", Value::of_integer(job.pid));
        value.set("TimeOut", Value::of_integer(30));
        value.set("Program", Value::of_string(job.spec.program));
        std::vector<Value> arguments;
        for (const auto& argument : job.spec.arguments) arguments.push_back(Value::of_string(argument));
        value.set("ProgramArguments", Value::of_array(std::move(arguments)));
        if (!job.spec.mach_services.empty()) {
            auto services = Value::of_dictionary();
            for (const auto& service : job.spec.mach_services) services.set(service.name, Value::of_integer(0));
            value.set("MachServices", std::move(services));
        }
        if (job.spec.standard_out) value.set("StandardOutPath", Value::of_string(*job.spec.standard_out));
        if (job.spec.standard_error) value.set("StandardErrorPath", Value::of_string(*job.spec.standard_error));
        return value;
    }

    Value state_of(const Job& job) const {
        auto value = Value::of_dictionary();
        value.set("path", Value::of_string(job.path.empty() ? "(submitted by a process)" : job.path));
        value.set("type", Value::of_string(job.path.empty() ? "Submitted" : "LaunchAgent"));
        value.set("state", Value::of_string(job.pid ? "running" : job.restart_at ? "waiting" : "not running"));
        if (job.pid) value.set("pid", Value::of_integer(job.pid));
        value.set("runs", Value::of_integer(static_cast<std::int64_t>(job.runs)));
        if (job.last_status) {
            const auto status = *job.last_status;
            value.set("last exit code", Value::of_string((status & 0x7f) ? "(signal " + std::to_string(status & 0x7f) + ")" : std::to_string((status >> 8) & 0xff)));
        } else value.set("last exit code", Value::of_string("(never exited)"));
        value.set("program", Value::of_string(job.spec.program));
        std::vector<Value> arguments;
        for (const auto& argument : job.spec.arguments) arguments.push_back(Value::of_string(argument));
        value.set("arguments", Value::of_array(std::move(arguments)));
        if (job.spec.working_directory) value.set("working directory", Value::of_string(*job.spec.working_directory));
        if (job.spec.standard_in) value.set("stdin path", Value::of_string(*job.spec.standard_in));
        if (job.spec.standard_out) value.set("stdout path", Value::of_string(*job.spec.standard_out));
        if (job.spec.standard_error) value.set("stderr path", Value::of_string(*job.spec.standard_error));
        auto environment = Value::of_dictionary();
        for (const auto& [name, text] : job.spec.environment) environment.set(name, Value::of_string(text));
        value.set("environment", std::move(environment));
        if (job.spec.start_interval) value.set("run interval", Value::of_integer(*job.spec.start_interval));
        if (!job.spec.calendar.empty()) value.set("calendar intervals", Value::of_integer(static_cast<std::int64_t>(job.spec.calendar.size())));
        value.set("keepalive", Value::of_boolean(job.spec.keep_alive.any()));
        value.set("run at load", Value::of_boolean(job.spec.run_at_load));
        value.set("disabled", Value::of_boolean(disabled(job.spec)));
        std::vector<Value> ignored;
        for (const auto& key : job.spec.ignored_keys) ignored.push_back(Value::of_string(key));
        value.set("keys without effect under Alchemy", Value::of_array(std::move(ignored)));
        return value;
    }

    // ---- overrides (launchctl enable / disable, load -w) ---------------------------------------
    void read_overrides() {
        if (!overrides_path_) return;
        std::error_code ignored;
        if (!std::filesystem::exists(*overrides_path_, ignored)) return;
        try {
            const auto value = read_plist_file(path_utf8(*overrides_path_));
            for (const auto& [label, member] : value.dictionary)
                if (member.is(Value::Kind::boolean)) overrides_[label] = member.boolean;
        } catch (const std::exception& error) {
            log(std::string("ignoring the overrides file: ") + error.what());
        }
    }

    void write_overrides() {
        if (!overrides_path_) return;
        auto value = Value::of_dictionary();
        for (const auto& [label, disabled] : overrides_) value.set(label, Value::of_boolean(disabled));
        std::error_code ignored;
        std::filesystem::create_directories(overrides_path_->parent_path(), ignored);
        std::ofstream file(*overrides_path_, std::ios::binary | std::ios::trunc);
        file << to_xml(value);
        if (!file) log("cannot write " + path_utf8(*overrides_path_));
    }

    // ---- ALCHEMY_LAUNCHD_AGENTS=1: the on-disk agents, as at a macOS login -----------------------
    void load_agents() {
        for (const auto& folder : {guest_home() + "/Library/LaunchAgents", std::string("/Library/LaunchAgents")}) {
            const auto host = host_path(folder);
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(host, error)) {
                if (entry.path().extension() != ".plist") continue;
                const auto guest = folder + "/" + path_utf8(entry.path().filename());
                try {
                    const auto value = read_plist_file(path_utf8(entry.path()));
                    std::string message;
                    std::lock_guard lock(mutex_);
                    const auto xml = to_xml(value);
                    const auto status = load(std::span(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()), guest, 0, message);
                    if (status && status != wire::status_service_disabled) log("agent " + guest + " not loaded: " + message);
                } catch (const std::exception& failure) {
                    log("agent " + guest + " not loaded: " + failure.what());
                }
            }
        }
    }

    std::string name_;
    std::vector<std::string> launch_options_;
    std::chrono::seconds grace_{}, linger_{};
    std::optional<std::filesystem::path> overrides_path_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Job> jobs_;
    std::map<std::uint32_t, Process> processes_;
    std::set<std::uint32_t> job_pids_;
    std::set<std::uint32_t> sessions_;
    std::map<std::string, bool> overrides_;     // label -> disabled
    std::map<std::string, std::string> environment_;
    std::optional<Clock::time_point> grace_until_, linger_until_;
    bool ending_{}, stopping_{};
    host::Handle listener_{};

    std::mutex events_mutex_;
    std::vector<std::uint32_t> exited_, ended_trees_;
};

} // namespace

int server_main(const std::vector<std::string>& arguments) {
    std::vector<std::string> launch_options;
    std::optional<std::filesystem::path> root;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "--root" && index + 1 < arguments.size()) root = utf8_path(arguments[++index]);
        else launch_options.push_back(arguments[index]);
    }
    if (root) {
        configure_alchemy_root(*root);
        static_cast<void>(host::process::set_environment("ALCHEMY_ROOT", path_utf8(*root)));
    }
    // Each job is the root of its own process tree (its own Mach broker).
    static_cast<void>(host::process::set_environment("ALCHEMY_MACH_BROKER", std::nullopt));
    const auto name = pipe_name();
    auto listener = host::ipc::listen(name, true);
    if (!listener) {
        if (listener.error() == host::Error::exists) return 0;   // another server for this user and root runs
        std::cerr << "alchemy launchd: cannot listen on " << name << " (" << host::name(listener.error()) << ")" << std::endl;
        return 1;
    }
    Server server(name, std::move(launch_options));
    const auto status = server.run(*listener);
    std::cerr.flush();
    host::process::terminate_self(static_cast<std::uint32_t>(status));
}

} // namespace alchemy::launchd
