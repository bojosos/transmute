#include "alchemy/darwin/launchd/client.hpp"
#include "alchemy/darwin/launchd/job.hpp"
#include "alchemy/darwin/launchd/mach_services.hpp"
#include "alchemy/darwin/launchd/protocol.hpp"

#include "alchemy/alchemy_root.hpp"
#include "alchemy/darwin_process.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/ipc.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/network_policy.hpp"
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

// A socket of a job's Sockets entry: made at load, kept here, handed to every
// run of the job at descriptor `fd`.
struct JobSocket {
    std::string key;
    host::Handle handle;
    int fd{};
    std::int32_t domain{}, type{};   // Darwin AF_* and SOCK_* values
};

struct Job {
    JobSpec spec;
    std::vector<JobSocket> sockets;
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
    Server(std::string name, std::vector<std::string> launch_options, std::unique_ptr<MachServices> mach)
        : name_(std::move(name)), launch_options_(std::move(launch_options)), mach_(std::move(mach)) {
        grace_ = seconds_setting("ALCHEMY_LAUNCHD_GRACE", 300);
        linger_ = seconds_setting("ALCHEMY_LAUNCHD_LINGER", 3);
        if (const auto root = configured_alchemy_root())
            overrides_path_ = guest_root_host_path("/private/var/db/com.apple.xpc.launchd/disabled." + std::to_string(guest_uid) + ".plist");
        read_overrides();
    }

