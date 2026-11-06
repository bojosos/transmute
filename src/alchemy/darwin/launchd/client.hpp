#pragma once

// The session launchd: Alchemy's analogue of the per-user launchd (and of
// Wine's wineserver). One runs per Windows user and Alchemy root, as
// `alchemy-fx launchd --root <root>`, started on demand by the first client
// that needs it (launchctl, SMJobSubmit and friends) and never otherwise. It
// is not a host service and writes nothing outside the Alchemy root. It ends
// a grace window after the last application process tree that used it
// (ALCHEMY_LAUNCHD_GRACE seconds, default 300), and its jobs end with it.
// README.md in this directory has the details.

#include "alchemy/darwin/launchd/mach_services.hpp"
#include "alchemy/darwin/launchd/plist.hpp"

#include "alchemy/host/host.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::launchd {

struct ListEntry {
    std::int64_t pid{-1};            // -1: not running
    std::int32_t last_status{};      // last exit status (negative: the signal that ended it)
    std::string label;
};

// What a job learns about itself at a legacy launch_msg CheckIn.
struct JobCheckIn {
    std::string label;
    std::vector<std::pair<std::string, std::int32_t>> sockets;   // (Sockets key, descriptor)
    std::vector<std::string> services;                         // its MachServices
};

struct JobInfo {
    Value dictionary;                // the job as SMJobCopyDictionary reports it
    Value state;                     // launchctl print's state fields
};

// The process asking: its pid, and the root of its Alchemy process tree
// (the process the user started), which keeps the session alive.
struct Caller {
    std::uint32_t pid{};
    std::uint32_t tree_root{};
    std::uint32_t tree_version{};
};
// This process's Caller, from the process tree records (process_tree.hpp).
[[nodiscard]] Caller current_caller();

class Session {
public:
    // Connects to the session launchd of this user and the configured root,
    // starting it when none runs and `start` is set. `launch_options` are the
    // Alchemy runtime options the server passes on to its jobs. nullptr when
    // none runs (and !start) or it could not be started; `error` says why.
    [[nodiscard]] static std::unique_ptr<Session> open(bool start, const std::vector<std::string>& launch_options,
                                                       std::string* error = nullptr);
    // Whether a session launchd is running now (it is never started by this).
    [[nodiscard]] static bool running();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Results are 0 or a status (wire.hpp: errno values and launchctl's own);
    // `message` explains a failure.
    std::int32_t load(const Value& job, const std::string& guest_path, std::uint8_t flags, std::string& message);
    std::int32_t remove(const std::string& label, std::string& message);
    std::vector<ListEntry> list();
    std::optional<JobInfo> info(const std::string& label);
    std::int32_t kickstart(const std::string& label, bool kill_first, std::uint32_t& pid, std::string& message);
    std::int32_t signal(const std::string& label, std::uint32_t signal, std::string& message);
    std::int32_t set_enabled(const std::string& label, bool enabled, std::string& message);
    std::vector<std::pair<std::string, bool>> overrides();   // (label, disabled)
    void set_variable(const std::string& name, const std::optional<std::string>& value);
    std::optional<std::string> variable(const std::string& name);
    std::int32_t start(const std::string& label, std::string& message);
    std::int32_t stop(const std::string& label, std::string& message);
    std::optional<ServiceLocation> mach_look_up(const std::string& service);
    std::optional<std::uint32_t> mach_check_in(const std::string& service);
    // launch_activate_socket for this process (a job): the descriptors of the
    // job's Sockets entry `key`; a status (ESRCH: not a job, ENOENT: no such key).
    std::int32_t sockets(const std::string& key, std::vector<std::int32_t>& descriptors);
    std::optional<JobCheckIn> job();

private:
    explicit Session(host::Handle channel) : channel_(channel) {}
    std::vector<std::uint8_t> call(const std::vector<std::uint8_t>& request);
    host::Handle channel_;
};

// MachServices of the session launchd, for the Mach layer (mach_exports.cpp).
// Where a job's service port is; nothing (after one failed pipe open) when no
// session launchd runs.
[[nodiscard]] std::optional<ServiceLocation> look_up_service(const std::string& service);
// This process, a job, checking in a service its job declares: the process
// that offers the receive right. Nothing when no session launchd runs or the
// service is not this job's.
[[nodiscard]] std::optional<std::uint32_t> check_in_service(const std::string& service);

// This process's job (launch_msg CheckIn); nothing when no session launchd
// runs or this process is not one of its jobs.
[[nodiscard]] std::optional<JobCheckIn> check_in_job();

// The session launchd's pipe name for this user and root (host::ipc).
[[nodiscard]] std::string pipe_name();

// `alchemy-fx launchd [--root <root>] [runtime options]`: the server's main.
// `mach` makes the MachServices ports (alchemy-fx: make_mach_services); without
// it, jobs' MachServices are reported but have no ports.
int server_main(const std::vector<std::string>& arguments, MachServicesFactory mach = nullptr);

} // namespace alchemy::launchd
