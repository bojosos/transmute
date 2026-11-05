#include "alchemy/darwin/launchd/job.hpp"

#include <algorithm>
#include <array>

namespace alchemy::launchd {
namespace {

[[noreturn]] void refuse(const std::string& label, const std::string& why) {
    throw JobError(label.empty() ? why : label + ": " + why);
}

const std::string& string_of(const std::string& label, std::string_view key, const Value& value) {
    if (!value.is(Value::Kind::string)) refuse(label, std::string(key) + " is not a string");
    return value.text;
}

bool bool_of(const std::string& label, std::string_view key, const Value& value) {
    if (!value.is(Value::Kind::boolean)) refuse(label, std::string(key) + " is not a boolean");
    return value.boolean;
}

std::uint32_t seconds_of(const std::string& label, std::string_view key, const Value& value) {
    if (!value.is(Value::Kind::integer) || value.integer < 0 || value.integer > 0x7fffffff)
        refuse(label, std::string(key) + " is not a non-negative integer");
    return static_cast<std::uint32_t>(value.integer);
}

CalendarInterval calendar_of(const std::string& label, const Value& value) {
    if (!value.is(Value::Kind::dictionary)) refuse(label, "StartCalendarInterval entry is not a dictionary");
    CalendarInterval entry;
    const std::array<std::pair<std::string_view, std::optional<int> CalendarInterval::*>, 5> fields{{
        {"Minute", &CalendarInterval::minute}, {"Hour", &CalendarInterval::hour}, {"Day", &CalendarInterval::day},
        {"Weekday", &CalendarInterval::weekday}, {"Month", &CalendarInterval::month}}};
    for (const auto& [key, member] : value.dictionary) {
        const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.first == key; });
        if (found == fields.end()) refuse(label, "StartCalendarInterval has an unknown key " + key);
        if (!member.is(Value::Kind::integer)) refuse(label, "StartCalendarInterval " + key + " is not an integer");
        entry.*(found->second) = static_cast<int>(member.integer);
    }
    return entry;
}

std::map<std::string, bool> bool_map(const std::string& label, std::string_view key, const Value& value) {
    if (!value.is(Value::Kind::dictionary)) refuse(label, std::string(key) + " is not a dictionary");
    std::map<std::string, bool> out;
    for (const auto& [name, member] : value.dictionary) out[name] = bool_of(label, key, member);
    return out;
}

void read_socket(const std::string& label, const std::string& key, const Value& value, std::vector<SocketSpec>& out) {
    if (value.is(Value::Kind::array)) {
        for (const auto& item : value.array) read_socket(label, key, item, out);
        return;
    }
    if (!value.is(Value::Kind::dictionary)) refuse(label, "Sockets " + key + " is not a dictionary");
    SocketSpec socket;
    socket.key = key;
    for (const auto& [name, member] : value.dictionary) {
        if (name == "SockType") {
            const auto& type = string_of(label, name, member);
            if (type == "stream") socket.stream = true;
            else if (type == "dgram") socket.stream = false;
            else refuse(label, "Sockets " + key + ": SockType " + type + " is not supported (stream or dgram)");
        } else if (name == "SockPassive") socket.passive = bool_of(label, name, member);
        else if (name == "SockNodeName") socket.node = string_of(label, name, member);
        else if (name == "SockServiceName") {
            socket.service = member.is(Value::Kind::integer) ? std::to_string(member.integer) : string_of(label, name, member);
        } else if (name == "SockFamily") socket.family = string_of(label, name, member);
        else if (name == "SockPathName") { socket.path = string_of(label, name, member); socket.family = "Unix"; }
        else if (name == "SockPathMode" || name == "SockPathOwner" || name == "SockPathGroup" || name == "Bonjour" ||
                 name == "MulticastGroup" || name == "SockProtocol" || name == "SecureSocketWithKey") continue;
        else refuse(label, "Sockets " + key + " has an unknown key " + name);
    }
    if (socket.path.empty() && socket.service.empty()) refuse(label, "Sockets " + key + " names no port and no path");
    out.push_back(std::move(socket));
}

// Keys with no effect under Alchemy: resource and scheduling hints, identity
// (one user), and macOS integration launchd itself provides.
constexpr std::array<std::string_view, 29> ignored{
    "ProcessType", "Nice", "LowPriorityIO", "LowPriorityBackgroundIO", "EnableTransactions", "EnablePressuredExit",
    "POSIXSpawnType", "LegacyTimers", "AssociatedBundleIdentifiers", "SoftResourceLimits", "HardResourceLimits",
    "Umask", "UserName", "GroupName", "InitGroups", "RootDirectory", "inetdCompatibility", "Debug", "WaitForDebugger",
    "MaterializeDatalessFiles", "SessionCreate", "EnableGlobbing", "Timeout", "BundleProgram", "LaunchOnlyOnce",
    "LimitLoadFromHosts", "LimitLoadToHosts", "LimitLoadToHardware", "LimitLoadFromHardware"};

} // namespace

bool loads_in_session(const Value& dictionary) {
    const auto* limit = dictionary.find("LimitLoadToSessionType");
    if (!limit) return true;
    const auto allowed = [](const std::string& type) { return type == "Aqua" || type == "Background" || type == "StandardIO"; };
    if (limit->is(Value::Kind::string)) return allowed(limit->text);
    if (limit->is(Value::Kind::array))
        return std::any_of(limit->array.begin(), limit->array.end(), [&](const Value& item) { return item.is(Value::Kind::string) && allowed(item.text); });
    return false;
}