    int run(host::Handle first_listener) {
        log("started for " + name_ + " (grace " + std::to_string(grace_.count()) + " s)");
        std::thread([this, first_listener] { accept_loop(first_listener); }).detach();
        if (mach_) std::thread([this] { demand_loop(); }).detach();
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
                    handle(op, in, answer, caller);
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

    // A message queued on a service port of a job that does not run (on demand launch).
    void demand_loop() {
        for (;;) {
            { std::lock_guard lock(mutex_); if (stopping_) return; }
            const auto seen = mach_->epoch();
            auto names = mach_->pending();
            const bool any = !names.empty();
            if (any) {
                { std::lock_guard events(events_mutex_); demanded_ = std::move(names); }
                wake();
            }
            mach_->wait(seen, std::chrono::milliseconds(any ? 500 : 2000));
        }
    }

    void handle(wire::Op op, wire::Reader& in, wire::Writer& out, const Caller& caller) {
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
        case wire::Op::mach_look_up: {
            const auto service = in.text();
            const auto owner = service_owner_.find(service);
            const auto job = owner == service_owner_.end() ? jobs_.end() : jobs_.find(owner->second);
            if (!mach_ || job == jobs_.end()) { out.i32(wire::status_no_such_service); break; }
            const auto& spec = job->second.spec.mach_services;
            const auto entry = std::find_if(spec.begin(), spec.end(), [&](const MachService& item) { return item.name == service; });
            if (entry != spec.end() && entry->hide_until_check_in && !checked_in_.contains(service)) { out.i32(wire::status_no_such_service); break; }
            const auto where = mach_->locate(service);
            if (!where) { out.i32(wire::status_no_such_service); break; }
            out.i32(0).u32(where->host).u32(where->key_pid).u64(where->key_id);
            break;
        }
        case wire::Op::mach_check_in: {
            const auto service = in.text();
            const auto owner = service_owner_.find(service);
            const auto job = owner == service_owner_.end() ? jobs_.end() : jobs_.find(owner->second);
            // Only the job that declares the service checks it in.
            if (!mach_ || job == jobs_.end() || !caller.pid || job->second.pid != caller.pid) { out.i32(wire::status_no_such_service); break; }
            if (!mach_->offer(service, caller.pid)) { out.i32(1103); break; }   // BOOTSTRAP_SERVICE_ACTIVE
            checked_in_.insert(service);
            log(job->first + " (pid " + std::to_string(caller.pid) + ") checked in " + service);
            out.i32(0).u32(mach_->self());
            break;
        }
        case wire::Op::sockets: {
            const auto key = in.text();
            const auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto& item) { return caller.pid && item.second.pid == caller.pid; });
            if (job == jobs_.end()) { out.i32(3); break; }   // ESRCH: not a job
            std::vector<std::int32_t> descriptors;
            for (const auto& socket : job->second.sockets) if (socket.key == key) descriptors.push_back(socket.fd);
            if (descriptors.empty()) { out.i32(2); break; }   // ENOENT
            out.i32(0).u32(static_cast<std::uint32_t>(descriptors.size()));
            for (const auto descriptor : descriptors) out.i32(descriptor);
            break;
        }
        case wire::Op::job: {
            const auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto& item) { return caller.pid && item.second.pid == caller.pid; });
            if (job == jobs_.end()) { out.i32(3); break; }   // ESRCH: not a job
            out.i32(0).text(job->first);
            out.u32(static_cast<std::uint32_t>(job->second.sockets.size()));
            for (const auto& socket : job->second.sockets) out.text(socket.key).i32(socket.fd);
            out.u32(static_cast<std::uint32_t>(job->second.spec.mach_services.size()));
            for (const auto& service : job->second.spec.mach_services) out.text(service.name);
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
        for (const auto& service : spec.mach_services) {
            if (const auto owner = service_owner_.find(service.name); owner != service_owner_.end()) {
                message = "the Mach service " + service.name + " belongs to " + owner->second;
                return 17;   // EEXIST
            }
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
        for (const auto& service : job.spec.mach_services) {
            service_owner_[service.name] = label;
            if (!mach_) log(label + ": the Mach service " + service.name + " has no port (the session launchd runs without Mach IPC)");
            else if (!mach_->create(service.name)) log(label + ": cannot make a port for the Mach service " + service.name);
        }
        int next_fd = 3;
        for (const auto& socket : job.spec.sockets) open_sockets(label, socket, next_fd, job.sockets);
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
        for (const auto& socket : found->second.sockets) host::net::close_socket(socket.handle);
        for (const auto& service : found->second.spec.mach_services) {
            service_owner_.erase(service.name);
            checked_in_.erase(service.name);
            if (mach_) mach_->destroy(service.name);
        }
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
            for (const auto& socket : job.sockets) {
                ChildFd entry;
                entry.fd = socket.fd;
                entry.kind = ChildFdKind::socket;
                entry.handle = static_cast<std::uint64_t>(socket.handle.value);
                entry.socket_domain = socket.domain;
                entry.socket_type = socket.type;
                entry.flags = 2;   // O_RDWR
                request.fds.push_back(std::move(entry));
            }
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

    // The listening (passive) sockets of a Sockets entry: a Unix-domain path,
    // or a TCP/UDP port on SockNodeName (default: every address, or only the
    // loopback addresses under ALCHEMY_NETWORK=loopback), IPv4 and IPv6 unless
    // SockFamily picks one.
    void open_sockets(const std::string& label, const SocketSpec& spec, int& next_fd, std::vector<JobSocket>& out) {
        const auto refuse = [&](const std::string& why) { log(label + ": Sockets " + spec.key + ": " + why); };
        if (!spec.passive) { refuse("connecting sockets (SockPassive false) are not supported; none made"); return; }
        if (!host::net::initialize()) { refuse("no host socket stack"); return; }
        const auto make = [&](host::net::Family family, const host::net::Address& address, std::int32_t domain) {
            const auto type = spec.stream || family == host::net::Family::local ? host::net::SocketType::stream : host::net::SocketType::datagram;
            auto socket = host::net::open_socket(family, type);
            if (!socket) { refuse("cannot make a socket (" + std::string(host::name(socket.error())) + ")"); return; }
            if (const auto bound = host::net::bind(*socket, address); !bound) {
                refuse("cannot bind " + host::net::to_string(address) + " (" + std::string(host::name(bound.error())) + ")");
                host::net::close_socket(*socket);
                return;
            }
            if (type == host::net::SocketType::stream) {
                if (const auto listening = host::net::listen(*socket, 128); !listening) {
                    refuse("cannot listen (" + std::string(host::name(listening.error())) + ")");
                    host::net::close_socket(*socket);
                    return;
                }
            }
            out.push_back(JobSocket{spec.key, *socket, next_fd++, domain, type == host::net::SocketType::stream ? 1 : 2});
            log(label + ": Sockets " + spec.key + " listens on " + host::net::to_string(address) + " (descriptor " + std::to_string(out.back().fd) + ")");
        };
        if (spec.family == "Unix" || !spec.path.empty()) {
            const auto native = path_utf8(host_path(spec.path));
            std::error_code ignored;
            std::filesystem::remove(utf8_path(native), ignored);
            make(host::net::Family::local, host::net::Address::local(native), 1);
            return;
        }
        const auto digits = !spec.service.empty() && std::all_of(spec.service.begin(), spec.service.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!digits || spec.service.size() > 5 || std::stoul(spec.service) > 65535) {
            refuse("the service " + spec.service + " is not a port number (named services are not resolved)");
            return;
        }
        const auto port = static_cast<std::uint16_t>(std::stoul(spec.service));
        const bool v4 = spec.family.empty() || spec.family == "IPv4" || spec.family == "IPv4v6";
        const bool v6 = spec.family.empty() || spec.family == "IPv6" || spec.family == "IPv4v6";
        if (!spec.node.empty()) {
            auto address = host::net::parse_numeric(spec.node, port);
            if (!address && host::net::loopback_host(spec.node)) address = host::net::Address::ipv4({127, 0, 0, 1}, port);
            if (!address) { refuse("the node " + spec.node + " is not a numeric address"); return; }
            make(address->family, *address, address->family == host::net::Family::ipv6 ? 30 : 2);
            return;
        }
        const bool loopback = network_policy::loopback_only();
        if (v4) make(host::net::Family::ipv4, host::net::Address::ipv4(loopback ? std::array<std::uint8_t, 4>{127, 0, 0, 1} : std::array<std::uint8_t, 4>{}, port), 2);
        if (v6) {
            std::array<std::uint8_t, 16> any{};
            if (loopback) any[15] = 1;
            make(host::net::Family::ipv6, host::net::Address::ipv6(any, port), 30);
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
        std::vector<std::string> demanded;
        {
            std::lock_guard events(events_mutex_);
            exited.swap(exited_);
            ended.swap(ended_trees_);
            demanded.swap(demanded_);
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
            // A port the job had checked in died with it: a new one, for the next run.
            for (const auto& service : job.spec.mach_services) {
                if (!checked_in_.erase(service.name) || !mach_) continue;
                mach_->destroy(service.name);
                if (!mach_->create(service.name)) log(label + ": cannot make a new port for the Mach service " + service.name);
            }
            log(label + " (pid " + std::to_string(pid) + ") ended with " +
                ((status & 0x7f) ? "signal " + std::to_string(status & 0x7f) : "status " + std::to_string((status >> 8) & 0xff)));
            if (!ending_ && keep_alive_holds(job, status)) {
                const auto earliest = job.last_start + std::chrono::seconds(job.spec.throttle_interval);
                job.restart_at = std::max(Clock::now(), earliest);
            }
        }
        // Messages waiting for jobs that do not run: launch them (throttled).
        if (active()) {
            std::string ignored;
            for (const auto& service : demanded) {
                const auto owner = service_owner_.find(service);
                const auto found = owner == service_owner_.end() ? jobs_.end() : jobs_.find(owner->second);
                if (found == jobs_.end() || found->second.pid) continue;
                auto& job = found->second;
                if (job.runs && Clock::now() < job.last_start + std::chrono::seconds(job.spec.throttle_interval)) {
                    job.restart_at = job.last_start + std::chrono::seconds(job.spec.throttle_interval);
                    continue;
                }
                log("a message for " + service + " starts " + found->first);
                static_cast<void>(start(job, ignored));
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
        if (!job.sockets.empty()) {
            auto sockets = Value::of_dictionary();
            for (const auto& socket : job.sockets) sockets.set(socket.key + " (descriptor " + std::to_string(socket.fd) + ")", Value::of_string("listening"));
            value.set("sockets", std::move(sockets));
        }
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
        if (!job.spec.mach_services.empty()) {
            auto services = Value::of_dictionary();
            for (const auto& service : job.spec.mach_services)
                services.set(service.name, Value::of_string(!mach_ ? "no port (no Mach IPC)" : checked_in_.contains(service.name) ? "checked in" : "held by launchd"));
            value.set("endpoints", std::move(services));
        }
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
    std::unique_ptr<MachServices> mach_;
    std::map<std::string, std::string> service_owner_;   // Mach service -> job label
    std::set<std::string> checked_in_;                   // services whose receive right went to the job
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
    std::vector<std::string> demanded_;
};

} // namespace

int server_main(const std::vector<std::string>& arguments, MachServicesFactory mach) {
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
    Server server(name, std::move(launch_options), mach ? mach() : nullptr);
    const auto status = server.run(*listener);
    std::cerr.flush();
    host::process::terminate_self(static_cast<std::uint32_t>(status));
}

} // namespace alchemy::launchd
