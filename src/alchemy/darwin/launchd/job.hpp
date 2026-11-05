#pragma once

// A launchd job as the session launchd keeps it: the launchd.plist keys
// Alchemy acts on, read from the job's dictionary. Keys are from the public
// launchd.plist(5) documentation. Keys that are accepted but have no effect
// under Alchemy are listed in `ignored_keys` (launchctl print shows them).

#include "alchemy/darwin/launchd/plist.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace alchemy::launchd {

// A job dictionary launchd refuses (launchctl: "Invalid property list").
struct JobError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct KeepAlive {
    bool always{};                                // KeepAlive = true
    std::optional<bool> successful_exit;          // restart when the exit was (true) / was not (false) status 0
    std::optional<bool> crashed;                  // restart when it ended by a signal (true) / did not (false)
    std::map<std::string, bool> path_state;       // host-independent guest paths: restart while (not) present
    std::map<std::string, bool> other_job_enabled;   // restart while another label is (not) loaded
    [[nodiscard]] bool any() const noexcept {
        return always || successful_exit || crashed || !path_state.empty() || !other_job_enabled.empty();
    }
};

// StartCalendarInterval entry: unset fields are wildcards (cron style).
struct CalendarInterval {
    std::optional<int> minute, hour, day, weekday, month;
};

struct MachService {
    std::string name;
    bool reset_at_close{};
    bool hide_until_check_in{};
};

// One socket of a Sockets entry (launch_activate_socket returns them by key).
struct SocketSpec {
    std::string key;                  // the Sockets dictionary key
    bool stream{true};                // SockType stream (default) / dgram
    bool passive{true};               // SockPassive: listen (default) / connect
    std::string node;                 // SockNodeName ("" = any)
    std::string service;              // SockServiceName: a port number or service name
    std::string family;               // SockFamily: "IPv4", "IPv6", "Unix" or "" (both)
    std::string path;                 // SockPathName (Unix domain)
};

struct JobSpec {
    std::string label;
    std::string program;                          // Program, or ProgramArguments[0]
    std::vector<std::string> arguments;           // argv (ProgramArguments, or {Program})
    bool run_at_load{};
    KeepAlive keep_alive;
    std::optional<std::uint32_t> start_interval;  // seconds
    std::vector<CalendarInterval> calendar;
    std::vector<std::pair<std::string, std::string>> environment;
    std::optional<std::string> working_directory, standard_in, standard_out, standard_error;
    std::uint32_t throttle_interval{10};          // seconds between (re)starts
    std::uint32_t exit_timeout{20};               // seconds from SIGTERM to SIGKILL
    bool abandon_process_group{};
    bool disabled{};                              // the plist's own Disabled key
    std::vector<MachService> mach_services;
    std::vector<SocketSpec> sockets;
    std::vector<std::string> ignored_keys;        // accepted, no effect here
    Value source;                                 // the dictionary as submitted
};

// Reads a job dictionary; throws JobError for what launchd would refuse.
[[nodiscard]] JobSpec parse_job(const Value& dictionary);

// Session types a gui/user-domain job may be limited to (LimitLoadToSessionType):
// true when `dictionary` loads in the Aqua session Alchemy runs.
[[nodiscard]] bool loads_in_session(const Value& dictionary);

} // namespace alchemy::launchd