JobSpec parse_job(const Value& dictionary) {
    if (!dictionary.is(Value::Kind::dictionary)) throw JobError("the job is not a dictionary");
    JobSpec job;
    job.source = dictionary;
    const auto* label = dictionary.find("Label");
    if (!label || !label->is(Value::Kind::string) || label->text.empty()) throw JobError("the job has no Label");
    job.label = label->text;
    const auto& name = job.label;
    for (const auto& [key, value] : dictionary.dictionary) {
        if (key == "Label") continue;
        if (key == "Program") job.program = string_of(name, key, value);
        else if (key == "ProgramArguments") {
            if (!value.is(Value::Kind::array)) refuse(name, "ProgramArguments is not an array");
            for (const auto& item : value.array) job.arguments.push_back(string_of(name, key, item));
        } else if (key == "RunAtLoad") job.run_at_load = bool_of(name, key, value);
        else if (key == "KeepAlive") {
            if (value.is(Value::Kind::boolean)) job.keep_alive.always = value.boolean;
            else if (value.is(Value::Kind::dictionary)) {
                for (const auto& [condition, member] : value.dictionary) {
                    if (condition == "SuccessfulExit") job.keep_alive.successful_exit = bool_of(name, condition, member);
                    else if (condition == "Crashed") job.keep_alive.crashed = bool_of(name, condition, member);
                    else if (condition == "PathState") job.keep_alive.path_state = bool_map(name, condition, member);
                    else if (condition == "OtherJobEnabled") job.keep_alive.other_job_enabled = bool_map(name, condition, member);
                    else if (condition == "NetworkState" || condition == "AfterInitialDemand" || condition == "OtherJobActive")
                        job.ignored_keys.push_back("KeepAlive." + condition);
                    else refuse(name, "KeepAlive has an unknown key " + condition);
                }
            } else refuse(name, "KeepAlive is neither a boolean nor a dictionary");
        } else if (key == "OnDemand") {
            // The pre-10.5 spelling: OnDemand false is KeepAlive true.
            if (!dictionary.find("KeepAlive")) job.keep_alive.always = !bool_of(name, key, value);
        } else if (key == "StartInterval") {
            const auto seconds = seconds_of(name, key, value);
            if (seconds) job.start_interval = seconds;
        } else if (key == "StartCalendarInterval") {
            if (value.is(Value::Kind::array)) for (const auto& item : value.array) job.calendar.push_back(calendar_of(name, item));
            else job.calendar.push_back(calendar_of(name, value));
        } else if (key == "EnvironmentVariables") {
            if (!value.is(Value::Kind::dictionary)) refuse(name, "EnvironmentVariables is not a dictionary");
            for (const auto& [variable, member] : value.dictionary) job.environment.emplace_back(variable, string_of(name, key, member));
        } else if (key == "WorkingDirectory") job.working_directory = string_of(name, key, value);
        else if (key == "StandardInPath") job.standard_in = string_of(name, key, value);
        else if (key == "StandardOutPath") job.standard_out = string_of(name, key, value);
        else if (key == "StandardErrorPath") job.standard_error = string_of(name, key, value);
        else if (key == "ThrottleInterval") job.throttle_interval = seconds_of(name, key, value);
        else if (key == "ExitTimeOut") job.exit_timeout = seconds_of(name, key, value);
        else if (key == "AbandonProcessGroup") job.abandon_process_group = bool_of(name, key, value);
        else if (key == "Disabled") job.disabled = bool_of(name, key, value);
        else if (key == "LimitLoadToSessionType") {
            if (!value.is(Value::Kind::string) && !value.is(Value::Kind::array)) refuse(name, "LimitLoadToSessionType is not a string or array");
        } else if (key == "MachServices") {
            if (!value.is(Value::Kind::dictionary)) refuse(name, "MachServices is not a dictionary");
            for (const auto& [service, member] : value.dictionary) {
                MachService entry;
                entry.name = service;
                if (member.is(Value::Kind::dictionary)) {
                    for (const auto& [option, flag] : member.dictionary) {
                        if (option == "ResetAtClose") entry.reset_at_close = bool_of(name, option, flag);
                        else if (option == "HideUntilCheckIn") entry.hide_until_check_in = bool_of(name, option, flag);
                    }
                } else if (!member.is(Value::Kind::boolean)) refuse(name, "MachServices " + service + " is not a boolean or dictionary");
                else if (!member.boolean) continue;
                job.mach_services.push_back(std::move(entry));
            }
        } else if (key == "Sockets") {
            if (!value.is(Value::Kind::dictionary)) refuse(name, "Sockets is not a dictionary");
            for (const auto& [socket_key, member] : value.dictionary) read_socket(name, socket_key, member, job.sockets);
        } else if (key == "QueueDirectories" || key == "WatchPaths" || key == "LaunchEvents" || key == "StartOnMount" ||
                   std::find(ignored.begin(), ignored.end(), key) != ignored.end()) {
            job.ignored_keys.push_back(key);
        } else {
            // launchd tolerates unknown keys too; launchctl print lists them.
            job.ignored_keys.push_back(key);
        }
    }
    if (job.program.empty()) {
        if (job.arguments.empty() || job.arguments.front().empty()) refuse(name, "neither Program nor ProgramArguments is set");
        job.program = job.arguments.front();
    }
    if (job.arguments.empty()) job.arguments.push_back(job.program);
    // Program must be absolute; ProgramArguments[0] alone is searched in PATH as execvp does.
    if (dictionary.find("Program") && !job.program.starts_with('/')) refuse(name, "the Program path " + job.program + " is not absolute");
    return job;
}

} // namespace alchemy::launchd
